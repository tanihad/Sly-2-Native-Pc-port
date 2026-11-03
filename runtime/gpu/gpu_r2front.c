/* (r2-m1) The frontend side of the modern renderer (notes/modern-renderer.md 2.5; runtime/r2/r2.h): R2 hints in stream
 * order -> GPU_FOP_R2 segments.
 *
 * R2H_FRAME + words: the frame's VU constants (r2_frame_block) and their screen model. R2H_DRAW + words: one native entry
 * (its mesh, world matrix, fade). Consecutive entries with the same drawing environment form one segment, closed by the
 * next event of any other kind (primitive, transfer, TEX0, reset, VBlank, checkpoint: gf_r2_flush). At the close:
 *   - the colour target of FRAME_1 and the depth target of ZBUF_1 (the main program draws in context 1), with the sample
 *     grids paired; ownership as for an exact draw that reads and writes both over the scissor (stale blocks loaded first,
 *     the blocks GPU-written afterwards), so later downloads, CLUT loads and the verifier see R2's pixels;
 *   - every texture and CLUT the draws sample made current in vram_buf (pending patches flushed, GPU data downloaded);
 *   - the draw records in the game's order: per sub-batch, per pass, the alpha test split into a "pass" and a "fail"
 *     draw only when a pass's source alpha range (vertex alpha x the CLUT entries' alpha) straddles the reference;
 *     material records are static (r2_render.c). (r2-m2) PS2_R2_CONSOLIDATE=1: one draw per (entry, pass, state, discard
 *     rule) over the entry's sub-batches (a batch mask where some belong to another draw), all of pass A before pass B
 *     - not the default: it changes foliage edges (ep2 hub) and the draw count was not R2's GPU cost.
 * (r2-m2) Hybrid mode: R2H_RUN hints, the entries from the frame's scene slot (r2.h r2_slot, W x C computed on the EE).
 * Bracket mode (R2H_BEGIN / R2H_END around an entry's exact CALLs): ignored, or, with gpu_front_r2_ab(1) (PS2_R2_AB), the
 * exact primitives between them are skipped and R2 draws the entry instead: the hybrid picture from the same stream. */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"
#include "r2/r2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* (r2-m2, PS2_R2_STATS) the frontend's R2 time per frame: slow frames are logged */
static double fr_ms, qpf_ms;
static double qms(void) {
    LARGE_INTEGER t, f;
    if (!qpf_ms) { QueryPerformanceFrequency(&f); qpf_ms = (double)f.QuadPart / 1000.0; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / qpf_ms;
}

void gf_flush_open(void);                /* gpu_front.c */

int gf_r2_pending;                       /* a segment is open (gpu_front.c calls gf_r2_flush before its other events) */
int gf_r2_skip;                          /* bracket A/B: the exact primitives of a bracketed entry are not drawn */
static int ab_native;                    /* gpu_front_r2_ab */
/* (r2-m7) the modern renderer's native passes (gpu_front.c: shadow volumes as commutative G adds, the shadow darken and the
 * motion blur as compute passes) run while this is > 0: R2H_FRAME sets it to 2 (the frame and the next VBlank, where the
 * game draws the motion blur before the next frame's hints), every VBlank counts it down, a bracket recording replayed
 * exact (--r2-ab 0) clears it at its first R2H_BEGIN. So reference mode, and all-exact replays, never see them.
 * PS2_R2_FX=0: off. */
int gf_r2_fx_ttl;
/* (r2-m7b) the decoded texture cache (exact, gpu_exec.c texdec) also covers the start of the next frames, where the game draws
 * the sky before that frame's first hint (frames of 2 VBlanks at a capped frame rate): 4 VBlanks */
int gf_r2_td_ttl;
static int fx_env = -1;
/* (r2-m6, R6) inside the particle class's span of this frame (R2H_PART): gpu_front.c draws its triangles natively */
int gf_r2_part;

/* ---- receiving hint words ----------------------------------------------------------------------------------------- */
enum { RX_IDLE, RX_FRAME, RX_ENTRY, RX_XQ };
static struct {
    int state, bracket;
    uint32_t need, got, handle, flags;
    uint32_t w[32];
} RX;
/* (r2-m3, bracket) the frame's extra blocks (R2H_XQ): index -> words in XQW */
#define XQ_MAX 256u
static uint32_t XQW[XQ_MAX * 4 * 64], xq_nw, xq_off[XQ_MAX], xq_n[XQ_MAX], xq_cur, xq_have;
static r2_frame_block FB;
static r2_screen_model SM;
static int fb_ok;
static uint32_t fb_serial;

/* ---- the open segment ---------------------------------------------------------------------------------------------- */
typedef struct { uint32_t handle, ent; } seg_item;
static seg_item* items;
static gpu_r2_entry* ents;
static uint32_t nitems, capitems, nents, capents;
static uint32_t *RT, nrt, caprt, *ent_rt, capert;   /* (r2-m5b) the entries' run tables (R2E_RUNS), ent_rt[e] = offset + 1 */
static uint64_t seg_frame, seg_zbuf, seg_sc, seg_xyo, seg_fba, seg_cc;

/* statistics (gpu_front_r2_stats) */
static gpu_r2_front_stats R2S;
/* (r2-m6) gpu_front.c counts the particle span's primitives: 0 native triangles, 1 exact FF triangles, 2 ROV triangles, 3 others */
void gf_r2_part_count(int k) { if (k == 0) R2S.part_prims++; else if (k == 1) R2S.part_exact++; else if (k == 2) R2S.part_rov++; else R2S.part_other++; }
static gpu_r2_draw* R2S_draws;           /* the closing segment's draw records */
static uint32_t R2S_capdraws;

static int grow(void** p, uint32_t* cap, uint32_t need, size_t el) {
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap * 2 : 256;
    while (c < need) c *= 2;
    void* n = realloc(*p, (size_t)c * el);
    if (!n) return 0;
    *p = n;
    *cap = c;
    return 1;
}

/* ---- texture / CLUT memory and the CLUT's alpha range ---------------------------------------------------------- */
#define TC_N 1024
static struct { uint64_t key; uint32_t frame; uint8_t amin, amax; } TC[TC_N];

static const uint8_t* mem_now(void) { return gf_vram ? gf_vram : gs_vram; }

static void tex_ready(uint64_t tex0, int* amin, int* amax) {
    uint32_t tbp = (uint32_t)(tex0 & 0x3FFF), tbw = (uint32_t)((tex0 >> 14) & 0x3F), psm = (uint32_t)((tex0 >> 20) & 0x3F);
    uint32_t tw = (uint32_t)((tex0 >> 26) & 0xF), th = (uint32_t)((tex0 >> 30) & 0xF), cbp = (uint32_t)((tex0 >> 37) & 0x3FFF);
    uint64_t key = (tex0 & 0x1FFFFFFFFFull) | (uint64_t)cbp << 37;
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 54) & (TC_N - 1);
    if (TC[h].key == key && TC[h].frame == gf_frame + 1) { *amin = TC[h].amin; *amax = TC[h].amax; return; }
    int w = 1 << (tw > 10 ? 10 : tw), hh = 1 << (th > 10 ? 10 : th);
    if (own_mem_read((int)psm, tbp, tbw, 0, 0, w > 2048 ? 2047 : w - 1, hh > 2048 ? 2047 : hh - 1)) R2S.tex_gpu++;
    int t8 = psm == PSMT8;
    if (own_mem_read(PSMCT32, cbp, 1, 0, 0, t8 ? 15 : 7, t8 ? 15 : 1)) R2S.tex_gpu++;
    /* the CLUT entries' alpha (CSM1 CT32: entry i at word cbp * 64 + the 16x16 / 8x2 layout) */
    const uint8_t* m = mem_now();
    int lo = 255, hi = 0, n = t8 ? 256 : 16;
    for (int i = 0; i < n; i++) {
        uint32_t x = t8 ? (uint32_t)((i & 7) | ((i >> 1) & 8)) : (uint32_t)(i & 7);
        uint32_t y = t8 ? (uint32_t)(((i >> 4) & 14) | ((i >> 3) & 1)) : (uint32_t)(i >> 3);
        uint32_t wa = (cbp * 64u + gs_swz_ct32[(y & 31) << 6 | (x & 63)]) & 0xFFFFFu;
        int a = m[wa * 4 + 3];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
    }
    TC[h].key = key; TC[h].frame = gf_frame + 1; TC[h].amin = (uint8_t)lo; TC[h].amax = (uint8_t)hi;
    *amin = lo; *amax = hi;
    R2S.textures++;
}

