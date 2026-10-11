/* Unit tests for runtime/threads.c (EE kernel threads on Windows fibers): thread restarts and the
 * fibers behind them. The "game" threads are C functions dispatched by a fake ps2_call; they call
 * thread_syscall() like the recompiled code does.
 *   powershell -ExecutionPolicy Bypass -File tests\unit\threads\run.ps1 */
#include "ps2_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <psapi.h>

/* ---- fakes for what threads.c needs from the rest of the runtime ---- */
uint8_t* ps2_ram;
uint8_t* ps2_spr;
volatile uint32_t ps2_irq_pending;
void ps2_service_irq(ps2_ctx* ctx) { (void)ctx; ps2_irq_pending = 0; }
int ps2_in_interrupt(void) { return 0; }
uint32_t ps2_vblank_count(void) { return 0; }
void ps2_mem_read_slow(ps2_ctx* ctx, uint32_t a, void* out, unsigned size) { (void)ctx; (void)a; memset(out, 0, size); }
void ps2_mem_write_slow(ps2_ctx* ctx, uint32_t a, const void* in, unsigned size) { (void)ctx; (void)a; (void)in; (void)size; }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int64_t sys(ps2_ctx* c, int n, uint32_t a0, uint32_t a1) {
    c->r[REG_A0].sd[0] = (int32_t)a0;
    c->r[REG_A1].sd[0] = (int32_t)a1;
    int64_t ret = 0x7777;
    if (!thread_syscall(c, n, &ret)) { failures++; printf("FAIL: syscall 0x%X not handled\n", n); }
    return ret;
}

/* ---- the "game" threads ---- */
enum { E_EXIT_DELETE = 0x1000, E_RETURNS = 0x2000, E_EXIT_MID = 0x3000, E_WAITS = 0x4000 };
static int runs_exit_delete, runs_returns, runs_exit_mid, runs_waits;
static int after_exit_delete, after_exit, after_wait;
static int forever_sema;
static int touch_stack(int depth) {              /* commits some of the fiber's stack, as game code does */
    volatile char buf[4096];
    buf[0] = (char)depth;
    return depth ? touch_stack(depth - 1) + buf[0] : 0;
}

void ps2_call(ps2_ctx* ctx, uint32_t addr) {
    switch (addr) {
    case E_EXIT_DELETE:                          /* Sly 2's 0x1AAF18: runs, then ExitDeleteThread */
        runs_exit_delete++;
        touch_stack(16);
        sys(ctx, 0x24, 0, 0);
        after_exit_delete++;                     /* must never run */
        return;
    case E_RETURNS:                              /* returns from its function (= ExitThread) */
        runs_returns++;
        return;
    case E_EXIT_MID:                             /* ExitThread from inside its call chain */
        runs_exit_mid++;
        sys(ctx, 0x23, 0, 0);
        after_exit++;                            /* must never run, even after a new StartThread */
        return;
    case E_WAITS:                                /* blocks forever; terminated from outside */
        runs_waits++;
        sys(ctx, 0x44, (uint32_t)forever_sema, 0);
        after_wait++;                            /* must never run after TerminateThread */
        return;
    default:
        failures++;
        printf("FAIL: ps2_call(0x%08X)\n", addr);
    }
}

/* CreateThread's parameter block in PS2 RAM: +4 entry, +8 stack, +12 size, +16 gp, +20 prio */
static int create(ps2_ctx* c, uint32_t entry, int prio) {
    const uint32_t p = 0x100000;
    uint32_t w[9] = { 0, entry, 0x200000, 0x1000, 0, (uint32_t)prio, 0, 0, 0 };
    memcpy(ps2_ram + p, w, sizeof w);
    return (int)sys(c, 0x20, p, 0);
}

static void start_and_run(ps2_ctx* c, int tid) {
    CHECK(sys(c, 0x22, (uint32_t)tid, 0) == 0, "StartThread(%d)", tid);
    thread_reschedule();                         /* the higher-priority thread runs until it blocks/exits */
}

static size_t committed(void) {
    PROCESS_MEMORY_COUNTERS_EX pm = { .cb = sizeof pm };
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    return pm.PrivateUsage;
}

