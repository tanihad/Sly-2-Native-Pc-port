/* Native IOP kernel tests: sysmem, thread scheduling (priority preemption, sleep/wakeup, delay),
 * semaphores, event flags, interrupt context (hard timers, CD, SIF DMA and libsd completion
 * callbacks), the RPC server loop, the time slice of busy-waiting threads, and sysclib/printf.
 *
 * "Guest" code is written here as C functions with the recompiled-code signature, registered
 * through iop_register_recompiled() at fake IOP addresses, and calls the native libraries exactly
 * as recompiled code does (arguments in the register context and on the IOP stack). Links
 * iop_kernel.c, iop_libs.c, iop_sif.c and iop_loader.c; the EE side, disc and sound backend are
 * fakes. */
#include "iop_internal.h"
#include "sd.h"

#include <stdlib.h>
#include <windows.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* native library entry points (normally declared by the generated iop_recomp.h) */
#define NATIVES(X) X(sysmem_AllocSysMemory) X(sysmem_FreeSysMemory) X(sysmem_QueryMaxFreeMemSize) \
    X(thbase_CreateThread) X(thbase_StartThread) X(thbase_DeleteThread) X(thbase_GetThreadId) \
    X(thbase_SleepThread) X(thbase_WakeupThread) X(thbase_iWakeupThread) X(thbase_DelayThread) \
    X(thbase_ChangeThreadPriority) X(thbase_TerminateThread) X(thbase_USec2SysClock) \
    X(thsemap_CreateSema) X(thsemap_DeleteSema) X(thsemap_SignalSema) X(thsemap_iSignalSema) X(thsemap_WaitSema) \
    X(thsemap_PollSema) X(thevent_CreateEventFlag) X(thevent_SetEventFlag) X(thevent_ClearEventFlag) \
    X(thevent_WaitEventFlag) X(thevent_PollEventFlag) X(intrman_CpuSuspendIntr) X(intrman_CpuResumeIntr) \
    X(intrman_QueryIntrContext) X(timrman_AllocHardTimer) X(timrman_SetTimerHandler) X(timrman_SetupHardTimer) \
    X(timrman_StartHardTimer) X(timrman_StopHardTimer) X(timrman_FreeHardTimer) X(sifcmd_sceSifSetRpcQueue) \
    X(sifcmd_sceSifRegisterRpc) X(sifcmd_sceSifRpcLoop) X(sifman_sceSifSetDmaIntr) X(sifman_sceSifDmaStat) \
    X(cdvdman_sceCdRead) X(cdvdman_sceCdSync) X(cdvdman_sceCdCallback) X(cdvdman_sceCdSearchFile) \
    X(libsd_sceSdSetTransIntrHandler) \
    X(sysclib_memcpy) X(sysclib_memset) X(sysclib_strncmp) X(sysclib_strtol) X(sysclib_wmemcopy) X(stdio_printf)
#define DECL(n) void iop_imp_##n(iop_ctx* c);
NATIVES(DECL)

static uint32_t K(void (*fn)(iop_ctx*), iop_ctx* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    fn(c);
    return c->r[2];
}
#define CALL(name, c, ...) K(iop_imp_##name, c, __VA_ARGS__)

/* ---- fakes for the EE side, the disc and the sound backend -------------------------------------- */

uint8_t* iop_mem(uint32_t a) { a &= 0x1FFFFFFFu; return a < IOP_RAM_SIZE ? iop_ram + a : NULL; }
static iop_rpc_req last_reply_req;
static uint32_t last_reply_addr, replies;
void sif_rpc_reply(const iop_rpc_req* r, uint32_t a) { last_reply_req = *r; last_reply_addr = a; replies++; }
static uint32_t dma_ee, dma_iop, dma_size;
void sif_iop_to_ee(uint32_t ee, uint32_t iop, uint32_t size) { dma_ee = ee; dma_iop = iop; dma_size = size; }
int sif_ee_to_iop(uint32_t iop, uint32_t ee, uint32_t size) { (void)iop; (void)ee; (void)size; return 1; }
uint32_t ps2_vblank_count(void) { return 0; }
/* The disc: an ISO 9660 volume descriptor at 16, root directory at 20 holding SYSTEM.CNF;1 and a
 * directory DATA (21) holding SND.BNK;1; every other sector is a byte pattern. */
