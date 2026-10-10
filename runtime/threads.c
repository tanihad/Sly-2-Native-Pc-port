/* EE kernel threads and semaphores, on Windows fibers.
 *
 * Each PS2 thread gets its own fiber (native stack for the recompiled call chain) and its own
 * register set. Scheduling follows the EE kernel: the highest-priority ready thread runs (lower
 * number = higher priority); a running thread keeps the CPU until it blocks or a higher-priority
 * thread becomes ready. Switches only happen at syscalls and after interrupt handlers return,
 * the same places the real kernel can switch. */
#include "ps2_internal.h"
#include "perf.h"

#include <stdlib.h>
#include <windows.h>

enum { TH_FREE, TH_DORMANT, TH_READY, TH_WAIT_SEMA, TH_SLEEP, TH_RUNNING };

typedef struct {
    int      status;
    int      prio, init_prio;
    int64_t  seq;                  /* FIFO order within a priority */
    uint32_t entry, stack, stack_size, gp, attr, option;
    int      wakeups;              /* WakeupThread calls banked while not sleeping */
    int      wait_sema;
    void*    fiber;
    ps2_ctx  ctx;
} thread;

#define MAX_THREADS 128
static thread threads[MAX_THREADS];
static int current = 1;            /* main thread */
static int64_t seq_counter;
static int need_resched;

typedef struct { int used, count, max, attr; } sema;
static sema semas[256];

static ps2_ctx idle_ctx;           /* runs interrupt handlers while every thread is blocked */

ps2_ctx* thread_ctx(void) { return &threads[current].ctx; }
int thread_current_id(void) { return current; }

/* PS2_LOG_THREADS=1: log thread creation, start, exit and priority changes. */
static int log_threads = -1;
#define TLOG(...) do { if (log_threads < 0) log_threads = getenv("PS2_LOG_THREADS") != NULL; \
                       if (log_threads) fprintf(stderr, __VA_ARGS__); } while (0)

/* Print every thread's state (debug: PS2_LOG_THREADS with PS2_PEEK_EVERY). */
void threads_debug_dump(void) {
    static const char* names[] = { "free", "dormant", "ready", "wait-sema", "sleep", "running" };
    for (int i = 1; i < MAX_THREADS; i++) {
        thread* t = &threads[i];
        if (t->status == TH_FREE) continue;
        fprintf(stderr, "[thread] v%u #%d %s prio %d entry 0x%08X%s", ps2_vblank_count(), i, names[t->status],
                t->prio, t->entry, i == current ? " (current)" : "");
        if (t->status == TH_WAIT_SEMA)
            fprintf(stderr, " sema %d (count %d)", t->wait_sema, semas[t->wait_sema].count);
        fprintf(stderr, " ra 0x%08X\n", t->ctx.r[REG_RA].uw[0]);
    }
}

static void make_ready(int tid, int at_head) {
    threads[tid].status = TH_READY;
    threads[tid].seq = at_head ? -(++seq_counter) : ++seq_counter;
    if (tid != current && threads[tid].prio < threads[current].prio) need_resched = 1;
}

static int pick_next(void) {
    int best = 0;
    for (int i = 1; i < MAX_THREADS; i++) {
        thread* t = &threads[i];
        if (t->status != TH_READY && t->status != TH_RUNNING) continue;
        if (!best || t->prio < threads[best].prio ||
            (t->prio == threads[best].prio && t->seq < threads[best].seq)) best = i;
    }
    return best;
}

static void switch_to(int tid) {
    if (tid == current) { threads[tid].status = TH_RUNNING; return; }
    if (threads[current].status == TH_RUNNING) make_ready(current, 1);   /* preempted: head */
    current = tid;
    threads[tid].status = TH_RUNNING;
    SwitchToFiber(threads[tid].fiber);
    /* ...execution continues here when this thread is scheduled again */
}

static int idling;

/* Choose and run the next thread. If none is ready, wait for interrupts to wake one. */
static void schedule(void) {
    need_resched = 0;
    int next = pick_next();
    if (!next) {
        idling = 1;
        while (!next) {
            int perf_prev = perf_enter(PERF_IDLE);
            while (!ps2_irq_pending) Sleep(1);
            perf_leave(perf_prev);
            ps2_service_irq(&idle_ctx);
            next = pick_next();
        }
        idling = 0;
    }
    switch_to(next);
}

void thread_reschedule(void) {
    if (need_resched && !idling && !ps2_in_interrupt()) schedule();
}

static VOID CALLBACK thread_main(LPVOID arg) {
    thread* t = (thread*)arg;
    ps2_call(&t->ctx, t->entry);
    t->status = TH_DORMANT;                       /* returned from its function: ExitThread */
    schedule();
    for (;;) schedule();                          /* never resumed while dormant */
}

