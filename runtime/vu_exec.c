/* VU micro mode: instruction decoder and interpreter, shared by VU0 and VU1.
 *
 * Written from Sony's VU User's Manual instruction set description and ps2tek
 * (https://psi-rockin.github.io/ps2tek/); the timing model and its sources are described in
 * vu_internal.h and notes/vu-microcode.md.
 *
 * Structure: micro memory is decoded into a cache of vu_op entries (one per 64-bit pair), each
 * holding a specialised handler for its upper and lower instruction. The main loop only does
 * fetch/validate, stall and pipeline bookkeeping, and calls the two handlers.
 *
 * Floating point: the caller (vu.c) runs us with MXCSR = round toward zero, flush-to-zero,
 * denormals-are-zero. That matches the VU's truncating FMAC, makes overflow saturate to
 * +-FLT_MAX instead of producing infinity, and flushes denormal inputs/outputs to zero. The only
 * PS2 values the host cannot represent are those with exponent 255 (no Inf/NaN on the VU: they are
 * ordinary numbers up to ~6.8e38); arithmetic inputs are clamped to +-FLT_MAX first (vclean). MAX,
 * MINI, ABS, MOVE, loads and stores work on raw bits, as on hardware.
 */
#include "vu_internal.h"
#include "vu_ops.h"                      /* operation semantics, shared with the compiled programs */

#include <math.h>

/* The game build compiles the runtime at -O1, which does not inline helpers on its own; the per-op
 * handlers rely on them (VU_INLINE, vu_ops.h) being inlined with constant arguments. */

/* ---- operation ids ------------------------------------------------------------------------- */

enum {
    UOP_INVALID, UOP_NOP, UOP_ADD, UOP_SUB, UOP_MUL, UOP_MADD, UOP_MSUB, UOP_MAX, UOP_MINI,
    UOP_OPMULA, UOP_OPMSUB, UOP_ABS, UOP_FTOI0, UOP_FTOI4, UOP_FTOI12, UOP_FTOI15,
    UOP_ITOF0, UOP_ITOF4, UOP_ITOF12, UOP_ITOF15, UOP_CLIP
};
enum { BM_VEC, BM_BC, BM_Q, BM_I };      /* second operand: ft, ft broadcast lane, Q, I */

enum {
    LOP_INVALID, LOP_NOP, LOP_LOI,
    LOP_LQ, LOP_SQ, LOP_ILW, LOP_ISW, LOP_IADDIU, LOP_ISUBIU,
    LOP_FCEQ, LOP_FCSET, LOP_FCAND, LOP_FCOR, LOP_FSEQ, LOP_FSSET, LOP_FSAND, LOP_FSOR,
    LOP_FMEQ, LOP_FMAND, LOP_FMOR, LOP_FCGET,
    LOP_B, LOP_BAL, LOP_JR, LOP_JALR, LOP_IBEQ, LOP_IBNE, LOP_IBLTZ, LOP_IBGTZ, LOP_IBLEZ, LOP_IBGEZ,
    LOP_IADD, LOP_ISUB, LOP_IADDI, LOP_IAND, LOP_IOR,
    LOP_MOVE, LOP_MR32, LOP_LQI, LOP_SQI, LOP_LQD, LOP_SQD,
    LOP_DIV, LOP_SQRT, LOP_RSQRT, LOP_WAITQ, LOP_MTIR, LOP_MFIR, LOP_ILWR, LOP_ISWR,
    LOP_RNEXT, LOP_RGET, LOP_RINIT, LOP_RXOR, LOP_MFP, LOP_XTOP, LOP_XITOP, LOP_XGKICK,
    LOP_ESADD, LOP_ERSADD, LOP_ELENG, LOP_ERLENG, LOP_EATANXY, LOP_EATANXZ, LOP_ESUM,
    LOP_ESQRT, LOP_ERSQRT, LOP_ERCPR, LOP_WAITP, LOP_ESIN, LOP_EATAN, LOP_EEXP,
    LOP_COUNT
};

/* ---- decode tables -------------------------------------------------------------------------- */

/* The instruction's 4-bit dest field has x in bit 3; internally lane 0 = x is bit 0. */
#define REV4 VU_REV4

typedef struct { uint8_t op, bm, acc; } uent;
#define BC4(o, a) { o, BM_BC, a }, { o, BM_BC, a }, { o, BM_BC, a }, { o, BM_BC, a }
/* Upper word bits 5-0 below 0x30. */
static const uent UPPER1[48] = {
    BC4(UOP_ADD, 0), BC4(UOP_SUB, 0), BC4(UOP_MADD, 0), BC4(UOP_MSUB, 0),
    BC4(UOP_MAX, 0), BC4(UOP_MINI, 0), BC4(UOP_MUL, 0),
    { UOP_MUL, BM_Q, 0 }, { UOP_MAX, BM_I, 0 }, { UOP_MUL, BM_I, 0 }, { UOP_MINI, BM_I, 0 },
    { UOP_ADD, BM_Q, 0 }, { UOP_MADD, BM_Q, 0 }, { UOP_ADD, BM_I, 0 }, { UOP_MADD, BM_I, 0 },
    { UOP_SUB, BM_Q, 0 }, { UOP_MSUB, BM_Q, 0 }, { UOP_SUB, BM_I, 0 }, { UOP_MSUB, BM_I, 0 },
    { UOP_ADD, BM_VEC, 0 }, { UOP_MADD, BM_VEC, 0 }, { UOP_MUL, BM_VEC, 0 }, { UOP_MAX, BM_VEC, 0 },
    { UOP_SUB, BM_VEC, 0 }, { UOP_MSUB, BM_VEC, 0 }, { UOP_OPMSUB, BM_VEC, 0 }, { UOP_MINI, BM_VEC, 0 },
};
/* Bits 5-2 = 1111: index ((bits 10-6) << 2) | bits 1-0, below 0x30. */
static const uent UPPER2[48] = {
    BC4(UOP_ADD, 1), BC4(UOP_SUB, 1), BC4(UOP_MADD, 1), BC4(UOP_MSUB, 1),
    { UOP_ITOF0, 0, 0 }, { UOP_ITOF4, 0, 0 }, { UOP_ITOF12, 0, 0 }, { UOP_ITOF15, 0, 0 },
    { UOP_FTOI0, 0, 0 }, { UOP_FTOI4, 0, 0 }, { UOP_FTOI12, 0, 0 }, { UOP_FTOI15, 0, 0 },
    BC4(UOP_MUL, 1),
    { UOP_MUL, BM_Q, 1 }, { UOP_ABS, 0, 0 }, { UOP_MUL, BM_I, 1 }, { UOP_CLIP, 0, 0 },
    { UOP_ADD, BM_Q, 1 }, { UOP_MADD, BM_Q, 1 }, { UOP_ADD, BM_I, 1 }, { UOP_MADD, BM_I, 1 },
    { UOP_SUB, BM_Q, 1 }, { UOP_MSUB, BM_Q, 1 }, { UOP_SUB, BM_I, 1 }, { UOP_MSUB, BM_I, 1 },
    { UOP_ADD, BM_VEC, 1 }, { UOP_MADD, BM_VEC, 1 }, { UOP_MUL, BM_VEC, 1 }, { UOP_INVALID, 0, 0 },
    { UOP_SUB, BM_VEC, 1 }, { UOP_MSUB, BM_VEC, 1 }, { UOP_OPMULA, BM_VEC, 1 }, { UOP_NOP, 0, 0 },
};

