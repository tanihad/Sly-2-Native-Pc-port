/* Unit tests for runtime/timer.c (EE timers T0-T3), driven by a fake host clock. */
#include "hw.h"

#include <stdio.h>

void timer_set_host_clock(uint64_t (*now)(void), uint64_t freq);   /* test hook in timer.c */

/* ---- fakes for the neighbours ------------------------------------------------------------------ */

static int irqs[16];
void intc_raise(int cause) { irqs[cause & 15]++; }

static int unhandled;
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
    unhandled++;
}

static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }

/* ---- helpers ------------------------------------------------------------------------------------- */

#define BUS 147456000ull
enum { CLK_BUS = 0, CLK_16 = 1, CLK_256 = 2, CLK_HBLNK = 3, GATE = 1 << 2, GATS_V = 1 << 3, ZRET = 1 << 6,
       CUE = 1 << 7, CMPE = 1 << 8, OVFE = 1 << 9, EQUF = 1 << 10, OVFF = 1 << 11 };
#define GATM(x) ((x) << 4)

#define COUNT(n) (0x10000000u + (n) * 0x800u)
#define MODE(n)  (COUNT(n) + 0x10)
#define COMP(n)  (COUNT(n) + 0x20)
#define HOLD(n)  (COUNT(n) + 0x30)

static uint32_t rd(uint32_t a) { uint32_t v; timer_access(a, &v, 4, 0); return v; }
static void wr(uint32_t a, uint32_t v) { timer_access(a, &v, 4, 1); }

/* Fresh timers; with freq = BUS one host tick is exactly one bus cycle. */
static void reset(uint64_t freq) {
    fake_now = 1000000;
    timer_set_host_clock(fake_clock, freq);
    memset(irqs, 0, sizeof irqs);
    unhandled = 0;
}
static void advance(uint64_t host_ticks) { fake_now += host_ticks; }

