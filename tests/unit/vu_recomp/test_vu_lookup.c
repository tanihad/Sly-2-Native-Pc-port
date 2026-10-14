/* How runtime/vu.c picks recompiled code (vu_execute): a program start runs compiled code only when
 * the unit's micro memory holds a compiled image of that unit and the start address is one of its
 * entries; every write to micro memory (vu_micro_mem(), EE stores) makes it look again. Images from
 * gen.py lookup_images(): la (VU1: routines at 0x0000 and 0x0018), lb (VU1: 2 pairs over la's
 * start), lc (VU1: 2 pairs at 0x0040 without an E bit), ld/le/lf (a pair the compiled code cannot
 * run in the delay slot of IBNE / JR / IBEQ on VU0). Every run is also compared with the
 * interpreter on the same state.
 *   test_vu_lookup <lk_images.bin> */
#include "vu_internal.h"
#include "vu_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <xmmintrin.h>

void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) { (void)vu1_data; (void)qw_addr; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}
uint32_t ps2_vblank_count(void) { return 0; }

static int failures;

#define E_NOP  0x400002FFu                /* upper: [E] nop */
#define U_NOP  0x000002FFu
#define L_NOP  0x8000033Cu
static uint32_t iaddiu(unsigned t, unsigned s, unsigned imm) {
    return 0x08u << 25 | ((imm >> 11) & 15) << 21 | t << 16 | s << 11 | (imm & 0x7FF);
}
static void put(int unit, uint32_t pc, uint32_t upper, uint32_t lower) {
    uint8_t* m = vu_micro_mem(unit);
    memcpy(m + pc * 8, &lower, 4);
    memcpy(m + pc * 8 + 4, &upper, 4);
}
static void load_la(int unit) {
    uint8_t* m = vu_micro_mem(unit);
    memset(m, 0, unit ? 16384 : 4096);
    put(unit, 0, U_NOP, iaddiu(1, 0, 0x11)); put(unit, 1, E_NOP, L_NOP); put(unit, 2, U_NOP, L_NOP);
    put(unit, 3, U_NOP, iaddiu(1, 0, 0x22)); put(unit, 4, U_NOP, iaddiu(2, 1, 1));
    put(unit, 5, E_NOP, L_NOP); put(unit, 6, U_NOP, L_NOP);
}

/* Run at pc through vu_execute and, from the same state, with the interpreter; both must agree, and
 * the run must (not) have started in compiled code. */
static void check(const char* what, int unit, uint32_t pc, int want_compiled, unsigned vi_reg, uint16_t want_vi) {
    vu_state* st = &vu_st[unit];
    static uint8_t data[16384];
    vu_state before = *st;
    memcpy(data, st->data, (st->dmask + 1u) * 16u);
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(VU_MXCSR);
    const uint64_t c0 = st->cprograms;
    const uint32_t n = vu_execute(st, pc);
    const int compiled = st->cprograms != c0;
    const uint64_t h = vu_state_hash(st);
    const uint16_t got = st->vi[vi_reg];
    const uint32_t tpc = st->tpc;
    vu_state after = *st;
    *st = before;
    memcpy(st->data, data, (st->dmask + 1u) * 16u);
    const uint32_t n2 = vu_run(st, pc);
    const uint64_t h2 = vu_state_hash(st);
    _mm_setcsr(csr);
    *st = after;
    if (compiled != want_compiled || n != n2 || h != h2 || got != want_vi) {
        printf("FAIL %s: VU%d at 0x%04X: %s (want %s), pairs %u/%u, state %s, vi%u = 0x%X (want 0x%X), tpc 0x%X\n", what,
               unit, pc * 8, compiled ? "compiled" : "interpreted", want_compiled ? "compiled" : "interpreted", n, n2,
               h == h2 ? "same" : "differs", vi_reg, got, want_vi, tpc * 8);
        failures++;
    }
}