/* Lower word bits 31-25. */
static const uint8_t LOWER7[128] = {
    [0x00] = LOP_LQ, [0x01] = LOP_SQ, [0x04] = LOP_ILW, [0x05] = LOP_ISW,
    [0x08] = LOP_IADDIU, [0x09] = LOP_ISUBIU,
    [0x10] = LOP_FCEQ, [0x11] = LOP_FCSET, [0x12] = LOP_FCAND, [0x13] = LOP_FCOR,
    [0x14] = LOP_FSEQ, [0x15] = LOP_FSSET, [0x16] = LOP_FSAND, [0x17] = LOP_FSOR,
    [0x18] = LOP_FMEQ, [0x1A] = LOP_FMAND, [0x1B] = LOP_FMOR, [0x1C] = LOP_FCGET,
    [0x20] = LOP_B, [0x21] = LOP_BAL, [0x24] = LOP_JR, [0x25] = LOP_JALR,
    [0x28] = LOP_IBEQ, [0x29] = LOP_IBNE, [0x2C] = LOP_IBLTZ, [0x2D] = LOP_IBGTZ,
    [0x2E] = LOP_IBLEZ, [0x2F] = LOP_IBGEZ,
};
/* Bits 31-25 = 0x40, bits 5-0 below 0x3C. */
static const uint8_t LOWER_S1[64] = {
    [0x30] = LOP_IADD, [0x31] = LOP_ISUB, [0x32] = LOP_IADDI, [0x34] = LOP_IAND, [0x35] = LOP_IOR,
};
/* Bits 31-25 = 0x40, bits 5-2 = 1111: index ((bits 10-6) << 2) | bits 1-0. */
static const uint8_t LOWER_S2[128] = {
    [0x30] = LOP_MOVE, [0x31] = LOP_MR32, [0x34] = LOP_LQI, [0x35] = LOP_SQI,
    [0x36] = LOP_LQD, [0x37] = LOP_SQD, [0x38] = LOP_DIV, [0x39] = LOP_SQRT,
    [0x3A] = LOP_RSQRT, [0x3B] = LOP_WAITQ, [0x3C] = LOP_MTIR, [0x3D] = LOP_MFIR,
    [0x3E] = LOP_ILWR, [0x3F] = LOP_ISWR, [0x40] = LOP_RNEXT, [0x41] = LOP_RGET,
    [0x42] = LOP_RINIT, [0x43] = LOP_RXOR, [0x64] = LOP_MFP, [0x68] = LOP_XTOP,
    [0x69] = LOP_XITOP, [0x6C] = LOP_XGKICK, [0x70] = LOP_ESADD, [0x71] = LOP_ERSADD,
    [0x72] = LOP_ELENG, [0x73] = LOP_ERLENG, [0x74] = LOP_EATANXY, [0x75] = LOP_EATANXZ,
    [0x76] = LOP_ESUM, [0x78] = LOP_ESQRT, [0x79] = LOP_ERSQRT, [0x7A] = LOP_ERCPR,
    [0x7B] = LOP_WAITP, [0x7C] = LOP_ESIN, [0x7D] = LOP_EATAN, [0x7E] = LOP_EEXP,
};

/* EFU latencies in cycles (ps2tek). Mirrored in recomp/vu_decode.py. */
static uint8_t efu_latency(unsigned lop) {
    switch (lop) {
    case LOP_ESADD: return 11;
    case LOP_ERSADD: case LOP_ELENG: case LOP_ERSQRT: return 18;
    case LOP_ERLENG: return 24;
    case LOP_ESUM: case LOP_ESQRT: case LOP_ERCPR: return 12;
    case LOP_ESIN: return 29;
    case LOP_EEXP: return 44;
    default: return 54;                  /* EATAN, EATANxy, EATANxz */
    }
}

/* ---- vector helpers (the operation semantics are in vu_ops.h) ------------------------------------ */

VU_INLINE __m128 vld(const vf128* v) { return _mm_loadu_ps(v->f); }
VU_INLINE void   vst(vf128* v, __m128 x) { _mm_storeu_ps(v->f, x); }
VU_INLINE void vwrite(vf128* v, __m128 x, unsigned lanes) { vst(v, vu_blend(vld(v), x, lanes)); }
#define vbits vu_bits
#define vblend vu_blend
#define vclean vu_vclean
#define sclean vu_sclean
#define fbits vu_fbits
/* ---- pipelines ---------------------------------------------------------------------------------- */

/* Results in flight (MAC/status flags, clip flags, Q, P) are folded into the visible registers
 * lazily: an instruction that reads one of them first folds in everything of that kind that is
 * ready by its issue cycle (sync_*), and the end of a program folds everything (retire). That is
 * exactly what folding at every cycle would show, without the per-pair cost. */
VU_INLINE void fold_flags(vu_state* st) {
    const vu_flagent* e = &st->fq[st->fq_head++ & 7];
    st->mac = e->mac;
    st->status = (st->status & ~0xFu) | e->zsuo | ((uint32_t)e->zsuo << 6);
}
VU_INLINE void sync_flags(vu_state* st, uint32_t cyc) {
    while (st->fq_head != st->fq_tail && st->fq[st->fq_head & 7].ready <= cyc) fold_flags(st);
}
VU_INLINE void sync_clip(vu_state* st, uint32_t cyc) {
    while (st->cq_head != st->cq_tail && st->cq[st->cq_head & 7].ready <= cyc)
        st->clip = st->cq[st->cq_head++ & 7].clip;
}
VU_INLINE void sync_q(vu_state* st, uint32_t cyc) {
    if (st->q_pend && st->q_ready <= cyc) {
        st->Q = st->q_next;
        st->status = (st->status & ~0x30u) | st->q_flags | (st->q_flags << 6);
        st->q_pend = 0;
    }
}
VU_INLINE void sync_p(vu_state* st, uint32_t cyc) {
    while (st->p_pend && st->p_ready[0] <= cyc) {
        st->P = st->p_next[0];
        st->p_next[0] = st->p_next[1];
        st->p_ready[0] = st->p_ready[1];
        st->p_pend--;
    }
}
VU_INLINE void retire(vu_state* st, uint32_t cyc) {
    sync_flags(st, cyc);
    sync_clip(st, cyc);
    sync_q(st, cyc);
    sync_p(st, cyc);
}

/* Queue the MAC/status result of an FMAC op issued in the current cycle. z and u are lane masks
 * (bit0 = x) of zero and underflowed results, already restricted to the dest fields. At most one
 * FMAC op issues per cycle and results land 4 cycles later, so when all 8 slots are taken the
 * oldest result is certainly visible already: fold it to make room. */
VU_INLINE void push_mac(vu_state* st, __m128 r, unsigned dst, unsigned z, unsigned u) {
    const uint32_t f = vu_macflags(r, dst, z, u);
    if (st->fq_tail - st->fq_head == 8) fold_flags(st);
    vu_flagent* e = &st->fq[st->fq_tail++ & 7];
    e->ready = st->cyc + 4;
    e->mac = (uint16_t)f;
    e->zsuo = (uint16_t)(f >> 16);
}

/* Conditional branches read a VI register before a chain of up to four immediately preceding
 * writes to it (PCSX2 VU documentation, "VI register delays on conditional branches"; games such
 * as Champions: Return to Arms rely on it). A chain starts at a write and continues only while
 * the next instruction reads AND writes the register (IADDI vi5, vi5, 1; SQI ..., (vi5++)); a
 * write that does not read it starts a new chain. A stall in the chain (or on the branch itself)
 * lets the writes complete: the chain starts at the stalled instruction. Flag instructions
 * (FS/FM/FC) write their result without this delay. */