/* how many alphas in [lo, hi] pass ATST (as gpu_front.c atst_count) */
static int atst_n(int atst, int ref, int lo, int hi) {
    int a, b, in = ref >= lo && ref <= hi;
    switch (atst) {
    case 0: return 0;
    case 1: return hi - lo + 1;
    case 2: a = lo; b = hi < ref - 1 ? hi : ref - 1; break;
    case 3: a = lo; b = hi < ref ? hi : ref; break;
    case 4: return in;
    case 5: a = lo > ref ? lo : ref; b = hi; break;
    case 6: a = lo > ref + 1 ? lo : ref + 1; b = hi; break;
    default: return hi - lo + 1 - in;
    }
    return b >= a ? b - a + 1 : 0;
}

enum { BL_OFF, BL_NOCOLOR, BL_ADD, BL_REVSUB, BL_DUAL };

/* The draws one pass of one sub-batch needs (0..2: the alpha test may split it into the fragments that pass and those
 * that fail, drawn with different Z / colour writes): their state (blend class | depth << 4 | wmask << 8 | cull << 12)
 * and discard rule (0: fragments failing the alpha test are discarded, 1: passing ones). The static classes come from
 * the decoder (r2_pass_classify), the dynamic ones from the fade, the frame's masks and the CLUT's alpha range. */
static int pass_items(const r2_batch* B, const r2_pass* P, float fade, int zmsk, int wmask_fb, uint32_t st[2], uint8_t rule[2]) {
    if (!B->icount) return 0;
    if (P->bl == 0xFF) { R2S.unsupported++; return 0; }
    uint64_t test = P->test, tex0 = P->tex0;
    int ate = (int)(test & 1), atst = (int)((test >> 1) & 7), aref = (int)((test >> 4) & 0xFF), afail = (int)((test >> 12) & 3);
    int zte = (int)((test >> 16) & 1), ztst = zte ? (int)((test >> 17) & 3) : 1;
    if (ztst == 0) return 0;                                     /* Z NEVER: nothing */
    if ((test >> 14) & 1) { R2S.unsupported++; return 0; }       /* DATE (never in prelit materials) */
    int bl = P->bl, wm = wmask_fb;
    if (P->nocolor) wm &= 8;
    /* source alpha range: vertex alpha (fade on colour A) x texture alpha (TFX MODULATE / TCC) */
    int csel = P->csel, tamin = 0, tamax = 0;
    tex_ready(tex0, &tamin, &tamax);
    int va_lo = B->amin[csel], va_hi = B->amax[csel];
    if (!csel && fade < 1.0f) { va_lo = (int)((float)va_lo * fade); va_hi = (int)((float)va_hi * fade); }
    int tfx = (int)((tex0 >> 35) & 3), tcc = (int)((tex0 >> 34) & 1);
    int as_lo = va_lo, as_hi = va_hi;
    if (tcc) {
        if (tfx == 0) { as_lo = (va_lo * tamin) >> 7; as_hi = (va_hi * tamax) >> 7; }
        else if (tfx == 2) { as_lo = tamin + va_lo; as_hi = tamax + va_hi; }
        else { as_lo = tamin; as_hi = tamax; }
        if (as_lo > 255) as_lo = 255;
        if (as_hi > 255) as_hi = 255;
    }
    if (bl == BL_DUAL && ((P->alpha >> 4) & 3) == 0 && as_hi > 128) R2S.over128++;  /* (the exact path: ROV tier) */
    /* the alpha test: halves */
    int nh = 1, zw_ok[2] = { 1, 1 }, wm_h[2] = { wm, wm }, disc = 0;
    uint8_t ru[2] = { 0, 1 };
    if (ate) {
        int n = atst_n(atst, aref, as_lo, as_hi), all = as_hi - as_lo + 1;
        int outcome = atst == 1 || n == all ? 1 : n == 0 ? 2 : 4;   /* all pass / all fail / straddle */
        disc = outcome == 4;                                     /* (only then can a fragment be discarded) */
        if (outcome == 2 || outcome == 4) {
            /* AFAIL of the failing fragments: KEEP nothing, FB_ONLY no Z, ZB_ONLY no colour, RGB_ONLY no Z and no alpha */
            int fz = afail == 2, fwm = afail == 2 ? 0 : afail == 3 ? (wm & 7) : wm;
            if (outcome == 2) {
                if (afail == 0) return 0;
                zw_ok[0] = fz; wm_h[0] = fwm; ru[0] = 1;         /* every fragment fails: none is discarded */
                R2S.all_fail++;
            } else if (afail != 0) {
                nh = 2;                                          /* (afail KEEP: one draw, failing fragments discarded) */
                zw_ok[1] = fz; wm_h[1] = fwm;
                R2S.splits++;
            }
        }
    }
    int k = 0;
    for (int hv = 0; hv < nh; hv++) {
        int zw = !zmsk && zw_ok[hv];
        if (!wm_h[hv] && !zw) continue;
        int depth = ztst * 2 + zw;                               /* (gpu_exec.c depth classes: ztst * 2 + zwrite) */
        st[k] = (uint32_t)bl | (uint32_t)depth << 4 | (uint32_t)wm_h[hv] << 8 | (uint32_t)(B->cull ? 1 : 0) << 12 |
                (uint32_t)disc << 14;            /* (bit 13: clip, set per entry) */
        rule[k] = ru[hv];
        if (zw) R2S.zwrite_draws++;
        k++;
    }
    return k;
}

/* ---- the segment's draws: consolidated per (entry, pass, state, rule) over the entry's sub-batches ----------------- */
static int consolidate = -1;             /* PS2_R2_CONSOLIDATE=1: per entry; default one draw per sub-batch, pass and half
                                            in the VU's order (r2-m1's) */
static uint32_t* M_words;                /* the segment's batch masks */
static uint32_t nmask, capmask;
typedef struct { uint32_t state; uint8_t rule, pass; uint32_t bmin, bmax, nset, mw; } grp;

