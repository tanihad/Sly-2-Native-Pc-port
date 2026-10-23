/* (pc-fps2) The frame's VIF1 DMA chain on a worker thread: see gfx_async.h for the model.
 *
 * Used above 60 fps ([display] frame_rate > 60; at 60 the chain runs on the EE: see take()).
 *   PS2_GFX_ASYNC=0   the chain always runs on the EE thread inside the CHCR write, as before; =1: the worker at 60 too
 *   PS2_GFX_ASYNC_LOG=1  a line per second: jobs, EE waits for the worker (count, ms) and what made it wait
 *
 * Not with PS2_VU_RECORD / PS2_VU_VERIFY (their state is the EE's). */
#include "hw.h"
#include "perf.h"
#include "vu_internal.h"
#include "gfx_async.h"
#include "frame_rate.h"

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
static int64_t job_t0, job_t1, st_job_ticks;     /* QPC: job handed out (EE) / done (worker); sum of job times */
static uint64_t job_pairs, job_progs, st_pairs, st_progs;   /* VU1 pairs / programs of the last job, sums */
static int job_quiet;                            /* (interp.c prototype) a re-run chain's job */
static uint64_t st_qjobs, st_qpairs;
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
        uint64_t p0 = vu_st[1].pairs, g0 = vu_st[1].programs;
        dma_run_owned(job_ch);
        job_pairs = vu_st[1].pairs - p0;
        job_progs = vu_st[1].programs - g0;
        gs_publish();
        perf_leave(prev);
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        job_t1 = t1.QuadPart;
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
    st_job_ticks += job_t1 - job_t0;
    if (job_quiet) { st_qjobs++; st_qpairs += job_pairs; }
    st_pairs += job_pairs;
    st_progs += job_progs;
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

/* PS2_GFX_ASYNC_CHECK=<n>: every n-th job, EE RAM and the scratchpad are copied when it starts, and every byte the
 * worker's DMA reads is compared with the copy: the worker must consume exactly what the synchronous model would have
 * (memory as it was at the kick), i.e. the game never writes what an in-flight chain still reads. */
static int check_every;
static uint8_t* snap_ram;
static uint8_t snap_spr[PS2_SPR_SIZE];
static volatile int check_job;
static uint64_t chk_jobs, chk_bytes, chk_bad_bytes, chk_bad_jobs;
static int chk_bad_now;
static uint32_t job_tadr;
static uint32_t race_addr[32], race_cnt[32], race_vb[32], race_len[32];
static int race_n;
uint32_t dma_debug_tadr(int ch);                 /* dma.c */

