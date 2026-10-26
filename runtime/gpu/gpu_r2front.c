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
 *   - the draw records in the game's order of entries; (r2-m2) consolidated per entry: one draw per (pass, state,
 *     discard rule) over the entry's sub-batches (a batch mask where some batches belong to another draw), the alpha
 *     test split into a "pass" and a "fail" draw only when a pass's source alpha range (vertex alpha x the CLUT
 *     entries' alpha) straddles the reference. Within an entry all of pass A is drawn before pass B (the design's
 *     consolidation; A/B: within level 2 of r2-m1's order, PS2_R2_CONSOLIDATE=0); material records are static (r2_render.c).
 * (r2-m2) Hybrid mode: R2H_RUN hints, the entries from the frame's scene slot (r2.h r2_slot, W x C computed on the EE).
 * Bracket mode (R2H_BEGIN / R2H_END around an entry's exact CALLs): ignored, or, with gpu_front_r2_ab(1) (PS2_R2_AB), the
 * exact primitives between them are skipped and R2 draws the entry instead: the hybrid picture from the same stream. */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"
#include "r2/r2.h"

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

/* ---- receiving hint words ----------------------------------------------------------------------------------------- */
enum { RX_IDLE, RX_FRAME, RX_ENTRY };
static struct {
    int state, bracket;
    uint32_t need, got, handle, flags;
    uint32_t w[32];
} RX;
static r2_frame_block FB;
static r2_screen_model SM;
static int fb_ok;
static uint32_t fb_serial;

/* ---- the open segment ---------------------------------------------------------------------------------------------- */
typedef struct { uint32_t handle, ent; } seg_item;
static seg_item* items;
static gpu_r2_entry* ents;
static uint32_t nitems, capitems, nents, capents;
static uint64_t seg_frame, seg_zbuf, seg_sc, seg_xyo, seg_fba, seg_cc;

/* statistics (gpu_front_r2_stats) */
static gpu_r2_front_stats R2S;
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
    int nh = 1, zw_ok[2] = { 1, 1 }, wm_h[2] = { wm, wm };
    uint8_t ru[2] = { 0, 1 };
    if (ate) {
        int n = atst_n(atst, aref, as_lo, as_hi), all = as_hi - as_lo + 1;
        int outcome = atst == 1 || n == all ? 1 : n == 0 ? 2 : 4;   /* all pass / all fail / straddle */
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
        st[k] = (uint32_t)bl | (uint32_t)depth << 4 | (uint32_t)wm_h[hv] << 8 | (uint32_t)(B->cull ? 1 : 0) << 12;
        rule[k] = ru[hv];
        if (zw) R2S.zwrite_draws++;
        k++;
    }
    return k;
}

/* ---- the segment's draws: consolidated per (entry, pass, state, rule) over the entry's sub-batches ----------------- */
static int consolidate = -1;             /* PS2_R2_CONSOLIDATE=0: one draw per sub-batch, pass and half (r2-m1's order) */
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

