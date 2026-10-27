/* The prelit vertex function of the VU1 main program (routines 0x06D0 / 0x0960 / 0x0BE0, notes/wip/r2-m1.md) in C: the
 * twin of shaders/r2.hlsl's vertex shader, used by the in-game check (bracket mode, PS2_R2_CHECK) and offline tools
 * (scripts/r2_mesh_check.c) to compare R2's vertices with what the VU XGKICKs. Every operation is the VU's own: the exact
 * result (in double) rounded toward zero and flushed to zero, in the microcode's order, so the outputs are bit-exact
 * (level-1 oracle on the ep3 hub: X, Y, Z, F, S, T, Q all identical); the shader does the same with error-free
 * transforms (shaders/r2_vu.hlsli, checked against these functions by tests/unit/r2). */
#include "r2.h"

#include <math.h>
#include <string.h>

/* the VU's FMAC on floats: the exact result (a double holds every product of two floats exactly, and every sum of two
 * floats whose exponents are close; the others round far below a float ulp) rounded toward zero, flushed to zero */
static float rz(double s) {
    float f = (float)s;
    if (fabs((double)f) > fabs(s)) f = nextafterf(f, 0.0f);
    if (fabsf(f) < 1.17549435e-38f) f = copysignf(0.0f, f);
    return f;
}
static float mul_rz(float a, float b) { return rz((double)a * (double)b); }   /* (the product is exact in a double) */
/* a sum is exact in a double only when the exponents are close: Knuth's two-sum in floats gives the exact sum as s + e
 * (round to nearest), s steps toward zero when e points the other way (as shaders/r2_vu.hlsli) */
static float add_rz(float a, float b) {
    float s = a + b;
    float bb = s - a;
    float e = (a - (s - bb)) + (b - bb);
    if (e != 0.0f && s != 0.0f && ((e < 0.0f) != (s < 0.0f))) s = nextafterf(s, 0.0f);
    if (fabsf(s) < 1.17549435e-38f) s = copysignf(0.0f, s);
    return s;
}
float r2_vu_mul(float a, float b) { return mul_rz(a, b); }
float r2_vu_add(float a, float b) { return add_rz(a, b); }
float r2_vu_rcp(float w) { return rz(1.0 / (double)w); }

void r2_matmul(const float w[16], const float c[16], float out[16]) {
    /* routine 0x0070 (0x0208): row i of M = ((C0 * Wi0 + C1 * Wi1) + C2 * Wi2) + C3 * Wi3, MULA / MADDA / MADD */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float acc = mul_rz(c[0 * 4 + j], w[i * 4 + 0]);
            acc = add_rz(acc, mul_rz(c[1 * 4 + j], w[i * 4 + 1]));
            acc = add_rz(acc, mul_rz(c[2 * 4 + j], w[i * 4 + 2]));
            out[i * 4 + j] = add_rz(acc, mul_rz(c[3 * 4 + j], w[i * 4 + 3]));
        }
}

/* (r2-m2) the same in plain SSE float operations under the VU's rounding (MXCSR: round toward zero, flush to zero,
 * denormals are zero), ~10x faster than the reference above: the EE capture computes every native entry's W x C with
 * it (tests/unit/r2 checks it against r2_matmul). Separate statements, no FMA contraction (the build targets plain
 * x86-64: no FMA instructions exist to contract into). */
#include <xmmintrin.h>
#pragma clang fp contract(off)
void r2_matmul_fast(const float w[16], const float c[16], float out[16]) {
    unsigned int old = _mm_getcsr();
    _mm_setcsr((old & ~0x6000u) | 0x6000u | 0x8040u);
    for (int i = 0; i < 4; i++) {
        __m128 acc = _mm_mul_ps(_mm_loadu_ps(c + 0), _mm_set1_ps(w[i * 4 + 0]));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(c + 4), _mm_set1_ps(w[i * 4 + 1])));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(c + 8), _mm_set1_ps(w[i * 4 + 2])));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(c + 12), _mm_set1_ps(w[i * 4 + 3])));
        _mm_storeu_ps(out + i * 4, acc);
    }
    _mm_setcsr(old);
}

