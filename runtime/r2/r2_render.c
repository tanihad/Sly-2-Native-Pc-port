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
#define MESH_IDX 16384u                  /* (r2_mesh.c MESH_MAX: mesh indices per generation) */
#define MAT_BYTES 64u                    /* a material record (shaders/r2.hlsl), 3 per sub-batch (passes 0..2) */
typedef struct {
    uint32_t gen;
    r2g_buf *vb, *ib, *mb, *mt;          /* vertices, indices, (r2-m2) material records, mesh table (material base) */
    uint32_t vused, iused, mused;        /* elements */
} arena;

static struct {
    int init, failed;
    r2g_dev* d;
    r2g_pipe* pipe;                      /* (r2-m2) one pipeline: the pixel shader always writes the dual-source factor */
    r2g_pipe* pipe_clip;                 /* (r2-m2) entries crossing the guard band (state bit 13): r2_gs_clip */
    r2g_pipe* pipe_nd;                   /* (r2-m2) draws that cannot discard (state bit 14 clear): early depth stays on */
    r2g_buf* cb;
    int cb_set;                          /* (r2-m2) cb holds the device's constants (r2.hlsl r2_static), written once */
    float cb_bias;
    arena A[SLOTS];
    uint32_t epoch;                      /* bumped when GPU resources are recreated: meshes upload again */
    uint64_t uploads, upload_bytes, draws, segments, missing;
} R;

static void release_all(void) {
    for (int i = 0; i < SLOTS; i++) {
        r2g_buf_free(R.A[i].vb); r2g_buf_free(R.A[i].ib); r2g_buf_free(R.A[i].mb); r2g_buf_free(R.A[i].mt);
        memset(&R.A[i], 0, sizeof R.A[i]);
    }
    r2g_pipe_free(R.pipe);
    r2g_pipe_free(R.pipe_clip);
    if (R.pipe_nd != R.pipe) r2g_pipe_free(R.pipe_nd);
    R.pipe = R.pipe_clip = R.pipe_nd = NULL;
    r2g_buf_free(R.cb);
    R.cb = NULL;
    R.cb_set = 0;
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
        { "COLA", R2G_U32, 20, 0 }, { "COLB", R2G_U32, 24, 0 }, { "BATCH", R2G_U32, 28, 0 }, { "DRAWID", R2G_U32, 0, 1 },
        { "NRM", R2G_U32, 32, 0 }, { "LCOL", R2G_U32, 36, 0 }, { "WTS", R2G_U32, 40, 0 },   /* (r2-m3) */
    };
    /* Z: the rasterizer's plane through the VU-exact vertices (default: the exact path's Z on 99.95% of pixels, the rest
     * +-1), or PS2_R2_ZREC=1 exact per-triangle records from the geometry stage (100%, about 2x the GPU time of R2's
     * draws: ep3 hub replay at 3x, notes/modern-renderer.md "R0 + R1 as built") */
    const char* zr = getenv("PS2_R2_ZREC");
    int zrec = zr && *zr == '1';
    r2g_pipe_desc pd = { gpu_sh_vs_r2, sizeof gpu_sh_vs_r2, gpu_sh_ps_r2_dual, sizeof gpu_sh_ps_r2_dual, attrs, 10,
                         gpu_sh_gs_r2, sizeof gpu_sh_gs_r2 };
    if (!zrec) { pd.ps = gpu_sh_ps_r2_iz_dual; pd.ps_size = sizeof gpu_sh_ps_r2_iz_dual; pd.gs = NULL; pd.gs_size = 0; }
    R.pipe = R.d ? r2g_pipe_create(R.d, &pd) : NULL;
    const char* nde = getenv("PS2_R2_NODISCARD");   /* (=0: every draw through the discarding PS, cost comparisons) */
    if (nde && *nde == '0') zrec |= 2;
    if (!zrec) { pd.ps = gpu_sh_ps_r2_nd; pd.ps_size = sizeof gpu_sh_ps_r2_nd; R.pipe_nd = R.d ? r2g_pipe_create(R.d, &pd) : NULL; }
    else R.pipe_nd = R.pipe;
    r2g_pipe_desc pc = { gpu_sh_vs_r2_clip, sizeof gpu_sh_vs_r2_clip, gpu_sh_ps_r2_clip, sizeof gpu_sh_ps_r2_clip, attrs, 10,
                         gpu_sh_gs_r2_clip, sizeof gpu_sh_gs_r2_clip };
    R.pipe_clip = R.d ? r2g_pipe_create(R.d, &pc) : NULL;
    /* (r2-m2) the segment constants by UpdateSubresource (a DEFAULT buffer): Map(DISCARD) per segment, ~45 a frame with
     * frames queued, made the driver block the render thread for up to 10 ms when the GPU was the bottleneck */
    R.cb = R.d ? r2g_buf_create(R.d, R2G_BUF_CONSTANT, R2G_USAGE_DEFAULT, 48, NULL) : NULL;
    if (!R.d || !R.pipe || !R.pipe_clip || !R.pipe_nd || !R.cb) {
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
    uint32_t cap = have ? have : (kind == R2G_BUF_VERTEX ? 65536u : kind == R2G_BUF_RAW ? 16384u : 262144u);
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
    if (a->gen != gen) { a->gen = gen; a->vused = a->iused = a->mused = 0; }   /* (the store reused the slot: a new level) */
    return a;
}