static int checks, fails;
#define CHECK_EQ(a, b) do {                                                                          \
        unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b);              \
        checks++;                                                                                   \
        if (a_ != b_) { fails++; printf("  %s:%d: %s = 0x%llX (%llu), expected 0x%llX (%llu)\n",     \
                                        __func__, __LINE__, #a, a_, a_, b_, b_); }                  \
    } while (0)
#define CHECK_NEAR(a, b, tol) do {                                                                   \
        long long a_ = (long long)(a), b_ = (long long)(b);                                         \
        checks++;                                                                                   \
        if (a_ - b_ > (tol) || b_ - a_ > (tol)) { fails++; printf("  %s:%d: %s = %lld, expected %lld +- %d\n", \
                                                 __func__, __LINE__, #a, a_, b_, (int)(tol)); }      \
    } while (0)

/* ---- clock sources ------------------------------------------------------------------------------- */

static void test_rates(void) {
    reset(BUS);
    wr(MODE(0), CUE | CLK_BUS);
    wr(MODE(1), CUE | CLK_16);
    wr(MODE(2), CUE | CLK_256);
    wr(MODE(3), CUE | CLK_HBLNK);
    advance(25600);
    CHECK_EQ(rd(COUNT(0)), 25600);
    CHECK_EQ(rd(COUNT(1)), 1600);
    CHECK_EQ(rd(COUNT(2)), 100);
    CHECK_EQ(rd(COUNT(3)), 2);                       /* 25600 / 9371.66 bus cycles per line */
    advance(BUS - 25600);                            /* one second in total */
    CHECK_EQ(rd(COUNT(0)), BUS & 0xFFFF);
    CHECK_EQ(rd(COUNT(1)), (BUS / 16) & 0xFFFF);
    CHECK_EQ(rd(COUNT(2)), (BUS / 256) & 0xFFFF);
    CHECK_EQ(rd(COUNT(3)), 15734);                   /* 15734.264 Hz */
    advance(9 * BUS);                                /* ten seconds */
    CHECK_EQ(rd(COUNT(3)), 157342 & 0xFFFF);
    CHECK_EQ(rd(COUNT(2)), (10 * BUS / 256) & 0xFFFF);
    CHECK_EQ(irqs[INTC_TIM0] + irqs[INTC_TIM1] + irqs[INTC_TIM2] + irqs[INTC_TIM3], 0);   /* nothing enabled */
    CHECK_EQ(rd(MODE(0)), CUE);                      /* no flags without CMPE/OVFE */
}

static void test_host_clock_conversion(void) {
    reset(10000000);                                 /* a typical 10 MHz QueryPerformanceCounter */
    wr(MODE(2), CUE | CLK_256);
    advance(1000000);                                /* 0.1 s */
    CHECK_EQ(rd(COUNT(2)), 57600);
    wr(COUNT(2), 0);
    advance(100000000);                              /* 10 s: 5,760,000 ticks */
    CHECK_EQ(rd(COUNT(2)), 5760000 & 0xFFFF);
    /* Over about 4 days the host-to-bus conversion must neither drift nor overflow. */
    advance(10000000ull * 86400 * 4);
    CHECK_EQ(rd(COUNT(2)), (5760000 + 576000ull * 86400 * 4) & 0xFFFF);
    reset(10000000);
    wr(MODE(0), CUE | CLK_BUS);
    advance(1);                                      /* 100 ns = 14.7456 bus cycles */
    CHECK_EQ(rd(COUNT(0)), 14);
    advance(1);
    CHECK_EQ(rd(COUNT(0)), 29);
}

static void test_prescaler_grid(void) {
    /* The /256 prescaler ticks on a fixed grid: frequent updates must not lose fractions. */
    reset(BUS);
    wr(MODE(1), CUE | CLK_256);
    for (int i = 0; i < 1000; i++) { advance(100); rd(COUNT(1)); }
    CHECK_EQ(rd(COUNT(1)), 100000 / 256);
}

static void test_cue_and_count_write(void) {
    reset(BUS);
    wr(MODE(0), CLK_BUS);                            /* CUE = 0: stopped */
    advance(500);
    CHECK_EQ(rd(COUNT(0)), 0);
    wr(COUNT(0), 0x1234);
    advance(500);
    CHECK_EQ(rd(COUNT(0)), 0x1234);
    wr(MODE(0), CUE | CLK_BUS);                      /* resumes from the written value */
    advance(0x100);
    CHECK_EQ(rd(COUNT(0)), 0x1334);
    wr(COUNT(0), 0x12345);                           /* 16 bits */
    CHECK_EQ(rd(COUNT(0)), 0x2345);
    advance(5);
    CHECK_EQ(rd(COUNT(0)), 0x234A);
    wr(MODE(0), CLK_BUS);                            /* stop again: holds */
    advance(1000);
    CHECK_EQ(rd(COUNT(0)), 0x234A);
    /* Changing CLKS while running keeps the count and switches the rate from then on. */
    reset(BUS);
    wr(MODE(0), CUE | CLK_BUS);
    advance(1000);
    wr(MODE(0), CUE | CLK_16);
    advance(1600);
    CHECK_EQ(rd(COUNT(0)), 1100);
}

/* ---- compare / overflow ------------------------------------------------------------------------- */

static void test_compare(void) {
    reset(BUS);
    wr(COMP(1), 100);
    wr(MODE(1), CUE | CMPE | CLK_BUS);
    advance(99);
    timer_service();
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), 0);
    CHECK_EQ(irqs[INTC_TIM1], 0);
    advance(1);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM1], 1);
    CHECK_EQ(rd(MODE(1)), CUE | CMPE | EQUF);        /* OVFE off: no OVFF even after wrapping */
    advance(3 * 65536);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM1], 1);                    /* edge-triggered: flag still set */
    CHECK_EQ(rd(COUNT(1)), 100);
    wr(MODE(1), CUE | CMPE | EQUF);                  /* acknowledge */
    CHECK_EQ(rd(MODE(1)), CUE | CMPE);
    advance(65535);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM1], 1);
    advance(1);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM1], 2);
    CHECK_EQ(rd(MODE(1)) & EQUF, EQUF);
    /* other timers untouched */
    CHECK_EQ(irqs[INTC_TIM0] + irqs[INTC_TIM2] + irqs[INTC_TIM3], 0);
}

static void test_compare_needs_enable(void) {
    reset(BUS);
    wr(COMP(0), 10);
    wr(MODE(0), CUE | CLK_BUS);
    advance(20);
    timer_service();
    CHECK_EQ(rd(MODE(0)), CUE);
    CHECK_EQ(irqs[INTC_TIM0], 0);
    wr(MODE(0), CUE | CMPE | CLK_BUS);               /* enabling later, past COMP: no event yet */
    CHECK_EQ(rd(MODE(0)), CUE | CMPE);
    CHECK_EQ(irqs[INTC_TIM0], 0);
}

