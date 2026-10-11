/* EE timers T0-T3 (0x10000000-0x10001FFF), implemented from ps2tek and the EE User's Manual.
 *
 * Registers (timer n at 0x10000000 + n * 0x800; 32 bits wide, unused upper bits read 0):
 *   +0x00 Tn_COUNT  16-bit up-counter
 *   +0x10 Tn_MODE   CLKS (0-1: bus 147.456 MHz, bus/16, bus/256, HBLNK), GATE (2), GATS (3: 0 = HBLNK,
 *                   1 = VBLNK), GATM (4-5), ZRET (6), CUE (7: count enable), CMPE (8), OVFE (9),
 *                   EQUF (10), OVFF (11): the flags are cleared by writing 1
 *   +0x20 Tn_COMP   16-bit compare value
 *   +0x30 Tn_HOLD   T0/T1 only
 *
 * Model
 *  - Time base: host time (QueryPerformanceCounter) converted to EE bus cycles (147.456 MHz)
 *    since the first timer access. Nothing ticks per cycle: each timer remembers the bus cycle up
 *    to which its COUNT and flags are current and catches up in O(1) on every access to it and in
 *    timer_service(). The /16 and /256 prescalers tick on a fixed grid of bus cycles and HBLNK is
 *    derived exactly from bus time (NTSC, 15734.264 Hz), so the rates never drift.
 *  - Accesses catch up first, so register changes take effect at the moment of the write and
 *    events before it are judged with the old settings. Writing COUNT sets the counter from that
 *    moment on. Writing MODE does not reset COUNT.
 *  - Events: a compare event when a tick makes COUNT equal to COMP, an overflow event when COUNT
 *    wraps from 0xFFFF to 0. An event sets its flag only when it is enabled (CMPE/OVFE). The
 *    interrupt is edge-triggered (ps2tek): INTC_TIM0+n is raised only when a flag goes from 0 to
 *    1. Any number of events between two updates therefore collapse into one interrupt, and no
 *    interrupt is raised while the game leaves the flag set.
 *  - ZRET: the counter runs 0..COMP and the tick after it reaches COMP loads 0, so the period is
 *    COMP+1 ticks. With COMP = 0xFFFF that reload also counts as an overflow. If COUNT is above
 *    COMP, the counter first runs up to 0xFFFF and wraps (overflow).
 *  - Gate (GATE = 1), an approximation: the blanking signals come from a synthetic NTSC CRTC on
 *    the same time base. A field is 262.5 lines, HBLANK is high for the first 17% of each line and
 *    VBLANK is high for the first 22 lines of each field. These signals are not phase-locked to
 *    the runtime's VBlank interrupt. GATM 0 counts only while the gate signal is low. GATM 1/2/3
 *    reset COUNT to 0 on the rising, falling or both edges and count the rest of the time. When
 *    many edges pass between two updates, the whole segments in between are examined until every
 *    armed event has been seen (at most 256 of them, enough to find the longest segment of each
 *    kind; see count_with_resets), so the events don't depend on how often the timer is updated.
 *    An HBLNK gate on an HBLNK clock is ignored, as on hardware.
 *  - Tn_HOLD (T0/T1) is plain storage. On hardware an SBUS interrupt latches COUNT into it; that
 *    interrupt isn't visible to this file.
 *
 * Sly 2 (libkernel's timer library) runs T2 at bus/256 with CMPE+OVFE (COUNT = 0, COMP = 0xFFFF,
 * MODE = 0xF82) and an INTC 11 handler that extends COUNT to a 64-bit system time. Its time query
 * reads MODE.OVFF right after COUNT to catch a wrap whose interrupt hasn't run yet, so MODE
 * reads must be current, which catching up on every access guarantees.
 */
#include "hw.h"

#include <windows.h>

enum {
    MODE_CLKS = 3u << 0, MODE_GATE = 1u << 2, MODE_GATS = 1u << 3, MODE_GATM = 3u << 4, MODE_ZRET = 1u << 6,
    MODE_CUE = 1u << 7, MODE_CMPE = 1u << 8, MODE_OVFE = 1u << 9, MODE_EQUF = 1u << 10, MODE_OVFF = 1u << 11,
    MODE_SETTINGS = 0x3FFu, MODE_FLAGS = MODE_EQUF | MODE_OVFF,
};

