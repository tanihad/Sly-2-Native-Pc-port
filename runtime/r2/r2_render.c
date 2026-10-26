/* The modern renderer on the render thread (notes/modern-renderer.md 2.5): a GPU_FOP_R2 segment's draws on the exact
 * path's own colour and depth targets, through r2gpu (Direct3D 11 backend on the shared device), in the game's order.
 *
 * Meshes are resident on the GPU per level generation: one vertex and one index arena per generation slot (4, as the
 * mesh store), appended to when a mesh is first drawn (its CPU data is the decoded mesh, r2_mesh.c) and grown by
 * copying; a generation's slot is reused when the store reuses it. Per draw only the draw record (in the frame's data
 * arena: frame block, entries with their world x clip matrix, draws with their material) is read by the shaders, by
 * the draw's index through the exact path's draw-id buffer (StartInstanceLocation). */
#include "r2.h"
#include "r2gpu.h"
#include "r2_render.h"
#include "gpu_shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 4
typedef struct {
    uint32_t gen;
    r2g_buf *vb, *ib;
    uint32_t vused, iused;               /* elements */
} arena;

static struct {
    int init, failed;
    r2g_dev* d;
    r2g_pipe* pipe[2];                   /* plain, dual-source */
    r2g_buf* cb;
    arena A[SLOTS];
    uint32_t epoch;                      /* bumped when GPU resources are recreated: meshes upload again */
    uint64_t uploads, upload_bytes, draws, segments, missing;
} R;

static void release_all(void) {
    for (int i = 0; i < SLOTS; i++) { r2g_buf_free(R.A[i].vb); r2g_buf_free(R.A[i].ib); memset(&R.A[i], 0, sizeof R.A[i]); }
    for (int i = 0; i < 2; i++) { r2g_pipe_free(R.pipe[i]); R.pipe[i] = NULL; }
    r2g_buf_free(R.cb);
    R.cb = NULL;
    r2g_destroy(R.d);
    R.d = NULL;
    R.init = 0;
    R.epoch++;
}

void r2_render_release(void) { release_all(); }

static int init(const r2_exec_in* in) {
    if (R.init) return 1;
    if (R.failed) return 0;
    R.d = r2g_d3d11_create(in->dev, in->ctx);
    static const r2g_attr attrs[] = {
        { "POSITION", R2G_F32x3, 0, 0 }, { "WBITS", R2G_U32, 12, 0 }, { "UV", R2G_S16x2, 16, 0 },
        { "COLA", R2G_U32, 20, 0 }, { "COLB", R2G_U32, 24, 0 }, { "DRAWID", R2G_U32, 0, 1 },
    };
    r2g_pipe_desc pd = { gpu_sh_vs_r2, sizeof gpu_sh_vs_r2, gpu_sh_ps_r2, sizeof gpu_sh_ps_r2, attrs, 6,
                         gpu_sh_gs_r2, sizeof gpu_sh_gs_r2 };
    R.pipe[0] = R.d ? r2g_pipe_create(R.d, &pd) : NULL;
    pd.ps = gpu_sh_ps_r2_dual;
    pd.ps_size = sizeof gpu_sh_ps_r2_dual;
    R.pipe[1] = R.d ? r2g_pipe_create(R.d, &pd) : NULL;
    R.cb = R.d ? r2g_buf_create(R.d, R2G_BUF_CONSTANT, R2G_USAGE_DYNAMIC, 48, NULL) : NULL;
    if (!R.d || !R.pipe[0] || !R.pipe[1] || !R.cb) {
        fprintf(stderr, "[r2] render: cannot create the pipelines: the modern renderer draws nothing\n");
        release_all();
        R.failed = 1;
        return 0;
    }
    R.init = 1;
    R.epoch++;
    return 1;
}

/* grow an arena buffer to hold `need` elements of size el (copying what it holds) */
static int arena_grow(r2g_buf** b, int kind, uint32_t used, uint32_t need, uint32_t el) {
    uint32_t have = *b ? r2g_buf_size(*b) / el : 0;
    if (need <= have) return 1;
    uint32_t cap = have ? have : (kind == R2G_BUF_VERTEX ? 65536u : 262144u);
    while (cap < need) cap *= 2;
    r2g_buf* n = r2g_buf_create(R.d, kind, R2G_USAGE_DEFAULT, cap * el, NULL);
    if (!n) return 0;
    if (*b && used) r2g_buf_copy(R.d, n, 0, *b, 0, used * el);
    r2g_buf_free(*b);
    *b = n;
    return 1;
}

static arena* arena_for(uint32_t gen) {
    arena* a = &R.A[gen % SLOTS];
    if (a->gen != gen) { a->gen = gen; a->vused = a->iused = 0; }   /* (the store reused the slot: a new level) */
    return a;
}

