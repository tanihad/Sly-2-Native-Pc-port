/* Native IOP kernel: what Sony's IOP kernel modules (sysmem, loadcore, intrman, threadman, timrman)
 * provide to the recompiled sound modules, written for the PC.
 *
 * - sysmem: one first-fit allocator over IOP RAM 0x20000-0x1FFFFF (memory map in iop.h).
 * - threads: each IOP thread is a Windows fiber with its own register context and a stack in IOP
 *   RAM. Scheduling follows the IOP kernel: the ready thread with the lowest priority number runs
 *   and keeps the CPU until it blocks or a higher-priority thread becomes ready. Threads only run
 *   inside iop_run(), which the EE side calls at its safe points and after RPC requests; iop_run
 *   returns when every IOP thread is blocked (or after a time slice, so a busy-waiting IOP thread
 *   cannot stall the game).
 * - interrupts: there is no interrupt hardware. Interrupt sources (hard timers, CD and SIF DMA
 *   completion, libsd transfers) are polled between thread slices and at loop heads of code that
 *   has interrupts enabled; handlers run on the interrupt stack with QueryIntrContext() true.
 * - timrman: hard timers fire on the host clock (the 989snd tick: 240 Hz of real time).
 * Semantics follow the IOP kernel documentation and ps2sdk's headers (thbase.h, thsemap.h,
 * thevent.h, intrman.h, timrman.h, sysmem.h, kerr.h). */
#include "iop_internal.h"
#include "sd.h"

#include <stdlib.h>
#include <windows.h>

uint8_t  iop_ram[IOP_RAM_SIZE];
uint8_t  iop_spr[IOP_SPR_SIZE];
uint32_t iop_trace[IOP_TRACE_SIZE];
uint32_t iop_trace_pos;
volatile uint32_t iop_poll_flag;
int iop_log;

#define SYSCLOCK_HZ 36864000u            /* IOP bus clock: SYSCLOCK timer source */

/* ---- recompiled code ----------------------------------------------------------------------------- */

static const iop_func_entry* funcs;
static size_t nfuncs;
static const iop_module_desc* mod_descs;
static size_t nmods;

void iop_register_recompiled(const iop_func_entry* f, size_t nf, const iop_module_desc* m, size_t nm) {
    funcs = f; nfuncs = nf; mod_descs = m; nmods = nm;
}

const iop_module_desc* iop_module_by_name(const char* name) {
    for (size_t i = 0; i < nmods; i++) if (strcmp(mod_descs[i].name, name) == 0) return &mod_descs[i];
    return NULL;
}

int iop_native_audio(void) {
    const char* e = getenv("PS2_AUDIO");
    return nmods > 0 && !(e && strcmp(e, "0") == 0);
}

static iop_func find_func(uint32_t addr) {
    size_t lo = 0, hi = nfuncs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < nfuncs && funcs[lo].addr == addr) ? funcs[lo].fn : NULL;
}

void iop_dump_trace(const char* why) {
    fprintf(stderr, "[iop] %s; last IOP functions (oldest first):\n ", why);
    for (uint32_t i = 16; i > 0; i--) fprintf(stderr, " %08X", iop_trace[(iop_trace_pos - i) & (IOP_TRACE_SIZE - 1)]);
    fprintf(stderr, "\n");
}

void iop_call(iop_ctx* c, uint32_t addr) {
    iop_func fn = find_func(addr);
    if (fn) { fn(c); return; }
    static uint32_t seen[32]; static int n;
    int known = 0;
    for (int i = 0; i < n; i++) known |= seen[i] == addr;
    if (!known) {
        if (n < 32) seen[n++] = addr;
        fprintf(stderr, "[iop] call to 0x%08X, which is not a recompiled IOP function ($ra=0x%08X, thread %d)\n",
                addr, c->r[31], iop_thread_current());
        iop_dump_trace("bad call");
    }
    c->r[2] = 0;
}

static int log_once(uint32_t* seen, int* n, int cap, uint32_t key) {
    for (int i = 0; i < *n; i++) if (seen[i] == key) return 0;
    if (*n < cap) seen[(*n)++] = key;
    return 1;
}

void iop_break(iop_ctx* c, uint32_t pc, uint32_t code) {
    static uint32_t seen[64]; static int n;
    if (log_once(seen, &n, 64, pc))
        fprintf(stderr, "[iop] break %u at 0x%08X (thread %d, a0=0x%08X)%s\n", code, pc, iop_thread_current(),
                c->r[4], code == 7 ? ": divide by zero" : "");
}

void iop_syscall(iop_ctx* c, uint32_t pc) {
    (void)c;
    static uint32_t seen[16]; static int n;
    if (log_once(seen, &n, 16, pc)) fprintf(stderr, "[iop] syscall at 0x%08X ignored\n", pc);
}

void iop_unimpl(iop_ctx* c, uint32_t pc, const char* op) {
    (void)c;
    static uint32_t seen[64]; static int n;
    if (log_once(seen, &n, 64, pc)) fprintf(stderr, "[iop] unimplemented %s at 0x%08X\n", op, pc);
}

uint32_t iop_hw_read(uint32_t addr, unsigned size) {
    static uint32_t seen[64]; static int n;
    if (log_once(seen, &n, 64, addr)) fprintf(stderr, "[iop] read%u of I/O address 0x%08X (returns 0)\n", size * 8, addr);
    return 0;
}

void iop_hw_write(uint32_t addr, uint32_t value, unsigned size) {
    static uint32_t seen[64]; static int n;
    if (log_once(seen, &n, 64, addr))
        fprintf(stderr, "[iop] write%u 0x%X to I/O address 0x%08X (ignored)\n", size * 8, value, addr);
}

void iop_strncpy_from(char* dst, uint32_t src, size_t cap) {
    size_t i = 0;
    if (src) for (; i + 1 < cap; i++) { char ch = (char)iop_r8(src + (uint32_t)i); if (!ch) break; dst[i] = ch; }
    dst[i] = 0;
}