#define BUS_HZ 147456000ull
/* HBLANK pulses per bus cycle: 15734.264 / 147456000 = 1966783 / 18432000000 */
#define H_NUM 1966783ull
#define H_DEN 18432000000ull

typedef unsigned __int128 u128;

typedef struct {
    uint32_t mode, comp, hold, count;
    uint64_t last;                           /* bus cycle up to which count and flags are current */
} ee_timer;

static ee_timer timers[4];

/* ---- host time -> bus cycles ----------------------------------------------------------------- */

static uint64_t qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }

static uint64_t (*host_now)(void);
static uint64_t host_freq, host_epoch;
static uint64_t hold_at;                     /* timer_hold: host time at which EE time stopped */
static int ready, held;
static unsigned gate_logged;

static void timers_init(void) {
    if (!host_now) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        host_now = qpc_now;
        host_freq = (uint64_t)f.QuadPart;
    }
    host_epoch = host_now();
    held = 0;
    memset(timers, 0, sizeof timers);
    ready = 1;
}

/* Unit-test hook (not part of hw.h): use `now` (ticking at `freq` Hz) as the host clock, or
 * QueryPerformanceCounter if `now` is NULL, and reset all timers. */
void timer_set_host_clock(uint64_t (*now)(void), uint64_t freq) {
    host_now = now;
    host_freq = now ? freq : 0;
    timers_init();
}

static uint64_t bus_now(void) {
    uint64_t h = held ? hold_at : host_now(), dt = h > host_epoch ? h - host_epoch : 0;
    return dt / host_freq * BUS_HZ + dt % host_freq * BUS_HZ / host_freq;   /* exact, no 64-bit overflow */
}

/* Debug pause (the live input token 'pause', window.c): EE time stands still while the EE waits for
 * the next input line, and continues from there afterwards. Sly 2 takes its frame time from T2
 * (the libkernel system time), so without this every resumed step would see the whole wait pass as
 * game time (the dance minigame's beats ran on while it was paused). EE thread only. */
void timer_hold(int on) {
    if (!ready) timers_init();
    if (on && !held) { hold_at = host_now(); held = 1; }
    else if (!on && held) { uint64_t h = host_now(); host_epoch += h > hold_at ? h - hold_at : 0; held = 0; }
}

static uint64_t clock_ticks(uint32_t mode, uint64_t bus) {
    switch (mode & MODE_CLKS) {
    case 0:  return bus;
    case 1:  return bus >> 4;
    case 2:  return bus >> 8;
    default: return bus / H_DEN * H_NUM + bus % H_DEN * H_NUM / H_DEN;     /* HBLANK pulses so far */
    }
}

/* ---- gate signals: a periodic signal, high (blanking) at the start of each period --------------
 * At bus cycle t the signal is in period floor(t*num/den) at phase (t*num) mod den, and it is high
 * while the phase is below `blank`. Period k >= 1 starts with a rising edge. */

typedef struct { uint64_t num, den, blank; } gate_sig;

static const gate_sig hsig = { H_NUM, H_DEN, H_DEN * 17 / 100 };            /* one line, 17% blank */
static const gate_sig vsig = { 2 * H_NUM, 525 * H_DEN, 44 * H_DEN };        /* 262.5 lines, 22 blank */

static uint64_t sig_rises(const gate_sig* g, uint64_t t) {                  /* rising edges in (0, t] */
    return (uint64_t)((u128)t * g->num / g->den);
}
static uint64_t sig_falls(const gate_sig* g, uint64_t t) {                  /* falling edges in (0, t] */
    u128 s = (u128)t * g->num;
    return s < g->blank ? 0 : (uint64_t)((s - g->blank) / g->den) + 1;
}
static uint64_t sig_rise_time(const gate_sig* g, uint64_t k) {              /* bus cycle of rising edge k >= 1 */
    return (uint64_t)(((u128)k * g->den + g->num - 1) / g->num);
}
static uint64_t sig_fall_time(const gate_sig* g, uint64_t k) {              /* bus cycle of falling edge k >= 1 */
    return (uint64_t)(((u128)(k - 1) * g->den + g->blank + g->num - 1) / g->num);
}
static uint64_t sig_low_time(const gate_sig* g, uint64_t t) {               /* bus cycles in [0, t) spent low */
    u128 s = (u128)t * g->num, k = s / g->den, ph = s % g->den;
    return (uint64_t)((k * (g->den - g->blank) + (ph > g->blank ? ph - g->blank : 0)) / g->num);
}

