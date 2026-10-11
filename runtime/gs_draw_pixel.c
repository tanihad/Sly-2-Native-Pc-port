/* GS pixel pipeline: texture sampling + texture function, fog, alpha test, destination alpha
 * test, depth test, alpha blending, dithering, colour clamp, FBA and masked frame/Z writes.
 *
 * The spans of one primitive are collected into a batch (at most GS_MAX_SPAN pixels) and run
 * through short stage loops over per-batch arrays:
 *   addresses -> colour (flat/Gouraud) -> texels -> texture function -> fog -> tests and Z ->
 *   blend and frame buffer write.
 * Each stage is a tight loop with few live values, picked once per batch from the primitive state
 * (no per-pixel switch over the whole state, no combinatorial set of fused loops), and the stage
 * overhead is paid per batch rather than per span. Pixels of one primitive never overlap, so
 * running stage by stage gives the same result as pixel by pixel.
 *
 * With pixel worker threads, spans are drawn deferred and in parallel bands (see "deferred,
 * band-parallel drawing" below). */
#include "gs_draw.h"

#include <emmintrin.h>
#include <math.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <windows.h>

#define MAX_SEGS 512

typedef struct {
    int y, x0, n, off;                   /* off: index of the first pixel in the batch arrays */
    gs_span_start st;
} seg;

typedef struct {                         /* one batch of spans and its per-pixel work arrays */
    const gs_prim* p;
    int stq;
    int nseg, npix;
    seg segs[MAX_SEGS];
    uint32_t col[GS_MAX_SPAN];           /* fragment colour 0xAABBGGRR */
    uint32_t tex[GS_MAX_SPAN];           /* texel colour */
    uint32_t fa[GS_MAX_SPAN];            /* frame buffer byte offsets from fbase */
    uint8_t wm[GS_MAX_SPAN];             /* write mode: 0 none, 1 RGBA, 2 RGB only */
    uint8_t ftmp[GS_MAX_SPAN * 4], ztmp[GS_MAX_SPAN * 4];   /* slow path: pixels via gs_mem */
} batch;

static batch B0;                         /* immediate drawing (no pixel workers) */

GS_AI int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
GS_AI uint32_t clamp255(int v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
GS_AI int wrapc(int c, int lo, int hi, int msk, int fix) { c = c < lo ? lo : c > hi ? hi : c; return (c & msk) | fix; }
GS_AI uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
GS_AI uint32_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
GS_AI void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
GS_AI void wr16(uint8_t* p, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }

GS_AI int f2i(float x) {                 /* floor, saturating, NaN -> large negative */
    if (!(x > -1e8f)) return -100000000;
    if (x > 1e8f) return 100000000;
    int i = (int)x;                      /* truncates toward zero: step down for negative fractions */
    return i - (x < (float)i);
}

/* ---- addresses ----------------------------------------------------------------------------------- */

static void row_addrs(const gs_fmt* f, uint32_t page0, uint32_t bw, int x0, int y, int n, uint32_t* out) {
    const uint16_t* row = f->tab + (((uint32_t)y & ((1u << f->ph_shift) - 1)) << 6);
    uint32_t prow = page0 + ((uint32_t)y >> f->ph_shift) * bw;
    int es = f->elem_shift, i = 0, x = x0;
    while (i < n) {
        uint32_t base = ((prow + ((uint32_t)x >> 6)) & 511u) << 13;
        int o = x & 63, run = 64 - o < n - i ? 64 - o : n - i;
        for (int k = 0; k < run; k++) out[i + k] = base | ((uint32_t)row[o + k] << es);
        i += run; x += run;
    }
}

/* ---- texture sampling (coordinates in 1/16 texel) ---------------------------------------------- */

typedef struct {
    const uint32_t* tex;
    int sh, ulo, uhi, umsk, ufix, vlo, vhi, vmsk, vfix;
} lvl_view;

GS_AI lvl_view view_of(const gs_texlvl* L) {
    lvl_view v = { L->tex, L->ushift, L->ulo, L->uhi, L->umsk, L->ufix, L->vlo, L->vhi, L->vmsk, L->vfix };
    return v;
}

GS_AI uint32_t tex_point(const lvl_view* L, int U, int V) {
    int x = wrapc(U >> 4, L->ulo, L->uhi, L->umsk, L->ufix);
    int y = wrapc(V >> 4, L->vlo, L->vhi, L->vmsk, L->vfix);
    return L->tex[(y << L->sh) + x];
}

/* Bilinear filter with 4-bit weights: sum(texel * wu * wv) >> 8, exactly; SSE2 16-bit lanes.
 * a b: top left, top right; c d: bottom left, bottom right. */
GS_AI uint32_t bilerp(uint32_t a, uint32_t b, uint32_t c, uint32_t d, int fu, int fv) {
    const __m128i zero = _mm_setzero_si128();
    __m128i top = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)a), _mm_cvtsi32_si128((int)b)), zero);
    __m128i bot = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)c), _mm_cvtsi32_si128((int)d)), zero);
    /* vertical: top * (16 - fv) + bot * fv  (<= 4080) */
    __m128i v = _mm_add_epi16(_mm_slli_epi16(top, 4), _mm_mullo_epi16(_mm_sub_epi16(bot, top), _mm_set1_epi16((short)fv)));
    /* horizontal: left * (16 - fu) + right * fu  (<= 65280, unsigned 16-bit) */
    __m128i h = _mm_mullo_epi16(v, _mm_unpacklo_epi64(_mm_set1_epi16((short)(16 - fu)), _mm_set1_epi16((short)fu)));
    h = _mm_add_epi16(h, _mm_srli_si128(h, 8));
    h = _mm_srli_epi16(h, 8);
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(h, h));
}

GS_AI uint32_t tex_bilinear(const lvl_view* L, int U, int V) {
    U -= 8; V -= 8;                      /* texel centres are at +0.5 */
    int fu = U & 15, fv = V & 15;
    int u0 = U >> 4, v0 = V >> 4;
    int xa = wrapc(u0, L->ulo, L->uhi, L->umsk, L->ufix), xb = wrapc(u0 + 1, L->ulo, L->uhi, L->umsk, L->ufix);
    int ya = wrapc(v0, L->vlo, L->vhi, L->vmsk, L->vfix) << L->sh;
    int yb = wrapc(v0 + 1, L->vlo, L->vhi, L->vmsk, L->vfix) << L->sh;
    return bilerp(L->tex[ya + xa], L->tex[ya + xb], L->tex[yb + xa], L->tex[yb + xb], fu, fv);
}

/* Direct sampling from VRAM (see gs_draw.h): K is the level's GS_TK_* kind. */
GS_AI uint32_t dtex_point(const gs_texlvl* L, int U, int V, const int K) {
    int x = wrapc(U >> 4, L->ulo, L->uhi, L->umsk, L->ufix), y = wrapc(V >> 4, L->vlo, L->vhi, L->vmsk, L->vfix);
    return gs_tex_texel(L, x, y, K);
}

GS_AI uint32_t dtex_bilinear(const gs_texlvl* L, int U, int V, const int K) {
    U -= 8; V -= 8;
    int fu = U & 15, fv = V & 15;
    int u0 = U >> 4, v0 = V >> 4;
    int xa = wrapc(u0, L->ulo, L->uhi, L->umsk, L->ufix), xb = wrapc(u0 + 1, L->ulo, L->uhi, L->umsk, L->ufix);
    int ya = wrapc(v0, L->vlo, L->vhi, L->vmsk, L->vfix), yb = wrapc(v0 + 1, L->vlo, L->vhi, L->vmsk, L->vfix);
    return bilerp(gs_tex_texel(L, xa, ya, K), gs_tex_texel(L, xb, ya, K),
                  gs_tex_texel(L, xa, yb, K), gs_tex_texel(L, xb, yb, K), fu, fv);
}

/* any kind, chosen at run time (mipmapped sampling) */
static uint32_t dtex_any(const gs_texlvl* L, int U, int V, int bilinear) {
#define DK(K) return bilinear ? dtex_bilinear(L, U, V, K) : dtex_point(L, U, V, K)
    switch (L->kind) {
    case GS_TK_32: DK(GS_TK_32);
    case GS_TK_24: DK(GS_TK_24);
    case GS_TK_16: DK(GS_TK_16);
    case GS_TK_8: DK(GS_TK_8);
    case GS_TK_4: DK(GS_TK_4);
    case GS_TK_8H: DK(GS_TK_8H);
    case GS_TK_4HL: DK(GS_TK_4HL);
    default: DK(GS_TK_4HH);
    }
#undef DK
}

/* the same without wrapping, when the primitive's texel range is known to be inside the texture */
GS_AI uint32_t tex_point_nw(const lvl_view* L, int U, int V) { return L->tex[((V >> 4) << L->sh) + (U >> 4)]; }

GS_AI uint32_t tex_bilinear_nw(const lvl_view* L, int U, int V) {
    U -= 8; V -= 8;
    int fu = U & 15, fv = V & 15;
    const uint32_t* t = L->tex + ((V >> 4) << L->sh) + (U >> 4);
    const uint32_t* b = t + (1 << L->sh);
    const __m128i zero = _mm_setzero_si128();
    __m128i top = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)t), zero);
    __m128i bot = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)b), zero);
    __m128i v = _mm_add_epi16(_mm_slli_epi16(top, 4), _mm_mullo_epi16(_mm_sub_epi16(bot, top), _mm_set1_epi16((short)fv)));
    __m128i h = _mm_mullo_epi16(v, _mm_unpacklo_epi64(_mm_set1_epi16((short)(16 - fu)), _mm_set1_epi16((short)fu)));
    h = _mm_add_epi16(h, _mm_srli_si128(h, 8));
    h = _mm_srli_epi16(h, 8);
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(h, h));
}