static void test_overflow(void) {
    reset(BUS);
    wr(COUNT(2), 0xFFF0);
    wr(COMP(2), 0x8000);
    wr(MODE(2), CUE | OVFE | CLK_BUS);
    advance(15);
    timer_service();
    CHECK_EQ(rd(COUNT(2)), 0xFFFF);
    CHECK_EQ(irqs[INTC_TIM2], 0);
    advance(1);
    CHECK_EQ(rd(COUNT(2)), 0);                       /* a register read alone also catches up */
    CHECK_EQ(irqs[INTC_TIM2], 1);
    CHECK_EQ(rd(MODE(2)), CUE | OVFE | OVFF);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM2], 1);
}

static void test_events_collapse(void) {
    reset(BUS);
    wr(COMP(3), 5);
    wr(MODE(3), CUE | CMPE | OVFE | CLK_BUS);
    advance(3 * 65536 + 10);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM3], 1);
    CHECK_EQ(rd(MODE(3)), CUE | CMPE | OVFE | EQUF | OVFF);
    CHECK_EQ(rd(COUNT(3)), 10);
}

static void test_edge_per_flag(void) {
    /* A flag going 0 -> 1 raises the interrupt even while the other flag is still set. */
    reset(BUS);
    wr(COMP(0), 0x100);
    wr(MODE(0), CUE | CMPE | OVFE | CLK_BUS);
    advance(0x100);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM0], 1);
    advance(0x10000 - 0x100);                        /* wraps; EQUF never acknowledged */
    timer_service();
    CHECK_EQ(irqs[INTC_TIM0], 2);
    CHECK_EQ(rd(MODE(0)) & (EQUF | OVFF), EQUF | OVFF);
}

static void test_flag_clearing(void) {
    reset(BUS);
    wr(COMP(1), 1);
    wr(MODE(1), CUE | CMPE | OVFE | CLK_BUS);
    advance(0x10000);
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), EQUF | OVFF);
    wr(MODE(1), CUE | CMPE | OVFE | OVFF);           /* write 1 to OVFF only */
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), EQUF);
    wr(MODE(1), CUE | CMPE | OVFE);                  /* writing 0 leaves the flag */
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), EQUF);
    wr(MODE(1), CUE | CMPE | OVFE | EQUF);
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), 0);
    /* Clearing a flag by a write with the other flag's bit 0 doesn't clear the other one, and a
     * cleared flag is set again by the next event. */
    advance(0x10000);
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), EQUF | OVFF);
    wr(MODE(1), (rd(MODE(1)) & ~EQUF));
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), EQUF);
}

static void test_events_before_comp_write(void) {
    /* Events before a COMP write are judged with the old COMP. */
    reset(BUS);
    wr(COMP(0), 10);
    wr(MODE(0), CUE | CMPE | CLK_BUS);
    advance(20);
    wr(COMP(0), 50);                                 /* no timer_service in between */
    CHECK_EQ(irqs[INTC_TIM0], 1);
    wr(MODE(0), CUE | CMPE | EQUF);
    advance(29);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM0], 1);
    advance(1);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM0], 2);
}

/* ---- ZRET ---------------------------------------------------------------------------------------- */

