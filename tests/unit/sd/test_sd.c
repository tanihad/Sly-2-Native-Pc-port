/* Unit tests for the native sound backend (runtime/sd.c, runtime/sd_reverb.c).
 * Links only those files plus the fakes below (IOP RAM, transfer-completion hook). */
#include "sd_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ---- fakes for the IOP side ------------------------------------------------------------------ */

static uint8_t g_iop[2 << 20];
static int     g_done[2];

uint8_t* iop_mem(uint32_t a) {
    a &= 0x1FFFFFFFu;
    return a < sizeof g_iop ? g_iop + a : NULL;
}
void iop_sd_transfer_done(int channel) { g_done[channel & 1]++; }
static int64_t g_iop_last;                 /* the IOP's last pass (QPC); 0 = no IOP: never hold */
int64_t iop_last_run_time(void) { return g_iop_last; }

/* ---- test plumbing -------------------------------------------------------------------------- */

static int g_failed, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* libsd entry encodings */
#define VP(p, c, v) ((uint16_t)((p) << 8 | (c) | (v) << 1))
#define CP(p, c)    ((uint16_t)((p) << 8 | 0x80 | (c)))
#define SW(s, c)    ((uint16_t)((s) << 8 | (c)))
#define VA(a, c, v) ((uint16_t)((a) << 8 | 0x40 | (c) | (v) << 1))
enum { VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR };
enum { MMIX = 8, MVOLL, MVOLR, EVOLL, EVOLR, AVOLL, AVOLR, BVOLL, BVOLR, MVOLXL, MVOLXR };
enum { PMON = 0x13, NON, KON, KOFF, ENDX, VMIXL, VMIXEL, VMIXR, VMIXER };
enum { ESA = 0x1C, EEA, TSA, IRQA, SSA = 0x20, LSAX, NAX };

static int16_t* render(uint32_t frames) {
    int16_t* b = malloc(frames * 4);
    sd_render(b, frames);
    return b;
}

/* The mixing setup 989snd makes after sceSdInit (voices -> core 0 -> core 1 -> output). */
static void mix_setup(void) {
    sceSdInit(0);
    sceSdSetParam(CP(MMIX, 0) & ~0x80, 0xF00);
    sceSdSetParam(CP(MMIX, 1) & ~0x80, 0xF0C);
    for (int c = 0; c < 2; c++) {
        sceSdSetParam(CP(MVOLL, c), 0x3FFF);
        sceSdSetParam(CP(MVOLR, c), 0x3FFF);
    }
    memset(g_done, 0, sizeof g_done);
}

static void voice_start(int c, int v, uint32_t ssa, uint16_t pitch, uint16_t adsr1, uint16_t adsr2, uint16_t vol) {
    sceSdSetParam(VP(VOLL, c, v), vol);
    sceSdSetParam(VP(VOLR, c, v), vol);
    sceSdSetParam(VP(PITCH, c, v), pitch);
    sceSdSetParam(VP(ADSR1, c, v), adsr1);
    sceSdSetParam(VP(ADSR2, c, v), adsr2);
    sceSdSetAddr(VA(SSA, c, v), ssa);
    sceSdSetSwitch(SW(KON, c), 1u << v);
}

/* ---- reference ADPCM decoder, written independently from psx-spx's description --------------- */

static void ref_decode(const uint8_t* blk, int32_t* old, int32_t* older, int16_t* out) {
    static const int pos[5] = { 0, 60, 115, 98, 122 }, neg[5] = { 0, 0, -52, -55, -60 };
    int range = blk[0] & 15, filter = (blk[0] >> 4) & 7;
    if (range > 12) range = 9;                /* reserved shift values act as 9 */
    if (filter > 4) filter = 0;
    for (int i = 0; i < 28; i++) {
        int nibble = (i & 1) ? blk[2 + i / 2] >> 4 : blk[2 + i / 2] & 15;
        int t = nibble >= 8 ? nibble - 16 : nibble;
        double scaled = floor(t * 4096.0 / (double)(1 << range));    /* (t SHL 12) SAR shift */
        double pred = floor((*old * pos[filter] + *older * neg[filter] + 32) / 64.0);
        double s = scaled + pred;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        *older = *old;
        *old = (int32_t)s;
        out[i] = (int16_t)s;
    }
}

/* ---- ADPCM encoder (test data) ---------------------------------------------------------------- */

static void enc_block(const int16_t* x, int32_t hist[2], uint8_t flags, uint8_t* out) {
    static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
    double best = 1e300;
    int bf = 0, bs = 0;
    for (int f = 0; f < 5; f++)
        for (int s = 0; s < 13; s++) {
            int32_t h0 = hist[0], h1 = hist[1];
            double err = 0;
            for (int i = 0; i < 28; i++) {
                int32_t pred = (h0 * f0[f] + h1 * f1[f] + 32) >> 6;
                long nib = lrint((x[i] - pred) / (double)(1 << (12 - s)));
                if (nib < -8) nib = -8;
                if (nib > 7) nib = 7;
                int32_t d = (int32_t)nib * (1 << (12 - s)) + pred;
                d = d < -32768 ? -32768 : d > 32767 ? 32767 : d;
                err += (double)(x[i] - d) * (x[i] - d);
                h1 = h0;
                h0 = d;
            }
            if (err < best) { best = err; bf = f; bs = s; }
        }
    memset(out, 0, 16);
    out[0] = (uint8_t)(bs | bf << 4);
    out[1] = flags;
    for (int i = 0; i < 28; i++) {
        int32_t pred = (hist[0] * f0[bf] + hist[1] * f1[bf] + 32) >> 6;
        long nib = lrint((x[i] - pred) / (double)(1 << (12 - bs)));
        if (nib < -8) nib = -8;
        if (nib > 7) nib = 7;
        int32_t d = (int32_t)nib * (1 << (12 - bs)) + pred;
        d = d < -32768 ? -32768 : d > 32767 ? 32767 : d;
        hist[1] = hist[0];
        hist[0] = d;
        out[2 + i / 2] |= (uint8_t)((nib & 15) << ((i & 1) * 4));
    }
}