static void dir_record(uint8_t* sec, uint32_t* p, const char* name, uint32_t lsn, uint32_t size, int is_dir) {
    uint8_t n = (uint8_t)strlen(name), len = (uint8_t)((33 + n + 1) & ~1);
    memset(sec + *p, 0, len);
    sec[*p] = len;
    memcpy(sec + *p + 2, &lsn, 4);
    memcpy(sec + *p + 10, &size, 4);
    sec[*p + 18] = 104;                               /* 2004 */
    sec[*p + 25] = is_dir ? 2 : 0;
    sec[*p + 32] = n;
    memcpy(sec + *p + 33, name, n);
    *p += len;
}
int cdvd_read_host(uint32_t first, uint32_t count, void* dst) {
    uint8_t* d = dst;
    uint32_t lbn = first;
    for (uint32_t s = 0; s < count; s++, lbn++, d += 2048) {
        memset(d, 0, 2048);
        uint32_t p = 0;
        if (lbn == 16) {
            d[0] = 1; memcpy(d + 1, "CD001", 5);
            uint32_t root = 20, size = 2048;
            memcpy(d + 156 + 2, &root, 4); memcpy(d + 156 + 10, &size, 4);
        } else if (lbn == 20) {
            dir_record(d, &p, "\0", 20, 2048, 1);
            dir_record(d, &p, "\1", 20, 2048, 1);
            dir_record(d, &p, "DATA", 21, 2048, 1);
            dir_record(d, &p, "SYSTEM.CNF;1", 30, 100, 0);
        } else if (lbn == 21) {
            dir_record(d, &p, "SND.BNK;1", 40, 5000, 0);
        } else {
            for (uint32_t i = 0; i < 2048u; i++) d[i] = (uint8_t)(first + s * 2048u + i);
        }
    }
    return 1;
}
int sceSdInit(int flag) { (void)flag; return 0; }
void sceSdSetParam(uint16_t e, uint16_t v) { (void)e; (void)v; }
uint16_t sceSdGetParam(uint16_t e) { (void)e; return 0; }
void sceSdSetSwitch(uint16_t e, uint32_t v) { (void)e; (void)v; }
uint32_t sceSdGetSwitch(uint16_t e) { (void)e; return 0; }
void sceSdSetAddr(uint16_t e, uint32_t v) { (void)e; (void)v; }
uint32_t sceSdGetAddr(uint16_t e) { (void)e; return 0; }
void sceSdSetCoreAttr(uint16_t e, uint16_t v) { (void)e; (void)v; }
uint16_t sceSdNote2Pitch(uint16_t a, uint16_t b, uint16_t c, int16_t d) { (void)a; (void)b; (void)c; (void)d; return 0; }
int sceSdVoiceTrans(int16_t ch, uint16_t m, uint32_t i, uint32_t s, uint32_t n) { (void)ch; (void)m; (void)i; (void)s; return (int)n; }
uint32_t sceSdVoiceTransStatus(int16_t ch, int16_t f) { (void)ch; (void)f; return 1; }
int sceSdBlockTrans(int16_t ch, uint16_t m, uint32_t a, uint32_t s, uint32_t st) { (void)ch; (void)m; (void)a; (void)s; (void)st; return 0; }
uint32_t sceSdBlockTransStatus(int16_t ch, int16_t f) { (void)ch; (void)f; return 0; }
int sceSdSetEffectAttr(int core, const sd_effect_attr* a) { (void)core; (void)a; return 0; }
int sceSdClearEffectWorkArea(int core, int ch, int m) { (void)core; (void)ch; (void)m; return 0; }
void audio_init(void) {}

/* ---- guest helpers ------------------------------------------------------------------------------- */

static void put_str(uint32_t addr, const char* s) { memcpy(iop_ram + addr, s, strlen(s) + 1); }

static char order[256];                          /* execution order of the threads under test */
static void mark(const char* s) { strncat(order, s, sizeof order - strlen(order) - 1); }

static uint32_t scratch;                         /* IOP RAM for parameter blocks */
static int new_thread(uint32_t entry, int prio) {
    iop_ctx c = { 0 };
    iop_w32(scratch + 0, 0x02000000u);           /* attr TH_C */
    iop_w32(scratch + 4, 0);
    iop_w32(scratch + 8, entry);
    iop_w32(scratch + 12, 0x800);
    iop_w32(scratch + 16, (uint32_t)prio);
    return (int)CALL(thbase_CreateThread, &c, scratch, 0, 0, 0);
}
static int new_sema(uint32_t attr, int init, int max) {
    iop_ctx c = { 0 };
    iop_w32(scratch + 0x40, attr); iop_w32(scratch + 0x44, 0); iop_w32(scratch + 0x48, (uint32_t)init); iop_w32(scratch + 0x4C, (uint32_t)max);
    return (int)CALL(thsemap_CreateSema, &c, scratch + 0x40, 0, 0, 0);
}
static void start(int tid, uint32_t arg) { iop_ctx c = { 0 }; CALL(thbase_StartThread, &c, (uint32_t)tid, arg, 0, 0); }
static void run_until(volatile int* done, int ms) {
    for (int i = 0; i < ms && !*done; i++) { iop_run(); if (!*done) Sleep(1); }
}

/* guest functions, at fake IOP addresses */
enum { G_PRIO_A = 0x00100000, G_PRIO_B = 0x00100010, G_WAITER = 0x00100020, G_SLEEPER = 0x00100030,
       G_DELAY = 0x00100040, G_TIMER = 0x00100050, G_EVF = 0x00100060, G_RPC_THREAD = 0x00100070,
       G_RPC_FUNC = 0x00100080, G_CD = 0x00100090, G_CD_CB = 0x001000A0, G_DMA_CB = 0x001000B0,
       G_SD_CB = 0x001000C0, G_SPIN = 0x001000D0, G_TIMER_WAITER = 0x001000E0 };

