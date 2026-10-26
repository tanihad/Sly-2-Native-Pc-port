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
 *   - the draw records in the game's order: per sub-batch, per pass, the alpha test split into a "pass" and a "fail" draw
 *     only when the pass's source alpha range (vertex alpha x the CLUT entries' alpha) straddles the reference.
 * Bracket mode (R2H_BEGIN / R2H_END around an entry's exact CALLs): ignored, or, with gpu_front_r2_ab(1) (PS2_R2_AB), the
 * exact primitives between them are skipped and R2 draws the entry instead: the hybrid picture from the same stream. */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"
#include "r2/r2.h"

#include <stdlib.h>
#include <string.h>

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

/* the draws of one pass of one sub-batch */
static void emit_pass(const r2_mesh* m, const r2_batch* B, const r2_pass* P, uint32_t ent, float fade, int zmsk, int wmask_fb,
                      uint32_t* nd) {
    if (!B->icount) return;
    uint32_t prim = P->prim;
    int abe = (prim >> 6) & 1, fge = (prim >> 5) & 1;
    uint64_t test = P->test, alpha = P->alpha, tex0 = P->tex0;
    int ate = (int)(test & 1), atst = (int)((test >> 1) & 7), aref = (int)((test >> 4) & 0xFF), afail = (int)((test >> 12) & 3);
    int zte = (int)((test >> 16) & 1), ztst = zte ? (int)((test >> 17) & 3) : 1;
    if (ztst == 0) return;                                       /* Z NEVER: nothing */
    if ((test >> 14) & 1) { R2S.unsupported++; return; }         /* DATE (never in prelit materials) */
    /* blend class (gpu_front.c state_update) */
    int A = (int)(alpha & 3), Bs = (int)((alpha >> 2) & 3), C = (int)((alpha >> 4) & 3), D = (int)((alpha >> 6) & 3);
    int fix = (int)((alpha >> 32) & 0xFF), bl = BL_OFF, wm = wmask_fb;
    if (abe) {
        int uses_cd = A == 1 || Bs == 1 || C == 1 || D == 1;
        if (A == Bs) { if (D == 1) { bl = BL_NOCOLOR; wm &= 8; } }
        else if (!uses_cd) bl = BL_OFF;
        else if (C != 1 && A == 0 && Bs == 2 && D == 1) bl = BL_ADD;
        else if (C != 1 && A == 2 && Bs == 0 && D == 1) bl = BL_REVSUB;
        else if (C != 1 && A == 0 && Bs == 1 && D == 1) bl = BL_DUAL;
        else { R2S.unsupported++; return; }
        if (bl == BL_DUAL && C == 2 && fix > 128) { R2S.unsupported++; return; }
    }
    /* texture filter: LCM 1 (fixed LOD K), no mipmaps (r2_mesh.c checks): K <= 0 magnifies (MMAG), else MMIN */
    uint64_t tex1 = P->tex1;
    int k = (int)((int64_t)(tex1 << 20) >> 52);                  /* K: signed 12 bits at bit 32 */
    int filt = k <= 0 ? (int)((tex1 >> 5) & 1) : (int)((tex1 >> 6) & 7);
    if (filt > 1) { R2S.unsupported++; return; }
    int texm = filt ? 2 : 1;
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
    if (bl == BL_DUAL && C == 0 && as_hi > 128) R2S.over128++;  /* (the exact path draws those on the ROV tier) */
    /* the alpha test: halves */
    int halves[2] = { 0, -1 };                                   /* draw kinds: 0 all, 1 pass half, 2 fail half */
    int zw_ok[2] = { 1, 1 }, wm_h[2] = { wm, wm };
    int keep = 0;
    if (ate) {
        int n = atst_n(atst, aref, as_lo, as_hi), all = as_hi - as_lo + 1;
        int outcome = atst == 1 || n == all ? 1 : n == 0 ? 2 : 4;   /* all pass / all fail / straddle */
        if (outcome == 2 || outcome == 4) {
            /* AFAIL of the failing fragments: KEEP nothing, FB_ONLY no Z, ZB_ONLY no colour, RGB_ONLY no Z and no alpha */
            int fz = afail == 2, fwm = afail == 2 ? 0 : afail == 3 ? (wm & 7) : wm;
            if (outcome == 2) {
                if (afail == 0) return;
                zw_ok[0] = fz; wm_h[0] = fwm;
            } else if (afail == 0) {
                keep = 1;                                        /* one draw, failing fragments discarded */
            } else {
                halves[0] = 1; halves[1] = 2;
                zw_ok[1] = fz; wm_h[1] = fwm;
                R2S.splits++;
            }
        }
    }
    for (int hv = 0; hv < 2 && halves[hv] >= 0; hv++) {
        int zw = !zmsk && zw_ok[hv] && ztst != 0;
        if (!wm_h[hv] && !zw) continue;
        if (!grow((void**)&R2S_draws, &R2S_capdraws, *nd + 1, sizeof(gpu_r2_draw))) return;
        gpu_r2_draw* d = &R2S_draws[(*nd)++];
        memset(d, 0, sizeof *d);
        d->mesh = m->handle;
        d->first = B->ifirst;
        d->count = B->icount;
        d->entry = ent;
        d->tex0[0] = (uint32_t)tex0; d->tex0[1] = (uint32_t)(tex0 >> 32);
        d->clamp[0] = (uint32_t)P->clamp; d->clamp[1] = (uint32_t)(P->clamp >> 32);
        d->tex1 = (uint32_t)P->tex1;
        d->test = (uint32_t)test;
        d->alpha = (uint32_t)(A | Bs << 2 | C << 4 | D << 6) | (uint32_t)fix << 8;
        d->fogcol = (uint32_t)P->fogcol & 0xFFFFFF;
        d->fog_scale = B->fog_scale;
        d->fog_wmax = B->fog_wmax;
        d->flags = (uint32_t)(csel ? GPU_R2D_CSEL : 0) | (halves[hv] == 1 ? GPU_R2D_HALF_PASS : 0) | (halves[hv] == 2 ? GPU_R2D_HALF_FAIL : 0) |
                   (abe ? GPU_R2D_ABE : 0) | (fge ? GPU_R2D_FGE : 0) | (tcc ? GPU_R2D_TCC : 0) | (keep ? GPU_R2D_ATST_KEEP : 0) |
                   (uint32_t)texm << GPU_R2D_TEXM_SHIFT;
        int depth = ztst * 2 + zw;                               /* (gpu_exec.c depth classes: ztst * 2 + zwrite) */
        if (zw) R2S.zwrite_draws++;
        if (ate && halves[0] == 0 && !keep && zw_ok[0] != 1) R2S.all_fail++;
        d->state = (uint32_t)bl | (uint32_t)depth << 4 | (uint32_t)wm_h[hv] << 8 | (uint32_t)(B->cull ? 1 : 0) << 12;
        R2S.draws++;
    }
}

/* the open segment becomes a GPU_FOP_R2 */
void gf_r2_flush(void) {
    if (!gf_r2_pending) return;
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
    /* the draws, in the game's order */
    uint32_t nd = 0;
    for (uint32_t i = 0; i < nitems; i++) {
        const r2_mesh* m = r2_mesh_get(items[i].handle);
        if (!m) { R2S.dropped++; continue; }
        float fade = ents[items[i].ent].fade;
        for (uint32_t b = 0; b < m->nb; b++)
            for (uint32_t p = 0; p < m->b[b].npass; p++) emit_pass(m, &m->b[b], &m->b[b].pass[p], items[i].ent, fade, zmsk, wm, &nd);
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
    o->draw_off = gf_data(R2S_draws, nd * (uint32_t)sizeof(gpu_r2_draw));
    o->nent = nents;
    o->ndraw = nd;
    o->gen = items[0].handle >> 20;
    gf.d3d_draws += nd;
    R2S.segments++;
    R2S.entries += nitems;
    nitems = nents = 0;
}

/* an entry's words are complete: into the open segment (a new one when the environment changed) */
static void entry_done(const gs_env_view* ev) {
    const uint64_t* r = ev->reg;
    uint64_t frame = r[0x4C], zbuf = r[0x4E], sc = r[0x40], xyo = r[0x18], fba = r[0x4A], cc = r[0x46];
    if (gf_r2_pending && (frame != seg_frame || zbuf != seg_zbuf || sc != seg_sc || xyo != seg_xyo || fba != seg_fba || cc != seg_cc))
        gf_r2_flush();
    uint32_t nw = (RX.flags & R2D_FADE ? 1u : 0u) + (RX.flags & R2D_IDENTITY ? 0u : 16u);
    if (!r2_mesh_get(RX.handle) && r2_mesh_resolve)
        r2_mesh_resolve(RX.handle, RX.w[nw] & 0x7FFFFFFFu, (int)(RX.w[nw] >> 31));   /* (offline replays) */
    if (!fb_ok || !r2_mesh_get(RX.handle)) { R2S.dropped++; return; }
    if (!gf_r2_pending) {
        gf_r2_pending = 1;
        seg_frame = frame; seg_zbuf = zbuf; seg_sc = sc; seg_xyo = xyo; seg_fba = fba; seg_cc = cc;
        nitems = nents = 0;
    }
    if (!grow((void**)&items, &capitems, nitems + 1, sizeof *items) || !grow((void**)&ents, &capents, nents + 1, sizeof *ents)) return;
    gpu_r2_entry* e = &ents[nents];
    memset(e, 0, sizeof *e);
    uint32_t k = 0;
    e->fade = 1.0f;
    if (RX.flags & R2D_FADE) memcpy(&e->fade, &RX.w[k++], 4);
    if (RX.flags & R2D_IDENTITY) memcpy(e->m, FB.clip, sizeof e->m);
    else {
        float w[16];
        memcpy(w, &RX.w[k], sizeof w);
        r2_matmul(w, FB.clip, e->m);
    }
    items[nitems].handle = RX.handle;
    items[nitems].ent = nents;
    nitems++;
    nents++;
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
            entry_done(ev);
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
        if (RX.bracket) { gf_r2_skip = ab_native; R2S.brackets++; }
        RX.state = RX_ENTRY;
        break;
    case R2H_END:
        gf_r2_skip = 0;
        break;
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
    if (!on || ++n % 300) return;
    const gpu_r2_front_stats* s = &R2S;
    double f = 300.0;
    fprintf(stderr, "[r2] front: per frame %.1f segments, %.1f entries, %.1f draws (%.1f Z-writing, %.1f ATST splits, %.1f all-fail), "
                    "%.1f textures (%.2f GPU data), dropped %.2f, unsupported %.2f, As>128 %.2f, bad hints %llu, brackets %.1f\n",
            s->segments / f, s->entries / f, s->draws / f, s->zwrite_draws / f, s->splits / f, s->all_fail / f, s->textures / f,
            s->tex_gpu / f, s->dropped / f, s->unsupported / f, s->over128 / f, (unsigned long long)s->bad_hints, s->brackets / f);
    memset(&R2S, 0, sizeof R2S);
}

void gpu_front_r2_stats(gpu_r2_front_stats* out, int reset) {
    *out = R2S;
    if (reset) memset(&R2S, 0, sizeof R2S);
}