VU_INLINE uint16_t branch_vi(const vu_state* st, unsigned r) {
    uint16_t v = st->vi[r];
    if (!r || st->stalled) return v;
    for (unsigned k = 1; k <= 4; k++) {
        uint32_t h = st->vih[(st->hpos - k) & 3];
        if ((h & 0xFF) != r) break;
        v = (uint16_t)(h >> 16);
        if ((h & 0x300) != 0x200) break;   /* stalled, or a write that does not read r: chain start */
    }
    return v;
}

/* ---- data memory -------------------------------------------------------------------------------- */

/* VU0 sees VU1's registers at data addresses 0x4000-0x43FF (quadwords 0x400-0x43F). */
#define VU0_HIGH(st, a) ((st)->idx == 0 && ((a) & 0x400))

VU_INLINE uint32_t* qwp(const vu_state* st, uint32_t a) {
    return (uint32_t*)(st->data + ((a & st->dmask) << 4));
}
VU_INLINE __m128 load_qw(const vu_state* st, uint32_t a) {
    if (VU0_HIGH(st, a)) { uint32_t t[4]; vu1_reg_read(a & 0x3F, t); return _mm_loadu_ps((const float*)t); }
    return _mm_loadu_ps((const float*)qwp(st, a));
}
VU_INLINE void store_qw(vu_state* st, uint32_t a, __m128 v, unsigned lanes) {
    if (VU0_HIGH(st, a)) { uint32_t t[4]; _mm_storeu_ps((float*)t, v); vu1_reg_write(a & 0x3F, t, lanes); return; }
    float* p = (float*)qwp(st, a);
    _mm_storeu_ps(p, vblend(_mm_loadu_ps(p), v, lanes));
}
VU_INLINE uint32_t load_w(const vu_state* st, uint32_t a, unsigned lane) {
    if (VU0_HIGH(st, a)) { uint32_t t[4]; vu1_reg_read(a & 0x3F, t); return t[lane]; }
    return qwp(st, a)[lane];
}
VU_INLINE void store_w(vu_state* st, uint32_t a, unsigned lanes, uint32_t v) {
    if (VU0_HIGH(st, a)) { uint32_t t[4] = { v, v, v, v }; vu1_reg_write(a & 0x3F, t, lanes); return; }
    uint32_t* p = qwp(st, a);
    if (lanes & 1) p[0] = v;
    if (lanes & 2) p[1] = v;
    if (lanes & 4) p[2] = v;
    if (lanes & 8) p[3] = v;
}

static void log_invalid(const vu_state* st, const vu_op* op, const char* what) {
    static int n;
    if (n++ < 8)
        fprintf(stderr, "[vu] VU%d: invalid %s instruction at 0x%04X (pair %08X %08X)\n", st->idx, what,
                (unsigned)(op - st->cache) * 8, (uint32_t)(op->raw >> 32), (uint32_t)op->raw);
}

/* ---- upper handlers ----------------------------------------------------------------------------- */

VU_INLINE __m128 opb(vu_state* st, const vu_op* op, int bm) {
    if (bm == BM_VEC) return vclean(vld(&st->vf[op->uft]));
    if (bm == BM_BC)  return vclean(vbits(st->vf[op->uft].u[op->ubc]));
    if (bm == BM_Q)   { sync_q(st, st->cyc); return vclean(vbits(st->Q)); }
    return vclean(vbits(st->I));
}
VU_INLINE vf128* utarget(vu_state* st, const vu_op* op) {
    return op->uacc ? &st->acc : op->ufd ? &st->vf[op->ufd] : NULL;
}

/* ADD/SUB/MUL/MADD/MSUB in all operand forms; flags per lane: Z, S, O (result clamped to
 * +-FLT_MAX), U (result flushed to zero although the exact result was not zero). */
VU_INLINE vf128* fmac(vu_state* st, const vu_op* op, __m128* out, int kind, int bm) {
    const unsigned dst = op->udst;
    __m128 a = vclean(vld(&st->vf[op->ufs])), b = opb(st, op, bm);
    unsigned z, u;
    __m128 r = vu_fmac(kind, a, b, kind >= VU_K_MADD ? vld(&st->acc) : _mm_setzero_ps(), dst, &z, &u);
    push_mac(st, r, dst, z, u);
    *out = r;
    return utarget(st, op);
}
#define FMAC_FN(name, kind, bm) \
    static vf128* name(vu_state* st, const vu_op* op, __m128* out) { return fmac(st, op, out, kind, bm); }