/* Encode `nblocks` blocks of `x` at SPU byte address `addr`; flags[i] per block (NULL = none). */
static void put_adpcm(uint32_t addr, const int16_t* x, int nblocks, const uint8_t* flags) {
    int32_t hist[2] = { 0, 0 };
    uint8_t* ram = (uint8_t*)sd_ram();
    for (int b = 0; b < nblocks; b++)
        enc_block(x + 28 * b, hist, flags ? flags[b] : 0, ram + addr + 16 * b);
}

/* A looping sine with `period` samples per cycle: `nblocks` blocks (start flag, end+repeat). */
static void put_sine_loop(uint32_t addr, int nblocks, double period, double amp) {
    int16_t* x = malloc(28 * nblocks * sizeof *x);
    uint8_t* fl = calloc(nblocks, 1);
    for (int i = 0; i < 28 * nblocks; i++) x[i] = (int16_t)lrint(amp * sin(2 * 3.14159265358979 * i / period));
    fl[0] = 4;
    fl[nblocks - 1] |= 3;
    /* encode block 0 with the predictor history the decoder has when it loops back, so the loop
     * point is as clean as the rest (iterate: the end history depends on the start) */
    int32_t hist[2] = { 0, 0 };
    uint8_t* ram = (uint8_t*)sd_ram();
    for (int pass = 0; pass < 3; pass++)
        for (int b = 0; b < nblocks; b++)
            enc_block(x + 28 * b, hist, fl[b], ram + addr + 16 * b);
    free(x);
    free(fl);
}

static double measure_freq(const int16_t* s, int n, int stride) {
    double first = -1, last = -1;
    int count = 0;
    for (int i = 1; i < n; i++) {
        int a = s[(i - 1) * stride], b = s[i * stride];
        if (a < 0 && b >= 0) {
            double t = (i - 1) + (double)(-a) / (double)(b - a);
            if (first < 0) first = t;
            last = t;
            count++;
        }
    }
    return count > 1 ? (count - 1) * 48000.0 / (last - first) : 0;
}

static double rms(const int16_t* s, int n, int stride) {
    double acc = 0;
    for (int i = 0; i < n; i++) acc += (double)s[i * stride] * s[i * stride];
    return sqrt(acc / n);
}

/* ---- tests ------------------------------------------------------------------------------------ */

static void test_adpcm(void) {
    printf("ADPCM decoder vs reference\n");
    srand(1234);
    int mismatches = 0;
    for (int trial = 0; trial < 4096; trial++) {
        uint8_t blk[16];
        for (int i = 0; i < 16; i++) blk[i] = (uint8_t)rand();
        blk[0] = (uint8_t)((trial & 15) | ((trial >> 4) & 7) << 4);   /* every shift x filter */
        int32_t h[2] = { (int16_t)rand(), (int16_t)rand() };
        int32_t old = h[0], older = h[1];
        int16_t a[28], b[28];
        sd_adpcm_decode_block(blk, h, a);
        ref_decode(blk, &old, &older, b);
        if (memcmp(a, b, sizeof a) || h[0] != old || h[1] != older) mismatches++;
    }
    CHECK(mismatches == 0, "%d of 4096 random blocks differ from the reference decoder", mismatches);

    /* a known block: filter 0, shift 0, nibbles 1 and -1 -> exactly +-4096 */
    uint8_t blk[16] = { 0x00, 0x00, 0xF1 };
    int32_t h[2] = { 0, 0 };
    int16_t out[28];
    sd_adpcm_decode_block(blk, h, out);
    CHECK(out[0] == 4096 && out[1] == -4096 && out[2] == 0, "known block: %d %d %d", out[0], out[1], out[2]);
}

static void test_gauss(void) {
    printf("Gaussian table\n");
    const int16_t* g = sd_gauss_table();
    int lo = 1 << 30, hi = -(1 << 30);
    for (int i = 0; i < 256; i++) {
        int s = g[0xFF - i] + g[0x1FF - i] + g[0x100 + i] + g[i];
        lo = s < lo ? s : lo;
        hi = s > hi ? s : hi;
    }
    CHECK(lo >= 0x7E00 && hi <= 0x8000, "tap sums %X..%X", lo, hi);
    CHECK(abs(g[0x1FF] - 0x59B3) <= 2 && abs(g[0x100] - 0x1307) <= 1 && abs(g[0]) <= 1,
          "end points %X %X %X", g[0x1FF], g[0x100], g[0]);
    for (int i = 1; i < 512; i++) CHECK(g[i] >= g[i - 1], "table not monotonic at %d", i);
}