GS_AI uint32_t lerp_rgba(uint32_t a, uint32_t b, uint32_t f) {     /* f in 0..16 */
    uint32_t rb = ((a & 0xFF00FFu) * (16 - f) + (b & 0xFF00FFu) * f) >> 4;
    uint32_t ga = (((a >> 8) & 0xFF00FFu) * (16 - f) + ((b >> 8) & 0xFF00FFu) * f) >> 4;
    return (rb & 0xFF00FFu) | ((ga & 0xFF00FFu) << 8);
}

static uint32_t tex_level(const gs_prim* p, int l, int U, int V, int bilinear) {
    if (p->lvl[l].kind >= 0) return dtex_any(&p->lvl[l], U >> l, V >> l, bilinear);
    lvl_view L = view_of(&p->lvl[l]);
    return bilinear ? tex_bilinear(&L, U >> l, V >> l) : tex_point(&L, U >> l, V >> l);
}

/* LOD in 1/16 units: LCM=0: (log2(1/|Q|) << L) + K, LCM=1: K */
static int lod16_of(const gs_prim* p, float q) {
    if (p->lcm) return p->k;
    union { float f; uint32_t u; } c = { fabsf(q) };
    if (c.u == 0) return 0x7FFF;
    int e = (int)((c.u >> 23) & 0xFF) - 127, m = (int)((c.u >> 19) & 15);
    return -(e * 16 + m) * (1 << p->lshift) + p->k;
}

/* General sampling: MMAG/MMIN with mipmaps, per-pixel LOD. */
static uint32_t tex_general(const gs_prim* p, int U, int V, float q) {
    int lod = lod16_of(p, q);
    if (lod <= 0) return tex_level(p, 0, U, V, p->mmag);
    int mxl = p->mxl;
    switch (p->mmin) {
    case 0: return tex_level(p, 0, U, V, 0);
    case 1: return tex_level(p, 0, U, V, 1);
    case 2: case 4: {                    /* *_MIPMAP_NEAREST */
        int l = (lod + 8) >> 4;
        return tex_level(p, l > mxl ? mxl : l, U, V, p->mmin == 4);
    }
    default: {                           /* *_MIPMAP_LINEAR */
        int l = lod >> 4, bl = p->mmin == 5;
        if (l >= mxl) return tex_level(p, mxl, U, V, bl);
        return lerp_rgba(tex_level(p, l, U, V, bl), tex_level(p, l + 1, U, V, bl), (uint32_t)(lod & 15));
    }
    }
}

/* texel stage, specialised on coordinates (UV/STQ) and filter (1 point, 2 bilinear, 3 general,
 * 4 point without wrapping, 5 bilinear without wrapping) */
GS_AI void sample_batch(batch* b, const gs_prim* p, int k0, int k1, const int STQ, const int FILT) {
    const lvl_view L = view_of(&p->lvl[p->cur_level]);
    const int cl = p->cur_level;
    const float ds = p->ds, dt = p->dt, dq = p->dq, tw16 = p->tw16, th16 = p->th16, lq = p->lod_q;
    const int32_t du = p->du, dv = p->dv;
    for (int k = k0; k < k1; k++) {
        const seg* sg = &b->segs[k];
        uint32_t* out = b->tex + sg->off;
        const int n = sg->n;
        if (STQ) {
            float s = sg->st.s, t = sg->st.t, q = sg->st.q;
            for (int i = 0; i < n; i++, s += ds, t += dt, q += dq) {
                float iq = 1.0f / q;
                int U = f2i(s * iq * tw16), V = f2i(t * iq * th16);
                out[i] = FILT == 1 ? tex_point(&L, U >> cl, V >> cl) : FILT == 2 ? tex_bilinear(&L, U >> cl, V >> cl)
                       : FILT == 4 ? tex_point_nw(&L, U >> cl, V >> cl) : FILT == 5 ? tex_bilinear_nw(&L, U >> cl, V >> cl)
                       : tex_general(p, U, V, q);
            }
        } else {
            int32_t u = sg->st.u, v = sg->st.v;
            for (int i = 0; i < n; i++, u += du, v += dv) {
                int U = u >> GS_UVSH, V = v >> GS_UVSH;
                out[i] = FILT == 1 ? tex_point(&L, U >> cl, V >> cl) : FILT == 2 ? tex_bilinear(&L, U >> cl, V >> cl)
                       : FILT == 4 ? tex_point_nw(&L, U >> cl, V >> cl) : FILT == 5 ? tex_bilinear_nw(&L, U >> cl, V >> cl)
                       : tex_general(p, U, V, lq);
            }
        }
    }
}

#define SAMPLE_FN(name, STQ, FILT) \
    static void name(batch* b, const gs_prim* p, int k0, int k1) { sample_batch(b, p, k0, k1, STQ, FILT); }
SAMPLE_FN(sample_uv_point, 0, 1)
SAMPLE_FN(sample_uv_bilin, 0, 2)
SAMPLE_FN(sample_uv_gen, 0, 3)
SAMPLE_FN(sample_uv_point_nw, 0, 4)
SAMPLE_FN(sample_uv_bilin_nw, 0, 5)
SAMPLE_FN(sample_stq_point, 1, 1)
SAMPLE_FN(sample_stq_bilin, 1, 2)
SAMPLE_FN(sample_stq_gen, 1, 3)
SAMPLE_FN(sample_stq_point_nw, 1, 4)
SAMPLE_FN(sample_stq_bilin_nw, 1, 5)

typedef void (*sample_fn)(batch*, const gs_prim*, int, int);
static const sample_fn sample_table[2][2][4] = {           /* [stq][nowrap][texm] */
    { { NULL, sample_uv_point, sample_uv_bilin, sample_uv_gen },
      { NULL, sample_uv_point_nw, sample_uv_bilin_nw, sample_uv_gen } },
    { { NULL, sample_stq_point, sample_stq_bilin, sample_stq_gen },
      { NULL, sample_stq_point_nw, sample_stq_bilin_nw, sample_stq_gen } },
};

/* texel stage for a level sampled directly from VRAM: point (FILT 1) or bilinear (2), kind K */
GS_AI void dsample_batch(batch* b, const gs_prim* p, int k0, int k1, const int STQ, const int FILT, const int K) {
    const int cl = p->cur_level;
    const gs_texlvl* L = &p->lvl[cl];
    const float ds = p->ds, dt = p->dt, dq = p->dq, tw16 = p->tw16, th16 = p->th16;
    const int32_t du = p->du, dv = p->dv;
    for (int k = k0; k < k1; k++) {
        const seg* sg = &b->segs[k];
        uint32_t* out = b->tex + sg->off;
        const int n = sg->n;
        if (STQ) {
            float s = sg->st.s, t = sg->st.t, q = sg->st.q;
            for (int i = 0; i < n; i++, s += ds, t += dt, q += dq) {
                float iq = 1.0f / q;
                int U = f2i(s * iq * tw16) >> cl, V = f2i(t * iq * th16) >> cl;
                out[i] = FILT == 1 ? dtex_point(L, U, V, K) : dtex_bilinear(L, U, V, K);
            }
        } else {
            int32_t u = sg->st.u, v = sg->st.v;
            for (int i = 0; i < n; i++, u += du, v += dv) {
                int U = (u >> GS_UVSH) >> cl, V = (v >> GS_UVSH) >> cl;
                out[i] = FILT == 1 ? dtex_point(L, U, V, K) : dtex_bilinear(L, U, V, K);
            }
        }
    }
}

#define DSAMPLE_FN(STQ, F, K) \
    static void dsample_##STQ##_##F##_##K(batch* b, const gs_prim* p, int k0, int k1) { dsample_batch(b, p, k0, k1, STQ, F, K); }
#define DSAMPLE_KINDS(STQ, F) DSAMPLE_FN(STQ, F, 0) DSAMPLE_FN(STQ, F, 1) DSAMPLE_FN(STQ, F, 2) DSAMPLE_FN(STQ, F, 3) \
                              DSAMPLE_FN(STQ, F, 4) DSAMPLE_FN(STQ, F, 5) DSAMPLE_FN(STQ, F, 6) DSAMPLE_FN(STQ, F, 7)
DSAMPLE_KINDS(0, 1) DSAMPLE_KINDS(0, 2) DSAMPLE_KINDS(1, 1) DSAMPLE_KINDS(1, 2)
#define DSAMPLE_ROW(STQ, F) { dsample_##STQ##_##F##_0, dsample_##STQ##_##F##_1, dsample_##STQ##_##F##_2, dsample_##STQ##_##F##_3, \
                              dsample_##STQ##_##F##_4, dsample_##STQ##_##F##_5, dsample_##STQ##_##F##_6, dsample_##STQ##_##F##_7 }
_Static_assert(GS_TK_32 == 0 && GS_TK_4HH == 7 && GS_TK_N == 8, "direct sampler table order");
static const sample_fn dsample_table[2][2][GS_TK_N] = {   /* [stq][texm - 1][kind] */
    { DSAMPLE_ROW(0, 1), DSAMPLE_ROW(0, 2) },
    { DSAMPLE_ROW(1, 1), DSAMPLE_ROW(1, 2) },
};