#define FMAC_ALL(n, kind) FMAC_FN(u_##n##_v, kind, BM_VEC) FMAC_FN(u_##n##_bc, kind, BM_BC) \
                          FMAC_FN(u_##n##_q, kind, BM_Q) FMAC_FN(u_##n##_i, kind, BM_I)
FMAC_ALL(add, VU_K_ADD)
FMAC_ALL(sub, VU_K_SUB)
FMAC_ALL(mul, VU_K_MUL)
FMAC_ALL(madd, VU_K_MADD)
FMAC_ALL(msub, VU_K_MSUB)
static const vu_ufn FMAC_TAB[5][4] = {
    { u_add_v, u_add_bc, u_add_q, u_add_i }, { u_sub_v, u_sub_bc, u_sub_q, u_sub_i },
    { u_mul_v, u_mul_bc, u_mul_q, u_mul_i }, { u_madd_v, u_madd_bc, u_madd_q, u_madd_i },
    { u_msub_v, u_msub_bc, u_msub_q, u_msub_i },
};

/* MAX/MINI compare raw bits (sign-magnitude) and pass values through untouched; no flags. */
VU_INLINE vf128* maxmin(vu_state* st, const vu_op* op, __m128* out, int is_max, int bm) {
    __m128 a = vld(&st->vf[op->ufs]);
    __m128 b = bm == BM_VEC ? vld(&st->vf[op->uft]) : bm == BM_BC ? vbits(st->vf[op->uft].u[op->ubc]) : vbits(st->I);
    *out = vu_maxmin(is_max, a, b);
    return op->ufd ? &st->vf[op->ufd] : NULL;
}
static vf128* u_max_v(vu_state* st, const vu_op* op, __m128* o)   { return maxmin(st, op, o, 1, BM_VEC); }
static vf128* u_max_bc(vu_state* st, const vu_op* op, __m128* o)  { return maxmin(st, op, o, 1, BM_BC); }
static vf128* u_max_i(vu_state* st, const vu_op* op, __m128* o)   { return maxmin(st, op, o, 1, BM_I); }
static vf128* u_mini_v(vu_state* st, const vu_op* op, __m128* o)  { return maxmin(st, op, o, 0, BM_VEC); }
static vf128* u_mini_bc(vu_state* st, const vu_op* op, __m128* o) { return maxmin(st, op, o, 0, BM_BC); }
static vf128* u_mini_i(vu_state* st, const vu_op* op, __m128* o)  { return maxmin(st, op, o, 0, BM_I); }

/* OPMULA ACC.xyz = fs.yzx * ft.zxy; OPMSUB fd.xyz = ACC - fs.yzx * ft.zxy (cross product pair). */
VU_INLINE vf128* opm(vu_state* st, const vu_op* op, __m128* out, int sub) {
    __m128 a = vclean(vld(&st->vf[op->ufs])), b = vclean(vld(&st->vf[op->uft]));
    const unsigned dst = op->udst;
    unsigned z, u;
    __m128 r = vu_opm(sub, a, b, sub ? vld(&st->acc) : _mm_setzero_ps(), dst, &z, &u);
    push_mac(st, r, dst, z, u);
    *out = r;
    return sub ? (op->ufd ? &st->vf[op->ufd] : NULL) : &st->acc;
}
static vf128* u_opmula(vu_state* st, const vu_op* op, __m128* o) { return opm(st, op, o, 0); }
static vf128* u_opmsub(vu_state* st, const vu_op* op, __m128* o) { return opm(st, op, o, 1); }

static vf128* u_abs(vu_state* st, const vu_op* op, __m128* out) {
    *out = vu_abs(vld(&st->vf[op->ufs]));
    return op->uft ? &st->vf[op->uft] : NULL;
}

/* FTOIn: truncate toward zero, saturating to 0x7FFFFFFF / 0x80000000. */
VU_INLINE vf128* ftoi(vu_state* st, const vu_op* op, __m128* out, float scale) {
    *out = vu_ftoi(vld(&st->vf[op->ufs]), scale);
    return op->uft ? &st->vf[op->uft] : NULL;
}
static vf128* u_ftoi0(vu_state* st, const vu_op* op, __m128* o)  { return ftoi(st, op, o, 1.0f); }
static vf128* u_ftoi4(vu_state* st, const vu_op* op, __m128* o)  { return ftoi(st, op, o, 16.0f); }
static vf128* u_ftoi12(vu_state* st, const vu_op* op, __m128* o) { return ftoi(st, op, o, 4096.0f); }
static vf128* u_ftoi15(vu_state* st, const vu_op* op, __m128* o) { return ftoi(st, op, o, 32768.0f); }

VU_INLINE vf128* itof(vu_state* st, const vu_op* op, __m128* out, float scale) {
    *out = vu_itof(vld(&st->vf[op->ufs]), scale);
    return op->uft ? &st->vf[op->uft] : NULL;
}
static vf128* u_itof0(vu_state* st, const vu_op* op, __m128* o)  { return itof(st, op, o, 1.0f); }
static vf128* u_itof4(vu_state* st, const vu_op* op, __m128* o)  { return itof(st, op, o, 1.0f / 16.0f); }
static vf128* u_itof12(vu_state* st, const vu_op* op, __m128* o) { return itof(st, op, o, 1.0f / 4096.0f); }
static vf128* u_itof15(vu_state* st, const vu_op* op, __m128* o) { return itof(st, op, o, 1.0f / 32768.0f); }

/* CLIP: judge fs.xyz against +-|ft.w|; the clip flag register shifts in 6 new bits. */
static vf128* u_clip(vu_state* st, const vu_op* op, __m128* out) {
    (void)out;
    const vf128* s = &st->vf[op->ufs];
    uint32_t j = vu_clip_judge(s->u[0], s->u[1], s->u[2], st->vf[op->uft].u[3]);
    st->clip_pipe = ((st->clip_pipe << 6) | j) & 0xFFFFFF;
    if (st->cq_tail - st->cq_head == 8) st->clip = st->cq[st->cq_head++ & 7].clip;   /* landed (see push_mac) */
    vu_clipent* e = &st->cq[st->cq_tail++ & 7];
    e->ready = st->cyc + 4;
    e->clip = st->clip_pipe;
    return NULL;
}
static vf128* u_invalid(vu_state* st, const vu_op* op, __m128* out) {
    (void)out;
    log_invalid(st, op, "upper");
    return NULL;
}

/* ---- lower handlers ------------------------------------------------------------------------------ */

#define LFN(name) static uint32_t name(vu_state* st, const vu_op* op, uint32_t npc)
#define VI(r) (st->vi[(r)])
#define SETVI(r, v) do { if (r) st->vi[(r)] = (uint16_t)(v); } while (0)

LFN(l_lq) {
    __m128 v = load_qw(st, (uint32_t)(VI(op->ls) + op->imm));
    if (op->lt) vwrite(&st->vf[op->lt], v, op->ldst);
    return npc;
}
LFN(l_sq) { store_qw(st, (uint32_t)(VI(op->lt) + op->imm), vld(&st->vf[op->ls]), op->ldst); return npc; }
LFN(l_ilw) { SETVI(op->lt, load_w(st, (uint32_t)(VI(op->ls) + op->imm), op->fsf)); return npc; }
LFN(l_isw) { store_w(st, (uint32_t)(VI(op->ls) + op->imm), op->ldst, VI(op->lt)); return npc; }
LFN(l_iaddiu) { SETVI(op->lt, VI(op->ls) + op->imm); return npc; }
LFN(l_isubiu) { SETVI(op->lt, VI(op->ls) - op->imm); return npc; }
LFN(l_iadd) { SETVI(op->ld, VI(op->ls) + VI(op->lt)); return npc; }
LFN(l_isub) { SETVI(op->ld, VI(op->ls) - VI(op->lt)); return npc; }
LFN(l_iand) { SETVI(op->ld, VI(op->ls) & VI(op->lt)); return npc; }
LFN(l_ior)  { SETVI(op->ld, VI(op->ls) | VI(op->lt)); return npc; }
/* flag readers fold in what has landed by now (see retire) */
#define CLIP_NOW() (sync_clip(st, st->cyc), st->clip)
#define STATUS_NOW() (sync_flags(st, st->cyc), sync_q(st, st->cyc), st->status)
#define MAC_NOW() (sync_flags(st, st->cyc), st->mac)
LFN(l_fceq) { st->vi[1] = (CLIP_NOW() & 0xFFFFFF) == (uint32_t)op->imm; return npc; }
LFN(l_fcand) { st->vi[1] = (CLIP_NOW() & (uint32_t)op->imm) != 0; return npc; }
LFN(l_fcor) { st->vi[1] = ((CLIP_NOW() | (uint32_t)op->imm) & 0xFFFFFF) == 0xFFFFFF; return npc; }
LFN(l_fcset) {
    st->clip = st->clip_pipe = (uint32_t)op->imm;
    st->cq_head = st->cq_tail;                      /* in-flight CLIP results are overwritten */
    return npc;
}
LFN(l_fcget) { SETVI(op->lt, CLIP_NOW() & 0xFFF); return npc; }
LFN(l_fseq)  { SETVI(op->lt, (STATUS_NOW() & 0xFFF) == (uint32_t)op->imm); return npc; }
LFN(l_fsand) { SETVI(op->lt, STATUS_NOW() & (uint32_t)op->imm); return npc; }
LFN(l_fsor)  { SETVI(op->lt, ((STATUS_NOW() | (uint32_t)op->imm) & 0xFFF) == 0xFFF); return npc; }
LFN(l_fsset) { st->status = (STATUS_NOW() & 0x3F) | ((uint32_t)op->imm & 0xFC0); return npc; }
LFN(l_fmeq)  { SETVI(op->lt, (MAC_NOW() & 0xFFFF) == VI(op->ls)); return npc; }
LFN(l_fmand) { SETVI(op->lt, MAC_NOW() & VI(op->ls)); return npc; }
LFN(l_fmor)  { SETVI(op->lt, ((MAC_NOW() | VI(op->ls)) & 0xFFFF) == 0xFFFF); return npc; }
/* Branches: npc is the address after the delay slot, which is also the link value. */
LFN(l_b)    { (void)st; (void)npc; return (uint32_t)op->imm; }
LFN(l_bal)  { SETVI(op->lt, npc); return (uint32_t)op->imm; }
LFN(l_jr)   { (void)npc; return VI(op->ls) & st->pcmask; }
LFN(l_jalr) { uint32_t to = VI(op->ls) & st->pcmask; SETVI(op->lt, npc); return to; }
LFN(l_ibeq)  { return branch_vi(st, op->lt) == branch_vi(st, op->ls) ? (uint32_t)op->imm : npc; }
LFN(l_ibne)  { return branch_vi(st, op->lt) != branch_vi(st, op->ls) ? (uint32_t)op->imm : npc; }
LFN(l_ibltz) { return (int16_t)branch_vi(st, op->ls) < 0 ? (uint32_t)op->imm : npc; }
LFN(l_ibgtz) { return (int16_t)branch_vi(st, op->ls) > 0 ? (uint32_t)op->imm : npc; }
LFN(l_iblez) { return (int16_t)branch_vi(st, op->ls) <= 0 ? (uint32_t)op->imm : npc; }
LFN(l_ibgez) { return (int16_t)branch_vi(st, op->ls) >= 0 ? (uint32_t)op->imm : npc; }
LFN(l_move) { vwrite(&st->vf[op->lt], vld(&st->vf[op->ls]), op->ldst); return npc; }
LFN(l_mr32) {
    __m128 v = vld(&st->vf[op->ls]);
    vwrite(&st->vf[op->lt], _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 3, 2, 1)), op->ldst);   /* (y, z, w, x) */
    return npc;
}
LFN(l_lqi) {
    uint32_t a = VI(op->ls);
    __m128 v = load_qw(st, a);
    SETVI(op->ls, a + 1);
    if (op->lt) vwrite(&st->vf[op->lt], v, op->ldst);
    return npc;
}
LFN(l_lqd) {
    uint32_t a = (VI(op->ls) - 1u) & 0xFFFF;
    __m128 v = load_qw(st, a);
    SETVI(op->ls, a);
    if (op->lt) vwrite(&st->vf[op->lt], v, op->ldst);
    return npc;
}
LFN(l_sqi) {
    uint32_t a = VI(op->lt);
    store_qw(st, a, vld(&st->vf[op->ls]), op->ldst);
    SETVI(op->lt, a + 1);
    return npc;
}
LFN(l_sqd) {
    uint32_t a = (VI(op->lt) - 1u) & 0xFFFF;
    store_qw(st, a, vld(&st->vf[op->ls]), op->ldst);
    SETVI(op->lt, a);
    return npc;
}