/* ---- (r2-m3) lit / skinned vertices: the VU's own operations under its MXCSR (round toward zero, flush to zero,
 * denormals are zero), as runtime/vu_ops.h computes them (SSE scalar operations: FENV_ACCESS keeps clang from folding
 * them with its own rounding) ----------------------------------------------------------------------------------------- */
static inline uint32_t f2bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float bits2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
#pragma STDC FENV_ACCESS ON
#define VU_CSR(old) ((old & ~0x6000u) | 0x6000u | 0x8040u)
static inline float f_mul(float a, float b) { return _mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float f_add(float a, float b) { return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float f_sub(float a, float b) { return _mm_cvtss_f32(_mm_sub_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static inline float f_div(float a, float b) {        /* DIV (vu_div; x / 0: the largest magnitude, as MINI then sees it) */
    if (b == 0.0f) { uint32_t s = (f2bits(a) ^ f2bits(b)) & 0x80000000u; return bits2f(s | 0x7F7FFFFFu); }
    return _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(a), _mm_set_ss(b)));
}
static inline float f_sqrt(float a) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(a))); }
static inline int32_t fkey(float f) { int32_t i = (int32_t)f2bits(f); return i ^ ((i >> 31) & 0x7FFFFFFF); }
static inline float f_max(float a, float b) { return fkey(a) > fkey(b) ? a : b; }   /* MAX / MINI: raw sign-magnitude */
static inline float f_min(float a, float b) { return fkey(b) > fkey(a) ? a : b; }
static inline int32_t f_toi(float f) {               /* FTOI0: truncate, saturate */
    if (f >= 2147483648.0f) return 0x7FFFFFFF;
    if (f < -2147483648.0f) return (int32_t)0x80000000u;
    return (int32_t)f;
}
static float qwf(const uint32_t* q, int c) { return bits2f(q[c]); }

