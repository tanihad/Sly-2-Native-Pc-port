/* Unit tests for the VU micro mode interpreter (runtime/vu.c, runtime/vu_exec.c).
 * Programs are hand-assembled with vuasm.h. Links only the VU sources plus a fake GIF PATH1. */
#include "vu_internal.h"
#include "vuasm.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <xmmintrin.h>

/* ---- fakes for neighbouring subsystems ------------------------------------------------------- */

static int kicks;
static uint32_t kick_addr[16], kick_word0, kick_csr;
static const uint8_t* kick_mem;
void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) {
    kick_mem = vu1_data;
    kick_addr[kicks & 15] = qw_addr;
    memcpy(&kick_word0, vu1_data + qw_addr * 16u, 4);
    kick_csr = _mm_getcsr();
    kicks++;
}
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}
uint32_t ps2_vblank_count(void) { return 0; }

/* ---- tiny test framework ---------------------------------------------------------------------- */

static int checks, failures;
static const char* cur_test;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, cur_test, #c); } } while (0)
#define CHECKU(a, b) do { uint32_t a_ = (uint32_t)(a), b_ = (uint32_t)(b); checks++; if (a_ != b_) { failures++; \
    fprintf(stderr, "FAIL %s:%d [%s]: %s = 0x%08X, expected 0x%08X\n", __FILE__, __LINE__, cur_test, #a, a_, b_); } } while (0)
#define CHECKF(a, b) do { float a_ = (a), b_ = (b); checks++; if (!(a_ == b_)) { failures++; \
    fprintf(stderr, "FAIL %s:%d [%s]: %s = %.9g, expected %.9g\n", __FILE__, __LINE__, cur_test, #a, a_, b_); } } while (0)
#define CHECKNEAR(a, b, eps) do { float a_ = (a), b_ = (b); checks++; if (!(fabsf(a_ - b_) <= (eps))) { failures++; \
    fprintf(stderr, "FAIL %s:%d [%s]: %s = %.9g, expected %.9g\n", __FILE__, __LINE__, cur_test, #a, a_, b_); } } while (0)

/* ---- program builder -------------------------------------------------------------------------- */

static uint32_t prog[2 * 2048];
static unsigned np;
static void begin(void) { np = 0; }
static void P(uint32_t upper, uint32_t lower) { prog[2 * np] = lower; prog[2 * np + 1] = upper; np++; }
static void PE(void) { P(U_NOP | UE, L_NOP); P(U_NOP, L_NOP); }        /* end: E bit + delay slot */
static void load(int vu, unsigned at) { memcpy(vu_micro_mem(vu) + at * 8u, prog, np * 8u); }

static vu_state* const V1 = &vu_st[1];

static void setv(vf128* v, float x, float y, float z, float w) { v->f[0] = x; v->f[1] = y; v->f[2] = z; v->f[3] = w; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Clear VU1 registers/flags and the start of data memory, then run prog from address 0. */
static void reset1(void) {
    memset(V1->vf + 1, 0, 31 * sizeof(vf128));
    memset(&V1->acc, 0, sizeof V1->acc);
    memset(V1->vi, 0, sizeof V1->vi);
    V1->status = V1->mac = V1->clip = 0;
    V1->Q = V1->P = V1->I = 0;
    memset(vu_data_mem(1), 0, 16384);
}
static uint64_t last_cycles;
static void run1(void) {
    load(1, 0);
    uint64_t c0 = V1->cycles;
    vu1_start(0);
    last_cycles = V1->cycles - c0;
}

/* ---- tests ------------------------------------------------------------------------------------- */

static void test_arith(void) {
    cur_test = "arith";
    reset1();
    setv(&V1->vf[1], 1, 2, 3, 4);
    setv(&V1->vf[2], 10, 20, 30, 40);
    setv(&V1->vf[5], -1, -1, -1, -1);
    setv(&V1->vf[10], 1, 0, 0, 0);       /* matrix rows (column vectors in VU convention) */
    setv(&V1->vf[11], 0, 2, 0, 0);
    setv(&V1->vf[12], 0, 0, 3, 0);
    setv(&V1->vf[13], 5, 6, 7, 1);
    setv(&V1->vf[14], 1, 1, 1, 1);       /* vector to transform */
    setv(&V1->vf[16], 1, 0, 0, 0);
    setv(&V1->vf[17], 0, 1, 0, 0);
    begin();
    P(U_ADD(DXYZW, 4, 1, 2), L_NOP);                 /* vf4 = vf1 + vf2 */
    P(U_SUB(DXZ, 5, 2, 1), L_NOP);                   /* masked: only x, z */
    P(U_MULbc(DXYZW, 6, 2, 1, FW), L_NOP);           /* vf6 = vf2 * vf1.w */
    P(U_MAXbc(DXYZW, 7, 1, 1, FZ), L_NOP);           /* max(vf1, 3) */
    P(U_MINI(DXYZW, 8, 2, 1), L_NOP);                /* min(vf2, vf1) */
    P(U_MULAbc(DXYZW, 10, 14, FX), L_NOP);           /* ACC = row0 * v.x */
    P(U_MADDAbc(DXYZW, 11, 14, FY), L_NOP);
    P(U_MADDAbc(DXYZW, 12, 14, FZ), L_NOP);
    P(U_MADDbc(DXYZW, 9, 13, 14, FW), L_NOP);        /* vf9 = ACC + row3 * v.w */
    P(U_OPMULA(DXYZ, 16, 17), L_NOP);                /* cross product vf16 x vf17 */
    P(U_OPMSUB(DXYZ, 18, 17, 16), L_NOP);
    P(U_ABS(DXYZW, 19, 5), L_NOP);
    P(U_MSUB(DXYZW, 20, 1, 1), L_NOP);               /* ACC (= cross product step) - vf1*vf1 */
    PE();
    run1();
    CHECKF(V1->vf[4].f[0], 11); CHECKF(V1->vf[4].f[1], 22); CHECKF(V1->vf[4].f[2], 33); CHECKF(V1->vf[4].f[3], 44);
    CHECKF(V1->vf[5].f[0], 9); CHECKF(V1->vf[5].f[1], -1); CHECKF(V1->vf[5].f[2], 27); CHECKF(V1->vf[5].f[3], -1);
    CHECKF(V1->vf[6].f[0], 40); CHECKF(V1->vf[6].f[3], 160);
    CHECKF(V1->vf[7].f[0], 3); CHECKF(V1->vf[7].f[2], 3); CHECKF(V1->vf[7].f[3], 4);
    CHECKF(V1->vf[8].f[0], 1); CHECKF(V1->vf[8].f[3], 4);
    CHECKF(V1->vf[9].f[0], 6); CHECKF(V1->vf[9].f[1], 8); CHECKF(V1->vf[9].f[2], 10); CHECKF(V1->vf[9].f[3], 1);
    CHECKF(V1->vf[18].f[0], 0); CHECKF(V1->vf[18].f[1], 0); CHECKF(V1->vf[18].f[2], 1); CHECKF(V1->vf[18].f[3], 0);
    CHECKF(V1->vf[19].f[0], 9); CHECKF(V1->vf[19].f[1], 1);
    /* ACC = (0, 0, 1, 0) after OPMULA; MSUB: ACC - vf1 * vf1 */
    CHECKF(V1->vf[20].f[0], -1); CHECKF(V1->vf[20].f[1], -4); CHECKF(V1->vf[20].f[2], -8); CHECKF(V1->vf[20].f[3], -16);
    CHECKF(V1->vf[0].f[3], 1.0f);                                /* vf0 untouched */

    /* conversions, Q and I operands */
    cur_test = "convert";
    reset1();
    V1->vf[1].u[0] = 16; V1->vf[1].u[1] = (uint32_t)-32; V1->vf[1].u[2] = 7; V1->vf[1].u[3] = 0x7FFFFFFF;
    setv(&V1->vf[2], 1e10f, -1e10f, 2.9f, -2.9f);
    setv(&V1->vf[3], 1.5f, 0.25f, -0.0625f, 100.0f);
    V1->Q = fb(3.0f); V1->I = fb(0.5f);
    begin();
    P(U_ITOF(DXYZW, 4, 1, 1), L_NOP);                /* ITOF4 */
    P(U_ITOF(DXYZW, 5, 1, 0), L_NOP);                /* ITOF0 */
    P(U_FTOI(DXYZW, 6, 2, 0), L_NOP);                /* FTOI0 with saturation, truncation */
    P(U_FTOI(DXYZW, 7, 3, 1), L_NOP);                /* FTOI4 */
    P(U_FTOI(DXYZW, 8, 3, 3), L_NOP);                /* FTOI15 */
    P(U_MULq(DXYZW, 9, 3), L_NOP);
    P(U_ADDi(DXYZW, 10, 3), L_NOP);
    P(U_MAXi(DXYZW, 11, 3), L_NOP);
    PE();
    run1();
    CHECKF(V1->vf[4].f[0], 1.0f); CHECKF(V1->vf[4].f[1], -2.0f); CHECKF(V1->vf[4].f[2], 0.4375f);
    CHECKF(V1->vf[5].f[0], 16.0f); CHECKF(V1->vf[5].f[3], 2147483520.0f);   /* rounded toward zero */
    CHECKU(V1->vf[6].u[0], 0x7FFFFFFF); CHECKU(V1->vf[6].u[1], 0x80000000); CHECKU(V1->vf[6].u[2], 2); CHECKU(V1->vf[6].u[3], (uint32_t)-2);
    CHECKU(V1->vf[7].u[0], 24); CHECKU(V1->vf[7].u[1], 4); CHECKU(V1->vf[7].u[2], (uint32_t)-1); CHECKU(V1->vf[7].u[3], 1600);
    CHECKU(V1->vf[8].u[0], 49152);
    CHECKF(V1->vf[9].f[0], 4.5f); CHECKF(V1->vf[9].f[3], 300.0f);
    CHECKF(V1->vf[10].f[1], 0.75f);
    CHECKF(V1->vf[11].f[1], 0.5f); CHECKF(V1->vf[11].f[0], 1.5f);
}

static void test_float_semantics(void) {
    cur_test = "ps2 floats";
    reset1();
    setv(&V1->vf[1], FLT_MAX, -FLT_MAX, 1e-30f, 3.0f);
    setv(&V1->vf[2], 2.0f, 2.0f, 1e-30f, 1.0f);
    V1->vf[3].u[0] = 0x7F800000; V1->vf[3].u[1] = 0xFFFFFFFF; V1->vf[3].u[2] = 0x000116C2; V1->vf[3].u[3] = 0x7FC00000;
    setv(&V1->vf[4], 1.0f, 1.0f, 1e30f, 1.0f);
    setv(&V1->vf[6], 1.0f, 3.0f, 0.0f, 1.0f);
    setv(&V1->vf[7], 3.0f, 1.0f, 0.0f, 1.0f);
    begin();
    P(U_MUL(DXYZW, 5, 1, 2), L_NOP);                 /* overflow, underflow */
    P(U_MUL(DXYZW, 8, 3, 4), L_NOP);                 /* "Inf/NaN" bit patterns are big numbers; denormal -> 0 */
    P(U_MAX(DXYZW, 9, 3, 0), L_NOP);                 /* MAX compares raw bits (sign-magnitude) */
    P(U_SUB(DXYZW, 10, 6, 7), L_NOP);                /* -2, 2, 0, 0 */
    P(U_ADD(DX, 11, 6, 7), L_NOP);                   /* 4: no flags */
    PE();
    run1();
    CHECKU(V1->vf[5].u[0], 0x7F7FFFFF);
    CHECKU(V1->vf[5].u[1], 0xFF7FFFFF);
    CHECKU(V1->vf[5].u[2], 0);
    CHECKF(V1->vf[5].f[3], 3.0f);
    for (int k = 0; k < 4; k++) CHECK((V1->vf[8].u[k] & 0x7F800000) != 0x7F800000);   /* never Inf/NaN */
    CHECKU(V1->vf[8].u[0], 0x7F7FFFFF);
    CHECKU(V1->vf[8].u[1], 0xFF7FFFFF);
    CHECKU(V1->vf[8].u[2], 0);                       /* denormal input times 1e30 is 0 on the VU */
    CHECKU(V1->vf[9].u[0], 0x7F800000);              /* MAX passes the raw value through */
    CHECKU(V1->vf[9].u[1], 0);                       /* negative < 0 */
    CHECKU(V1->vf[9].u[2], 0x000116C2);              /* a denormal still compares above 0 */
    CHECKU(V1->vf[9].u[3], 0x7FC00000);
    CHECKF(V1->vf[10].f[0], -2); CHECKF(V1->vf[10].f[2], 0);
    /* The visible flags are those of the last FMAC op (ADD.x vf11 = 4): none set. */
    CHECKU(V1->mac, 0x0000);
    CHECKU(V1->status & 0xF, 0);
    CHECKU(V1->status & 0x3C0, 0x3C0);               /* sticky: O, U, S, Z all happened earlier */
}

static void test_mac_flags(void) {
    cur_test = "mac flags";
    reset1();
    setv(&V1->vf[1], FLT_MAX, -FLT_MAX, 1e-30f, 3.0f);
    setv(&V1->vf[2], 2.0f, 2.0f, 1e-30f, 1.0f);
    V1->vi[5] = 0xFFFF;
    begin();
    P(U_MUL(DXYZW, 5, 1, 2), L_NOP);
    P(U_NOP, L_NOP); P(U_NOP, L_NOP); P(U_NOP, L_NOP);
    P(U_NOP, L_FMAND(1, 5));
    P(U_NOP, L_FSAND(2, 0xFFF));
    PE();
    run1();
    /* x: O; y: O+S; z: U+Z; w: none */
    CHECKU(V1->vi[1], (8 | 4) << 12 | 2 << 8 | 4 << 4 | 2);
    CHECKU(V1->vi[2], 0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x80 | 0x100 | 0x200);
}

static void test_memory(void) {
    cur_test = "load/store";
    reset1();
    uint32_t* d = (uint32_t*)vu_data_mem(1);
    for (int k = 0; k < 4; k++) { d[10 * 4 + k] = fb((float)(k + 1)); d[11 * 4 + k] = fb((float)(10 * (k + 1))); }
    d[12 * 4 + 1] = 0x1234; d[12 * 4 + 2] = 0xFFFE;
    d[5 * 4 + 0] = fb(55.0f);
    setv(&V1->vf[3], 7, 8, 9, 10);
    setv(&V1->vf[9], -1, -1, -1, -1);
    begin();
    P(U_NOP, L_LQ(DXYZW, 1, 0, 10));                 /* vf1 = mem[10] */
    P(U_NOP, L_IADDIU(1, 0, 10));                    /* vi1 = 10 */
    P(U_NOP, L_LQ(DY, 2, 1, 1));                     /* vf2.y = mem[11].y */
    P(U_NOP, L_SQ(DXZ, 3, 0, 20));                   /* mem[20].xz = vf3 */
    P(U_NOP, L_IADDIU(2, 0, 30));
    P(U_NOP, L_SQI(DXYZW, 3, 2));                    /* mem[30] = vf3, vi2 = 31 */
    P(U_NOP, L_SQI(DXYZW, 1, 2));                    /* mem[31] = vf1, vi2 = 32 */
    P(U_NOP, L_LQD(DXYZW, 4, 2));                    /* vi2 = 31, vf4 = mem[31] */
    P(U_NOP, L_LQI(DXYZW, 5, 2));                    /* vf5 = mem[31], vi2 = 32 */
    P(U_NOP, L_SQD(DW, 1, 2));                       /* vi2 = 31, mem[31].w = vf1.w */
    P(U_NOP, L_ILW(DY, 3, 0, 12));                   /* vi3 = mem[12].y */
    P(U_NOP, L_ILW(DZ, 4, 0, 12));                   /* vi4 = 0xFFFE */
    P(U_NOP, L_ISW(DXW, 3, 0, 40));                  /* mem[40].x, .w = vi3 */
    P(U_NOP, L_IADDIU(6, 0, 41));
    P(U_NOP, L_ISWR(DY, 4, 6));                      /* mem[41].y = vi4 */
    P(U_NOP, L_ILWR(DY, 7, 6));
    P(U_NOP, L_MFIR(DXZ, 6, 4));                     /* vf6.xz = -2 (sign-extended) */
    P(U_NOP, L_MTIR(8, 3, FW));                      /* vi8 = low 16 bits of vf3.w bits */
    P(U_NOP, L_IADDIU(9, 0, 1024 + 5));              /* address wraps at 16 KB */
    P(U_NOP, L_LQ(DX, 9, 9, 0));
    P(U_NOP, L_MOVE(DXZ, 10, 3));
    P(U_NOP, L_MR32(DXYZW, 11, 3));
    PE();
    run1();
    CHECKF(V1->vf[1].f[0], 1); CHECKF(V1->vf[1].f[3], 4);
    CHECKF(V1->vf[2].f[0], 0); CHECKF(V1->vf[2].f[1], 20); CHECKF(V1->vf[2].f[2], 0);
    CHECKU(d[20 * 4 + 0], fb(7)); CHECKU(d[20 * 4 + 1], 0); CHECKU(d[20 * 4 + 2], fb(9)); CHECKU(d[20 * 4 + 3], 0);
    CHECKU(d[30 * 4 + 3], fb(10));
    CHECKF(V1->vf[4].f[0], 1); CHECKF(V1->vf[5].f[2], 3);
    CHECKU(V1->vi[2], 31);
    CHECKU(d[31 * 4 + 3], fb(4));
    CHECKU(V1->vi[3], 0x1234); CHECKU(V1->vi[4], 0xFFFE);
    CHECKU(d[40 * 4 + 0], 0x1234); CHECKU(d[40 * 4 + 1], 0); CHECKU(d[40 * 4 + 3], 0x1234);
    CHECKU(d[41 * 4 + 1], 0xFFFE); CHECKU(V1->vi[7], 0xFFFE);
    CHECKU(V1->vf[6].u[0], 0xFFFFFFFE); CHECKU(V1->vf[6].u[1], 0); CHECKU(V1->vf[6].u[2], 0xFFFFFFFE);
    CHECKU(V1->vi[8], fb(10) & 0xFFFF);
    CHECKF(V1->vf[9].f[0], 55); CHECKF(V1->vf[9].f[1], -1);
    CHECKF(V1->vf[10].f[0], 7); CHECKF(V1->vf[10].f[1], 0); CHECKF(V1->vf[10].f[2], 9);
    CHECKF(V1->vf[11].f[0], 8); CHECKF(V1->vf[11].f[1], 9); CHECKF(V1->vf[11].f[2], 10); CHECKF(V1->vf[11].f[3], 7);
}

static void test_integer(void) {
    cur_test = "integer";
    reset1();
    begin();
    P(U_NOP, L_IADDIU(1, 0, 0x7FFF));
    P(U_NOP, L_IADDI(2, 0, -3));
    P(U_NOP, L_IADD(3, 1, 2));                       /* 0x7FFC */
    P(U_NOP, L_ISUB(4, 2, 1));                       /* -3 - 0x7FFF */
    P(U_NOP, L_IAND(5, 1, 2));
    P(U_NOP, L_IOR(6, 1, 2));
    P(U_NOP, L_ISUBIU(7, 0, 1));                     /* 0xFFFF */
    P(U_NOP, L_IADDI(0, 1, 1));                      /* writes to vi0 are dropped */
    P(U_NOP, L_IADD(18, 1, 1));                      /* register fields are 4 bits: vi18 = vi2 */
    PE();
    run1();
    CHECKU(V1->vi[1], 0x7FFF); CHECKU(V1->vi[3], 0x7FFC);
    CHECKU(V1->vi[4], (uint16_t)(-3 - 0x7FFF)); CHECKU(V1->vi[5], 0x7FFD); CHECKU(V1->vi[6], 0xFFFF);
    CHECKU(V1->vi[7], 0xFFFF); CHECKU(V1->vi[0], 0); CHECKU(V1->vi[2], 0xFFFE);
}

static void test_branches(void) {
    cur_test = "branch delay slot";
    reset1();
    begin();
    P(U_NOP, L_IADDIU(1, 0, 1));                     /* 0 */
    P(U_NOP, L_B(2));                                /* 1: -> 4 */
    P(U_NOP, L_IADDIU(2, 0, 2));                     /* 2: delay slot (runs) */
    P(U_NOP, L_IADDIU(3, 0, 3));                     /* 3: skipped */
    P(U_NOP, L_IADDIU(4, 0, 4));                     /* 4 */
    PE();
    run1();
    CHECKU(V1->vi[1], 1); CHECKU(V1->vi[2], 2); CHECKU(V1->vi[3], 0); CHECKU(V1->vi[4], 4);

    cur_test = "loop";
    reset1();
    begin();
    P(U_NOP, L_IADDIU(1, 0, 5));                     /* 0 */
    P(U_NOP, L_NOP);                                 /* 1 */
    P(U_NOP, L_IADDI(2, 2, 1));                      /* 2: loop */
    P(U_NOP, L_IADDI(1, 1, -1));                     /* 3 */
    P(U_NOP, L_NOP);                                 /* 4 */
    P(U_NOP, L_IBNE(1, 0, -4));                      /* 5: -> 2 */
    P(U_NOP, L_IADDI(3, 3, 1));                      /* 6: delay slot, runs every time */
    PE();
    run1();
    CHECKU(V1->vi[2], 5); CHECKU(V1->vi[3], 5); CHECKU(V1->vi[1], 0);

    cur_test = "bal/jr/jalr";
    reset1();
    begin();
    P(U_NOP, L_BAL(15, 4));                          /* 0: call 5, link = 2 */
    P(U_NOP, L_IADDIU(1, 0, 1));                     /* 1: delay */
    P(U_NOP, L_IADDIU(3, 0, 3));                     /* 2: return point */
    PE();                                            /* 3, 4 */
    P(U_NOP, L_IADDIU(2, 0, 2));                     /* 5: subroutine */
    P(U_NOP, L_IADDIU(8, 0, 10));                    /* 6 */
    P(U_NOP, L_JR(15));                              /* 7: return */
    P(U_NOP, L_NOP);                                 /* 8 */
    P(U_NOP, L_JALR(14, 8));                         /* 9: unused */
    run1();
    CHECKU(V1->vi[15], 2); CHECKU(V1->vi[1], 1); CHECKU(V1->vi[2], 2); CHECKU(V1->vi[3], 3);
    reset1();
    begin();
    P(U_NOP, L_IADDIU(8, 0, 6));                     /* 0 */
    P(U_NOP, L_NOP);                                 /* 1 */
    P(U_NOP, L_JALR(14, 8));                         /* 2: -> 6, vi14 = 4 */
    P(U_NOP, L_NOP);                                 /* 3 */
    PE();                                            /* 4, 5 */
    P(U_NOP, L_IADDIU(5, 0, 5));                     /* 6 */
    P(U_NOP, L_JR(14));                              /* 7 */
    P(U_NOP, L_NOP);                                 /* 8 */
    run1();
    CHECKU(V1->vi[14], 4); CHECKU(V1->vi[5], 5);
    CHECKU(V1->tpc, 6);

    cur_test = "conditional branches";
    reset1();
    begin();
    P(U_NOP, L_IADDI(1, 0, -2));                     /* 0 */
    P(U_NOP, L_IADDI(2, 0, 3));                      /* 1 */
    P(U_NOP, L_NOP);                                 /* 2 */
    P(U_NOP, L_IBLTZ(1, 2));                         /* 3: taken -> 6 */
    P(U_NOP, L_NOP);                                 /* 4: delay slot */
    P(U_NOP, L_IADDI(10, 10, 1));                    /* 5: skipped */
    P(U_NOP, L_IBGTZ(1, 2));                         /* 6: not taken */
    P(U_NOP, L_NOP);                                 /* 7 */
    P(U_NOP, L_IADDI(10, 10, 2));                    /* 8: runs */
    P(U_NOP, L_IBLEZ(2, 2));                         /* 9: not taken */
    P(U_NOP, L_NOP);                                 /* 10 */
    P(U_NOP, L_IADDI(10, 10, 4));                    /* 11: runs */
    P(U_NOP, L_IBGEZ(2, 2));                         /* 12: taken -> 15 */
    P(U_NOP, L_NOP);                                 /* 13 */
    P(U_NOP, L_IADDI(10, 10, 8));                    /* 14: skipped */
    P(U_NOP, L_IBEQ(1, 2, 2));                       /* 15: not taken */
    P(U_NOP, L_NOP);                                 /* 16 */
    P(U_NOP, L_IADDI(10, 10, 15));                   /* 17: runs */
    P(U_NOP, L_IBLEZ(0, 2));                         /* 18: vi0 = 0 <= 0: taken -> 21 */
    P(U_NOP, L_NOP);                                 /* 19 */
    P(U_NOP, L_IADDI(10, 10, 15));                   /* 20: skipped */
    P(U_NOP, L_IBNE(1, 2, 2));                       /* 21: taken -> 24 */
    P(U_NOP, L_NOP);                                 /* 22 */
    P(U_NOP, L_IADDI(10, 10, 15));                   /* 23: skipped */
    PE();                                            /* 24, 25 */
    run1();
    CHECKU(V1->vi[10], 2 + 4 + 15);

    cur_test = "branch in delay slot";
    reset1();
    begin();
    P(U_NOP, L_B(3));                                /* 0: -> 4 */
    P(U_NOP, L_B(5));                                /* 1: in the delay slot: -> 7 */
    P(U_NOP, L_IADDIU(1, 0, 1));                     /* 2 */
    P(U_NOP, L_IADDIU(1, 0, 1));                     /* 3 */
    P(U_NOP, L_IADDIU(2, 0, 2));                     /* 4: runs once (delay slot of the 2nd branch) */
    P(U_NOP, L_IADDIU(3, 0, 3));                     /* 5: skipped */
    P(U_NOP, L_IADDIU(3, 0, 3));                     /* 6: skipped */
    P(U_NOP, L_IADDIU(4, 0, 4));                     /* 7 */
    PE();
    run1();
    CHECKU(V1->vi[1], 0); CHECKU(V1->vi[2], 2); CHECKU(V1->vi[3], 0); CHECKU(V1->vi[4], 4);
}

static void test_ebit(void) {
    cur_test = "E bit";
    reset1();
    begin();
    P(U_NOP, L_IADDIU(1, 0, 1));                     /* 0 */
    P(U_NOP | UE, L_IADDIU(2, 0, 2));                /* 1: E */
    P(U_ADD(DX, 3, 0, 0), L_IADDIU(3, 0, 3));        /* 2: delay slot runs */
    P(U_NOP, L_IADDIU(4, 0, 4));                     /* 3: not run */
    PE();                                            /* 4, 5: for the MSCNT-style continue below */
    run1();
    CHECKU(V1->vi[1], 1); CHECKU(V1->vi[2], 2); CHECKU(V1->vi[3], 3); CHECKU(V1->vi[4], 0);
    CHECKU(V1->tpc, 3);
    CHECKU(last_cycles, 3);
    /* MSCNT-style continue from TPC */
    uint64_t c0 = V1->cycles;
    vu1_start(V1->tpc);
    (void)c0;
    CHECKU(V1->vi[4], 4);
}

static void test_q_latency(void) {
    cur_test = "Q latency";
    reset1();
    V1->Q = fb(1.0f);
    setv(&V1->vf[1], 10, 0, 0, 0);
    setv(&V1->vf[2], 2, 0, 0, 0);
    setv(&V1->vf[3], 1, 1, 1, 1);
    begin();
    P(U_NOP, L_DIV(1, FX, 2, FX));                   /* cycle 0: Q = 5 at cycle 7 */
    P(U_MULq(DX, 4, 3), L_NOP);                      /* 1: old Q */
    P(U_NOP, L_NOP); P(U_NOP, L_NOP); P(U_NOP, L_NOP); P(U_NOP, L_NOP);   /* 2-5 */
    P(U_MULq(DX, 5, 3), L_NOP);                      /* 6: still old */
    P(U_MULq(DX, 6, 3), L_NOP);                      /* 7: new */
    P(U_NOP, L_RSQRT(1, FX, 2, FX));                 /* 8: Q = 10/sqrt(2) at cycle 21 */
    P(U_MULq(DX, 7, 3), L_WAITQ);                    /* 9: stalls to 21; the upper sees the new Q */
    PE();
    run1();
    CHECKF(V1->vf[4].f[0], 1.0f);
    CHECKF(V1->vf[5].f[0], 1.0f);
    CHECKF(V1->vf[6].f[0], 5.0f);
    CHECKNEAR(V1->vf[7].f[0], 7.0710678f, 1e-5f);
    CHECKU(last_cycles, 24);                         /* 0..9 issue at 0..8, 21; E 22; delay 23 */

    cur_test = "Q after stall";
    reset1();
    V1->Q = fb(1.0f);
    setv(&V1->vf[1], 10, 0, 0, 0);
    setv(&V1->vf[2], 2, 0, 0, 0);
    setv(&V1->vf[3], 1, 1, 1, 1);
    begin();
    P(U_NOP, L_DIV(1, FX, 2, FX));                   /* 0 */
    P(U_ADD(DX, 8, 3, 3), L_NOP);                    /* 1 */
    P(U_ADD(DX, 9, 8, 3), L_NOP);                    /* stalls: issues at 5 */
    P(U_MULq(DX, 4, 3), L_NOP);                      /* 6: old Q */
    P(U_MULq(DX, 5, 3), L_NOP);                      /* 7: new Q after only 4 pairs */
    PE();
    run1();
    CHECKF(V1->vf[9].f[0], 3.0f);
    CHECKF(V1->vf[4].f[0], 1.0f);
    CHECKF(V1->vf[5].f[0], 5.0f);

    cur_test = "FDIV errors";
    reset1();
    setv(&V1->vf[1], 3, 0, -4, 0);
    begin();
    P(U_NOP, L_DIV(1, FX, 1, FY));                   /* 3/0: D flag, +max */
    P(U_NOP, L_WAITQ);
    P(U_NOP, L_FSAND(1, 0xFFF));
    P(U_ADDq(DX, 2, 0), L_DIV(1, FY, 1, FY));        /* 0/0: I flag */
    P(U_NOP, L_WAITQ);
    P(U_NOP, L_FSAND(2, 0xFFF));
    P(U_NOP, L_SQRT(1, FZ));                         /* sqrt(-4): I flag, |x| used */
    P(U_NOP, L_WAITQ);
    P(U_NOP, L_FSAND(3, 0x030));
    PE();
    run1();
    CHECKU(V1->vi[1] & 0x820, 0x820);
    CHECKF(V1->vf[2].f[0], FLT_MAX);                 /* ADDq clamps the 0x7FFFFFFF Q to +FLT_MAX */
    CHECKU(V1->vi[2] & 0x430, 0x410);
    CHECKU(V1->vi[3], 0x010);
    CHECKF(bf(V1->Q), 2.0f);
}

static void test_flags_pipeline(void) {
    cur_test = "flag latency";
    reset1();
    setv(&V1->vf[1], 1, -1, 0, 2);
    setv(&V1->vf[2], 1, 1, 1, 1);
    V1->vi[5] = 0xFFFF;
    begin();
    P(U_SUB(DXYZW, 3, 1, 2), L_NOP);                 /* 0: (0, -2, -1, 1): Zx, Sy, Sz */
    P(U_NOP, L_FMAND(1, 5));                         /* 1: old flags */
    P(U_NOP, L_NOP);                                 /* 2 */
    P(U_NOP, L_FMAND(2, 5));                         /* 3: old flags */
    P(U_NOP, L_FMAND(3, 5));                         /* 4: new */
    P(U_ADD(DX, 4, 2, 2), L_FSAND(4, 0xFFF));        /* 5: status Z|S + sticky */
    P(U_NOP, L_NOP); P(U_NOP, L_NOP); P(U_NOP, L_NOP);
    P(U_NOP, L_FSAND(6, 0xFFF));                     /* 9: ADD.x flags: none set, sticky stays */
    P(U_NOP, L_FMEQ(7, 0));                          /* MAC == 0 */
    P(U_NOP, L_FSEQ(8, 0x0C0));
    P(U_NOP, L_FSOR(9, 0xF3F));                      /* (status | 0xF3F) == 0xFFF */
    P(U_NOP, L_FMOR(10, 5));
    P(U_NOP, L_FSSET(0));                            /* clears sticky flags */
    P(U_NOP, L_FSAND(11, 0xFFF));
    PE();
    run1();
    CHECKU(V1->vi[1], 0); CHECKU(V1->vi[2], 0);
    CHECKU(V1->vi[3], 0x0008 | 0x0040 | 0x0020);
    CHECKU(V1->vi[4], 0x003 | 0x0C0);
    CHECKU(V1->vi[6], 0x0C0);
    CHECKU(V1->vi[7], 1); CHECKU(V1->vi[8], 1); CHECKU(V1->vi[9], 1); CHECKU(V1->vi[10], 1);
    CHECKU(V1->vi[11], 0);

    cur_test = "clip flags";
    reset1();
    V1->clip = 0x3F;
    setv(&V1->vf[1], 2, -2, 0.5f, 1);
    setv(&V1->vf[2], 0, 0, 0, -1);                   /* |w| is used */
    setv(&V1->vf[3], 0, 0, 3, 1);
    begin();
    P(U_CLIP(1, 2), L_NOP);                          /* 0: +x, -y */
    P(U_CLIP(3, 2), L_FCGET(8));                     /* 1: +z; FCGET sees the old clip */
    P(U_NOP, L_NOP); P(U_NOP, L_NOP);
    P(U_NOP, L_FCGET(2));                            /* 4: first CLIP visible */
    P(U_NOP, L_FCGET(3));                            /* 5: second: clip = 0x03F250 */
    P(U_NOP, L_FCAND(0x000010));                     /* 6: vi1 = 1 */
    P(U_NOP, L_IADD(9, 1, 0));
    P(U_NOP, L_FCAND(0x000001));                     /* vi1 = 0 */
    P(U_NOP, L_IADD(10, 1, 0));
    P(U_NOP, L_FCEQ(0x03F250));                      /* vi1 = 1 */
    P(U_NOP, L_IADD(4, 1, 0));
    P(U_NOP, L_FCOR(0xFC0DAF));                      /* (clip | imm) == 0xFFFFFF: vi1 = 1 */
    P(U_NOP, L_IADD(5, 1, 0));
    P(U_NOP, L_FCSET(0x000001));
    P(U_NOP, L_FCGET(6));
    PE();
    run1();
    CHECKU(V1->vi[8], 0x3F);
    CHECKU(V1->vi[2], (0x3F << 6 | 0x09) & 0xFFF);
    CHECKU(V1->vi[3], (0x09 << 6 | 0x10) & 0xFFF);
    CHECKU(V1->vi[9], 1);
    CHECKU(V1->vi[10], 0);
    CHECKU(V1->vi[4], 1);
    CHECKU(V1->vi[5], 1);
    CHECKU(V1->vi[6], 1);
    CHECKU(V1->clip, 1);
}

static void test_xgkick(void) {
    cur_test = "xgkick";
    reset1();
    ((uint32_t*)vu_data_mem(1))[0x20 * 4] = 0xCAFE;
    kicks = 0;
    begin();
    P(U_NOP, L_IADDIU(1, 0, 0x20));
    P(U_NOP, L_NOP);
    P(U_ADD(DX, 2, 0, 0), L_XGKICK(1));
    P(U_NOP, L_IADDIU(1, 0, 0x420));                 /* addresses wrap at 16 KB */
    P(U_NOP, L_NOP);
    P(U_NOP, L_XGKICK(1));
    PE();
    run1();
    CHECKU(kicks, 2);
    CHECK(kick_mem == vu_data_mem(1));
    CHECKU(kick_addr[0], 0x20);
    CHECKU(kick_addr[1], 0x20);
    CHECKU(kick_word0, 0xCAFE);
    CHECKU(kick_csr & 0xE040, 0);                    /* the GIF runs in the host FP mode (no RZ/FTZ/DAZ) */
}

static void test_vi_branch_quirk(void) {
    cur_test = "VI branch delay";
    reset1();
    begin();
    P(U_NOP, L_IADDIU(5, 0, 100));                   /* 0 */
    P(U_NOP, L_IBNE(5, 0, 2));                       /* 1: reads vi5 from before 0: not taken */
    P(U_NOP, L_NOP);                                 /* 2 */
    P(U_NOP, L_IADDIU(7, 0, 1));                     /* 3: runs */
    P(U_NOP, L_IADDIU(6, 0, 100));                   /* 4 */
    P(U_NOP, L_NOP);                                 /* 5: chain broken */
    P(U_NOP, L_IBNE(6, 0, 2));                       /* 6: taken -> 9 */
    P(U_NOP, L_NOP);                                 /* 7 */
    P(U_NOP, L_IADDIU(8, 0, 1));                     /* 8: skipped */
    P(U_NOP, L_IADDIU(9, 0, 1));                     /* 9 */
    P(U_NOP, L_IADDI(9, 9, 1));                      /* 10: chain of 4 on vi9 */
    P(U_NOP, L_IADDI(9, 9, 1));                      /* 11 */
    P(U_NOP, L_IADDI(9, 9, 1));                      /* 12 */
    P(U_NOP, L_IBEQ(9, 0, 2));                       /* 13: sees vi9 = 0 (before 9): taken -> 16 */
    P(U_NOP, L_NOP);                                 /* 14 */
    P(U_NOP, L_IADDIU(10, 0, 1));                    /* 15: skipped */
    PE();                                            /* 16, 17 */
    run1();
    CHECKU(V1->vi[7], 1);
    CHECKU(V1->vi[8], 0);
    CHECKU(V1->vi[9], 4);
    CHECKU(V1->vi[10], 0);

    cur_test = "VI branch, flag op";
    reset1();
    V1->vi[11] = 0;
    begin();
    P(U_NOP, L_FSOR(11, 0xFFF));                     /* 0: vi11 = 1 */
    P(U_NOP, L_IBNE(11, 0, 2));                      /* 1: sees the new value: taken -> 4 */
    P(U_NOP, L_NOP);                                 /* 2 */
    P(U_NOP, L_IADDIU(12, 0, 1));                    /* 3: skipped */
    P(U_NOP, L_ILW(DX, 13, 0, 0));                   /* 4: ILW: branch stalls, then reads the load */
    P(U_NOP, L_IBEQ(13, 0, 2));                      /* 5: vi13 = 7 -> not taken */
    P(U_NOP, L_NOP);                                 /* 6 */
    P(U_NOP, L_IADDIU(14, 0, 1));                    /* 7: runs */
    PE();
    ((uint32_t*)vu_data_mem(1))[0] = 7;
    run1();
    CHECKU(V1->vi[12], 0);
    CHECKU(V1->vi[13], 7);
    CHECKU(V1->vi[14], 1);

    /* PCSX2 VU documentation, note 2: a chain starts at the first write and only continues while
     * the next instruction reads AND writes the register. A write that does not read it starts a
     * new chain, so the branch sees the value from just before that write. */
    cur_test = "VI branch, write-only restarts the chain";
    reset1();
    V1->vi[6] = 3;
    begin();
    P(U_NOP, L_IADDIU(5, 0, 100));                   /* 0: chain on vi5 starts */
    P(U_NOP, L_IADDIU(5, 0, 0));                     /* 1: writes vi5 without reading it: new chain */
    P(U_NOP, L_IBNE(5, 0, 2));                       /* 2: sees 100 (before 1): taken -> 5 */
    P(U_NOP, L_NOP);                                 /* 3 */
    P(U_NOP, L_IADDIU(7, 0, 1));                     /* 4: skipped */
    P(U_NOP, L_IADDIU(8, 0, 100));                   /* 5: chain on vi8 */
    P(U_NOP, L_IADD(8, 6, 6));                       /* 6: vi8 = vi6 + vi6, does not read vi8: new chain */
    P(U_NOP, L_IBEQ(8, 0, 2));                       /* 7: sees 100: not taken */
    P(U_NOP, L_NOP);                                 /* 8 */
    P(U_NOP, L_IADDIU(9, 0, 1));                     /* 9: runs */
    PE();
    run1();
    CHECKU(V1->vi[5], 0);
    CHECKU(V1->vi[7], 0);
    CHECKU(V1->vi[8], 6);
    CHECKU(V1->vi[9], 1);

    /* The documentation's own example (Champions: Return to Arms): an SQI loop whose IBNE sees vi02
     * from before the chain, which includes the previous pass's delay slot. Exits after 256 SQIs. */
    cur_test = "VI branch, SQI loop example";
    reset1();
    begin();
    P(U_NOP, L_IADDIU(3, 2, 251));                   /* 0 */
    P(U_NOP, L_SQI(DXYZW, 20, 2));                   /* 1: loop */
    P(U_NOP, L_SQI(DXYZW, 21, 2));                   /* 2 */
    P(U_NOP, L_SQI(DXYZW, 22, 2));                   /* 3 */
    P(U_NOP, L_IBNE(3, 2, -4));                      /* 4: -> 1 */
    P(U_NOP, L_SQI(DXYZW, 23, 2));                   /* 5: delay slot */
    PE();
    run1();
    CHECKU(V1->vi[2], 256);
}

static void test_stalls(void) {
    cur_test = "FMAC stall";
    reset1();
    setv(&V1->vf[2], 1, 2, 3, 4);
    begin();
    P(U_ADD(DX, 1, 2, 2), L_NOP);                    /* 0 */
    P(U_ADD(DY, 3, 1, 2), L_NOP);                    /* 1: reads vf1.y only: no stall */
    P(U_ADD(DX, 4, 1, 2), L_NOP);                    /* 2: reads vf1.x: waits until 4 */
    PE();                                            /* 5, 6 */
    run1();
    CHECKU(last_cycles, 7);
    CHECKF(V1->vf[4].f[0], 3.0f);

    cur_test = "upper/lower order";
    reset1();
    setv(&V1->vf[1], 1, 1, 1, 1);
    setv(&V1->vf[2], 5, 5, 5, 5);
    uint32_t* d = (uint32_t*)vu_data_mem(1);
    d[4 * 3] = fb(9.0f);
    V1->I = fb(2.0f);
    begin();
    P(U_ADD(DXYZW, 1, 2, 2), L_SQ(DXYZW, 1, 0, 1));  /* SQ stores the old vf1 */
    P(U_ADD(DX, 3, 2, 2), L_LQ(DX, 2, 0, 3));        /* ADD reads the old vf2 */
    P(U_ADD(DXY, 4, 2, 2), L_MOVE(DXYZW, 4, 1));     /* both write vf4: upper wins in x, y */
    P(U_MULi(DX, 5, 1) | UI, fb(3.0f));              /* reads the old I (2) */
    P(U_MULi(DX, 6, 1), L_NOP);                      /* new I */
    PE();
    run1();
    CHECKU(d[4], fb(1.0f));
    CHECKF(V1->vf[1].f[0], 10.0f);
    CHECKF(V1->vf[3].f[0], 10.0f);
    CHECKF(V1->vf[2].f[0], 9.0f);
    CHECKF(V1->vf[4].f[0], 18.0f); CHECKF(V1->vf[4].f[1], 10.0f); CHECKF(V1->vf[4].f[2], 10.0f);
    CHECKF(V1->vf[5].f[0], 20.0f);
    CHECKF(V1->vf[6].f[0], 30.0f);
    CHECKU(V1->I, fb(3.0f));
}

static void test_efu_random_tops(void) {
    cur_test = "EFU";
    reset1();
    V1->P = fb(-1.0f);
    setv(&V1->vf[1], 1, 2, 3, 4);
    setv(&V1->vf[2], 3, 4, 0, 0);
    begin();
    P(U_NOP, L_ESADD(1));                            /* 0: 14 at cycle 11 */
    P(U_NOP, L_MFP(DX, 3));                          /* 1: old P */
    P(U_NOP, L_WAITP);                               /* 2: until 10 */
    P(U_NOP, L_MFP(DY, 3));                          /* 11: new */
    P(U_NOP, L_ELENG(2)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DZ, 3));
    P(U_NOP, L_ERSQRT(1, FW)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DW, 3));
    P(U_NOP, L_ESUM(1)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DX, 4));
    P(U_NOP, L_ERCPR(1, FW)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DY, 4));
    P(U_NOP, L_EEXP(0, FX)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DZ, 4));
    P(U_NOP, L_ESIN(0, FX)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DW, 4));
    P(U_NOP, L_ERLENG(2)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DX, 5));
    P(U_NOP, L_ESQRT(1, FW)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DY, 5));
    P(U_NOP, L_EATAN(0, FW)); P(U_NOP, L_WAITP); P(U_NOP, L_MFP(DZ, 5));
    PE();
    run1();
    CHECKF(V1->vf[3].f[0], -1.0f);
    CHECKF(V1->vf[3].f[1], 14.0f);
    CHECKF(V1->vf[3].f[2], 5.0f);
    CHECKF(V1->vf[3].f[3], 0.5f);
    CHECKF(V1->vf[4].f[0], 10.0f);
    CHECKF(V1->vf[4].f[1], 0.25f);
    CHECKF(V1->vf[4].f[2], 1.0f);
    CHECKF(V1->vf[4].f[3], 0.0f);
    CHECKNEAR(V1->vf[5].f[0], 0.2f, 1e-6f);
    CHECKF(V1->vf[5].f[1], 2.0f);
    CHECKNEAR(V1->vf[5].f[2], 0.785398163f, 1e-6f);

    /* ps2tek: the EFU is the FDIV pipeline without a writeback hazard check, "meaning that a stall
     * lasts for one cycle less than usual" -- for WAITP and for a new EFU op on a busy EFU alike.
     * The sequence is the game's own (376110651 at 0x00D0, 188054619 at 0x00A8): three ELENGs whose
     * MFPs each read the previous result while the next one is already running. */
    cur_test = "EFU busy stall";
    reset1();
    V1->P = fb(-1.0f);
    setv(&V1->vf[3], 3, 4, 0, 0);                    /* |vf3| = 5 */
    setv(&V1->vf[4], 0, 6, 8, 0);                    /* 10 */
    setv(&V1->vf[5], 1, 2, 2, 0);                    /* 3 */
    begin();
    P(U_NOP, L_ELENG(3));                            /* 0: P = 5 at 18 */
    P(U_NOP, L_ELENG(4));                            /* 1: EFU busy: issues at 17, P = 10 at 35 */
    P(U_NOP, L_MFP(DX, 22));                         /* 18: 5 */
    P(U_NOP, L_ELENG(5));                            /* 19: issues at 34, P = 3 at 52 */
    P(U_NOP, L_MFP(DY, 22));                         /* 35: 10 */
    P(U_NOP, L_WAITP);                               /* 36: until 51 */
    P(U_NOP, L_MFP(DZ, 22));                         /* 52: 3 */
    PE();                                            /* 53, 54 */
    run1();
    CHECKF(V1->vf[22].f[0], 5.0f);
    CHECKF(V1->vf[22].f[1], 10.0f);
    CHECKF(V1->vf[22].f[2], 3.0f);
    CHECKU(last_cycles, 55);

    cur_test = "random";
    reset1();
    V1->vf[1].u[0] = 0x12345678;
    begin();
    P(U_NOP, L_RINIT(1, FX));
    P(U_NOP, L_RGET(DX, 2));
    P(U_NOP, L_RNEXT(DXYZW, 3));
    P(U_NOP, L_RXOR(1, FX));
    P(U_NOP, L_RGET(DX, 4));
    PE();
    run1();
    CHECKU(V1->vf[2].u[0], 0x3F800000 | 0x345678);
    CHECK(V1->vf[3].f[0] >= 1.0f && V1->vf[3].f[0] < 2.0f && V1->vf[3].u[0] != V1->vf[2].u[0]);
    CHECKU(V1->vf[3].u[3], V1->vf[3].u[0]);
    CHECKU(V1->vf[4].u[0], 0x3F800000 | ((V1->vf[3].u[0] ^ 0x12345678) & 0x7FFFFF));

    cur_test = "xtop";
    reset1();
    vu_set_vif_tops(1, 0x123, 0x45);
    begin();
    P(U_NOP, L_XTOP(1));
    P(U_NOP, L_XITOP(2));
    PE();
    run1();
    CHECKU(V1->vi[1], 0x123); CHECKU(V1->vi[2], 0x45);
}

