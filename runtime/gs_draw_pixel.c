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
 * running stage by stage gives the same result as pixel by pixel. */
#include "gs_draw.h"

#include <emmintrin.h>
#include <math.h>

#define MAX_SEGS 512

typedef struct {
    int y, x0, n, off;                   /* off: index of the first pixel in the batch arrays */
    gs_span_start st;
} seg;

static struct {
    const gs_prim* p;
    int stq;
    int nseg, npix;
    seg segs[MAX_SEGS];
    uint32_t col[GS_MAX_SPAN];           /* fragment colour 0xAABBGGRR */
    uint32_t tex[GS_MAX_SPAN];           /* texel colour */
    uint32_t fa[GS_MAX_SPAN];            /* frame buffer byte offsets from fbase */
    uint8_t wm[GS_MAX_SPAN];             /* write mode: 0 none, 1 RGBA, 2 RGB only */
    uint8_t ftmp[GS_MAX_SPAN * 4], ztmp[GS_MAX_SPAN * 4];   /* slow path: pixels via gs_mem */
} B;

GS_AI int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
GS_AI uint32_t clamp255(int v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
GS_AI int wrapc(int c, int lo, int hi, int msk, int fix) { c = c < lo ? lo : c > hi ? hi : c; return (c & msk) | fix; }
GS_AI uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
GS_AI uint32_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
GS_AI void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
GS_AI void wr16(uint8_t* p, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); }