void threads_init(ps2_ctx* main_ctx) {
    threads[1].status = TH_RUNNING;
    threads[1].prio = threads[1].init_prio = 0;
    threads[1].fiber = ConvertThreadToFiber(NULL);
    threads[1].ctx = *main_ctx;
    idle_ctx.ie = 1;
    idle_ctx.r[REG_SP].sd[0] = 0x00070000;        /* kernel-area stack for handlers while idle */
    idle_ctx.vf[0].f[3] = 1.0f;
}

/* ---- semaphores -------------------------------------------------------------------------------- */

static int32_t sema_create(ps2_ctx* ctx, uint32_t param) {
    for (int i = 1; i < 256; i++) {
        if (semas[i].used) continue;
        /* param: count, max_count, init_count, wait_threads, attr, option */
        semas[i] = (sema){ 1, (int32_t)ps2_r32(ctx, param + 8), (int32_t)ps2_r32(ctx, param + 4),
                           (int32_t)ps2_r32(ctx, param + 16) };
        return i;
    }
    return -1;
}

static int sema_signal(int id) {
    sema* s = &semas[id];
    int best = 0;
    for (int i = 1; i < MAX_THREADS; i++)        /* wake the highest-priority waiter */
        if (threads[i].status == TH_WAIT_SEMA && threads[i].wait_sema == id &&
            (!best || threads[i].prio < threads[best].prio)) best = i;
    if (best) make_ready(best, 0);                /* the count passes straight to it */
    else if (s->max <= 0 || s->count < s->max) s->count++;
    return id;
}

/* ---- syscalls ------------------------------------------------------------------------------------ */

static thread* get(uint32_t tid) {
    if (tid == 0) tid = (uint32_t)current;        /* TH_SELF */
    return (tid < MAX_THREADS && threads[tid].status != TH_FREE) ? &threads[tid] : NULL;
}