static void test_cache_invalidation(void) {
    cur_test = "decode cache";
    reset1();
    begin();
    P(U_NOP, L_IADDIU(1, 0, 1));
    PE();
    run1();
    CHECKU(V1->vi[1], 1);
    uint32_t w = L_IADDIU(1, 0, 2);
    vu_mem_access(0x11008000u, &w, 4, 1);            /* EE store into VU1 micro memory */
    vu1_start(0);
    CHECKU(V1->vi[1], 2);
    w = L_IADDIU(1, 0, 3);
    memcpy(vu_micro_mem(1), &w, 4);                  /* VIF MPG writes through the pointer */
    vu1_start(0);
    CHECKU(V1->vi[1], 3);
    uint32_t r = 0;
    vu_mem_access(0x11008000u, &r, 4, 0);
    CHECKU(r, w);
}

static ps2_ctx ctx;

static void test_vu0(void) {
    cur_test = "vu0 callms";
    memset(&ctx, 0, sizeof ctx);
    ctx.vf[0].f[3] = 1.0f;
    setv(&ctx.vf[1], 1, 2, 3, 4);
    ctx.vi[1] = 7;
    float q = 2.0f; memcpy(&ctx.vq, &q, 4);
    setv(&V1->vf[6], 6, 6, 6, 6);
    V1->vi[3] = 0x333;
    begin();
    P(U_NOP, L_NOP);                                 /* 0: entry at 1 */
    P(U_ADD(DXYZW, 2, 1, 1), L_IADDI(2, 1, 1));      /* 1 */
    P(U_MULq(DX, 3, 1), L_IADDIU(5, 0, 0x400));      /* 2 */
    P(U_NOP, L_NOP); P(U_NOP, L_NOP); P(U_NOP, L_NOP);
    P(U_NOP, L_SQ(DXYZW, 2, 5, 5));                  /* VU1 vf5 = vf2 */
    P(U_NOP, L_LQ(DXYZW, 4, 5, 6));                  /* vf4 = VU1 vf6 */
    P(U_NOP, L_ILW(DX, 3, 5, 32 + 3));               /* vi3 = VU1 vi3 */
    P(U_NOP, L_ISW(DX, 2, 5, 32 + 4));               /* VU1 vi4 = vi2 */
    P(U_NOP, L_SQ(DXYZW, 1, 0, 0x10));               /* VU0 data */
    P(U_NOP, L_DIV(0, FW, 1, FY));                   /* Q = 0.5 */
    PE();
    load(0, 0);
    vu0_callms(&ctx, 1);
    CHECKF(ctx.vf[2].f[0], 2); CHECKF(ctx.vf[2].f[3], 8);
    CHECKU(ctx.vi[2], 8);
    CHECKF(ctx.vf[3].f[0], 2);
    CHECKF(V1->vf[5].f[1], 4);
    CHECKF(ctx.vf[4].f[2], 6);
    CHECKU(ctx.vi[3], 0x333);
    CHECKU(V1->vi[4], 8);
    CHECKF(((float*)vu_data_mem(0))[0x10 * 4 + 1], 2.0f);
    CHECKF(ctx.vq, 0.5f);
    CHECKF(ctx.vf[0].f[3], 1.0f);
    CHECKU(vu0_cfc2_ctrl(&ctx, 26), 14);             /* TPC after the E delay slot */

    cur_test = "cfc2/ctc2";
    vu0_ctc2_ctrl(&ctx, 16, 0xFFF);
    CHECKU(vu0_cfc2_ctrl(&ctx, 16) & 0xFC0, 0xFC0);
    vu0_ctc2_ctrl(&ctx, 18, 0x123456);
    CHECKU(vu0_cfc2_ctrl(&ctx, 18), 0x123456);
    CHECKU(vu0_cfc2_ctrl(&ctx, 29), 0);              /* VPU-STAT: idle */
    vu0_ctc2_ctrl(&ctx, 27, 0x40);
    CHECKU(vu0_cfc2_ctrl(&ctx, 27), 0x40);
    vu0_ctc2_ctrl(&ctx, 28, 0x002);                  /* reset VU0 */
    CHECKU(vu0_cfc2_ctrl(&ctx, 16), 0);
    CHECKU(vu0_cfc2_ctrl(&ctx, 18), 0);
    vf128 r;
    setv(&r, 0.0f, -2.0f, 1.0f, -0.0f);
    vu0_macro_flags(&r, DXYZW);                      /* macro-mode flags: Z x,w; S y,w */
    CHECKU(vu0_cfc2_ctrl(&ctx, 17), 0x59);
    CHECKU(vu0_cfc2_ctrl(&ctx, 16), 0xC3);
    setv(&r, 3.0f, -2.0f, 1.0f, -1.0f);
    vu0_macro_flags(&r, DY);
    CHECKU(vu0_cfc2_ctrl(&ctx, 17), 0x40);

    /* R is 23 bits with a fixed 0x7F exponent; the EE-side copy can hold any value (the inline
     * CTC2 in ps2_runtime.h stores all 32 bits), so micro mode must not see the extra bits. */
    cur_test = "vu0 R register";
    begin();
    P(U_NOP, L_RGET(DXYZW, 9));
    PE();
    load(0, 0x40);
    ctx.vr = 0x12345678u;
    vu0_callms(&ctx, 0x40);
    CHECKU(ctx.vf[9].u[0], 0x3F800000u | (0x12345678u & 0x7FFFFFu));
    CHECKU(ctx.vr & 0xFF800000u, 0x3F800000u);

    /* CMSAR1 starts VU1 */
    reset1();
    begin();
    P(U_NOP, L_IADDIU(1, 0, 42));
    PE();
    load(1, 0x40);
    vu0_ctc2_ctrl(&ctx, 31, 0x40);
    CHECKU(V1->vi[1], 42);
    CHECKU(vu0_cfc2_ctrl(&ctx, 31), 0x40);
    CHECKU(vu_get_tpc(1), 0x43);
    CHECKU(vu1_busy(), 0);
}