static void emit_entry(const r2_mesh* m, uint32_t ent, float fade, int zmsk, int wm, uint32_t clipbit, uint32_t* nd) {
    uint32_t st[2];
    uint8_t ru[2];
    if (!consolidate) {                  /* batch-major, as the VU emits them */
        for (uint32_t b = 0; b < m->nb; b++)
            for (uint32_t p = 0; p < m->b[b].npass; p++) {
                int n = pass_items(&m->b[b], &m->b[b].pass[p], fade, zmsk, wm, st, ru);
                for (int k = 0; k < n; k++) {
                    gpu_r2_draw* d = new_draw(nd);
                    if (!d) return;
                    d->entry = ent; d->mode = p | (uint32_t)ru[k] << 2 | b << 8; d->mask_off = ~0u; d->mesh = m->handle;
                    d->first = m->b[b].ifirst; d->count = m->b[b].icount; d->state = st[k] | clipbit;
                }
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
    if (consolidate < 0) { const char* e = getenv("PS2_R2_CONSOLIDATE"); consolidate = !(e && *e == '0'); }
    uint32_t nd = 0;
    nmask = 0;
    for (uint32_t i = 0; i < nitems; i++) {
        const r2_mesh* m = r2_mesh_get(items[i].handle);
        if (!m) { R2S.dropped++; continue; }
        emit_entry(m, items[i].ent, ents[items[i].ent].fade, zmsk, wm, (ents[items[i].ent].flags & R2E_CLIP) ? 1u << 13 : 0u, &nd);
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
static int entry_add(const gs_env_view* ev, uint32_t handle, const float m[16], float fade, uint32_t eflags) {
    if (!fb_ok || !r2_mesh_get(handle)) { R2S.dropped++; return 0; }
    seg_env(ev);
    if (!grow((void**)&items, &capitems, nitems + 1, sizeof *items) || !grow((void**)&ents, &capents, nents + 1, sizeof *ents)) return 0;
    gpu_r2_entry* e = &ents[nents];
    memcpy(e->m, m, sizeof e->m);
    e->fade = fade;
    e->flags = eflags;
    e->pad[0] = e->pad[1] = 0;
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
    if (RX.flags & R2D_FADE) memcpy(&fade, &RX.w[k++], 4);
    if (RX.flags & R2D_IDENTITY) memcpy(m, FB.clip, sizeof m);
    else {
        float w[16];
        memcpy(w, &RX.w[k], sizeof w);
        r2_matmul(w, FB.clip, m);
    }
    return entry_add(ev, RX.handle, m, fade, RX.flags & R2D_CLIP ? R2E_CLIP : 0);
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
        entry_add(ev, se->handle, se->m, se->fade, se->flags);
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
        if (RX.state == RX_IDLE || RX.got >= RX.need || RX.got >= 32) { R2S.bad_hints++; return; }
        RX.w[RX.got++] = value;
        if (RX.got < RX.need) return;
        if (RX.state == RX_FRAME) {
            memcpy(&FB, RX.w, sizeof FB);
            fb_ok = r2_screen_model_of(&FB, &SM);
        } else if (!RX.bracket || ab_native) {
            int taken = entry_done(ev);
            /* (r2-m2) bracket A/B: the exact primitives are skipped only when R2 took the entry (a recording that starts
             * after its frame's R2H_FRAME block has no frame constants for the rest of that frame: drawn exactly) */
            if (RX.bracket) gf_r2_skip = ab_native && taken;
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
        break;
    case R2H_DRAW: case R2H_BEGIN:
        RX.bracket = kind == R2H_BEGIN;
        RX.handle = value;
        RX.flags = arg;
        RX.need = (arg & R2D_FADE ? 1u : 0u) + (arg & R2D_IDENTITY ? 0u : 16u) + 1u;   /* (+ the address word) */
        RX.got = 0;
        if (RX.bracket) { gf_r2_skip = 0; R2S.brackets++; }   /* (set once the entry's words are in: entry taken) */
        RX.state = RX_ENTRY;
        break;
    case R2H_END:
        gf_r2_skip = 0;
        break;
    case R2H_RUN: {
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
    if (on < 0) { const char* e = getenv("PS2_R2_STATS"); on = e && *e && *e != '0'; }
    if (on && fr_ms > 3.0) fprintf(stderr, "[r2] slow frontend frame %u: %.2f ms in R2 runs / segments\n", n, fr_ms);
    fr_ms = 0;
    if (!on || ++n % 300) return;
    const gpu_r2_front_stats* s = &R2S;
    double f = 300.0;
    fprintf(stderr, "[r2] front: per frame %.1f segments, %.1f runs, %.1f entries, %.1f draws of %.1f items (%.1f Z-writing, %.1f ATST "
                    "splits, %.1f all-fail), %.1f textures (%.2f GPU data), dropped %.2f, unsupported %.2f, As>128 %.2f, bad hints %llu, "
                    "stale %llu, brackets %.1f\n",
            s->segments / f, s->runs / f, s->entries / f, s->draws / f, s->items / f, s->zwrite_draws / f, s->splits / f, s->all_fail / f,
            s->textures / f, s->tex_gpu / f, s->dropped / f, s->unsupported / f, s->over128 / f, (unsigned long long)s->bad_hints,
            (unsigned long long)s->stale, s->brackets / f);
    memset(&R2S, 0, sizeof R2S);
}

void gpu_front_r2_stats(gpu_r2_front_stats* out, int reset) {
    *out = R2S;
    if (reset) memset(&R2S, 0, sizeof R2S);
}