static gpu_r2_draw* new_draw(uint32_t* nd) {
    if (!grow((void**)&R2S_draws, &R2S_capdraws, *nd + 1, sizeof(gpu_r2_draw))) return NULL;
    gpu_r2_draw* d = &R2S_draws[(*nd)++];
    memset(d, 0, sizeof *d);
    R2S.draws++;
    return d;
}

static void emit_entry(const r2_mesh* m, uint32_t ent, float fade, int zmsk, int wm, uint32_t clipbit, const uint32_t* rt, uint32_t* nd) {
    uint32_t st[2];
    uint8_t ru[2];
    if (!consolidate || rt) {            /* batch-major, as the VU emits them */
        const uint32_t* r = rt ? rt + 2 : NULL;   /* (r2-m5b) per sub-batch: nruns, start | count << 16 (triangles) */
        if (rt && (rt[1] != m->nb || rt[0] < 2 + m->nb)) { R2S.dropped++; return; }
        for (uint32_t b = 0; b < m->nb; b++) {
            uint32_t nr = r ? *r++ : 1;
            for (uint32_t p = 0; p < m->b[b].npass; p++) {
                int n = pass_items(&m->b[b], &m->b[b].pass[p], fade, zmsk, wm, st, ru);
                for (int k = 0; k < n; k++)
                    for (uint32_t q = 0; q < nr; q++) {
                        uint32_t s0 = r ? r[q] & 0xFFFF : 0, cnt = r ? 3 * (r[q] >> 16) : m->b[b].icount;
                        if (3 * s0 + cnt > m->b[b].icount) { R2S.dropped++; continue; }
                        gpu_r2_draw* d = new_draw(nd);
                        if (!d) return;
                        d->entry = ent; d->mode = p | (uint32_t)ru[k] << 2 | b << 8; d->mask_off = ~0u; d->mesh = m->handle;
                        d->first = m->b[b].ifirst + 3 * s0; d->count = cnt; d->state = st[k] | clipbit;
                    }
            }
            if (r) r += nr;
        }
        return;
    }
    grp g[16];
    uint32_t mw = (m->nb + 31) / 32;
    for (uint32_t p = 0; p < 3; p++) {
        uint32_t ng = 0;
        for (uint32_t b = 0; b < m->nb; b++) {
            if (p >= m->b[b].npass) continue;
            int n = pass_items(&m->b[b], &m->b[b].pass[p], fade, zmsk, wm, st, ru);
            for (int k = 0; k < n; k++) {
                uint32_t gi = 0;
                while (gi < ng && (g[gi].state != st[k] || g[gi].rule != ru[k])) gi++;
                if (gi == ng) {
                    if (ng == 16 || !grow((void**)&M_words, &capmask, nmask + mw, 4)) { R2S.dropped++; continue; }
                    g[ng] = (grp){ st[k], ru[k], (uint8_t)p, b, b, 0, nmask };
                    memset(M_words + nmask, 0, mw * 4);
                    nmask += mw;
                    ng++;
                }
                M_words[g[gi].mw + b / 32] |= 1u << (b & 31);
                g[gi].bmax = b;
                g[gi].nset++;
            }
        }
        for (uint32_t gi = 0; gi < ng; gi++) {
            gpu_r2_draw* d = new_draw(nd);
            if (!d) return;
            uint32_t b0 = g[gi].bmin, b1 = g[gi].bmax;
            d->entry = ent;
            d->mode = p | (uint32_t)g[gi].rule << 2 | b0 << 8;
            /* every batch in [b0, b1] drawn: no mask; else the words realigned so that bit 0 is batch b0 */
            d->mask_off = ~0u;
            if (g[gi].nset != b1 - b0 + 1) {
                uint32_t* w = M_words + g[gi].mw;
                uint32_t s = b0 & 31, j0 = b0 / 32;
                for (uint32_t i = 0; j0 + i < mw; i++) {
                    uint32_t j = j0 + i;
                    w[i] = s ? (w[j] >> s) | (j + 1 < mw ? w[j + 1] << (32 - s) : 0) : w[j];
                }
                d->mask_off = g[gi].mw;          /* (made absolute when the segment's masks are placed) */
            }
            d->mesh = m->handle;
            d->first = m->b[b0].ifirst;
            d->count = m->b[b1].ifirst + m->b[b1].icount - m->b[b0].ifirst;
            d->state = g[gi].state | clipbit;
            R2S.items += g[gi].nset;
        }
    }
}

/* ---- (r8-perf) the segment's draws as fewer GPU draws --------------------------------------------------------------
 * Most of R2's GPU time in a hub was the draws themselves (ep1: ~5500 a frame of 1-6 triangles each - grass, leaves -
 * three states alternating per sub-batch: pass A's alpha-test halves and pass B), not their pixels. Two draws that touch
 * no common pixel commute exactly (every per-pixel effect - depth test and write, blend, alpha test - stays the same), so
 * the draws are reordered within that freedom only: each draw's screen box (its sub-batch's object-space box through
 * the entry's W x C and the frame's screen model, 2 pixels of margin; a skinned sub-batch or a corner behind the camera:
 * the whole scissor) is entered into a tile grid, and draw j gets the level
 *     L(j) = max over earlier draws k sharing a tile with j of L(k) + (rank(k) > rank(j))
 * (rank: the order in which the segment's states first appear). Executing by (level, rank), the game's order within each,
 * keeps every overlapping pair in order: L(k) < L(j), or equal levels with rank(k) <= rank(j). Runs of one (level, rank)
 * become instanced draws of up to n small draws (shaders/r2.hlsl R2_PULL: the vertices pulled by index, padding
 * triangles rejected; the waste is bounded: n x max count <= 2 x the indices + 96). Large draws stay plain. */
#define PL_TS pl_ts                      /* tile size, GS pixels (PS2_R2_PLAN_TS, default 16) */
#define PL_TMAX 128                      /* tiles per axis */
#define PL_R 16                          /* distinct states per segment */
#define PL_SMALL 384u                    /* indices: larger draws are not merged */
#define PL_MESHES (4u * 16384u)          /* (r2_mesh.c: 4 generations x MESH_MAX) */
typedef struct { uint32_t handle, nb; float* bb; } pl_mbox;   /* per sub-batch: min xyz, max xyz; min x > max x: no triangle */
static pl_mbox* PLB;
static int16_t (*pl_lev)[PL_R];
static uint32_t* pl_stamp;
static uint32_t pl_seg;
static uint64_t* pl_key;
static gpu_r2_chunk* pl_ch;
static uint32_t* pl_mem;
static uint32_t pl_capkey, pl_capch, pl_capmem;
static uint64_t pl_stat_draws, pl_stat_chunks, pl_stat_merged, pl_stat_levels, pl_stat_segs;
static int merge_env = -1;               /* PS2_R2_MERGE=0: no plan (the render thread draws every record) */
static int pl_ts = 16, pl_margin = 2;    /* (PS2_R2_PLAN_TS, PS2_R2_PLAN_MARGIN: experiments) */