/* Run the game's own VU0 microcode (.vutext program 11753019) if the ELF is available. */
static int read_file(const char* path, uint8_t** out, long* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    *out = malloc((size_t)*size);
    size_t got = fread(*out, 1, (size_t)*size, f);
    fclose(f);
    return got == (size_t)*size;
}
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void test_game_microcode(const char* elf) {
    cur_test = "game microcode";
    uint8_t* b; long size;
    if (!read_file(elf, &b, &size)) { fprintf(stderr, "note: %s not found, skipping game microcode test\n", elf); return; }
    /* sections */
    uint32_t shoff = rd32(b + 32); uint16_t shent, shnum, shstr;
    memcpy(&shent, b + 46, 2); memcpy(&shnum, b + 48, 2); memcpy(&shstr, b + 50, 2);
    const uint8_t* sh = b + shoff;
    uint32_t stroff = rd32(sh + shstr * shent + 16);
    uint32_t vt_addr = 0, vt_off = 0, tab_off = 0, tab_size = 0, ostr_off = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t* s = sh + i * shent;
        const char* name = (const char*)b + stroff + rd32(s);
        if (!strcmp(name, ".vutext")) { vt_addr = rd32(s + 12); vt_off = rd32(s + 16); }
        if (!strcmp(name, ".DVP.ovlytab")) { tab_off = rd32(s + 16); tab_size = rd32(s + 20); }
        if (!strcmp(name, ".DVP.ovlystrtab")) ostr_off = rd32(s + 16);
    }
    CHECK(vt_off && tab_off && ostr_off);
    if (!vt_off || !tab_off || !ostr_off) { free(b); return; }
    /* decode every overlay chunk through VU1's decoder: no invalid instruction may remain */
    int invalid = 0, pairs = 0;
    for (uint32_t k = 0; k < tab_size; k += 12) {
        const char* name = (const char*)b + ostr_off + rd32(b + tab_off + k);
        uint32_t ee = rd32(b + tab_off + k + 4), vua = rd32(b + tab_off + k + 8);
        uint32_t csize = 0;
        for (int i = 0; i < shnum; i++) {
            const uint8_t* s = sh + i * shent;
            if (!strcmp((const char*)b + stroff + rd32(s), name)) csize = rd32(s + 20);
        }
        memcpy(vu_micro_mem(1) + vua, b + vt_off + (ee - vt_addr), csize);
        vu_init_state(V1);
        for (uint32_t pc = vua / 8; pc < (vua + csize) / 8; pc++, pairs++)
            if (!V1->cache[pc].up || !V1->cache[pc].lo) invalid++;
        if (strstr(name, ".11753019.")) memcpy(vu_micro_mem(0), b + vt_off + (ee - vt_addr), csize);
    }
    CHECKU(invalid, 0);
    CHECK(pairs > 4000);
    memset(vu_micro_mem(1), 0, 16384);
    vu_init_state(V1);

    /* VU0 routine at 0x58: vf5..vf8 = M(vf1..vf4) * vf5..vf8 */
    memset(&ctx, 0, sizeof ctx);
    ctx.vf[0].f[3] = 1.0f;
    setv(&ctx.vf[1], 1, 0, 0, 0); setv(&ctx.vf[2], 0, 2, 0, 0); setv(&ctx.vf[3], 0, 0, 4, 0); setv(&ctx.vf[4], 10, 20, 30, 1);
    setv(&ctx.vf[5], 1, 1, 1, 1); setv(&ctx.vf[6], 0, 0, 0, 1); setv(&ctx.vf[7], 2, 0, 0, 0); setv(&ctx.vf[8], 1, 2, 3, 0);
    vu0_callms(&ctx, 0xC);                           /* VCALLMS 0xC, as the game does */
    CHECKF(ctx.vf[5].f[0], 11); CHECKF(ctx.vf[5].f[1], 22); CHECKF(ctx.vf[5].f[2], 34); CHECKF(ctx.vf[5].f[3], 1);
    CHECKF(ctx.vf[6].f[0], 10); CHECKF(ctx.vf[6].f[2], 30);
    CHECKF(ctx.vf[7].f[0], 2); CHECKF(ctx.vf[7].f[3], 0);
    CHECKF(ctx.vf[8].f[1], 4); CHECKF(ctx.vf[8].f[2], 12);
    CHECKU(vu0_cfc2_ctrl(&ctx, 26), 0xF0 / 8);       /* stopped after the delay slot at 0xE8 */

    /* VU0 routine at 0x1C0 (VCALLMS 0x38): its FMAND reads the MSUBx flags exactly 4 cycles
     * later and its FSAND the SUB.x vf24 flags; with immediate flags both branches would exit
     * early and leave vf19.x = 0. Path: vf19.x = min((ab)^2 - |d|^2) / ((ab)^2 - (ab - 400a)^2), 1). */
    cur_test = "game VU0 flags";
    memset(&ctx, 0, sizeof ctx);
    ctx.vf[0].f[3] = 1.0f;
    setv(&ctx.vf[14], 1, 0, 0, 0);                   /* a */
    setv(&ctx.vf[17], 10, 0, 0, 0);                  /* b */
    setv(&ctx.vf[15], 3, 4, 0, 0);                   /* d = vf15 - vf13 */
    setv(&ctx.vf[16], 1, 0, 0, 0);                   /* ACC (= 0) - 1 < 0 in all lanes */
    vu0_callms(&ctx, 0x38);
    CHECKF(ctx.vf[24].f[0], 75.0f);
    CHECKF(ctx.vf[25].f[0], -152000.0f);
    CHECKNEAR(ctx.vf[19].f[0], 75.0f / -152000.0f, 1e-9f);
    setv(&ctx.vf[15], 30, 40, 0, 0);                 /* |d|^2 > (ab)^2: FSAND path exits */
    vu0_callms(&ctx, 0x38);
    CHECKF(ctx.vf[24].f[0], 100.0f - 2500.0f);
    CHECKF(ctx.vf[19].f[0], 0.0f);
    free(b);
}