static int sema_a, ab_done;
static void prio_a(iop_ctx* c) {
    mark("A1 ");
    int b = new_thread(G_PRIO_B, 40);
    CALL(thbase_StartThread, c, (uint32_t)b, 0, 0, 0);          /* B outranks A: runs at once */
    mark("A2 ");
    CALL(thsemap_WaitSema, c, (uint32_t)sema_a, 0, 0, 0);
    mark("A3");
    ab_done = 1;
}
static void prio_b(iop_ctx* c) { (void)c; mark("B "); }

static int sema_w;
static void waiter(iop_ctx* c) {                 /* arg = name character */
    uint32_t name = c->r[4];
    int r = (int)CALL(thsemap_WaitSema, c, (uint32_t)sema_w, 0, 0, 0);
    char s[3] = { (char)name, r == KE_OK ? ' ' : '!', 0 };
    mark(s);
}

static int sleeps_done;
static void sleeper(iop_ctx* c) {
    CALL(thbase_SleepThread, c, 0, 0, 0, 0);    /* banked wakeups: these two return at once */
    CALL(thbase_SleepThread, c, 0, 0, 0, 0);
    mark("S2 ");
    CALL(thbase_SleepThread, c, 0, 0, 0, 0);    /* blocks */
    mark("S3");
    sleeps_done = 1;
}

static volatile int delay_done;
static void delayer(iop_ctx* c) { CALL(thbase_DelayThread, c, 20000, 0, 0, 0); delay_done = 1; }

static uint32_t timer_calls, timer_limit, timer_in_intr = 1, timer_wake_sema;
static void timer_handler(iop_ctx* c) {          /* returns the next compare value (0 = stop) */
    timer_calls++;
    uint32_t arg = c->r[4];
    iop_ctx q = *c;
    if (!CALL(intrman_QueryIntrContext, &q, 0, 0, 0, 0)) timer_in_intr = 0;
    if (timer_wake_sema && timer_calls == 3) CALL(thsemap_iSignalSema, &q, timer_wake_sema, 0, 0, 0);
    c->r[2] = timer_calls >= timer_limit ? 0 : arg;
}
static volatile int timer_waiter_done;
static void timer_waiter(iop_ctx* c) { timer_waiter_done = (int)CALL(thsemap_WaitSema, c, timer_wake_sema, 0, 0, 0) == KE_OK; }

static int evf;
static volatile int evf_done;
static uint32_t evf_res;
static void evf_waiter(iop_ctx* c) {
    int r = (int)CALL(thevent_WaitEventFlag, c, (uint32_t)evf, 0x3, 0x10 /* AND | CLEAR */, scratch + 0x80);
    evf_res = iop_r32(scratch + 0x80);
    evf_done = r == KE_OK ? 1 : -1;
}

static uint32_t rpc_qd, rpc_sd, rpc_buf, rpc_out;
static void rpc_thread(iop_ctx* c) {
    CALL(sifcmd_sceSifSetRpcQueue, c, rpc_qd, (uint32_t)CALL(thbase_GetThreadId, c, 0, 0, 0, 0), 0, 0);
    c->r[29] -= 32;                               /* 7 arguments: cfunc, cbuff, qd on the stack */
    iop_w32(c->r[29] + 16, 0); iop_w32(c->r[29] + 20, 0); iop_w32(c->r[29] + 24, rpc_qd);
    CALL(sifcmd_sceSifRegisterRpc, c, rpc_sd, 0x1234, G_RPC_FUNC, rpc_buf);
    c->r[29] += 32;
    CALL(sifcmd_sceSifRpcLoop, c, rpc_qd, 0, 0, 0);
}
static void rpc_func(iop_ctx* c) {               /* (fno, buff, size) -> reply buffer */
    iop_w32(rpc_out, c->r[4] * 2 + iop_r32(c->r[5]));
    c->r[2] = rpc_out;
}

static uint32_t cd_buf, cd_cb_calls, cd_cb_reason, cd_cb_intr;
static volatile int cd_done;
static int cd_busy_after_read;
static void cd_thread(iop_ctx* c) {
    CALL(cdvdman_sceCdCallback, c, G_CD_CB, 0, 0, 0);
    int r = (int)CALL(cdvdman_sceCdRead, c, 100, 2, cd_buf, 0);
    cd_busy_after_read = (int)CALL(cdvdman_sceCdSync, c, 1, 0, 0, 0);
    CALL(cdvdman_sceCdSync, c, 0, 0, 0, 0);
    cd_done = r == 1 ? 1 : -1;
}
static void cd_cb(iop_ctx* c) {
    cd_cb_calls++;
    cd_cb_reason = c->r[4];
    iop_ctx q = *c;
    cd_cb_intr = CALL(intrman_QueryIntrContext, &q, 0, 0, 0, 0);
}