static const float* pl_batch_box(const r2_mesh* m, uint32_t b) {
    if (!PLB && !(PLB = calloc(PL_MESHES, sizeof *PLB))) return NULL;
    uint32_t k = ((m->handle >> 20) & 3u) * 16384u + (m->handle & 0x3FFFu);
    pl_mbox* e = &PLB[k];
    if (e->handle != m->handle || e->nb != m->nb || !e->bb) {
        free(e->bb);
        e->bb = malloc((size_t)m->nb * 6 * sizeof(float));
        e->handle = m->handle;
        e->nb = m->nb;
        if (!e->bb) { e->handle = 0; return NULL; }
        for (uint32_t i = 0; i < m->nb; i++) {
            float* q = e->bb + i * 6;
            q[0] = q[1] = q[2] = 1e30f; q[3] = q[4] = q[5] = -1e30f;
            const r2_batch* B = &m->b[i];
            for (uint32_t j = B->ifirst; j < B->ifirst + B->icount && j < m->nidx; j++) {
                uint32_t v = m->idx[j];
                if (v >= m->nvtx) { q[0] = -1e30f; q[3] = 1e30f; break; }   /* (an index the box cannot bound: everything) */
                const r2_vtx* p = &m->vtx[v];
                if (p->x < q[0]) q[0] = p->x;
                if (p->y < q[1]) q[1] = p->y;
                if (p->z < q[2]) q[2] = p->z;
                if (p->x > q[3]) q[3] = p->x;
                if (p->y > q[4]) q[4] = p->y;
                if (p->z > q[5]) q[5] = p->z;
            }
        }
    }
    return b < e->nb ? e->bb + b * 6 : NULL;
}

/* the tiles draw d may touch: 1 (t[0..3] inclusive), 0 none, -1 all */
static int pl_tiles(const gpu_r2_draw* d, const r2_mesh* m, float xo, float yo, int sx0, int sy0, int sx1, int sy1, int t[4]) {
    uint32_t b = d->mode >> 8;
    if (b >= m->nb || m->b[b].skin || (d->state >> 13) & 1 || d->mask_off != ~0u) return -1;
    const float* q = pl_batch_box(m, b);
    if (!q || q[0] < -1e29f || q[3] > 1e29f) return -1;
    if (q[0] > q[3]) return 0;
    const float* M = ents[d->entry].m;
    float gx0 = 1e30f, gy0 = 1e30f, gx1 = -1e30f, gy1 = -1e30f;
    int bad = 0;
    for (int c = 0; c < 8 && !bad; c++) {
        float x = q[(c & 1) ? 3 : 0], y = q[(c & 2) ? 4 : 1], z = q[(c & 4) ? 5 : 2];
        float cx = x * M[0] + y * M[4] + z * M[8] + M[12];
        float cy = x * M[1] + y * M[5] + z * M[9] + M[13];
        float cw = x * M[3] + y * M[7] + z * M[11] + M[15];
        if (!(cw > 1e-6f)) { bad = 1; break; }
        float X = (SM.bx + SM.ax * (cx / cw) - xo) * (1.0f / 16.0f), Y = (SM.by + SM.ay * (cy / cw) - yo) * (1.0f / 16.0f);
        if (!(X > -1e7f && X < 1e7f && Y > -1e7f && Y < 1e7f)) { bad = 1; break; }
        if (X < gx0) gx0 = X;
        if (X > gx1) gx1 = X;
        if (Y < gy0) gy0 = Y;
        if (Y > gy1) gy1 = Y;
    }
    if (bad) {                           /* (the box reaches behind the camera: the draw's own vertices, when few) */
        if (d->first + d->count > m->nidx || d->count > 1536u) return -1;
        gx0 = gy0 = 1e30f; gx1 = gy1 = -1e30f;
        for (uint32_t j = d->first; j < d->first + d->count; j++) {
            uint32_t v = m->idx[j];
            if (v >= m->nvtx) return -1;
            const r2_vtx* p = &m->vtx[v];
            float cx = p->x * M[0] + p->y * M[4] + p->z * M[8] + M[12];
            float cy = p->x * M[1] + p->y * M[5] + p->z * M[9] + M[13];
            float cw = p->x * M[3] + p->y * M[7] + p->z * M[11] + M[15];
            if (!(cw > 1e-6f)) return -1;
            float X = (SM.bx + SM.ax * (cx / cw) - xo) * (1.0f / 16.0f), Y = (SM.by + SM.ay * (cy / cw) - yo) * (1.0f / 16.0f);
            if (!(X > -1e7f && X < 1e7f && Y > -1e7f && Y < 1e7f)) return -1;
            if (X < gx0) gx0 = X;
            if (X > gx1) gx1 = X;
            if (Y < gy0) gy0 = Y;
            if (Y > gy1) gy1 = Y;
        }
        if (gx0 > gx1) return 0;
    }
    int x0 = (int)floorf(gx0) - pl_margin, y0 = (int)floorf(gy0) - pl_margin, x1 = (int)ceilf(gx1) + pl_margin,
        y1 = (int)ceilf(gy1) + pl_margin;
    if (x0 < sx0) x0 = sx0;
    if (y0 < sy0) y0 = sy0;
    if (x1 > sx1) x1 = sx1;
    if (y1 > sy1) y1 = sy1;
    if (x0 > x1 || y0 > y1) return 0;
    t[0] = (x0 - sx0) / PL_TS; t[1] = (y0 - sy0) / PL_TS; t[2] = (x1 - sx0) / PL_TS; t[3] = (y1 - sy0) / PL_TS;
    return 1;
}