/* Rough speed check: a transform/perspective loop like the game's VU1 renderers. */
static void bench(void) {
    reset1();
    setv(&V1->vf[10], 1, 0, 0, 0); setv(&V1->vf[11], 0, 1, 0, 0);
    setv(&V1->vf[12], 0, 0, 1, 0); setv(&V1->vf[13], 0, 0, 5, 1);
    float* d = (float*)vu_data_mem(1);
    for (int i = 0; i < 256; i++) { d[i * 4] = (float)i; d[i * 4 + 1] = 1; d[i * 4 + 2] = 2; d[i * 4 + 3] = 1; }
    begin();
    P(U_NOP, L_IADDIU(4, 0, 0));                     /* 0: source pointer (FCAND writes vi01) */
    P(U_NOP, L_IADDIU(2, 0, 256));                   /* 1: dest pointer */
    P(U_NOP, L_IADDIU(3, 0, 255));                   /* 2: count */
    P(U_NOP, L_LQI(DXYZW, 1, 4));                    /* 3: loop */
    P(U_MULAbc(DXYZW, 10, 1, FX), L_NOP);
    P(U_MADDAbc(DXYZW, 11, 1, FY), L_NOP);
    P(U_MADDAbc(DXYZW, 12, 1, FZ), L_NOP);
    P(U_MADDbc(DXYZW, 2, 13, 1, FW), L_NOP);
    P(U_NOP, L_NOP); P(U_NOP, L_NOP); P(U_NOP, L_NOP);
    P(U_NOP, L_DIV(0, FW, 2, FW));
    P(U_CLIP(2, 2), L_WAITQ);
    P(U_MULq(DXYZ, 3, 2), L_FCAND(0x3F));
    P(U_NOP, L_NOP); P(U_NOP, L_NOP); P(U_NOP, L_NOP);
    P(U_FTOI(DXYZW, 4, 3, 1), L_ISUBIU(3, 3, 1));
    P(U_NOP, L_NOP); P(U_NOP, L_NOP);
    P(U_NOP, L_IBNE(3, 0, -18));                     /* 20 -> 3 */
    P(U_NOP, L_SQI(DXYZW, 4, 2));
    PE();
    load(1, 0);
    uint64_t p0 = V1->pairs, c0 = V1->cycles;
    clock_t t0 = clock();
    int reps = getenv("VU_BENCH_REPS") ? atoi(getenv("VU_BENCH_REPS")) : 400;
    for (int r = 0; r < reps; r++) vu1_start(0);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    uint64_t pairs = V1->pairs - p0, cyc = V1->cycles - c0;
    printf("bench: %llu pairs (%llu VU cycles) in %.3f s = %.1f ns/pair, %.0f M pairs/s\n",
           (unsigned long long)pairs, (unsigned long long)cyc, secs, secs * 1e9 / (double)pairs,
           (double)pairs / secs / 1e6);
    if (getenv("VU_BENCH_NOPS")) {                   /* pure per-pair overhead */
        begin();
        for (int i = 0; i < 1000; i++) P(U_NOP, L_NOP);
        PE();
        load(1, 0);
        p0 = V1->pairs;
        t0 = clock();
        for (int r = 0; r < reps * 20; r++) vu1_start(0);
        secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("bench nops: %.2f ns/pair\n", secs * 1e9 / (double)(V1->pairs - p0));
        return;
    }
    /* vertex 7: (7, 1, 7, 1) / 1 -> (7, 1, 7) * 16 */
    uint32_t* out = (uint32_t*)vu_data_mem(1) + 256 * 4;
    cur_test = "bench result";
    if (getenv("VU_BENCH_DUMP"))
        for (int i = 0; i < 10; i++) printf("out[%d] = %d %d %d %d\n", i, (int)out[i * 4], (int)out[i * 4 + 1], (int)out[i * 4 + 2], (int)out[i * 4 + 3]);
    CHECKU(out[7 * 4 + 0], 112); CHECKU(out[7 * 4 + 2], 112);
}

int main(int argc, char** argv) {
    vu_set_vif_tops(0, 0, 0);                        /* initialise both units */
    vu_set_vif_tops(1, 0, 0);
    test_arith();
    test_float_semantics();
    test_mac_flags();
    test_memory();
    test_integer();
    test_branches();
    test_ebit();
    test_q_latency();
    test_flags_pipeline();
    test_xgkick();
    test_vi_branch_quirk();
    test_stalls();
    test_efu_random_tops();
    test_cache_invalidation();
    test_vu0();
    test_game_microcode(argc > 1 ? argv[1] : "C:/Users/tanih/Projects/sly2-decomp/orig/SCUS_973.16");
    bench();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