/* 0x0298 (with 0x3BA8, the inverse of the world 3x3) for the entry's light set */
int r2_lights_prep(const uint32_t* (*hdr)(uint32_t), const r2_mesh* m, const float vu10[4], float out[R2_LIGHT_QW][4]) {
    const uint32_t *q0 = hdr(0), *q1 = hdr(1), *q2 = hdr(2), *qp = m->lpos_bone ? hdr(m->lpos_off + 3u) : hdr(3);
    if (!q0 || !q1 || !q2 || !qp) return 0;
    unsigned int old = _mm_getcsr();
    _mm_setcsr(VU_CSR(old));
    float r[3][3], c[3][3], inv[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i][j] = qwf(i == 0 ? q0 : i == 1 ? q1 : q2, j);
    /* OPMULA / OPMSUB: c0 = r1 x r2, c1 = r2 x r0, c2 = r0 x r1 */
    static const int A_[3] = { 1, 2, 0 }, B_[3] = { 2, 0, 1 };
    for (int k = 0; k < 3; k++) {
        const float *a = r[A_[k]], *b = r[B_[k]];
        c[k][0] = f_sub(f_mul(a[1], b[2]), f_mul(b[1], a[2]));
        c[k][1] = f_sub(f_mul(a[2], b[0]), f_mul(b[2], a[0]));
        c[k][2] = f_sub(f_mul(a[0], b[1]), f_mul(b[0], a[1]));
    }
    float det = f_add(f_add(f_mul(r[0][0], c[0][0]), f_mul(r[0][1], c[0][1])), f_mul(r[0][2], c[0][2]));
    if (det == 0.0f) { _mm_setcsr(old); return 0; }
    float Q = f_div(1.0f, det);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) inv[i][j] = f_mul(c[j][i], Q);
    float A[4][4], C[4][4], D[4][4];
    for (int l = 0; l < 4; l++) {
        const uint32_t *qa = hdr(0x15u + 3u * (uint32_t)l), *qc = hdr(0x16u + 3u * (uint32_t)l), *qd = hdr(0x17u + 3u * (uint32_t)l);
        if (!qa || !qc || !qd) { _mm_setcsr(old); return 0; }
        float v[4];
        for (int j = 0; j < 4; j++) { A[l][j] = qwf(qa, j); C[l][j] = qwf(qc, j); v[j] = qwf(qd, j); }
        if (qa[0] & 0xFFFF) {                            /* a point light: attenuation by the distance */
            for (int j = 0; j < 3; j++) v[j] = f_sub(qwf(qp, j), v[j]);
            float P = f_sqrt(f_add(f_add(f_mul(v[0], v[0]), f_mul(v[1], v[1])), f_mul(v[2], v[2])));
            float f = f_add(f_add(A[l][3], 0.0f), f_mul(v[3], P));
            f = f_max(f_min(f, 1.0f), 0.0f);
            A[l][1] = f_mul(A[l][1], f);
            A[l][2] = f_mul(A[l][2], f);
        }
        float o[3];
        for (int j = 0; j < 3; j++) o[j] = f_add(f_add(f_mul(inv[0][j], v[0]), f_mul(inv[1][j], v[1])), f_mul(inv[2][j], v[2]));
        float P = f_div(1.0f, f_sqrt(f_add(f_add(f_mul(o[0], o[0]), f_mul(o[1], o[1])), f_mul(o[2], o[2]))));
        for (int j = 0; j < 3; j++) D[l][j] = f_mul(o[j], P);
        D[l][3] = v[3];
    }
    const float k127 = bits2f(0x3C010204u);
    for (int l = 0; l < 4; l++) for (int j = 0; j < 4; j++) out[j][l] = f_mul(D[l][j], k127);   /* transposed: [21..24] */
    out[0][1] = f_add(0.0f, out[0][1]);              /* (the transpose's ADDs turn -0 into +0) */
    out[0][2] = f_add(0.0f, out[0][2]);
    out[1][2] = f_add(0.0f, out[1][2]);
    out[2][0] = f_add(0.0f, out[2][0]);
    for (int j = 0; j < 4; j++) {                                    /* (the game's [24] is not this: nobody reads it) */
        float acc = f_mul(C[0][j], A[0][1]);
        for (int l = 1; l < 4; l++) acc = f_add(acc, f_mul(C[l][j], A[l][1]));
        out[8][j] = f_mul(acc, 2.0f);
        for (int l = 0; l < 4; l++) out[4 + l][j] = f_mul(f_mul(C[l][j], A[l][2]), 2.0f);
    }
    for (int l = 0; l < 4; l++) out[9][l] = f_add(0.0f, f_div(f_sub(0.0f, A[l][1]), A[l][2]));
    for (int j = 0; j < 4; j++) out[10][j] = vu10[j];
    _mm_setcsr(old);
    return 1;
}

static float sb(uint32_t w, int c) { return (float)(int8_t)(w >> (8 * c)); }
static float ub(uint32_t w, int c) { return (float)((w >> (8 * c)) & 255u); }

