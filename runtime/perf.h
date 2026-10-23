/* Performance instrumentation (perf.c): where the host's time goes, and a sampling profiler.
 *
 *   PS2_PERF=1     once per second: VBlanks serviced vs due, game frame flips, and the share of
 *                  time spent in EE code, GS drawing, VU microprograms, VIF, IPU, IOP services,
 *                  presentation and idle (waiting for the next interrupt).
 *   PS2_PROFILE=1  a host thread samples the emulation thread's instruction pointer (~2 kHz) and
 *                  prints the hottest host functions at exit (recompiled game functions show up
 *                  as f_XXXXXXXX), resolved through the executable's own COFF symbol table.
 *
 * Time categories are exclusive: perf_enter() charges the time so far to the current category
 * and switches; perf_leave() switches back. Scopes nest (VIF -> VU -> GS for an XGKICK). With
 * PS2_PERF unset both are a single predictable branch.
 *
 * The state is defined in this header (selectany), so subsystem files can use the hooks without
 * linking perf.c (their unit tests link only the file under test). */
#pragma once
#include <stdint.h>
#include <x86intrin.h>

/* GS: with the GS thread running (gs_thread.c) this is only the EE side (GIF parsing, queueing);
 * GSWAIT: the EE thread waiting for the GS thread (downloads, one frame of lag at most). */
enum { PERF_EE, PERF_GS, PERF_VU, PERF_VIF, PERF_IPU, PERF_IOP, PERF_PRESENT, PERF_GSWAIT, PERF_IDLE, PERF_N };
/* (pc-fps2) the graphics worker's categories: PERF_IDLE = waiting for a job, PERF_EE = DMA chain walking */

typedef struct {
    int      on;                   /* PS2_PERF set */
    int      cat;                  /* category being charged now (EE thread) */
    uint64_t t0;                   /* TSC when `cat` was entered */
    uint64_t acc[PERF_N];          /* TSC ticks charged per category */
    uint32_t flips;                /* displayed-buffer changes (DISPFB writes that change the value) */
    volatile uint64_t gs_busy;     /* TSC ticks the GS thread spent executing (written by it only) */
    /* (pc-fps2) the graphics worker (gfx_async.h) keeps its own split: OS thread id, category, TSC, ticks */
    volatile uint32_t worker_tid;
    int      wcat;
    uint64_t wt0;
    uint64_t wacc[PERF_N];
} perf_state_t;

__attribute__((selectany)) perf_state_t perf_state = { 0 };

static inline int perf_on_worker(void) {
    if (!perf_state.worker_tid) return 0;
    uint32_t v;
    __asm__ volatile("movl %%gs:0x48, %0" : "=r"(v));   /* TEB ClientId.UniqueThread */
    return v == perf_state.worker_tid;
}

static inline int perf_enter(int cat) {
    if (!perf_state.on) return -1;
    uint64_t t = __rdtsc();
    if (perf_on_worker()) {
        perf_state.wacc[perf_state.wcat] += t - perf_state.wt0;
        perf_state.wt0 = t;
        int prev = perf_state.wcat;
        perf_state.wcat = cat;
        return prev;
    }
    perf_state.acc[perf_state.cat] += t - perf_state.t0;
    perf_state.t0 = t;
    int prev = perf_state.cat;
    perf_state.cat = cat;
    return prev;
}

static inline void perf_leave(int prev) {
    if (prev < 0) return;
    uint64_t t = __rdtsc();
    if (perf_on_worker()) {
        perf_state.wacc[perf_state.wcat] += t - perf_state.wt0;
        perf_state.wt0 = t;
        perf_state.wcat = prev;
        return;
    }
    perf_state.acc[perf_state.cat] += t - perf_state.t0;
    perf_state.t0 = t;
    perf_state.cat = prev;
}

/* perf.c (main executable only) */
void perf_init(void);                          /* reads PS2_PERF / PS2_PROFILE; starts the sampler */
void perf_vblank(uint32_t serviced, uint32_t due);   /* once per serviced VBlank: totals so far */
void perf_final(void);                         /* prints the profile (once); call before exiting */
void perf_burn(int gs_thread);                 /* PS2_PERF_BURN / PS2_PERF_BURN_GS headroom test */
/* set by perf_init when a burn is requested, so files linked without perf.c can call it */
__attribute__((selectany)) void (*perf_burn_hook)(int gs_thread) = 0;
