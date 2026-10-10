/* VU micro mode: the semantics of the individual operations, shared by the interpreter (vu_exec.c)
 * and the statically recompiled microprograms (recomp/vu_codegen.py output). Keeping one copy is
 * what makes the compiled programs bit-exact against the interpreter: both call these helpers.
 *
 * Everything here works on values (no vu_state), runs under the VU MXCSR (round toward zero,
 * flush-to-zero, denormals-are-zero; see vu_internal.h) and is forced inline: the per-op callers
 * pass constant lane masks/kinds and rely on them being folded.
 *
 * Floating point notes (see notes/vu-microcode.md): PS2 floats with exponent 255 are ordinary huge
 * numbers; arithmetic inputs are clamped to +-FLT_MAX first (vu_vclean). MAX/MINI, ABS, MOVE, loads
 * and stores work on raw bits. */
#pragma once
#include <emmintrin.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define VU_INLINE static inline __attribute__((always_inline))

/* The instruction's 4-bit dest field has x in bit 3; internally lane 0 = x is bit 0. */
static const uint8_t VU_REV4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };

#define VU_LM(m) { (m) & 1 ? ~0u : 0u, (m) & 2 ? ~0u : 0u, (m) & 4 ? ~0u : 0u, (m) & 8 ? ~0u : 0u }
static const union { uint32_t u[16][4]; __m128i v[16]; } VU_LANEMASK = { {
    VU_LM(0), VU_LM(1), VU_LM(2), VU_LM(3), VU_LM(4), VU_LM(5), VU_LM(6), VU_LM(7),
    VU_LM(8), VU_LM(9), VU_LM(10), VU_LM(11), VU_LM(12), VU_LM(13), VU_LM(14), VU_LM(15) } };

/* ---- lanes and bits ---------------------------------------------------------------------------- */

VU_INLINE __m128 vu_bits(uint32_t b) { return _mm_castsi128_ps(_mm_set1_epi32((int)b)); }
VU_INLINE __m128 vu_blend(__m128 old, __m128 nw, unsigned lanes) {
    if (lanes == 15) return nw;
    if (lanes == 0) return old;
    __m128 m = _mm_castsi128_ps(VU_LANEMASK.v[lanes]);
    return _mm_or_ps(_mm_and_ps(m, nw), _mm_andnot_ps(m, old));
}
VU_INLINE uint32_t vu_lane(__m128 v, unsigned lane) {
    __m128i i = _mm_castps_si128(v);
    switch (lane & 3) {
    case 0: return (uint32_t)_mm_cvtsi128_si32(i);
    case 1: return (uint32_t)_mm_cvtsi128_si32(_mm_shuffle_epi32(i, 0x55));
    case 2: return (uint32_t)_mm_cvtsi128_si32(_mm_shuffle_epi32(i, 0xAA));
    default: return (uint32_t)_mm_cvtsi128_si32(_mm_shuffle_epi32(i, 0xFF));
    }
}
VU_INLINE __m128 vu_bcast(__m128 v, unsigned lane) {
    switch (lane & 3) {
    case 0: return _mm_shuffle_ps(v, v, 0x00);
    case 1: return _mm_shuffle_ps(v, v, 0x55);
    case 2: return _mm_shuffle_ps(v, v, 0xAA);
    default: return _mm_shuffle_ps(v, v, 0xFF);
    }
}

/* PS2 floats with exponent 255 are ordinary (huge) numbers; the host would see Inf/NaN.
 * Clamp them to +-FLT_MAX (keeping the sign) before arithmetic. */