static int pl_cmp(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

/* the plan of the closing segment's nd draws (records at draw_off); 0: none (the render thread draws every record) */
static uint32_t r2_plan(uint32_t nd, uint32_t draw_off, int sx0, int sy0, int sx1, int sy1, float xo, float yo, uint32_t* nplan) {
    *nplan = 0;
    if (merge_env < 0) {
        const char* e = getenv("PS2_R2_MERGE"); merge_env = !(e && *e == '0');
        e = getenv("PS2_R2_PLAN_TS"); if (e && atoi(e) >= 4 && atoi(e) <= 64) pl_ts = atoi(e);
        e = getenv("PS2_R2_PLAN_MARGIN"); if (e && atoi(e) >= 1 && atoi(e) <= 16) pl_margin = atoi(e);
    }
    if (!merge_env || consolidate || nd < 2) return 0;
    int ntx = (sx1 - sx0) / PL_TS + 1, nty = (sy1 - sy0) / PL_TS + 1;
    if (ntx > PL_TMAX || nty > PL_TMAX) return 0;
    if (!pl_lev && (!(pl_lev = calloc(PL_TMAX * PL_TMAX, sizeof *pl_lev)) || !(pl_stamp = calloc(PL_TMAX * PL_TMAX, 4)))) return 0;
    if (!grow((void**)&pl_key, &pl_capkey, nd, 8) || !grow((void**)&pl_ch, &pl_capch, nd, sizeof *pl_ch) ||
        !grow((void**)&pl_mem, &pl_capmem, nd + 8 * nd, 4))
        return 0;
    pl_seg++;
    uint32_t st_of[PL_R];
    int nr = 0;
    int floor_[PL_R], gmax[PL_R];
    for (int s = 0; s < PL_R; s++) floor_[s] = gmax[s] = -1;
    const gpu_r2_draw* D = R2S_draws;
    int maxl = 0;
    for (uint32_t i = 0; i < nd; i++) {
        const gpu_r2_draw* d = &D[i];
        int r = 0;
        while (r < nr && st_of[r] != d->state) r++;
        if (r == nr) { if (nr == PL_R) return 0; st_of[nr++] = d->state; }
        const r2_mesh* m = r2_mesh_get(d->mesh);
        if (!m) return 0;
        int t[4];
        int k = pl_tiles(d, m, xo, yo, sx0, sy0, sx1, sy1, t);
        int L = 0;
        if (k < 0) {                     /* (everything: against every earlier draw) */
            for (int s = 0; s < nr; s++) if (gmax[s] >= 0 && gmax[s] + (s > r) > L) L = gmax[s] + (s > r);
            if (L > floor_[r]) floor_[r] = L;
        } else if (k > 0) {
            for (int y = t[1]; y <= t[3]; y++)
                for (int x = t[0]; x <= t[2]; x++) {
                    uint32_t ti = (uint32_t)(y * PL_TMAX + x);
                    if (pl_stamp[ti] != pl_seg) { pl_stamp[ti] = pl_seg; for (int s = 0; s < PL_R; s++) pl_lev[ti][s] = -1; }
                    for (int s = 0; s < nr; s++) {
                        int v = pl_lev[ti][s] > floor_[s] ? pl_lev[ti][s] : floor_[s];
                        if (v >= 0 && v + (s > r) > L) L = v + (s > r);
                    }
                }
            for (int y = t[1]; y <= t[3]; y++)
                for (int x = t[0]; x <= t[2]; x++) {
                    uint32_t ti = (uint32_t)(y * PL_TMAX + x);
                    if (pl_lev[ti][r] < L) pl_lev[ti][r] = (int16_t)L;
                }
        }
        if (L > 32000) return 0;
        if (L > gmax[r]) gmax[r] = L;
        if (L > maxl) maxl = L;
        pl_key[i] = (uint64_t)((uint32_t)L * PL_R + (uint32_t)r) << 32 | i;
    }
    qsort(pl_key, nd, 8, pl_cmp);
    /* chunks: runs of one key; small draws merged with bounded padding */
    uint32_t nc = 0, nm = 0, mem0 = 0;
    uint32_t cn = 0, cmax = 0, csum = 0, cst = 0;
    uint64_t ckey = ~0ull;
    uint32_t base = draw_off / 32u;
#define PL_FLUSH() do { \
        if (cn == 1) { pl_ch[nc++] = (gpu_r2_chunk){ pl_mem[mem0] - base, 0, 0, cst }; nm = mem0; } \
        else if (cn > 1) { pl_ch[nc++] = (gpu_r2_chunk){ mem0, cn, cmax, cst }; nm = (mem0 + cn + 7) & ~7u; pl_stat_merged += cn; } \
        cn = 0; cmax = 0; csum = 0; mem0 = nm; } while (0)
    for (uint32_t j = 0; j < nd; j++) {
        uint32_t i = (uint32_t)pl_key[j];
        uint64_t key = pl_key[j] >> 32;
        const gpu_r2_draw* d = &D[i];
        uint32_t c = d->count;
        if (key != ckey) { PL_FLUSH(); ckey = key; }
        if (c > PL_SMALL || c % 3u) {
            PL_FLUSH();
            pl_ch[nc++] = (gpu_r2_chunk){ i, 0, 0, d->state };
            continue;
        }
        uint32_t nmx = c > cmax ? c : cmax;
        if (cn && (cn + 1) * nmx > 2u * (csum + c) + 96u) PL_FLUSH();
        if (!cn) { cst = d->state; mem0 = nm; }
        pl_mem[mem0 + cn] = base + i;
        cn++;
        csum += c;
        if (c > cmax) cmax = c;
    }
    PL_FLUSH();
#undef PL_FLUSH
    {                                    /* (diagnostics: PS2_R2_PLAN_LOG=<n> segments described once) */
        static int plog = -1;
        if (plog < 0) { const char* e = getenv("PS2_R2_PLAN_LOG"); plog = e ? atoi(e) : 0; }
        if (plog > 0 && gf_frame > 3000) {
            plog--;
            uint32_t nall = 0, nsmall = 0, nbig = 0, nskin = 0;
            for (uint32_t i = 0; i < nd; i++) {
                int t[4];
                const r2_mesh* m = r2_mesh_get(D[i].mesh);
                int k = m ? pl_tiles(&D[i], m, xo, yo, sx0, sy0, sx1, sy1, t) : -1;
                if (m && (D[i].mode >> 8) < m->nb && m->b[D[i].mode >> 8].skin) nskin++;
                if (k < 0) nall++;
                else if (k > 0 && (t[2] - t[0] + 1) * (t[3] - t[1] + 1) > 64) nbig++;
                else nsmall++;
            }
            fprintf(stderr, "[r2] plan: segment of %u draws (%d states, %d levels): %u GPU draws; boxes: %u everything (%u skinned), %u > 64 tiles, %u small\n",
                    nd, nr, maxl + 1, nc, nall, nskin, nbig, nsmall);
        }
    }
    pl_stat_draws += nd;
    pl_stat_chunks += nc;
    pl_stat_levels += (uint64_t)maxl + 1;
    pl_stat_segs++;
    if (nc >= nd) return 0;
    /* the member lists (32-byte aligned per merged chunk), then the chunk records */
    uint32_t mo = 0;
    if (nm) {
        uint32_t raw = gf_data(NULL, nm * 4u + 32u);
        mo = (raw + 31u) & ~31u;
        memcpy(gf_data_ptr(mo), pl_mem, nm * 4u);
        for (uint32_t c = 0; c < nc; c++)
            if (pl_ch[c].n) pl_ch[c].first = (mo + pl_ch[c].first * 4u) / 32u;
    }
    *nplan = nc;
    return gf_data(pl_ch, nc * (uint32_t)sizeof *pl_ch);
}

/* the open segment becomes a GPU_FOP_R2 */
static void gf_r2_flush_impl(void);
void gf_r2_flush(void) {
    if (!gf_r2_pending) return;
    double t0 = qms();
    gf_r2_flush_impl();
    fr_ms += qms() - t0;
}