static uint32_t dma_cb_arg, dma_cb_calls;
static void dma_cb(iop_ctx* c) { dma_cb_calls++; dma_cb_arg = c->r[4]; }
static uint32_t sd_cb_ch, sd_cb_arg, sd_cb_calls;
static void sd_cb(iop_ctx* c) { sd_cb_calls++; sd_cb_ch = c->r[4]; sd_cb_arg = c->r[5]; }

static volatile int spin_release, spin_done, spin_polls;
static void spinner(iop_ctx* c) {                /* a busy-wait loop, as recompiled code has it */
    while (!spin_release) { iop_poll_flag = 1; spin_polls++; IOP_POLL(c); }
    spin_done = 1;
}

static const iop_func_entry funcs[] = {
    { G_PRIO_A, prio_a }, { G_PRIO_B, prio_b }, { G_WAITER, waiter }, { G_SLEEPER, sleeper },
    { G_DELAY, delayer }, { G_TIMER, timer_handler }, { G_EVF, evf_waiter }, { G_RPC_THREAD, rpc_thread },
    { G_RPC_FUNC, rpc_func }, { G_CD, cd_thread }, { G_CD_CB, cd_cb }, { G_DMA_CB, dma_cb },
    { G_SD_CB, sd_cb }, { G_SPIN, spinner }, { G_TIMER_WAITER, timer_waiter },
};
static const iop_module_desc fake_mod[] = { { "fake", 0x00030000u, 0x1000u, 0x1000u, 0x1000u, 0, 0, 0x30000u, 0, 0, 0, NULL } };

/* ---- tests -------------------------------------------------------------------------------------- */

static void test_sysmem(void) {
    printf("sysmem\n");
    iop_ctx c = { 0 };
    CHECK(iop_alloc_at(0x30000, 0x100) == 0, "module area not reserved");
    uint32_t a = CALL(sysmem_AllocSysMemory, &c, 0, 0x123, 0, 0);
    CHECK(a >= 0x20000 && (a & 0xFF) == 0, "ALLOC_FIRST -> 0x%X", a);
    uint32_t l = CALL(sysmem_AllocSysMemory, &c, 1, 0x100, 0, 0);
    CHECK(l > a && l + 0x100 <= IOP_RAM_SIZE, "ALLOC_LAST -> 0x%X", l);
    uint32_t fixed = CALL(sysmem_AllocSysMemory, &c, 2, 0x100, 0x150000, 0);
    CHECK(fixed == 0x150000, "ALLOC_ADDRESS -> 0x%X", fixed);
    CHECK(CALL(sysmem_AllocSysMemory, &c, 2, 0x100, 0x150080, 0) == 0, "overlapping ALLOC_ADDRESS accepted");
    uint32_t maxfree = CALL(sysmem_QueryMaxFreeMemSize, &c, 0, 0, 0, 0);
    CHECK(maxfree > 0 && maxfree < IOP_RAM_SIZE, "QueryMaxFreeMemSize 0x%X (must look like a 2 MB machine)", maxfree);
    CHECK(CALL(sysmem_FreeSysMemory, &c, fixed, 0, 0, 0) == 0, "free");
    CHECK(CALL(sysmem_FreeSysMemory, &c, fixed, 0, 0, 0) != 0, "double free accepted");
    CHECK(CALL(sysmem_AllocSysMemory, &c, 2, 0x100, 0x150080, 0) == 0x150000, "freed range not reusable");
    iop_free(0x150000); iop_free(a); iop_free(l);
}

static void test_priority(void) {
    printf("thread priorities and semaphores\n");
    order[0] = 0;
    sema_a = new_sema(1, 0, 1);
    start(new_thread(G_PRIO_A, 50), 0);
    iop_run();
    CHECK(strcmp(order, "A1 B A2 ") == 0, "order \"%s\", want \"A1 B A2 \"", order);
    iop_ctx c = { 0 };
    CALL(thsemap_SignalSema, &c, (uint32_t)sema_a, 0, 0, 0);    /* from outside: A becomes ready */
    iop_run();
    CHECK(ab_done && strcmp(order, "A1 B A2 A3") == 0, "after signal: \"%s\"", order);

    /* wake order: by priority for SA_THPRI, by arrival for FIFO */
    for (int fifo = 0; fifo < 2; fifo++) {
        order[0] = 0;
        sema_w = new_sema(fifo ? 0 : 1, 0, 3);
        start(new_thread(G_WAITER, 30), 'x');        /* they start waiting in the order x y z */
        iop_run();
        start(new_thread(G_WAITER, 20), 'y');
        iop_run();
        start(new_thread(G_WAITER, 25), 'z');
        iop_run();
        for (int k = 0; k < 3; k++) {                /* one at a time: each signal releases one waiter */
            CALL(thsemap_SignalSema, &c, (uint32_t)sema_w, 0, 0, 0);
            iop_run();
        }
        const char* want = fifo ? "x y z " : "y z x ";
        CHECK(strcmp(order, want) == 0, "%s wake order \"%s\", want \"%s\"", fifo ? "FIFO" : "priority", order, want);
        CHECK(CALL(thsemap_PollSema, &c, (uint32_t)sema_w, 0, 0, 0) == (uint32_t)KE_SEMA_ZERO, "count left over");
    }
    /* signalling past the maximum fails; deleting releases waiters with KE_WAIT_DELETE */
    int s = new_sema(1, 1, 1);
    CHECK((int)CALL(thsemap_SignalSema, &c, (uint32_t)s, 0, 0, 0) == KE_SEMA_OVF, "SignalSema past max");
    order[0] = 0;
    sema_w = new_sema(1, 0, 1);
    start(new_thread(G_WAITER, 30), 'd');
    iop_run();
    CALL(thsemap_DeleteSema, &c, (uint32_t)sema_w, 0, 0, 0);
    iop_run();
    CHECK(strcmp(order, "d!") == 0, "DeleteSema released waiter: \"%s\"", order);
}