/* ---- colour stages -------------------------------------------------------------------------------- */

/* first pixel index of segment k0 and the pixel count of segments [k0, k1) */
GS_AI int range_off(const batch* b, int k0) { return b->segs[k0].off; }
GS_AI int range_pix(const batch* b, int k0, int k1) { return b->segs[k1 - 1].off + b->segs[k1 - 1].n - b->segs[k0].off; }

static void colour_batch(batch* b, const gs_prim* p, int k0, int k1) {
    if (!p->iip) {
        const gs_span_start* st = &b->segs[0].st;
        uint32_t c = clamp255(st->r >> 16) | clamp255(st->g >> 16) << 8 | clamp255(st->b >> 16) << 16 | clamp255(st->a >> 16) << 24;
        uint32_t* col = b->col + range_off(b, k0);
        for (int i = 0, n = range_pix(b, k0, k1); i < n; i++) col[i] = c;
        return;
    }
    /* r, g, b, a in 4 lanes: >> 16, then saturating packs clamp to 0..255 exactly like clamp255 */
    const __m128i step = _mm_set_epi32(p->da, p->db, p->dg, p->dr);
    for (int k = k0; k < k1; k++) {
        const seg* sg = &b->segs[k];
        uint32_t* col = b->col + sg->off;
        __m128i v = _mm_set_epi32(sg->st.a, sg->st.b, sg->st.g, sg->st.r);
        for (int i = 0; i < sg->n; i++, v = _mm_add_epi32(v, step)) {
            __m128i c = _mm_srai_epi32(v, 16);
            c = _mm_packs_epi32(c, c);
            col[i] = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(c, c));
        }
    }
}

/* texture function: Cf (col) x Ct (tex), GS alpha 0x80 = 1.0 */
static void tfx_batch(int tfx, int tcc, int n, uint32_t* col, const uint32_t* tex) {
    switch (tfx * 2 + tcc) {
    case 0: case 1: {                    /* MODULATE: (Cf * Ct) >> 7 saturated, 4 pixels at a time */
        const __m128i zero = _mm_setzero_si128();
        const __m128i amask = _mm_set1_epi32(tcc ? 0 : (int)0xFF000000u);
        int i = 0;
        for (; i + 4 <= n; i += 4) {
            __m128i c = _mm_loadu_si128((const __m128i*)(col + i)), t = _mm_loadu_si128((const __m128i*)(tex + i));
            __m128i lo = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(c, zero), _mm_unpacklo_epi8(t, zero)), 7);
            __m128i hi = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(c, zero), _mm_unpackhi_epi8(t, zero)), 7);
            __m128i m = _mm_packus_epi16(lo, hi);
            m = _mm_or_si128(_mm_andnot_si128(amask, m), _mm_and_si128(amask, c));   /* TCC=0: vertex alpha */
            _mm_storeu_si128((__m128i*)(col + i), m);
        }
        for (; i < n; i++) {
            uint32_t c = col[i], t = tex[i];
            uint32_t r = ((c & 0xFF) * (t & 0xFF)) >> 7, g = (((c >> 8) & 0xFF) * ((t >> 8) & 0xFF)) >> 7;
            uint32_t b = (((c >> 16) & 0xFF) * ((t >> 16) & 0xFF)) >> 7;
            uint32_t a = tcc ? ((c >> 24) * (t >> 24)) >> 7 : c >> 24;
            col[i] = (r > 255 ? 255 : r) | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b) << 16 | (a > 255 ? 255 : a) << 24;
        }
        break;
    }
    case 2:                              /* DECAL, vertex alpha */
        for (int i = 0; i < n; i++) col[i] = (tex[i] & 0xFFFFFF) | (col[i] & 0xFF000000u);
        break;
    case 3:                              /* DECAL */
        memcpy(col, tex, (size_t)n * 4);
        break;
    default:                             /* HIGHLIGHT (2), HIGHLIGHT2 (3) */
        for (int i = 0; i < n; i++) {
            uint32_t c = col[i], t = tex[i], af = c >> 24;
            uint32_t r = (((c & 0xFF) * (t & 0xFF)) >> 7) + af, g = ((((c >> 8) & 0xFF) * ((t >> 8) & 0xFF)) >> 7) + af;
            uint32_t b = ((((c >> 16) & 0xFF) * ((t >> 16) & 0xFF)) >> 7) + af;
            uint32_t a = !tcc ? af : tfx == 2 ? (t >> 24) + af : t >> 24;
            col[i] = (r > 255 ? 255 : r) | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b) << 16 | (a > 255 ? 255 : a) << 24;
        }
        break;
    }
}

static void fog_batch(batch* b, const gs_prim* p, int k0, int k1) {
    const int32_t df = p->df;
    const int fr = p->fogr, fg = p->fogg, fb = p->fogb;
    for (int k = k0; k < k1; k++) {
        const seg* sg = &b->segs[k];
        uint32_t* col = b->col + sg->off;
        int32_t f = sg->st.f;
        for (int i = 0; i < sg->n; i++, f += df) {
            int fv = clampi(f >> 16, 0, 255), c = (int)col[i];
            int r = (fv * (c & 0xFF) + (255 - fv) * fr) >> 8;
            int g = (fv * ((c >> 8) & 0xFF) + (255 - fv) * fg) >> 8;
            int b = (fv * ((c >> 16) & 0xFF) + (255 - fv) * fb) >> 8;
            col[i] = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (col[i] & 0xFF000000u);
        }
    }
}

/* ---- tests: alpha test, destination alpha test, depth test (+ Z write) -> write modes ---------- */

/* Returns the number of pixels left to write. */
GS_AI int tests_batch(batch* b, const gs_prim* p, uint8_t* fbase, uint8_t* zbase, int k0, int k1, const int ZBITS) {
    const int ate = p->ate, afail = p->afail, date = p->date, datm = p->datm, fbits = p->ffmt->bits;
    const int date_sh = fbits == 16 ? 15 : 31;
    const uint8_t* apass = p->apass;
    const int ztst = ZBITS ? p->ztst : 1, zmsk = ZBITS ? p->zmsk : 1;
    const uint32_t zmax = p->zmax;
    const int64_t dz = p->dz;
    const int fast = gs_swz_fast;
    const gs_fmt* zf = p->zfmt ? p->zfmt : p->ffmt;
    const int zes = zf->elem_shift;
    int left = 0;
    for (int k = k0; k < k1; k++) {
        const seg* sg = &b->segs[k];
        const int y = sg->y, x0 = sg->x0, n = sg->n, off = sg->off;
        const uint32_t* col = b->col + off;
        const uint32_t* fa = b->fa + off;
        uint8_t* wm = b->wm + off;
        const uint16_t* zrow = zf->tab + (((uint32_t)y & ((1u << zf->ph_shift) - 1)) << 6);
        const uint32_t zprow = p->zpage + ((uint32_t)y >> zf->ph_shift) * p->zbw;
        int64_t z = sg->st.z;
        for (int i = 0; i < n; i++, z += dz) {
            int mode = 1, zw = 1;
            if (ate && !apass[col[i] >> 24]) {
                if (afail == 0) { wm[i] = 0; continue; }                /* KEEP */
                if (afail == 1) zw = 0;                                  /* FB_ONLY */
                else if (afail == 2) mode = 0;                           /* ZB_ONLY */
                else { zw = 0; mode = 2; }                               /* RGB_ONLY */
            }
            if (date) {
                uint32_t d = fbits == 16 ? rd16(fbase + fa[i]) : rd32(fbase + fa[i]);
                if ((int)((d >> date_sh) & 1) != datm) { wm[i] = 0; continue; }
            }
            if (ZBITS) {
                uint32_t x = (uint32_t)(x0 + i);
                uint8_t* zp = zbase + (fast ? ((((zprow + (x >> 6)) & 511u) << 13) | ((uint32_t)zrow[x & 63] << zes))
                                            : (uint32_t)(off + i) << zes);
                int64_t zi = z >> 16;
                uint32_t zv = zi < 0 ? 0 : zi > (int64_t)zmax ? zmax : (uint32_t)zi;
                uint32_t zb = ZBITS == 16 ? rd16(zp) : ZBITS == 24 ? (rd32(zp) & 0xFFFFFF) : rd32(zp);
                if (ztst == 2) { if (zv < zb) { wm[i] = 0; continue; } }        /* GEQUAL */
                else if (ztst == 3) { if (zv <= zb) { wm[i] = 0; continue; } }  /* GREATER */
                else if (ztst == 0) { wm[i] = 0; continue; }                    /* NEVER */
                if (zw && !zmsk) {
                    if (ZBITS == 16) wr16(zp, zv);
                    else if (ZBITS == 24) wr32(zp, (rd32(zp) & 0xFF000000u) | zv);
                    else wr32(zp, zv);
                }
            }
            wm[i] = (uint8_t)mode;
            left += mode != 0;
        }
    }
    return left;
}

#define TESTS_FN(Z) static int tests_z##Z(batch* b, const gs_prim* p, uint8_t* f, uint8_t* z, int k0, int k1) \
    { return tests_batch(b, p, f, z, k0, k1, Z); }
TESTS_FN(0) TESTS_FN(16) TESTS_FN(24) TESTS_FN(32)