int main(void) {
    ps2_ram = calloc(1, PS2_RAM_SIZE + 16);
    ps2_spr = calloc(1, PS2_SPR_SIZE + 16);
    ps2_ctx main_ctx;
    memset(&main_ctx, 0, sizeof main_ctx);
    threads_init(&main_ctx);
    ps2_ctx* c = thread_ctx();
    sys(c, 0x29, 0, 10);                         /* main at priority 10: the test threads (1) preempt it */
    uint32_t sp = 0x100100;                      /* CreateSema(count 0, max 1) */
    uint32_t sw[6] = { 0, 1, 0, 0, 0, 0 };
    memcpy(ps2_ram + sp, sw, sizeof sw);
    forever_sema = (int)sys(c, 0x40, sp, 0);
    CHECK(forever_sema > 0, "CreateSema");

    /* 1. create / start / ExitDeleteThread, as Sly 2 does about once a second: the slot is reused and
     *    no fiber may be left behind (each one is 1 MB of committed stack) */
    size_t before = committed();
    for (int i = 0; i < 300; i++) {
        int tid = create(c, E_EXIT_DELETE, 1);
        CHECK(tid > 1, "CreateThread #%d -> %d", i, tid);
        start_and_run(c, tid);
    }
    size_t grown = committed() - before;
    CHECK(runs_exit_delete == 300, "exit-delete thread ran %d times, expected 300", runs_exit_delete);
    CHECK(after_exit_delete == 0, "code after ExitDeleteThread ran");
    CHECK(grown < (size_t)32 << 20, "300 create/start/ExitDeleteThread cycles grew committed memory by %zu MB",
          grown >> 20);
    printf("create/start/ExitDeleteThread x300: committed memory %+lld KB\n", (long long)grown / 1024);

    /* 2. a thread that returned from its function starts again from its entry (parked fiber reused) */
    int t2 = create(c, E_RETURNS, 1);
    for (int i = 0; i < 3; i++) start_and_run(c, t2);
    CHECK(runs_returns == 3, "returning thread ran %d times, expected 3", runs_returns);

    /* 3. ExitThread mid-way, StartThread again: the entry runs again, never the code after ExitThread */
    int t3 = create(c, E_EXIT_MID, 1);
    for (int i = 0; i < 3; i++) start_and_run(c, t3);
    CHECK(runs_exit_mid == 3, "ExitThread thread ran %d times, expected 3", runs_exit_mid);
    CHECK(after_exit == 0, "a restarted thread resumed after its ExitThread (%d times)", after_exit);

    /* 4. TerminateThread of a blocked thread, StartThread again: from the entry, not from WaitSema */
    int t4 = create(c, E_WAITS, 1);
    for (int i = 0; i < 3; i++) {
        start_and_run(c, t4);
        CHECK(sys(c, 0x25, (uint32_t)t4, 0) == 0, "TerminateThread");
    }
    CHECK(runs_waits == 3, "terminated thread ran %d times, expected 3", runs_waits);
    CHECK(after_wait == 0, "a restarted thread resumed in its WaitSema (%d times)", after_wait);

    /* 5. DeleteThread of a blocked thread, then CreateThread reuses the slot for another entry */
    int t5 = create(c, E_WAITS, 1);
    start_and_run(c, t5);
    CHECK(sys(c, 0x21, (uint32_t)t5, 0) == 0, "DeleteThread");
    int t6 = create(c, E_RETURNS, 1);
    CHECK(t6 == t5, "slot %d reused (got %d)", t5, t6);
    start_and_run(c, t6);
    CHECK(runs_returns == 4 && after_wait == 0, "new thread in a deleted blocked thread's slot: returns %d, after wait %d",
          runs_returns, after_wait);

    /* 6. a returned (parked) thread deleted, its slot reused by a thread that ExitDeleteThreads */
    CHECK(sys(c, 0x21, (uint32_t)t2, 0) == 0, "DeleteThread of a dormant thread");
    int t7 = create(c, E_EXIT_DELETE, 1);
    start_and_run(c, t7);
    CHECK(runs_exit_delete == 301 && after_exit_delete == 0, "parked fiber reused for another entry");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("threads: all tests passed\n");
    return 0;
}