/* ---- time ------------------------------------------------------------------------------------------ */

static LARGE_INTEGER qpc_freq;

int64_t iop_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t iop_usec_to_ticks(uint64_t usec) {
    if (!qpc_freq.QuadPart) QueryPerformanceFrequency(&qpc_freq);
    return (int64_t)(usec * (uint64_t)qpc_freq.QuadPart / 1000000u);
}

static int64_t sysclock_to_ticks(uint64_t cycles, uint32_t hz) {
    if (!qpc_freq.QuadPart) QueryPerformanceFrequency(&qpc_freq);
    return (int64_t)(cycles * (uint64_t)qpc_freq.QuadPart / hz);
}

/* ---- sysmem ------------------------------------------------------------------------------------------ */

#define SYSMEM_START 0x00020000u
#define SYSMEM_GRAIN 0x100u
#define MAX_BLOCKS   1024

typedef struct { uint32_t addr, size; } mem_block;
static mem_block blocks[MAX_BLOCKS];       /* allocated blocks, sorted by address */
static int nblocks;
static int inited;
static uint32_t intr_stack_top;

static uint32_t sysmem_alloc(int mode, uint32_t size, uint32_t want) {
    if (!inited) iop_init();
    if (!size || size > IOP_RAM_SIZE || nblocks == MAX_BLOCKS) return 0;
    size = (size + SYSMEM_GRAIN - 1) & ~(SYSMEM_GRAIN - 1);
    want &= ~(SYSMEM_GRAIN - 1);
    int found = 0, pos = 0;
    uint32_t best = 0, prev_end = SYSMEM_START;
    for (int i = 0; i <= nblocks; i++) {
        uint32_t gs = prev_end, ge = i < nblocks ? blocks[i].addr : IOP_RAM_SIZE;
        if (mode == 2) {                                   /* ALLOC_ADDRESS */
            if (want >= gs && want + size <= ge) { best = want; pos = i; found = 1; break; }
        } else if (ge - gs >= size) {
            best = mode == 1 ? ge - size : gs;             /* ALLOC_LAST : ALLOC_FIRST */
            pos = i; found = 1;
            if (mode == 0) break;
        }
        if (i < nblocks) prev_end = blocks[i].addr + blocks[i].size;
    }
    if (!found) return 0;
    memmove(&blocks[pos + 1], &blocks[pos], (size_t)(nblocks - pos) * sizeof *blocks);
    blocks[pos] = (mem_block){ best, size };
    nblocks++;
    return best;
}

uint32_t iop_alloc(uint32_t size) { return sysmem_alloc(0, size, 0); }
uint32_t iop_alloc_at(uint32_t addr, uint32_t size) { return sysmem_alloc(2, size, addr); }

int iop_free(uint32_t addr) {
    addr &= 0x1FFFFFFFu;
    for (int i = 0; i < nblocks; i++) {
        if (blocks[i].addr != addr) continue;
        memmove(&blocks[i], &blocks[i + 1], (size_t)(nblocks - i - 1) * sizeof *blocks);
        nblocks--;
        return 0;
    }
    return -1;
}

uint32_t iop_max_free(void) {
    if (!inited) iop_init();
    uint32_t best = 0, prev_end = SYSMEM_START;
    for (int i = 0; i <= nblocks; i++) {
        uint32_t ge = i < nblocks ? blocks[i].addr : IOP_RAM_SIZE;
        if (ge - prev_end > best) best = ge - prev_end;
        if (i < nblocks) prev_end = blocks[i].addr + blocks[i].size;
    }
    return best;
}

uint32_t iop_total_free(void) {
    if (!inited) iop_init();
    uint32_t used = 0;
    for (int i = 0; i < nblocks; i++) used += blocks[i].size;
    return IOP_RAM_SIZE - SYSMEM_START - used;
}

void iop_init(void) {
    if (inited) return;
    inited = 1;
    iop_log = getenv("PS2_LOG_IOP") != NULL;
    QueryPerformanceFrequency(&qpc_freq);
    nblocks = 0;
    for (size_t i = 0; i < nmods; i++)                     /* the recompiled modules' fixed homes */
        if (!sysmem_alloc(2, mod_descs[i].memsz, mod_descs[i].base))
            fprintf(stderr, "[iop] cannot reserve 0x%08X+0x%X for %s\n", mod_descs[i].base, mod_descs[i].memsz,
                    mod_descs[i].name);
    uint32_t st = sysmem_alloc(1, 0x2000, 0);              /* interrupt stack */
    intr_stack_top = st + 0x2000 - 0x10;
}

/* sysmem exports: AllocSysMemory(mode, size, addr), FreeSysMemory(ptr), Query*MemSize() */
void iop_imp_sysmem_AllocSysMemory(iop_ctx* c) {
    uint32_t a = sysmem_alloc((int)c->r[4], c->r[5], c->r[6]);
    if (iop_log) fprintf(stderr, "[iop] AllocSysMemory(%u, 0x%X, 0x%X) -> 0x%08X\n", c->r[4], c->r[5], c->r[6], a);
    if (!a) fprintf(stderr, "[iop] AllocSysMemory(mode %u, 0x%X bytes, at 0x%X) failed (largest free block 0x%X)\n",
                    c->r[4], c->r[5], c->r[6], iop_max_free());
    iop_ret(c, (int32_t)a);
}
void iop_imp_sysmem_FreeSysMemory(iop_ctx* c) { iop_ret(c, iop_free(c->r[4]) ? KE_ERROR : KE_OK); }
void iop_imp_sysmem_QueryMemSize(iop_ctx* c) { iop_ret(c, (int32_t)IOP_RAM_SIZE); }
void iop_imp_sysmem_QueryMaxFreeMemSize(iop_ctx* c) { iop_ret(c, (int32_t)iop_max_free()); }
void iop_imp_sysmem_QueryTotalFreeMemSize(iop_ctx* c) { iop_ret(c, (int32_t)iop_total_free()); }