static void test_zret(void) {
    reset(BUS);
    wr(COMP(0), 99);
    wr(MODE(0), CUE | ZRET | CMPE | OVFE | CLK_BUS);
    advance(99);
    CHECK_EQ(rd(COUNT(0)), 99);                      /* reaches COMP ... */
    CHECK_EQ(irqs[INTC_TIM0], 1);
    advance(1);
    CHECK_EQ(rd(COUNT(0)), 0);                       /* ... and the next tick returns to 0 */
    advance(150);
    CHECK_EQ(rd(COUNT(0)), 50);                      /* period COMP+1 = 100 */
    CHECK_EQ(rd(MODE(0)) & (EQUF | OVFF), EQUF);     /* never overflows */
    wr(MODE(0), CUE | ZRET | CMPE | OVFE | EQUF);
    advance(48);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM0], 1);
    advance(1);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM0], 2);
    advance(1000000);
    CHECK_EQ(rd(COUNT(0)), (99 + 1000000) % 100);
    CHECK_EQ(rd(MODE(0)) & OVFF, 0);

    /* COUNT above COMP: runs up to 0xFFFF, wraps (overflow), then cycles 0..COMP. */
    reset(BUS);
    wr(COUNT(1), 0xFFF0);
    wr(COMP(1), 10);
    wr(MODE(1), CUE | ZRET | CMPE | OVFE | CLK_BUS);
    advance(16);
    CHECK_EQ(rd(COUNT(1)), 0);
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), OVFF);
    advance(10);
    CHECK_EQ(rd(COUNT(1)), 10);
    CHECK_EQ(rd(MODE(1)) & (EQUF | OVFF), EQUF | OVFF);
    CHECK_EQ(irqs[INTC_TIM1], 2);
    advance(1);
    CHECK_EQ(rd(COUNT(1)), 0);

    /* COMP = 0: every tick is a compare event; the count stays 0. */
    reset(BUS);
    wr(MODE(2), CUE | ZRET | CMPE | CLK_BUS);
    advance(1);
    CHECK_EQ(rd(COUNT(2)), 0);
    CHECK_EQ(irqs[INTC_TIM2], 1);
    wr(MODE(2), CUE | ZRET | CMPE | EQUF);
    advance(1);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM2], 2);

    /* COMP = 0xFFFF behaves like a plain 16-bit wrap: compare at 0xFFFF, overflow on the reload. */
    reset(BUS);
    wr(COMP(3), 0xFFFF);
    wr(MODE(3), CUE | ZRET | CMPE | OVFE | CLK_BUS);
    advance(0xFFFF);
    CHECK_EQ(rd(MODE(3)) & (EQUF | OVFF), EQUF);
    advance(1);
    CHECK_EQ(rd(MODE(3)) & (EQUF | OVFF), EQUF | OVFF);
    CHECK_EQ(rd(COUNT(3)), 0);

    /* ZRET with the HBLNK clock: one event per COMP+1 lines. */
    reset(BUS);
    wr(COMP(0), 261);
    wr(MODE(0), CUE | ZRET | CMPE | CLK_HBLNK);
    advance(BUS);                                    /* 15734 lines */
    CHECK_EQ(rd(COUNT(0)), 15734 % 262);
}

/* ---- the game's T2 setup (libkernel timer library) ----------------------------------------------- */

static uint64_t lib_overflows;

/* INTC 11 handler of the library with no alarms queued (0x239F28 -> 0x23A0D8 -> 0x239CD8). */
static void lib_handler(void) {
    rd(COUNT(2));
    uint32_t m = rd(MODE(2));
    if (m & OVFF) { lib_overflows++; wr(MODE(2), m & ~EQUF); }   /* ack OVFF only */
    rd(COUNT(2));
    uint32_t m2 = rd(MODE(2));
    wr(COMP(2), 0);                                              /* next compare: the next wrap */
    wr(MODE(2), m2 & ~OVFF);                                     /* ack EQUF only */
}

/* System time in bus cycles (0x23A1D0). */
static uint64_t lib_time(void) {
    uint64_t c = rd(COUNT(2));
    uint32_t m = rd(MODE(2));
    uint64_t o = lib_overflows;
    if (m & OVFF) { c = rd(COUNT(2)); o++; }                     /* wrapped, interrupt not handled yet */
    return ((o << 16) | c) << 8;
}

static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