GS_AI int f2i(float x) {                 /* saturating, NaN -> large negative */
    if (!(x > -1e8f)) return -100000000;
    if (x > 1e8f) return 100000000;
    return (int)x;
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

/* Bilinear filter with 4-bit weights: sum(texel * wu * wv) >> 8, exactly; SSE2 16-bit lanes. */
GS_AI uint32_t tex_bilinear(const lvl_view* L, int U, int V) {
    U -= 8; V -= 8;                      /* texel centres are at +0.5 */
    int fu = U & 15, fv = V & 15;
    int u0 = U >> 4, v0 = V >> 4;
    int xa = wrapc(u0, L->ulo, L->uhi, L->umsk, L->ufix), xb = wrapc(u0 + 1, L->ulo, L->uhi, L->umsk, L->ufix);
    int ya = wrapc(v0, L->vlo, L->vhi, L->vmsk, L->vfix) << L->sh;
    int yb = wrapc(v0 + 1, L->vlo, L->vhi, L->vmsk, L->vfix) << L->sh;
    const __m128i zero = _mm_setzero_si128();
    __m128i top = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)L->tex[ya + xa]),
                                                       _mm_cvtsi32_si128((int)L->tex[ya + xb])), zero);
    __m128i bot = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)L->tex[yb + xa]),
                                                       _mm_cvtsi32_si128((int)L->tex[yb + xb])), zero);
    /* vertical: top * (16 - fv) + bot * fv  (<= 4080) */
    __m128i v = _mm_add_epi16(_mm_slli_epi16(top, 4), _mm_mullo_epi16(_mm_sub_epi16(bot, top), _mm_set1_epi16((short)fv)));
    /* horizontal: left * (16 - fu) + right * fu  (<= 65280, unsigned 16-bit) */
    __m128i h = _mm_mullo_epi16(v, _mm_unpacklo_epi64(_mm_set1_epi16((short)(16 - fu)), _mm_set1_epi16((short)fu)));
    h = _mm_add_epi16(h, _mm_srli_si128(h, 8));
    h = _mm_srli_epi16(h, 8);
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(h, h));
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
GS_AI void sample_batch(const gs_prim* p, const int STQ, const int FILT) {
    const lvl_view L = view_of(&p->lvl[p->cur_level]);
    const int cl = p->cur_level;
    const float ds = p->ds, dt = p->dt, dq = p->dq, tw16 = p->tw16, th16 = p->th16, lq = p->lod_q;
    const int32_t du = p->du, dv = p->dv;
    for (int k = 0; k < B.nseg; k++) {
        const seg* sg = &B.segs[k];
        uint32_t* out = B.tex + sg->off;
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

static void sample_uv_point(const gs_prim* p) { sample_batch(p, 0, 1); }
static void sample_uv_bilin(const gs_prim* p) { sample_batch(p, 0, 2); }
static void sample_uv_gen(const gs_prim* p) { sample_batch(p, 0, 3); }
static void sample_uv_point_nw(const gs_prim* p) { sample_batch(p, 0, 4); }
static void sample_uv_bilin_nw(const gs_prim* p) { sample_batch(p, 0, 5); }
static void sample_stq_point(const gs_prim* p) { sample_batch(p, 1, 1); }
static void sample_stq_bilin(const gs_prim* p) { sample_batch(p, 1, 2); }
static void sample_stq_gen(const gs_prim* p) { sample_batch(p, 1, 3); }
static void sample_stq_point_nw(const gs_prim* p) { sample_batch(p, 1, 4); }
static void sample_stq_bilin_nw(const gs_prim* p) { sample_batch(p, 1, 5); }

typedef void (*sample_fn)(const gs_prim*);
static const sample_fn sample_table[2][2][4] = {           /* [stq][nowrap][texm] */
    { { NULL, sample_uv_point, sample_uv_bilin, sample_uv_gen },
      { NULL, sample_uv_point_nw, sample_uv_bilin_nw, sample_uv_gen } },
    { { NULL, sample_stq_point, sample_stq_bilin, sample_stq_gen },
      { NULL, sample_stq_point_nw, sample_stq_bilin_nw, sample_stq_gen } },
};

/* ---- colour stages -------------------------------------------------------------------------------- */

static void colour_batch(const gs_prim* p) {
    if (!p->iip) {
        const gs_span_start* st = &B.segs[0].st;
        uint32_t c = clamp255(st->r >> 16) | clamp255(st->g >> 16) << 8 | clamp255(st->b >> 16) << 16 | clamp255(st->a >> 16) << 24;
        for (int i = 0; i < B.npix; i++) B.col[i] = c;
        return;
    }
    /* r, g, b, a in 4 lanes: >> 16, then saturating packs clamp to 0..255 exactly like clamp255 */
    const __m128i step = _mm_set_epi32(p->da, p->db, p->dg, p->dr);
    for (int k = 0; k < B.nseg; k++) {
        const seg* sg = &B.segs[k];
        uint32_t* col = B.col + sg->off;
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

static void fog_batch(const gs_prim* p) {
    const int32_t df = p->df;
    const int fr = p->fogr, fg = p->fogg, fb = p->fogb;
    for (int k = 0; k < B.nseg; k++) {
        const seg* sg = &B.segs[k];
        uint32_t* col = B.col + sg->off;
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
GS_AI int tests_batch(const gs_prim* p, uint8_t* fbase, uint8_t* zbase, const int ZBITS) {
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
    for (int k = 0; k < B.nseg; k++) {
        const seg* sg = &B.segs[k];
        const int y = sg->y, x0 = sg->x0, n = sg->n, off = sg->off;
        const uint32_t* col = B.col + off;
        const uint32_t* fa = B.fa + off;
        uint8_t* wm = B.wm + off;
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

static int tests_z0(const gs_prim* p, uint8_t* f, uint8_t* z) { return tests_batch(p, f, z, 0); }
static int tests_z16(const gs_prim* p, uint8_t* f, uint8_t* z) { return tests_batch(p, f, z, 16); }
static int tests_z24(const gs_prim* p, uint8_t* f, uint8_t* z) { return tests_batch(p, f, z, 24); }
static int tests_z32(const gs_prim* p, uint8_t* f, uint8_t* z) { return tests_batch(p, f, z, 32); }

/* ---- blend + frame buffer write ------------------------------------------------------------------ */

GS_AI int sel3(int s, int src, int dst) { return s == 0 ? src : s == 1 ? dst : 0; }

/* One segment. BL: 0 none, 1 (Cs - Cd) * As + Cd, 2 generic. FB: 32, 24 or 16. WM: write modes. */
GS_AI void write_seg(const gs_prim* p, const seg* sg, uint8_t* fbase, const int FB, const int BL, const int WM) {
    const uint32_t* col = B.col + sg->off;
    const uint32_t* fa = B.fa + sg->off;
    const uint8_t* wm = B.wm + sg->off;
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

GS_AI void write_batch(const gs_prim* p, uint8_t* fbase, const int FB, const int BL, const int WM) {
    for (int k = 0; k < B.nseg; k++) write_seg(p, &B.segs[k], fbase, FB, BL, WM);
}

#define WRITE_FN(F, BL, W) static void write_##F##_##BL##_##W(const gs_prim* p, uint8_t* fb) { write_batch(p, fb, F, BL, W); }
#define WRITE_FNS(F) WRITE_FN(F, 0, 0) WRITE_FN(F, 0, 1) WRITE_FN(F, 1, 0) WRITE_FN(F, 1, 1) WRITE_FN(F, 2, 0) WRITE_FN(F, 2, 1)
WRITE_FNS(32) WRITE_FNS(24) WRITE_FNS(16)
typedef void (*write_fn)(const gs_prim*, uint8_t*);
#define WRITE_ROW(F) { { write_##F##_0_0, write_##F##_0_1 }, { write_##F##_1_0, write_##F##_1_1 }, { write_##F##_2_0, write_##F##_2_1 } }
static const write_fn write_table[3][3][2] = { WRITE_ROW(32), WRITE_ROW(24), WRITE_ROW(16) };

/* ---- batch driver --------------------------------------------------------------------------------- */

static void flush(void) {
    const gs_prim* p = B.p;
    if (!B.npix) return;
    const gs_fmt* ff = p->ffmt;
    const gs_fmt* zf = p->zfmt;
    const int fbits = ff->bits, fes = ff->elem_shift, fast = gs_swz_fast, zuse = p->zuse;
    uint8_t* fbase = gs_vram;
    uint8_t* zbase = gs_vram;

    if (fast) {
        for (int k = 0; k < B.nseg; k++)
            row_addrs(ff, p->fpage, p->fbw, B.segs[k].x0, B.segs[k].y, B.segs[k].n, B.fa + B.segs[k].off);
    } else {                             /* gather through gs_mem; scattered back below */
        fbase = B.ftmp;
        zbase = B.ztmp;
        for (int k = 0; k < B.nseg; k++) {
            const seg* sg = &B.segs[k];
            for (int i = 0; i < sg->n; i++) {
                uint32_t j = (uint32_t)(sg->off + i);
                B.fa[j] = j << fes;
                uint32_t v = gs_mem_read(p->fpsm, p->fpage * 32, p->fbw, sg->x0 + i, sg->y);
                if (fbits == 16) wr16(fbase + B.fa[j], v); else wr32(fbase + B.fa[j], v);
                if (zuse) {
                    v = gs_mem_read(p->zpsm, p->zpage * 32, p->zbw, sg->x0 + i, sg->y);
                    if (zf->bits == 16) wr16(zbase + (j << zf->elem_shift), v); else wr32(zbase + (j << zf->elem_shift), v);
                }
            }
        }
    }

    colour_batch(p);
    if (p->tme && p->texm) {
        sample_table[B.stq][p->nowrap][p->texm](p);
        tfx_batch(p->tfx, p->tcc, B.npix, B.col, B.tex);
    }
    if (p->fge) fog_batch(p);

    int wmode = 0;
    if (p->ate || p->date || zuse) {
        int left;
        switch (zuse ? zf->bits : 0) {
        case 16: left = tests_z16(p, fbase, zbase); break;
        case 24: left = tests_z24(p, fbase, zbase); break;
        case 32: left = tests_z32(p, fbase, zbase); break;
        default: left = tests_z0(p, fbase, zbase); break;
        }
        wmode = 1;
        if (!left) goto done;
    }
    write_table[fbits == 32 ? 0 : fbits == 24 ? 1 : 2][!p->abe ? 0 : p->std_blend ? 1 : 2][wmode](p, fbase);

done:
    if (!fast) {
        for (int k = 0; k < B.nseg; k++) {
            const seg* sg = &B.segs[k];
            for (int i = 0; i < sg->n; i++) {
                uint32_t j = (uint32_t)(sg->off + i);
                uint8_t* fp = fbase + B.fa[j];
                gs_mem_write(p->fpsm, p->fpage * 32, p->fbw, sg->x0 + i, sg->y,
                             fbits == 16 ? rd16(fp) : rd32(fp) & (fbits == 24 ? 0xFFFFFFu : 0xFFFFFFFFu));
                if (zuse && !p->zmsk) {
                    uint8_t* zp = zbase + (j << zf->elem_shift);
                    gs_mem_write(p->zpsm, p->zpage * 32, p->zbw, sg->x0 + i, sg->y,
                                 zf->bits == 16 ? rd16(zp) : rd32(zp) & (zf->bits == 24 ? 0xFFFFFFu : 0xFFFFFFFFu));
                }
            }
        }
    }
    B.nseg = 0;
    B.npix = 0;
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

void gs_batch_begin(const gs_prim* p, int stq) {
    B.p = p;
    B.stq = stq;
    B.nseg = B.npix = 0;
}

void gs_batch_span(int y, int x0, int n, const gs_span_start* st) {
    const gs_prim* p = B.p;
    if (p->solid && gs_swz_fast) { fill_span(p, y, x0, n, st); return; }
    gs_span_start s = *st;
    while (n > 0) {
        if (B.nseg == MAX_SEGS || B.npix == GS_MAX_SPAN) flush();
        int take = GS_MAX_SPAN - B.npix < n ? GS_MAX_SPAN - B.npix : n;
        seg* sg = &B.segs[B.nseg++];
        sg->y = y; sg->x0 = x0; sg->n = take; sg->off = B.npix; sg->st = s;
        B.npix += take;
        n -= take;
        if (n) {                         /* split: advance the interpolants */
            x0 += take;
            s.r += p->dr * take; s.g += p->dg * take; s.b += p->db * take; s.a += p->da * take;
            s.f += p->df * take; s.z += p->dz * take; s.u += p->du * take; s.v += p->dv * take;
            s.s += p->ds * (float)take; s.t += p->dt * (float)take; s.q += p->dq * (float)take;
        }
    }
}

void gs_batch_end(void) { flush(); }

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
