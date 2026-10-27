/* (r2-m3, R4) The cel-outline VU1 program (188054331, notes/wip/r2-m3.md "Findings") in C: its routines run on an image
 * of VU1 data memory and append the GIF packets the VU would XGKICK, bit for bit (the level-1 oracle: scripts/
 * r2_mesh_check.c replays recorded outline programs through both). The GS thread then decodes those packets as it would
 * the VU's (gs_draw.c, R2H_OUTLINE), so outlines look exactly like the exact path's in both outline settings, their Z
 * ties with their meshes are the exact path's, and the VIF unpack + VU1 work of the outline entries goes away.
 *
 *   0x0738  set-up (X = 0x24B - TOP, kept in vi04 for the batches): [10].x & 1: 0x0060 (W x C into 4..7 by 0x0868; the
 *           outline colour [15] = ITOF0([15]) lerped toward (160, 0, 0) by [8].z, alpha x 0.6 [8].y when [8].y < 1);
 *           & 2: 0x0120 (0..9 and 0xD..[10].y from the other buffer); XGKICK X + 0xD (ALPHA + RGBAQ)
 *   0x07F0  a batch: [10].z: 0x01E0 (4-bone skinning of the V3-32 positions [11].x..[11].y, weights at [10].z, bones
 *           [12]); 0x03C0 (positions x (W x C), / w, in place); 0x0500 (edges [11].z..[11].w: {a, v0, v1, b} offsets
 *           from [11].x; skipped when a vertex is outside |.| <= 1; a silhouette when the two screen areas differ in
 *           sign; v0, v1 pushed out by 0.00024; screen transform VU 0 / VU 1; one 8-qword packet per edge, alternating
 *           X + 0x12 / X + 0x1A: an AA1 line (TEST RGB only), then the same line Z only).
 * Every operation is the VU's (round toward zero, flush to zero) in the microcode's order, as runtime/vu_ops.h. */
#include "r2.h"

#include <math.h>
#include <string.h>
#include <xmmintrin.h>

#pragma STDC FENV_ACCESS ON
#pragma clang fp contract(off)

typedef union { float f[4]; uint32_t u[4]; } q4;