/* FDIV: Q arrives 7 (DIV, SQRT) or 13 (RSQRT) cycles after issue, with the I/D status flags. */
VU_INLINE void start_fdiv(vu_state* st, uint32_t lat, uint32_t q, uint32_t flags) {
    sync_q(st, st->cyc);                         /* the previous result (FDIV stalls until it lands) */
    st->q_pend = 1; st->q_next = q; st->q_ready = st->cyc + lat; st->q_flags = flags;
}
LFN(l_div) {
    uint32_t fl, q = vu_div(st->vf[op->ls].u[op->fsf], st->vf[op->lt].u[op->ftf], &fl);
    start_fdiv(st, 7, q, fl);
    return npc;
}
LFN(l_sqrt) {
    uint32_t fl, q = vu_sqrt(st->vf[op->lt].u[op->ftf], &fl);
    start_fdiv(st, 7, q, fl);
    return npc;
}
LFN(l_rsqrt) {
    uint32_t fl, q = vu_rsqrt(st->vf[op->ls].u[op->fsf], st->vf[op->lt].u[op->ftf], &fl);
    start_fdiv(st, 13, q, fl);
    return npc;
}
LFN(l_mtir) { SETVI(op->lt, st->vf[op->ls].u[op->fsf]); return npc; }
LFN(l_mfir) { vwrite(&st->vf[op->lt], vbits((uint32_t)(int32_t)(int16_t)VI(op->ls)), op->ldst); return npc; }

LFN(l_rnext) { st->R = vu_rnext(st->R); if (op->lt) vwrite(&st->vf[op->lt], vbits(st->R), op->ldst); return npc; }
LFN(l_rget)  { vwrite(&st->vf[op->lt], vbits(st->R), op->ldst); return npc; }
LFN(l_mfp)   { sync_p(st, st->cyc); vwrite(&st->vf[op->lt], vbits(st->P), op->ldst); return npc; }
LFN(l_rinit) { st->R = 0x3F800000u | (st->vf[op->ls].u[op->fsf] & 0x7FFFFF); return npc; }
LFN(l_rxor)  { st->R = 0x3F800000u | ((st->R ^ st->vf[op->ls].u[op->fsf]) & 0x7FFFFF); return npc; }
LFN(l_xtop)  { SETVI(op->lt, st->top); return npc; }
LFN(l_xitop) { SETVI(op->lt, st->itop); return npc; }
LFN(l_xgkick) {
    if (st->idx == 1) vu_xgkick(VI(op->ls));
    else log_invalid(st, op, "(XGKICK on VU0)");
    return npc;
}

/* EFU (VU1 only on hardware; harmless on VU0): P arrives op->imm cycles after issue. The op may
 * issue one cycle before the previous EFU result lands (see stall_until), so results queue. */
static int efu_kind(unsigned lop) {
    switch (lop) {
    case LOP_ESADD: return VU_E_ESADD;     case LOP_ERSADD: return VU_E_ERSADD;
    case LOP_ELENG: return VU_E_ELENG;     case LOP_ERLENG: return VU_E_ERLENG;
    case LOP_EATANXY: return VU_E_EATANXY; case LOP_EATANXZ: return VU_E_EATANXZ;
    case LOP_ESUM: return VU_E_ESUM;       case LOP_ESQRT: return VU_E_ESQRT;
    case LOP_ERSQRT: return VU_E_ERSQRT;   case LOP_ERCPR: return VU_E_ERCPR;
    case LOP_ESIN: return VU_E_ESIN;       case LOP_EATAN: return VU_E_EATAN;
    default: return VU_E_EEXP;
    }
}
LFN(l_efu) {
    const vf128* v = &st->vf[op->ls];
    const uint32_t p = vu_efu(efu_kind(op->lo), v->u[0], v->u[1], v->u[2], v->u[3], v->u[op->fsf]);
    sync_p(st, st->cyc);
    const uint32_t k = st->p_pend++;         /* at most one older result is still in flight */
    st->p_next[k] = p;
    st->p_ready[k] = st->cyc + (uint32_t)op->imm;
    return npc;
}LFN(l_invalid) { log_invalid(st, op, "lower"); return npc; }