/* Reset edges for GATM 1 (rising), 2 (falling), 3 (both: falling edges come first, then they alternate) */
static uint64_t gate_edges(uint32_t gatm, const gate_sig* g, uint64_t t) {
    return (gatm & 1 ? sig_rises(g, t) : 0) + (gatm & 2 ? sig_falls(g, t) : 0);
}
static uint64_t gate_edge_time(uint32_t gatm, const gate_sig* g, uint64_t k) {
    if (gatm == 1) return sig_rise_time(g, k);
    if (gatm == 2) return sig_fall_time(g, k);
    return (k & 1) ? sig_fall_time(g, (k + 1) / 2) : sig_rise_time(g, k / 2);
}

static const gate_sig* timer_gate(uint32_t mode) {
    if (!(mode & MODE_GATE)) return NULL;
    if (mode & MODE_GATS) return &vsig;
    return (mode & MODE_CLKS) == 3 ? NULL : &hsig;                          /* HBLNK gate + HBLNK clock: off */
}

/* ---- counting ---------------------------------------------------------------------------------- */

/* Advance COUNT by d ticks; returns the events (MODE_EQUF / MODE_OVFF) that happened on the way. */
static uint32_t count_ticks(ee_timer* t, uint64_t d) {
    if (!d) return 0;
    uint64_t c = t->count, comp = t->comp;
    uint32_t ev = 0;
    if (!(t->mode & MODE_ZRET)) {
        uint64_t to_comp = (comp - c) & 0xFFFF;                             /* 0: a full lap away */
        if (d >= (to_comp ? to_comp : 0x10000)) ev |= MODE_EQUF;
        if (c + d > 0xFFFF) ev |= MODE_OVFF;
        t->count = (uint32_t)((c + d) & 0xFFFF);
        return ev;
    }
    if (c > comp) {                                                         /* above COMP: wrap first */
        if (d < 0x10000 - c) { t->count = (uint32_t)(c + d); return 0; }
        d -= 0x10000 - c;
        c = 0;
        ev |= MODE_OVFF | (comp == 0 ? MODE_EQUF : 0);
    }
    uint64_t period = comp + 1;                                             /* counts 0..COMP */
    if (d >= (c < comp ? comp - c : period)) ev |= MODE_EQUF;
    if (period == 0x10000 && c + d > 0xFFFF) ev |= MODE_OVFF;
    t->count = (uint32_t)((c + d) % period);
    return ev;
}

/* GATM 1-3: COUNT is reset to 0 at every gate edge in (then, now]. A whole segment between two
 * edges counts up from 0, so its events depend only on its length, and a longer segment raises
 * every event a shorter one does. Segments of one kind differ in length by a tick (a line is
 * 9371.66 bus cycles) and the longer ones can be rare, so the whole segments are examined one by
 * one until every armed event (enabled, flag clear) has been seen, up to SCAN_SEGMENTS of them.
 * That is more than twice the longest run of short segments of one kind for any gate and clock
 * (106 fields: the VBLANK high part on bus/16), so the window always contains a longest segment of
 * each kind and a long gap raises the same events as frequent updates. */
#define SCAN_SEGMENTS 256
static uint32_t count_with_resets(ee_timer* t, const gate_sig* g, uint64_t then, uint64_t now, uint32_t armed) {
    uint32_t m = t->mode, gatm = (m & MODE_GATM) >> 4;
    uint64_t e0 = gate_edges(gatm, g, then), e1 = gate_edges(gatm, g, now);
    if (e0 == e1) return count_ticks(t, clock_ticks(m, now) - clock_ticks(m, then));
    uint64_t at = clock_ticks(m, gate_edge_time(gatm, g, e0 + 1));
    uint32_t ev = count_ticks(t, at - clock_ticks(m, then));
    for (uint64_t k = e0 + 1; k < e1 && k <= e0 + SCAN_SEGMENTS && (armed & ~ev); k++) {
        uint64_t next = clock_ticks(m, gate_edge_time(gatm, g, k + 1));
        t->count = 0;
        ev |= count_ticks(t, next - at);
        at = next;
    }
    t->count = 0;
    return ev | count_ticks(t, clock_ticks(m, now) - clock_ticks(m, gate_edge_time(gatm, g, e1)));
}