static void test_sleep_delay(void) {
    printf("sleep/wakeup and DelayThread\n");
    order[0] = 0;
    int t = new_thread(G_SLEEPER, 30);
    iop_ctx c = { 0 };
    start(t, 0);
    CALL(thbase_WakeupThread, &c, (uint32_t)t, 0, 0, 0);    /* before it runs: banked */
    CALL(thbase_WakeupThread, &c, (uint32_t)t, 0, 0, 0);
    iop_run();
    CHECK(strcmp(order, "S2 ") == 0 && !sleeps_done, "banked wakeups: \"%s\"", order);
    CALL(thbase_WakeupThread, &c, (uint32_t)t, 0, 0, 0);
    iop_run();
    CHECK(sleeps_done && strcmp(order, "S2 S3") == 0, "wakeup of a sleeping thread: \"%s\"", order);

    int64_t t0 = iop_now();
    start(new_thread(G_DELAY, 30), 0);
    run_until(&delay_done, 1000);
    double ms = (double)(iop_now() - t0) * 1000.0 / (double)iop_usec_to_ticks(1000000u);
    CHECK(delay_done && ms >= 19.5 && ms < 250, "DelayThread(20 ms) took %.1f ms", ms);

    CALL(thbase_USec2SysClock, &c, 1000000, scratch + 0x90, 0, 0);
    CHECK(iop_r32(scratch + 0x90) == 36864000u && iop_r32(scratch + 0x94) == 0, "USec2SysClock(1 s) = %u", iop_r32(scratch + 0x90));
}

static void test_timer(void) {
    printf("hard timer (interrupt context, real time)\n");
    iop_ctx c = { 0 };
    timer_wake_sema = (uint32_t)new_sema(1, 0, 1);
    start(new_thread(G_TIMER_WAITER, 20), 0);
    iop_run();
    int id = (int)CALL(timrman_AllocHardTimer, &c, 1, 32, 1, 0);
    CHECK(id > 0, "AllocHardTimer -> %d", id);
    timer_limit = 100;
    CALL(timrman_SetTimerHandler, &c, (uint32_t)id, 36864 /* 1 ms of SYSCLOCK */, G_TIMER, 36864);
    CALL(timrman_SetupHardTimer, &c, (uint32_t)id, 1, 0, 1);
    int64_t t0 = iop_now();
    CALL(timrman_StartHardTimer, &c, (uint32_t)id, 0, 0, 0);
    while (timer_calls < 100 && iop_now() - t0 < iop_usec_to_ticks(2000000u)) { iop_service(); Sleep(1); }
    double ms = (double)(iop_now() - t0) * 1000.0 / (double)iop_usec_to_ticks(1000000u);
    CHECK(timer_calls == 100, "handler calls %u", timer_calls);
    CHECK(ms >= 98 && ms < 400, "100 ticks at 1 kHz took %.1f ms", ms);
    CHECK(timer_in_intr, "handler not in interrupt context");
    CHECK(timer_waiter_done, "iSignalSema from the handler did not wake the waiting thread");
    for (int i = 0; i < 20; i++) { iop_service(); Sleep(1); }
    CHECK(timer_calls == 100, "handler returning 0 did not stop the timer (%u calls)", timer_calls);
    CHECK(CALL(timrman_FreeHardTimer, &c, (uint32_t)id, 0, 0, 0) == 0, "FreeHardTimer");
}

static void test_event_flags(void) {
    printf("event flags\n");
    iop_ctx c = { 0 };
    iop_w32(scratch + 0x60, 2); iop_w32(scratch + 0x64, 0); iop_w32(scratch + 0x68, 0);   /* EA_MULTI */
    evf = (int)CALL(thevent_CreateEventFlag, &c, scratch + 0x60, 0, 0, 0);
    start(new_thread(G_EVF, 30), 0);
    iop_run();
    CALL(thevent_SetEventFlag, &c, (uint32_t)evf, 0x1, 0, 0);
    iop_run();
    CHECK(!evf_done, "AND wait released with only bit 0");
    CALL(thevent_SetEventFlag, &c, (uint32_t)evf, 0x6, 0, 0);
    iop_run();
    CHECK(evf_done == 1 && evf_res == 0x7, "AND wait: done %d, bits 0x%X", evf_done, evf_res);
    CHECK((int)CALL(thevent_PollEventFlag, &c, (uint32_t)evf, 0x4, 1, 0) == KE_OK, "bit 2 kept after WEF_CLEAR of 0x3");
    CHECK((int)CALL(thevent_PollEventFlag, &c, (uint32_t)evf, 0x1, 1, 0) == KE_EVF_COND, "bits 0x3 not cleared");
    CALL(thevent_ClearEventFlag, &c, (uint32_t)evf, 0, 0, 0);
    CHECK((int)CALL(thevent_PollEventFlag, &c, (uint32_t)evf, 0x4, 1, 0) == KE_EVF_COND, "ClearEventFlag(0)");
}