/* ---- blend + frame buffer write ------------------------------------------------------------------ */

GS_AI int sel3(int s, int src, int dst) { return s == 0 ? src : s == 1 ? dst : 0; }

/* One segment. BL: 0 none, 1 (Cs - Cd) * As + Cd, 2 generic. FB: 32, 24 or 16. WM: write modes. */
GS_AI void write_seg(batch* b, const gs_prim* p, const seg* sg, uint8_t* fbase, const int FB, const int BL, const int WM) {
    const uint32_t* col = b->col + sg->off;
    const uint32_t* fa = b->fa + sg->off;
    const uint8_t* wm = b->wm + sg->off;
    const int n = sg->n;
    const uint32_t fmask = p->fmask, amask = p->amask, fba = (uint32_t)p->fba;
    const int pabe = p->pabe, colclamp = p->colclamp, dthe = FB == 16 && p->dthe;
    const int asel = p->asel, bsel = p->bsel, csel = p->csel, dsel = p->dsel, fix = p->fix;
    int dm[4];
    for (int k = 0; k < 4; k++) dm[k] = dthe ? p->dimx[sg->y & 3][(sg->x0 + k) & 3] : 0;   /* dm[i & 3] */
    for (int i = 0; i < n; i++) {
        if (WM && !wm[i]) continue;
        uint8_t* fp = fbase + fa[i];
        uint32_t c = col[i], m = fmask;
        if (WM && wm[i] == 2) m |= amask;
        uint32_t ca = c >> 24, dst = 0;
        int have = 0;
        if (BL && (!pabe || (ca & 0x80))) {
            dst = FB == 16 ? rd16(fp) : rd32(fp);
            have = 1;
            if (BL == 1 && FB != 16 && ca <= 0x80 && !dthe) {
                /* Cs*As + Cd*(128-As) >> 7 == ((Cs-Cd)*As >> 7) + Cd, two channels per multiply */
                uint32_t rb = (((c & 0xFF00FFu) * ca + (dst & 0xFF00FFu) * (128 - ca)) >> 7) & 0xFF00FFu;
                uint32_t g = ((((c >> 8) & 0xFF) * ca + ((dst >> 8) & 0xFF) * (128 - ca)) >> 7) & 0xFF;
                c = rb | (g << 8) | (c & 0xFF000000u);
            } else {
                int cr = (int)(c & 0xFF), cg = (int)((c >> 8) & 0xFF), cb = (int)((c >> 16) & 0xFF);
                int er, eg, eb, ea;
                if (FB == 16) {
                    er = (int)((dst << 3) & 0xF8); eg = (int)((dst >> 2) & 0xF8); eb = (int)((dst >> 7) & 0xF8);
                    ea = dst & 0x8000 ? 0x80 : 0;
                } else {
                    er = (int)(dst & 0xFF); eg = (int)((dst >> 8) & 0xFF); eb = (int)((dst >> 16) & 0xFF);
                    ea = FB == 24 ? 0x80 : (int)(dst >> 24);
                }
                if (BL == 1) {
                    cr = (((cr - er) * (int)ca) >> 7) + er;
                    cg = (((cg - eg) * (int)ca) >> 7) + eg;
                    cb = (((cb - eb) * (int)ca) >> 7) + eb;
                } else {
                    int cc = csel == 0 ? (int)ca : csel == 1 ? ea : fix;
                    int nr = (((sel3(asel, cr, er) - sel3(bsel, cr, er)) * cc) >> 7) + sel3(dsel, cr, er);
                    int ng = (((sel3(asel, cg, eg) - sel3(bsel, cg, eg)) * cc) >> 7) + sel3(dsel, cg, eg);
                    int nb = (((sel3(asel, cb, eb) - sel3(bsel, cb, eb)) * cc) >> 7) + sel3(dsel, cb, eb);
                    cr = nr; cg = ng; cb = nb;
                }
                if (dthe) { int d = dm[i & 3]; cr += d; cg += d; cb += d; }   /* before COLCLAMP */
                if (colclamp) { cr = clampi(cr, 0, 255); cg = clampi(cg, 0, 255); cb = clampi(cb, 0, 255); }
                else { cr &= 0xFF; cg &= 0xFF; cb &= 0xFF; }
                c = (uint32_t)cr | (uint32_t)cg << 8 | (uint32_t)cb << 16 | (c & 0xFF000000u);
            }
        } else if (dthe) {
            int d = dm[i & 3];
            int cr = (int)(c & 0xFF) + d, cg = (int)((c >> 8) & 0xFF) + d, cb = (int)((c >> 16) & 0xFF) + d;
            if (colclamp) { cr = clampi(cr, 0, 255); cg = clampi(cg, 0, 255); cb = clampi(cb, 0, 255); }
            else { cr &= 0xFF; cg &= 0xFF; cb &= 0xFF; }
            c = (uint32_t)cr | (uint32_t)cg << 8 | (uint32_t)cb << 16 | (c & 0xFF000000u);
        }
        c |= fba << 24;
        if (FB == 16) {
            uint32_t nv = ((c >> 3) & 0x1F) | ((c >> 6) & 0x3E0) | ((c >> 9) & 0x7C00) | ((c >> 16) & 0x8000);
            if (m) { if (!have) dst = rd16(fp); nv = (dst & m) | (nv & ~m); }
            wr16(fp, nv);
        } else {
            if (m) { if (!have) dst = rd32(fp); c = (dst & m) | (c & ~m); }
            wr32(fp, c);
        }
    }
}

GS_AI void write_batch(batch* b, const gs_prim* p, uint8_t* fbase, int k0, int k1, const int FB, const int BL, const int WM) {
    for (int k = k0; k < k1; k++) write_seg(b, p, &b->segs[k], fbase, FB, BL, WM);
}

#define WRITE_FN(F, BL, W) static void write_##F##_##BL##_##W(batch* b, const gs_prim* p, uint8_t* fb, int k0, int k1) \
    { write_batch(b, p, fb, k0, k1, F, BL, W); }
#define WRITE_FNS(F) WRITE_FN(F, 0, 0) WRITE_FN(F, 0, 1) WRITE_FN(F, 1, 0) WRITE_FN(F, 1, 1) WRITE_FN(F, 2, 0) WRITE_FN(F, 2, 1)
WRITE_FNS(32) WRITE_FNS(24) WRITE_FNS(16)
typedef void (*write_fn)(batch*, const gs_prim*, uint8_t*, int, int);
#define WRITE_ROW(F) { { write_##F##_0_0, write_##F##_0_1 }, { write_##F##_1_0, write_##F##_1_1 }, { write_##F##_2_0, write_##F##_2_1 } }
static const write_fn write_table[3][3][2] = { WRITE_ROW(32), WRITE_ROW(24), WRITE_ROW(16) };

/* ---- batch driver --------------------------------------------------------------------------------- */

/* Colour, texture, fog, tests and frame buffer write for segments [k0, k1) (addresses in b->fa). */
static void pipeline(batch* b, int k0, int k1, uint8_t* fbase, uint8_t* zbase) {
    const gs_prim* p = b->p;
    colour_batch(b, p, k0, k1);
    if (p->tme && p->texm) {
        const int kind = p->lvl[p->cur_level].kind;
        if (p->texm != 3 && kind >= 0) dsample_table[b->stq][p->texm - 1][kind](b, p, k0, k1);
        else sample_table[b->stq][p->nowrap][p->texm](b, p, k0, k1);
        int off = range_off(b, k0);
        tfx_batch(p->tfx, p->tcc, range_pix(b, k0, k1), b->col + off, b->tex + off);
    }
    if (p->fge) fog_batch(b, p, k0, k1);

    int wmode = 0;
    if (p->ate || p->date || p->zuse) {
        int left;
        switch (p->zuse ? p->zfmt->bits : 0) {
        case 16: left = tests_z16(b, p, fbase, zbase, k0, k1); break;
        case 24: left = tests_z24(b, p, fbase, zbase, k0, k1); break;
        case 32: left = tests_z32(b, p, fbase, zbase, k0, k1); break;
        default: left = tests_z0(b, p, fbase, zbase, k0, k1); break;
        }
        wmode = 1;
        if (!left) return;
    }
    const int fbits = p->ffmt->bits;
    write_table[fbits == 32 ? 0 : fbits == 24 ? 1 : 2][!p->abe ? 0 : p->std_blend ? 1 : 2][wmode](b, p, fbase, k0, k1);
}

static void fill_span(const gs_prim* p, int y, int x0, int n, const gs_span_start* st);

/* Segments [k0, k1) straight into gs_vram (gs_swz_fast). */
static void run_range(batch* b, int k0, int k1) {
    const gs_prim* p = b->p;
    if (p->solid) {
        for (int k = k0; k < k1; k++) fill_span(p, b->segs[k].y, b->segs[k].x0, b->segs[k].n, &b->segs[k].st);
        return;
    }
    for (int k = k0; k < k1; k++)
        row_addrs(p->ffmt, p->fpage, p->fbw, b->segs[k].x0, b->segs[k].y, b->segs[k].n, b->fa + b->segs[k].off);
    pipeline(b, k0, k1, gs_vram, gs_vram);
}

/* ---- immediate drawing (no pixel workers) ---------------------------------------------------------- */