/* Bring timer n up to bus cycle `now`: count, set enabled flags, raise the interrupt on a 0->1 flag. */
static void timer_update(int n, uint64_t now) {
    ee_timer* t = &timers[n];
    if (now <= t->last) return;
    uint64_t then = t->last;
    t->last = now;
    uint32_t m = t->mode;
    if (!(m & MODE_CUE)) return;

    uint32_t armed = (((m >> 8) & 3u) << 10) & ~m;                          /* CMPE -> EQUF, OVFE -> OVFF, still clear */
    const gate_sig* g = timer_gate(m);
    uint32_t ev;
    if (!g)
        ev = count_ticks(t, clock_ticks(m, now) - clock_ticks(m, then));
    else if (!(m & MODE_GATM))                                              /* count while the gate is low */
        ev = count_ticks(t, clock_ticks(m, sig_low_time(g, now)) - clock_ticks(m, sig_low_time(g, then)));
    else
        ev = count_with_resets(t, g, then, now, armed);

    uint32_t rising = ev & armed;
    if (rising) {
        t->mode = m | rising;
        intc_raise(INTC_TIM0 + n);
    }
}

/* ---- registers ----------------------------------------------------------------------------------- */

/* The timer is already up to date (timer_access). */
static uint32_t reg_read(int n, unsigned reg) {
    ee_timer* t = &timers[n];
    switch (reg) {
    case 0:  return t->count;
    case 1:  return t->mode;
    case 2:  return t->comp;
    default: return t->hold;
    }
}

static void reg_write(int n, unsigned reg, uint32_t v) {
    ee_timer* t = &timers[n];
    switch (reg) {
    case 0: t->count = v & 0xFFFF; break;
    case 1:
        if ((v & MODE_GATE) && !(gate_logged & (1u << n))) {
            gate_logged |= 1u << n;
            fprintf(stderr, "[timer] T%d gated (MODE=0x%X): approximated with a synthetic NTSC video timing\n", n, v);
        }
        t->mode = (v & MODE_SETTINGS) | (t->mode & MODE_FLAGS & ~v);       /* flags: write 1 to clear */
        break;
    case 2: t->comp = v & 0xFFFF; break;
    default: t->hold = v & 0xFFFF; break;
    }
}

void timer_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!ready) timers_init();
    int n = (int)((phys >> 11) & 3);
    unsigned reg = (phys & 0x7FF) >> 4, byte = phys & 15;
    if (reg > 3 || (reg == 3 && n >= 2) || byte >= 4) {                     /* no register there */
        if (!is_write) memset(data, 0, size);
        hw_log_unhandled("timer", phys, data, size, is_write);
        return;
    }
    unsigned span = size < 4 - byte ? size : 4 - byte;                     /* register bytes covered */
    /* Bring the timer up to date once, so the whole access (a narrow write reads, merges and writes)
     * happens at one instant and events before a write are judged with the old settings. */
    if (reg <= 1 || (reg == 2 && is_write)) timer_update(n, bus_now());
    if (!is_write) {
        uint32_t v = reg_read(n, reg) >> (8 * byte);
        memset(data, 0, size);
        memcpy(data, &v, span);
        return;
    }
    uint32_t v = 0;
    if (span < 4)                                                           /* narrow write: merge; never echo flags */
        v = reg == 1 ? timers[n].mode & MODE_SETTINGS : reg_read(n, reg);
    memcpy((uint8_t*)&v + byte, data, span);
    reg_write(n, reg, v);
}

/* Called about once per millisecond: catch up the running timers that could still raise an interrupt. */
void timer_service(void) {
    if (!ready) timers_init();
    uint64_t now = 0;
    int have_now = 0;
    for (int n = 0; n < 4; n++) {
        uint32_t m = timers[n].mode;
        if (!(m & MODE_CUE) || !((((m >> 8) & 3u) << 10) & ~m)) continue;  /* stopped, or nothing armed */
        if (!have_now) { now = bus_now(); have_now = 1; }
        timer_update(n, now);
    }
}