static int resident(r2_mesh* m) {
    if (m->gpu_state == (int)R.epoch) return 1;
    if (m->gpu_state == -(int)R.epoch) return 0;
    arena* a = arena_for(m->handle >> 20);
    if (!arena_grow(&a->vb, R2G_BUF_VERTEX, a->vused, a->vused + m->nvtx, sizeof(r2_vtx)) ||
        !arena_grow(&a->ib, R2G_BUF_INDEX, a->iused, a->iused + m->nidx, 4)) {
        m->gpu_state = -(int)R.epoch;
        return 0;
    }
    r2g_buf_write(R.d, a->vb, a->vused * (uint32_t)sizeof(r2_vtx), m->vtx, m->nvtx * (uint32_t)sizeof(r2_vtx));
    if (m->nidx) r2g_buf_write(R.d, a->ib, a->iused * 4u, m->idx, m->nidx * 4u);
    m->gpu_vbase = a->vused;
    m->gpu_ibase = a->iused;
    a->vused += m->nvtx;
    a->iused += m->nidx;
    m->gpu_state = (int)R.epoch;
    R.uploads++;
    R.upload_bytes += m->nvtx * sizeof(r2_vtx) + m->nidx * 4u;
    return 1;
}

int r2_render_segment(const gpu_fop_r2* o, const uint8_t* fdata, size_t fdata_bytes, const r2_exec_in* in) {
    if (!o->ndraw || !init(in)) return 0;
    if ((size_t)o->draw_off + (size_t)o->ndraw * sizeof(gpu_r2_draw) > fdata_bytes) return 0;
    const gpu_r2_draw* dr = (const gpu_r2_draw*)(fdata + o->draw_off);
    /* residency first (uploads), then the draws */
    arena* a = arena_for(o->gen);
    for (uint32_t i = 0; i < o->ndraw; i++) {
        r2_mesh* m = (r2_mesh*)r2_mesh_get(dr[i].mesh);
        if (m) resident(m);
    }
    if (!a->vb || !a->ib) return 0;
    r2g_invalidate(R.d);
    float* cb = r2g_buf_map(R.d, R.cb, 1);
    if (!cb) return 0;
    uint32_t* cu = (uint32_t*)cb;
    cb[0] = (float)in->kx; cb[1] = (float)in->ky;
    cb[2] = (float)(o->xyoffset & 0xFFFF); cb[3] = (float)(o->xyoffset >> 16);
    cu[4] = o->frame_off; cu[5] = o->ent_off; cu[6] = o->draw_off; cu[7] = (o->flags & GPU_R2F_FBA) ? 1u : 0u;
    static float zbias = -1e9f;
    if (zbias == -1e9f) { const char* e = getenv("PS2_R2_ZBIAS"); zbias = e ? (float)atof(e) : 0.0f; }
    cb[8] = in->dual_bias; cb[9] = zbias; cu[10] = (uint32_t)o->sc[1]; cu[11] = 0;   /* (scissor y0: gs_tri_setup's by0) */
    r2g_buf_unmap(R.d, R.cb);
    r2g_bind_targets_native(R.d, in->rtv, in->dsv);
    r2g_viewport(R.d, -8192.0f, -8192.0f, 16384.0f, 16384.0f);   /* (the REF viewport of mixed frames: d3d pixels = NDC * 8192) */
    r2g_scissor(R.d, o->sc[0] * in->kx, o->sc[1] * in->ky, (o->sc[2] + 1) * in->kx, (o->sc[3] + 1) * in->ky);
    r2g_bind_srv_native(R.d, 0, in->vram_srv);
    r2g_bind_srv_native(R.d, 6, in->swz_srv);
    r2g_bind_srv_native(R.d, 7, in->fdata_srv);
    r2g_bind_constants(R.d, 1, R.cb);
    r2g_bind_vertex(R.d, 0, a->vb, sizeof(r2_vtx));
    r2g_bind_vertex_native(R.d, 1, in->iota_vb, 4);
    r2g_bind_index(R.d, a->ib);
    int n = 0;
    for (uint32_t i = 0; i < o->ndraw; i++) {
        const gpu_r2_draw* d = &dr[i];
        const r2_mesh* m = r2_mesh_get(d->mesh);
        if (!m || m->gpu_state != (int)R.epoch || (m->handle >> 20) != o->gen) { R.missing++; continue; }
        uint32_t st = d->state;
        int bl = (int)(st & 15), depth = (int)((st >> 4) & 15);
        r2g_state s = { (uint8_t)bl, (uint8_t)((st >> 8) & 15), (uint8_t)(depth >> 1), (uint8_t)(depth & 1),
                        (uint8_t)(((st >> 12) & 1) ? R2G_CULL_FRONT : R2G_CULL_NONE) };
        r2g_bind_pipe(R.d, R.pipe[bl == R2G_BLEND_DUAL]);
        r2g_bind_state(R.d, &s);
        r2g_draw_indexed(R.d, d->count, m->gpu_ibase + d->first, (int32_t)m->gpu_vbase, i);
        n++;
    }
    r2g_end(R.d);
    R.draws += (uint64_t)n;
    R.segments++;
    return n;
}

void r2_render_stats(uint64_t* draws, uint64_t* segments, uint64_t* uploads, uint64_t* upload_bytes, uint64_t* missing) {
    *draws = R.draws; *segments = R.segments; *uploads = R.uploads; *upload_bytes = R.upload_bytes; *missing = R.missing;
}