static const vu_lfn LOWER_FN[LOP_COUNT] = {
    [LOP_INVALID] = l_invalid,
    [LOP_LQ] = l_lq, [LOP_SQ] = l_sq, [LOP_ILW] = l_ilw, [LOP_ISW] = l_isw,
    [LOP_IADDIU] = l_iaddiu, [LOP_ISUBIU] = l_isubiu,
    [LOP_FCEQ] = l_fceq, [LOP_FCSET] = l_fcset, [LOP_FCAND] = l_fcand, [LOP_FCOR] = l_fcor,
    [LOP_FSEQ] = l_fseq, [LOP_FSSET] = l_fsset, [LOP_FSAND] = l_fsand, [LOP_FSOR] = l_fsor,
    [LOP_FMEQ] = l_fmeq, [LOP_FMAND] = l_fmand, [LOP_FMOR] = l_fmor, [LOP_FCGET] = l_fcget,
    [LOP_B] = l_b, [LOP_BAL] = l_bal, [LOP_JR] = l_jr, [LOP_JALR] = l_jalr,
    [LOP_IBEQ] = l_ibeq, [LOP_IBNE] = l_ibne, [LOP_IBLTZ] = l_ibltz, [LOP_IBGTZ] = l_ibgtz,
    [LOP_IBLEZ] = l_iblez, [LOP_IBGEZ] = l_ibgez,
    [LOP_IADD] = l_iadd, [LOP_ISUB] = l_isub, [LOP_IADDI] = l_iaddiu, [LOP_IAND] = l_iand, [LOP_IOR] = l_ior,
    [LOP_MOVE] = l_move, [LOP_MR32] = l_mr32, [LOP_LQI] = l_lqi, [LOP_SQI] = l_sqi,
    [LOP_LQD] = l_lqd, [LOP_SQD] = l_sqd, [LOP_DIV] = l_div, [LOP_SQRT] = l_sqrt, [LOP_RSQRT] = l_rsqrt,
    [LOP_MTIR] = l_mtir, [LOP_MFIR] = l_mfir, [LOP_ILWR] = l_ilw, [LOP_ISWR] = l_isw,
    [LOP_RNEXT] = l_rnext, [LOP_RGET] = l_rget, [LOP_RINIT] = l_rinit, [LOP_RXOR] = l_rxor,
    [LOP_MFP] = l_mfp, [LOP_XTOP] = l_xtop, [LOP_XITOP] = l_xitop, [LOP_XGKICK] = l_xgkick,
    [LOP_ESADD] = l_efu, [LOP_ERSADD] = l_efu, [LOP_ELENG] = l_efu, [LOP_ERLENG] = l_efu,
    [LOP_EATANXY] = l_efu, [LOP_EATANXZ] = l_efu, [LOP_ESUM] = l_efu, [LOP_ESQRT] = l_efu,
    [LOP_ERSQRT] = l_efu, [LOP_ERCPR] = l_efu, [LOP_ESIN] = l_efu, [LOP_EATAN] = l_efu, [LOP_EEXP] = l_efu,
    /* LOP_NOP, LOP_LOI, LOP_WAITQ, LOP_WAITP: no handler (stalls are handled by the main loop) */
};

/* ---- decoder ---------------------------------------------------------------------------------- */

static void add_src(vu_op* op, unsigned reg, unsigned mask) {
    if (!reg || !mask) return;           /* vf0 is constant: never stalls */
    for (unsigned k = 0; k < op->nsrc; k++)
        if (op->sreg[k] == reg) { op->smask[k] |= (uint8_t)mask; return; }
    if (op->nsrc < 4) { op->sreg[op->nsrc] = (uint8_t)reg; op->smask[op->nsrc++] = (uint8_t)mask; }
}
static void add_visrc(vu_op* op, unsigned reg) {
    if (!reg) return;
    if (!op->visrc[0]) op->visrc[0] = (uint8_t)reg; else op->visrc[1] = (uint8_t)reg;
}
static void set_viw(vu_op* op, unsigned reg, unsigned lat, int chain) {
    if (!reg) return;
    op->viw = (uint8_t)reg; op->viwlat = (uint8_t)lat;
    if (chain) op->vichain = (uint8_t)reg;
}
static void set_lw(vu_op* op, unsigned reg, unsigned mask) {
    if (reg && mask) { op->lwreg = (uint8_t)reg; op->lwmask = (uint8_t)mask; }
}
static unsigned lowest_lane(unsigned m) { return m & 1 ? 0 : m & 2 ? 1 : m & 4 ? 2 : m & 8 ? 3 : 0; }

static void decode_upper(vu_op* op, uint32_t u) {
    unsigned dst = REV4[(u >> 21) & 15], ft = (u >> 16) & 31, fs = (u >> 11) & 31, fd = (u >> 6) & 31;
    unsigned f6 = u & 63;
    uent e = { UOP_INVALID, 0, 0 };
    if (f6 < 0x30) e = UPPER1[f6];
    else if (f6 >= 0x3C) {
        unsigned k = (((u >> 6) & 31) << 2) | (u & 3);
        if (k < 0x30) e = UPPER2[k];
    }
    op->up = e.op; op->ubm = e.bm; op->uacc = e.acc; op->udst = (uint8_t)dst;
    op->ufd = (uint8_t)fd; op->ufs = (uint8_t)fs; op->uft = (uint8_t)ft; op->ubc = u & 3;
    switch (e.op) {
    case UOP_ADD: case UOP_SUB: case UOP_MUL: case UOP_MADD: case UOP_MSUB: case UOP_MAX: case UOP_MINI:
        add_src(op, fs, dst);
        if (e.bm == BM_VEC) add_src(op, ft, dst);
        else if (e.bm == BM_BC) add_src(op, ft, 1u << (u & 3));
        if (!e.acc && fd && dst) { op->uwreg = (uint8_t)fd; op->uwmask = (uint8_t)dst; }
        if (e.op == UOP_MAX || e.op == UOP_MINI) {
            static const vu_ufn mx[2][4] = { { u_mini_v, u_mini_bc, NULL, u_mini_i }, { u_max_v, u_max_bc, NULL, u_max_i } };
            op->ufn = mx[e.op == UOP_MAX][e.bm];
        } else {
            op->ufn = FMAC_TAB[e.op - UOP_ADD][e.bm];
        }
        break;
    case UOP_OPMULA:
        add_src(op, fs, 7); add_src(op, ft, 7);
        op->ufn = u_opmula;
        break;
    case UOP_OPMSUB:
        add_src(op, fs, 7); add_src(op, ft, 7);
        if (fd && dst) { op->uwreg = (uint8_t)fd; op->uwmask = (uint8_t)dst; }
        op->ufn = u_opmsub;
        break;
    case UOP_ABS: case UOP_FTOI0: case UOP_FTOI4: case UOP_FTOI12: case UOP_FTOI15:
    case UOP_ITOF0: case UOP_ITOF4: case UOP_ITOF12: case UOP_ITOF15: {
        static const vu_ufn cv[9] = { u_abs, u_ftoi0, u_ftoi4, u_ftoi12, u_ftoi15, u_itof0, u_itof4, u_itof12, u_itof15 };
        add_src(op, fs, dst);
        if (ft && dst) { op->uwreg = (uint8_t)ft; op->uwmask = (uint8_t)dst; }
        op->ufn = cv[e.op - UOP_ABS];
        break;
    }
    case UOP_CLIP:
        add_src(op, fs, 7); add_src(op, ft, 8);
        op->ufn = u_clip;
        break;
    case UOP_NOP:
        break;
    default:
        op->ufn = u_invalid;
        break;
    }
}