/* a sub-batch's material records (shaders/r2.hlsl r2_mat): passes 0..2 */
static void mat_records(const r2_mesh* m, uint32_t* out) {
    memset(out, 0, (size_t)m->nb * 3 * MAT_BYTES);
    for (uint32_t b = 0; b < m->nb; b++)
        for (uint32_t p = 0; p < m->b[b].npass && p < 3; p++) {
            const r2_pass* P = &m->b[b].pass[p];
            uint32_t* r = out + (b * 3 + p) * (MAT_BYTES / 4);
            r[0] = (uint32_t)P->tex0; r[1] = (uint32_t)(P->tex0 >> 32);
            r[2] = (uint32_t)P->clamp; r[3] = (uint32_t)(P->clamp >> 32);
            r[4] = (uint32_t)P->tex1; r[5] = (uint32_t)P->test; r[6] = P->alpha_enc; r[7] = (uint32_t)P->fogcol & 0xFFFFFF;
            memcpy(&r[8], &m->b[b].fog_scale, 4);
            memcpy(&r[9], &m->b[b].fog_wmax, 4);
            r[10] = P->mflags;
            /* (r2-m3) lit / skinned sub-batches: shaders/r2.hlsl r2_lit */
            const r2_batch* B = &m->b[b];
            r[11] = (uint32_t)B->lit | (uint32_t)B->skin << 1 | (uint32_t)B->tanim << 16;
            r[12] = (uint32_t)B->bone[0] | (uint32_t)B->bone[1] << 16;
            r[13] = (uint32_t)B->bone[2] | (uint32_t)B->bone[3] << 16;
            memcpy(&r[14], &B->bright, 4);
        }
}

static int resident(r2_mesh* m) {
    if (m->gpu_state == (int)R.epoch) return 1;
    if (m->gpu_state == -(int)R.epoch) return 0;
    arena* a = arena_for(m->handle >> 20);
    uint32_t nm = m->nb * 3;
    if ((m->handle & 0xFFFFF) >= MESH_IDX ||
        !arena_grow(&a->vb, R2G_BUF_VERTEX, a->vused, a->vused + m->nvtx, sizeof(r2_vtx)) ||
        !arena_grow(&a->ib, R2G_BUF_INDEX, a->iused, a->iused + m->nidx, 4) ||
        !arena_grow(&a->mb, R2G_BUF_RAW, a->mused, a->mused + nm, MAT_BYTES) ||
        (!a->mt && !(a->mt = r2g_buf_create(R.d, R2G_BUF_RAW, R2G_USAGE_DEFAULT, MESH_IDX * 4, NULL)))) {
        m->gpu_state = -(int)R.epoch;
        return 0;
    }
    uint32_t* mats = malloc((size_t)nm * MAT_BYTES);
    if (!mats) { m->gpu_state = -(int)R.epoch; return 0; }
    mat_records(m, mats);
    r2g_buf_write(R.d, a->vb, a->vused * (uint32_t)sizeof(r2_vtx), m->vtx, m->nvtx * (uint32_t)sizeof(r2_vtx));
    if (m->nidx) r2g_buf_write(R.d, a->ib, a->iused * 4u, m->idx, m->nidx * 4u);
    r2g_buf_write(R.d, a->mb, a->mused * MAT_BYTES, mats, nm * MAT_BYTES);
    free(mats);
    m->gpu_vbase = a->vused;
    m->gpu_ibase = a->iused;
    m->gpu_mbase = a->mused;
    if (m->gpu_tdec) memset(m->gpu_tdec, 0, (size_t)m->nb * 3 * 4);   /* (r2-m7b: the records hold 0) */
    r2g_buf_write(R.d, a->mt, (m->handle & 0xFFFFF) * 4u, &m->gpu_mbase, 4);
    a->vused += m->nvtx;
    a->iused += m->nidx;
    a->mused += nm;
    m->gpu_state = (int)R.epoch;
    R.uploads++;
    R.upload_bytes += m->nvtx * sizeof(r2_vtx) + m->nidx * 4u + nm * MAT_BYTES;
    return 1;
}