static void flush(batch* b) {
    const gs_prim* p = b->p;
    if (!b->npix) return;
    if (gs_swz_fast) {
        run_range(b, 0, b->nseg);
        b->nseg = 0;
        b->npix = 0;
        return;
    }
    /* slow path: gather the pixels through gs_mem, run the pipeline, scatter them back */
    const gs_fmt* ff = p->ffmt;
    const gs_fmt* zf = p->zfmt;
    const int fbits = ff->bits, fes = ff->elem_shift, zuse = p->zuse;
    uint8_t* fbase = b->ftmp;
    uint8_t* zbase = b->ztmp;
    for (int k = 0; k < b->nseg; k++) {
        const seg* sg = &b->segs[k];
        for (int i = 0; i < sg->n; i++) {
            uint32_t j = (uint32_t)(sg->off + i);
            b->fa[j] = j << fes;
            uint32_t v = gs_mem_read(p->fpsm, p->fpage * 32, p->fbw, sg->x0 + i, sg->y);
            if (fbits == 16) wr16(fbase + b->fa[j], v); else wr32(fbase + b->fa[j], v);
            if (zuse) {
                v = gs_mem_read(p->zpsm, p->zpage * 32, p->zbw, sg->x0 + i, sg->y);
                if (zf->bits == 16) wr16(zbase + (j << zf->elem_shift), v); else wr32(zbase + (j << zf->elem_shift), v);
            }
        }
    }
    pipeline(b, 0, b->nseg, fbase, zbase);
    for (int k = 0; k < b->nseg; k++) {
        const seg* sg = &b->segs[k];
        for (int i = 0; i < sg->n; i++) {
            uint32_t j = (uint32_t)(sg->off + i);
            uint8_t* fp = fbase + b->fa[j];
            gs_mem_write(p->fpsm, p->fpage * 32, p->fbw, sg->x0 + i, sg->y,
                         fbits == 16 ? rd16(fp) : rd32(fp) & (fbits == 24 ? 0xFFFFFFu : 0xFFFFFFFFu));
            if (zuse && !p->zmsk) {
                uint8_t* zp = zbase + (j << zf->elem_shift);
                gs_mem_write(p->zpsm, p->zpage * 32, p->zbw, sg->x0 + i, sg->y,
                             zf->bits == 16 ? rd16(zp) : rd32(zp) & (zf->bits == 24 ? 0xFFFFFFu : 0xFFFFFFFFu));
            }
        }
    }
    b->nseg = 0;
    b->npix = 0;
}

