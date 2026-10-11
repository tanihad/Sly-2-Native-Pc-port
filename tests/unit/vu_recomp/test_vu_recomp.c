/* Differential test of the VU recompiler (recomp/vu_recomp.py): every random program from gen.py
 * runs with the interpreter (vu_run) and with its recompiled code (vu_execute) from the same random
 * states; registers, flags, Q/P/R/I, TPC, data memory, XGKICK addresses and issue cycles, pair and
 * cycle counts must all be identical.
 *   test_vu_recomp <programs.bin> [states per program]
 */
#include "vu_internal.h"
#include "vu_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <xmmintrin.h>

static uint64_t khash;
static uint32_t kicks;
void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) {
    (void)vu1_data;
    khash = vu_kick_hash(khash, qw_addr, vu_st[1].cyc);
    kicks++;
}
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}
uint32_t ps2_vblank_count(void) { return 0; }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
/* Float bit patterns that exercise the VU's number handling: zeros, denormals, exponent 255,
 * FLT_MAX, small integers (ITOF/MTIR), ordinary values. */
static uint32_t rfloat(void) {
    static const uint32_t pool[] = { 0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x40000000,
                                     0x00400000, 0x80000001, 0x7F800000, 0xFF800001, 0x7F7FFFFF, 0xFFFFFFFF,
                                     0x00000005, 0x0000FFFF, 0x43200000, 0x3C010247, 0x4B000000, 0x00800000 };
    uint32_t k = rnd() % 4;
    if (k == 0) return pool[rnd() % (sizeof pool / sizeof pool[0])];
    if (k == 1) return rnd() << 16 ^ rnd();
    float f = ((float)(int)(rnd() % 20001) - 10000.0f) / (float)(1 + rnd() % 300);
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

typedef struct {
    vf128 vf[32], acc;
    uint16_t vi[16];
    uint32_t I, Q, P, R, status, mac, clip, tpc, top, itop;
} regs;
static void save(regs* r, const vu_state* st) {
    memcpy(r->vf, st->vf, sizeof r->vf); r->acc = st->acc; memcpy(r->vi, st->vi, sizeof r->vi);
    r->I = st->I; r->Q = st->Q; r->P = st->P; r->R = st->R; r->status = st->status; r->mac = st->mac;
    r->clip = st->clip; r->tpc = st->tpc; r->top = st->top; r->itop = st->itop;
}
static void load(vu_state* st, const regs* r) {
    memcpy(st->vf, r->vf, sizeof r->vf); st->acc = r->acc; memcpy(st->vi, r->vi, sizeof r->vi);
    st->I = r->I; st->Q = r->Q; st->P = r->P; st->R = r->R; st->status = r->status; st->mac = r->mac;
    st->clip = r->clip; st->tpc = r->tpc; st->top = r->top; st->itop = r->itop;
}
static void random_state(vu_state* st) {
    for (int k = 1; k < 32; k++) for (int l = 0; l < 4; l++) st->vf[k].u[l] = rfloat();
    for (int l = 0; l < 4; l++) st->acc.u[l] = rfloat();
    for (int k = 1; k < 16; k++) st->vi[k] = (uint16_t)(rnd() % 4 ? rnd() % 0x500 : rnd());
    st->I = rfloat(); st->Q = rfloat(); st->P = rfloat();
    st->R = 0x3F800000u | (rnd() & 0x7FFFFF);
    st->status = rnd() % 3 ? rnd() & 0xFFF : 0;
    st->mac = rnd() & 0xFFFF;
    st->clip = rnd() & 0xFFFFFF;
    st->top = rnd() & 0x3FF;
    st->itop = rnd() & 0x3FF;
    uint32_t* d = (uint32_t*)st->data;
    for (uint32_t i = 0; i < (st->dmask + 1u) * 4u; i++) d[i] = rfloat();
}

typedef struct { uint32_t pairs, cycles, kicks; uint64_t khash, hash, vu1hash; regs r; } result;

static result run(int unit, uint32_t pc, int compiled) {
    vu_state* st = &vu_st[unit];
    khash = VU_HASH_SEED;
    kicks = 0;
    const uint64_t c0 = st->cycles;
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(VU_MXCSR);
    result res;
    res.pairs = compiled ? vu_execute(st, pc) : vu_run(st, pc);
    _mm_setcsr(csr);
    res.cycles = (uint32_t)(st->cycles - c0);
    res.kicks = kicks;
    res.khash = khash;
    res.hash = vu_state_hash(st);
    res.vu1hash = unit == 0 ? vu_state_hash(&vu_st[1]) : 0;   /* VU0 programs can write VU1's registers */
    save(&res.r, st);
    return res;
}

static void diff(const result* a, const result* b) {
    for (int k = 0; k < 32; k++)
        if (memcmp(&a->r.vf[k], &b->r.vf[k], 16))
            printf("    vf%02d interp %08X %08X %08X %08X compiled %08X %08X %08X %08X\n", k, a->r.vf[k].u[0], a->r.vf[k].u[1],
                   a->r.vf[k].u[2], a->r.vf[k].u[3], b->r.vf[k].u[0], b->r.vf[k].u[1], b->r.vf[k].u[2], b->r.vf[k].u[3]);
    if (memcmp(&a->r.acc, &b->r.acc, 16)) printf("    acc differs\n");
    for (int k = 0; k < 16; k++)
        if (a->r.vi[k] != b->r.vi[k]) printf("    vi%02d interp %04X compiled %04X\n", k, a->r.vi[k], b->r.vi[k]);
    const char* nm[8] = { "I", "Q", "P", "R", "status", "mac", "clip", "tpc" };
    const uint32_t x[8] = { a->r.I, a->r.Q, a->r.P, a->r.R, a->r.status, a->r.mac, a->r.clip, a->r.tpc };
    const uint32_t y[8] = { b->r.I, b->r.Q, b->r.P, b->r.R, b->r.status, b->r.mac, b->r.clip, b->r.tpc };
    for (int k = 0; k < 8; k++) if (x[k] != y[k]) printf("    %s interp %08X compiled %08X\n", nm[k], x[k], y[k]);
    printf("    pairs %u/%u cycles %u/%u kicks %u/%u%s%s\n", a->pairs, b->pairs, a->cycles, b->cycles, a->kicks, b->kicks,
           a->khash != b->khash ? " (xgkick addresses/cycles differ)" : "", a->vu1hash != b->vu1hash ? " (VU1 registers differ)" : "");
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_vu_recomp <programs.bin> [states]\n"); return 2; }
    const int nstates = argc > 2 ? atoi(argv[2]) : 24;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);

    vu_set_vif_tops(0, 0, 0);                                 /* initialises both units */
    vu_set_vif_tops(1, 0, 0);
    /* VU_FUZZ_ONLY=<program>:<state>: run just that one (with -DVU_TRACE: compare the timelines) */
    int only_prog = -1, only_state = -1;
    const char* only = getenv("VU_FUZZ_ONLY");
    if (only) sscanf(only, "%d:%d", &only_prog, &only_state);
    static uint8_t data_before[16384], vu1_data_before[16384];
    static regs vu1_before;
    int programs = 0, runs = 0, failures = 0;
    uint64_t compiled_starts = 0, pairs = 0;
    for (long pos = 0; pos + 8 <= size;) {
        uint32_t unit, np;
        memcpy(&unit, buf + pos, 4);
        memcpy(&np, buf + pos + 4, 4);
        pos += 8;
        vu_state* st = &vu_st[unit];
        uint8_t* micro = vu_micro_mem((int)unit);
        memset(micro, 0, (st->pcmask + 1u) * 8u);
        memcpy(micro, buf + pos, np * 8u);
        pos += (long)np * 8;
        for (int s = 0; s < nstates; s++) {
            random_state(st);
            if (only_prog >= 0 && (programs != only_prog || s != only_state)) continue;
            if (only_prog >= 0) fprintf(stderr, "--- interpreter\n");
            regs before;
            save(&before, st);
            memcpy(data_before, st->data, (st->dmask + 1u) * 16u);
            save(&vu1_before, &vu_st[1]);
            memcpy(vu1_data_before, vu_data_mem(1), 16384);
            const result a = run((int)unit, 0, 0);
            load(st, &before);
            memcpy(st->data, data_before, (st->dmask + 1u) * 16u);
            load(&vu_st[1], &vu1_before);
            memcpy(vu_data_mem(1), vu1_data_before, 16384);
            const uint64_t cs = vu_st[unit].cprograms;
            if (only_prog >= 0) fprintf(stderr, "--- compiled\n");
            const result b = run((int)unit, 0, 1);
            compiled_starts += vu_st[unit].cprograms - cs;
            pairs += a.pairs;
            runs++;
            if (a.pairs != b.pairs || a.cycles != b.cycles || a.kicks != b.kicks || a.khash != b.khash ||
                a.hash != b.hash || a.vu1hash != b.vu1hash) {
                if (failures++ < 12) {
                    printf("program %d (VU%u, %u pairs), state %d: compiled code differs from the interpreter\n",
                           programs, unit, np, s);
                    diff(&a, &b);
                }
            }
        }
        programs++;
    }
    printf("%d programs x %d states (%llu pairs): %d differ; %llu of %d runs started in recompiled code\n", programs,
           nstates, (unsigned long long)pairs, failures, (unsigned long long)compiled_starts, runs);
    return failures || compiled_starts != (uint64_t)runs || !runs ? 1 : 0;
}