static void test_rpc(void) {
    printf("RPC server loop\n");
    rpc_qd = scratch + 0x100; rpc_sd = scratch + 0x140; rpc_buf = scratch + 0x200; rpc_out = scratch + 0x280;
    start(new_thread(G_RPC_THREAD, 40), 0);
    iop_run();
    uint32_t sd = 0, buff = 0;
    CHECK(iop_rpc_find_server(0x1234, &sd, &buff) && sd == rpc_sd && buff == rpc_buf, "server lookup: sd 0x%X buff 0x%X", sd, buff);
    CHECK(iop_rpc_is_server(rpc_sd) && !iop_rpc_is_server(rpc_buf), "iop_rpc_is_server");
    iop_w32(rpc_buf, 100);                            /* the EE's argument data, DMA'd to buff */
    iop_rpc_req r = { { 11, 22, 33, 44 }, rpc_sd, 21, 4, 0x00400000u, 4, 0 };
    iop_rpc_request(&r);
    CHECK(replies == 0, "replied before the IOP ran");
    iop_run();
    CHECK(replies == 1 && last_reply_addr == rpc_out && iop_r32(rpc_out) == 142, "reply %u at 0x%X = %u", replies,
          last_reply_addr, iop_r32(rpc_out));
    CHECK(last_reply_req.pkt[0] == 11 && last_reply_req.pkt[3] == 44 && last_reply_req.receive == 0x00400000u,
          "request fields not echoed");
    CHECK(iop_r32(rpc_sd + 36) == 21 && iop_r32(rpc_sd + 0) == 0x1234, "SifRpcServerData: rpc_number %u sid 0x%X",
          iop_r32(rpc_sd + 36), iop_r32(rpc_sd));
    r.fno = 5;                                        /* two queued requests are served in order */
    iop_rpc_request(&r);
    r.fno = 6;
    iop_rpc_request(&r);
    iop_run();
    CHECK(replies == 3 && iop_r32(rpc_out) == 112, "queued requests: %u replies, last %u", replies, iop_r32(rpc_out));
}

static void test_completions(void) {
    printf("CD, SIF DMA and libsd completion interrupts\n");
    cd_buf = iop_alloc(4096);
    start(new_thread(G_CD, 30), 0);
    run_until(&cd_done, 100);
    CHECK(cd_done == 1 && cd_busy_after_read == 1, "sceCdRead/Sync: done %d, busy right after read %d", cd_done, cd_busy_after_read);
    CHECK(iop_r8(cd_buf) == 100 && iop_r8(cd_buf + 4095) == (uint8_t)(100 + 4095), "sectors not in IOP RAM");
    CHECK(cd_cb_calls == 1 && cd_cb_reason == 1 && cd_cb_intr == 1, "sceCdCallback: %u calls, reason %u, intr %u",
          cd_cb_calls, cd_cb_reason, cd_cb_intr);

    iop_ctx c = { 0 };
    uint32_t fp = scratch + 0x340;                    /* sceCdlFILE: lsn, size, name[16], date[8] */
    put_str(scratch + 0x380, "\\SYSTEM.CNF;1");
    put_str(scratch + 0x3A0, "\\data\\snd.bnk");
    put_str(scratch + 0x3C0, "\\DATA\\NOPE.BNK");
    CHECK(CALL(cdvdman_sceCdSearchFile, &c, fp, scratch + 0x380, 0, 0) == 1 && iop_r32(fp) == 30 && iop_r32(fp + 4) == 100,
          "SearchFile SYSTEM.CNF: lsn %u size %u", iop_r32(fp), iop_r32(fp + 4));
    CHECK(CALL(cdvdman_sceCdSearchFile, &c, fp, scratch + 0x3A0, 0, 0) == 1 && iop_r32(fp) == 40 &&
          iop_r32(fp + 4) == 5000 && iop_r8(fp + 8) == 'S' && iop_r8(fp + 24) == 104,
          "SearchFile in a subdirectory: lsn %u size %u", iop_r32(fp), iop_r32(fp + 4));
    CHECK(CALL(cdvdman_sceCdSearchFile, &c, fp, scratch + 0x3C0, 0, 0) == 0, "SearchFile of a missing file");

    uint32_t d = scratch + 0x300;                     /* SifDmaTransfer_t: src, dest, size, attr */
    iop_w32(d, 0x1234); iop_w32(d + 4, 0x00500000u); iop_w32(d + 8, 0x24); iop_w32(d + 12, 0);
    uint32_t id = CALL(sifman_sceSifSetDmaIntr, &c, d, 1, G_DMA_CB, 0xBEEF);
    CHECK(id != 0 && dma_iop == 0x1234 && dma_ee == 0x00500000u && dma_size == 0x24, "IOP->EE DMA");
    CHECK(dma_cb_calls == 0, "DMA callback ran inside sceSifSetDmaIntr");
    CHECK((int)CALL(sifman_sceSifDmaStat, &c, id, 0, 0, 0) < 0, "sceSifDmaStat of a finished transfer");
    iop_run();
    CHECK(dma_cb_calls == 1 && dma_cb_arg == 0xBEEF, "DMA callback: %u calls, arg 0x%X", dma_cb_calls, dma_cb_arg);

    CALL(libsd_sceSdSetTransIntrHandler, &c, 1, G_SD_CB, 0x77, 0);
    iop_sd_transfer_done(1);
    CHECK(iop_poll_flag, "transfer completion does not request a poll");
    iop_run();
    CHECK(sd_cb_calls == 1 && sd_cb_ch == 1 && sd_cb_arg == 0x77, "sd handler: %u calls ch %u arg 0x%X", sd_cb_calls,
          sd_cb_ch, sd_cb_arg);
    iop_sd_transfer_done(1);              /* two transfers finished before the handler could run */
    iop_sd_transfer_done(1);
    iop_run();
    CHECK(sd_cb_calls == 3, "sd handler: one call per completion expected, %u calls", sd_cb_calls);
}