/* loadcore: the recompiler links imports statically, so library registration has nothing to do. */
void iop_imp_loadcore_RegisterLibraryEntries(iop_ctx* c) { iop_ret(c, KE_OK); }
void iop_imp_loadcore_ReleaseLibraryEntries(iop_ctx* c) { iop_ret(c, KE_OK); }
void iop_imp_loadcore_FlushDcache(iop_ctx* c) { (void)c; }

/* ---- threads --------------------------------------------------------------------------------------- */

enum { TS_FREE, TS_DORMANT, TS_READY, TS_RUNNING, TS_WAIT };

typedef struct {
    int      status, wait_type, wake_code;
    uint32_t wait_obj, wdata[4];
    int      prio, init_prio;
    int64_t  seq, wait_seq;
    uint32_t entry, stack, stack_size, gp, attr, option, init_a1;
    int      wakeups;
    int64_t  delay_until;
    uint32_t delay_pass;
    int      intr_disabled;
    uint32_t exit_value;
    void*    fiber;
    int      fiber_stale;                  /* fiber stopped mid-way (ExitThread): replace on start */
    uint32_t slices, slices_seen;          /* time slices used up without blocking (busy-waiting) */
    iop_ctx  ctx;
} iop_thread;

#define MAX_THREADS 64
static iop_thread th[MAX_THREADS];        /* index = thread id; 0 unused */
static int cur;                            /* running thread, 0 = scheduler / outside threads */
static int64_t seq_counter;
static void* sched_fiber;
static int in_run, in_intr;
static uint32_t run_pass;
static int64_t run_deadline;
static iop_ctx intr_ctx;

#define SLICE_USEC 2000                    /* longest uninterrupted IOP run before the EE continues */

int iop_thread_current(void) { return in_intr ? 0 : cur; }
int iop_in_interrupt(void) { return in_intr; }
int iop_thread_priority(int tid) { return tid > 0 && tid < MAX_THREADS ? th[tid].prio : 0; }

static iop_thread* get_thread(int tid) {
    if (tid == 0) tid = cur;                           /* TH_SELF */
    return (tid > 0 && tid < MAX_THREADS && th[tid].status != TS_FREE) ? &th[tid] : NULL;
}

static void make_ready(int tid) {
    th[tid].status = TS_READY;
    th[tid].seq = ++seq_counter;
}

static int pick_next(void) {
    int best = 0;
    for (int i = 1; i < MAX_THREADS; i++) {
        if (th[i].status != TS_READY) continue;
        if (!best || th[i].prio < th[best].prio || (th[i].prio == th[best].prio && th[i].seq < th[best].seq))
            best = i;
    }
    return best;
}

static void switch_to_scheduler(void) {
    SwitchToFiber(sched_fiber);
}

/* The running thread gives up the CPU but stays ready, first in line at its priority. */
static void yield_running(void) {
    iop_thread* t = &th[cur];
    t->status = TS_READY;
    t->seq = -(++seq_counter);
    switch_to_scheduler();
}

void iop_check_preempt(void) {
    if (!cur || in_intr) return;
    int next = pick_next();
    if (next && th[next].prio < th[cur].prio) yield_running();
}

int iop_block(int wait_type, uint32_t wait_obj) {
    if (in_intr || !cur) {
        fprintf(stderr, "[iop] blocking call (wait %d) outside thread context\n", wait_type);
        return KE_ILLEGAL_CONTEXT;
    }
    iop_thread* t = &th[cur];
    t->status = TS_WAIT;
    t->wait_type = wait_type;
    t->wait_obj = wait_obj;
    t->wake_code = KE_OK;
    t->wait_seq = ++seq_counter;
    switch_to_scheduler();
    return t->wake_code;
}

void iop_wake(int tid, int code) {
    if (tid <= 0 || tid >= MAX_THREADS || th[tid].status != TS_WAIT) return;
    th[tid].wake_code = code;
    th[tid].wait_type = 0;
    make_ready(tid);
}

int iop_find_waiter(int wait_type, uint32_t wait_obj, int fifo) {
    int best = 0;
    for (int i = 1; i < MAX_THREADS; i++) {
        iop_thread* t = &th[i];
        if (t->status != TS_WAIT || t->wait_type != wait_type || t->wait_obj != wait_obj) continue;
        if (!best || (fifo ? t->wait_seq < th[best].wait_seq
                           : (t->prio < th[best].prio || (t->prio == th[best].prio && t->wait_seq < th[best].wait_seq))))
            best = i;
    }
    return best;
}

uint32_t iop_waiter_data(int tid, int k) { return th[tid].wdata[k & 3]; }
void iop_set_waiter_data(int tid, int k, uint32_t v) { th[tid].wdata[k & 3] = v; }

static VOID CALLBACK thread_main(LPVOID arg) {
    iop_thread* t = (iop_thread*)arg;
    for (;;) {
        iop_call(&t->ctx, t->entry);
        t->exit_value = t->ctx.r[2];                       /* returning from the entry = ExitThread */
        t->status = TS_DORMANT;
        if (iop_log) fprintf(stderr, "[iop] thread %d exited (0x%X)\n", (int)(t - th), t->exit_value);
        switch_to_scheduler();                             /* resumed only by a new StartThread */
    }
}

