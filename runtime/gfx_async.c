/* (pc-fps2) The frame's VIF1 DMA chain on a worker thread: see gfx_async.h for the model.
 *
 *   PS2_GFX_ASYNC=0   the chain runs on the EE thread inside the CHCR write, as before
 *   PS2_GFX_ASYNC_LOG=1  a line per second: jobs, EE waits for the worker (count, ms) and what made it wait
 *
 * Not with PS2_VU_RECORD / PS2_VU_VERIFY (their state is the EE's). */
#include "hw.h"
#include "perf.h"
#include "gfx_async.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <windows.h>

void gs_publish(void);                           /* gs_thread.c */
void gs_exec_write_reg(uint32_t r, uint64_t v);  /* gs_draw.c: SIGNAL / FINISH / LABEL on the EE's timeline */

#define MAX_EVENTS 256
typedef struct { int kind; uint32_t a; uint64_t b; } event;

static HANDLE thread;
static int job_ch;                               /* the channel of the job (DMA_VIF1) */
static _Atomic uint32_t job_seq;                 /* EE: ++ to start a job */
static _Atomic uint32_t done_seq;                /* worker: = job_seq when it is done */
static event events[MAX_EVENTS];                 /* worker -> EE, in order */
static uint32_t nevents;
static int log_on;
static uint64_t st_jobs, st_waits, st_wait_ticks, st_events;
static LARGE_INTEGER qpc_freq;

static void defer(int kind, uint32_t a, uint64_t b) {   /* worker */
    if (nevents < MAX_EVENTS) events[nevents++] = (event){ kind, a, b };
    else {
        static int logged;
        if (!logged++) fprintf(stderr, "[gfx] more than %d deferred events in one job: dropped\n", MAX_EVENTS);
    }
}

static DWORD WINAPI worker_main(LPVOID arg) {
    (void)arg;
    gfx_worker_tid = gfx_tid();
    perf_state.wt0 = __rdtsc();
    perf_state.wcat = PERF_IDLE;
    perf_state.worker_tid = gfx_worker_tid;
    uint32_t seen = 0;
    for (;;) {
        uint32_t s;
        while ((s = atomic_load_explicit(&job_seq, memory_order_acquire)) == seen) {
            for (int i = 0; i < 2000 && (s = atomic_load_explicit(&job_seq, memory_order_acquire)) == seen; i++) _mm_pause();
            if (s != seen) break;
            WaitOnAddress((volatile VOID*)&job_seq, &seen, sizeof seen, INFINITE);
        }
        seen = s;
        int prev = perf_enter(PERF_EE);
        dma_run_owned(job_ch);
        gs_publish();
        perf_leave(prev);
        atomic_store_explicit(&done_seq, seen, memory_order_release);
        WakeByAddressAll((PVOID)&done_seq);
        InterlockedOr((volatile LONG*)&ps2_irq_pending, PS2_IRQ_HW);   /* the EE applies the results at its next safe point */
        WakeByAddressAll((PVOID)&ps2_irq_pending);
    }
    return 0;
}

static void complete(void) {                     /* EE: the job is done; apply what it deferred, in order */
    for (uint32_t i = 0; i < nevents; i++) {
        const event* e = &events[i];
        if (e->kind == GFX_EV_INTC) intc_raise((int)e->a);
        else if (e->kind == GFX_EV_DMAC) dmac_raise((int)e->a);
        else gs_exec_write_reg(e->a, e->b);
    }
    st_events += nevents;
    nevents = 0;
    gfx_owned = 0;
}

static int is_done(void) {
    return atomic_load_explicit(&done_seq, memory_order_acquire) == atomic_load_explicit(&job_seq, memory_order_relaxed);
}

void gfx_poll(void) {
    if (gfx_owned && is_done()) complete();
}

static void wait_job(void) {                     /* EE: gfx_sync() */
    if (!gfx_owned) return;
    if (!is_done()) {
        int prev = perf_enter(PERF_GSWAIT);
        uint64_t t0 = __rdtsc();
        for (int i = 0; i < 4000 && !is_done(); i++) _mm_pause();
        while (!is_done()) {
            uint32_t d = atomic_load_explicit(&done_seq, memory_order_acquire);
            if (d == atomic_load_explicit(&job_seq, memory_order_relaxed)) break;
            WaitOnAddress((volatile VOID*)&done_seq, &d, sizeof d, 1);
            if (!is_done()) { int p = perf_enter(PERF_IOP); ps2_poll_iop(); perf_leave(p); }   /* sound keeps going */
        }
        st_waits++;
        st_wait_ticks += __rdtsc() - t0;
        perf_leave(prev);
    }
    complete();
}

static int take(int ch) {                        /* EE, dma.c pump */
    if (gfx_owned) return 1;
    job_ch = ch;
    nevents = 0;
    gfx_owned = 1;
    st_jobs++;
    atomic_fetch_add_explicit(&job_seq, 1, memory_order_release);
    WakeByAddressSingle((PVOID)&job_seq);
    if (log_on) {
        static int64_t next;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (t.QuadPart >= next) {
            static uint64_t tsc0, j0, w0, wt0;
            uint64_t tsc = __rdtsc();
            if (next) fprintf(stderr, "[gfx] %llu jobs, EE waited %llu times, %.1f%% of the time\n",
                              (unsigned long long)(st_jobs - j0), (unsigned long long)(st_waits - w0),
                              tsc > tsc0 ? 100.0 * (double)(st_wait_ticks - wt0) / (double)(tsc - tsc0) : 0.0);
            tsc0 = tsc; j0 = st_jobs; w0 = st_waits; wt0 = st_wait_ticks;
            next = t.QuadPart + qpc_freq.QuadPart;
        }
    }
    return 1;
}

int gfx_async_init(void) {
    const char* e = getenv("PS2_GFX_ASYNC");
    if (e && *e == '0') return 0;
    if ((getenv("PS2_VU_RECORD") && *getenv("PS2_VU_RECORD")) || (getenv("PS2_VU_VERIFY") && *getenv("PS2_VU_VERIFY") != '0')) {
        fprintf(stderr, "[gfx] graphics worker off (PS2_VU_RECORD / PS2_VU_VERIFY)\n");
        return 0;
    }
    QueryPerformanceFrequency(&qpc_freq);
    log_on = getenv("PS2_GFX_ASYNC_LOG") != NULL;
    gfx_defer_fn = defer;
    gfx_wait_fn = wait_job;
    thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!thread) { fprintf(stderr, "[gfx] cannot start the graphics worker\n"); return 0; }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    while (!gfx_worker_tid) Sleep(0);
    gfx_take_fn = take;
    fprintf(stderr, "[gfx] graphics worker on: the VIF1 DMA chain runs beside the EE (PS2_GFX_ASYNC=0: on the EE)\n");
    return 1;
}

void* gfx_worker_os_handle(void) { return thread; }   /* PS2_PROFILE=worker (perf.c) */