static int segment(const gpu_fop_r2* o, const uint8_t* fdata, size_t fdata_bytes, const r2_exec_in* in);
static double ph_ms[4];                  /* (stats) the last segment's phases: residency, constants, bindings, draws */
static double ph_now(void);

/* (r2-m2, PS2_R2_STATS) a slow segment on the render thread is logged (uploads, driver stalls) */
#include <windows.h>
int r2_render_segment(const gpu_fop_r2* o, const uint8_t* fdata, size_t fdata_bytes, const r2_exec_in* in) {
    static int stats = -1;
    static double qf;
    if (stats < 0) {
        const char* e = getenv("PS2_R2_STATS");
        stats = e && *e && *e != '0';
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qf = (double)f.QuadPart / 1000.0;
    }
    if (!stats) return segment(o, fdata, fdata_bytes, in);
    LARGE_INTEGER t0, t1;
    uint64_t up0 = R.uploads;
    QueryPerformanceCounter(&t0);
    int n = segment(o, fdata, fdata_bytes, in);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) / qf;
    if (ms > 2.0)
        fprintf(stderr, "[r2] slow render segment: %.2f ms (residency %.2f, constants %.2f, bindings %.2f, draws %.2f), %u draws, %llu uploads\n",
                ms, ph_ms[0], ph_ms[1], ph_ms[2], ph_ms[3], o->ndraw, (unsigned long long)(R.uploads - up0));
    return n;
}