static int create_thread(uint32_t entry, int prio, uint32_t stack_size, uint32_t attr, uint32_t option, uint32_t gp) {
    if (!entry) return KE_ILLEGAL_ENTRY;
    if (prio < 1 || prio > 126) return KE_ILLEGAL_PRIORITY;
    if (stack_size < 0x130) return KE_ILLEGAL_STACK_SIZE;
    for (int i = 1; i < MAX_THREADS; i++) {
        if (th[i].status != TS_FREE) continue;
        stack_size = (stack_size + 0xFF) & ~0xFFu;
        uint32_t stack = sysmem_alloc(1, stack_size, 0);
        if (!stack) return KE_NO_MEMORY;
        iop_thread* t = &th[i];
        void* fiber = t->fiber;                            /* a parked fiber of a deleted thread is reused */
        int stale = t->fiber_stale;
        memset(t, 0, sizeof *t);
        t->fiber = fiber;
        t->fiber_stale = stale;
        t->status = TS_DORMANT;
        t->entry = entry;
        t->prio = t->init_prio = prio;
        t->stack = stack;
        t->stack_size = stack_size;
        t->attr = attr;
        t->option = option;
        t->gp = gp;
        memset(iop_ram + stack, 0xFF, stack_size);         /* the kernel fills new stacks */
        return i;
    }
    return KE_NO_MEMORY;
}

static int start_thread(int tid, uint32_t a0, uint32_t a1) {
    iop_thread* t = get_thread(tid);
    if (!t) return KE_UNKNOWN_THID;
    if (t->status != TS_DORMANT) return KE_NOT_DORMANT;
    memset(&t->ctx, 0, sizeof t->ctx);
    t->ctx.r[4] = a0;
    t->ctx.r[5] = a1;
    t->ctx.r[28] = t->gp;
    t->ctx.r[29] = (t->stack + t->stack_size - 0x10) & ~7u;
    t->prio = t->init_prio;
    t->wakeups = 0;
    t->intr_disabled = 0;
    if (t->fiber && t->fiber_stale) { DeleteFiber(t->fiber); t->fiber = NULL; }
    t->fiber_stale = 0;
    if (!t->fiber) t->fiber = CreateFiberEx(64 * 1024, 1 << 20, 0, thread_main, t);
    if (!t->fiber) { fprintf(stderr, "[iop] CreateFiber failed\n"); return KE_NO_MEMORY; }
    make_ready((int)(t - th));
    return KE_OK;
}

static void free_thread(iop_thread* t) {
    iop_free(t->stack);
    t->status = TS_FREE;                                   /* fiber is parked or deleted: kept for reuse */
}

int iop_create_kernel_thread(uint32_t entry, int prio, uint32_t stack_size, uint32_t a0, uint32_t a1, uint32_t gp) {
    int tid = create_thread(entry, prio, stack_size, 0x02000000u, 0, gp);
    if (tid > 0) start_thread(tid, a0, a1);
    return tid;
}

int iop_thread_status_dormant(int tid, uint32_t* exit_value) {
    if (tid <= 0 || tid >= MAX_THREADS || th[tid].status != TS_DORMANT) return 0;
    if (exit_value) *exit_value = th[tid].exit_value;
    return 1;
}

void iop_delete_kernel_thread(int tid) {
    if (tid > 0 && tid < MAX_THREADS && th[tid].status == TS_DORMANT) free_thread(&th[tid]);
}

/* thbase */
void iop_imp_thbase_CreateThread(iop_ctx* c) {
    uint32_t p = c->r[4];       /* iop_thread_t: attr, option, entry, stacksize, priority */
    int tid = create_thread(iop_r32(p + 8), (int)iop_r32(p + 16), iop_r32(p + 12), iop_r32(p), iop_r32(p + 4), c->r[28]);
    if (iop_log || tid < 0)
        fprintf(stderr, "[iop] CreateThread(entry 0x%08X, prio %d, stack 0x%X) -> %d\n", iop_r32(p + 8),
                (int)iop_r32(p + 16), iop_r32(p + 12), tid);
    iop_ret(c, tid);
}

void iop_imp_thbase_DeleteThread(iop_ctx* c) {
    iop_thread* t = get_thread((int)c->r[4]);
    if (!t || c->r[4] == 0) { iop_ret(c, t ? KE_ILLEGAL_THID : KE_UNKNOWN_THID); return; }
    if (t->status != TS_DORMANT) { iop_ret(c, KE_NOT_DORMANT); return; }
    free_thread(t);
    iop_ret(c, KE_OK);
}

void iop_imp_thbase_StartThread(iop_ctx* c) {
    int r = start_thread((int)c->r[4], c->r[5], 0);
    if (iop_log) fprintf(stderr, "[iop] StartThread(%d, 0x%X) -> %d\n", (int)c->r[4], c->r[5], r);
    iop_ret(c, r);
    if (r == KE_OK) iop_check_preempt();
}

void iop_imp_thbase_StartThreadArgs(iop_ctx* c) {      /* (thid, args size, argp) */
    int r = start_thread((int)c->r[4], c->r[5], c->r[6]);
    iop_ret(c, r);
    if (r == KE_OK) iop_check_preempt();
}

void iop_imp_thbase_ExitThread(iop_ctx* c) {
    (void)c;
    if (!cur || in_intr) return;
    th[cur].status = TS_DORMANT;
    th[cur].exit_value = 0;
    th[cur].fiber_stale = 1;                               /* never resumed: StartThread makes a new one */
    switch_to_scheduler();
}

void iop_imp_thbase_TerminateThread(iop_ctx* c) {
    int tid = (int)c->r[4];
    iop_thread* t = get_thread(tid);
    if (!t) { iop_ret(c, KE_UNKNOWN_THID); return; }
    if (t == &th[cur] || tid == 0) { iop_ret(c, KE_ILLEGAL_THID); return; }
    if (t->status == TS_DORMANT) { iop_ret(c, KE_DORMANT); return; }
    t->status = TS_DORMANT;
    if (t->fiber) { DeleteFiber(t->fiber); t->fiber = NULL; }   /* it was stopped mid-way: start afresh */
    iop_ret(c, KE_OK);
}

