// (r2-m1) Exact arithmetic for the modern renderer's shaders (shaders/r2.hlsl), shared with its conformance kernels
// (shaders/r2_conf.hlsl, tests/unit/r2): the VU's float arithmetic bit for bit, and exact rational 64-bit division.
#ifndef GPU_R2_VU_HLSLI
#define GPU_R2_VU_HLSLI
#include "int64.hlsli"

// ---- the VU's FMAC arithmetic, bit for bit: every MUL / ADD rounds toward zero and flushes to zero (runtime/vu_exec.c
// runs the microcode under that MXCSR). D3D11 rounds float add / mul to nearest; the error-free transforms (Dekker's
// product with Veltkamp's split, Knuth's two-sum) give the exact result as r + e, and r steps one ulp toward zero when e
// points the other way. Twin of runtime/r2/r2_vertex.c (r2_vu_mul / r2_vu_add / r2_vu_rcp there, through doubles).
// Valid for operands whose products neither overflow nor reach the denormal range (|values| within 2^-60 .. 2^60).
float vu_fix(float r, float e) {
    if (e != 0.0 && r != 0.0 && ((e < 0.0) != (r < 0.0))) r = asfloat(asuint(r) - 1u);
    if (abs(r) < 1.17549435e-38) r = asfloat(asuint(r) & 0x80000000u);
    return r;
}
float2 vu_two_prod(float a, float b) {
    precise float p = a * b;
    precise float ca = 4097.0 * a;
    precise float ah = ca - (ca - a);
    precise float al = a - ah;
    precise float cb = 4097.0 * b;
    precise float bh = cb - (cb - b);
    precise float bl = b - bh;
    precise float e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return float2(p, e);
}
float vu_mul(float a, float b) { float2 pe = vu_two_prod(a, b); return vu_fix(pe.x, pe.y); }
float vu_add(float a, float b) {
    precise float s = a + b;
    precise float bb = s - a;
    precise float e = (a - (s - bb)) + (b - bb);
    return vu_fix(s, e);
}
bool vu_rcp_over(float c, float w) {   // c * w > 1, exactly
    float2 pe = vu_two_prod(c, w);
    precise float d = 1.0 - pe.x;      // exact (Sterbenz: pe.x is near 1)
    return d < pe.y;
}
float vu_rcp(float w) {                // DIV Q, vf00w, w for w > 0: the largest float c with c * w <= 1
    precise float q = 1.0 / w;         // (D3D11: within 2.5 ulp)
    [unroll] for (uint k = 0u; k < 3u; k++) if (vu_rcp_over(q, w)) q = asfloat(asuint(q) - 1u);
    [unroll] for (uint k2 = 0u; k2 < 3u; k2++) {
        float n = asfloat(asuint(q) + 1u);
        if (!vu_rcp_over(n, w)) q = n;
    }
    return q;
}

// ---- exact rational arithmetic on int64.hlsli values ----
// (the magnitude, then the sign: hi * 2^32 + lo with a negative hi cancels catastrophically for small negative values)
float i64_to_float(i64 a) {
    bool neg = i64_is_neg(a);
    i64 m = neg ? i64_neg(a) : a;
    float f = (float)m.y * 4294967296.0 + (float)m.x;
    return neg ? -f : f;
}

// floor(v) as int64 for any |v| < 2^62 (int64.hlsli's i64_from_float_floor loses small negative values: its low word
// 2^32 - k is not a float)
i64 r2_floor_i64(float v) {
    float fv = floor(v), a = abs(fv);
    float hi = floor(a * (1.0 / 4294967296.0));
    i64 r = uint2((uint)(a - hi * 4294967296.0), (uint)hi);
    return fv < 0.0 ? i64_neg(r) : r;
}

// floor(n / d) and n - q d for d > 0 (|n| < 2^62, d < 2^40): float estimates refined on the exact remainder
i64 i64_divmod_floor(i64 n, i64 d, out i64 r) {
    float fd = i64_to_float(d);
    i64 q = r2_floor_i64(i64_to_float(n) / fd);
    r = i64_sub(n, i64_mul(q, d));
    [unroll] for (uint k = 0u; k < 2u; k++) {
        i64 dq = r2_floor_i64(i64_to_float(r) / fd);
        q = i64_add(q, dq);
        r = i64_sub(r, i64_mul(dq, d));
    }
    [unroll] for (uint k2 = 0u; k2 < 2u; k2++) if (i64_is_neg(r)) { q = i64_sub(q, uint2(1u, 0u)); r = i64_add(r, d); }
    [unroll] for (uint k3 = 0u; k3 < 2u; k3++) if (!i64_lt(r, d)) { q = i64_add(q, uint2(1u, 0u)); r = i64_sub(r, d); }
    return q;
}

// llround(2^s n / d) for d > 0, half away from zero (`pos`: the total it is added to is >= 0: halves round up)
i64 i64_round_scaled(i64 n, i64 d, uint s, bool pos) {
    i64 r, r2;
    i64 q = i64_divmod_floor(n, d, r);                     // n = q d + r, 0 <= r < d
    i64 q2 = i64_divmod_floor(i64_shl(r, s), d, r2);       // 2^s r = q2 d + r2
    i64 t = i64_add(i64_shl(q, s), q2);                    // floor(2^s n / d); fraction r2 / d
    i64 twice = i64_shl(r2, 1u);
    bool neg = !pos && i64_is_neg(t);
    if (neg ? i64_lt(d, twice) : !i64_lt(twice, d)) t = i64_add(t, uint2(1u, 0u));   // (> 1/2 below zero, >= 1/2 above)
    return t;
}
#endif