/* solid fill: flat colour, no texture/fog/tests/blend/dither, direct addressing */
static void fill_span(const gs_prim* p, int y, int x0, int n, const gs_span_start* st) {
    const gs_fmt* ff = p->ffmt;
    uint32_t oa = clamp255(st->a >> 16) | (uint32_t)p->fba, r = clamp255(st->r >> 16), g = clamp255(st->g >> 16), b = clamp255(st->b >> 16);
    uint32_t nv = ff->bits == 16 ? (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((oa >> 7) << 15)
                                 : r | (g << 8) | (b << 16) | (oa << 24);
    const uint32_t m = p->fmask;
    const uint16_t* row = ff->tab + (((uint32_t)y & ((1u << ff->ph_shift) - 1)) << 6);
    uint32_t prow = p->fpage + ((uint32_t)y >> ff->ph_shift) * p->fbw;
    int es = ff->elem_shift, i = 0, x = x0;
    while (i < n) {
        uint8_t* base = gs_vram + ((((prow + ((uint32_t)x >> 6)) & 511u)) << 13);
        int o = x & 63, run = 64 - o < n - i ? 64 - o : n - i;
        if (ff->bits == 16) {
            for (int k = 0; k < run; k++) {
                uint8_t* fp = base + ((uint32_t)row[o + k] << es);
                wr16(fp, m ? (rd16(fp) & m) | (nv & ~m) : nv);
            }
        } else if (!m) {
            for (int k = 0; k < run; k++) wr32(base + ((uint32_t)row[o + k] << es), nv);
        } else {
            for (int k = 0; k < run; k++) {
                uint8_t* fp = base + ((uint32_t)row[o + k] << es);
                wr32(fp, (rd32(fp) & m) | (nv & ~m));
            }
        }
        i += run; x += run;
    }
}

/* ---- deferred, band-parallel drawing -----------------------------------------------------------------
 * With pixel workers (gs_draw_set_workers), spans are not drawn when they arrive: the drawing thread
 * (the GS thread) records them in a deferred batch and goes on setting up primitives while the
 * workers draw the previous batch.
 *  - A span piece (cut at 64-pixel page columns, exactly where immediate drawing cuts it too, so
 *    the pixels are identical) goes to the bin of the band that owns the 8 KB pages it touches in
 *    the frame and Z buffers in this batch; the first piece to touch a page decides (by its page).
 *    All accesses to a page therefore happen in one bin, in primitive order, whatever buffer,
 *    format or coordinates they come through. A piece whose pages belong to different bands
 *    starts the next batch, as does a full bin or record array (batches are drawn in order).
 *  - Workers (and the drawing thread while it waits) take whole bands and draw their pieces in
 *    order, a run of pieces of one primitive at a time, with the same stage functions as immediate
 *    drawing (each thread has its own batch arrays).
 *  - Primitive state: a snapshot of gs_prim when gs_prim_version has changed, and per primitive only
 *    the steps (prim_rec). Batches run one after another: one is drawn while the next is filled.
 *  - Everything else that touches VRAM (texture decoding, CLUT loads, transfers, the display
 *    readout) or reuses texture cache memory first finishes the deferred drawing it could see
 *    (gs_draw_sync, gs_draw_pages_pending, gs_draw_busy). */

#define NBANDS     32
#define BIN_SEGS   4096                  /* pieces per band per batch */
#define DB_PRIMS   8192
#define DB_STATES  256
#define DB_QUEUE   6                     /* batches: the one being filled + up to 5 queued or drawing */
#define MAX_WORKERS 31
#define WORKER_SPINS 20000               /* pause iterations before an idle worker blocks (~1 ms) */

typedef struct {                         /* what changes with every primitive */
    int32_t dr, dg, db, da, df;
    int64_t dz;
    float ds, dt, dq, lod_q;
    int32_t du, dv;
    uint16_t state;
    uint8_t stq, nowrap;
} prim_rec;

typedef struct {                         /* a piece of a span inside one frame page and one Z page */
    int16_t y, x0, n;
    uint16_t prim;
    gs_span_start st;
} dseg;

typedef struct {
    gs_prim* states;                     /* DB_STATES snapshots */
    prim_rec* prims;                     /* DB_PRIMS */
    dseg* bins;                          /* NBANDS x BIN_SEGS */
    int nbin[NBANDS];
    int nstates, nprims;
    uint32_t serial;
    uint8_t owner[512];                  /* band owning each page in this batch (0xFF: none yet) */
    uint64_t written[8];                 /* pages this batch writes */
    uint64_t readp[8];                   /* pages its primitives sample directly (textures) */
    uint64_t copyp[8];                   /* pages copied to gs_vram_shadow before it starts */
    uint64_t snap_hold[8];               /* shadow pages its primitives sample (must not change) */
    void (*job_fn)(void*);               /* a job batch (gs_draw_queue_job): no primitives */
    void* job_arg;
} dbatch;

#define JOB_BAND 0xFF                    /* the one "band" of a job batch */

typedef struct {                         /* a drawing thread's own state */
    batch b;
    gs_prim p;
    uint32_t serial;                     /* batch and state the snapshot in p came from */
    int state;
} wctx;

/* Batch queue: DB[q_tail % DB_QUEUE] is filled by the drawing thread; the submitted batches
 * q_head .. q_tail - 1 are drawn strictly one after another (the oldest is the job of the pool),
 * so a batch boundary only orders drawing - the drawing thread does not wait for it. */
uint32_t gs_prim_version;
static dbatch DB[DB_QUEUE];
static int db_fill;                      /* batch being filled: q_tail % DB_QUEUE */
static uint32_t q_tail;                  /* batches submitted so far (drawing thread only) */
static _Atomic uint32_t q_head;          /* batches retired so far */
static uint32_t db_serial = 1;
static _Atomic uint32_t db_done;         /* serial of the last completed batch */
static uint32_t snap_version = ~0u;      /* gs_prim_version of the newest snapshot in DB[db_fill] */
static int cur_prim = -1;                /* record of the current primitive in DB[db_fill] */
static const gs_prim* cur_p;
static int cur_stq;
static wctx* own_ctx;                    /* the drawing thread's, for helping */

static struct {
    int n;                               /* worker threads */
    SRWLOCK lock;                        /* starting and retiring jobs */
    dbatch* job;                         /* batch being drawn, or NULL (under lock) */
    uint8_t bands[NBANDS];               /* non-empty bands of the job */
    _Atomic uint64_t claim;              /* job generation << 16 | bands << 8 | next unclaimed */
    _Atomic uint32_t gen;                /* job generation (idle workers wait on it) */
    _Atomic int pending;                 /* bands of the job not finished */
    _Atomic int sleepers;                /* workers blocked in WaitOnAddress */
    uint32_t next_gen;                   /* (under lock) */
    _Atomic uint64_t job_max;            /* longest band of the job (TSC), for the statistics */
} pool;

static int deferred(void) { return pool.n && gs_swz_fast; }
static HANDLE worker_handles[MAX_WORKERS];

uint8_t gs_vram_shadow[GS_VRAM_SIZE];

static void copy_pages(const uint64_t set[8]) {
    for (int k = 0; k < 8; k++)
        for (uint64_t m = set[k]; m; m &= m - 1) {
            size_t pg = (size_t)k * 64 + (size_t)__builtin_ctzll(m);
            memcpy(gs_vram_shadow + pg * 8192, gs_vram + pg * 8192, 8192);
        }
}

/* ---- PS2_GS_STATS=1: what deferred drawing does, once every 5 seconds -------------------------------- */

enum { SUB_SYNC, SUB_KICK, SUB_RAW, SUB_SNAP, SUB_HOLD, SUB_WAR, SUB_BANDS, SUB_FULL, SUB_LIMIT, SUB_JOB, SUB_N };
static struct {
    int on;
    uint64_t t0_ms, tsc0;
    uint32_t batches, bands, prims, why[SUB_N], syncs, queue_waits, sync_why[8];
    uint64_t sync_tsc, queue_tsc;
    _Atomic uint64_t band_tsc, crit_tsc;
} st;

static void stats_report(void) {
    if (!st.on) return;
    uint64_t now = GetTickCount64(), tsc = __rdtsc();
    if (!st.t0_ms) { st.t0_ms = now; st.tsc0 = tsc; return; }
    if (now - st.t0_ms < 5000) return;
    double secs = (double)(now - st.t0_ms) / 1000.0, tsc_ms = (double)(tsc - st.tsc0) / ((double)(now - st.t0_ms));
#define PER_S(v) ((double)(v) / secs)
#define MS_S(t) ((double)(t) / tsc_ms / secs)
    fprintf(stderr, "[gs-stats] %.0f batches/s (%.1f bands, %.0f primitives each); new batch per s: sync %.0f kick %.0f "
                    "read-after-write %.0f snapshot %.0f+%.0f write-after-read %.0f band conflict %.0f bin full %.0f limits %.0f picture %.0f\n",
            PER_S(st.batches), st.batches ? (double)st.bands / st.batches : 0, st.batches ? (double)st.prims / st.batches : 0,
            PER_S(st.why[SUB_SYNC]), PER_S(st.why[SUB_KICK]), PER_S(st.why[SUB_RAW]), PER_S(st.why[SUB_SNAP]),
            PER_S(st.why[SUB_HOLD]), PER_S(st.why[SUB_WAR]), PER_S(st.why[SUB_BANDS]), PER_S(st.why[SUB_FULL]),
            PER_S(st.why[SUB_LIMIT]), PER_S(st.why[SUB_JOB]));
    fprintf(stderr, "[gs-stats] drawing thread waits: queue full %.0f/s %.2f ms/s, syncs %.0f/s %.2f ms/s (display %.0f "
                    "transfer %.0f CLUT %.0f EE %.0f texture cache %.0f palette %.0f other %.0f) | pixel work "
                    "%.1f ms/s on %d workers, critical path (longest band per batch) %.1f ms/s\n",
            PER_S(st.queue_waits), MS_S(st.queue_tsc), PER_S(st.syncs), MS_S(st.sync_tsc),
            PER_S(st.sync_why[GS_SYNC_DISPLAY]), PER_S(st.sync_why[GS_SYNC_XFER]), PER_S(st.sync_why[GS_SYNC_CLUT]),
            PER_S(st.sync_why[GS_SYNC_EE]), PER_S(st.sync_why[GS_SYNC_TEXCACHE]), PER_S(st.sync_why[GS_SYNC_PALETTE]),
            PER_S(st.sync_why[GS_SYNC_OTHER]),
            MS_S(atomic_exchange(&st.band_tsc, 0)), pool.n, MS_S(atomic_exchange(&st.crit_tsc, 0)));
    memset(st.sync_why, 0, sizeof st.sync_why);
#undef PER_S
#undef MS_S
    st.t0_ms = now; st.tsc0 = tsc;
    st.batches = st.bands = st.prims = st.syncs = st.queue_waits = 0;
    st.sync_tsc = st.queue_tsc = 0;
    memset(st.why, 0, sizeof st.why);
}

/* ---- the worker pool ---------------------------------------------------------------------------------- */

static void process_band(wctx* w, const dbatch* d, int band) {
    if (band == JOB_BAND) { d->job_fn(d->job_arg); return; }
    const dseg* s = d->bins + (size_t)band * BIN_SEGS;
    const int n = d->nbin[band];
    int i = 0;
    while (i < n) {
        const int pi = s[i].prim;
        const prim_rec* r = &d->prims[pi];
        if (w->serial != d->serial || w->state != r->state) {
            memcpy(&w->p, &d->states[r->state], sizeof w->p);
            w->serial = d->serial;
            w->state = r->state;
        }
        gs_prim* p = &w->p;
        p->dr = r->dr; p->dg = r->dg; p->db = r->db; p->da = r->da; p->df = r->df; p->dz = r->dz;
        p->ds = r->ds; p->dt = r->dt; p->dq = r->dq; p->lod_q = r->lod_q; p->du = r->du; p->dv = r->dv;
        p->nowrap = r->nowrap;
        batch* b = &w->b;
        b->p = p;
        b->stq = r->stq;
        b->nseg = b->npix = 0;
        while (i < n && s[i].prim == pi && b->nseg < MAX_SEGS && b->npix + s[i].n <= GS_MAX_SPAN) {
            seg* sg = &b->segs[b->nseg++];
            sg->y = s[i].y; sg->x0 = s[i].x0; sg->n = s[i].n; sg->off = b->npix; sg->st = s[i].st;
            b->npix += s[i].n;
            i++;
        }
        run_range(b, 0, b->nseg);
    }
}

static int claim_band(uint32_t g) {
    uint64_t c = atomic_load_explicit(&pool.claim, memory_order_acquire);
    for (;;) {
        if ((uint32_t)(c >> 16) != g) return -1;              /* a later job: this one is over */
        int nb = (int)((c >> 8) & 0xFF), i = (int)(c & 0xFF);
        if (i >= nb) return -1;
        if (atomic_compare_exchange_weak_explicit(&pool.claim, &c, c + 1, memory_order_acq_rel, memory_order_acquire))
            return i;
    }
}

static void retire_job(void);

/* Start drawing batch d, the oldest submitted one (everything before it is drawn). Under pool.lock. */
static void start_job(dbatch* d) {
    int nb;
    for (;;) {
        copy_pages(d->copyp);                    /* snapshots: VRAM as everything before d left it */
        nb = 0;
        if (d->job_fn) pool.bands[nb++] = JOB_BAND;
        else for (int band = 0; band < NBANDS; band++) if (d->nbin[band]) pool.bands[nb++] = (uint8_t)band;
        if (nb) break;
        /* nothing to draw (should not happen: a batch is submitted with pieces): retire it now */
        atomic_store_explicit(&db_done, d->serial, memory_order_release);
        uint32_t h = atomic_load_explicit(&q_head, memory_order_relaxed) + 1;
        atomic_store_explicit(&q_head, h, memory_order_release);
        pool.job = NULL;
        if (h == q_tail) return;
        d = &DB[h % DB_QUEUE];
    }
    st.batches++; st.bands += (uint32_t)nb; st.prims += (uint32_t)d->nprims;
    pool.job = d;
    atomic_store_explicit(&pool.job_max, 0, memory_order_relaxed);
    atomic_store_explicit(&pool.pending, nb, memory_order_relaxed);
    uint32_t g = ++pool.next_gen;
    atomic_store_explicit(&pool.claim, (uint64_t)g << 16 | (uint64_t)nb << 8, memory_order_release);
    atomic_store_explicit(&pool.gen, g, memory_order_seq_cst);
    if (atomic_load(&pool.sleepers)) WakeByAddressAll((PVOID)&pool.gen);
}

/* The job's last band is done: retire it and start the next submitted batch, if any. */
static void retire_job(void) {
    AcquireSRWLockExclusive(&pool.lock);
    atomic_store_explicit(&db_done, pool.job->serial, memory_order_release);
    if (st.on) atomic_fetch_add(&st.crit_tsc, atomic_load(&pool.job_max));
    pool.job = NULL;
    uint32_t h = atomic_load_explicit(&q_head, memory_order_relaxed) + 1;
    atomic_store_explicit(&q_head, h, memory_order_release);
    if (h != q_tail) start_job(&DB[h % DB_QUEUE]);
    ReleaseSRWLockExclusive(&pool.lock);
}

/* Draw bands of job generation g while there are any (the last one to finish retires the job). */
static void run_bands(uint32_t g, wctx* w) {
    int i;
    while ((i = claim_band(g)) >= 0) {
        uint64_t t0 = st.on ? __rdtsc() : 0;
        process_band(w, pool.job, pool.bands[i]);
        if (st.on) {
            uint64_t dt = __rdtsc() - t0, m = atomic_load(&pool.job_max);
            atomic_fetch_add(&st.band_tsc, dt);
            while (dt > m && !atomic_compare_exchange_weak(&pool.job_max, &m, dt)) {}
        }
        if (atomic_fetch_sub_explicit(&pool.pending, 1, memory_order_acq_rel) == 1) retire_job();
    }
}

static DWORD WINAPI pixel_worker(LPVOID arg) {
    wctx* w = arg;
    /* Generation 0: no job yet. All workers are created before the first job is started (nothing is
     * deferred before pool.n is set), but a thread may begin running only after that job was posted:
     * reading pool.gen here would make it skip that job, and with every worker late and the drawing
     * thread idle, nobody would ever draw it (the first VBlank picture then never came). */
    uint32_t seen = 0;
    for (;;) {
        uint32_t g;
        int spins = 0;
        while ((g = atomic_load_explicit(&pool.gen, memory_order_acquire)) == seen) {
            if (++spins < WORKER_SPINS) { _mm_pause(); continue; }
            atomic_fetch_add(&pool.sleepers, 1);
            if (atomic_load(&pool.gen) == seen) WaitOnAddress((volatile VOID*)&pool.gen, &seen, sizeof seen, INFINITE);
            atomic_fetch_sub(&pool.sleepers, 1);
            spins = 0;
        }
        seen = g;
        run_bands(g, w);
    }
    return 0;
}

static wctx* new_ctx(void) {
    wctx* w = VirtualAlloc(NULL, sizeof(wctx), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (w) w->serial = 0;
    return w;
}

static void db_reset(dbatch* d) {
    memset(d->nbin, 0, sizeof d->nbin);
    d->nstates = d->nprims = 0;
    memset(d->owner, 0xFF, sizeof d->owner);
    memset(d->written, 0, sizeof d->written);
    memset(d->readp, 0, sizeof d->readp);
    memset(d->copyp, 0, sizeof d->copyp);
    memset(d->snap_hold, 0, sizeof d->snap_hold);
    d->job_fn = NULL;
    d->serial = ++db_serial;
}

void gs_draw_set_workers(int n) {
    if (pool.n || n <= 0) return;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    for (int k = 0; k < DB_QUEUE; k++) {
        DB[k].states = VirtualAlloc(NULL, DB_STATES * sizeof(gs_prim), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DB[k].prims = VirtualAlloc(NULL, DB_PRIMS * sizeof(prim_rec), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DB[k].bins = VirtualAlloc(NULL, (size_t)NBANDS * BIN_SEGS * sizeof(dseg), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!DB[k].states || !DB[k].prims || !DB[k].bins) return;
    }
    if (!(own_ctx = new_ctx())) return;
    InitializeSRWLock(&pool.lock);
    db_fill = 0;
    db_reset(&DB[0]);
    st.on = getenv("PS2_GS_STATS") != NULL;
    for (int i = 0; i < n; i++) {
        wctx* w = new_ctx();
        HANDLE h = w ? CreateThread(NULL, 0, pixel_worker, w, 0, NULL) : NULL;
        if (!h) break;
        worker_handles[pool.n++] = h;            /* kept for the profiler (PS2_PROFILE=workers) */
    }
}

int gs_draw_workers(void) { return pool.n; }
void* gs_draw_worker_handle(int i) { return i >= 0 && i < pool.n ? worker_handles[i] : NULL; }

/* The drawing thread waits: it draws bands of the current job meanwhile. */
static void help(void) {
    run_bands(atomic_load_explicit(&pool.gen, memory_order_acquire), own_ctx);
    _mm_pause();
}

/* Queue the batch being filled (drawn after the batches before it) and start filling the next. */
static void db_submit(int why) {
    dbatch* d = &DB[db_fill];
    if (!d->nprims && !d->job_fn) return;
    stats_report();
    st.why[why]++;
    if (q_tail + 2 - atomic_load_explicit(&q_head, memory_order_acquire) > DB_QUEUE) {   /* no free batch */
        uint64_t t0 = __rdtsc();
        while (q_tail + 2 - atomic_load_explicit(&q_head, memory_order_acquire) > DB_QUEUE) help();
        st.queue_waits++;
        st.queue_tsc += __rdtsc() - t0;
    }
    AcquireSRWLockExclusive(&pool.lock);
    q_tail++;
    if (!pool.job) start_job(d);                 /* nothing being drawn: everything before d is done */
    ReleaseSRWLockExclusive(&pool.lock);
    db_fill = (int)(q_tail % DB_QUEUE);
    db_reset(&DB[db_fill]);
    snap_version = ~0u;
    cur_prim = -1;
}

void gs_draw_sync_why(int why) {
    if (!deferred()) return;
    if (!DB[db_fill].nprims && !DB[db_fill].job_fn && atomic_load_explicit(&q_head, memory_order_acquire) == q_tail) return;
    uint64_t t0 = __rdtsc();
    db_submit(SUB_SYNC);
    while (atomic_load_explicit(&q_head, memory_order_acquire) != q_tail) help();
    st.syncs++;
    st.sync_why[why >= 0 && why < 8 ? why : 0]++;
    st.sync_tsc += __rdtsc() - t0;
}

void gs_draw_sync(void) { gs_draw_sync_why(GS_SYNC_OTHER); }

void gs_draw_queue_job(void (*fn)(void* arg), void* arg, const uint64_t reads[8]) {
    if (!deferred()) { fn(arg); return; }
    db_submit(SUB_JOB);                          /* what was recorded before it */
    dbatch* d = &DB[db_fill];
    d->job_fn = fn;
    d->job_arg = arg;
    memcpy(d->readp, reads, sizeof d->readp);
    db_submit(SUB_JOB);
}

int gs_draw_pages_touched(const uint64_t set[8], int writes_only) {
    if (!deferred()) return 0;
    for (uint32_t b = atomic_load_explicit(&q_head, memory_order_acquire); b != q_tail + 1; b++) {
        const dbatch* d = &DB[b % DB_QUEUE];     /* the queued batches and the one being filled */
        if (!d->nprims && !d->job_fn) continue;
        for (int k = 0; k < 8; k++)
            if (set[k] & (d->written[k] | (writes_only ? 0 : d->readp[k] | d->copyp[k]))) return 1;
    }
    return 0;
}

void gs_draw_kick(void) {
    if (deferred() && DB[db_fill].nprims) db_submit(SUB_KICK);
}

int gs_draw_busy(uint32_t serial) {
    return deferred() && serial > atomic_load_explicit(&db_done, memory_order_acquire);
}

uint32_t gs_draw_fill_serial(void) { return DB[db_fill].serial; }

int gs_draw_pages_pending(uint32_t page0, uint32_t n) {
    if (!deferred()) return 0;
    if (n > 512) n = 512;
    for (uint32_t b = atomic_load_explicit(&q_head, memory_order_acquire); b != q_tail + 1; b++) {
        const dbatch* d = &DB[b % DB_QUEUE];     /* the queued batches and the one being filled */
        if (!d->nprims) continue;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t pg = (page0 + i) & 511u;
            if ((d->written[pg >> 6] >> (pg & 63)) & 1) return 1;
        }
    }
    return 0;
}

/* Pages the current primitive samples directly / from the snapshot (gs_draw_set_reads; cleared by
 * gs_batch_end), and whether its snapshot has been arranged (only once: if its spans continue in
 * the next batch, the shadow must keep the pages as they were before it started). */
static uint64_t cur_reads[8], cur_snap[8];
static int cur_has_reads, cur_has_snap, cur_snap_done;

void gs_draw_set_reads(const uint64_t reads[8], const uint64_t snap[8]) {
    cur_has_reads = cur_has_snap = cur_snap_done = 0;
    if (snap && !deferred()) { copy_pages(snap); snap = NULL; }   /* everything before it is drawn */
    if (!deferred()) return;
    for (int k = 0; reads && k < 8; k++) { cur_reads[k] = reads[k]; cur_has_reads |= reads[k] != 0; }
    for (int k = 0; snap && k < 8; k++) { cur_snap[k] = snap[k]; cur_has_snap |= snap[k] != 0; }
}

/* A record for the current primitive in the batch being filled (and its state snapshot). */
static void register_prim(void) {
    dbatch* d = &DB[db_fill];
    int need_state = gs_prim_version != snap_version || !d->nstates;
    /* it samples pages this batch writes, or needs a snapshot of pages whose shadow copy another
     * primitive of this batch still samples as it was before: next batch */
    int why = -1;
    for (int k = 0; k < 8; k++) {
        if (cur_has_reads && (cur_reads[k] & d->written[k]) != 0) why = SUB_RAW;
        if (cur_has_snap && (cur_snap[k] & d->written[k]) != 0) why = SUB_SNAP;
        if (cur_has_snap && !cur_snap_done && (cur_snap[k] & d->snap_hold[k] & ~d->copyp[k]) != 0) why = SUB_HOLD;
    }
    if (why < 0 && (d->nprims == DB_PRIMS || (need_state && d->nstates == DB_STATES))) why = SUB_LIMIT;
    if (why >= 0) {
        db_submit(why);
        d = &DB[db_fill];
        need_state = 1;
    }
    if (cur_has_reads)
        for (int k = 0; k < 8; k++) d->readp[k] |= cur_reads[k];
    if (cur_has_snap) {
        for (int k = 0; k < 8; k++) {
            if (!cur_snap_done) d->copyp[k] |= cur_snap[k];
            d->snap_hold[k] |= cur_snap[k];
        }
        cur_snap_done = 1;
    }
    if (need_state) {
        memcpy(&d->states[d->nstates++], cur_p, sizeof(gs_prim));
        snap_version = gs_prim_version;
    }
    const gs_prim* p = cur_p;
    prim_rec* r = &d->prims[d->nprims];
    r->dr = p->dr; r->dg = p->dg; r->db = p->db; r->da = p->da; r->df = p->df; r->dz = p->dz;
    r->ds = p->ds; r->dt = p->dt; r->dq = p->dq; r->lod_q = p->lod_q; r->du = p->du; r->dv = p->dv;
    r->state = (uint16_t)(d->nstates - 1);
    r->stq = (uint8_t)cur_stq;
    r->nowrap = (uint8_t)p->nowrap;
    cur_prim = d->nprims++;
    if (p->tme && p->texm) {                     /* its textures must stay as they are until drawn */
        int lo = p->texm == 3 ? 0 : p->cur_level, hi = p->texm == 3 ? p->mxl : p->cur_level;
        for (int l = lo; l <= hi; l++) gs_tex_note_use(&p->lvl[l], d->serial);
    }
}

#define HAS(set, pg) (((set)[(pg) >> 6] >> ((pg) & 63)) & 1)

/* The band for a piece of row y, [x0, x0 + n): the band that owns its frame and Z pages in this batch
 * (claiming the unowned ones for it), -1 if they belong to different bands, -2 if it writes a page
 * that a primitive of this batch samples directly (that one may run later, in another band). A piece
 * on new pages goes to the band of its page (row group and 64-pixel column), so a frame is spread
 * over as many bands as it has pages (up to NBANDS). */
static int assign_band(dbatch* d, const gs_prim* p, int y, int x0, int n, int check_reads) {
    const uint32_t c0 = (uint32_t)x0 >> 6, c1 = (uint32_t)(x0 + n - 1) >> 6;
    const uint32_t frow = p->fpage + ((uint32_t)y >> p->ffmt->ph_shift) * p->fbw;
    const uint32_t zrow = p->zuse ? p->zpage + ((uint32_t)y >> p->zfmt->ph_shift) * p->zbw : 0;
    const int fw = p->fbmsk != 0xFFFFFFFFu, zw = p->zuse && !p->zmsk;
    int band = 0xFF;
    for (uint32_t c = c0; c <= c1; c++) {
        int o = d->owner[(frow + c) & 511u];
        if (o != 0xFF) { if (band == 0xFF) band = o; else if (o != band) return -1; }
        if (check_reads && fw && HAS(d->readp, (frow + c) & 511u)) return -2;
        if (p->zuse) {
            o = d->owner[(zrow + c) & 511u];
            if (o != 0xFF) { if (band == 0xFF) band = o; else if (o != band) return -1; }
            if (check_reads && zw && HAS(d->readp, (zrow + c) & 511u)) return -2;
        }
    }
    if (band == 0xFF) {                          /* a new page: its own band, by row group and column */
        int sh = p->zuse && p->zfmt->ph_shift > p->ffmt->ph_shift ? p->zfmt->ph_shift : p->ffmt->ph_shift;
        band = (int)((((uint32_t)y >> sh) * 16u + c0) % NBANDS);   /* (a Z page spans whole row groups) */
    }
    for (uint32_t c = c0; c <= c1; c++) {
        uint32_t pg = (frow + c) & 511u;
        d->owner[pg] = (uint8_t)band;
        if (fw) d->written[pg >> 6] |= 1ull << (pg & 63);
        if (p->zuse) {
            pg = (zrow + c) & 511u;
            d->owner[pg] = (uint8_t)band;
            if (zw) d->written[pg >> 6] |= 1ull << (pg & 63);
        }
    }
    return band;
}

/* Spans are cut exactly where immediate drawing (B0) cuts them, so the interpolants of every
 * piece start from the same values and the pixels come out identical. Both cut at every 64-pixel
 * column (the width of a page in every format a frame or Z buffer can have), so a piece touches
 * one frame page and one Z page, and the pages of a buffer row can go to different bands. */
static int vnseg, vnpix;                 /* B0's fill for the current primitive */

GS_AI int piece_len(int x0, int n, int room) {   /* up to the batch room and the next page column */
    int col = 64 - (x0 & 63);
    int t = room < n ? room : n;
    return col < t ? col : t;
}

static void deferred_span(int y, int x0, int n, const gs_span_start* st) {
    const gs_prim* p = cur_p;
    gs_span_start s = *st;
    while (n > 0) {
        if (vnseg == MAX_SEGS || vnpix == GS_MAX_SPAN) vnseg = vnpix = 0;
        int take = piece_len(x0, n, GS_MAX_SPAN - vnpix);
        vnseg++;
        vnpix += take;
        for (;;) {
            if (cur_prim < 0) register_prim();
            dbatch* d = &DB[db_fill];
            /* the reads of a primitive alone in its batch are its own (and never its destination,
             * gs_draw.c samples such feedback from the cache): no need to look */
            int band = assign_band(d, p, y, x0, take, d->nprims > 1);
            /* pages in two bands, or a write after a read: this piece starts the next batch, which
             * is drawn after this one (no need to wait here) */
            if (band < 0) { db_submit(band == -1 ? SUB_BANDS : SUB_WAR); continue; }
            if (d->nbin[band] == BIN_SEGS) { db_submit(SUB_FULL); continue; }
            dseg* g = &d->bins[(size_t)band * BIN_SEGS + (size_t)d->nbin[band]++];
            g->y = (int16_t)y; g->x0 = (int16_t)x0; g->n = (int16_t)take; g->prim = (uint16_t)cur_prim; g->st = s;
            break;
        }
        n -= take;
        if (n) {                         /* split: advance the interpolants as B0 does */
            x0 += take;
            s.r += p->dr * take; s.g += p->dg * take; s.b += p->db * take; s.a += p->da * take;
            s.f += p->df * take; s.z += p->dz * take; s.u += p->du * take; s.v += p->dv * take;
            s.s += p->ds * (float)take; s.t += p->dt * (float)take; s.q += p->dq * (float)take;
        }
    }
}

/* ---- span input ------------------------------------------------------------------------------------- */

void gs_batch_begin(const gs_prim* p, int stq) {
    if (deferred()) {
        cur_p = p;
        cur_stq = stq;
        cur_prim = -1;                   /* registered with its first span */
        vnseg = vnpix = 0;
        return;
    }
    batch* b = &B0;
    b->p = p;
    b->stq = stq;
    b->nseg = b->npix = 0;
}

void gs_batch_span(int y, int x0, int n, const gs_span_start* st) {
    gs_stats.pixels += (uint32_t)n;
    if (deferred()) { deferred_span(y, x0, n, st); return; }
    batch* b = &B0;
    const gs_prim* p = b->p;
    if (p->solid && gs_swz_fast) { fill_span(p, y, x0, n, st); return; }
    gs_span_start s = *st;
    while (n > 0) {
        if (b->nseg == MAX_SEGS || b->npix == GS_MAX_SPAN) flush(b);
        int take = piece_len(x0, n, GS_MAX_SPAN - b->npix);   /* (cut as deferred drawing cuts) */
        seg* sg = &b->segs[b->nseg++];
        sg->y = y; sg->x0 = x0; sg->n = take; sg->off = b->npix; sg->st = s;
        b->npix += take;
        n -= take;
        if (n) {                         /* split: advance the interpolants */
            x0 += take;
            s.r += p->dr * take; s.g += p->dg * take; s.b += p->db * take; s.a += p->da * take;
            s.f += p->df * take; s.z += p->dz * take; s.u += p->du * take; s.v += p->dv * take;
            s.s += p->ds * (float)take; s.t += p->dt * (float)take; s.q += p->dq * (float)take;
        }
    }
}

void gs_batch_end(void) {
    cur_has_reads = cur_has_snap = 0;
    if (deferred()) { cur_prim = -1; return; }
    flush(&B0);
}

void gs_prim_finalize(gs_prim* p) {
    /* alpha test table: ATST 0 NEVER 1 ALWAYS 2 LESS 3 LEQUAL 4 EQUAL 5 GEQUAL 6 GREATER 7 NOTEQUAL */
    if (p->ate && (p->atst != p->apass_atst || p->aref != p->apass_aref)) {
        for (int a = 0; a < 256; a++) {
            int ref = p->aref, pass;
            switch (p->atst) {
            case 0: pass = 0; break;
            case 1: pass = 1; break;
            case 2: pass = a < ref; break;
            case 3: pass = a <= ref; break;
            case 4: pass = a == ref; break;
            case 5: pass = a >= ref; break;
            case 6: pass = a > ref; break;
            default: pass = a != ref; break;
            }
            p->apass[a] = (uint8_t)pass;
        }
        p->apass_atst = p->atst;
        p->apass_aref = p->aref;
    }
    if (p->ate && p->atst == 1) p->ate = 0;        /* ALWAYS: no test */
    /* write masks in the stored format */
    if (p->ffmt->bits == 16) {
        uint32_t m = p->fbmsk;
        p->fmask = ((m >> 3) & 0x1F) | (((m >> 11) & 0x1F) << 5) | (((m >> 19) & 0x1F) << 10) | (((m >> 31) & 1) << 15);
        p->amask = 0x8000;
    } else {
        p->fmask = p->fbmsk | (p->ffmt->bits == 24 ? 0xFF000000u : 0);
        p->amask = 0xFF000000u;
    }
    if (p->ffmt->bits == 24) p->date = 0;          /* no destination alpha */
    p->std_blend = p->asel == 0 && p->bsel == 1 && p->csel == 0 && p->dsel == 1;
    p->solid = !p->tme && !p->iip && !p->fge && !p->ate && !p->date && !p->zuse && !p->abe &&
               !(p->ffmt->bits == 16 && p->dthe);
}
