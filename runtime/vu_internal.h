/* VU0/VU1 micro mode: state shared by vu.c (memory, EE-facing API) and vu_exec.c (decoder and
 * interpreter). Not part of the frozen interface; unit tests include it to inspect registers.
 *
 * Model (see notes/vu-microcode.md for the reasoning and sources):
 *  - One 64-bit instruction pair per cycle; the cycle counter advances by one per pair plus any
 *    stall cycles. Stalls are modelled because results that are not hazard-checked (Q, P, the
 *    MAC/status/clip flags) arrive a fixed number of *cycles* after issue, so code scheduled
 *    around them only behaves correctly if stalls are counted like on hardware.
 *  - FMAC (upper ops and lower VF writes such as LQ/MOVE/MFIR) results: 4-cycle latency, readers
 *    of a pending field stall. Values are written to the register file at issue; together with
 *    the stall this gives the hardware's visible behaviour.
 *  - MAC/status/clip flags: queued with ready time issue+4; FS/FM/FC instructions read the
 *    architecturally visible copy.
 *  - Q: DIV/SQRT ready at issue+7, RSQRT at issue+13. P: EFU latency per op. EFU stalls (WAITP
 *    and a new EFU op while one runs) end one cycle before the result lands (ps2tek), so up to
 *    two P results can be in flight.
 *  - Within a pair the upper op reads its operands, then the lower op executes (it sees the
 *    registers as they were before the pair), then the upper result is committed (so if both
 *    write the same VF field, the upper op wins), then the I bit loads I.
 *  - Integer registers: written at issue (1-cycle bypass) except ILW/ILWR (4 cycles, readers
 *    stall). Conditional branches read the value from before a chain of up to 4 immediately
 *    preceding writes to that register, where every write after the first also reads it (quirk
 *    documented by the PCSX2 team; see vu_exec.c).
 */
#pragma once
#include "hw.h"

#include <emmintrin.h>

/* Upper-word control bits, shifted down (bits >> 27). */
enum { VUB_T = 0x01, VUB_D = 0x02, VUB_M = 0x04, VUB_E = 0x08, VUB_I = 0x10 };

struct vu_op;
struct vu_state;
/* Upper handler: computes the result into *r (committed by the caller after the lower op) and
 * returns the destination register, or NULL. Lower handler: returns the next-next PC (the input
 * value unless it is a taken branch; for BAL/JALR it is also the link value). */
typedef vf128*   (*vu_ufn)(struct vu_state* st, const struct vu_op* op, __m128* r);
typedef uint32_t (*vu_lfn)(struct vu_state* st, const struct vu_op* op, uint32_t npc);

/* One pre-decoded instruction pair. The cache entry is valid while raw matches micro memory, so
 * any writer (VIF MPG through vu_micro_mem(), EE stores) invalidates it implicitly. */
typedef struct vu_op {
    uint64_t raw;                /* the instruction pair this entry was decoded from */
    vu_ufn   ufn;                /* NULL for NOP */
    vu_lfn   lfn;                /* NULL for NOP / LOI / WAITQ / WAITP */
    uint8_t  up, ubm, udst, uacc;/* upper op, second-operand mode, dest mask (bit0 = x), ACC target */
    uint8_t  ufd, ufs, uft, ubc; /* upper registers / broadcast lane */
    uint8_t  lo, ldst;           /* lower op, dest mask (bit0 = x) */
    uint8_t  lt, ls, ld;         /* lower register fields (VI fields already & 15) */
    uint8_t  fsf, ftf;           /* lane selectors (0 = x .. 3 = w) */
    uint8_t  bits;               /* VUB_* */
    int32_t  imm;                /* lower immediate (sign-extended), branch target or I value */
    /* hazard bookkeeping, filled by the decoder */
    uint8_t  nsrc;
    uint8_t  sreg[4], smask[4];  /* VF registers read (by either op) and the fields read */
    uint8_t  visrc[2];           /* VI registers read by the lower op (0 = none) */
    uint8_t  uwreg, uwmask;      /* VF register written by the upper op (0 = none) */
    uint8_t  lwreg, lwmask;      /* VF register written by the lower op (0 = none) */
    uint8_t  viw, viwlat;        /* VI register written by the lower op and its latency */
    uint8_t  vichain;            /* VI register this write chains for the branch quirk (0 = none) */
    uint8_t  vichain_rd;         /* 1: the op also reads vichain, so it continues a chain */
    uint8_t  stall;              /* VU_STALL_* */
    uint8_t  check;              /* nonzero: operands/units to check for stalls */
    uint8_t  writes;             /* nonzero: uwreg, lwreg or viw set */
} vu_op;