VU_INLINE __m128 vu_vclean(__m128 v) {
    const __m128i E = _mm_set1_epi32(0x7F800000);
    __m128i i = _mm_castps_si128(v);
    __m128i big = _mm_cmpeq_epi32(_mm_and_si128(i, E), E);
    i = _mm_xor_si128(i, _mm_and_si128(big, _mm_set1_epi32(0x00800000)));
    i = _mm_or_si128(i, _mm_and_si128(big, _mm_set1_epi32(0x007FFFFF)));
    return _mm_castsi128_ps(i);
}
VU_INLINE float vu_sclean(uint32_t b) {
    if ((b & 0x7F800000u) == 0x7F800000u) b = (b & 0x80000000u) | 0x7F7FFFFFu;
    float f; memcpy(&f, &b, 4); return f;
}
VU_INLINE uint32_t vu_fbits(float f) {   /* host result -> PS2 bits (no Inf/NaN, no denormals) */
    uint32_t b; memcpy(&b, &f, 4);
    if ((b & 0x7F800000u) == 0x7F800000u) b = (b & 0x80000000u) | 0x7F7FFFFFu;
    if ((b & 0x7F800000u) == 0) b &= 0x80000000u;
    return b;
}
VU_INLINE unsigned vu_zlanes(__m128 v) { return (unsigned)_mm_movemask_ps(_mm_cmpeq_ps(v, _mm_setzero_ps())); }
VU_INLINE unsigned vu_nelanes(__m128 a, __m128 b) { return (unsigned)_mm_movemask_ps(_mm_cmpneq_ps(a, b)); }
VU_INLINE __m128 vu_vneg(__m128 v) { return _mm_xor_ps(v, vu_bits(0x80000000u)); }

/* Sign-magnitude ordering key for MAX/MINI (compares raw bits, denormals included). */
VU_INLINE __m128i vu_fkey(__m128 v) {
    __m128i i = _mm_castps_si128(v);
    return _mm_xor_si128(i, _mm_and_si128(_mm_srai_epi32(i, 31), _mm_set1_epi32(0x7FFFFFFF)));
}

/* ---- FMAC ---------------------------------------------------------------------------------------- */

enum { VU_K_ADD, VU_K_SUB, VU_K_MUL, VU_K_MADD, VU_K_MSUB };

/* ADD/SUB/MUL/MADD/MSUB on cleaned operands: the result, and per lane (bit0 = x, restricted to dst)
 * the Z mask and the U mask (result flushed to zero although the exact result was not zero). */
VU_INLINE __m128 vu_fmac(int kind, __m128 a, __m128 b, __m128 acc, unsigned dst, unsigned* zo, unsigned* uo) {
    __m128 r;
    unsigned z, u = 0;
    if (kind == VU_K_ADD) {
        r = _mm_add_ps(a, b);
        z = vu_zlanes(r) & dst;
        if (z) u = z & vu_nelanes(a, vu_vneg(b));
    } else if (kind == VU_K_SUB) {
        r = _mm_sub_ps(a, b);
        z = vu_zlanes(r) & dst;
        if (z) u = z & vu_nelanes(a, b);
    } else if (kind == VU_K_MUL) {
        r = _mm_mul_ps(a, b);
        z = vu_zlanes(r) & dst;
        if (z) u = z & ~vu_zlanes(a) & ~vu_zlanes(b);
    } else {
        __m128 p = _mm_mul_ps(a, b);
        r = kind == VU_K_MADD ? _mm_add_ps(acc, p) : _mm_sub_ps(acc, p);
        z = vu_zlanes(r) & dst;
        if (z) u = z & ((kind == VU_K_MADD ? vu_nelanes(acc, vu_vneg(p)) : vu_nelanes(acc, p)) |
                        (vu_zlanes(p) & ~vu_zlanes(a) & ~vu_zlanes(b)));
    }
    *zo = z;
    *uo = u;
    return r;
}

/* OPMULA ACC.xyz = fs.yzx * ft.zxy; OPMSUB fd.xyz = ACC - fs.yzx * ft.zxy (cross product pair).
 * a and b are the cleaned, unshuffled operands. */
VU_INLINE __m128 vu_opm(int sub, __m128 a, __m128 b, __m128 acc, unsigned dst, unsigned* zo, unsigned* uo) {
    a = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1));
    b = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2));
    __m128 p = _mm_mul_ps(a, b), r;
    unsigned z, u = 0;
    if (!sub) {
        r = p;
        z = vu_zlanes(r) & dst;
        if (z) u = z & ~vu_zlanes(a) & ~vu_zlanes(b);
    } else {
        r = _mm_sub_ps(acc, p);
        z = vu_zlanes(r) & dst;
        if (z) u = z & (vu_nelanes(acc, p) | (vu_zlanes(p) & ~vu_zlanes(a) & ~vu_zlanes(b)));
    }
    *zo = z;
    *uo = u;
    return r;
}