/* Returns 1 and sets *ret if `n` is a thread/semaphore syscall. May switch threads. */
int thread_syscall(ps2_ctx* ctx, int n, int64_t* ret) {
    uint32_t a0 = ctx->r[REG_A0].uw[0], a1 = ctx->r[REG_A1].uw[0];
    thread* t;
    switch (n) {
    case 0x20: {                                  /* CreateThread(param) */
        for (int i = 2; i < MAX_THREADS; i++) {
            if (threads[i].status != TH_FREE) continue;
            t = &threads[i];
            memset(t, 0, sizeof *t);
            t->status = TH_DORMANT;
            t->entry = ps2_r32(ctx, a0 + 4);
            t->stack = ps2_r32(ctx, a0 + 8);
            t->stack_size = ps2_r32(ctx, a0 + 12);
            t->gp = ps2_r32(ctx, a0 + 16);
            t->prio = t->init_prio = (int)ps2_r32(ctx, a0 + 20);
            t->attr = ps2_r32(ctx, a0 + 28);
            t->option = ps2_r32(ctx, a0 + 32);
            TLOG("[thread] create #%d entry 0x%08X prio %d stack 0x%08X+0x%X (by #%d, ra 0x%08X)\n", i, t->entry,
                 t->prio, t->stack, t->stack_size, current, ctx->r[REG_RA].uw[0]);
            *ret = i;
            return 1;
        }
        *ret = -1;
        return 1;
    }
    case 0x21: case 0x25: case 0x26:              /* DeleteThread, (i)TerminateThread */
        if (!(t = get(a0)) || t == &threads[current]) { *ret = -1; return 1; }
        t->status = (n == 0x21) ? TH_FREE : TH_DORMANT;
        *ret = 0;
        return 1;
    case 0x22: {                                  /* StartThread(tid, arg) */
        if (!(t = get(a0)) || t->status != TH_DORMANT) { *ret = -1; return 1; }
        memset(&t->ctx, 0, sizeof t->ctx);
        t->ctx.ie = 1;
        t->ctx.vf[0].f[3] = 1.0f;
        t->ctx.r[REG_A0].sd[0] = (int32_t)a1;
        t->ctx.r[REG_GP].sd[0] = (int32_t)t->gp;
        t->ctx.r[REG_SP].sd[0] = (int32_t)((t->stack + t->stack_size - 0x2A0u) & ~15u);
        t->prio = t->init_prio;
        if (!t->fiber) t->fiber = CreateFiber(1 << 20, thread_main, t);
        TLOG("[thread] start #%d arg 0x%08X (by #%d prio %d)\n", (int)(t - threads), a1, current, threads[current].prio);
        make_ready((int)(t - threads), 0);
        *ret = 0;
        return 1;
    }
    case 0x23: case 0x24:                         /* ExitThread, ExitDeleteThread */
        TLOG("[thread] exit #%d (%s)\n", current, n == 0x23 ? "ExitThread" : "ExitDeleteThread");
        threads[current].status = (n == 0x23) ? TH_DORMANT : TH_FREE;
        schedule();
        *ret = 0;
        return 1;
    case 0x29: case 0x2A:                         /* (i)ChangeThreadPriority(tid, prio) */
        if (!(t = get(a0))) { *ret = -1; return 1; }
        TLOG("[thread] priority #%d %d -> %d (by #%d, ra 0x%08X)\n", (int)(t - threads), t->prio, (int)a1, current,
             ctx->r[REG_RA].uw[0]);
        *ret = t->prio;
        t->prio = (int)a1;
        if (t == &threads[current]) need_resched = 1;          /* may now be outranked */
        else if (t->status == TH_READY && t->prio < threads[current].prio) need_resched = 1;
        return 1;
    case 0x2B: case 0x2C:                         /* (i)RotateThreadReadyQueue(prio) */
        if (threads[current].prio == (int)a0) { threads[current].seq = ++seq_counter; need_resched = 1; }
        *ret = 0;
        return 1;
    case 0x2F:                                    /* GetThreadId */
        *ret = current;
        return 1;
    case 0x30: case 0x31: {                       /* (i)ReferThreadStatus(tid, info) */
        if (!(t = get(a0))) { *ret = -1; return 1; }
        static const int ps2_status[] = { 0, 0x10, 2, 4, 4, 1 };  /* dormant, ready, wait, run */
        if (a1) {
            uint32_t w[12] = { (uint32_t)ps2_status[t->status], t->entry, t->stack, t->stack_size,
                               t->gp, (uint32_t)t->init_prio, (uint32_t)t->prio, t->attr, t->option,
                               (uint32_t)(t->status == TH_SLEEP ? 1 : t->status == TH_WAIT_SEMA ? 3 : 0),
                               (uint32_t)t->wait_sema, (uint32_t)t->wakeups };
            for (int k = 0; k < 12; k++) ps2_w32(ctx, a1 + 4u * k, w[k]);
        }
        *ret = ps2_status[t->status];
        return 1;
    }
    case 0x32:                                    /* SleepThread */
        if (threads[current].wakeups > 0) threads[current].wakeups--;
        else { threads[current].status = TH_SLEEP; schedule(); }
        *ret = 0;
        return 1;
    case 0x33: case 0x34:                         /* (i)WakeupThread(tid) */
        if (!(t = get(a0))) { *ret = -1; return 1; }
        if (t->status == TH_SLEEP) make_ready((int)(t - threads), 0);
        else t->wakeups++;
        *ret = 0;
        return 1;
    case 0x35: case 0x36:                         /* (i)CancelWakeupThread(tid) */
        if (!(t = get(a0))) { *ret = -1; return 1; }
        *ret = t->wakeups;
        t->wakeups = 0;
        return 1;

    case 0x40: *ret = sema_create(ctx, a0); return 1;                      /* CreateSema */
    case 0x41:                                                             /* DeleteSema */
        if (a0 >= 256 || !semas[a0].used) { *ret = -1; return 1; }
        semas[a0].used = 0;
        for (int i = 1; i < MAX_THREADS; i++)
            if (threads[i].status == TH_WAIT_SEMA && threads[i].wait_sema == (int)a0) make_ready(i, 0);
        *ret = (int32_t)a0;
        return 1;
    case 0x42: case 0x43:                                                  /* (i)SignalSema */
        *ret = (a0 < 256 && semas[a0].used) ? sema_signal((int)a0) : -1;
        return 1;
    case 0x44:                                                             /* WaitSema */
        if (a0 >= 256 || !semas[a0].used) { *ret = -1; return 1; }
        if (semas[a0].count > 0) semas[a0].count--;
        else { threads[current].status = TH_WAIT_SEMA; threads[current].wait_sema = (int)a0; schedule(); }
        *ret = (int32_t)a0;
        return 1;
    case 0x45: case 0x46:                                                  /* (i)PollSema */
        if (a0 >= 256 || !semas[a0].used) { *ret = -1; return 1; }
        if (semas[a0].count > 0) { semas[a0].count--; *ret = (int32_t)a0; }
        else *ret = -419;                                                  /* KE_SEMA_ZERO */
        return 1;
    case 0x47: case 0x48:                                                  /* (i)ReferSemaStatus */
        if (a0 >= 256 || !semas[a0].used) { *ret = -1; return 1; }
        if (a1) {
            int waiting = 0;
            for (int i = 1; i < MAX_THREADS; i++)
                waiting += threads[i].status == TH_WAIT_SEMA && threads[i].wait_sema == (int)a0;
            uint32_t w[6] = { (uint32_t)semas[a0].count, (uint32_t)semas[a0].max, 0, (uint32_t)waiting,
                              (uint32_t)semas[a0].attr, 0 };
            for (int k = 0; k < 6; k++) ps2_w32(ctx, a1 + 4u * k, w[k]);
        }
        *ret = (int32_t)a0;
        return 1;
    default:
        return 0;
    }
}