/* lk_images.bin (gen.py): unit, start, pair count, pairs, per image in lookup_images() order */
static uint8_t* images;
static long images_size;
static void load_image(int k) {
    long pos = 0;
    for (int i = 0; pos + 12 <= images_size; i++) {
        uint32_t unit, start, np;
        memcpy(&unit, images + pos, 4); memcpy(&start, images + pos + 4, 4); memcpy(&np, images + pos + 8, 4);
        pos += 12;
        if (i == k) {
            uint8_t* m = vu_micro_mem((int)unit);
            memset(m, 0, unit ? 16384 : 4096);
            memcpy(m + start * 8, images + pos, np * 8u);
            return;
        }
        pos += (long)np * 8;
    }
    printf("no image %d\n", k);
    exit(2);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_vu_lookup <lk_images.bin>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    images_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    images = malloc((size_t)images_size);
    if (fread(images, 1, (size_t)images_size, f) != (size_t)images_size) return 2;
    fclose(f);
    vu_set_vif_tops(0, 0, 0);                                 /* initialises both units */
    vu_set_vif_tops(1, 0, 0);
    uint32_t w;

    load_la(1);
    check("la entry 0", 1, 0, 1, 1, 0x11);
    check("la routine start 0x18", 1, 3, 1, 1, 0x22);
    check("start inside la, not an entry", 1, 4, 0, 2, 0x23);
    check("la routine start again (cached slot)", 1, 3, 1, 2, 0x23);

    put(1, 3, U_NOP, iaddiu(1, 0, 0x44));                     /* VIF MPG path: stale compiled code */
    check("la after an MPG write changed it", 1, 3, 0, 1, 0x44);
    w = iaddiu(1, 0, 0x22);                                   /* EE store restores the image */
    vu_mem_access(0x11008000u + 3 * 8, &w, 4, 1);
    check("la restored by an EE store", 1, 3, 1, 1, 0x22);
    w = iaddiu(1, 0, 0x55);                                   /* EE store changes it again */
    vu_mem_access(0x11008000u + 3 * 8, &w, 4, 1);
    check("la after an EE store changed it", 1, 3, 0, 1, 0x55);
    w = iaddiu(1, 0, 0x22);
    vu_mem_access(0x11008000u + 3 * 8, &w, 4, 1);
    check("la restored", 1, 3, 1, 1, 0x22);

    w = 0;                                                    /* writes to VU0's micro memory or to data */
    vu_mem_access(0x11000000u + 0x100, &w, 4, 1);             /* memory leave VU1's choice alone */
    vu_mem_access(0x1100C000u + 0x100, &w, 4, 1);
    check("la after unrelated writes", 1, 3, 1, 1, 0x22);

    put(1, 0, E_NOP, iaddiu(1, 0, 0x33));                     /* lb over la's first two pairs */
    put(1, 1, U_NOP, L_NOP);
    check("lb over la", 1, 0, 1, 1, 0x33);
    check("la's routine with lb over its start", 1, 3, 0, 1, 0x22);

    load_la(0);                                               /* la is compiled for VU1 only */
    check("la in VU0's memory", 0, 3, 0, 1, 0x22);

    uint8_t* m = vu_micro_mem(1);                             /* lc runs off its end into an E bit */
    memset(m, 0, 16384);
    put(1, 8, U_NOP, iaddiu(3, 0, 5));
    put(1, 9, U_NOP, 0x80000000u | 3u << 16 | 3u << 11 | 1u << 6 | 0x32u);   /* iaddi vi3, vi3, 1 */
    put(1, 10, E_NOP, L_NOP);
    put(1, 11, U_NOP, L_NOP);
    check("lc continues outside its image", 1, 8, 1, 3, 6);
    if (vu_st[1].tpc != 12) { printf("FAIL lc: tpc 0x%X, want 0x60\n", vu_st[1].tpc * 8); failures++; }

    load_image(3);                                            /* invalid instruction in an IBNE delay slot */
    check("ld: invalid lower op in a conditional branch's delay slot", 1, 0, 1, 3, 0x99);
    load_image(4);                                            /* invalid upper op in a JR delay slot */
    check("le: invalid upper op in a JR delay slot", 1, 0, 1, 3, 0x99);
    load_image(5);                                            /* XGKICK on VU0 in an IBEQ delay slot */
    check("lf: XGKICK on VU0 in a conditional branch's delay slot", 0, 0, 1, 3, 0x99);

    printf("%s\n", failures ? "VU lookup tests FAILED" : "VU lookup tests passed");
    return failures ? 1 : 0;
}