static void test_slice(void) {
    printf("time slice of a busy-waiting thread\n");
    start(new_thread(G_SPIN, 30), 0);
    int64_t t0 = iop_now();
    iop_run();
    double ms = (double)(iop_now() - t0) * 1000.0 / (double)iop_usec_to_ticks(1000000u);
    CHECK(!spin_done && ms < 100, "iop_run returned after %.1f ms (spin done %d)", ms, spin_done);
    CHECK(spin_polls > 0, "no polls");
    spin_release = 1;
    int64_t before = iop_now();
    iop_run();
    CHECK(spin_done, "spinning thread did not resume");
    /* the end of every pass is published for the mixer (sd.c holds the voices when it gets old) */
    CHECK(iop_last_run_time() >= before && iop_last_run_time() <= iop_now(), "iop_last_run_time not updated by iop_run");
}

/* The IOP thread (iop.h): with the "EE" (this thread) no longer running the IOP at all, the IOP's
 * timers, delays and RPC replies still happen in real time; while the EE side holds the IOP lock
 * nothing runs, and the missed timer interrupts are delivered afterwards. */
static volatile uint32_t* vol(uint32_t* p) { return (volatile uint32_t*)p; }

static void test_iop_thread(void) {
    printf("IOP thread (the IOP keeps running while the EE does not)\n");
    iop_thread_start();
    CHECK(iop_threaded(), "IOP thread not running");
    iop_ctx c = { 0 };
    /* a 1 kHz hard timer: 100 ticks in ~100 ms with nobody calling iop_run here */
    iop_lock();                                       /* kernel calls from this thread: under the lock */
    timer_calls = 0; timer_limit = 100; timer_wake_sema = 0;
    int id = (int)CALL(timrman_AllocHardTimer, &c, 1, 32, 1, 0);
    CALL(timrman_SetTimerHandler, &c, (uint32_t)id, 36864, G_TIMER, 36864);
    CALL(timrman_SetupHardTimer, &c, (uint32_t)id, 1, 0, 1);
    int64_t t0 = iop_now();
    CALL(timrman_StartHardTimer, &c, (uint32_t)id, 0, 0, 0);
    iop_unlock();
    while (*vol(&timer_calls) < 30 && iop_now() - t0 < iop_usec_to_ticks(2000000u)) Sleep(1);
    /* the EE side holds the lock for 40 ms: no tick runs meanwhile, the missed ones come after */
    iop_lock();
    uint32_t held_at = *vol(&timer_calls);
    Sleep(40);
    uint32_t after_hold = *vol(&timer_calls);
    iop_unlock();
    CHECK(after_hold == held_at, "timer ran while the lock was held (%u -> %u)", held_at, after_hold);
    while (*vol(&timer_calls) < 100 && iop_now() - t0 < iop_usec_to_ticks(2000000u)) Sleep(1);
    double ms = (double)(iop_now() - t0) * 1000.0 / (double)iop_usec_to_ticks(1000000u);
    CHECK(*vol(&timer_calls) == 100 && ms >= 98 && ms < 400, "100 ticks: %u calls in %.1f ms", *vol(&timer_calls), ms);
    iop_lock();
    CALL(timrman_FreeHardTimer, &c, (uint32_t)id, 0, 0, 0);
    iop_unlock();

    /* DelayThread wakes in real time */
    delay_done = 0;
    iop_lock();
    start(new_thread(G_DELAY, 30), 0);
    iop_unlock();
    t0 = iop_now();
    while (!delay_done && iop_now() - t0 < iop_usec_to_ticks(1000000u)) Sleep(1);
    ms = (double)(iop_now() - t0) * 1000.0 / (double)iop_usec_to_ticks(1000000u);
    CHECK(delay_done && ms < 250, "DelayThread(20 ms) on the IOP thread: done %d after %.1f ms", delay_done, ms);

    /* an RPC request from the EE side is served by the IOP thread (test_rpc's server is waiting);
     * iop_kick (as sif.c does) wakes the IOP thread instead of running the IOP on this one */
    uint32_t before = *vol(&replies);
    iop_w32(rpc_buf, 7);
    iop_rpc_req r = { { 1, 2, 3, 4 }, rpc_sd, 9, 4, 0x00400000u, 4, 0 };
    iop_rpc_request(&r);
    iop_kick(0);
    t0 = iop_now();
    while (*vol(&replies) == before && iop_now() - t0 < iop_usec_to_ticks(1000000u)) Sleep(1);
    ms = (double)(iop_now() - t0) * 1000.0 / (double)iop_usec_to_ticks(1000000u);
    CHECK(*vol(&replies) == before + 1 && iop_r32(rpc_out) == 25 && ms < 100, "RPC on the IOP thread: %u replies, result %u, %.1f ms",
          *vol(&replies) - before, iop_r32(rpc_out), ms);
    /* the EE's safe points no longer run the IOP */
    iop_lock();
    uint32_t pass_check = *vol(&timer_calls);
    iop_unlock();
    iop_service();
    CHECK(*vol(&timer_calls) == pass_check, "iop_service ran the IOP although it has its own thread");
}

