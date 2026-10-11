/* Statically recompiled VU microprograms (recomp/vu_recomp.py output) and what they need from the
 * runtime. Generated files include this header only.
 *
 * A compile unit (vuc_program) is one microcode image (a dvp-as overlay of the game) on one VU. Its
 * function runs a program from one of its entry points to the pair after the E bit, with the
 * results, XGKICKs and pair/cycle counts the interpreter (vu_exec.c) would produce; vu.c calls it
 * when a program starts at an entry and micro memory holds exactly the unit's image. Registers live
 * in locals; the generated code only touches vu_state at the start, at XGKICK (st->cyc) and at the
 * end, or when it hands a program over to the interpreter (vu_resume) with the pipeline state
 * written out as the interpreter keeps it. */
#pragma once
#include "vu_internal.h"
#include "vu_ops.h"

typedef uint32_t (*vuc_fn)(vu_state* st, uint32_t entry);
typedef struct { uint16_t start, len; } vuc_range;          /* instruction addresses (64-bit units) */
typedef struct vuc_program {
    const char*      name;
    uint32_t         unit;
    uint32_t         nentries;
    const uint16_t*  entries;         /* entry addresses; the function's entry argument is the index */
    uint32_t         nranges;
    const vuc_range* ranges;          /* the image ... */
    const uint32_t*  code;            /* ... and its words (lower, upper per pair), range by range */
    vuc_fn           fn;
} vuc_program;

#define VUC_NOT_COMPILED 0xFFFFFFFFu  /* returned by a unit's function for an entry it has no code for */
/* A program still running after this many pairs continues in the interpreter at its next backward
 * jump, so that one without an E bit stops exactly where the interpreter stops it (VU_MAX_PAIRS). */
#define VUC_RUNAWAY_HANDOFF (VU_MAX_PAIRS - 0x10000u)

/* vu.c */
void     vu_register_compiled(const vuc_program* const* progs, uint32_t n);
void     vuc_unknown_jump(const vu_state* st, uint32_t site, uint32_t target);

/* Float values the compiler could know: vf00, LOI immediates and VI values moved to VF (MFIR). They
 * pass through these barriers so that clang cannot fold float operations or comparisons on them:
 * its folding assumes IEEE denormals (the VU code runs with DAZ/FTZ, e.g. `loi -6.68e-39` then
 * `mulai ACC, vf07, I` must see I as zero for the U flag, and x * vf00w is not x for a denormal x)
 * and round-to-nearest (FENV_ACCESS in the generated files covers the rounding, not the denormal
 * mode). */
static inline __attribute__((always_inline)) uint32_t vuc_opaque(uint32_t x) {
    __asm__("" : "+r"(x));
    return x;
}
static inline __attribute__((always_inline)) __m128 vuc_opaque_v(__m128 x) {
    __asm__("" : "+x"(x));
    return x;
}
#define VF0 vf0                          /* a local of every generated function: (0, 0, 0, 1), opaque */

/* VF registers and ACC of a running program (recomp/vu_recomp.py REGS_IN_MEMORY) */
typedef struct { __m128 vf[32], acc; } vuc_regs;

/* -DVU_TRACE: every pair prints its address and issue cycle, like vu_exec.c under the same flag. */
#ifdef VU_TRACE
#define VUC_TRACE(pc, c) fprintf(stderr, "T %04X %u\n", (unsigned)(pc) * 8u, (unsigned)(c))
#else
#define VUC_TRACE(pc, c) ((void)0)
#endif

/* Data memory, as vu_exec.c's load_qw/store_qw/load_w/store_w: VU1 16 KB (quadword mask 0x3FF);
 * VU0 4 KB (mask 0xFF) where addresses with bit 0x400 set reach VU1's registers. */
VU_INLINE __m128 vuc_ldq1(vu_state* st, uint8_t* dm, uint32_t a) {
    (void)st;
    return _mm_loadu_ps((const float*)(dm + ((a & 0x3FF) << 4)));
}
VU_INLINE void vuc_stq1(vu_state* st, uint8_t* dm, uint32_t a, __m128 v, unsigned lanes) {
    (void)st;
    float* p = (float*)(dm + ((a & 0x3FF) << 4));
    _mm_storeu_ps(p, vu_blend(_mm_loadu_ps(p), v, lanes));
}
VU_INLINE uint32_t vuc_ldw1(vu_state* st, uint8_t* dm, uint32_t a, unsigned lane) {
    (void)st;
    uint32_t w;
    memcpy(&w, dm + ((a & 0x3FF) << 4) + lane * 4, 4);
    return w;
}
VU_INLINE void vuc_stw1(vu_state* st, uint8_t* dm, uint32_t a, unsigned lanes, uint32_t v) {
    (void)st;
    uint8_t* p = dm + ((a & 0x3FF) << 4);
    if (lanes & 1) memcpy(p, &v, 4);
    if (lanes & 2) memcpy(p + 4, &v, 4);
    if (lanes & 4) memcpy(p + 8, &v, 4);
    if (lanes & 8) memcpy(p + 12, &v, 4);
}
VU_INLINE __m128 vuc_ldq0(vu_state* st, uint8_t* dm, uint32_t a) {
    (void)st;
    if (a & 0x400) { uint32_t t[4]; vu1_reg_read(a & 0x3F, t); return _mm_loadu_ps((const float*)t); }
    return _mm_loadu_ps((const float*)(dm + ((a & 0xFF) << 4)));
}
VU_INLINE void vuc_stq0(vu_state* st, uint8_t* dm, uint32_t a, __m128 v, unsigned lanes) {
    (void)st;
    if (a & 0x400) { uint32_t t[4]; _mm_storeu_ps((float*)t, v); vu1_reg_write(a & 0x3F, t, lanes); return; }
    float* p = (float*)(dm + ((a & 0xFF) << 4));
    _mm_storeu_ps(p, vu_blend(_mm_loadu_ps(p), v, lanes));
}
VU_INLINE uint32_t vuc_ldw0(vu_state* st, uint8_t* dm, uint32_t a, unsigned lane) {
    (void)st;
    if (a & 0x400) { uint32_t t[4]; vu1_reg_read(a & 0x3F, t); return t[lane]; }
    uint32_t w;
    memcpy(&w, dm + ((a & 0xFF) << 4) + lane * 4, 4);
    return w;
}
VU_INLINE void vuc_stw0(vu_state* st, uint8_t* dm, uint32_t a, unsigned lanes, uint32_t v) {
    (void)st;
    if (a & 0x400) { uint32_t t[4] = { v, v, v, v }; vu1_reg_write(a & 0x3F, t, lanes); return; }
    uint8_t* p = dm + ((a & 0xFF) << 4);
    if (lanes & 1) memcpy(p, &v, 4);
    if (lanes & 2) memcpy(p + 4, &v, 4);
    if (lanes & 4) memcpy(p + 8, &v, 4);
    if (lanes & 8) memcpy(p + 12, &v, 4);
}

/* Before a handoff: the pipeline as vu_run() starts it (nothing in flight); the handoff site then
 * fills in what its block has in flight. */
static inline void vuc_pipe_clear(vu_state* st) {
    memset(st->vf_ready, 0, sizeof st->vf_ready);
    memset(st->vf_ready_any, 0, sizeof st->vf_ready_any);
    memset(st->vi_ready, 0, sizeof st->vi_ready);
    st->vih[0] = st->vih[1] = st->vih[2] = st->vih[3] = 0xFF;
    st->fq_head = st->fq_tail = st->cq_head = st->cq_tail = 0;
    st->q_pend = st->p_pend = 0;
    st->next_event = UINT32_MAX;
}