static void decode_lower(vu_op* op, uint32_t l, uint32_t pc, uint32_t pcmask) {
    unsigned op7 = l >> 25, ldst = REV4[(l >> 21) & 15];
    unsigned t = (l >> 16) & 31, s = (l >> 11) & 31, it = t & 15, is = s & 15, id = (l >> 6) & 15;
    int32_t imm11 = (int32_t)(l << 21) >> 21;
    uint32_t target = (pc + 1u + (uint32_t)imm11) & pcmask;
    unsigned lop;
    if (op7 == 0x40) {
        unsigned f6 = l & 63;
        lop = f6 >= 0x3C ? LOWER_S2[(((l >> 6) & 31) << 2) | (l & 3)] : LOWER_S1[f6];
    } else {
        lop = LOWER7[op7];
    }
    op->ldst = (uint8_t)ldst;
    op->fsf = (l >> 21) & 3;
    op->ftf = (l >> 23) & 3;
    switch (lop) {
    case LOP_LQ:
        op->lt = (uint8_t)t; op->ls = (uint8_t)is; op->imm = imm11;
        add_visrc(op, is); set_lw(op, t, ldst);
        break;
    case LOP_SQ:
        op->ls = (uint8_t)s; op->lt = (uint8_t)it; op->imm = imm11;
        add_src(op, s, ldst); add_visrc(op, it);
        break;
    case LOP_ILW: case LOP_ILWR:
        op->lt = (uint8_t)it; op->ls = (uint8_t)is; op->imm = lop == LOP_ILW ? imm11 : 0;
        op->fsf = (uint8_t)lowest_lane(ldst);
        add_visrc(op, is); set_viw(op, it, 4, 1);
        break;
    case LOP_ISW: case LOP_ISWR:
        op->lt = (uint8_t)it; op->ls = (uint8_t)is; op->imm = lop == LOP_ISW ? imm11 : 0;
        add_visrc(op, is); add_visrc(op, it);
        break;
    case LOP_IADDIU: case LOP_ISUBIU:
        op->lt = (uint8_t)it; op->ls = (uint8_t)is; op->imm = (int32_t)(((l >> 10) & 0x7800) | (l & 0x7FF));
        add_visrc(op, is); set_viw(op, it, 1, 1);
        break;
    case LOP_FCEQ: case LOP_FCAND: case LOP_FCOR:
        op->imm = (int32_t)(l & 0xFFFFFF); set_viw(op, 1, 1, 0);
        break;
    case LOP_FCSET:
        op->imm = (int32_t)(l & 0xFFFFFF);
        break;
    case LOP_FCGET:
        op->lt = (uint8_t)it; set_viw(op, it, 1, 0);
        break;
    case LOP_FSEQ: case LOP_FSAND: case LOP_FSOR:
        op->lt = (uint8_t)it; op->imm = (int32_t)(((l >> 10) & 0x800) | (l & 0x7FF));
        set_viw(op, it, 1, 0);
        break;
    case LOP_FSSET:
        op->imm = (int32_t)(((l >> 10) & 0x800) | (l & 0x7FF));
        break;
    case LOP_FMEQ: case LOP_FMAND: case LOP_FMOR:
        op->lt = (uint8_t)it; op->ls = (uint8_t)is; add_visrc(op, is); set_viw(op, it, 1, 0);
        break;
    case LOP_B:
        op->imm = (int32_t)target;
        break;
    case LOP_BAL:
        op->lt = (uint8_t)it; op->imm = (int32_t)target; set_viw(op, it, 1, 1);
        break;
    case LOP_JR:
        op->ls = (uint8_t)is; add_visrc(op, is);
        break;
    case LOP_JALR:
        op->lt = (uint8_t)it; op->ls = (uint8_t)is; add_visrc(op, is); set_viw(op, it, 1, 1);
        break;
    case LOP_IBEQ: case LOP_IBNE:
        op->lt = (uint8_t)it; op->ls = (uint8_t)is; op->imm = (int32_t)target;
        add_visrc(op, it); add_visrc(op, is);
        break;
    case LOP_IBLTZ: case LOP_IBGTZ: case LOP_IBLEZ: case LOP_IBGEZ:
        op->ls = (uint8_t)is; op->imm = (int32_t)target; add_visrc(op, is);
        break;
    case LOP_IADD: case LOP_ISUB: case LOP_IAND: case LOP_IOR:
        op->ld = (uint8_t)id; op->ls = (uint8_t)is; op->lt = (uint8_t)it;
        add_visrc(op, is); add_visrc(op, it); set_viw(op, id, 1, 1);
        break;
    case LOP_IADDI:
        op->lt = (uint8_t)it; op->ls = (uint8_t)is; op->imm = (int32_t)(l << 21) >> 27;
        add_visrc(op, is); set_viw(op, it, 1, 1);
        break;
    case LOP_MOVE: case LOP_MR32:
        if (!t || !ldst) { lop = LOP_NOP; break; }        /* MOVE.none vf0, vf0 is the canonical NOP */
        op->lt = (uint8_t)t; op->ls = (uint8_t)s;
        add_src(op, s, lop == LOP_MOVE ? ldst : (((ldst << 1) | (ldst >> 3)) & 15));
        set_lw(op, t, ldst);
        break;
    case LOP_LQI: case LOP_LQD:
        op->lt = (uint8_t)t; op->ls = (uint8_t)is;
        add_visrc(op, is); set_lw(op, t, ldst); set_viw(op, is, 1, 1);
        break;
    case LOP_SQI: case LOP_SQD:
        op->ls = (uint8_t)s; op->lt = (uint8_t)it;
        add_src(op, s, ldst); add_visrc(op, it); set_viw(op, it, 1, 1);
        break;
    case LOP_DIV: case LOP_RSQRT:
        op->ls = (uint8_t)s; op->lt = (uint8_t)t;
        add_src(op, s, 1u << op->fsf); add_src(op, t, 1u << op->ftf); op->stall = VU_STALL_Q;
        break;
    case LOP_SQRT:
        op->lt = (uint8_t)t; add_src(op, t, 1u << op->ftf); op->stall = VU_STALL_Q;
        break;
    case LOP_WAITQ:
        op->stall = VU_STALL_Q;
        break;
    case LOP_MTIR:
        op->lt = (uint8_t)it; op->ls = (uint8_t)s; add_src(op, s, 1u << op->fsf); set_viw(op, it, 1, 1);
        break;
    case LOP_MFIR:
        if (!t || !ldst) { lop = LOP_NOP; break; }
        op->lt = (uint8_t)t; op->ls = (uint8_t)is; add_visrc(op, is); set_lw(op, t, ldst);
        break;
    case LOP_RGET: case LOP_MFP:
        if (!t || !ldst) { lop = LOP_NOP; break; }
        op->lt = (uint8_t)t; set_lw(op, t, ldst);
        break;
    case LOP_RNEXT:                                          /* advances R even without a destination */
        op->lt = (uint8_t)t; set_lw(op, t, ldst);
        break;
    case LOP_RINIT: case LOP_RXOR:
        op->ls = (uint8_t)s; add_src(op, s, 1u << op->fsf);
        break;
    case LOP_XTOP: case LOP_XITOP:
        op->lt = (uint8_t)it; set_viw(op, it, 1, 1);
        break;
    case LOP_XGKICK:
        op->ls = (uint8_t)is; add_visrc(op, is);
        break;
    case LOP_ESADD: case LOP_ERSADD: case LOP_ELENG: case LOP_ERLENG:
    case LOP_EATANXY: case LOP_EATANXZ: case LOP_ESUM:
        op->ls = (uint8_t)s; op->stall = VU_STALL_EFU; op->imm = efu_latency(lop);
        add_src(op, s, lop == LOP_EATANXY ? 3 : lop == LOP_EATANXZ ? 5 : lop == LOP_ESUM ? 15 : 7);
        break;
    case LOP_ESQRT: case LOP_ERSQRT: case LOP_ERCPR: case LOP_ESIN: case LOP_EATAN: case LOP_EEXP:
        op->ls = (uint8_t)s; op->stall = VU_STALL_EFU; op->imm = efu_latency(lop);
        add_src(op, s, 1u << op->fsf);
        break;
    case LOP_WAITP:
        op->stall = VU_STALL_P;
        break;
    default:
        break;
    }
    op->lo = (uint8_t)lop;
    op->lfn = LOWER_FN[lop];
}