void iop_imp_thbase_ChangeThreadPriority(iop_ctx* c) {
    iop_thread* t = get_thread((int)c->r[4]);
    int prio = (int)c->r[5];
    if (!t) { iop_ret(c, KE_UNKNOWN_THID); return; }
    if (prio == 0) prio = cur ? th[cur].prio : t->prio;   /* TPRI_RUN */
    if (prio < 1 || prio > 126) { iop_ret(c, KE_ILLEGAL_PRIORITY); return; }
    t->prio = prio;
    if (t == &th[cur]) t->seq = ++seq_counter;             /* goes to the back of its new level */
    iop_ret(c, KE_OK);
    iop_check_preempt();
}

void iop_imp_thbase_GetThreadId(iop_ctx* c) { iop_ret(c, in_intr ? KE_ILLEGAL_CONTEXT : cur); }

void iop_imp_thbase_SleepThread(iop_ctx* c) {
    if (in_intr || !cur) { iop_ret(c, KE_ILLEGAL_CONTEXT); return; }
    if (th[cur].wakeups > 0) { th[cur].wakeups--; iop_ret(c, KE_OK); return; }
    iop_ret(c, iop_block(IOP_WAIT_SLEEP, 0));
}

static int wakeup(int tid) {
    iop_thread* t = get_thread(tid);
    if (!t) return KE_UNKNOWN_THID;
    if (tid == 0 || t == &th[cur]) return KE_ILLEGAL_THID;
    if (t->status == TS_DORMANT) return KE_DORMANT;
    if (t->status == TS_WAIT && t->wait_type == IOP_WAIT_SLEEP) iop_wake((int)(t - th), KE_OK);
    else t->wakeups++;
    return KE_OK;
}
void iop_imp_thbase_WakeupThread(iop_ctx* c) { iop_ret(c, wakeup((int)c->r[4])); iop_check_preempt(); }
void iop_imp_thbase_iWakeupThread(iop_ctx* c) { iop_ret(c, wakeup((int)c->r[4])); }

void iop_imp_thbase_DelayThread(iop_ctx* c) {
    if (in_intr || !cur) { iop_ret(c, KE_ILLEGAL_CONTEXT); return; }
    th[cur].delay_until = iop_now() + iop_usec_to_ticks(c->r[4]);
    th[cur].delay_pass = run_pass;
    iop_ret(c, iop_block(IOP_WAIT_DELAY, 0));
}

void iop_imp_thbase_USec2SysClock(iop_ctx* c) {          /* (usec, iop_sys_clock_t* {lo, hi}) */
    uint64_t v = (uint64_t)c->r[4] * (SYSCLOCK_HZ / 1000u) / 1000u;
    iop_w32(c->r[5], (uint32_t)v);
    iop_w32(c->r[5] + 4, (uint32_t)(v >> 32));
}

void iop_imp_thbase_SysClock2USec(iop_ctx* c) {          /* (clock*, sec*, usec*) */
    uint64_t v = iop_r32(c->r[4]) | ((uint64_t)iop_r32(c->r[4] + 4) << 32);
    uint64_t us = v * 1000u / (SYSCLOCK_HZ / 1000u);
    iop_w32(c->r[5], (uint32_t)(us / 1000000u));
    iop_w32(c->r[6], (uint32_t)(us % 1000000u));
}

void iop_imp_thbase_GetSystemTime(iop_ctx* c) {
    static int64_t t0;
    if (!t0) t0 = iop_now();
    if (!qpc_freq.QuadPart) QueryPerformanceFrequency(&qpc_freq);
    uint64_t v = (uint64_t)((double)(iop_now() - t0) * SYSCLOCK_HZ / (double)qpc_freq.QuadPart);
    iop_w32(c->r[4], (uint32_t)v);
    iop_w32(c->r[4] + 4, (uint32_t)(v >> 32));
}

/* ---- semaphores (thsemap) --------------------------------------------------------------------------- */

typedef struct { int used, count, max; uint32_t attr, option; } iop_sema;
#define MAX_SEMAS 128
static iop_sema semas[MAX_SEMAS];

static iop_sema* get_sema(uint32_t id) { return id > 0 && id < MAX_SEMAS && semas[id].used ? &semas[id] : NULL; }

void iop_imp_thsemap_CreateSema(iop_ctx* c) {
    uint32_t p = c->r[4];       /* iop_sema_t: attr, option, initial, max */
    for (int i = 1; i < MAX_SEMAS; i++) {
        if (semas[i].used) continue;
        semas[i] = (iop_sema){ 1, (int)iop_r32(p + 8), (int)iop_r32(p + 12), iop_r32(p), iop_r32(p + 4) };
        if (iop_log) fprintf(stderr, "[iop] CreateSema(init %d, max %d) -> %d\n", semas[i].count, semas[i].max, i);
        iop_ret(c, i);
        return;
    }
    iop_ret(c, KE_NO_MEMORY);
}

void iop_imp_thsemap_DeleteSema(iop_ctx* c) {
    iop_sema* s = get_sema(c->r[4]);
    if (!s) { iop_ret(c, KE_UNKNOWN_SEMID); return; }
    s->used = 0;
    for (int t; (t = iop_find_waiter(IOP_WAIT_SEMA, c->r[4], 1)) != 0;) iop_wake(t, KE_WAIT_DELETE);
    iop_ret(c, KE_OK);
    iop_check_preempt();
}

static int sema_signal(uint32_t id) {
    iop_sema* s = get_sema(id);
    if (!s) return KE_UNKNOWN_SEMID;
    int t = iop_find_waiter(IOP_WAIT_SEMA, id, !(s->attr & 1));   /* SA_THPRI = 1, else FIFO */
    if (t) { iop_wake(t, KE_OK); return KE_OK; }                  /* the count passes straight to it */
    if (s->count >= s->max) return KE_SEMA_OVF;
    s->count++;
    return KE_OK;
}

void iop_imp_thsemap_SignalSema(iop_ctx* c) { iop_ret(c, sema_signal(c->r[4])); iop_check_preempt(); }
void iop_imp_thsemap_iSignalSema(iop_ctx* c) { iop_ret(c, sema_signal(c->r[4])); }