/* MAC flags of an FMAC result (hardware bit order: Z 0-3, S 4-7, U 8-11, O 12-15, w = lowest bit
 * of each group) in bits 0-15, and the status bits Z S U O in bits 16-19. z and u are lane masks
 * (bit0 = x) already restricted to the dest fields. */
VU_INLINE uint32_t vu_macflags(__m128 r, unsigned dst, unsigned z, unsigned u) {
    unsigned s = (unsigned)_mm_movemask_ps(r) & dst;
    __m128i a = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF));
    unsigned o = (unsigned)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(a, _mm_set1_epi32(0x7F7FFFFF)))) & dst;
    uint32_t mac = (uint32_t)(VU_REV4[z] | VU_REV4[s] << 4 | VU_REV4[u] << 8 | VU_REV4[o] << 12);
    uint32_t zsuo = (z ? 1u : 0u) | (s ? 2u : 0u) | (u ? 4u : 0u) | (o ? 8u : 0u);
    return mac | zsuo << 16;
}
/* Status register after the MAC/status entry f (vu_macflags) lands: Z S U O replaced, sticky ORed. */
VU_INLINE uint32_t vu_status_fold(uint32_t status, uint32_t f) {
    uint32_t zsuo = f >> 16;
    return (status & ~0xFu) | zsuo | (zsuo << 6);
}
/* Status register after a Q result with I/D flags q_flags (0x10 / 0x20) lands. */
VU_INLINE uint32_t vu_status_qfold(uint32_t status, uint32_t q_flags) {
    return (status & ~0x30u) | q_flags | (q_flags << 6);
}

/* MAX/MINI compare raw bits (sign-magnitude) and pass values through untouched; no flags. */
VU_INLINE __m128 vu_maxmin(int is_max, __m128 a, __m128 b) {
    __m128 pick_a = _mm_castsi128_ps(is_max ? _mm_cmpgt_epi32(vu_fkey(a), vu_fkey(b)) : _mm_cmpgt_epi32(vu_fkey(b), vu_fkey(a)));
    return _mm_or_ps(_mm_and_ps(pick_a, a), _mm_andnot_ps(pick_a, b));
}

VU_INLINE __m128 vu_abs(__m128 v) { return _mm_and_ps(v, vu_bits(0x7FFFFFFFu)); }

/* FTOIn: truncate toward zero, saturating to 0x7FFFFFFF / 0x80000000. v is the raw register. */
VU_INLINE __m128 vu_ftoi(__m128 v, float scale) {
    v = _mm_mul_ps(vu_vclean(v), _mm_set1_ps(scale));
    __m128i i = _mm_cvttps_epi32(v);                               /* out of range -> 0x80000000 */
    __m128i pos = _mm_castps_si128(_mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f)));
    return _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(pos, i), _mm_and_si128(pos, _mm_set1_epi32(0x7FFFFFFF))));
}
VU_INLINE __m128 vu_itof(__m128 raw, float scale) {
    __m128 v = _mm_cvtepi32_ps(_mm_castps_si128(raw));             /* rounds toward zero */
    return scale == 1.0f ? v : _mm_mul_ps(v, _mm_set1_ps(scale));
}

/* CLIP judgement of fs.xyz against +-|ft.w| (6 bits: +x -x +y -y +z -z). */
VU_INLINE uint32_t vu_clip_judge(uint32_t xs, uint32_t ys, uint32_t zs, uint32_t wt) {
    float w = fabsf(vu_sclean(wt)), x = vu_sclean(xs), y = vu_sclean(ys), z = vu_sclean(zs);
    return (uint32_t)(x > w) | (uint32_t)(x < -w) << 1 | (uint32_t)(y > w) << 2 |
           (uint32_t)(y < -w) << 3 | (uint32_t)(z > w) << 4 | (uint32_t)(z < -w) << 5;
}