static void test_note2pitch(void) {
    printf("sceSdNote2Pitch\n");
    struct { uint16_t cn, cf, n; int16_t f; uint16_t want; } k[] = {
        { 60, 0, 60, 0, 0x1000 }, { 60, 0, 72, 0, 0x2000 }, { 60, 0, 48, 0, 0x0800 },
        { 60, 0, 84, 0, 0x4000 }, { 60, 0, 96, 0, 0x4000 },          /* no shift beyond 2 octaves */
        { 60, 0, 61, 0, 0x10F4 }, { 60, 0, 59, 0, 0x0F1A }, { 60, 0, 60, -1, 0x0FFE },
        { 0, 0, 0, 0, 0x1000 }, { 64, 0, 52, 0, 0x0800 }, { 60, 0, 24, 0, 0x0200 },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        uint16_t p = sceSdNote2Pitch(k[i].cn, k[i].cf, k[i].n, k[i].f);
        CHECK(p == k[i].want, "Note2Pitch(%u,%u,%u,%d) = %04X, want %04X", k[i].cn, k[i].cf, k[i].n, k[i].f, p, k[i].want);
    }
    /* grid against an independent formulation (floor octave, Sony's truncated tables) and the ideal */
    int bad = 0, off_ideal = 0;
    for (int cn = 30; cn < 90; cn += 7)
        for (int cf = 0; cf < 128; cf += 31)
            for (int n = 0; n < 128; n++)
                for (int f = -127; f <= 127; f += 18) {
                    int total = (n - cn) * 128 + f + cf;
                    int oct = (int)floor(total / 1536.0), rem = total - oct * 1536;
                    uint32_t r = (uint32_t)(((uint64_t)(uint32_t)floor(32768.0 * pow(2.0, (rem >> 7) / 12.0)) *
                                             (uint32_t)floor(32768.0 * pow(2.0, (rem & 127) / 1536.0))) >> 16);
                    int sh = oct - 2;
                    if (sh < 0) r = sh < -31 ? 0 : (r + (1u << (-sh - 1))) >> -sh;
                    uint16_t p = sceSdNote2Pitch((uint16_t)cn, (uint16_t)cf, (uint16_t)n, (int16_t)f);
                    if (p != (uint16_t)r) bad++;
                    double ideal = 4096.0 * pow(2.0, total / 1536.0);
                    if (ideal > 64 && ideal < 0x4000 && fabs(p - ideal) > ideal * 0.001 + 1) off_ideal++;
                }
    CHECK(bad == 0, "%d grid values differ from the floor-octave formulation", bad);
    CHECK(off_ideal == 0, "%d grid values differ from 4096*2^(semitones/12) by more than 0.1%%", off_ideal);
}

static void test_registers(void) {
    printf("init defaults and register encodings\n");
    sceSdInit(0);
    CHECK(sceSdGetParam(VP(PITCH, 1, 23)) == 0x3FFF, "voice pitch after init");
    CHECK(sceSdGetAddr(VA(SSA, 0, 5)) == 0x5000, "SSA after init = %X", sceSdGetAddr(VA(SSA, 0, 5)));
    CHECK(sceSdGetParam(SW(MMIX, 0)) == 0xFF0 && sceSdGetParam(SW(MMIX, 1)) == 0xFFC, "MMIX defaults");
    CHECK(sceSdGetSwitch(SW(VMIXL, 0)) == 0xFFFFFF && sceSdGetSwitch(SW(VMIXER, 1)) == 0xFFFFFF, "VMIX defaults");
    CHECK(sceSdGetAddr(SW(EEA, 0)) == 0x1DFFFF && sceSdGetAddr(SW(EEA, 1)) == 0x1FFFFF, "EEA defaults");
    CHECK(sceSdGetParam(CP(AVOLL, 1)) == 0x7FFF && sceSdGetParam(CP(AVOLL, 0)) == 0, "AVOL defaults");
    CHECK(sceSdGetParam(0x2300) == 0xC000 && sceSdGetParam(0x2301) == 0xC001, "ATTR defaults");
    CHECK(sceSdGetSwitch(SW(ENDX, 0)) == 0, "ENDX after init");

    sceSdSetAddr(VA(SSA, 1, 3), 0x12345);                       /* 16-byte granularity */
    CHECK(sceSdGetAddr(VA(SSA, 1, 3)) == 0x12340, "SSA truncation: %X", sceSdGetAddr(VA(SSA, 1, 3)));
    sceSdSetAddr(VA(LSAX, 0, 7), 0x1ABCD0);
    CHECK(sceSdGetAddr(VA(LSAX, 0, 7)) == 0x1ABCD0, "LSAX round trip");
    sceSdSetAddr(SW(EEA, 0), 0x1A0000);
    CHECK(sceSdGetAddr(SW(EEA, 0)) == 0x1BFFFF, "EEA keeps only the top bits: %X", sceSdGetAddr(SW(EEA, 0)));
    sceSdSetSwitch(SW(PMON, 1), 0xFFFFFFFF);
    CHECK(sceSdGetSwitch(SW(PMON, 1)) == 0xFFFFFF, "switches are 24 bits");
    sceSdSetCoreAttr(0x8 | 1, 0x3F);
    CHECK((sceSdGetParam(0x2301) & 0x3F00) == 0x3F00, "noise clock in ATTR");
    sceSdSetCoreAttr(0x2 | 0, 1);
    CHECK(sceSdGetParam(0x2300) & 0x80, "effect enable bit");
    sceSdSetCoreAttr(0x2 | 0, 0);
    CHECK(!(sceSdGetParam(0x2300) & 0x80), "effect disable bit");
    sceSdSetParam(VP(VOLL, 0, 2), 0x3FFF);
    CHECK(sceSdGetParam(VP(VOLXL, 0, 2)) == 0x7FFE, "fixed volume reads back x2 in VOLX");
}