static void vu_decode(vu_op* op, uint64_t raw, uint32_t pc, uint32_t pcmask) {
    memset(op, 0, sizeof *op);
    op->raw = raw;
    uint32_t u = (uint32_t)(raw >> 32), l = (uint32_t)raw;
    op->bits = (uint8_t)(u >> 27);
    decode_upper(op, u);
    if (op->bits & VUB_I) { op->lo = LOP_LOI; op->imm = (int32_t)l; }
    else decode_lower(op, l, pc, pcmask);
    op->check = op->nsrc || op->visrc[0] || op->stall;
    op->writes = op->uwreg || op->lwreg || op->viw;
    op->vichain_rd = op->vichain && (op->visrc[0] == op->vichain || op->visrc[1] == op->vichain);
}

void vu_init_state(vu_state* st) {
    for (uint32_t pc = 0; pc <= st->pcmask; pc++) {
        uint64_t raw;
        memcpy(&raw, st->micro + pc * 8u, 8);
        vu_decode(&st->cache[pc], raw, pc, st->pcmask);
    }
    memset(&st->vf[0], 0, sizeof st->vf[0]);
    st->vf[0].f[3] = 1.0f;
    st->vi[0] = 0;
    st->R = 0x3F800000u;
    st->inited = 1;
}

/* ---- interpreter -------------------------------------------------------------------------------- */

/* Cycle at which every operand of op is available and its unit (FDIV/EFU) is free. */
VU_INLINE uint32_t stall_until(const vu_state* st, const vu_op* op, uint32_t t) {
    for (unsigned k = 0; k < op->nsrc; k++) {
        unsigned r = op->sreg[k];
        if (st->vf_ready_any[r] > t) {
            const uint32_t* rd = st->vf_ready[r];
            unsigned m = op->smask[k];
            if ((m & 1) && rd[0] > t) t = rd[0];
            if ((m & 2) && rd[1] > t) t = rd[1];
            if ((m & 4) && rd[2] > t) t = rd[2];
            if ((m & 8) && rd[3] > t) t = rd[3];
        }
    }
    if (st->vi_ready[op->visrc[0]] > t) t = st->vi_ready[op->visrc[0]];
    if (st->vi_ready[op->visrc[1]] > t) t = st->vi_ready[op->visrc[1]];
    switch (op->stall) {
    case VU_STALL_Q:   if (st->q_pend && st->q_ready > t) t = st->q_ready; break;          /* WAITQ, busy FDIV */
    /* busy EFU and WAITP: "a stall lasts for one cycle less than usual" (ps2tek; no writeback
     * hazard check), i.e. until one cycle before the newest result lands */
    case VU_STALL_EFU:
    case VU_STALL_P:
        if (st->p_pend && st->p_ready[st->p_pend - 1] - 1 > t) t = st->p_ready[st->p_pend - 1] - 1;
        break;
    default: break;
    }
    return t;
}

static void mark_writes(vu_state* st, const vu_op* op, uint32_t cyc) {
    if (op->uwreg) {
        uint32_t* r = st->vf_ready[op->uwreg];
        unsigned m = op->uwmask;
        if (m & 1) r[0] = cyc + 4;
        if (m & 2) r[1] = cyc + 4;
        if (m & 4) r[2] = cyc + 4;
        if (m & 8) r[3] = cyc + 4;
        st->vf_ready_any[op->uwreg] = cyc + 4;   /* every VF write has the same latency: newest is latest */
    }
    if (op->lwreg) {
        uint32_t* r = st->vf_ready[op->lwreg];
        unsigned m = op->lwmask;
        if (m & 1) r[0] = cyc + 4;
        if (m & 2) r[1] = cyc + 4;
        if (m & 4) r[2] = cyc + 4;
        if (m & 8) r[3] = cyc + 4;
        st->vf_ready_any[op->lwreg] = cyc + 4;
    }
    if (op->viw) st->vi_ready[op->viw] = cyc + op->viwlat;
}

static uint32_t vu_loop(vu_state* st, uint32_t pc, uint32_t npc, uint32_t cyc, uint32_t n, int ending);

uint32_t vu_run(vu_state* st, uint32_t pc) {
    /* fresh pipeline: everything from the previous program drained when it ended */
    memset(st->vf_ready, 0, sizeof st->vf_ready);
    memset(st->vf_ready_any, 0, sizeof st->vf_ready_any);
    memset(st->vi_ready, 0, sizeof st->vi_ready);
    st->vih[0] = st->vih[1] = st->vih[2] = st->vih[3] = 0xFF;
    st->fq_head = st->fq_tail = st->cq_head = st->cq_tail = 0;
    st->q_pend = st->p_pend = 0;
    st->next_event = UINT32_MAX;
    st->clip_pipe = st->clip;
    pc &= st->pcmask;
    return vu_loop(st, pc, (pc + 1) & st->pcmask, 0, 0, 0);
}

/* Continue a program that recompiled code started (vu_compiled.h): the registers and the pipeline
 * (scoreboard, flag/clip queues, Q/P in flight, VI history) are in st as this loop keeps them, the
 * next pair is at pc with npc after it, cyc/n count what already ran. */
uint32_t vu_resume(vu_state* st, uint32_t pc, uint32_t npc, uint32_t cyc, uint32_t n, int ending) {
    return vu_loop(st, pc & st->pcmask, npc & st->pcmask, cyc, n, ending);
}

static uint32_t vu_loop(vu_state* st, uint32_t pc, uint32_t npc, uint32_t cyc, uint32_t n, int ending) {
    vu_op* const cache = st->cache;
    const uint8_t* const micro = st->micro;
    const uint32_t pcmask = st->pcmask;
    const uint32_t start = pc;

    for (;;) {
        vu_op* const op = &cache[pc];
        uint64_t raw;
        memcpy(&raw, micro + pc * 8u, 8);
        if (op->raw != raw) vu_decode(op, raw, pc, pcmask);
        pc = npc;
        npc = (pc + 1) & pcmask;

        uint32_t stalled = 0;
        if (op->check) {
            uint32_t t = stall_until(st, op, cyc);
            stalled = t != cyc;
            cyc = t;
        }
        st->cyc = cyc;
        st->stalled = stalled;
        st->hpos = n;

        /* upper reads its operands, the lower executes, then the upper result is committed (it
         * wins over a lower write to the same field), then the I bit loads I */
        __m128 ur;
        vf128* udst = op->ufn ? op->ufn(st, op, &ur) : NULL;
        const uint32_t hv = op->vichain ? op->vichain | (uint32_t)op->vichain_rd << 9 |
                                              (uint32_t)st->vi[op->vichain] << 16 : 0xFF;
        if (op->lfn) npc = op->lfn(st, op, npc);
        if (udst) vwrite(udst, ur, op->udst);
        if (op->writes) mark_writes(st, op, cyc);
        st->vih[n & 3] = hv | stalled << 8;

        cyc++;
        n++;
        if (op->bits) {
            if (op->bits & VUB_I) st->I = (uint32_t)op->imm;
            if (op->bits & (VUB_D | VUB_T)) {
                static int logged;
                if (!logged++) fprintf(stderr, "[vu] VU%d: D/T debug bit at 0x%04X ignored\n", st->idx,
                                       (unsigned)(op - cache) * 8);
            }
            if (ending) break;
            if (op->bits & VUB_E) ending = 1;
        } else if (ending) {
            break;
        }
        if (n >= VU_MAX_PAIRS) {
            static int logged;
            if (!logged++) fprintf(stderr, "[vu] VU%d: program at 0x%04X ran %u pairs without an E bit; stopped\n",
                                   st->idx, start * 8, n);
            break;
        }
    }

    st->tpc = pc;
    retire(st, UINT32_MAX);                /* the pipeline drains when the program ends */
    st->programs++;
    st->pairs += n;
    st->cycles += cyc;
    return n;
}