static void gf_r2_flush_impl(void) {
    gf_r2_pending = 0;
    if (!nitems) return;
    gf_flush_open();
    uint64_t frame = seg_frame, zbuf = seg_zbuf, sc = seg_sc;
    uint32_t fpsm = (uint32_t)((frame >> 24) & 0x3F), fpage = (uint32_t)(frame & 0x1FF), fbw = (uint32_t)((frame >> 16) & 0x3F);
    uint32_t fbmsk = (uint32_t)(frame >> 32);
    uint32_t zpsm = 0x30 | (uint32_t)((zbuf >> 24) & 0xF), zpage = (uint32_t)(zbuf & 0x1FF);
    int zmsk = (int)((zbuf >> 32) & 1);
    int x0 = (int)(sc & 0x7FF), x1 = (int)((sc >> 16) & 0x7FF), y0 = (int)((sc >> 32) & 0x7FF), y1 = (int)((sc >> 48) & 0x7FF);
    gpu_target* ct = NULL;
    gpu_target* zt = NULL;
    if ((fpsm == PSMCT32 || fpsm == PSMCT24) && fbw) ct = own_target(TGT_C32, PSMCT32, fpage, fbw, y1 + 1);
    if (fbw && (zpsm == PSMZ32 || zpsm == PSMZ24 || zpsm == PSMZ16 || zpsm == PSMZ16S))
        zt = own_target(own_target_kind_of_zpsm((int)zpsm), (int)zpsm, zpage, fbw, y1 + 1);
    if (!ct || !zt || x1 < x0 || y1 < y0) { R2S.dropped += nitems; nitems = nents = 0; return; }
    if (ct->kx != zt->kx || ct->ky != zt->ky) own_pair_family(ct, zt);
    /* byte write mask of FRAME (R1 G2 B4 A8); a CT24 frame keeps the top byte */
    int wm = 0;
    uint32_t fm = fpsm == PSMCT24 ? (fbmsk | 0xFF000000u) : fbmsk;
    for (int b = 0; b < 4; b++) {
        uint32_t mb = (fm >> (8 * b)) & 0xFF;
        if (mb != 0 && mb != 0xFF) R2S.unsupported++;
        if (mb != 0xFF) wm |= 1 << b;
    }
    own_draw(ct, x0, y0, x1, y1, 1, 0);
    own_draw(zt, x0, y0, x1, y1, !zmsk, 0);
    ct->last_used = gf_frame;
    zt->last_used = gf_frame;
    /* the draws, in the game's order of entries */
    /* (PS2_R2_CONSOLIDATE=1: opt-in. Drawing an entry's pass A before its pass B changes foliage edges where a later
     * batch's translucent pass-A pixels cover an earlier batch's pass B: ep2 hub 0.06% of the pixels > 16, over the A/B
     * gate; and the draw count was not R2's GPU cost. The default keeps the VU's order.) */
    if (consolidate < 0) { const char* e = getenv("PS2_R2_CONSOLIDATE"); consolidate = e && *e == '1'; }
    uint32_t nd = 0;
    nmask = 0;
    for (uint32_t i = 0; i < nitems; i++) {
        const r2_mesh* m = r2_mesh_get(items[i].handle);
        if (!m) { R2S.dropped++; continue; }
        uint32_t ro = ent_rt[items[i].ent];   /* (r2-m5b) a run table: the strip triangles through the plain pipeline */
        emit_entry(m, items[i].ent, ents[items[i].ent].fade, zmsk, wm, !ro && (ents[items[i].ent].flags & R2E_CLIP) ? 1u << 13 : 0u,
                   ro ? RT + ro - 1 : NULL, &nd);
    }
    gpu_fop_r2* o = gf_cmd(GPU_FOP_R2, sizeof *o);
    o->ct = ct->id;
    o->zt = zt->id;
    o->sc[0] = (int16_t)x0; o->sc[1] = (int16_t)y0; o->sc[2] = (int16_t)x1; o->sc[3] = (int16_t)y1;
    o->bbox[0] = (int16_t)x0; o->bbox[1] = (int16_t)y0; o->bbox[2] = (int16_t)x1; o->bbox[3] = (int16_t)y1;
    o->xyoffset = (uint32_t)(seg_xyo & 0xFFFF) | (uint32_t)((seg_xyo >> 32) & 0xFFFF) << 16;
    o->flags = (seg_fba & 1 ? GPU_R2F_FBA : 0) | (seg_cc & 1 ? GPU_R2F_COLCLAMP : 0) | (zmsk ? GPU_R2F_ZMSK : 0) | (uint32_t)wm << GPU_R2F_WMASK_SHIFT;
    gpu_r2_frame fr;
    memset(&fr, 0, sizeof fr);
    memcpy(fr.c0, FB.c0, 16);
    memcpy(fr.c1, FB.c1, 16);
    fr.ax = SM.ax; fr.bx = SM.bx; fr.ay = SM.ay; fr.by = SM.by; fr.az = SM.az; fr.bz = SM.bz;
    fr.serial = fb_serial;
    o->frame_off = gf_data(&fr, sizeof fr);
    o->ent_off = gf_data(ents, nents * (uint32_t)sizeof *ents);
    if (nmask) {
        uint32_t mo = gf_data(M_words, nmask * 4) / 4;
        for (uint32_t i = 0; i < nd; i++)
            if (R2S_draws[i].mask_off != ~0u) R2S_draws[i].mask_off += mo;
    }
    /* (r2-m2) the segment's constants as a record in fdata, found by every draw record (no per-segment cbuffer) */
    gpu_r2_seg sg = { (float)ct->kx, (float)ct->ky, (float)(seg_xyo & 0xFFFF), (float)((seg_xyo >> 32) & 0xFFFF),
                      o->frame_off, o->ent_off, (seg_fba & 1) ? 1u : 0u, (uint32_t)y0 };
    uint32_t so = gf_data(&sg, sizeof sg);
    for (uint32_t i = 0; i < nd; i++) R2S_draws[i].seg_off = so;
    /* the draw records 32-byte aligned: the vertex shader finds record i at (StartInstanceLocation = draw_off / 32 + i) x 32 */
    uint32_t base = gf_data(NULL, nd * (uint32_t)sizeof(gpu_r2_draw) + 16);
    o->draw_off = (base + 31) & ~31u;
    memcpy(gf_data_ptr(o->draw_off), R2S_draws, nd * sizeof(gpu_r2_draw));
    o->plan_off = r2_plan(nd, o->draw_off, x0, y0, x1, y1, (float)(seg_xyo & 0xFFFF), (float)((seg_xyo >> 32) & 0xFFFF), &o->nplan);
    o->nent = nents;
    o->ndraw = nd;
    o->gen = items[0].handle >> 20;
    gf.d3d_draws += nd;
    R2S.segments++;
    R2S.entries += nitems;
    nitems = nents = 0;
}

/* the open segment for the current environment (a new one when it changed) */
static void seg_env(const gs_env_view* ev) {
    const uint64_t* r = ev->reg;
    uint64_t frame = r[0x4C], zbuf = r[0x4E], sc = r[0x40], xyo = r[0x18], fba = r[0x4A], cc = r[0x46];
    if (gf_r2_pending && (frame != seg_frame || zbuf != seg_zbuf || sc != seg_sc || xyo != seg_xyo || fba != seg_fba || cc != seg_cc))
        gf_r2_flush();
    if (!gf_r2_pending) {
        gf_r2_pending = 1;
        seg_frame = frame; seg_zbuf = zbuf; seg_sc = sc; seg_xyo = xyo; seg_fba = fba; seg_cc = cc;
        nitems = nents = 0;
    }
}