void iop_imp_thsemap_WaitSema(iop_ctx* c) {
    iop_sema* s = get_sema(c->r[4]);
    if (!s) { iop_ret(c, KE_UNKNOWN_SEMID); return; }
    if (s->count > 0) { s->count--; iop_ret(c, KE_OK); return; }
    iop_ret(c, iop_block(IOP_WAIT_SEMA, c->r[4]));
}

void iop_imp_thsemap_PollSema(iop_ctx* c) {
    iop_sema* s = get_sema(c->r[4]);
    if (!s) { iop_ret(c, KE_UNKNOWN_SEMID); return; }
    if (s->count > 0) { s->count--; iop_ret(c, KE_OK); return; }
    iop_ret(c, KE_SEMA_ZERO);
}
void iop_imp_thsemap_iPollSema(iop_ctx* c) { iop_imp_thsemap_PollSema(c); }

/* ---- event flags (thevent) ---------------------------------------------------------------------------- */

enum { WEF_AND = 0, WEF_OR = 1, WEF_CLEAR = 0x10, WEF_CLEARALL = 0x20, EA_MULTI = 2 };
typedef struct { int used; uint32_t bits, attr, option; } iop_evf;
#define MAX_EVFS 64
static iop_evf evfs[MAX_EVFS];

static iop_evf* get_evf(uint32_t id) { return id > 0 && id < MAX_EVFS && evfs[id].used ? &evfs[id] : NULL; }

static int evf_match(uint32_t bits, uint32_t want, uint32_t mode) {
    return (mode & WEF_OR) ? (bits & want) != 0 : (bits & want) == want;
}

static void evf_consume(iop_evf* e, uint32_t want, uint32_t mode) {
    if (mode & WEF_CLEARALL) e->bits = 0;
    else if (mode & WEF_CLEAR) e->bits &= ~want;
}

void iop_imp_thevent_CreateEventFlag(iop_ctx* c) {
    uint32_t p = c->r[4];       /* iop_event_t: attr, option, bits */
    for (int i = 1; i < MAX_EVFS; i++) {
        if (evfs[i].used) continue;
        evfs[i] = (iop_evf){ 1, iop_r32(p + 8), iop_r32(p), iop_r32(p + 4) };
        iop_ret(c, i);
        return;
    }
    iop_ret(c, KE_NO_MEMORY);
}

void iop_imp_thevent_DeleteEventFlag(iop_ctx* c) {
    iop_evf* e = get_evf(c->r[4]);
    if (!e) { iop_ret(c, KE_UNKNOWN_EVFID); return; }
    e->used = 0;
    for (int t; (t = iop_find_waiter(IOP_WAIT_EVENT, c->r[4], 1)) != 0;) iop_wake(t, KE_WAIT_DELETE);
    iop_ret(c, KE_OK);
    iop_check_preempt();
}

static int evf_set(uint32_t id, uint32_t bits) {
    iop_evf* e = get_evf(id);
    if (!e) return KE_UNKNOWN_EVFID;
    e->bits |= bits;
    int64_t last = INT64_MIN;
    for (;;) {                                         /* release waiters in wait order */
        int t = 0;
        for (int i = 1; i < MAX_THREADS; i++) {
            iop_thread* w = &th[i];
            if (w->status != TS_WAIT || w->wait_type != IOP_WAIT_EVENT || w->wait_obj != id || w->wait_seq <= last) continue;
            if (!t || w->wait_seq < th[t].wait_seq) t = i;
        }
        if (!t) break;
        last = th[t].wait_seq;
        uint32_t want = th[t].wdata[0], mode = th[t].wdata[1], res = th[t].wdata[2];
        if (!evf_match(e->bits, want, mode)) continue;
        if (res) iop_w32(res, e->bits);
        evf_consume(e, want, mode);
        iop_wake(t, KE_OK);
    }
    return KE_OK;
}

void iop_imp_thevent_SetEventFlag(iop_ctx* c) { iop_ret(c, evf_set(c->r[4], c->r[5])); iop_check_preempt(); }
void iop_imp_thevent_iSetEventFlag(iop_ctx* c) { iop_ret(c, evf_set(c->r[4], c->r[5])); }

void iop_imp_thevent_ClearEventFlag(iop_ctx* c) {
    iop_evf* e = get_evf(c->r[4]);
    if (!e) { iop_ret(c, KE_UNKNOWN_EVFID); return; }
    e->bits &= c->r[5];
    iop_ret(c, KE_OK);
}
void iop_imp_thevent_iClearEventFlag(iop_ctx* c) { iop_imp_thevent_ClearEventFlag(c); }

static int evf_poll(uint32_t id, uint32_t want, uint32_t mode, uint32_t res) {
    iop_evf* e = get_evf(id);
    if (!e) return KE_UNKNOWN_EVFID;
    if (!want) return KE_EVF_ILPAT;
    if (!evf_match(e->bits, want, mode)) return KE_EVF_COND;
    if (res) iop_w32(res, e->bits);
    evf_consume(e, want, mode);
    return KE_OK;
}

void iop_imp_thevent_WaitEventFlag(iop_ctx* c) {      /* (id, bits, mode, resbits*) */
    int r = evf_poll(c->r[4], c->r[5], c->r[6], c->r[7]);
    if (r != KE_EVF_COND) { iop_ret(c, r); return; }
    iop_evf* e = get_evf(c->r[4]);
    if (!(e->attr & EA_MULTI) && iop_find_waiter(IOP_WAIT_EVENT, c->r[4], 1)) { iop_ret(c, KE_EVF_MULTI); return; }
    if (in_intr || !cur) { iop_ret(c, KE_ILLEGAL_CONTEXT); return; }
    th[cur].wdata[0] = c->r[5]; th[cur].wdata[1] = c->r[6]; th[cur].wdata[2] = c->r[7];
    iop_ret(c, iop_block(IOP_WAIT_EVENT, c->r[4]));
}