/* ---- FDIV: Q result and status I (0x10) / D (0x20) flags --------------------------------------- */

VU_INLINE uint32_t vu_div(uint32_t sb, uint32_t tb, uint32_t* flags) {
    float s = vu_sclean(sb), t = vu_sclean(tb);
    if (t == 0.0f) { *flags = s == 0.0f ? 0x10 : 0x20; return ((sb ^ tb) & 0x80000000u) | 0x7FFFFFFFu; }
    *flags = 0;
    return vu_fbits(s / t);
}
VU_INLINE uint32_t vu_sqrt(uint32_t tb, uint32_t* flags) {
    float t = vu_sclean(tb);
    *flags = t < 0.0f ? 0x10 : 0;
    return vu_fbits(sqrtf(fabsf(t)));
}
VU_INLINE uint32_t vu_rsqrt(uint32_t sb, uint32_t tb, uint32_t* flags) {
    float s = vu_sclean(sb), t = vu_sclean(tb);
    if (t == 0.0f) { *flags = s == 0.0f ? 0x10 : 0x20; return (sb & 0x80000000u) | 0x7FFFFFFFu; }
    *flags = t < 0.0f ? 0x10 : 0;
    return vu_fbits(s / sqrtf(fabsf(t)));
}

/* ---- EFU ------------------------------------------------------------------------------------------ */

enum { VU_E_ESADD, VU_E_ERSADD, VU_E_ELENG, VU_E_ERLENG, VU_E_EATANXY, VU_E_EATANXZ, VU_E_ESUM,
       VU_E_ESQRT, VU_E_ERSQRT, VU_E_ERCPR, VU_E_ESIN, VU_E_EATAN, VU_E_EEXP };

VU_INLINE float vu_sdiv(float a, float b) {   /* division with the VU's divide-by-zero result */
    if (b == 0.0f) return (signbit(a) != signbit(b)) ? -FLT_MAX : FLT_MAX;
    return a / b;
}
/* P result of an EFU op on register bits v[4] (f = the selected lane for single-lane ops). */
VU_INLINE uint32_t vu_efu(int e, uint32_t xb, uint32_t yb, uint32_t zb, uint32_t wb, uint32_t fb) {
    float x = vu_sclean(xb), y = vu_sclean(yb), z = vu_sclean(zb), w = vu_sclean(wb), f = vu_sclean(fb), p;
    switch (e) {
    case VU_E_ESADD:   p = x * x + y * y + z * z; break;
    case VU_E_ERSADD:  p = vu_sdiv(1.0f, x * x + y * y + z * z); break;
    case VU_E_ELENG:   p = sqrtf(x * x + y * y + z * z); break;
    case VU_E_ERLENG:  p = vu_sdiv(1.0f, sqrtf(x * x + y * y + z * z)); break;
    case VU_E_EATANXY: p = atanf(vu_sdiv(y, x)); break;
    case VU_E_EATANXZ: p = atanf(vu_sdiv(z, x)); break;
    case VU_E_ESUM:    p = x + y + z + w; break;
    case VU_E_ESQRT:   p = sqrtf(fabsf(f)); break;
    case VU_E_ERSQRT:  p = vu_sdiv(1.0f, sqrtf(fabsf(f))); break;
    case VU_E_ERCPR:   p = vu_sdiv(1.0f, f); break;
    case VU_E_ESIN:    p = sinf(f); break;
    case VU_E_EATAN:   p = atanf(f); break;
    default:           p = expf(-f); break;     /* EEXP */
    }
    return vu_fbits(p);
}

/* 23-bit maximal-length LFSR (x^23 + x^18 + 1) for RNEXT. Sony documents R as an M-sequence but
 * not its polynomial; games only use it for visual noise. */
VU_INLINE uint32_t vu_rnext(uint32_t r) {
    uint32_t x = r & 0x7FFFFF;
    if (!x) x = 0x5A5A5A;
    uint32_t bit = ((x >> 22) ^ (x >> 17)) & 1;
    return 0x3F800000u | (((x << 1) | bit) & 0x7FFFFF);
}