enum { VU_STALL_NONE, VU_STALL_Q, VU_STALL_P, VU_STALL_EFU };

typedef struct { uint32_t ready; uint16_t mac, zsuo; } vu_flagent;
typedef struct { uint32_t ready, clip; } vu_clipent;

typedef struct vu_state {
    vf128    vf[32];             /* vf0 = (0, 0, 0, 1), never written */
    vf128    acc;
    uint16_t vi[16];             /* vi0 = 0, never written */
    uint32_t I, Q, P, R;         /* raw bits; R keeps exponent 0x7F (1.0 <= R < 2.0) */
    uint32_t status, mac, clip;  /* flag registers as seen by FS/FM/FC instructions and CFC2 */
    uint32_t clip_pipe;          /* newest clip value inside the pipeline (CLIP shifts this one) */
    uint32_t tpc;                /* address (64-bit units) where the last program stopped */
    uint32_t top, itop;          /* VIF TOP/ITOP for XTOP/XITOP */

    /* pipeline (cycle counts restart at 0 for every program; everything drains at its end) */
    uint32_t   next_event;       /* earliest ready time among the queues below */
    vu_flagent fq[8];  uint32_t fq_head, fq_tail;
    vu_clipent cq[8];  uint32_t cq_head, cq_tail;
    uint32_t   q_pend, q_next, q_ready, q_flags;   /* q_flags: status I (0x10) / D (0x20) */
    uint32_t   p_pend, p_next[2], p_ready[2];      /* EFU results in flight (oldest first) */
    uint32_t   vf_ready[32][4], vf_ready_any[32], vi_ready[16];
    uint32_t   vih[4], hpos;     /* last 4 pairs' VI writes, for the branch quirk: register |
                                    stalled << 8 | reads-it << 9 | old value << 16 (0xFF: none) */
    uint32_t   cyc, stalled;     /* issue cycle of the current pair, and whether it stalled */

    /* memory */
    int      idx;                /* 0 = VU0, 1 = VU1 */
    uint8_t* data;  uint32_t dmask;    /* data memory, quadword address mask */
    uint8_t* micro; uint32_t pcmask;   /* micro memory, instruction address mask */
    vu_op*   cache;
    int      inited;

    /* statistics */
    uint64_t programs, pairs, cycles;
} vu_state;

extern vu_state vu_st[2];

void     vu_init_state(vu_state* st);
/* Run from instruction address pc (64-bit units) until the pair after an E bit. Requires the VU
 * MXCSR mode (vu.c sets it). Returns the number of pairs executed. */
uint32_t vu_run(vu_state* st, uint32_t pc);
/* XGKICK from VU1 (implemented in vu.c: restores the host FP mode around the GIF call). */
void     vu_xgkick(uint32_t qw_addr);
/* VU1 registers as VU0 sees them at data addresses 0x4000-0x43FF (quadword index 0-63). */
void     vu1_reg_read(uint32_t idx, uint32_t out[4]);
void     vu1_reg_write(uint32_t idx, const uint32_t in[4], unsigned lanes);

/* Proposed additions to hw.h / ps2_runtime.h (implemented in vu.c, see the report):
 * VIF MSCNT continues at the TPC; the recompiler can keep macro-mode MAC/status flags current. */
uint32_t vu_get_tpc(int vu);
void     vu0_macro_flags(const vf128* r, unsigned dest_xyzw);

/* MXCSR while microcode runs: flush-to-zero, denormals-are-zero, round toward zero, all
 * exceptions masked. Round-toward-zero also turns overflow into +-FLT_MAX instead of infinity. */
#define VU_MXCSR 0xFFC0u