void iop_imp_thevent_PollEventFlag(iop_ctx* c) { iop_ret(c, evf_poll(c->r[4], c->r[5], c->r[6], c->r[7])); }

/* ---- interrupts (intrman) ------------------------------------------------------------------------------ */

uint32_t iop_intr_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t gp) {
    if (in_intr) { fprintf(stderr, "[iop] nested interrupt handler 0x%08X dropped\n", fn); return 0; }
    if (!inited) iop_init();
    in_intr = 1;
    memset(&intr_ctx, 0, sizeof intr_ctx);
    intr_ctx.r[4] = a0;
    intr_ctx.r[5] = a1;
    intr_ctx.r[28] = gp;
    intr_ctx.r[29] = intr_stack_top;
    iop_call(&intr_ctx, fn);
    in_intr = 0;
    return intr_ctx.r[2];
}

/* CpuSuspendIntr(int* state): interrupts off for the calling context; *state remembers the old
 * setting for CpuResumeIntr. Returns KE_CPUDI if they were already off. */
void iop_imp_intrman_CpuSuspendIntr(iop_ctx* c) {
    int was_off = in_intr || (cur && th[cur].intr_disabled);
    if (c->r[4]) iop_w32(c->r[4], was_off ? 0u : 1u);
    if (cur && !in_intr) th[cur].intr_disabled = 1;
    iop_ret(c, was_off ? KE_CPUDI : KE_OK);
}

void iop_imp_intrman_CpuResumeIntr(iop_ctx* c) {
    if (cur && !in_intr) th[cur].intr_disabled = !(c->r[4] & 1);
    iop_ret(c, KE_OK);
}

void iop_imp_intrman_QueryIntrContext(iop_ctx* c) { iop_ret(c, in_intr); }

/* Only libsd uses these (it is replaced by the native sd backend); accepted and ignored. */
void iop_imp_intrman_RegisterIntrHandler(iop_ctx* c) { iop_ret(c, KE_OK); }
void iop_imp_intrman_ReleaseIntrHandler(iop_ctx* c) { iop_ret(c, KE_OK); }
void iop_imp_intrman_EnableIntr(iop_ctx* c) { iop_ret(c, KE_OK); }
void iop_imp_intrman_DisableIntr(iop_ctx* c) { iop_ret(c, KE_OK); }

/* ---- hard timers (timrman) ------------------------------------------------------------------------------ */

typedef struct {
    int used, active;
    uint32_t source, prescale, compare, handler, arg, gp;
    int64_t next_due;
} hard_timer;
static hard_timer timers[4];

static uint32_t source_hz(uint32_t source) {
    switch (source) {
    case 2: return 13500000u;            /* TC_PIXEL */
    case 3: return 15734u;               /* TC_HLINE (NTSC) */
    default: return SYSCLOCK_HZ;         /* TC_SYSCLOCK */
    }
}

static int64_t timer_period(const hard_timer* t, uint32_t compare) {
    int64_t p = sysclock_to_ticks((uint64_t)compare * (t->prescale ? t->prescale : 1), source_hz(t->source));
    return p > 0 ? p : 1;
}

static hard_timer* get_timer(uint32_t id) { return id >= 1 && id <= 4 && timers[id - 1].used ? &timers[id - 1] : NULL; }

void iop_imp_timrman_AllocHardTimer(iop_ctx* c) {          /* (source, size, prescale) */
    for (int i = 0; i < 4; i++) {
        if (timers[i].used) continue;
        timers[i] = (hard_timer){ 1, 0, c->r[4], c->r[6], 0, 0, 0, 0, 0 };
        iop_ret(c, i + 1);
        return;
    }
    iop_ret(c, KE_NO_TIMER);
}

void iop_imp_timrman_FreeHardTimer(iop_ctx* c) {
    hard_timer* t = get_timer(c->r[4]);
    if (!t) { iop_ret(c, KE_ILLEGAL_TIMERID); return; }
    t->used = t->active = 0;
    iop_ret(c, KE_OK);
}

void iop_imp_timrman_SetTimerHandler(iop_ctx* c) {         /* (id, compare, handler, arg) */
    hard_timer* t = get_timer(c->r[4]);
    if (!t) { iop_ret(c, KE_ILLEGAL_TIMERID); return; }
    t->compare = c->r[5];
    t->handler = c->r[6];
    t->arg = c->r[7];
    t->gp = c->r[28];
    iop_ret(c, KE_OK);
}

void iop_imp_timrman_SetupHardTimer(iop_ctx* c) {          /* (id, source, mode, prescale) */
    hard_timer* t = get_timer(c->r[4]);
    if (!t) { iop_ret(c, KE_ILLEGAL_TIMERID); return; }
    t->source = c->r[5];
    t->prescale = c->r[7];
    iop_ret(c, KE_OK);
}

void iop_imp_timrman_StartHardTimer(iop_ctx* c) {
    hard_timer* t = get_timer(c->r[4]);
    if (!t) { iop_ret(c, KE_ILLEGAL_TIMERID); return; }
    t->active = 1;
    t->next_due = iop_now() + timer_period(t, t->compare);
    if (iop_log) fprintf(stderr, "[iop] hard timer %u started: compare %u, handler 0x%08X\n", c->r[4], t->compare, t->handler);
    iop_ret(c, KE_OK);
}

void iop_imp_timrman_StopHardTimer(iop_ctx* c) {
    hard_timer* t = get_timer(c->r[4]);
    if (!t) { iop_ret(c, KE_ILLEGAL_TIMERID); return; }
    t->active = 0;
    iop_ret(c, KE_OK);
}