static void test_game_boot_sequence(void) {
    reset(BUS);
    lib_overflows = 0;
    CHECK_EQ(rd(MODE(2)), 0);
    uint32_t m = (rd(MODE(2)) & ~3u) | 2 | 0x300;               /* as at 0x239BB0 */
    if (!(m & CUE)) { wr(COUNT(2), 0); m |= 0xC80; }
    wr(COMP(2), 0xFFFF);
    wr(MODE(2), m);
    CHECK_EQ(m, 0xF82);
    CHECK_EQ(rd(MODE(2)), 0x382);                                /* flags written as 1 = cleared */
    advance(256 * 1000);
    CHECK_EQ(rd(COUNT(2)), 1000);
    CHECK_EQ(lib_time(), 256 * 1000);

    /* First compare at 0xFFFF, overflow one tick later. */
    advance(256ull * (0xFFFF - 1000));
    timer_service();
    CHECK_EQ(irqs[INTC_TIM2], 1);
    CHECK_EQ(rd(MODE(2)) & (EQUF | OVFF), EQUF);
    lib_handler();
    CHECK_EQ(rd(MODE(2)) & (EQUF | OVFF), 0);
    advance(256);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM2], 2);                                /* wrap to 0 = new COMP, and overflow */
    CHECK_EQ(rd(MODE(2)) & (EQUF | OVFF), EQUF | OVFF);
    CHECK_EQ(lib_time(), 256ull * 0x10000);                      /* correct before the handler runs */
    lib_handler();
    CHECK_EQ(lib_overflows, 1);
    CHECK_EQ(lib_time(), 256ull * 0x10000);

    /* Random service intervals and handler latency (up to ~30 ms): the 64-bit time stays exact. */
    uint64_t elapsed = 256ull * 0x10000, prev = 0;
    int pending = 0, bad = 0, last_irqs = irqs[INTC_TIM2];
    for (int i = 0; i < 200000; i++) {
        uint64_t step = rnd(4) == 0 ? rnd(400000) : rnd(150000);   /* up to ~2.7 ms */
        advance(step);
        elapsed += step;
        timer_service();
        if (irqs[INTC_TIM2] != last_irqs) { last_irqs = irqs[INTC_TIM2]; pending = 1 + (int)rnd(10); }
        if (pending && --pending == 0) lib_handler();
        uint64_t t = lib_time();
        if (t != (elapsed & ~255ull) || t < prev) {
            if (bad++ < 5) printf("  lib_time %llu, expected %llu (iteration %d)\n",
                                  (unsigned long long)t, (unsigned long long)(elapsed & ~255ull), i);
        }
        prev = t;
    }
    CHECK_EQ(bad, 0);
    if (pending) lib_handler();
    CHECK_EQ(lib_overflows, elapsed >> 24);                      /* one per 65536 * 256 bus cycles */
    CHECK_EQ(irqs[INTC_TIM2] >= (int)(elapsed >> 24), 1);
}

/* ---- gates ------------------------------------------------------------------------------------- */

#define LINE_BUS (147456000.0 / 15734.264)          /* bus cycles per line */
#define FIELD_BUS (262.5 * LINE_BUS)

static void test_gate_vblank_count_while_low(void) {
    reset(BUS);
    wr(MODE(0), CUE | CLK_256 | GATE | GATS_V | GATM(0));
    advance((uint64_t)(20 * LINE_BUS));                         /* inside the first VBLANK */
    CHECK_EQ(rd(COUNT(0)), 0);
    advance((uint64_t)(FIELD_BUS - 20 * LINE_BUS));             /* one whole field */
    CHECK_NEAR(rd(COUNT(0)), 240.5 * LINE_BUS / 256, 2);
    advance((uint64_t)(10 * LINE_BUS));                          /* next VBLANK: holds */
    uint32_t c = rd(COUNT(0));
    advance((uint64_t)(10 * LINE_BUS));
    CHECK_EQ(rd(COUNT(0)), c);
    CHECK_EQ(unhandled, 0);
}

static void test_gate_hblank_count_while_low(void) {
    reset(BUS);
    wr(MODE(1), CUE | CLK_BUS | GATE | GATM(0));                 /* HBLANK gate */
    advance(BUS / 100);                                          /* 10 ms = 157.34 lines */
    double lines = BUS / 100 / LINE_BUS, ph = lines - (int)lines;
    double low = ((int)lines * 0.83 + (ph > 0.17 ? ph - 0.17 : 0)) * LINE_BUS;
    CHECK_NEAR(rd(COUNT(1)), (uint64_t)low & 0xFFFF, 2);
    /* An HBLNK gate on the HBLNK clock is ignored. */
    reset(BUS);
    wr(MODE(1), CUE | CLK_HBLNK | GATE | GATM(0));
    advance(BUS);
    CHECK_EQ(rd(COUNT(1)), 15734);
}

