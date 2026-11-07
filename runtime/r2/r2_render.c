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

#include <math.h>
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
    r2g_pipe *mpipe, *mpipe_nd, *mpipe_a2c;   /* (r2-m8, R8) modern frames: hardware Z on the MFT surfaces (R2_MFT) */
    r2g_pipe *mcpipe, *mcpipe_nd, *mcpipe_a2c;   /* (mft-pop) the same for entries crossing the guard band (r2_gs_clip) */
    r2g_pipe* mvpipe;                    /* (r2-m8) the motion-vector pass */
    r2g_pipe *ppipe, *ppipe_nd, *ppipe_a2c;   /* (r8-perf) the merged draws of a plan (vertices pulled: r2.hlsl R2_PULL) */
    /* (light) the same three sets with Enhanced lighting (r2.hlsl R2_ENH: lit sub-batches per pixel); NULL: not available */
    r2g_pipe *empipe, *empipe_nd, *empipe_a2c, *emcpipe, *emcpipe_nd, *emcpipe_a2c, *eppipe, *eppipe_nd, *eppipe_a2c;
    int enh_ok;
    /* (tex) the modern frame's pipelines with Enhanced textures (r2.hlsl R2_ETEX): [lighting per pixel][family: plain, clip,
     * pull][discard, no discard, alpha-to-coverage]; tex_ok: all of them exist */
    r2g_pipe* tp[2][3][3];
    int tex_ok;
    /* (reimagine) Look = Reimagined: the Enhanced lighting sets above with the R2_ENH=2 pixel shaders - rp in the order of
     * empipe .. eppipe_a2c, rtp as tp[1]; swapped in for a Reimagined segment (reim_ok: all of them exist) */
    r2g_pipe* rp[9];
    r2g_pipe* rtp[3][3];
    int reim_ok;
    r2g_buf* cb;
    int cb_set;                          /* (r2-m2) cb holds the device's constants (r2.hlsl r2_static), written once */
    float cb_bias;
    float cb_rim;                        /* (light) r2_misc.z as written */
    float cb_mft[4];                     /* (r2-m8) r2_mft as written: sx, sy, samples */
    arena A[SLOTS];
    uint32_t epoch;                      /* bumped when GPU resources are recreated: meshes upload again */
    uint64_t uploads, upload_bytes, draws, segments, missing;
    uint64_t merged, gpu_draws;          /* (r8-perf) records drawn in merged draws; the planned segments' GPU draws */
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
    r2g_pipe_free(R.mpipe); r2g_pipe_free(R.mpipe_nd); r2g_pipe_free(R.mpipe_a2c); r2g_pipe_free(R.mvpipe);
    R.mpipe = R.mpipe_nd = R.mpipe_a2c = R.mvpipe = NULL;
    r2g_pipe_free(R.ppipe); r2g_pipe_free(R.ppipe_nd); r2g_pipe_free(R.ppipe_a2c);
    R.ppipe = R.ppipe_nd = R.ppipe_a2c = NULL;
    r2g_pipe_free(R.mcpipe); r2g_pipe_free(R.mcpipe_nd); r2g_pipe_free(R.mcpipe_a2c);
    R.mcpipe = R.mcpipe_nd = R.mcpipe_a2c = NULL;
    r2g_pipe* ep[9] = { R.empipe, R.empipe_nd, R.empipe_a2c, R.emcpipe, R.emcpipe_nd, R.emcpipe_a2c, R.eppipe, R.eppipe_nd, R.eppipe_a2c };
    for (int k = 0; k < 9; k++) r2g_pipe_free(ep[k]);
    R.empipe = R.empipe_nd = R.empipe_a2c = R.emcpipe = R.emcpipe_nd = R.emcpipe_a2c = R.eppipe = R.eppipe_nd = R.eppipe_a2c = NULL;
    R.enh_ok = 0;
    for (int a = 0; a < 2; a++) for (int f = 0; f < 3; f++) for (int v = 0; v < 3; v++) { r2g_pipe_free(R.tp[a][f][v]); R.tp[a][f][v] = NULL; }
    R.tex_ok = 0;
    for (int k = 0; k < 9; k++) { r2g_pipe_free(R.rp[k]); R.rp[k] = NULL; }   /* (reimagine) */
    for (int f = 0; f < 3; f++) for (int v = 0; v < 3; v++) { r2g_pipe_free(R.rtp[f][v]); R.rtp[f][v] = NULL; }
    R.reim_ok = 0;
    memset(R.cb_mft, 0, sizeof R.cb_mft);
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
    /* (r2-m8, R8) modern frames: the same draws on the MFT surfaces (hardware Z; with MSAA, alpha-to-coverage) */
    r2g_pipe_desc pm = { gpu_sh_vs_r2m, sizeof gpu_sh_vs_r2m, gpu_sh_ps_r2m, sizeof gpu_sh_ps_r2m, attrs, 10, NULL, 0 };
    R.mpipe = R.d ? r2g_pipe_create(R.d, &pm) : NULL;
    pm.ps = gpu_sh_ps_r2m_nd; pm.ps_size = sizeof gpu_sh_ps_r2m_nd;
    R.mpipe_nd = R.d ? r2g_pipe_create(R.d, &pm) : NULL;
    pm.ps = gpu_sh_ps_r2m_a2c; pm.ps_size = sizeof gpu_sh_ps_r2m_a2c;
    R.mpipe_a2c = R.d ? r2g_pipe_create(R.d, &pm) : NULL;
    r2g_pipe_desc pmc = { gpu_sh_vs_r2m_clip, sizeof gpu_sh_vs_r2m_clip, gpu_sh_ps_r2m_clip, sizeof gpu_sh_ps_r2m_clip, attrs, 10,
                          gpu_sh_gs_r2m_clip, sizeof gpu_sh_gs_r2m_clip };
    R.mcpipe = R.d ? r2g_pipe_create(R.d, &pmc) : NULL;
    pmc.ps = gpu_sh_ps_r2m_clip_nd; pmc.ps_size = sizeof gpu_sh_ps_r2m_clip_nd;
    R.mcpipe_nd = R.d ? r2g_pipe_create(R.d, &pmc) : NULL;
    pmc.ps = gpu_sh_ps_r2m_clip_a2c; pmc.ps_size = sizeof gpu_sh_ps_r2m_clip_a2c;
    R.mcpipe_a2c = R.d ? r2g_pipe_create(R.d, &pmc) : NULL;
    r2g_pipe_desc pv = { gpu_sh_vs_r2m_mv, sizeof gpu_sh_vs_r2m_mv, gpu_sh_ps_r2m_mv, sizeof gpu_sh_ps_r2m_mv, attrs, 10, NULL, 0 };
    R.mvpipe = R.d ? r2g_pipe_create(R.d, &pv) : NULL;
    /* (r8-perf) the plan's merged draws: the same pixel shaders, vertices pulled per instance's draw record (optional:
     * without them every record is drawn on its own) */
    static const r2g_attr pattrs[] = { { "DRAWID", R2G_U32, 0, 1 } };
    r2g_pipe_desc pp = { gpu_sh_vs_r2m_pull, sizeof gpu_sh_vs_r2m_pull, gpu_sh_ps_r2m, sizeof gpu_sh_ps_r2m, pattrs, 1, NULL, 0 };
    R.ppipe = R.d ? r2g_pipe_create(R.d, &pp) : NULL;
    pp.ps = gpu_sh_ps_r2m_nd; pp.ps_size = sizeof gpu_sh_ps_r2m_nd;
    R.ppipe_nd = R.d ? r2g_pipe_create(R.d, &pp) : NULL;
    pp.ps = gpu_sh_ps_r2m_a2c; pp.ps_size = sizeof gpu_sh_ps_r2m_a2c;
    R.ppipe_a2c = R.d ? r2g_pipe_create(R.d, &pp) : NULL;
    if (R.d && (!R.ppipe || !R.ppipe_nd || !R.ppipe_a2c)) fprintf(stderr, "[r2] render: no merged-draw pipelines (every draw on its own)\n");
    /* (light) Enhanced lighting's pipelines (optional: without them modern frames keep the per-vertex lighting) */
    if (R.d) {
        r2g_pipe_desc e = { gpu_sh_vs_r2m_e, sizeof gpu_sh_vs_r2m_e, gpu_sh_ps_r2m_e, sizeof gpu_sh_ps_r2m_e, attrs, 10, NULL, 0 };
        R.empipe = r2g_pipe_create(R.d, &e);
        e.ps = gpu_sh_ps_r2m_nd_e; e.ps_size = sizeof gpu_sh_ps_r2m_nd_e;
        R.empipe_nd = r2g_pipe_create(R.d, &e);
        e.ps = gpu_sh_ps_r2m_a2c_e; e.ps_size = sizeof gpu_sh_ps_r2m_a2c_e;
        R.empipe_a2c = r2g_pipe_create(R.d, &e);
        r2g_pipe_desc ec = { gpu_sh_vs_r2m_clip_e, sizeof gpu_sh_vs_r2m_clip_e, gpu_sh_ps_r2m_clip_e, sizeof gpu_sh_ps_r2m_clip_e, attrs, 10,
                             gpu_sh_gs_r2m_clip_e, sizeof gpu_sh_gs_r2m_clip_e };
        R.emcpipe = r2g_pipe_create(R.d, &ec);
        ec.ps = gpu_sh_ps_r2m_clip_nd_e; ec.ps_size = sizeof gpu_sh_ps_r2m_clip_nd_e;
        R.emcpipe_nd = r2g_pipe_create(R.d, &ec);
        ec.ps = gpu_sh_ps_r2m_clip_a2c_e; ec.ps_size = sizeof gpu_sh_ps_r2m_clip_a2c_e;
        R.emcpipe_a2c = r2g_pipe_create(R.d, &ec);
        r2g_pipe_desc ee = { gpu_sh_vs_r2m_pull_e, sizeof gpu_sh_vs_r2m_pull_e, gpu_sh_ps_r2m_e, sizeof gpu_sh_ps_r2m_e, pattrs, 1, NULL, 0 };
        R.eppipe = r2g_pipe_create(R.d, &ee);
        ee.ps = gpu_sh_ps_r2m_nd_e; ee.ps_size = sizeof gpu_sh_ps_r2m_nd_e;
        R.eppipe_nd = r2g_pipe_create(R.d, &ee);
        ee.ps = gpu_sh_ps_r2m_a2c_e; ee.ps_size = sizeof gpu_sh_ps_r2m_a2c_e;
        R.eppipe_a2c = r2g_pipe_create(R.d, &ee);
        R.enh_ok = R.empipe && R.empipe_nd && R.empipe_a2c && R.emcpipe && R.emcpipe_nd && R.emcpipe_a2c &&
                   (R.eppipe && R.eppipe_nd && R.eppipe_a2c ? 1 : 0);
        if (!R.enh_ok) fprintf(stderr, "[r2] render: no Enhanced lighting pipelines (per-vertex lighting)\n");
    }
    /* (tex) Enhanced textures' pipelines (optional: without them modern frames keep the original textures) */
    if (R.d) {
#define PS_(n) gpu_sh_##n, sizeof gpu_sh_##n
        static const struct { const void* p; size_t n; } tps[2][2][3] = {
            { { { PS_(ps_r2m_t) }, { PS_(ps_r2m_nd_t) }, { PS_(ps_r2m_a2c_t) } },
              { { PS_(ps_r2m_clip_t) }, { PS_(ps_r2m_clip_nd_t) }, { PS_(ps_r2m_clip_a2c_t) } } },
            { { { PS_(ps_r2m_e_t) }, { PS_(ps_r2m_nd_e_t) }, { PS_(ps_r2m_a2c_e_t) } },
              { { PS_(ps_r2m_clip_e_t) }, { PS_(ps_r2m_clip_nd_e_t) }, { PS_(ps_r2m_clip_a2c_e_t) } } } };
#undef PS_
        R.tex_ok = 1;
        for (int a = 0; a < 2; a++)
            for (int f = 0; f < 3; f++)
                for (int v = 0; v < 3; v++) {
                    r2g_pipe_desc t;
                    if (f == 0) t = (r2g_pipe_desc){ a ? gpu_sh_vs_r2m_e : gpu_sh_vs_r2m, a ? sizeof gpu_sh_vs_r2m_e : sizeof gpu_sh_vs_r2m,
                                                     tps[a][0][v].p, tps[a][0][v].n, attrs, 10, NULL, 0 };
                    else if (f == 1) t = (r2g_pipe_desc){ a ? gpu_sh_vs_r2m_clip_e : gpu_sh_vs_r2m_clip,
                                                          a ? sizeof gpu_sh_vs_r2m_clip_e : sizeof gpu_sh_vs_r2m_clip,
                                                          tps[a][1][v].p, tps[a][1][v].n, attrs, 10,
                                                          a ? gpu_sh_gs_r2m_clip_e : gpu_sh_gs_r2m_clip,
                                                          a ? sizeof gpu_sh_gs_r2m_clip_e : sizeof gpu_sh_gs_r2m_clip };
                    else t = (r2g_pipe_desc){ a ? gpu_sh_vs_r2m_pull_e : gpu_sh_vs_r2m_pull,
                                              a ? sizeof gpu_sh_vs_r2m_pull_e : sizeof gpu_sh_vs_r2m_pull,
                                              tps[a][0][v].p, tps[a][0][v].n, pattrs, 1, NULL, 0 };
                    R.tp[a][f][v] = r2g_pipe_create(R.d, &t);
                    if (!R.tp[a][f][v]) R.tex_ok = 0;
                }
        if (!R.tex_ok) fprintf(stderr, "[r2] render: no Enhanced textures pipelines (original textures)\n");
    }
    /* (reimagine) Look = Reimagined's R2 pipelines: the Enhanced lighting ones with the R2_ENH=2 pixel shaders */
    if (R.d) {
#define PS_(n) gpu_sh_##n, sizeof gpu_sh_##n
        static const struct { const void* p; size_t n; } rps[2][3] = {
            { { PS_(ps_r2m_e_r) }, { PS_(ps_r2m_nd_e_r) }, { PS_(ps_r2m_a2c_e_r) } },
            { { PS_(ps_r2m_clip_e_r) }, { PS_(ps_r2m_clip_nd_e_r) }, { PS_(ps_r2m_clip_a2c_e_r) } } };
        static const struct { const void* p; size_t n; } rtps[2][3] = {
            { { PS_(ps_r2m_e_t_r) }, { PS_(ps_r2m_nd_e_t_r) }, { PS_(ps_r2m_a2c_e_t_r) } },
            { { PS_(ps_r2m_clip_e_t_r) }, { PS_(ps_r2m_clip_nd_e_t_r) }, { PS_(ps_r2m_clip_a2c_e_t_r) } } };
#undef PS_
        R.reim_ok = 1;
        for (int f = 0; f < 3; f++)          /* (family: plain, clip, pull; variant: discard, no discard, alpha-to-coverage) */
            for (int v = 0; v < 3; v++)
                for (int t = 0; t < 2; t++) {
                    const void* ps = t ? rtps[f == 1][v].p : rps[f == 1][v].p;
                    size_t pn = t ? rtps[f == 1][v].n : rps[f == 1][v].n;
                    r2g_pipe_desc d;
                    if (f == 0) d = (r2g_pipe_desc){ gpu_sh_vs_r2m_e, sizeof gpu_sh_vs_r2m_e, ps, pn, attrs, 10, NULL, 0 };
                    else if (f == 1) d = (r2g_pipe_desc){ gpu_sh_vs_r2m_clip_e, sizeof gpu_sh_vs_r2m_clip_e, ps, pn, attrs, 10,
                                                          gpu_sh_gs_r2m_clip_e, sizeof gpu_sh_gs_r2m_clip_e };
                    else d = (r2g_pipe_desc){ gpu_sh_vs_r2m_pull_e, sizeof gpu_sh_vs_r2m_pull_e, ps, pn, pattrs, 1, NULL, 0 };
                    r2g_pipe* p = r2g_pipe_create(R.d, &d);
                    if (t) R.rtp[f][v] = p; else R.rp[f * 3 + v] = p;
                    if (!p) R.reim_ok = 0;
                }
        if (!R.reim_ok) fprintf(stderr, "[r2] render: no Reimagined pipelines (Enhanced lighting's instead)\n");
    }
    /* (r2-m2) the segment constants by UpdateSubresource (a DEFAULT buffer): Map(DISCARD) per segment, ~45 a frame with
     * frames queued, made the driver block the render thread for up to 10 ms when the GPU was the bottleneck */
    R.cb = R.d ? r2g_buf_create(R.d, R2G_BUF_CONSTANT, R2G_USAGE_DEFAULT, 48, NULL) : NULL;
    if (!R.d || !R.pipe || !R.pipe_clip || !R.pipe_nd || !R.cb || !R.mpipe || !R.mpipe_nd || !R.mpipe_a2c || !R.mvpipe ||
        !R.mcpipe || !R.mcpipe_nd || !R.mcpipe_a2c) {
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
        (!a->mt && !(a->mt = r2g_buf_create(R.d, R2G_BUF_RAW, R2G_USAGE_DEFAULT, MESH_IDX * 16, NULL)))) {
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
    uint32_t mt[4] = { m->gpu_mbase, m->gpu_vbase, m->gpu_ibase, 0 };   /* (r8-perf: the bases the merged draws pull by) */
    r2g_buf_write(R.d, a->mt, (m->handle & 0xFFFFF) * 16u, mt, 16);
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
    if (R.segments % 20000 == 0)
        fprintf(stderr, "[r2] render: %llu segments, %llu draws (%llu of them in merged GPU draws; planned segments: %llu GPU draws)\n",
                (unsigned long long)R.segments, (unsigned long long)R.draws, (unsigned long long)R.merged, (unsigned long long)R.gpu_draws);
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
                    const r2_pass* P = &m->b[b].pass[p];
                    r2_tex_use u = { P->clamp, P->tex1, P->test, P->mflags, p };
                    uint32_t v = in->texdec(in->texdec_user, P->tex0, &u), k = b * 3 + p;
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
    if (!in->mft && (sg->kx != (float)in->kx || sg->ky != (float)in->ky)) { R.missing += o->ndraw; return 0; }
    float mft[4] = { in->mft ? in->sx : 0.0f, in->mft ? in->sy : 0.0f, in->mft ? (float)in->msaa : 0.0f, 0.0f };
    /* (light) Enhanced lighting's pipelines for this segment (modern frames), the rim strength in r2_misc.z */
    int enh = in->mft && in->enh && R.enh_ok && !in->mv;
    float rim = enh ? in->rim : 0.0f;
    /* (reimagine) Look = Reimagined: its pipelines in the Enhanced ones' place for this segment, the specular strength in
     * r2_mft.w (0 otherwise, as before) */
    int reim = enh && in->reim && R.reim_ok;
    r2g_pipe *rsave[9], *rtsave[3][3];
    r2g_pipe** epp[9] = { &R.empipe, &R.empipe_nd, &R.empipe_a2c, &R.emcpipe, &R.emcpipe_nd, &R.emcpipe_a2c,
                          &R.eppipe, &R.eppipe_nd, &R.eppipe_a2c };
    if (reim) {
        mft[3] = in->spec;
        for (int k = 0; k < 9; k++) { rsave[k] = *epp[k]; *epp[k] = R.rp[k]; }
        memcpy(rtsave, R.tp[1], sizeof rtsave);
        memcpy(R.tp[1], R.rtp, sizeof rtsave);
    }
    if (!R.cb_set || R.cb_bias != in->dual_bias || (in->mft && memcmp(mft, R.cb_mft, sizeof mft)) || (enh && rim != R.cb_rim)) {   /* the device's constants */
        float cb[12];                    /* (the whole 48-byte buffer: UpdateSubresource of a constant buffer writes all of it) */
        uint32_t* cu = (uint32_t*)cb;
        memset(cb, 0, sizeof cb);
        const char* ze = getenv("PS2_R2_ZBIAS");
        const char* de = getenv("PS2_R2_DBG");   /* (r2-m2) cost experiments (shaders/r2.hlsl), never in play */
        cb[0] = in->dual_bias; cb[1] = ze ? (float)atof(ze) : 0.0f; cb[2] = 0.0f; cu[3] = de ? (uint32_t)atoi(de) : 0u;
        if (enh) R.cb_rim = rim;         /* (light: read by the R2_ENH pixel shaders only) */
        cb[2] = R.cb_rim;
        const char* ce = getenv("PS2_R2_CLIP");  /* (r2-m5b) 1: crossing triangles GPU-clipped (else the VU's fans draw them) */
        if (ce && *ce == '1') cu[3] |= 64u;
        if (in->mft) memcpy(R.cb_mft, mft, sizeof mft);
        memcpy(&cb[4], R.cb_mft, sizeof R.cb_mft);   /* (r2-m8) r2_mft: the modern frame's scale and samples */
        r2g_buf_write(R.d, R.cb, 0, cb, sizeof cb);
        R.cb_set = 1;
        R.cb_bias = in->dual_bias;
    }
    double p2 = ph_now();
    /* (tex) Enhanced textures' pipelines for this segment (modern frames, not the motion vectors) and their bindings */
    int etex = in->mft && in->etex && R.tex_ok && !in->mv && in->etex_bind;
    if (etex) in->etex_bind();
    r2g_bind_targets_native(R.d, in->rtv, in->dsv);
    r2g_viewport(R.d, -8192.0f, -8192.0f, 16384.0f, 16384.0f);   /* (the REF viewport of mixed frames: d3d pixels = NDC * 8192) */
    if (in->dbg_cull) r2g_scissor(R.d, 0, 0, 0, 0);
    else if (in->mft)                    /* (r2-m8) native pixel x covers d3d [x s, (x + 1) s): the pixels whose centres are inside */
        r2g_scissor(R.d, (int)ceilf(o->sc[0] * in->sx - 0.5f), (int)ceilf(o->sc[1] * in->sy - 0.5f),
                    (int)ceilf((o->sc[2] + 1) * in->sx - 0.5f), (int)ceilf((o->sc[3] + 1) * in->sy - 0.5f));
    else r2g_scissor(R.d, o->sc[0] * in->kx, o->sc[1] * in->ky, (o->sc[2] + 1) * in->kx, (o->sc[3] + 1) * in->ky);
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
    int n = 0, pulled = 0;
    /* (r8-perf) the segment's plan (gpu_r2front.c r2_plan): fewer, merged draws in an order equivalent to the game's. Only
     * on the modern frame's surfaces, with every mesh resident in the segment's generation */
    uint32_t np = o->nplan;
    const gpu_r2_chunk* pc = NULL;
    if (np && in->mft && (in->opt & R2X_BATCH) && !in->mv && !in->dbg_cull && R.ppipe && R.ppipe_nd && R.ppipe_a2c &&
        (size_t)o->plan_off + (size_t)np * sizeof(gpu_r2_chunk) <= fdata_bytes) {
        pc = (const gpu_r2_chunk*)(fdata + o->plan_off);
        for (uint32_t i = 0; i < o->ndraw && pc; i++) {
            const r2_mesh* m = r2_mesh_get(dr[i].mesh);
            if (!m || m->gpu_state != (int)R.epoch || (m->handle >> 20) != o->gen) pc = NULL;
        }
        for (uint32_t c = 0; c < np && pc; c++)   /* (merged chunks never hold guard-band entries: r2_plan keeps them plain) */
            if (pc[c].n ? ((size_t)pc[c].first * 32u + (size_t)pc[c].n * 4u > fdata_bytes || pc[c].maxcount % 3u ||
                           ((pc[c].state >> 13) & 1))
                        : pc[c].first >= o->ndraw)
                pc = NULL;
    }
    if (pc) {
        r2g_bind_srv_vs(R.d, 20, a->vb);
        r2g_bind_srv_vs(R.d, 21, a->ib);
        pulled = 1;
        static int rev = -1;             /* (diagnostics: PS2_R2_PLAN_REVERSE=1 draws the plan backwards - a wrong order the
                                            verifier, PS2_R8P_VERIFY, must catch) */
        if (rev < 0) { const char* e = getenv("PS2_R2_PLAN_REVERSE"); rev = e && *e == '1'; }
        for (uint32_t cc = 0; cc < np; cc++) {
            uint32_t c = rev ? np - 1 - cc : cc;
            uint32_t st = pc[c].state;
            int bl = (int)(st & 15), depth = (int)((st >> 4) & 15), disc = (st >> 14) & 1;
            r2g_state s = { (uint8_t)bl, (uint8_t)((st >> 8) & 15), (uint8_t)(depth >> 1), (uint8_t)(depth & 1),
                            (uint8_t)(((st >> 12) & 1) ? R2G_CULL_FRONT : R2G_CULL_NONE) };
            if (pc[c].n) {
                int el = 0;                  /* (light: the Enhanced pipelines only where a member mesh is lit - they cost) */
                if (enh) {
                    const uint32_t* mem = (const uint32_t*)(fdata + (size_t)pc[c].first * 32u);
                    for (uint32_t k = 0; k < pc[c].n && !el; k++) {
                        size_t ro = (size_t)mem[k] * 32u;
                        if (ro + sizeof(gpu_r2_draw) > fdata_bytes) { el = 1; break; }
                        const r2_mesh* mm = r2_mesh_get(((const gpu_r2_draw*)(fdata + ro))->mesh);
                        el = !mm || mm->any_lit;
                    }
                }
                if (etex) r2g_bind_pipe(R.d, R.tp[el][2][disc ? (in->msaa > 1 ? 2 : 0) : 1]);
                else
                if (el) r2g_bind_pipe(R.d, disc ? (in->msaa > 1 ? R.eppipe_a2c : R.eppipe) : R.eppipe_nd);
                else
                r2g_bind_pipe(R.d, disc ? (in->msaa > 1 ? R.ppipe_a2c : R.ppipe) : R.ppipe_nd);
                r2g_bind_state(R.d, &s);
                r2g_draw_instanced(R.d, pc[c].maxcount, pc[c].n, pc[c].first);
                n += (int)pc[c].n;
                R.merged += pc[c].n;
            } else {
                uint32_t i = pc[c].first;
                const gpu_r2_draw* d = &dr[i];
                const r2_mesh* m = r2_mesh_get(d->mesh);
                if (etex) r2g_bind_pipe(R.d, R.tp[enh && (!m || m->any_lit)][(st >> 13) & 1][disc ? (in->msaa > 1 ? 2 : 0) : 1]);
                else
                if (enh && (!m || m->any_lit)) {
                    if ((st >> 13) & 1) r2g_bind_pipe(R.d, disc ? (in->msaa > 1 ? R.emcpipe_a2c : R.emcpipe) : R.emcpipe_nd);
                    else r2g_bind_pipe(R.d, disc ? (in->msaa > 1 ? R.empipe_a2c : R.empipe) : R.empipe_nd);
                } else
                if ((st >> 13) & 1) r2g_bind_pipe(R.d, disc ? (in->msaa > 1 ? R.mcpipe_a2c : R.mcpipe) : R.mcpipe_nd);
                else r2g_bind_pipe(R.d, disc ? (in->msaa > 1 ? R.mpipe_a2c : R.mpipe) : R.mpipe_nd);
                r2g_bind_state(R.d, &s);
                r2g_draw_indexed(R.d, d->count, m->gpu_ibase + d->first, (int32_t)m->gpu_vbase, o->draw_off / 32u + i);
                n++;
            }
            R.gpu_draws++;
        }
    }
    for (uint32_t i = 0; i < o->ndraw && !pc; i++) {
        const gpu_r2_draw* d = &dr[i];
        const r2_mesh* m = r2_mesh_get(d->mesh);
        if (!m || m->gpu_state != (int)R.epoch || (m->handle >> 20) != o->gen) { R.missing++; continue; }
        uint32_t st = d->state;
        int bl = (int)(st & 15), depth = (int)((st >> 4) & 15);
        r2g_state s = { (uint8_t)bl, (uint8_t)((st >> 8) & 15), (uint8_t)(depth >> 1), (uint8_t)(depth & 1),
                        (uint8_t)(((st >> 12) & 1) ? R2G_CULL_FRONT : R2G_CULL_NONE) };
        if (in->mv) {                    /* (r2-m8) the motion vectors: pass A, its own depth surface passes, two channels */
            if (((st >> 13) & 1) || (d->mode & 3)) continue;
            r2g_state sm = { 0, 3, R2G_Z_GEQUAL, 0, s.cull };
            r2g_bind_pipe(R.d, R.mvpipe);
            r2g_bind_state(R.d, &sm);
            r2g_draw_indexed(R.d, d->count, m->gpu_ibase + d->first, (int32_t)m->gpu_vbase, o->draw_off / 32u + i);
            n++;
            continue;
        }
        if (in->dbg_cull == 3) {         /* (diagnostics: one pipeline and state for every draw) */
            r2g_bind_pipe(R.d, R.mpipe_nd);
            r2g_state s0 = { 0, 15, R2G_Z_GEQUAL, 1, 0 };
            r2g_bind_state(R.d, &s0);
            r2g_draw_indexed(R.d, 3u, m->gpu_ibase + d->first, (int32_t)m->gpu_vbase, o->draw_off / 32u + i);
            n++;
            continue;
        }
        if (etex) {                      /* (tex) */
            int dv = (st >> 14) & 1;
            r2g_bind_pipe(R.d, R.tp[enh && m->any_lit][(st >> 13) & 1][dv ? (in->msaa > 1 ? 2 : 0) : 1]);
        } else
        if (enh && m->any_lit) {         /* (light: lit meshes only) */
            if ((st >> 13) & 1) r2g_bind_pipe(R.d, (st >> 14) & 1 ? (in->msaa > 1 ? R.emcpipe_a2c : R.emcpipe) : R.emcpipe_nd);
            else r2g_bind_pipe(R.d, (st >> 14) & 1 ? (in->msaa > 1 ? R.empipe_a2c : R.empipe) : R.empipe_nd);
        } else
        if (in->mft) {                   /* (r2-m8; mft-pop: entries crossing the guard band through the clip stage) */
            if ((st >> 13) & 1) r2g_bind_pipe(R.d, (st >> 14) & 1 ? (in->msaa > 1 ? R.mcpipe_a2c : R.mcpipe) : R.mcpipe_nd);
            else r2g_bind_pipe(R.d, (st >> 14) & 1 ? (in->msaa > 1 ? R.mpipe_a2c : R.mpipe) : R.mpipe_nd);
        } else
        r2g_bind_pipe(R.d, (st >> 13) & 1 ? R.pipe_clip : (st >> 14) & 1 ? R.pipe : R.pipe_nd);   /* (filtered when unchanged) */
        r2g_bind_state(R.d, &s);
        r2g_draw_indexed(R.d, in->dbg_cull == 2 ? 3u : d->count, m->gpu_ibase + d->first, (int32_t)m->gpu_vbase, o->draw_off / 32u + i);
        n++;
    }
    if (pulled) { r2g_bind_srv_vs(R.d, 20, NULL); r2g_bind_srv_vs(R.d, 21, NULL); }
    r2g_bind_srv(R.d, 10, NULL);
    r2g_bind_srv(R.d, 11, NULL);
    r2g_end(R.d);
    if (reim) {                          /* (reimagine) the Enhanced lighting pipelines back */
        for (int k = 0; k < 9; k++) *epp[k] = rsave[k];
        memcpy(R.tp[1], rtsave, sizeof rtsave);
    }
    double p4 = ph_now();
    ph_ms[0] = p1 - p0; ph_ms[1] = p2 - p1; ph_ms[2] = p3 - p2; ph_ms[3] = p4 - p3;
    R.draws += (uint64_t)n;
    R.segments++;
    return n;
}

void r2_render_stats(uint64_t* draws, uint64_t* segments, uint64_t* uploads, uint64_t* upload_bytes, uint64_t* missing) {
    *draws = R.draws; *segments = R.segments; *uploads = R.uploads; *upload_bytes = R.upload_bytes; *missing = R.missing;
}