static uint64_t timer_ticks_total;
uint64_t iop_timer_ticks(void) { return timer_ticks_total; }
/* How late timer interrupts are delivered (status line): the IOP only runs at EE safe points, so a
 * long stretch of EE code without one delays the 989snd tick (sequenced notes, stream refills). */
static int64_t  timer_late_max;
static uint64_t timer_late_count;           /* deliveries more than TIMER_LATE_USEC late */
#define TIMER_LATE_USEC 8000u

/* Timers keep real time: when the EE reached no safe point for a while (a long frame), the missed
 * interrupts are delivered back to back, so the 989snd tick averages 240 Hz. Only a stall of more
 * than a second (debugger, window drag) is dropped instead of replayed. */
static void timers_service(int64_t now) {
    for (int i = 0; i < 4; i++) {
        hard_timer* t = &timers[i];
        if (t->used && t->active && now - t->next_due > iop_usec_to_ticks(1000000u))
            t->next_due = now;
        while (t->used && t->active && t->handler && now >= t->next_due) {
            int64_t late = now - t->next_due;
            if (late > timer_late_max) timer_late_max = late;
            if (late > iop_usec_to_ticks(TIMER_LATE_USEC)) timer_late_count++;
            uint32_t next = iop_intr_call(t->handler, t->arg, 0, t->gp);
            timer_ticks_total++;
            if (!next) { t->active = 0; break; }           /* handler returned 0: timer stops */
            t->compare = next;
            t->next_due += timer_period(t, next);
        }
    }
}

/* ---- scheduler ------------------------------------------------------------------------------------------ */

static void deliver_interrupts(void) {
    if (in_intr) return;
    int64_t now = iop_now();
    timers_service(now);
    iop_cd_service(now);
    iop_sif_service();
    iop_sd_service();
    for (int i = 1; i < MAX_THREADS; i++) {               /* DelayThread: wake at the next pass after expiry */
        iop_thread* t = &th[i];
        if (t->status == TS_WAIT && t->wait_type == IOP_WAIT_DELAY && now >= t->delay_until && t->delay_pass != run_pass)
            iop_wake(i, KE_OK);
    }
}

static int64_t busy_ticks;                 /* host time spent running the IOP (status line) */
static volatile int64_t last_run_end;      /* read by the mixer thread (sd.c holds the voices when old) */

int64_t iop_last_run_time(void) { return last_run_end; }

void iop_run(void) {
    if (in_run || in_intr) return;
    in_run = 1;
    run_pass++;
    sched_fiber = GetCurrentFiber();
    int64_t t0 = iop_now();
    run_deadline = t0 + iop_usec_to_ticks(SLICE_USEC);
    for (;;) {
        iop_poll_flag = 0;
        deliver_interrupts();
        int next = pick_next();
        if (!next || iop_now() > run_deadline) break;
        cur = next;
        th[next].status = TS_RUNNING;
        SwitchToFiber(th[next].fiber);
        cur = 0;
    }
    int64_t t1 = iop_now();
    busy_ticks += t1 - t0;
    last_run_end = t1;
    in_run = 0;
}

/* Every 10 s while the recompiled modules run: one line showing the IOP is alive. */
static void status_line(void) {
    static int64_t last, last_busy;
    static uint64_t last_ticks;
    int64_t now = iop_now();
    if (!last) { last = now; last_ticks = timer_ticks_total; last_busy = busy_ticks; return; }
    if (now - last < iop_usec_to_ticks(10000000u)) return;
    double secs = (double)(now - last) / (double)qpc_freq.QuadPart;
    double rate = (double)(timer_ticks_total - last_ticks) / secs;
    double load = 100.0 * (double)(busy_ticks - last_busy) / (double)(now - last);
    last = now;
    last_ticks = timer_ticks_total;
    last_busy = busy_ticks;
    char sif[512];
    iop_sif_report(sif, sizeof sif);
    int ready = 0, waiting = 0;
    for (int i = 1; i < MAX_THREADS; i++) { ready += th[i].status == TS_READY; waiting += th[i].status == TS_WAIT; }
    fprintf(stderr, "[iop] status: hard timer handler %llu calls (%.1f/s over the last %.0f s, %llu of them more than "
                    "%u ms late, at most %.1f ms), IOP code %.2f%% of the time, threads %d waiting/%d ready, free IOP "
                    "memory 0x%X; %s\n",
            (unsigned long long)timer_ticks_total, rate, secs, (unsigned long long)timer_late_count,
            TIMER_LATE_USEC / 1000u, (double)timer_late_max * 1000.0 / (double)qpc_freq.QuadPart, load, waiting,
            ready, iop_total_free(), sif);
    timer_late_count = 0;
    timer_late_max = 0;
    for (int i = 1; i < MAX_THREADS; i++) {                /* threads that busy-waited since the last line */
        iop_thread* t = &th[i];
        if (t->status == TS_FREE || t->slices == t->slices_seen) continue;
        fprintf(stderr, "[iop]   thread %d (entry 0x%05X, prio %d) used up %u time slices without blocking\n",
                i, t->entry, t->prio, t->slices - t->slices_seen);
        t->slices_seen = t->slices;
    }
}

void iop_service(void) {
    if (!inited) return;
    iop_run();
    if (timer_ticks_total) status_line();
}

/* Loop heads of recompiled code (IOP_POLL): pending interrupts, preemption, end of the time slice. */
void iop_poll(iop_ctx* c) {
    (void)c;
    iop_poll_flag = 0;
    if (in_intr || !cur) return;
    if (!th[cur].intr_disabled) deliver_interrupts();
    if (iop_now() > run_deadline) { th[cur].slices++; yield_running(); return; }
    iop_check_preempt();
}

uint32_t iop_guest_call(iop_ctx* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t ra = c->r[31], sp = c->r[29];
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    c->r[29] = sp - 0x20;                                   /* argument home area for the callee */
    iop_call(c, fn);
    c->r[29] = sp;
    c->r[31] = ra;
    return c->r[2];
}