/* one native entry into the open segment; m = world x clip */
static int entry_add(const gs_env_view* ev, uint32_t handle, const float m[16], float fade, uint32_t eflags, const void* xq, uint32_t xn,
                     const uint32_t* rt, const float* prev) {
    if (!fb_ok || !r2_mesh_get(handle)) { R2S.dropped++; return 0; }
    seg_env(ev);
    if (!grow((void**)&items, &capitems, nitems + 1, sizeof *items) || !grow((void**)&ents, &capents, nents + 1, sizeof *ents) ||
        !grow((void**)&ent_rt, &capert, nents + 1, 4)) return 0;
    if (!nents) nrt = 0;
    ent_rt[nents] = 0;
    if (rt && rt[0] >= 2 && rt[0] <= 8192 && grow((void**)&RT, &caprt, nrt + rt[0], 4)) {   /* (r2-m5b) */
        memcpy(RT + nrt, rt, rt[0] * 4);
        ent_rt[nents] = nrt + 1;
        nrt += rt[0];
    }
    gpu_r2_entry* e = &ents[nents];
    memcpy(e->m, m, sizeof e->m);
    memcpy(e->prev, prev ? prev : m, sizeof e->prev);   /* (r2-m8: motion vectors) */
    e->fade = fade;
    e->flags = eflags;
    e->pad[0] = e->pad[1] = 0;
    if (xn) e->pad[0] = gf_data(xq, xn * 16) / 16 + 1;   /* (r2-m3) the extra qwords: fdata qword index + 1 */
    items[nitems].handle = handle;
    items[nitems].ent = nents;
    nitems++;
    nents++;
    return 1;
}

/* (bracket mode) an entry's inline words are complete */
static int entry_done(const gs_env_view* ev) {
    uint32_t nw = (RX.flags & R2D_FADE ? 1u : 0u) + (RX.flags & R2D_IDENTITY ? 0u : 16u);
    if (!r2_mesh_get(RX.handle) && r2_mesh_resolve)
        r2_mesh_resolve(RX.handle, RX.w[nw] & 0x7FFFFFFFu, (int)(RX.w[nw] >> 31));   /* (offline replays) */
    uint32_t k = 0;
    float fade = 1.0f, m[16];
    static int noclip = -1;              /* (r2-m2, A/B diagnostics) PS2_R2_AB_NOCLIP=1: clipped entries stay exact */
    if (noclip < 0) { const char* e = getenv("PS2_R2_AB_NOCLIP"); noclip = e && *e == '1'; }
    if (noclip && (RX.flags & R2D_CLIP)) return 0;
    static int allclip = -1;             /* (diagnostics) PS2_R2_AB_ALLCLIP=1: every entry through the clip pipeline */
    if (allclip < 0) { const char* e = getenv("PS2_R2_AB_ALLCLIP"); allclip = e && *e == '1'; }
    if (allclip) RX.flags |= R2D_CLIP;
    if (RX.flags & R2D_FADE) memcpy(&fade, &RX.w[k++], 4);
    if (RX.flags & R2D_IDENTITY) memcpy(m, FB.clip, sizeof m);
    else {
        float w[16];
        memcpy(w, &RX.w[k], sizeof w);
        r2_matmul(w, FB.clip, m);
    }
    static int mc = -1;                  /* (diagnostics) PS2_R2_AB_MODELCHECK=n: the screen model vs the VU-exact vertex */
    if (mc < 0) { const char* e = getenv("PS2_R2_AB_MODELCHECK"); mc = e ? atoi(e) : 0; }
    const r2_mesh* me = r2_mesh_get(RX.handle);
    if (mc > 0 && me && fb_ok) {
        mc--;
        for (uint32_t i = 0; i < me->nvtx && i < 4; i++) {
            const r2_vtx* v = &me->vtx[i];
            r2_gsv g;
            r2_vertex_eval(v, m, &FB, &me->b[0], 0, fade, &g);
            float c[4];
            for (int j = 0; j < 4; j++) c[j] = v->x * m[j] + v->y * m[4 + j] + v->z * m[8 + j] + m[12 + j];
            fprintf(stderr, "[r2] model check: exact X %d Y %d Z %u | model X %.2f Y %.2f Z %.2f (w %.3f)\n", g.x, g.y, g.z,
                    SM.bx + SM.ax * c[0] / c[3], SM.by + SM.ay * c[1] / c[3], SM.bz + SM.az * c[2] / c[3], c[3]);
        }
    }
    uint32_t xn = 0, xo = 0;
    if (RX.flags & R2D_EXTRA) {                       /* (r2-m3) the frame's extra block it names */
        uint32_t id = RX.w[nw + 1];
        if (id >= xq_have) { R2S.bad_hints++; return 0; }
        xn = xq_n[id]; xo = xq_off[id];
    }
    return entry_add(ev, RX.handle, m, fade, RX.flags & R2D_CLIP ? R2E_CLIP : 0, XQW + xo, xn, NULL, NULL);
}

/* (r2-m2, hybrid) a run of native entries from the frame's scene slot */
static void run_hint(const gs_env_view* ev, uint32_t arg, uint32_t serial) {
    const r2_slot* S = r2_slot_at(arg & 15u);
    uint32_t run = arg >> 4;
    if (!S || __atomic_load_n(&S->serial, __ATOMIC_ACQUIRE) != serial || run >= S->nrun) { R2S.stale++; return; }
    if (fb_serial != serial || !fb_ok) {
        FB = S->fb;
        fb_ok = r2_screen_model_of(&FB, &SM);
        fb_serial = serial;
    }
    uint32_t first = S->run_first[run], n = S->run_count[run];
    if (first + n > S->nent) { R2S.stale++; return; }
    if (!n) return;
    seg_env(ev);
    uint32_t keep_items = nitems, keep_ents = nents;
    for (uint32_t i = 0; i < n; i++) {
        const r2_slot_ent* se = &S->ent[first + i];
        const uint32_t* rt = (se->flags & R2E_RUNS) && se->pad[0] && se->pad[0] <= R2_SLOT_XQ ? (const uint32_t*)S->xq[se->pad[0] - 1] : NULL;
        entry_add(ev, se->handle, se->m, se->fade, se->flags, se->xn ? S->xq[se->xoff] : NULL, se->xn, rt, se->prev);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&S->serial, __ATOMIC_RELAXED) != serial) {   /* reused while read (the EE 16 frames ahead) */
        R2S.stale++;
        nitems = keep_items;
        nents = keep_ents;
    }
    R2S.runs++;
}