static void test_adsr(void) {
    printf("ADSR envelope\n");
    mix_setup();
    put_sine_loop(0x10000, 12, 48, 8000);
    /* linear attack, shift 11 step 0: +7 per sample */
    voice_start(1, 0, 0x10000, 0x1000, 0x2C00 | 0xA0 | 0x7, 0x1FC0 | 0x0D, 0x3FFF);
    free(render(100));
    CHECK(sceSdGetParam(VP(ENVX, 1, 0)) == 700, "attack after 100 samples = %d (want 700)", sceSdGetParam(VP(ENVX, 1, 0)));
    free(render(4681 - 100));
    CHECK(sceSdGetParam(VP(ENVX, 1, 0)) == 0x7FFF, "attack peak after 4681 samples = %X", sceSdGetParam(VP(ENVX, 1, 0)));
    /* exponential decay, shift 10 (time constant 2048 samples) to sustain level 7 (4000h) */
    int t = 0, env = 0x7FFF;
    while (env > 0x4000 && t < 5000) {
        free(render(1));
        t++;
        env = sceSdGetParam(VP(ENVX, 1, 0));
    }
    CHECK(t > 1300 && t < 1550, "decay to sustain level took %d samples (want ~1420)", t);
    free(render(10000));
    int sus = sceSdGetParam(VP(ENVX, 1, 0));
    CHECK(sus > 0x3FC0 && sus <= 0x4000, "sustain holds (rate 7Fh): %X", sus);
    /* linear release, shift 13: -8 every 4 samples */
    sceSdSetSwitch(SW(KOFF, 1), 1);
    free(render(4096));
    int rel = sceSdGetParam(VP(ENVX, 1, 0));
    CHECK(abs(rel - (sus - 8192)) <= 16, "release after 4096 samples = %d (want %d)", rel, sus - 8192);
    free(render(4200));
    CHECK(sceSdGetParam(VP(ENVX, 1, 0)) == 0, "release reaches 0");

    /* exponential attack: +7 per sample to 6000h, then 4x slower */
    voice_start(1, 1, 0x10000, 0x1000, 0x8000 | 0x2C00 | 0xF, 0x1FC0, 0x3FFF);
    free(render(3000));
    CHECK(sceSdGetParam(VP(ENVX, 1, 1)) == 21000, "exp attack below 6000h: %d", sceSdGetParam(VP(ENVX, 1, 1)));
    free(render(3511 - 3000 + 400));
    int e = sceSdGetParam(VP(ENVX, 1, 1));
    CHECK(e > 0x6000 && e < 0x6000 + 7 * 110, "exp attack above 6000h slows down: %X", e);
    free(render(4800));
    CHECK(sceSdGetParam(VP(ENVX, 1, 1)) == 0x7FFF, "exp attack completes");

    /* attack rate 7Fh never steps */
    voice_start(1, 2, 0x10000, 0x1000, 0x7F00 | 0xF, 0x1FC0, 0x3FFF);
    free(render(48000));
    CHECK(sceSdGetParam(VP(ENVX, 1, 2)) == 0, "attack rate 7Fh: %d", sceSdGetParam(VP(ENVX, 1, 2)));

    /* sustain increase (linear, shift 12 step 0: +7 every 2 samples) from sustain level 0 */
    voice_start(1, 3, 0x10000, 0x1000, 0x000F & 0, 0x0000 | (12 << 2) << 6, 0x3FFF);
    free(render(2000));    /* attack shift 0 is instant, decay shift 0 reaches level 800h fast */
    int s0 = sceSdGetParam(VP(ENVX, 1, 3));
    free(render(200));
    int s1 = sceSdGetParam(VP(ENVX, 1, 3));
    CHECK(s1 == 0x7FFF || s1 - s0 == 700, "sustain linear increase: %d -> %d", s0, s1);

    /* volume sweep: linear increase, +7 per sample, starting from the current volume */
    sceSdSetParam(VP(VOLL, 1, 3), 0x0100);
    sceSdSetParam(VP(VOLL, 1, 3), 0x8000 | 0x2C);
    int v0 = (int16_t)sceSdGetParam(VP(VOLXL, 1, 3));
    free(render(100));
    int v1 = (int16_t)sceSdGetParam(VP(VOLXL, 1, 3));
    CHECK(v0 == 0x200 && v1 - v0 == 700, "volume sweep: %d -> %d", v0, v1);
}

static void test_pitch(void) {
    printf("pitch accuracy (1 kHz sine at several pitches)\n");
    const struct { uint16_t pitch; double hz; } k[] = {
        { 0x1000, 1000.0 }, { 0x0800, 500.0 }, { 0x2000, 2000.0 }, { 0x0C00, 750.0 }, { 0x3FFF, 3999.756 }, { 0x0155, 83.252 },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        mix_setup();
        put_sine_loop(0x20000, 12, 48, 12000);
        voice_start(0, 4, 0x20000, k[i].pitch, 0x000F, 0x1FC0, 0x3FFF);
        int16_t* b = render(48000 * 2);
        double f = measure_freq(b + 2 * 4800, 48000 * 2 - 4800, 2);
        CHECK(fabs(f - k[i].hz) < k[i].hz * 0.002, "pitch %04X: %.3f Hz, want %.3f", k[i].pitch, f, k[i].hz);
        if (k[i].pitch == 0x1000) {
            double amp = rms(b + 2 * 4800, 48000, 2) * sqrt(2.0);
            CHECK(amp > 12000 * 0.93 && amp < 12000 * 1.01, "level through core 0 -> core 1: amplitude %.0f (want ~12000)", amp);
            double r = rms(b + 2 * 4800 + 1, 48000, 2) * sqrt(2.0);
            CHECK(fabs(r - amp) < 20, "left/right equal: %.0f %.0f", amp, r);
        }
        free(b);
    }
}

