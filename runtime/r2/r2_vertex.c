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