void r2_lit_eval(const r2_vtx* v, const r2_batch* b, const float lb[R2_LIGHT_QW][4], const float (*hq)[4], float pos[3],
                 uint32_t col[2]) {
    unsigned int old = _mm_getcsr();
    _mm_setcsr(VU_CSR(old));
    float p[3] = { v->x, v->y, v->z }, n[4];
    for (int c = 0; c < 4; c++) n[c] = sb(v->nrm, c);
    if (b->skin) {                                   /* 0x16B0 / 0x1940 */
        float w[4], t[4][3], nb[4][3];
        const float k255 = bits2f(0x3B808081u);
        for (int c = 0; c < 4; c++) w[c] = f_mul(ub(v->wts, c), k255);
        for (int k = 0; k < 4; k++) {
            const float(*M)[4] = hq + (b->bone[k] - R2_HQ_LO);
            for (int j = 0; j < 3; j++) {
                t[k][j] = f_add(f_add(f_add(f_mul(M[0][j], p[0]), f_mul(M[1][j], p[1])), f_mul(M[2][j], p[2])), M[3][j]);
                nb[k][j] = f_add(f_add(f_mul(M[0][j], n[0]), f_mul(M[1][j], n[1])), f_mul(M[2][j], n[2]));
            }
        }
        float P[3], N[3];
        for (int j = 0; j < 3; j++) {
            P[j] = f_add(f_add(f_add(f_mul(t[0][j], w[0]), f_mul(t[1][j], w[1])), f_mul(t[2][j], w[2])), f_mul(t[3][j], w[3]));
            N[j] = f_add(f_add(f_add(f_mul(nb[0][j], w[0]), f_mul(nb[1][j], w[1])), f_mul(nb[2][j], w[2])), f_mul(nb[3][j], w[3]));
        }
        for (int j = 0; j < 3; j++) p[j] = P[j];
        if (b->skin == 2) {
            float L = f_div(1.0f, f_sqrt(f_add(f_add(f_mul(N[0], N[0]), f_mul(N[1], N[1])), f_mul(N[2], N[2]))));
            for (int j = 0; j < 3; j++) n[j] = (float)f_toi(f_mul(f_mul(N[j], L), 127.0f));
        }
    }
    pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
    col[0] = v->col[0];
    col[1] = v->col[1];
    if (b->lit) {                                    /* 0x1050 */
        float f[4], C[4], A[4];
        for (int l = 0; l < 4; l++) {
            float d = f_add(f_add(f_mul(lb[0][l], n[0]), f_mul(lb[1][l], n[1])), f_mul(lb[2][l], n[2]));
            float e = f_add(f_add(d, 0.0f), f_mul(d, f_mul(d, d)));
            f[l] = f_min(e, lb[9][l]);
        }
        for (int c = 0; c < 4; c++) A[c] = lb[8][c];
        A[3] = f_add(f_add(A[3], b->bright), lb[10][3]);
        for (int c = 0; c < 4; c++) {
            float acc = f_mul(lb[4][c], f[0]);
            for (int l = 1; l < 4; l++) acc = f_add(acc, f_mul(lb[4 + l][c], f[l]));
            C[c] = f_add(acc, A[c]);
        }
        float mx = f_max(f_max(C[0], C[1]), C[2]);
        float lim = f_max(f_sub(2.0f, mx), 0.0f);
        float cl[4] = { f_min(C[0], 2.0f), f_min(C[1], 2.0f), f_min(C[2], 2.0f), f_min(C[3], lim) };
        float k = f_sub(lim, cl[3]);
        const float c14 = bits2f(0x427DFF7Du);
        float c13 = f_add(2.0f, bits2f(0x3C810247u));
        float den = f_sub(f_mul(c13, c14), f_mul(k, c14));
        float cf[4];
        for (int c = 0; c < 4; c++) cf[c] = ub(v->lcol, c);
        float Q = f_div(cf[3], den);
        int32_t a[4], bq[4];
        for (int c = 0; c < 3; c++) {
            a[c] = f_toi(f_mul(f_add(cl[c], f_mul(lb[10][c], k)), n[3]));
            bq[c] = f_toi(f_mul(f_mul(cf[c], cl[3]), Q));
        }
        a[3] = f_toi(cf[3]);
        bq[3] = f_toi(den);
        col[0] = col[1] = 0;
        for (int c = 0; c < 4; c++) { col[0] |= ((uint32_t)a[c] & 255u) << (8 * c); col[1] |= ((uint32_t)bq[c] & 255u) << (8 * c); }
    }
    _mm_setcsr(old);
}
#pragma STDC FENV_ACCESS OFF