static inline float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float clean(uint32_t b) { if ((b & 0x7F800000u) == 0x7F800000u) b = (b & 0x80000000u) | 0x7F7FFFFFu; return bf(b); }
static inline float m_(float a, float b) { return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float a_(float a, float b) { return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float s_(float a, float b) { return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline uint32_t res(float r) {            /* a host result as PS2 bits (no Inf / NaN, no denormals) */
    uint32_t b = fb(r);
    if ((b & 0x7F800000u) == 0x7F800000u) b = (b & 0x80000000u) | 0x7F7FFFFFu;
    if ((b & 0x7F800000u) == 0) b &= 0x80000000u;
    return b;
}
static inline float divq(float s, float t) {     /* DIV: x / 0 -> +-0x7FFFFFFF bits (cleaned when read: +-FLT_MAX) */
    if (t == 0.0f) return bf(((fb(s) ^ fb(t)) & 0x80000000u) | 0x7F7FFFFFu);
    return bf(res(_mm_cvtss_f32(_mm_div_ss(_mm_set_ss(s), _mm_set_ss(t)))));
}

typedef struct {
    uint32_t (*vu)[4];
    uint8_t* out;
    uint32_t nout, cap, overflow;
} st_t;

static q4 ld(st_t* s, uint32_t a) { q4 v; memcpy(v.u, s->vu[a & 0x3FF], 16); for (int i = 0; i < 4; i++) v.f[i] = clean(v.u[i]); return v; }
static q4 ldraw(st_t* s, uint32_t a) { q4 v; memcpy(v.u, s->vu[a & 0x3FF], 16); return v; }
static void st_(st_t* s, uint32_t a, const q4* v, unsigned lanes) {
    for (int i = 0; i < 4; i++) if (lanes & (1u << i)) s->vu[a & 0x3FF][i] = v->u[i];
}

/* XGKICK: the packet at a (tags until EOP; PACKED / REGLIST / IMAGE sizes as gif.c) appended to the output */
static void kick(st_t* s, uint32_t a) {
    for (int guard = 0; guard < 64; guard++) {
        const uint32_t* t = s->vu[a & 0x3FF];
        uint64_t lo = (uint64_t)t[0] | (uint64_t)t[1] << 32;
        uint32_t nloop = (uint32_t)(lo & 0x7FFF), eop = (uint32_t)((lo >> 15) & 1), flg = (uint32_t)((lo >> 58) & 3), nreg = (uint32_t)(lo >> 60);
        if (!nreg) nreg = 16;
        uint32_t len = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        if (s->nout + len * 16 > s->cap) { s->overflow = 1; return; }
        for (uint32_t i = 0; i < len; i++) memcpy(s->out + s->nout + i * 16, s->vu[(a + i) & 0x3FF], 16);
        s->nout += len * 16;
        a = (a + len) & 0x3FF;
        if (eop) break;
    }
}

/* 0x0868: out[i] = M (rows from VU 2..5) x row i of the matrix at w (MULAx / MADDAy / MADDAz / MADDw) */
static void matmul(st_t* s, uint32_t c, uint32_t w, uint32_t out) {
    q4 C[4], o[4];
    for (int k = 0; k < 4; k++) C[k] = ld(s, c + (uint32_t)k);
    for (int i = 0; i < 4; i++) {
        q4 W = ld(s, w + (uint32_t)i);
        for (int j = 0; j < 4; j++) {
            float acc = m_(C[0].f[j], W.f[0]);
            acc = a_(acc, m_(C[1].f[j], W.f[1]));
            acc = a_(acc, m_(C[2].f[j], W.f[2]));
            o[i].u[j] = res(a_(acc, m_(C[3].f[j], W.f[3])));
        }
    }
    for (int i = 0; i < 4; i++) st_(s, out + (uint32_t)i, &o[i], 15);
}

static void setup(st_t* s, uint32_t X) {
    uint32_t fl = s->vu[(X + 10) & 0x3FF][0] & 0xFFFF;
    if (fl & 1) {                                        /* 0x0060 */
        q4 v3 = ld(s, X + 8), raw = ldraw(s, X + 15), v4, v5;
        matmul(s, 2, X, X + 4);
        for (int i = 0; i < 4; i++) v4.f[i] = (float)(int32_t)raw.u[i];       /* ITOF0 */
        int sflag = (fb(s_(v3.f[1], 1.0f)) >> 31) != 0;   /* SUBw.y vf00 (the S flag, read by FSAND 4 pairs on) */
        v5.f[0] = a_(0.0f, 160.0f); v5.f[1] = s_(0.0f, 0.0f); v5.f[2] = s_(0.0f, 0.0f);
        v3.f[1] = m_(v3.f[1], bf(0x3F19999Au));         /* MULi.y x 0.6 */
        for (int i = 0; i < 3; i++) v5.f[i] = s_(v5.f[i], v4.f[i]);
        for (int i = 0; i < 3; i++) v4.f[i] = a_(a_(0.0f, v4.f[i]), m_(v5.f[i], v3.f[2]));   /* ADDA + MADDz */
        if (sflag) v4.f[3] = m_(v4.f[3], v3.f[1]);
        q4 o;
        for (int i = 0; i < 4; i++) {
            float f = v4.f[i];
            o.u[i] = f >= 2147483648.0f ? 0x7FFFFFFFu : f < -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)f;   /* FTOI0 */
        }
        st_(s, X + 15, &o, 15);
    }
    fl = s->vu[(X + 10) & 0x3FF][0] & 0xFFFF;
    if (fl & 2) {                                        /* 0x0120 */
        uint32_t Y = (0x24B - X) & 0x3FF;
        for (uint32_t i = 0; i < 10; i++) memcpy(s->vu[(X + i) & 0x3FF], s->vu[(Y + i) & 0x3FF], 16);
        uint32_t e = (Y + (s->vu[(X + 10) & 0x3FF][1] & 0xFFFF)) & 0xFFFF;
        for (uint32_t a = Y + 0xD, b = X + 0xD; a != e && a < Y + 0x400; a++, b++) memcpy(s->vu[b & 0x3FF], s->vu[a & 0x3FF], 16);
    }
    kick(s, X + 0xD);
}

static void skin(st_t* s, uint32_t X) {               /* 0x01E0 */
    uint32_t p0 = X + (s->vu[(X + 11) & 0x3FF][0] & 0xFFFF), p1 = X + (s->vu[(X + 11) & 0x3FF][1] & 0xFFFF);
    uint32_t wa = X + (s->vu[(X + 10) & 0x3FF][2] & 0xFFFF);
    q4 M[4][4];
    for (int b = 0; b < 4; b++) {
        uint32_t o = X + (s->vu[(X + 12) & 0x3FF][b] & 0xFFFF);
        for (int r = 0; r < 4; r++) M[b][r] = ld(s, o + (uint32_t)r);
    }
    const float k255 = bf(0x3B808081u);
    for (uint32_t p = p0; (p & 0xFFFF) != (p1 & 0xFFFF) && p < p0 + 0x400; p++, wa++) {
        q4 P = ld(s, p), Wr = ldraw(s, wa), o = ldraw(s, p);
        float w[4], t[4][3];
        for (int c = 0; c < 4; c++) w[c] = m_((float)(int32_t)Wr.u[c], k255);
        for (int b = 0; b < 4; b++)
            for (int j = 0; j < 3; j++)
                t[b][j] = a_(a_(a_(m_(M[b][0].f[j], P.f[0]), m_(M[b][1].f[j], P.f[1])), m_(M[b][2].f[j], P.f[2])), M[b][3].f[j]);
        for (int j = 0; j < 3; j++)
            o.u[j] = res(a_(a_(a_(m_(t[0][j], w[0]), m_(t[1][j], w[1])), m_(t[2][j], w[2])), m_(t[3][j], w[3])));
        st_(s, p, &o, 7);
    }
}

static void project(st_t* s, uint32_t X) {            /* 0x03C0 */
    uint32_t p0 = X + (s->vu[(X + 11) & 0x3FF][0] & 0xFFFF), p1 = X + (s->vu[(X + 11) & 0x3FF][1] & 0xFFFF);
    q4 M[4];
    for (int r = 0; r < 4; r++) M[r] = ld(s, X + 4 + (uint32_t)r);
    for (uint32_t p = p0; (p & 0xFFFF) != (p1 & 0xFFFF) && p < p0 + 0x400; p++) {
        q4 P = ld(s, p), o = ldraw(s, p);
        float c[4];
        for (int j = 0; j < 4; j++) c[j] = a_(a_(a_(m_(M[0].f[j], P.f[0]), m_(M[1].f[j], P.f[1])), m_(M[2].f[j], P.f[2])), M[3].f[j]);
        float q = clean(fb(divq(1.0f, bf(res(c[3])))));
        for (int j = 0; j < 3; j++) o.u[j] = res(m_(bf(res(c[j])), q));
        st_(s, p, &o, 7);
    }
}

static int outside(const q4* v) {                     /* CLIPw.xyz against 1.0: any judgement bit */
    for (int j = 0; j < 3; j++) if (v->f[j] > 1.0f || v->f[j] < -1.0f) return 1;
    return 0;
}

static void edges(st_t* s, uint32_t X) {              /* 0x0500 */
    uint32_t base = (X + (s->vu[(X + 11) & 0x3FF][0] & 0xFFFF)) & 0xFFFF;
    uint32_t e0 = X + (s->vu[(X + 11) & 0x3FF][2] & 0xFFFF), e1 = X + (s->vu[(X + 11) & 0x3FF][3] & 0xFFFF);
    q4 c0 = ld(s, 0), c1 = ld(s, 1);
    uint32_t pk = 0x12;
    for (uint32_t e = e0; (e & 0xFFFF) != (e1 & 0xFFFF) && e < e0 + 0x400; e++) {
        const uint32_t* r = s->vu[e & 0x3FF];
        q4 A = ld(s, base + (r[0] & 0xFFFF)), V0 = ld(s, base + (r[1] & 0xFFFF)), V1 = ld(s, base + (r[2] & 0xFFFF)),
           Bq = ld(s, base + (r[3] & 0xFFFF));
        float d[3], e10[3], e11[3];
        for (int j = 0; j < 3; j++) { d[j] = s_(V1.f[j], V0.f[j]); e10[j] = s_(V0.f[j], A.f[j]); e11[j] = s_(Bq.f[j], V1.f[j]); }
        if (outside(&A) || outside(&V0) || outside(&V1) || outside(&Bq)) continue;
        float z13 = s_(m_(e10[0], d[1]), m_(d[0], e10[1]));   /* (vf10 x vf09).z: OPMULA / OPMSUB */
        float z14 = s_(m_(e11[0], d[1]), m_(d[0], e11[1]));   /* (vf11 x vf09).z */
        if (!(res(m_(z13, z14)) >> 31)) continue;              /* MUL.z vf00: S - the two triangles face apart */
        int s2 = (res(a_(z13, 0.0f)) >> 31) != 0;              /* ADD.z vf00: S */
        float len2 = a_(m_(d[0], d[0]), m_(d[1], d[1]));
        float P = len2 == 0.0f ? bf(0x7F7FFFFFu) : _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(1.0f), _mm_sqrt_ss(_mm_set_ss(fabsf(len2)))));
        P = bf(res(P));
        float I = bf(0x397BA882u), Ik = s2 ? bf(0xB97BA882u) : I;
        float p17[2] = { a_(0.0f, d[1]), s_(0.0f, d[0]) };
        float p18[2] = { m_(d[0], I), m_(d[1], I) };
        for (int j = 0; j < 2; j++) p17[j] = m_(p17[j], Ik);
        for (int j = 0; j < 2; j++) p18[j] = a_(p18[j], p17[j]);
        for (int j = 0; j < 2; j++) { p17[j] = m_(p17[j], P); p18[j] = m_(p18[j], P); }
        float v15[3] = { a_(V0.f[0], p17[0]), a_(V0.f[1], p17[1]), V0.f[2] };
        float v16[3] = { a_(V1.f[0], p18[0]), a_(V1.f[1], p18[1]), V1.f[2] };
        q4 o19, o20;
        for (int j = 0; j < 3; j++) {
            o19.u[j] = res(a_(m_(v15[j], c0.f[j]), c1.f[j]));   /* MULA.xyz + MADDw.xyz (VU 1 x 1.0) */
            o20.u[j] = res(a_(m_(v16[j], c0.f[j]), c1.f[j]));
        }
        o19.u[3] = o20.u[3] = 0;                                 /* (w: 0 since 0x0548's SUB) */
        uint32_t a = X + pk;
        st_(s, a + 2, &o19, 15); st_(s, a + 6, &o19, 15);
        st_(s, a + 3, &o20, 15); st_(s, a + 7, &o20, 15);
        kick(s, a);
        pk = 0x2C - pk;
    }
}

int r2_outline_run(uint32_t (*vu)[4], uint32_t addr, uint32_t* x, uint8_t* out, uint32_t cap, uint32_t* nout) {
    st_t s = { vu, out, *nout, cap, 0 };
    unsigned int old = _mm_getcsr();
    _mm_setcsr((old & ~0x6000u) | 0x6000u | 0x8040u);
    int ok = 1;
    if (addr == 0x0738) {
        setup(&s, *x);
    } else if (addr == 0x07F0) {
        uint32_t X = *x;
        if (s.vu[(X + 10) & 0x3FF][2] & 0xFFFF) skin(&s, X);
        project(&s, X);
        edges(&s, X);
    } else ok = 0;
    _mm_setcsr(old);
    *nout = s.nout;
    return ok && !s.overflow;
}
