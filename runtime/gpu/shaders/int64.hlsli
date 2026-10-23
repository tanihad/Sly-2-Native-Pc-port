// 64-bit integer helpers for shader model 5 (no native int64). A value is a uint2 (x = low word,
// y = high word) in two's complement; arithmetic wraps modulo 2^64 exactly like the C reference
// (runtime/gpu/gpu_conf.c, uint64_t arithmetic). Checked bit for bit by cs_conformance on the
// hardware adapter and on WARP (notes/d3d11-renderer.md sections 3.6 and 9).
//
// Note: DXBC shifts use the low 5 bits of the amount, so every shift by a variable amount that can
// be 0 or 32 is special-cased.
#ifndef GPU_INT64_HLSLI
#define GPU_INT64_HLSLI

typedef uint2 i64;

i64 i64_make(int lo_signed)            { return uint2(asuint(lo_signed), lo_signed < 0 ? 0xFFFFFFFFu : 0u); }
i64 i64_from_u32(uint lo)              { return uint2(lo, 0u); }

i64 i64_add(i64 a, i64 b) {
    uint lo = a.x + b.x;
    return uint2(lo, a.y + b.y + (lo < a.x ? 1u : 0u));
}

i64 i64_neg(i64 a) { return i64_add(~a, uint2(1u, 0u)); }
i64 i64_sub(i64 a, i64 b) { return i64_add(a, i64_neg(b)); }

bool i64_is_neg(i64 a) { return (a.y & 0x80000000u) != 0u; }

// signed comparisons
bool i64_lt(i64 a, i64 b) {
    int ah = asint(a.y), bh = asint(b.y);
    return ah < bh || (ah == bh && a.x < b.x);
}
bool i64_eq(i64 a, i64 b) { return all(a == b); }

// full 32 x 32 -> 64 unsigned product
i64 u32_mul_wide(uint a, uint b) {
    uint al = a & 0xFFFFu, ah = a >> 16, bl = b & 0xFFFFu, bh = b >> 16;
    uint ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint mid = (ll >> 16) + (lh & 0xFFFFu) + (hl & 0xFFFFu);
    return uint2((ll & 0xFFFFu) | (mid << 16), hh + (lh >> 16) + (hl >> 16) + (mid >> 16));
}

// a * c for a signed 32-bit c, modulo 2^64 (= C: (int64_t)((uint64_t)a * (uint64_t)(int64_t)c))
i64 i64_mul_i32(i64 a, int c) {
    uint cu = asuint(c), chi = c < 0 ? 0xFFFFFFFFu : 0u;
    i64 p = u32_mul_wide(a.x, cu);
    p.y += a.y * cu + a.x * chi;
    return p;
}

// a * b modulo 2^64
i64 i64_mul(i64 a, i64 b) {
    i64 p = u32_mul_wide(a.x, b.x);
    p.y += a.y * b.x + a.x * b.y;
    return p;
}

// arithmetic shift right by s (0..63)
i64 i64_asr(i64 a, uint s) {
    s &= 63u;
    if (s == 0u) return a;
    if (s < 32u) return uint2((a.x >> s) | (a.y << (32u - s)), asuint(asint(a.y) >> s));
    return uint2(asuint(asint(a.y) >> (s - 32u)), asuint(asint(a.y) >> 31));
}

// logical shift left by s (0..63)
i64 i64_shl(i64 a, uint s) {
    s &= 63u;
    if (s == 0u) return a;
    if (s < 32u) return uint2(a.x << s, (a.y << s) | (a.x >> (32u - s)));
    return uint2(0u, a.x << (s - 32u));
}