int r2_screen_model_of(const r2_frame_block* fb, r2_screen_model* m) {
    float* a[3] = { &m->ax, &m->ay, &m->az };
    float* b[3] = { &m->bx, &m->by, &m->bz };
    for (int i = 0; i < 3; i++) {
        uint32_t u;
        memcpy(&u, &fb->c1[i], 4);
        int e = (int)((u >> 23) & 0xFF) - 127;
        if ((u >> 31) || e < 24 || e > 100) return 0;
        double base = ldexp(1.0, e), unit = ldexp(1.0, e - 23) * (i == 2 ? 16.0 : 1.0);
        double sa = (double)fb->c0[i] / unit, sb = ((double)fb->c1[i] - base) / unit;
        double lim = i == 2 ? 524288.0 : 65536.0;            /* Z: mantissa >> 4 (19 bits); X, Y: the low 16 bits */
        if (!(sa > 0.0) || sb - sa < 0.0 || sb + sa >= lim) return 0;
        *a[i] = (float)sa;
        *b[i] = (float)sb;
    }
    return 1;
}

void r2_vertex_eval(const r2_vtx* v, const float m[16], const r2_frame_block* fb, const r2_batch* b, int csel,
                    float fade, r2_gsv* o) {
    /* 0x07F8: MULAx / MADDAy / MADDAz / MADDw (row 3 times w = 1.0: exact) */
    float c4[4];
    for (int j = 0; j < 4; j++) {
        float acc = mul_rz(m[0 + j], v->x);
        acc = add_rz(acc, mul_rz(m[4 + j], v->y));
        acc = add_rz(acc, mul_rz(m[8 + j], v->z));
        c4[j] = add_rz(acc, m[12 + j]);
    }
    float cw = c4[3];
    float q = rz(1.0 / (double)cw);                                /* DIV Q, vf00w, vf19w */
    float wm = cw < b->fog_wmax ? cw : b->fog_wmax;                /* MINI with (FLT_MAX x3, VU 9.z) */
    float qx = mul_rz(c4[0], q), qy = mul_rz(c4[1], q), qz = mul_rz(c4[2], q), r = mul_rz(wm, q);
    float fx = add_rz(fb->c1[0], mul_rz(fb->c0[0], qx));          /* ADDA (VU 1, pos.w) + MADD VU 0 x vf15 */
    float fy = add_rz(fb->c1[1], mul_rz(fb->c0[1], qy));
    float fz = add_rz(fb->c1[2], mul_rz(fb->c0[2], qz));
    float wf;
    memcpy(&wf, &v->wbits, 4);
    float fw = add_rz(wf, mul_rz(b->fog_scale, r));
    uint32_t bx, by, bz, bw;
    memcpy(&bx, &fx, 4);
    memcpy(&by, &fy, 4);
    memcpy(&bz, &fz, 4);
    memcpy(&bw, &fw, 4);
    o->x = (int32_t)(bx & 0xFFFF);
    o->y = (int32_t)(by & 0xFFFF);
    o->z = (bz >> 4) & 0xFFFFFF;
    o->f = (bw >> 4) & 0xFF;
    o->adc = (int)((v->wbits >> 15) & 1);
    float s = (float)v->u * (1.0f / 4096.0f), t = (float)v->v * (1.0f / 4096.0f);   /* ITOF12: exact */
    o->s = mul_rz(s, q);
    o->t = mul_rz(t, q);
    o->q = q;                            /* (vf25.z = 1.0: 1 x Q) */
    uint32_t c = v->col[csel ? 1 : 0];
    if (fade < 1.0f) {                   /* 0x0BE0: colour A alpha, colour B RGB scaled, truncated */
        uint32_t ch[4] = { c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24 };
        if (!csel) ch[3] = (uint32_t)(int32_t)((float)ch[3] * fade);
        else for (int k = 0; k < 3; k++) ch[k] = (uint32_t)(int32_t)((float)ch[k] * fade);
        c = ch[0] | ch[1] << 8 | ch[2] << 16 | ch[3] << 24;
    }
    o->rgba = c;
}