static void test_loops(void) {
    printf("looping, ENDX, NAX\n");
    mix_setup();
    int16_t x[28 * 3];
    for (int i = 0; i < 28 * 3; i++) x[i] = (int16_t)(1000 + i);
    uint8_t loopfl[3] = { 4, 0, 3 };
    put_adpcm(0x30000, x, 3, loopfl);
    voice_start(0, 0, 0x30000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    CHECK(sceSdGetAddr(VA(NAX, 0, 0)) == 0x30002, "NAX at KON = %X", sceSdGetAddr(VA(NAX, 0, 0)));
    free(render(28 * 2 + 5));
    CHECK(sceSdGetAddr(VA(NAX, 0, 0)) >= 0x30020 && sceSdGetAddr(VA(NAX, 0, 0)) < 0x30030,
          "NAX in third block: %X", sceSdGetAddr(VA(NAX, 0, 0)));
    CHECK(!(sceSdGetSwitch(SW(ENDX, 0)) & 1), "ENDX not yet set");
    free(render(28));
    CHECK(sceSdGetSwitch(SW(ENDX, 0)) & 1, "ENDX set at loop end");
    CHECK(sceSdGetAddr(VA(NAX, 0, 0)) >= 0x30000 && sceSdGetAddr(VA(NAX, 0, 0)) < 0x30010,
          "looped back to the start: %X", sceSdGetAddr(VA(NAX, 0, 0)));
    CHECK(sceSdGetParam(VP(ENVX, 0, 0)) == 0x7FFF, "repeat keeps the voice playing");
    sceSdSetSwitch(SW(ENDX, 0), 0);
    CHECK(sceSdGetSwitch(SW(ENDX, 0)) == 0, "writing ENDX clears it");

    /* one-shot: end without repeat mutes the voice but it keeps running from the loop address */
    uint8_t onefl[3] = { 4, 0, 1 };
    put_adpcm(0x31000, x, 3, onefl);
    voice_start(0, 1, 0x31000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    free(render(28 * 3 + 2));
    CHECK(sceSdGetSwitch(SW(ENDX, 0)) & 2, "one-shot sets ENDX");
    CHECK(sceSdGetParam(VP(ENVX, 0, 1)) == 0, "one-shot end mutes (ENVX 0)");
    uint32_t nax1 = sceSdGetAddr(VA(NAX, 0, 1));
    free(render(28 * 10));
    CHECK(sceSdGetAddr(VA(NAX, 0, 1)) != nax1, "a muted voice keeps advancing NAX");

    /* LSAX written by software (no loop-start flag in the data) */
    uint8_t nfl[3] = { 0, 0, 3 };
    put_adpcm(0x32000, x, 3, nfl);
    voice_start(0, 2, 0x32000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    sceSdSetAddr(VA(LSAX, 0, 2), 0x32010);
    free(render(28 * 3 + 2));
    uint32_t nax2 = sceSdGetAddr(VA(NAX, 0, 2));
    CHECK(nax2 >= 0x32010 && nax2 < 0x32020, "loop to software LSAX: NAX %X", nax2);
    CHECK(sceSdGetAddr(VA(LSAX, 0, 2)) == 0x32010, "LSAX kept: %X", sceSdGetAddr(VA(LSAX, 0, 2)));

    /* NAX advances with playback: pitch 0x1000 = 28 samples per block */
    voice_start(1, 5, 0x40000, 0x1000, 0x000F, 0x1FC0, 0);
    free(render(28 * 50));
    uint32_t nax = sceSdGetAddr(VA(NAX, 1, 5));
    CHECK(nax == 0x40000 + 50 * 16 + 2, "NAX after 50 blocks = %X (want %X)", nax, 0x40000 + 50 * 16 + 2);
    sceSdSetParam(VP(PITCH, 1, 5), 0x2000);
    free(render(28 * 25));
    nax = sceSdGetAddr(VA(NAX, 1, 5));
    CHECK(nax == 0x40000 + 100 * 16 + 2, "NAX at double speed = %X (want %X)", nax, 0x40000 + 100 * 16 + 2);
    /* the same holds for a released silent voice (fast path) */
    sceSdSetSwitch(SW(KOFF, 1), 1u << 5);
    sceSdSetParam(VP(ENVX, 1, 5), 0);
    free(render(28 * 25));
    nax = sceSdGetAddr(VA(NAX, 1, 5));
    CHECK(nax == 0x40000 + 150 * 16 + 2, "NAX of a silent voice = %X (want %X)", nax, 0x40000 + 150 * 16 + 2);
}

/* sd_hold (the debug pause): silence, no voice moves, the audio clock keeps counting. */
static void test_debug_hold(void) {
    printf("debug pause hold\n");
    mix_setup();
    put_sine_loop(0x60000, 8, 56.0, 12000);
    voice_start(0, 3, 0x60000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    int16_t* a = render(4800);
    CHECK(rms(a + 2 * 2400, 2400, 2) > 1000, "playing before the hold: rms %.0f", rms(a + 2 * 2400, 2400, 2));
    free(a);
    uint32_t nax = sceSdGetAddr(VA(NAX, 0, 3));
    uint16_t env = sceSdGetParam(VP(ENVX, 0, 3));
    uint64_t clock = sd_frames_rendered();
    sd_hold(1);
    int16_t* h = render(9600);
    int loud = 0;
    for (int i = 0; i < 2 * 9600; i++) loud |= h[i];
    free(h);
    CHECK(!loud, "silence while held");
    CHECK(sceSdGetAddr(VA(NAX, 0, 3)) == nax, "NAX moved while held: %X -> %X", nax, sceSdGetAddr(VA(NAX, 0, 3)));
    CHECK(sceSdGetParam(VP(ENVX, 0, 3)) == env, "envelope moved while held");
    CHECK(sd_frames_rendered() == clock + 9600, "audio clock while held: +%llu (want 9600)",
          (unsigned long long)(sd_frames_rendered() - clock));
    sd_hold(0);
    int16_t* b = render(4800);
    CHECK(rms(b + 2 * 2400, 2400, 2) > 1000, "playing after the hold: rms %.0f", rms(b + 2 * 2400, 2400, 2));
    free(b);
    CHECK(sceSdGetAddr(VA(NAX, 0, 3)) != nax, "NAX moves again after the hold");
    sceSdSetSwitch(SW(KOFF, 0), 1u << 3);
}

static void test_transfers(void) {
    printf("VoiceTrans\n");
    mix_setup();
    for (int i = 0; i < 0x1000; i++) g_iop[0x10000 + i] = (uint8_t)(i * 7 + 3);
    uint8_t* ram = (uint8_t*)sd_ram();
    memset(ram + 0x50000, 0xEE, 0x200);
    int r = sceSdVoiceTrans(0, 0, 0x10000, 0x50000, 100);
    CHECK(r == 128, "VoiceTrans returns the size rounded to 64: %d", r);
    CHECK(memcmp(ram + 0x50000, g_iop + 0x10000, 128) == 0, "data written to sound memory");
    CHECK(ram[0x50080] == 0xEE, "nothing beyond the rounded size");
    CHECK(g_done[0] == 1 && g_done[1] == 0, "completion announced once on channel 0 (%d,%d)", g_done[0], g_done[1]);
    CHECK(sceSdVoiceTransStatus(0, 1) == 1, "status: done");
    CHECK(sceSdGetAddr(SW(TSA, 0)) == 0x50080, "TSA after transfer: %X", sceSdGetAddr(SW(TSA, 0)));
    memset(g_iop + 0x20000, 0, 0x100);
    r = sceSdVoiceTrans(1, 1, 0x20000, 0x50000, 64);
    CHECK(r == 64 && memcmp(g_iop + 0x20000, ram + 0x50000, 64) == 0, "read back");
    CHECK(g_done[1] == 1, "read completion on channel 1");
    CHECK(sceSdVoiceTrans(0, 2, 0, 0, 0) == 0, "stop");
    /* wrap at the end of sound memory */
    r = sceSdVoiceTrans(0, 0, 0x10000, 0x1FFFC0, 128);
    CHECK(r == 128 && memcmp(ram + 0x1FFFC0, g_iop + 0x10000, 64) == 0 && memcmp(ram, g_iop + 0x10040, 64) == 0,
          "transfer wraps at 2 MB");
}

/* four 1.15 volume stages (BVOL, MVOL core 0, AVOL core 1, MVOL core 1) each lose up to 1 LSB */
static int close_to(int got, int want) { return abs(got - want) <= 4 + abs(want) / 1000; }

static void put_pcm_blocks(uint32_t addr, int nblocks) {
    for (int b = 0; b < nblocks; b++)
        for (int i = 0; i < 256; i++) {
            int16_t l = (int16_t)(1000 * (b + 1)), r = (int16_t)-l;
            memcpy(g_iop + addr + 1024 * b + 2 * i, &l, 2);
            memcpy(g_iop + addr + 1024 * b + 512 + 2 * i, &r, 2);
        }
}

static void test_adma(void) {
    printf("BlockTrans (sound-data input)\n");
    mix_setup();
    sceSdSetParam(SW(MMIX, 0), 0xC0);                /* core 0: sound-data input dry only */
    sceSdSetParam(SW(MMIX, 1), 0x0C);                /* core 1: external input (core 0) dry */
    sceSdSetParam(CP(BVOLL, 0), 0x7FFF);
    sceSdSetParam(CP(BVOLR, 0), 0x7FFF);
    put_pcm_blocks(0x60000, 8);
    CHECK(sceSdBlockTrans(0, 0x10, 0x60000, 0x2000, 0) == 0, "BlockTrans loop start");
    CHECK((sceSdBlockTransStatus(0, 0) & 0xFFFFFF) == 0x60000, "status before playback");
    int16_t* b = render(511);
    CHECK(close_to(b[0], 1000) && close_to(b[1], -1000), "first block plays: %d %d", b[0], b[1]);
    CHECK(close_to(b[2 * 300], 2000), "second block plays: %d", b[2 * 300]);
    CHECK(g_done[0] == 0, "no half done yet");
    free(b);
    free(render(1));
    CHECK(g_done[0] == 1, "first half announced when its last block is fetched (%d)", g_done[0]);
    CHECK((sceSdBlockTransStatus(0, 0) >> 24) == 1, "status reports the second half");
    free(render(1024));
    CHECK(g_done[0] == 2, "second half announced (%d)", g_done[0]);
    b = render(512);
    CHECK(close_to(b[0], 7000) && close_to(b[2 * 256], 8000), "end of buffer: %d %d", b[0], b[2 * 256]);
    free(b);
    b = render(256);
    CHECK(close_to(b[0], 1000), "loops to the start: %d", b[0]);
    free(b);
    uint32_t st = (uint32_t)sceSdBlockTrans(0, 2, 0, 0, 0);
    CHECK((st & 0xFFFFFF) >= 0x60000 && (st & 0xFFFFFF) < 0x62000, "stop returns the position: %X", st);
    b = render(64);
    CHECK(b[10] == 0 && b[11] == 0, "silent after stop");
    free(b);

    /* WRITE_FROM: start in the middle (block 5), as 989snd's movie player does */
    memset(g_done, 0, sizeof g_done);
    sceSdBlockTrans(0, 0x13, 0x60000, 0x2000, 0x60000 + 5 * 1024);
    b = render(256);
    CHECK(close_to(b[0], 6000), "write-from starts at block 5: %d", b[0]);
    free(b);
    sceSdBlockTrans(0, 2, 0, 0, 0);

    /* one-shot: one completion, then silence */
    memset(g_done, 0, sizeof g_done);
    sceSdBlockTrans(0, 0, 0x60000 + 2048, 1024, 0);
    b = render(300);
    CHECK(g_done[0] == 1 && close_to(b[0], 3000) && b[2 * 290] == 0, "one-shot: done %d, %d then %d", g_done[0], b[0], b[2 * 290]);
    free(b);
}

static void test_routing(void) {
    printf("core routing, mute, noise, pitch modulation\n");
    mix_setup();
    put_sine_loop(0x20000, 12, 48, 12000);
    voice_start(0, 0, 0x20000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    int16_t* b = render(4800);
    CHECK(rms(b + 2400, 2400, 2) > 5000, "core 0 voice reaches the output");
    free(b);
    sceSdSetParam(CP(AVOLL, 1), 0);
    sceSdSetParam(CP(AVOLR, 1), 0);
    b = render(4800);
    CHECK(rms(b + 2400, 2400, 2) < 1, "core 1 AVOL 0 blocks core 0");
    free(b);
    sceSdSetParam(CP(AVOLL, 1), 0x7FFF);
    sceSdSetParam(CP(AVOLR, 1), 0x7FFF);
    sceSdSetSwitch(SW(VMIXL, 0), 0);                 /* left dry send off for every core-0 voice */
    b = render(4800);
    CHECK(rms(b + 2400, 2400, 2) < 1 && rms(b + 2401, 2400, 2) > 5000, "VMIXL gates the left send");
    free(b);
    sceSdSetSwitch(SW(VMIXL, 0), 0xFFFFFF);
    sceSdSetCoreAttr(0x6 | 1, 0);                    /* clear the core-1 unmute bit */
    b = render(4800);
    CHECK(rms(b + 2400, 2400, 2) < 1, "mute bit silences the core");
    free(b);
    sceSdSetCoreAttr(0x6 | 1, 1);

    /* noise replaces the sample data */
    mix_setup();
    sceSdSetCoreAttr(0x8 | 1, 0x30);
    sceSdSetSwitch(SW(NON, 1), 1u << 7);
    voice_start(1, 7, 0x5000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    b = render(4800);
    double nr = rms(b + 2400, 2400, 2);
    int changes = 0;
    for (int i = 1; i < 4800; i++) changes += b[2 * i] != b[2 * i - 2];
    CHECK(nr > 4000 && changes > 100, "noise voice: rms %.0f, %d changes", nr, changes);
    free(b);

    /* pitch modulation: voice 0 outputs DC 4000h (silent in the mix) -> voice 1 runs 1.5x faster */
    mix_setup();
    uint8_t* ram = (uint8_t*)sd_ram();
    memset(ram + 0x70000, 0x44, 16);                 /* filter 0, shift 0, nibbles 4 -> +4000h */
    ram[0x70000] = 0x00;
    ram[0x70001] = 7;
    put_sine_loop(0x20000, 12, 48, 12000);
    sceSdSetSwitch(SW(PMON, 1), 1u << 1);
    voice_start(1, 0, 0x70000, 0x1000, 0x000F, 0x1FC0, 0);
    voice_start(1, 1, 0x20000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    b = render(48000);
    double f = measure_freq(b + 4800 * 2, 48000 - 4800, 2);
    CHECK(fabs(f - 1500.0) < 5.0, "PMON with factor C000h: %.2f Hz (want ~1500)", f);
    free(b);
}

static void test_reverb(void) {
    printf("reverb presets: bounded, decaying impulse responses\n");
    static const char* names[] = { "off", "room", "studio A", "studio B", "studio C", "hall", "space", "echo", "delay", "pipe" };
    uint8_t* ram = (uint8_t*)sd_ram();
    for (int mode = 1; mode < 10; mode++) {
        mix_setup();
        /* impulse: one block with a pulse, then a silent end+mute block */
        int16_t x[56] = { 0 };
        x[3] = 20000;
        uint8_t fl[2] = { 0, 1 };
        put_adpcm(0x30000, x, 2, fl);
        sd_effect_attr a = { 1, mode | 0x100, 0x7FFF, 0x7FFF, 32, 64 };
        CHECK(sceSdSetEffectAttr(1, &a) == 0, "SetEffectAttr(%d)", mode);
        uint32_t esa = sceSdGetAddr(SW(ESA, 1));
        CHECK(esa == 0x200000 - sd_reverb_size[mode], "%s: ESA %X (want %X)", names[mode], esa, 0x200000 - sd_reverb_size[mode]);
        memset(ram + esa - 0x100, 0x5A, 0x100);       /* guard below the work area */
        sceSdSetCoreAttr(0x2 | 1, 1);
        sceSdSetSwitch(SW(VMIXL, 1), 0);              /* wet only */
        sceSdSetSwitch(SW(VMIXR, 1), 0);
        sceSdSetSwitch(SW(VMIXEL, 1), 1);
        sceSdSetSwitch(SW(VMIXER, 1), 1);
        voice_start(1, 0, 0x30000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
        const int secs = 4, win = 4800;
        int16_t* b = render(48000 * secs);
        double peak = 0, last = 0;
        int maxabs = 0;
        for (int w = 0; w < 48000 * secs / win; w++) {
            double l = rms(b + 2 * w * win, win, 2), r = rms(b + 2 * w * win + 1, win, 2);
            double e = l > r ? l : r;
            if (e > peak) peak = e;
            last = e;
        }
        for (int i = 0; i < 2 * 48000 * secs; i++) maxabs = abs(b[i]) > maxabs ? abs(b[i]) : maxabs;
        int guard_ok = 1;
        for (int i = 0; i < 0x100; i++) guard_ok &= ram[esa - 0x100 + i] == 0x5A;
        printf("  %-9s work area %6X bytes, peak rms %6.0f, last %8.3f, max %5d\n", names[mode], sd_reverb_size[mode], peak, last, maxabs);
        CHECK(peak > 50, "%s: reverb produces output (peak rms %.1f)", names[mode], peak);
        CHECK(maxabs < 32767, "%s: output bounded (max %d)", names[mode], maxabs);
        /* the documented arithmetic floors (SAR 15), so the IIRs can settle on a few LSB of DC */
        CHECK(last < peak * 0.01 || last < 6.0, "%s: impulse response decays (last %.2f vs peak %.1f)", names[mode], last, peak);
        CHECK(guard_ok, "%s: reverb wrote outside its work area", names[mode]);
        free(b);
        /* stays silent after enough time */
        b = render(48000 * 4);
        double tail = rms(b, 48000 * 4, 2);
        CHECK(tail < 6.0, "%s: tail after 8 s rms %.2f", names[mode], tail);
        free(b);
    }
    /* echo at maximum delay and feedback (vWALL 7FFEh): a loud input must not make it grow */
    mix_setup();
    sd_effect_attr a = { 1, 7 | 0x100, 0x7FFF, 0x7FFF, 127, 127 };
    sceSdSetEffectAttr(1, &a);
    sceSdSetCoreAttr(0x2 | 1, 1);
    sceSdSetSwitch(SW(VMIXL, 1), 0);
    sceSdSetSwitch(SW(VMIXR, 1), 0);
    sceSdSetSwitch(SW(VMIXEL, 1), 1);
    sceSdSetSwitch(SW(VMIXER, 1), 1);
    put_sine_loop(0x20000, 12, 48, 30000);
    voice_start(1, 0, 0x20000, 0x1000, 0x000F, 0x1FC0 | 0x5, 0x3FFF);
    free(render(48000 * 2));
    sceSdSetSwitch(SW(KOFF, 1), 1);
    int16_t* b = render(48000 * 8);
    double first = rms(b + 2 * 48000, 48000, 2), lastsec = rms(b + 2 * 48000 * 7, 48000, 2);
    printf("  echo, feedback 127: rms %.0f after 1 s, %.0f after 7 s\n", first, lastsec);
    CHECK(first > 100 && lastsec <= first * 1.05, "full-feedback echo grows: %.0f -> %.0f", first, lastsec);
    free(b);
}

/* The voices never play more than the driver limit (60 ms = 2880 frames of sound) past the IOP's last
 * pass, however the rendering is spread out, so 989snd never finds a stream voice further on than it
 * can refill. "The IOP ran" = iop_last_run_time() changed. */
static int all_zero(const int16_t* b, uint32_t frames) {
    for (uint32_t i = 0; i < 2 * frames; i++) if (b[i]) return 0;
    return 1;
}

static void test_driver_pace(void) {
    printf("driver pace (voices held while the IOP does not run)\n");
    const uint32_t lim = 2880;
    mix_setup();
    sd_set_driver_limit_ms(60);
    put_sine_loop(0x20000, 12, 48, 12000);
    voice_start(0, 4, 0x20000, 0x1000, 0x000F, 0x1FC0, 0x3FFF);
    g_iop_last = 1;                                       /* the IOP has run */
    int16_t* b = render(2000);
    CHECK(rms(b + 2 * 1000, 1000, 2) > 5000, "plays while the IOP keeps up: rms %.0f", rms(b + 2 * 1000, 1000, 2));
    free(b);
    g_iop_last = 2;                                       /* it ran again: a fresh allowance */
    uint64_t h0, f0, h1, f1, clock0 = sd_frames_rendered();
    sd_driver_holds(&h0, &f0);
    b = render(4800);                                     /* no pass during these 100 ms */
    CHECK(rms(b + 2 * (lim - 1000), 1000, 2) > 5000 && all_zero(b + 2 * lim, 4800 - lim),
          "plays the first %u frames, then silence (rms %.0f before)", lim, rms(b + 2 * (lim - 1000), 1000, 2));
    free(b);
    uint32_t nax = sceSdGetAddr(VA(NAX, 0, 4));
    uint16_t env = sceSdGetParam(VP(ENVX, 0, 4));
    b = render(4800);
    CHECK(all_zero(b, 4800), "silence while the IOP stays behind");
    free(b);
    sd_driver_holds(&h1, &f1);
    CHECK(sceSdGetAddr(VA(NAX, 0, 4)) == nax, "NAX held: %X -> %X", nax, sceSdGetAddr(VA(NAX, 0, 4)));
    CHECK(sceSdGetParam(VP(ENVX, 0, 4)) == env, "envelope held");
    CHECK(h1 == h0 + 1 && f1 - f0 == 9600 - lim, "one hold of %u frames (%llu holds, %llu frames)", 9600 - lim,
          (unsigned long long)(h1 - h0), (unsigned long long)(f1 - f0));
    CHECK(sd_frames_rendered() - clock0 == 9600, "the audio clock runs on during a hold");
    /* the IOP ran: the voice continues where it stopped */
    g_iop_last = 3;
    b = render(28 * 4);
    uint32_t nax2 = sceSdGetAddr(VA(NAX, 0, 4));
    int moved = (((int)nax2 - (int)nax) % (12 * 16) + 12 * 16) % (12 * 16);   /* inside the 12-block loop */
    CHECK(moved >= 4 * 16 - 16 && moved <= 4 * 16 + 16,
          "continues from the held position: NAX %X -> %X after 4 blocks (%d bytes)", nax, nax2, moved);
    CHECK(rms(b, 28 * 4, 2) > 5000, "audible again: rms %.0f", rms(b, 28 * 4, 2));
    free(b);
    /* a burst (the output thread catching up after it was held up itself) counts in sound time too */
    g_iop_last = 4;
    b = render(9600);
    CHECK(!all_zero(b + 2 * (lim - 64), 64) && all_zero(b + 2 * lim, 9600 - lim), "a burst also stops at the limit");
    free(b);
    /* limit 0, or no IOP side: never hold */
    sd_set_driver_limit_ms(0);
    b = render(4800);
    CHECK(rms(b + 2 * 3800, 1000, 2) > 5000, "limit 0 never holds");
    free(b);
    sd_set_driver_limit_ms(60);
    g_iop_last = 0;
    b = render(4800);
    CHECK(rms(b + 2 * 3800, 1000, 2) > 5000, "no IOP side: never holds");
    free(b);
}

static void test_perf(void) {
    printf("performance\n");
    mix_setup();
    put_sine_loop(0x20000, 12, 48, 6000);
    sceSdSetCoreAttr(0x2 | 1, 1);
    for (int c = 0; c < 2; c++) {
        for (int v = 0; v < 24; v++) voice_start(c, v, 0x20000, (uint16_t)(0x0800 + v * 0x80), 0x000F, 0x1FC0, 0x0800);
    }
    sd_effect_attr a = { 1, 5, 0x3000, 0x3000, 0, 0 };
    sceSdSetEffectAttr(1, &a);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int16_t* b = render(48000 * 10);
    QueryPerformanceCounter(&t1);
    free(b);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart / 10.0;
    printf("  48 voices + reverb: %.1f ms of CPU per second of audio\n", ms);
    CHECK(ms < 250, "mixer too slow: %.1f ms per second", ms);
}

int main(void) {
    test_adpcm();
    test_gauss();
    test_note2pitch();
    test_registers();
    test_adsr();
    test_pitch();
    test_loops();
    test_debug_hold();
    test_transfers();
    test_adma();
    test_routing();
    test_reverb();
    test_driver_pace();
    test_perf();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