static void read_check(uint32_t addr, const uint8_t* p, uint32_t bytes) {   /* worker */
    if (!check_job) return;
    const uint8_t* s = (addr & 0x80000000u) ? snap_spr + (addr & 0x3FF0u) : snap_ram + (addr & 0x0FFFFFF0u);
    chk_bytes += bytes;
    if (memcmp(s, p, bytes)) {
        uint32_t d = 0;
        for (uint32_t i = 0; i < bytes; i++) d += s[i] != p[i];
        chk_bad_bytes += d;
        int k = 0;                                   /* where: per address, and the VBlank it last happened */
        while (k < race_n && race_addr[k] != addr) k++;
        if (k == race_n && race_n < 32) { race_addr[race_n++] = addr; race_cnt[k] = 0; }
        if (k < race_n) { race_cnt[k]++; race_vb[k] = ps2_vblank_count(); race_len[k] = bytes; }
        if (!chk_bad_now++ && chk_bad_jobs < 10) {
            fprintf(stderr, "[gfx] PS2_GFX_ASYNC_CHECK: the chain read %u bytes at 0x%08X that changed after the kick (%u differ; "
                            "job from TADR 0x%08X, VBlank %u)\n", bytes, addr, d, job_tadr, ps2_vblank_count());
            if (chk_bad_jobs < 2)
                for (uint32_t q = 0; q < bytes / 16 && q < 48; q++) {
                    const float* a = (const float*)(s + q * 16);
                    const float* b = (const float*)(p + q * 16);
                    const uint32_t* wa = (const uint32_t*)(s + q * 16);
                    fprintf(stderr, "   +%03X kick %08X %08X %08X %08X (%g %g %g %g) now %g %g %g %g\n", q * 16, wa[0], wa[1], wa[2],
                            wa[3], a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                }
        }
    }
}

static int always_on;                            /* PS2_GFX_ASYNC=1: also at 60 (else only above 60 fps) */

static int take(int ch) {                        /* EE, dma.c pump */
    if (gfx_owned) return 1;
    /* At 60 the chain runs on the EE as before: the GS stream stays exactly the synchronous model's. The game sometimes
     * writes memory a kicked chain still reads (PS2_GFX_ASYNC_CHECK: a pool of effect objects at 0x1CC2xxx, written by
     * f_0015B428 and the main loop, read by REF tags of the frame in flight - on a PS2 the DMA races the EE the same way);
     * with the worker such a frame shows those objects' next values, one frame early. */
    if (!always_on && !frame_rate_high() && !gfx_quiet) return 0;
    if (check_every) {
        if (check_job) {                         /* the previous checked job's verdict */
            chk_jobs++;
            if (chk_bad_now) chk_bad_jobs++;
            if (chk_jobs % 100 == 0) {
                fprintf(stderr, "[gfx] PS2_GFX_ASYNC_CHECK: %llu jobs checked (%.1f MB read), %llu read changed memory (%llu bytes)\n",
                        (unsigned long long)chk_jobs, chk_bytes / 1e6, (unsigned long long)chk_bad_jobs, (unsigned long long)chk_bad_bytes);
                if (chk_jobs % 1000 == 0)
                    for (int k = 0; k < race_n; k++)
                        fprintf(stderr, "[gfx]   changed reads at 0x%08X (%u bytes): %u jobs, last at VBlank %u\n", race_addr[k],
                                race_len[k], race_cnt[k], race_vb[k]);
            }
        }
        check_job = (st_jobs % (uint64_t)check_every) == 0;
        chk_bad_now = 0;
        if (check_job) {
            memcpy(snap_ram, ps2_ram, PS2_RAM_SIZE);
            memcpy(snap_spr, ps2_spr, PS2_SPR_SIZE);
        }
    }
    job_ch = ch;
    job_tadr = dma_debug_tadr(ch);
    job_quiet = gfx_quiet;
    nevents = 0;
    gfx_owned = 1;
    st_jobs++;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    job_t0 = t0.QuadPart;
    atomic_fetch_add_explicit(&job_seq, 1, memory_order_release);
    WakeByAddressSingle((PVOID)&job_seq);
    if (log_on) {
        static int64_t next;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (t.QuadPart >= next) {
            static uint64_t tsc0, j0, w0, wt0;
            static int64_t jt0;
            static uint64_t pp0, pg0;
            uint64_t tsc = __rdtsc();
            double nj = st_jobs > j0 ? (double)(st_jobs - j0) : 1.0;
            if (next) fprintf(stderr, "[gfx] %llu jobs (%.2f ms each, kick to done; %.0fk VU1 pairs in %.0f programs each), EE waited %llu times, %.1f%% of the time\n",
                              (unsigned long long)(st_jobs - j0),
                              (double)(st_job_ticks - jt0) * 1000.0 / (double)qpc_freq.QuadPart / nj,
                              (double)(st_pairs - pp0) / nj / 1000.0, (double)(st_progs - pg0) / nj,
                              (unsigned long long)(st_waits - w0),
                              tsc > tsc0 ? 100.0 * (double)(st_wait_ticks - wt0) / (double)(tsc - tsc0) : 0.0);
            static uint64_t qj0, qp0;
            if (next && st_qjobs > qj0)
                fprintf(stderr, "[gfx] of these %llu re-run jobs (interp.c): %.0fk VU1 pairs each; the others %.0fk\n",
                        (unsigned long long)(st_qjobs - qj0), (double)(st_qpairs - qp0) / (double)(st_qjobs - qj0) / 1000.0,
                        st_jobs - j0 > st_qjobs - qj0 ? (double)((st_pairs - pp0) - (st_qpairs - qp0)) /
                        (double)((st_jobs - j0) - (st_qjobs - qj0)) / 1000.0 : 0.0);
            qj0 = st_qjobs; qp0 = st_qpairs;
            tsc0 = tsc; j0 = st_jobs; w0 = st_waits; wt0 = st_wait_ticks; jt0 = st_job_ticks; pp0 = st_pairs; pg0 = st_progs;
            next = t.QuadPart + qpc_freq.QuadPart;
        }
    }
    return 1;
}

/* PS2_WRITE_WATCH=<hex EE address>: the 4 KB host page holding it is made read-only; every write into the page is logged
 * once per writer (the recompiled function, by host symbol) with the address, then let through (single step, protected
 * again). A debugging aid for what writes memory an in-flight chain reads. */
const char* perf_sym_name(uint64_t pc);          /* perf.c */
static uint8_t* watch_page;
static uint32_t watch_lo, watch_hi;              /* EE range reported: watch address .. + 0x1000 */
static LONG CALLBACK watch_handler(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    static __thread int stepping;
    if (code == EXCEPTION_ACCESS_VIOLATION && watch_page) {
        uint8_t* a = (uint8_t*)e->ExceptionRecord->ExceptionInformation[1];
        if (a < watch_page || a >= watch_page + 4096) return EXCEPTION_CONTINUE_SEARCH;
        uint32_t ee = (uint32_t)(a - ps2_ram);
        if (ee >= watch_lo && ee < watch_hi) {
            static uint64_t seen[64];
            static int nseen;
            uint64_t rip = e->ContextRecord->Rip;
            int k = 0;
            while (k < nseen && seen[k] != rip) k++;
            if (k == nseen && nseen < 64) {
                seen[nseen++] = rip;
                fprintf(stderr, "[watch] write to 0x%08X by %s (host %p), VBlank %u, thread %u%s\n", ee, perf_sym_name(rip), (void*)rip,
                        ps2_vblank_count(), (unsigned)GetCurrentThreadId(), gfx_owned ? ", while a graphics job is out" : "");
            }
        }
        DWORD old;
        VirtualProtect(watch_page, 4096, PAGE_READWRITE, &old);
        e->ContextRecord->EFlags |= 0x100;       /* single step, then protect again */
        stepping = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && stepping) {
        stepping = 0;
        DWORD old;
        VirtualProtect(watch_page, 4096, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void watch_init(void) {
    const char* w = getenv("PS2_WRITE_WATCH");
    if (!w || !*w) return;
    uint32_t ee = (uint32_t)strtoul(w, NULL, 16) & 0x1FFFFFFu;
    watch_lo = ee & ~0xFFFu;
    watch_hi = watch_lo + 0x1000;
    uint8_t* host = ps2_ram + ee;
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(host, &mbi, sizeof mbi);
    watch_page = (uint8_t*)((uintptr_t)host & ~(uintptr_t)4095);
    AddVectoredExceptionHandler(1, watch_handler);
    DWORD old;
    if (!VirtualProtect(watch_page, 4096, PAGE_READONLY, &old)) { fprintf(stderr, "[watch] cannot protect the page\n"); watch_page = NULL; return; }
    fprintf(stderr, "[watch] writes to EE 0x%08X-0x%08X are logged\n", watch_lo, watch_hi - 1);
}

int gfx_async_init(void) {
    watch_init();
    const char* e = getenv("PS2_GFX_ASYNC");
    if (e && *e == '0') return 0;
    always_on = e && *e == '1';
    if ((getenv("PS2_VU_RECORD") && *getenv("PS2_VU_RECORD")) || (getenv("PS2_VU_VERIFY") && *getenv("PS2_VU_VERIFY") != '0')) {
        fprintf(stderr, "[gfx] graphics worker off (PS2_VU_RECORD / PS2_VU_VERIFY)\n");
        return 0;
    }
    QueryPerformanceFrequency(&qpc_freq);
    log_on = getenv("PS2_GFX_ASYNC_LOG") != NULL;
    const char* c = getenv("PS2_GFX_ASYNC_CHECK");
    if (c && atoi(c) > 0 && (snap_ram = malloc(PS2_RAM_SIZE))) {
        check_every = atoi(c);
        gfx_read_hook = read_check;
        fprintf(stderr, "[gfx] PS2_GFX_ASYNC_CHECK: every %d-th job's reads compared with memory at its kick\n", check_every);
    }
    gfx_defer_fn = defer;
    gfx_wait_fn = wait_job;
    thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!thread) { fprintf(stderr, "[gfx] cannot start the graphics worker\n"); return 0; }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    while (!gfx_worker_tid) Sleep(0);
    gfx_take_fn = take;
    fprintf(stderr, "[gfx] graphics worker ready: above 60 fps%s the VIF1 DMA chain runs beside the EE (PS2_GFX_ASYNC=0: never)\n",
            always_on ? " and at 60 (PS2_GFX_ASYNC=1)" : "");
    return 1;
}

void* gfx_worker_os_handle(void) { return thread; }   /* PS2_PROFILE=worker (perf.c) */