static double ph_now(void) {
    static double qf;
    LARGE_INTEGER t;
    if (!qf) { QueryPerformanceFrequency(&t); qf = (double)t.QuadPart / 1000.0; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / qf;
}

static int segment(const gpu_fop_r2* o, const uint8_t* fdata, size_t fdata_bytes, const r2_exec_in* in) {
    if (!o->ndraw || !init(in)) return 0;
    double p0 = ph_now();
    if ((size_t)o->draw_off + (size_t)o->ndraw * sizeof(gpu_r2_draw) > fdata_bytes) return 0;
    const gpu_r2_draw* dr = (const gpu_r2_draw*)(fdata + o->draw_off);
    /* residency first (uploads), then the draws */
    arena* a = arena_for(o->gen);
    for (uint32_t i = 0; i < o->ndraw; i++) {
        r2_mesh* m = (r2_mesh*)r2_mesh_get(dr[i].mesh);
        if (m) resident(m);
    }
    if (!a->vb || !a->ib || !a->mb || !a->mt) return 0;
    /* (r2-m7b) the materials' decoded textures (gpu_exec.c td_r2: decoded now when GS memory changed), each mesh once per
     * segment: a material record's word 15 holds 1 + the first decoded word (shaders/r2.hlsl), rewritten when it changes */
    if (in->texdec) {
        uint32_t seg = (uint32_t)R.segments + 1;
        for (uint32_t i = 0; i < o->ndraw; i++) {
            r2_mesh* m = (r2_mesh*)r2_mesh_get(dr[i].mesh);
            if (!m || m->gpu_state != (int)R.epoch || m->gpu_tseg == seg) continue;
            m->gpu_tseg = seg;
            arena* ma = &R.A[(m->handle >> 20) % SLOTS];
            if (ma->gen != (m->handle >> 20) || !ma->mb) continue;
            if (!m->gpu_tdec && !(m->gpu_tdec = calloc((size_t)m->nb * 3, 4))) continue;
            for (uint32_t b = 0; b < m->nb; b++)
                for (uint32_t p = 0; p < m->b[b].npass && p < 3; p++) {
                    uint32_t v = in->texdec(in->texdec_user, m->b[b].pass[p].tex0), k = b * 3 + p;
                    if (v == m->gpu_tdec[k]) continue;
                    r2g_buf_write(R.d, ma->mb, (m->gpu_mbase + k) * MAT_BYTES + 60u, &v, 4);
                    m->gpu_tdec[k] = v;
                }
        }
    }
    double p1 = ph_now();
    r2g_invalidate(R.d);
    /* (r2-m2) the segment's constants are a record in fdata (gpu_r2_seg, written by the frontend; a per-segment constant
     * buffer update stalled the render thread for up to 13 ms when the GPU was the bottleneck). Its sample factors must
     * be the targets' here (a rescale between the two would misplace every vertex). */
    if (dr[0].seg_off + sizeof(gpu_r2_seg) > fdata_bytes) return 0;
    const gpu_r2_seg* sg = (const gpu_r2_seg*)(fdata + dr[0].seg_off);
    if (sg->kx != (float)in->kx || sg->ky != (float)in->ky) { R.missing += o->ndraw; return 0; }
    if (!R.cb_set || R.cb_bias != in->dual_bias) {   /* the device's constants: once */
        float cb[4];
        uint32_t* cu = (uint32_t*)cb;
        const char* ze = getenv("PS2_R2_ZBIAS");
        const char* de = getenv("PS2_R2_DBG");   /* (r2-m2) cost experiments (shaders/r2.hlsl), never in play */
        cb[0] = in->dual_bias; cb[1] = ze ? (float)atof(ze) : 0.0f; cb[2] = 0.0f; cu[3] = de ? (uint32_t)atoi(de) : 0u;
        r2g_buf_write(R.d, R.cb, 0, cb, sizeof cb);
        R.cb_set = 1;
        R.cb_bias = in->dual_bias;
    }
    double p2 = ph_now();
    r2g_bind_targets_native(R.d, in->rtv, in->dsv);
    r2g_viewport(R.d, -8192.0f, -8192.0f, 16384.0f, 16384.0f);   /* (the REF viewport of mixed frames: d3d pixels = NDC * 8192) */
    r2g_scissor(R.d, o->sc[0] * in->kx, o->sc[1] * in->ky, (o->sc[2] + 1) * in->kx, (o->sc[3] + 1) * in->ky);
    r2g_bind_srv_native(R.d, 0, in->vram_srv);
    r2g_bind_srv_native(R.d, 6, in->swz_srv);
    r2g_bind_srv_native(R.d, 7, in->fdata_srv);
    r2g_bind_srv_native(R.d, 13, in->texdec_srv);   /* (r2-m7b) */
    r2g_bind_srv(R.d, 10, a->mb);
    r2g_bind_srv(R.d, 11, a->mt);
    r2g_bind_constants(R.d, 1, R.cb);
    r2g_bind_vertex(R.d, 0, a->vb, sizeof(r2_vtx));
    r2g_bind_vertex_native(R.d, 1, in->iota_vb, 4);
    r2g_bind_index(R.d, a->ib);
    double p3 = ph_now();
    int n = 0;
    for (uint32_t i = 0; i < o->ndraw; i++) {
        const gpu_r2_draw* d = &dr[i];
        const r2_mesh* m = r2_mesh_get(d->mesh);
        if (!m || m->gpu_state != (int)R.epoch || (m->handle >> 20) != o->gen) { R.missing++; continue; }
        uint32_t st = d->state;
        int bl = (int)(st & 15), depth = (int)((st >> 4) & 15);
        r2g_state s = { (uint8_t)bl, (uint8_t)((st >> 8) & 15), (uint8_t)(depth >> 1), (uint8_t)(depth & 1),
                        (uint8_t)(((st >> 12) & 1) ? R2G_CULL_FRONT : R2G_CULL_NONE) };
        r2g_bind_pipe(R.d, (st >> 13) & 1 ? R.pipe_clip : (st >> 14) & 1 ? R.pipe : R.pipe_nd);   /* (filtered when unchanged) */
        r2g_bind_state(R.d, &s);
        r2g_draw_indexed(R.d, d->count, m->gpu_ibase + d->first, (int32_t)m->gpu_vbase, o->draw_off / 32u + i);
        n++;
    }
    r2g_bind_srv(R.d, 10, NULL);
    r2g_bind_srv(R.d, 11, NULL);
    r2g_end(R.d);
    double p4 = ph_now();
    ph_ms[0] = p1 - p0; ph_ms[1] = p2 - p1; ph_ms[2] = p3 - p2; ph_ms[3] = p4 - p3;
    R.draws += (uint64_t)n;
    R.segments++;
    return n;
}

void r2_render_stats(uint64_t* draws, uint64_t* segments, uint64_t* uploads, uint64_t* upload_bytes, uint64_t* missing) {
    *draws = R.draws; *segments = R.segments; *uploads = R.uploads; *upload_bytes = R.upload_bytes; *missing = R.missing;
}