// unsigned division by 1 <= k <= 65535, in 16-bit limbs (remainder < k keeps every step in 32 bits)
i64 u64_div_small(i64 a, uint k, out uint rem) {
    uint r = 0u, q3, q2, q1, q0, cur;
    cur = (r << 16) | (a.y >> 16);     q3 = cur / k; r = cur - q3 * k;
    cur = (r << 16) | (a.y & 0xFFFFu); q2 = cur / k; r = cur - q2 * k;
    cur = (r << 16) | (a.x >> 16);    q1 = cur / k; r = cur - q1 * k;
    cur = (r << 16) | (a.x & 0xFFFFu); q0 = cur / k; r = cur - q0 * k;
    rem = r;
    return uint2((q1 << 16) | q0, (q3 << 16) | q2);
}

// floor(a / k) for signed a, 1 <= k <= 65535
i64 i64_floordiv_small(i64 a, uint k) {
    uint r;
    if (!i64_is_neg(a)) return u64_div_small(a, k, r);
    i64 q = u64_div_small(i64_neg(a), k, r);
    if (r != 0u) q = i64_add(q, uint2(1u, 0u));
    return i64_neg(q);
}

// ---- exact integer GS depth (notes/d3d11-renderer.md 3.6) -----------------------------------------
// Z record: 16.16 value `row` at row by0 and x = 0, per-row step dy, per-pixel step dx (rowstep_init,
// runtime/gs_draw.c). Sample (i, j) of native pixel (px, py) at scale (kx, ky).
i64 gs_z16(i64 row, i64 dy, i64 dx, int by0, int px, int py, uint i, uint j, uint kx, uint ky) {
    i64 z = i64_add(row, i64_mul_i32(dy, py - by0));
    z = i64_add(z, i64_mul_i32(dx, px));
    if (j != 0u) z = i64_add(z, i64_floordiv_small(i64_mul_i32(dy, (int)j), ky));
    if (i != 0u) z = i64_add(z, i64_floordiv_small(i64_mul_i32(dx, (int)i), kx));
    return z;
}

// (M7) a float holding an integer value (|v| < 2^62) as int64: floor(v)
i64 i64_from_float_floor(float v) {
    float fv = floor(v);
    float hi = floor(fv * (1.0 / 4294967296.0));
    float lo = fv - hi * 4294967296.0;                // 0 <= lo < 2^32, exact (fv and hi * 2^32 share the exponent range)
    return uint2((uint)lo, asuint((int)hi));
}

// (M7) the Z of another sample (i, j) of the pixel: the native value (exact) plus the plane's step to the sample
// in float - the sub-sample offsets need no bit-exactness, only one formula for every tier (FF and ROV), and two
// integer 64-bit divisions per sample cost more than the rest of the Z work
i64 gs_z16_sample(i64 row, i64 dy, i64 dx, int by0, int px, int py, uint i, uint j, uint kx, uint ky) {
    i64 z = i64_add(row, i64_mul_i32(dy, py - by0));
    z = i64_add(z, i64_mul_i32(dx, px));
    if ((i | j) != 0u) {
        float fdy = (float)asint(dy.y) * 4294967296.0 + (float)dy.x, fdx = (float)asint(dx.y) * 4294967296.0 + (float)dx.x;
        z = i64_add(z, i64_from_float_floor(fdy * ((float)j / (float)ky) + fdx * ((float)i / (float)kx)));
    }
    return z;
}

// tests_batch (runtime/gs_draw_pixel.c): z >> 16 clamped to [0, zmax]
uint gs_z_clamp(i64 z16, uint zmax) {
    i64 zi = i64_asr(z16, 16u);
    if (i64_is_neg(zi)) return 0u;
    if (zi.y != 0u || zi.x > zmax) return zmax;
    return zi.x;
}

// order-preserving depth encoding, exact for z <= 0x3F000000 (encodes to <= 1.0, never a denormal)
float gs_z_encode(uint z) { return asfloat(min(z, 0x00FFFFFFu) + 0x3E800000u); }   // (pc-research: [0.25, 1), linear below 2^23: an interpolable bound)
uint  gs_z_decode(float d) { return asuint(d) - 0x3E800000u; }

#endif