static void fr_r2_hint(const gs_env_view* ev, uint64_t v) {
    uint32_t kind = (uint32_t)(v & 0xFF), arg = (uint32_t)((v >> 8) & 0xFFFFFF), value = (uint32_t)(v >> 32);
    R2S.hints++;
    if (kind == R2H_DATA) {
        if (RX.state == RX_XQ) {                     /* (r2-m3) an extra block's words */
            if (xq_nw >= sizeof XQW / 4) { R2S.bad_hints++; RX.state = RX_IDLE; return; }
            XQW[xq_nw++] = value;
            if (++RX.got == RX.need) { xq_have = xq_cur + 1; RX.state = RX_IDLE; }
            return;
        }
        if (RX.state == RX_IDLE || RX.got >= RX.need || RX.got >= sizeof RX.w / 4) { R2S.bad_hints++; return; }
        RX.w[RX.got++] = value;
        if (RX.got < RX.need) return;
        if (RX.state == RX_FRAME) {
            memcpy(&FB, RX.w, sizeof FB);
            fb_ok = r2_screen_model_of(&FB, &SM);
        } else if (!RX.bracket || ab_native) {
            int taken = entry_done(ev);
            /* (r2-m2) bracket A/B: the exact primitives are skipped only when R2 took the entry (a recording that starts
             * after its frame's R2H_FRAME block has no frame constants for the rest of that frame: drawn exactly) */
            if (RX.bracket) gf_r2_skip = ab_native && taken ? (RX.flags & R2D_FANS ? 2 : 1) : 0;   /* (r2-m5b: 2 keeps fans) */
        }
        RX.state = RX_IDLE;
        return;
    }
    if (RX.state != RX_IDLE && RX.got < RX.need) R2S.bad_hints++;      /* (a block cut short) */
    RX.state = RX_IDLE;
    switch (kind) {
    case R2H_FRAME:
        if (arg != R2_FRAME_WORDS) { R2S.bad_hints++; return; }
        RX.state = RX_FRAME; RX.need = arg; RX.got = 0;
        fb_serial = value;
        if (fx_env < 0) { const char* e = getenv("PS2_R2_FX"); fx_env = !(e && *e == '0'); }
        gf_r2_fx_ttl = fx_env ? 2 : 0;
        gf_r2_td_ttl = fx_env ? 4 : 0;
        break;
    case R2H_DRAW: case R2H_BEGIN:
        RX.bracket = kind == R2H_BEGIN;
        if (RX.bracket && !ab_native) gf_r2_fx_ttl = gf_r2_td_ttl = 0;
        RX.handle = value;
        RX.flags = arg;
        RX.need = (arg & R2D_FADE ? 1u : 0u) + (arg & R2D_IDENTITY ? 0u : 16u) + 1u + (arg & R2D_EXTRA ? 1u : 0u);   /* (+ the address word [+ the extra count]) */
        RX.got = 0;
        if (RX.bracket) { gf_r2_skip = 0; R2S.brackets++; }   /* (set once the entry's words are in: entry taken) */
        RX.state = RX_ENTRY;
        break;
    case R2H_END:
        gf_r2_skip = 0;
        break;
    case R2H_XQ:                                     /* (r2-m3) */
        if (arg == 0) { xq_nw = 0; xq_have = 0; }
        if (arg >= XQ_MAX || arg != xq_have || value > R2_LIGHT_QW + R2_HQ_MAX || xq_nw + 4u * value > sizeof XQW / 4) { R2S.bad_hints++; break; }
        xq_cur = arg; xq_off[arg] = xq_nw; xq_n[arg] = value;
        RX.state = RX_XQ; RX.need = 4u * value; RX.got = 0;
        if (!value) { xq_have = arg + 1; RX.state = RX_IDLE; }
        break;
    case R2H_PART:                               /* (r2-m6, R6) the particle class's span (gpu_front.c "part") */
        gf_r2_part = arg & 1;
        if (gf_r2_part) R2S.part_spans++;
        break;
    case R2H_MARK: {                             /* (r2-m8, R8) a point of the frame: GPU_FOP_MARK in stream order */
        gf_r2_flush();
        gf_flush_open();
        gpu_fop_mark* o = gf_cmd(GPU_FOP_MARK, sizeof *o);
        if (o) { o->kind = arg; o->value = value; }
        break;
    }
    case R2H_RUN: {                              /* (hybrid mode: runs, no R2H_FRAME) */
        if (fx_env < 0) { const char* e = getenv("PS2_R2_FX"); fx_env = !(e && *e == '0'); }
        gf_r2_fx_ttl = fx_env ? 2 : 0;
        gf_r2_td_ttl = fx_env ? 4 : 0;
        double t0 = qms();
        run_hint(ev, arg, value);
        fr_ms += qms() - t0;
        break;
    }
    default:
        break;
    }
}

void (*gf_r2_hook(void))(const gs_env_view*, uint64_t) { return fr_r2_hint; }

void gpu_front_r2_ab(int native) { ab_native = native; }

/* (PS2_R2_STATS) the frontend's counters every 300 frames, from gf_vblank_impl */
void gf_r2_vblank(void) {
    static int on = -1;
    static uint32_t n;
    if (gf_r2_fx_ttl > 0) gf_r2_fx_ttl--;
    if (gf_r2_td_ttl > 0) gf_r2_td_ttl--;
    gf_r2_part = 0;                              /* (r2-m6: a span never outlives its frame, whatever hint was lost) */
    if (on < 0) { const char* e = getenv("PS2_R2_STATS"); on = e && *e && *e != '0'; }
    if (on && fr_ms > 3.0) fprintf(stderr, "[r2] slow frontend frame %u: %.2f ms in R2 runs / segments\n", n, fr_ms);
    fr_ms = 0;
    if (!on || ++n % 300) return;
    const gpu_r2_front_stats* s = &R2S;
    double f = 300.0;
    fprintf(stderr, "[r2] front: per frame %.1f segments, %.1f runs, %.1f entries, %.1f draws of %.1f items (%.1f Z-writing, %.1f ATST "
                    "splits, %.1f all-fail), %.1f textures (%.2f GPU data), dropped %.2f, unsupported %.2f, As>128 %.2f, bad hints %llu, "
                    "stale %llu, brackets %.1f; particles: %.2f spans, %.1f native / %.1f FF / %.1f ROV triangles, %.1f other primitives\n",
            s->segments / f, s->runs / f, s->entries / f, s->draws / f, s->items / f, s->zwrite_draws / f, s->splits / f, s->all_fail / f,
            s->textures / f, s->tex_gpu / f, s->dropped / f, s->unsupported / f, s->over128 / f, (unsigned long long)s->bad_hints,
            (unsigned long long)s->stale, s->brackets / f, s->part_spans / f, s->part_prims / f, s->part_exact / f, s->part_rov / f, s->part_other / f);
    if (pl_stat_segs)                            /* (r8-perf) the batching plan */
        fprintf(stderr, "[r2] front: plan per frame %.1f draws -> %.1f GPU draws (%.1f in merged ones), %.2f levels per segment\n",
                pl_stat_draws / f, pl_stat_chunks / f, pl_stat_merged / f, (double)pl_stat_levels / (double)pl_stat_segs);
    pl_stat_draws = pl_stat_chunks = pl_stat_merged = pl_stat_levels = pl_stat_segs = 0;
    extern uint32_t gf_z32_ab[2];                /* (r2-m5b, diagnostics) gpu_front.c: Z above the FF range per A / B phase */
    fprintf(stderr, "[r2] front: Z beyond the FF range: %u primitives in A phases, %u in B (300 frames)\n", gf_z32_ab[0], gf_z32_ab[1]);
    gf_z32_ab[0] = gf_z32_ab[1] = 0;
    extern uint32_t gf_rov_ab[2][16];
    for (int b = 0; b < 2; b++) {
        fprintf(stderr, "[r2] front: ROV primitives by reason, phase %c:", 'A' + b);
        for (int k = 0; k < 16; k++) if (gf_rov_ab[b][k]) fprintf(stderr, " %d:%u", k, gf_rov_ab[b][k]);
        fprintf(stderr, "\n");
    }
    memset(gf_rov_ab, 0, sizeof gf_rov_ab);
    memset(&R2S, 0, sizeof R2S);
}

void gpu_front_r2_stats(gpu_r2_front_stats* out, int reset) {
    *out = R2S;
    if (reset) memset(&R2S, 0, sizeof R2S);
}