static void test_gate_reset_modes(void) {
    /* GATM 1: reset at the rising edge of VBLANK (field start). */
    reset(BUS);
    wr(MODE(2), CUE | CLK_256 | GATE | GATS_V | GATM(1));
    advance((uint64_t)(FIELD_BUS / 2));
    CHECK_NEAR(rd(COUNT(2)), FIELD_BUS / 2 / 256, 1);
    advance((uint64_t)(FIELD_BUS / 2 + 1000 * 256.0));
    CHECK_NEAR(rd(COUNT(2)), 1000, 1);

    /* GATM 2: reset at the falling edge (end of VBLANK, 22 lines into the field). */
    reset(BUS);
    wr(MODE(2), CUE | CLK_256 | GATE | GATS_V | GATM(2));
    advance((uint64_t)(21 * LINE_BUS));
    CHECK_NEAR(rd(COUNT(2)), 21 * LINE_BUS / 256, 1);
    advance((uint64_t)(2 * LINE_BUS));
    CHECK_NEAR(rd(COUNT(2)), LINE_BUS / 256, 1);

    /* GATM 3: reset at both edges. */
    reset(BUS);
    wr(MODE(2), CUE | CLK_256 | GATE | GATS_V | GATM(3));
    advance((uint64_t)(23 * LINE_BUS));
    CHECK_NEAR(rd(COUNT(2)), LINE_BUS / 256, 1);
    advance((uint64_t)(FIELD_BUS - 23 * LINE_BUS + 5 * LINE_BUS));
    CHECK_NEAR(rd(COUNT(2)), 5 * LINE_BUS / 256, 1);

    /* Compare events with resets: a field is ~9610 ticks, so COMP = 5000 fires every field and
     * COMP = 20000 never does, however many fields pass between updates. */
    reset(BUS);
    wr(COMP(3), 20000);
    wr(MODE(3), CUE | CMPE | OVFE | CLK_256 | GATE | GATS_V | GATM(1));
    for (int i = 0; i < 5; i++) { advance((uint64_t)(FIELD_BUS * 7.3)); timer_service(); }
    advance((uint64_t)(FIELD_BUS * 100));
    timer_service();
    CHECK_EQ(irqs[INTC_TIM3], 0);
    CHECK_EQ(rd(MODE(3)) & (EQUF | OVFF), 0);
    wr(COMP(3), 5000);
    advance((uint64_t)(FIELD_BUS * 3));
    timer_service();
    CHECK_EQ(irqs[INTC_TIM3], 1);
    CHECK_EQ(rd(MODE(3)) & (EQUF | OVFF), EQUF);

    /* HBLANK rising-edge reset on the bus clock: COUNT never exceeds one line. */
    reset(BUS);
    wr(COMP(0), 9000);
    wr(MODE(0), CUE | CMPE | OVFE | CLK_BUS | GATE | GATM(1));
    int max = 0;
    for (int i = 0; i < 3000; i++) { advance(997); int c = (int)rd(COUNT(0)); if (c > max) max = c; }
    CHECK_NEAR(max, LINE_BUS, 1000);
    CHECK_EQ(rd(MODE(0)) & (EQUF | OVFF), EQUF);                 /* 9000 < 9371.66 */
}

/* ---- register access ------------------------------------------------------------------------------ */

static void test_access_widths(void) {
    reset(BUS);
    wr(COMP(0), 0xABCD);
    uint16_t h;
    timer_access(COMP(0), &h, 2, 0);
    CHECK_EQ(h, 0xABCD);
    uint8_t b;
    timer_access(COMP(0) + 1, &b, 1, 0);
    CHECK_EQ(b, 0xAB);
    uint64_t d = ~0ull;
    timer_access(COMP(0), &d, 8, 0);
    CHECK_EQ(d, 0xABCD);
    uint8_t q[16];
    memset(q, 0xEE, sizeof q);
    q[0] = 0x34; q[1] = 0x12;
    timer_access(COMP(0), q, 16, 1);                             /* 128-bit store: low word only */
    CHECK_EQ(rd(COMP(0)), 0x1234);
    timer_access(COMP(0), q, 16, 0);
    CHECK_EQ(q[0] | q[1] << 8 | q[2] << 16 | q[3] << 24, 0x1234);
    CHECK_EQ(q[4] | q[8] | q[15], 0);
    h = 0x5678;
    timer_access(COUNT(0), &h, 2, 1);
    CHECK_EQ(rd(COUNT(0)), 0x5678);

    /* Narrow MODE writes don't echo the flags of the bytes not written. */
    wr(COMP(0), 0);
    wr(COUNT(0), 0xFFFF);
    wr(MODE(0), CUE | CMPE | OVFE | CLK_BUS);
    advance(1);
    CHECK_EQ(rd(MODE(0)), CUE | CMPE | OVFE | EQUF | OVFF);
    b = CUE | CLK_16;
    timer_access(MODE(0), &b, 1, 1);                             /* low byte: flags untouched */
    CHECK_EQ(rd(MODE(0)), CUE | CMPE | OVFE | EQUF | OVFF | CLK_16);
    b = (CMPE | OVFE | EQUF) >> 8;
    timer_access(MODE(0) + 1, &b, 1, 1);                         /* high byte: clears EQUF only */
    CHECK_EQ(rd(MODE(0)), CUE | CMPE | OVFE | OVFF | CLK_16);
    timer_access(MODE(0) + 1, &b, 1, 0);
    CHECK_EQ(b, (CMPE | OVFE | OVFF) >> 8);
    CHECK_EQ(unhandled, 0);
}