static void test_libs(void) {
    printf("sysclib and printf\n");
    iop_ctx c = { 0 };
    uint32_t b = scratch + 0x400;
    for (int i = 0; i < 16; i++) iop_w8(b + (uint32_t)i, (uint8_t)i);
    CALL(sysclib_memcpy, &c, b + 2, b, 8, 0);                    /* overlapping: memmove semantics */
    CHECK(iop_r8(b + 2) == 0 && iop_r8(b + 9) == 7, "memcpy overlap");
    CALL(sysclib_memset, &c, b, 0xAB, 3, 0);
    CHECK(c.r[2] == b && iop_r8(b + 2) == 0xAB && iop_r8(b + 3) == 1, "memset");
    CALL(sysclib_wmemcopy, &c, b + 0x20, b, 8, 0);
    CHECK(iop_r32(b + 0x20) == iop_r32(b) && iop_r32(b + 0x24) == iop_r32(b + 4), "wmemcopy");
    put_str(b + 0x40, "abcd");
    put_str(b + 0x45, "abce");
    put_str(b + 0x4A, "  -0x1Fz");
    CHECK((int)CALL(sysclib_strncmp, &c, b + 0x40, b + 0x45, 3, 0) == 0, "strncmp equal prefix");
    CHECK((int)CALL(sysclib_strncmp, &c, b + 0x40, b + 0x45, 4, 0) < 0, "strncmp less");
    CHECK((int)CALL(sysclib_strtol, &c, b + 0x4A, b + 0x60, 16, 0) == -0x1F && iop_r32(b + 0x60) == b + 0x51,
          "strtol -> %d, end 0x%X", (int)c.r[2], iop_r32(b + 0x60));

    /* printf with register and stack arguments, captured from stderr */
    char path[MAX_PATH];
    GetTempPathA(sizeof path, path);
    strcat(path, "iop_printf_test.txt");
    fflush(stderr);
    FILE* f = freopen(path, "w", stderr);
    put_str(b + 0x80, "%d|%5s|%-4x|%c|%08X|%%|%s\n");
    put_str(b + 0xC0, "hi");
    put_str(b + 0xD0, "five");
    c.r[29] = scratch + 0x800;
    iop_w32(c.r[29] + 16, 'Q'); iop_w32(c.r[29] + 20, 0xBEEF); iop_w32(c.r[29] + 24, b + 0xD0);
    CALL(stdio_printf, &c, b + 0x80, (uint32_t)-42, b + 0xC0, 0xAB);
    fflush(stderr);
    if (f) fclose(f);
    char line[256] = "";
    FILE* in = fopen(path, "r");
    if (in) { if (!fgets(line, sizeof line, in)) line[0] = 0; fclose(in); }
    remove(path);
    CHECK(strcmp(line, "[iop] -42|   hi|ab  |Q|0000BEEF|%|five\n") == 0, "printf wrote \"%s\"", line);
}

int main(void) {
    ConvertThreadToFiber(NULL);
    iop_register_recompiled(funcs, sizeof funcs / sizeof funcs[0], fake_mod, 1);
    iop_init();
    scratch = iop_alloc(0x1000);
    test_sysmem();
    test_priority();
    test_sleep_delay();
    test_timer();
    test_event_flags();
    test_rpc();
    test_completions();
    test_slice();
    test_iop_thread();                              /* from here on the IOP runs on its own thread */
    iop_lock();                                     /* test_libs calls kernel code from this thread */
    test_libs();                                    /* last: it redirects stderr */
    iop_unlock();
    printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