static void test_hold_and_unmapped(void) {
    reset(BUS);
    wr(HOLD(0), 0x1111);
    wr(HOLD(1), 0x12222);
    CHECK_EQ(rd(HOLD(0)), 0x1111);
    CHECK_EQ(rd(HOLD(1)), 0x2222);
    CHECK_EQ(unhandled, 0);
    CHECK_EQ(rd(HOLD(2)), 0);                                    /* T2/T3 have no HOLD */
    CHECK_EQ(rd(HOLD(3)), 0);
    CHECK_EQ(unhandled, 2);
    CHECK_EQ(rd(COUNT(0) + 4), 0);
    CHECK_EQ(rd(COUNT(1) + 0x40), 0);
    wr(COUNT(3) + 0x7F0, 5);
    CHECK_EQ(unhandled, 5);
    CHECK_EQ(rd(MODE(3)), 0);
}

static void test_interrupt_causes(void) {
    reset(BUS);
    for (int n = 0; n < 4; n++) { wr(COMP(n), 10 * (n + 1)); wr(MODE(n), CUE | CMPE | CLK_BUS); }
    advance(25);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM0], 1);
    CHECK_EQ(irqs[INTC_TIM1], 1);
    CHECK_EQ(irqs[INTC_TIM2], 0);
    CHECK_EQ(irqs[INTC_TIM3], 0);
    advance(20);
    timer_service();
    CHECK_EQ(irqs[INTC_TIM2], 1);
    CHECK_EQ(irqs[INTC_TIM3], 1);
    CHECK_EQ(irqs[INTC_GS] + irqs[INTC_VBLANK_S] + irqs[INTC_SBUS], 0);
}

static void test_time_going_backwards(void) {
    reset(BUS);
    wr(MODE(0), CUE | CLK_BUS);
    advance(1000);
    CHECK_EQ(rd(COUNT(0)), 1000);
    fake_now -= 500;                                             /* must not count backwards or explode */
    CHECK_EQ(rd(COUNT(0)), 1000);
    fake_now += 600;
    CHECK_EQ(rd(COUNT(0)), 1100);
}

int main(void) {
    struct { const char* name; void (*fn)(void); } tests[] = {
        { "rates", test_rates },
        { "host_clock_conversion", test_host_clock_conversion },
        { "prescaler_grid", test_prescaler_grid },
        { "cue_and_count_write", test_cue_and_count_write },
        { "compare", test_compare },
        { "compare_needs_enable", test_compare_needs_enable },
        { "overflow", test_overflow },
        { "events_collapse", test_events_collapse },
        { "edge_per_flag", test_edge_per_flag },
        { "flag_clearing", test_flag_clearing },
        { "events_before_comp_write", test_events_before_comp_write },
        { "zret", test_zret },
        { "game_boot_sequence", test_game_boot_sequence },
        { "gate_vblank_count_while_low", test_gate_vblank_count_while_low },
        { "gate_hblank_count_while_low", test_gate_hblank_count_while_low },
        { "gate_reset_modes", test_gate_reset_modes },
        { "access_widths", test_access_widths },
        { "hold_and_unmapped", test_hold_and_unmapped },
        { "interrupt_causes", test_interrupt_causes },
        { "time_going_backwards", test_time_going_backwards },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = fails;
        tests[i].fn();
        printf("%-30s %s\n", tests[i].name, fails == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
