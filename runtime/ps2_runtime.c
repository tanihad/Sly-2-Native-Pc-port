/* Runtime for recompiled PS2 code: function lookup, interrupts, BIOS syscalls (HLE), startup. */
#include "hw.h"
#include "perf.h"
#include "iop.h"
#include "audio_out.h"

#include <stdlib.h>
#include <windows.h>

uint8_t* ps2_ram;
uint8_t* ps2_spr;
uint32_t ps2_trace[PS2_TRACE_SIZE];
uint32_t ps2_trace_pos;
static int in_interrupt;                         /* inside a game interrupt handler */

/* PS2_WATCHDOG=<seconds>: after that long, print where the game has been running and exit.
 * The trace is written without locks, so the snapshot can be slightly torn; fine for debugging. */
static DWORD WINAPI watchdog(LPVOID arg) {
    Sleep((DWORD)(uintptr_t)arg * 1000);
    perf_final();
    static uint32_t snap[PS2_TRACE_SIZE];
    uint32_t pos = ps2_trace_pos;
    memcpy(snap, ps2_trace, sizeof snap);

    fprintf(stderr, "\n[watchdog] %u function calls so far. Most frequent in the last %u:\n", pos, PS2_TRACE_SIZE);
    static uint32_t addr[PS2_TRACE_SIZE], cnt[PS2_TRACE_SIZE];
    uint32_t n = 0;
    for (uint32_t i = 0; i < PS2_TRACE_SIZE; i++) {
        uint32_t k = 0;
        while (k < n && addr[k] != snap[i]) k++;
        if (k == n) { addr[n] = snap[i]; cnt[n++] = 0; }
        cnt[k]++;
    }
    for (int shown = 0; shown < 12; shown++) {
        uint32_t best = 0;
        for (uint32_t k = 1; k < n; k++) if (cnt[k] > cnt[best]) best = k;
        if (!cnt[best]) break;
        fprintf(stderr, "  0x%08X  %u\n", addr[best], cnt[best]);
        cnt[best] = 0;
    }
    fprintf(stderr, "[watchdog] last 24 calls (oldest first):\n ");
    for (uint32_t i = 24; i > 0; i--) fprintf(stderr, " %08X", snap[(pos - i) & (PS2_TRACE_SIZE - 1)]);
    fprintf(stderr, "\n");
    fflush(stderr);
    ExitProcess(3);
    return 0;
}

/* PS2_CALLS="<vblank>[:<frames>][,...]": count every recompiled function called from that VBlank on,
 * for <frames> VBlanks (default 1), with the threads that called it, then print the table sorted by
 * address; up to 8 windows. Debug builds only (needs PS2_TRACE). */
volatile int ps2_calls_on;
#define CALLS_HASH 65536u
static struct { uint32_t addr, count, threads; } calls[CALLS_HASH];
static uint32_t calls_from, calls_frames;
static int calls_window;

/* PS2_WATCH="<hexaddr>,...": log calls of these functions with $ra and $a0-$a3, each distinct
 * caller once (PS2_WATCH_ALL=1: every call, up to 5000 lines). */
static uint32_t watch[16], watch_seen[256][2], watch_a0;
static int watch_n, watch_all, watch_seen_n, watch_lines, watch_a0_on;

/* PS2_INPUT "@<hexaddr>[/<hexra>]" tokens (from PS2_INPUT or live PS2_INPUT_FILE lines): window.c's
 * input script waits for calls of these functions (made from the call site returning to <hexra>,
 * if not 0). An anchor is registered by its first query and counted from then on; queries and
 * counting both run on the emulation thread. Debug builds count calls of any function (PS2_TRACE ->
 * ps2_calls_record); release builds only those of functions with an entry hook (ps2_hook). */
#define ANCHORS_MAX 64
static uint32_t anchor_addr[ANCHORS_MAX], anchor_ra[ANCHORS_MAX], anchor_calls[ANCHORS_MAX];
static int anchor_n;
volatile int ps2_hooks_on;

uint32_t ps2_func_calls(uint32_t addr, uint32_t ra) {
    for (int k = 0; k < anchor_n; k++)
        if (anchor_addr[k] == addr && anchor_ra[k] == ra) return anchor_calls[k];
    if (anchor_n == ANCHORS_MAX) {
        static int full;
        if (!full++) fprintf(stderr, "[ps2] PS2_INPUT: more than %d function anchors; the rest never fire\n", ANCHORS_MAX);
        return 0;
    }
#ifdef PS2_NO_TRACE
    int hooked = 0;
    for (const uint32_t* h = ps2_hook_addrs; *h && !hooked; h++) hooked = *h == addr;
    if (!hooked)
        fprintf(stderr, "[ps2] PS2_INPUT: function anchor @%X needs the debug build (release builds: only the "
                        "functions in recomp/hooks.py); it never fires here\n", addr);
    ps2_hooks_on = 1;
#endif
    anchor_addr[anchor_n] = addr;
    anchor_ra[anchor_n] = ra;
    anchor_calls[anchor_n++] = 0;
    ps2_calls_on = 1;
    return 0;
}

static void anchors_count(ps2_ctx* ctx, uint32_t addr) {
    for (int k = 0; k < anchor_n; k++)
        if (anchor_addr[k] == addr && (!anchor_ra[k] || anchor_ra[k] == ctx->r[REG_RA].uw[0])) anchor_calls[k]++;
}

/* PS2_WARP_TASK (runtime/warp.c): start a job directly on "Resume Game" (debug tool, off by default). */
int warp_init(void);
void warp_on_call(ps2_ctx* ctx, uint32_t addr);

/* Entry hooks (recomp/hooks.py, every build): called at the entry of the hooked functions while a
 * tool has set ps2_hooks_on; runs on the thread that made the call. */
void ps2_hook(ps2_ctx* ctx, uint32_t addr) {
#ifdef PS2_NO_TRACE
    anchors_count(ctx, addr);                    /* debug builds count them in ps2_calls_record */
#endif
    warp_on_call(ctx, addr);
}

void ps2_calls_record(ps2_ctx* ctx, uint32_t addr) {
    anchors_count(ctx, addr);
    for (int i = 0; i < watch_n; i++) {
        if (watch[i] != addr) continue;
        if (watch_a0_on && ctx->r[REG_A0].uw[0] != watch_a0) continue;   /* PS2_WATCH_A0=<hex> */
        uint32_t ra = ctx->r[REG_RA].uw[0];
        int seen = 0;
        for (int k = 0; k < watch_seen_n && !seen; k++) seen = watch_seen[k][0] == addr && watch_seen[k][1] == ra;
        if (!seen && watch_seen_n < 256) { watch_seen[watch_seen_n][0] = addr; watch_seen[watch_seen_n++][1] = ra; }
        if ((!seen || watch_all) && watch_lines++ < 5000)
            fprintf(stderr, "[watch] v%u #%d %08X ra %08X a0 %08X a1 %08X a2 %08X a3 %08X\n", ps2_vblank_count(),
                    thread_current_id(), addr, ra, ctx->r[REG_A0].uw[0], ctx->r[REG_A1].uw[0],
                    ctx->r[REG_A2].uw[0], ctx->r[REG_A3].uw[0]);
    }
    if (!calls_window) return;
    uint32_t h = ((addr >> 2) * 2654435761u) >> 16;
    while (calls[h].addr != addr) {
        if (!calls[h].addr) { calls[h].addr = addr; break; }
        h = (h + 1) & (CALLS_HASH - 1);
    }
    calls[h].count++;
    calls[h].threads |= 1u << (thread_current_id() & 31);
}

static void watch_init(void) {
    const char* e = getenv("PS2_WATCH");
    if (!e) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char* tok = strtok(buf, ","); tok && watch_n < 16; tok = strtok(NULL, ","))
        watch[watch_n++] = (uint32_t)strtoul(tok, NULL, 16);
    watch_all = getenv("PS2_WATCH_ALL") != NULL;
    if (getenv("PS2_WATCH_A0")) { watch_a0_on = 1; watch_a0 = (uint32_t)strtoul(getenv("PS2_WATCH_A0"), NULL, 16); }
    if (watch_n) ps2_calls_on = 1;
}

static int calls_cmp(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void calls_vblank(uint32_t v) {
    static int init, next;
    static uint32_t windows[9][2];
    if (!init) {                                 /* "<from>[:<frames>],..." up to 8 windows */
        init = 1;
        const char* e = getenv("PS2_CALLS");
        int n = 0;
        while (e && n < 8) {
            windows[n][1] = 1;
            if (sscanf(e, "%u:%u", &windows[n][0], &windows[n][1]) >= 1) n++;
            e = strchr(e, ',');
            if (e) e++;
        }
        if (n) { calls_from = windows[0][0]; calls_frames = windows[0][1]; next = 1; }
        windows[n][0] = 0;
    }
    if (!calls_from) return;
    if (v == calls_from) ps2_calls_on = calls_window = 1;
    if (v == calls_from + calls_frames) {
        calls_window = 0;
        ps2_calls_on = watch_n > 0 || anchor_n > 0;
        static uint32_t rows[CALLS_HASH][3];
        uint32_t n = 0;
        for (uint32_t i = 0; i < CALLS_HASH; i++)
            if (calls[i].addr) { rows[n][0] = calls[i].addr; rows[n][1] = calls[i].count; rows[n][2] = calls[i].threads; n++; }
        qsort(rows, n, sizeof rows[0], calls_cmp);
        fprintf(stderr, "[calls] VBlank %u-%u: %u functions\n", calls_from, v, n);
        for (uint32_t i = 0; i < n; i++)
            fprintf(stderr, "[calls] %08X %8u threads 0x%X\n", rows[i][0], rows[i][1], rows[i][2]);
        memset(calls, 0, sizeof calls);
        calls_from = windows[next][0];           /* next window, if any (0 ends the list) */
        calls_frames = windows[next][1];
        if (calls_from) next++;
    }
}

static void die(const char* fmt, uint32_t v) {
    fprintf(stderr, "[ps2] fatal: ");
    fprintf(stderr, fmt, v);
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- memory: the cases the inline accessors (ps2_runtime.h) leave out ------------------------- */

void ps2_mem_read_slow(ps2_ctx* ctx, uint32_t a, void* out, unsigned size) {
    uint8_t* p = ps2_fast(a);
    if (p) memcpy(out, p, size); else ps2_hw_read(ctx, a, out, size);
}

void ps2_mem_write_slow(ps2_ctx* ctx, uint32_t a, const void* in, unsigned size) {
    uint8_t* p = ps2_fast(a);
    if (p) memcpy(p, in, size); else ps2_hw_write(ctx, a, in, size);
}

/* ---- function lookup ---------------------------------------------------------------------- */

static ps2_func find_func(uint32_t addr) {
    size_t lo = 0, hi = ps2_func_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ps2_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < ps2_func_count && ps2_funcs[lo].addr == addr) ? ps2_funcs[lo].fn : NULL;
}

void ps2_call(ps2_ctx* ctx, uint32_t addr) {
    ps2_func fn = find_func(addr);
    if (!fn) {
        fprintf(stderr, "[ps2] fatal: call to 0x%08X, which is not the start of a recompiled function "
                        "($ra=0x%08X)\n", addr, ctx->r[31].uw[0]);
        exit(1);
    }
    fn(ctx);
}

/* ---- COP0 / unimplemented ------------------------------------------------------------------ */

static LARGE_INTEGER qpc_freq, qpc_start;

uint32_t ps2_mfc0(ps2_ctx* ctx, int reg) {
    if (reg == 9) {                              /* Count: EE clock, 294.912 MHz */
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double secs = (double)(now.QuadPart - qpc_start.QuadPart) / (double)qpc_freq.QuadPart;
        return (uint32_t)(uint64_t)(secs * 294912000.0);
    }
    if (reg == 12) return 0x70000001u | (ctx->ie ? 0x10000u : 0);   /* Status: CU0-2, IE, EIE */
    if (reg == 15) return 0x00002E20u;           /* PRId: R5900 */
    return 0;
}

void ps2_mtc0(ps2_ctx* ctx, int reg, uint32_t v) {
    if (reg == 12) ctx->ie = (v >> 16) & 1;      /* Status.EIE */
}

void ps2_unimpl(ps2_ctx* ctx, uint32_t pc, const char* op) {
    (void)ctx;
    static uint32_t seen[256]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == pc) return;
    if (n < 256) seen[n++] = pc;
    fprintf(stderr, "[ps2] unimplemented %s at 0x%08X\n", op, pc);
}

/* ---- interrupt handlers ---------------------------------------------------------------------- */

typedef struct { uint32_t fn, arg; int used; } handler;
static handler dmac_handlers[16][8], intc_handlers[16][8];

static int add_handler(handler list[8], uint32_t fn, uint32_t arg) {
    for (int i = 0; i < 8; i++)
        if (!list[i].used) { list[i] = (handler){ fn, arg, 1 }; return i + 1; }
    fprintf(stderr, "[ps2] too many interrupt handlers\n");
    return -1;
}

static int remove_handler(handler list[8], uint32_t id) {
    if (id < 1 || id > 8 || !list[id - 1].used) return -1;
    list[id - 1].used = 0;
    return 0;
}

/* Run the game's handlers for one interrupt source as the kernel would: on the kernel's
 * interrupt stack, with every register of the interrupted thread restored afterwards. */
#define INTERRUPT_STACK_TOP 0x0006F000u
static void run_handlers(ps2_ctx* ctx, handler list[8], int src) {
    static ps2_ctx saved;
    in_interrupt = 1;
    for (int i = 0; i < 8; i++) {
        if (!list[i].used) continue;
        saved = *ctx;
        ctx->r[REG_A0].sd[0] = src;
        ctx->r[REG_A1].sd[0] = (int32_t)list[i].arg;
        ctx->r[REG_A2].sd[0] = 0;
        ctx->r[REG_SP].sd[0] = (int32_t)INTERRUPT_STACK_TOP;
        ps2_call(ctx, list[i].fn);
        *ctx = saved;
    }
    in_interrupt = 0;
}

int ps2_in_interrupt(void) { return in_interrupt; }

/* ---- timing: VBlank and a 1 kHz housekeeping tick --------------------------------------------- */

volatile uint32_t ps2_irq_pending;
static volatile LONG frames_behind;              /* VBlanks the emulation hasn't serviced yet */
static volatile LONG vblanks_due;                /* VBlanks the host timer has raised so far */
static uint32_t vblank_count;
static const char* dump_dir;
static int dump_every = 60, headless;
static uint32_t dump_from;                       /* PS2_DUMP_FROM=<vblank>: no dumps before it */

/* VBlank pacing: VBlanks are due at exact multiples of 1/59.94 s of host time (QPC), so the game
 * never runs faster than real time. The thread sleeps until the next VBlank or 1 ms housekeeping
 * tick, whichever is first, with a high-resolution waitable timer (Windows 10 1803+; else
 * Sleep(1)), and wakes an idle EE at once (threads.c waits on ps2_irq_pending). When the EE falls
 * behind, the VBlanks it missed are dropped, not delivered in a burst (ps2_service_irq): the game
 * slows down instead of stuttering through catch-up frames. */
static DWORD WINAPI host_timer(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER now;
    const int64_t tick = qpc_freq.QuadPart / 1000;
    int64_t next_tick = qpc_start.QuadPart + tick;
    for (uint64_t frame = 1;;) {                 /* NTSC: 59.94 fields per second */
        int64_t due = qpc_start.QuadPart + (int64_t)(frame * 1001 * (uint64_t)qpc_freq.QuadPart / 60000);
        QueryPerformanceCounter(&now);
        uint32_t raise = 0;
        if (now.QuadPart >= due) {
            frame++;
            InterlockedIncrement(&frames_behind);
            InterlockedIncrement(&vblanks_due);
            raise |= PS2_IRQ_VBLANK;
        }
        if (now.QuadPart >= next_tick) {
            next_tick = now.QuadPart + tick;
            raise |= PS2_IRQ_TICK;
            iop_poll_flag = 1;                   /* IOP loop heads: check interrupts, time slice */
        }
        if (raise) {
            InterlockedOr((volatile LONG*)&ps2_irq_pending, (LONG)raise);
            WakeByAddressAll((PVOID)&ps2_irq_pending);
            continue;
        }
        int64_t wait = (due < next_tick ? due : next_tick) - now.QuadPart;   /* QPC units */
        if (timer) {
            LARGE_INTEGER rel;
            rel.QuadPart = -(wait * 10000000 / qpc_freq.QuadPart);          /* 100 ns units, relative */
            if (!rel.QuadPart) rel.QuadPart = -1;
            SetWaitableTimer(timer, &rel, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
    }
    return 0;
}

uint32_t ps2_vblank_count(void) { return vblank_count; }

static volatile int ram_dump_request;
void ps2_request_ram_dump(void) { ram_dump_request = 1; }   /* written at the next VBlank */

/* Debug pause (window.c, the live input token 'pause'; EE thread): the whole PS2 stands still while
 * the EE waits for the next input line - EE timers (the game's frame time), IOP time (989snd's
 * tick: sequenced music) and the SPU2 mixer - so a game played step by step sees the same time
 * passing as at full speed. VBlanks missed meanwhile are dropped as usual (ps2_service_irq). */
void sd_hold(int on);
void ps2_debug_hold(int on) {
    timer_hold(on);
    iop_time_hold(on);
    sd_hold(on);
}

/* The picture of VBlank `vb` (ok = 0: display off). Runs on the GS thread when there is one. */
static void present_frame(const uint32_t* frame, int ok, int w, int h, uint32_t vb) {
    if (!ok) {                                   /* display off: show black, not a stale frame */
        static uint32_t black[640 * 448];
        if (!headless) window_present(black, 640, 448, 640);
        return;
    }
    if (!headless) window_present(frame, w, h, 1024);
    if (dump_dir && vb % (uint32_t)dump_every == 0 && vb >= dump_from) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%06u.png", dump_dir, vb);
        debug_write_png(path, frame, w, h, 1024);
        /* audio clock = position in a PS2_AUDIO_WAV recording, to line sound up with pictures */
        if (audio_backend() != AUDIO_OFF)
            fprintf(stderr, "[dump] frame_%06u.png at audio %.2f s\n", vb,
                    (double)audio_frames_written() / 48000.0);
        char latest[512];                        /* latest.png: always the newest dump (live play) */
        snprintf(latest, sizeof latest, "%s/latest.png", dump_dir);
        CopyFileA(path, latest, FALSE);
        static int ring = -1;                    /* PS2_DUMP_RING=<n>: keep only the newest n dumps */
        if (ring < 0) ring = getenv("PS2_DUMP_RING") ? atoi(getenv("PS2_DUMP_RING")) : 0;
        if (ring > 0 && vb >= (uint32_t)(ring * dump_every)) {
            snprintf(path, sizeof path, "%s/frame_%06u.png", dump_dir, vb - (uint32_t)(ring * dump_every));
            DeleteFileA(path);
        }
    }
}

static void vblank(ps2_ctx* ctx) {
    vblank_count++;
    perf_vblank(vblank_count, (uint32_t)vblanks_due);
    if (perf_burn_hook) perf_burn_hook(0);       /* PS2_PERF_BURN headroom test */
    calls_vblank(vblank_count);
    gs_vsync();
    int perf_prev = perf_enter(PERF_PRESENT);
    if (gs_async()) {
        gs_queue_vsync(vblank_count);            /* composed and presented by the GS thread */
    } else {
        static uint32_t frame[1024 * 1024];
        int w = 0, h = 0;
        int ok = gs_display(frame, 1024, &w, &h);
        present_frame(frame, ok, w, h, vblank_count);
    }
    perf_leave(perf_prev);
    pad_vsync(ctx);

    /* PS2_PEEK="<hexaddr>:<words>[,...]": print EE memory every PS2_PEEK_EVERY VBlanks (default 60) */
    static const char* peek = (const char*)1;
    static int peek_every = 60;
    if (peek == (const char*)1) {
        peek = getenv("PS2_PEEK");
        if (getenv("PS2_PEEK_EVERY")) peek_every = atoi(getenv("PS2_PEEK_EVERY")) > 0 ? atoi(getenv("PS2_PEEK_EVERY")) : 60;
    }
    /* PS2_DUMP_RAM="<vblank>[,...]": write EE RAM (32 MB) to <dump dir or .>/ram_<vblank>.bin */
    static const char* ram_at = (const char*)1;
    if (ram_at == (const char*)1) ram_at = getenv("PS2_DUMP_RAM");
    int ram_now = ram_dump_request;              /* live input token "ram" */
    ram_dump_request = 0;
    for (const char* s = ram_now ? "0" : ram_at; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : "") {
        if (!ram_now && (uint32_t)strtoul(s, NULL, 10) != vblank_count) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/ram_%06u.bin", dump_dir ? dump_dir : ".", vblank_count);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(ps2_ram, 1, PS2_RAM_SIZE, f); fclose(f); fprintf(stderr, "[ps2] RAM written to %s\n", path); }
    }
    static int log_threads = -1;
    if (log_threads < 0) log_threads = getenv("PS2_LOG_THREADS") != NULL;
    if (log_threads && vblank_count % (uint32_t)peek_every == 0) threads_debug_dump();
    if (peek && vblank_count % (uint32_t)peek_every == 0) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", peek);
        for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            unsigned addr = 0, words = 1;
            if (sscanf(tok, "%x:%u", &addr, &words) < 1) continue;
            fprintf(stderr, "[peek] v%u %08X:", vblank_count, addr);
            for (unsigned k = 0; k < words && k < 16; k++) fprintf(stderr, " %08X", ps2_r32(ctx, addr + 4 * k));
            fprintf(stderr, "\n");
        }
    }
    /* PS2_POKE="<hexaddr>=<hexvalue>[,...]" (debugging, off by default): write these 32-bit words
     * every VBlank, e.g. hold a character's health (Sly 2: bentley.hpCur, 3D4AC8=28) to test the
     * later parts of a fight that scripted input cannot win. */
    static const char* poke = (const char*)1;
    if (poke == (const char*)1) poke = getenv("PS2_POKE");
    for (const char* s = poke; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : "") {
        unsigned addr = 0, val = 0;
        if (sscanf(s, "%x=%x", &addr, &val) == 2) ps2_w32(ctx, addr & ~3u, val);
    }
    /* PS2_POKE_FILE=<path> (debugging, off by default): lines "<hexaddr>=<hexvalue>[,...]" appended
     * while the game runs are written once, at the next VBlank (looked at every 15 VBlanks). */
    static const char* poke_file = (const char*)1;
    static long poke_pos;
    if (poke_file == (const char*)1) poke_file = getenv("PS2_POKE_FILE");
    if (poke_file && *poke_file && vblank_count % 15 == 0) {
        FILE* f = fopen(poke_file, "rb");
        char buf[1024];
        size_t n = 0;
        if (f && fseek(f, poke_pos, SEEK_SET) == 0) {
            n = fread(buf, 1, sizeof buf - 1, f);
            while (n > 0 && buf[n - 1] != '\n') n--;    /* whole lines only */
            buf[n] = 0;
            poke_pos += (long)n;
        }
        if (f) fclose(f);
        for (char* s = n ? buf : NULL; s && *s; s++) {
            unsigned addr = 0, val = 0;
            if ((s == buf || s[-1] == ',' || s[-1] == '\n') && sscanf(s, "%x=%x", &addr, &val) == 2) {
                ps2_w32(ctx, addr & ~3u, val);
                fprintf(stderr, "[poke] v%u: %08X = %08X\n", vblank_count, addr & ~3u, val);
            }
        }
    }
    intc_raise(INTC_VBLANK_S);
    intc_raise(INTC_VBLANK_E);
}

/* Between the steps of long runtime operations (dma.c). The IOP is independent of the EE's state, but
 * like sif.c it is not run from inside a game interrupt handler. */
void ps2_poll_iop(void) {
    if (iop_poll_flag && !in_interrupt) iop_service();
}

/* Called at safe points (loop heads, syscall returns, idle) when ps2_irq_pending is set. */
void ps2_service_irq(ps2_ctx* ctx) {
    if (in_interrupt) return;
    uint32_t p = (uint32_t)InterlockedExchange((volatile LONG*)&ps2_irq_pending, 0);
    if (p & PS2_IRQ_VBLANK) {
        LONG n = InterlockedExchange(&frames_behind, 0);
        if (n > 0) vblank(ctx);                  /* if we fell behind, skip rather than burst */
    }
    timer_service();
    dma_service();
    iop_service();                               /* IOP threads and interrupts (timers, CD, SIF) */
    sif_deliver(ctx);

    if (!ctx->ie) {                              /* interrupts off: keep them pending */
        if (intc_pending() || dmac_pending()) InterlockedOr((volatile LONG*)&ps2_irq_pending, PS2_IRQ_HW);
        return;
    }
    for (int round = 0; round < 16; round++) {
        uint32_t ip = intc_pending(), dp = dmac_pending();
        if (!ip && !dp) break;
        for (int c = 0; c < 16; c++)
            if (ip & (1u << c)) { intc_ack(c); run_handlers(ctx, intc_handlers[c], c); }
        for (int ch = 0; ch < 16; ch++)
            if (dp & (1u << ch)) { dmac_ack(ch); run_handlers(ctx, dmac_handlers[ch], ch); }
        dma_service();                           /* handlers often unblock a stalled channel */
        sif_deliver(ctx);                        /* next queued IOP reply, if any */
    }
    thread_reschedule();                         /* handlers may have woken a thread */
}

/* ---- BIOS syscalls (high-level emulation) --------------------------------------------------- */

/* OSD configuration (the console's system settings) as Get/SetOsdConfigParam see it: bit 0 SPDIF
 * off, bits 1-2 screen type (0 = 4:3), bit 3 video output, bit 4 "language is not Japanese",
 * bits 5-12 PS1 driver settings, bits 13-15 version, bits 16-20 language (1 = English), bits 21-31
 * time zone offset. A US console with English and a 4:3 TV. */
static uint32_t osd_config = (1u << 16) | (1u << 13) | (1u << 4);

void ps2_syscall(ps2_ctx* ctx, uint32_t pc) {
    int32_t n = ctx->r[REG_V1].sw[0];
    if (n < 0) n = -n;                           /* negative = interrupt-context ("i") variant */
    uint32_t a0 = ctx->r[REG_A0].uw[0], a1 = ctx->r[REG_A1].uw[0], a2 = ctx->r[REG_A2].uw[0];
    int64_t ret = 0;

    /* A handler the game installed (SetSyscall or a direct kernel patch) takes priority, if it
     * is recompiled code. Sony's libkernel also copies kernel patches (e.g. the alarm patch)
     * from .data into kernel RAM; those aren't recompiled, so our own HLE stands in for them. */
    uint32_t user = (n < 256) ? ps2_r32(ctx, PS2_SYSCALL_TABLE + 4u * (uint32_t)n) : 0;
    if (user) {
        static uint8_t logged[256];
        ps2_func fn = find_func(user);
        if (!logged[n]) {
            logged[n] = 1;
            fprintf(stderr, "[ps2] syscall 0x%X -> game handler 0x%08X%s (from 0x%08X)\n", (unsigned)n, user,
                    fn ? "" : " (not recompiled: using HLE)", pc);
        }
        if (fn) { fn(ctx); return; }
    }

    if (thread_syscall(ctx, n, &ret)) goto done;   /* threads + semaphores; may switch threads */

    switch (n) {
    case 0x74:                                   /* SetSyscall(num, handler) */
        fprintf(stderr, "[ps2] SetSyscall(0x%X, 0x%08X)\n", a0, a1);
        if (a0 < 256) ps2_w32(ctx, PS2_SYSCALL_TABLE + 4u * a0, a1);
        break;
    case 0x04:                                   /* Exit */
        fprintf(stderr, "[ps2] Exit(%d)\n", (int)a0);
        exit((int)a0);
    case 0x3C: {                                 /* SetupThread(gp, stack, size, args, root) */
        ctx->r[REG_GP].sd[0] = (int32_t)a0;
        uint32_t top = (a1 == 0xFFFFFFFFu) ? PS2_RAM_SIZE : a1 + a2;
        ret = (int32_t)top;
        break;
    }
    case 0x3D:                                   /* SetupHeap(heap, size) */
        ret = (int32_t)((a1 == 0xFFFFFFFFu) ? ctx->r[29].uw[0] : a0 + a1);
        break;
    case 0x64:                                   /* FlushCache: nothing to flush natively */
    case 0x68:                                   /* iFlushCache */
        break;
    case 0x6B:                                   /* sceSifStopDma: transfers complete at once */
        break;
    case 0x4A:                                   /* SetOsdConfigParam(const ConfigParam*) */
        osd_config = ps2_r32(ctx, a0);
        break;
    case 0x4B:                                   /* GetOsdConfigParam(ConfigParam*) */
        ps2_w32(ctx, a0, osd_config);
        break;
    case 0x10: ret = add_handler(intc_handlers[a0 & 15], a1, ctx->r[REG_A3].uw[0]); break;   /* AddIntcHandler */
    case 0x11: ret = remove_handler(intc_handlers[a0 & 15], a1); break;                     /* RemoveIntcHandler */
    case 0x12: ret = add_handler(dmac_handlers[a0 & 15], a1, ctx->r[REG_A3].uw[0]); break;   /* AddDmacHandler */
    case 0x13: ret = remove_handler(dmac_handlers[a0 & 15], a1); break;                     /* RemoveDmacHandler */
    case 0x14: intc_set_mask((int)(a0 & 15), 1); ret = 1; break;                            /* EnableIntc */
    case 0x15: intc_set_mask((int)(a0 & 15), 0); ret = 1; break;                            /* DisableIntc */
    case 0x16: dmac_set_mask((int)(a0 & 15), 1); ret = 1; break;                            /* EnableDmac */
    case 0x17: dmac_set_mask((int)(a0 & 15), 0); ret = 1; break;                            /* DisableDmac */
    case 0x76: ret = -1; break;                  /* SifDmaStat: every transfer completes at once */
    case 0x77: ret = sif_set_dma(ctx, a0, a1); break;                                       /* SifSetDma */
    case 0x78: break;                            /* SifSetDChain: re-arms SIF0 receive; nothing to do */
    case 0x79: sif_set_reg(a0, a1); ret = 1; break;                                         /* SifSetReg */
    case 0x7A: ret = (int32_t)sif_get_reg(a0); break;                                       /* SifGetReg */
    case 0x7C:                                   /* Deci2Call: dev-kit debug channel, absent on retail */
        ret = -1;
        break;
    case 0x02: {                                 /* SetGsCrt(interlace, mode, ffmd) */
        uint64_t smode2 = (a0 & 1) | ((uint64_t)(a2 & 1) << 1);
        gs_priv_access(0x12000020, &smode2, 8, 1);
        break;
    }
    case 0x70: {                                 /* GsGetIMR */
        uint64_t imr = 0;
        gs_priv_access(0x12001010, &imr, 8, 0);
        ret = (int64_t)imr;
        break;
    }
    case 0x71: {                                 /* GsPutIMR(imr) */
        uint64_t imr = a0;
        gs_priv_access(0x12001010, &imr, 8, 1);
        break;
    }
    case 0x7F:                                   /* GetMemorySize */
        ret = PS2_RAM_SIZE;
        break;
    default: {
        static uint32_t seen[512];
        if (seen[n & 511]++ < 3)
            fprintf(stderr, "[ps2] syscall 0x%X (a0=0x%08X a1=0x%08X) at 0x%08X not implemented%s\n",
                    (unsigned)n, a0, a1, pc, seen[n & 511] == 3 ? " (further calls not logged)" : "");
        break;
    }
    }
done:
    ctx->r[REG_V0].sd[0] = ret;
    if (in_interrupt) return;                    /* i-syscalls: the kernel acts after the handler */
    sif_deliver(ctx);                            /* IOP replies arrive "after" the syscall */
    PS2_POLL_IRQ(ctx);
    thread_reschedule();
}

/* ---- ELF loading + entry ------------------------------------------------------------------- */

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void load_elf(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ps2] cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) die("cannot read ELF (%u bytes)", (uint32_t)size);
    fclose(f);

    if (size < 52 || memcmp(buf, "\x7f" "ELF", 4) != 0) die("not an ELF file (%u)", 0);
    uint32_t phoff = rd32(buf + 28);
    uint16_t phentsize = rd16(buf + 42), phnum = rd16(buf + 44);
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t* ph = buf + phoff + (size_t)i * phentsize;
        if (rd32(ph) != 1) continue;             /* PT_LOAD */
        uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        uint8_t* dst = ps2_fast(vaddr);
        if (!dst || (vaddr & (PS2_RAM_SIZE - 1)) + memsz > PS2_RAM_SIZE) die("segment at 0x%08X outside RAM", vaddr);
        memcpy(dst, buf + off, filesz);
        memset(dst + filesz, 0, memsz - filesz);
    }
    free(buf);
}

/* A crash in any thread: say what and where (as an offset into the executable, for llvm-nm/objdump),
 * then let Windows end the process as usual. */
static LONG WINAPI crash_report(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL), pc = (uintptr_t)er->ExceptionAddress;
    fprintf(stderr, "\n[ps2] crash: exception 0x%08lX at %p (exe+0x%llX) in thread %lu", (unsigned long)er->ExceptionCode,
            er->ExceptionAddress, (unsigned long long)(pc - base), (unsigned long)GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, ", %s address 0x%llX", er->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)er->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A game must not be treated as background work when its window is not in front: Windows 11 then
 * runs a process at low QoS ("EcoQoS": efficiency cores, low clocks) and may ignore its timer
 * resolution. Opt out of both (SetProcessInformation(ProcessPowerThrottling), looked up at run
 * time: Windows 10 1709+). PS2_HIGH_QOS=0 keeps Windows' default. */
static void request_high_qos(void) {
    const char* e = getenv("PS2_HIGH_QOS");
    if (e && atoi(e) == 0) return;
    typedef struct { ULONG version, control, state; } throttling;            /* PROCESS_POWER_THROTTLING_STATE */
    typedef BOOL(WINAPI * set_info_fn)(HANDLE, int, LPVOID, DWORD);
    set_info_fn set_info = (set_info_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (!set_info) return;
    enum { PROCESS_POWER_THROTTLING = 4, EXECUTION_SPEED = 0x1, IGNORE_TIMER_RESOLUTION = 0x4 };
    throttling t = { 1, EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, 0 };     /* controlled, and off */
    if (!set_info(GetCurrentProcess(), PROCESS_POWER_THROTTLING, &t, sizeof t)) {
        t.control = EXECUTION_SPEED;                                          /* before Windows 11 */
        set_info(GetCurrentProcess(), PROCESS_POWER_THROTTLING, &t, sizeof t);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <path to original PS2 executable>\n", argv[0]); return 2; }
    SetUnhandledExceptionFilter(crash_report);
    request_high_qos();
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_start);
    perf_init();                                 /* PS2_PERF / PS2_PROFILE */
    atexit(perf_final);
    ps2_ram = calloc(1, PS2_RAM_SIZE + 16);      /* +16: a 128-bit access at the very end */
    ps2_spr = calloc(1, PS2_SPR_SIZE + 16);
    if (!ps2_ram || !ps2_spr) die("out of memory (%u)", 0);
    load_elf(argv[1]);
    iop_init();                                  /* IOP memory map before the EE's first IOP request */

    int dump_regs = argc > 2 && strcmp(argv[2], "--dump-regs") == 0;
    ps2_log_sif = getenv("PS2_LOG_SIF") != NULL;
    watch_init();
    if (warp_init()) ps2_hooks_on = 1;
    headless = dump_regs || getenv("PS2_HEADLESS") != NULL;
    dump_dir = getenv("PS2_DUMP_FRAMES");
    if (getenv("PS2_DUMP_EVERY")) dump_every = atoi(getenv("PS2_DUMP_EVERY")) > 0 ? atoi(getenv("PS2_DUMP_EVERY")) : 60;
    if (getenv("PS2_DUMP_FROM")) dump_from = (uint32_t)atoi(getenv("PS2_DUMP_FROM"));
    const char* wd = getenv("PS2_WATCHDOG");
    if (wd && atoi(wd) > 0) CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)atoi(wd), 0, NULL);
    if (!headless) window_init();
    if (!dump_regs) audio_init();                /* sound output thread (sd.c mixer -> WASAPI) */

    /* The GS on its own thread plus pixel workers (PS2_GS_ASYNC=0: synchronous, as in the unit
     * tests; PS2_GS_THREADS=<n>: pixel workers, default half the logical processors minus one, at
     * most 8). */
    const char* ga = getenv("PS2_GS_ASYNC");
    if (!dump_regs && !(ga && atoi(ga) == 0)) {
        int ncpu = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        int workers = ncpu / 2 - 1;
        if (workers > 8) workers = 8;
        if (workers < 0) workers = 0;
        const char* gt = getenv("PS2_GS_THREADS");
        if (gt) workers = atoi(gt);
        gs_thread_start(workers, present_frame);
        if (gs_async()) fprintf(stderr, "[ps2] GS thread started with %d pixel workers\n", workers);
        else fprintf(stderr, "[ps2] GS thread could not be started: the GS runs on the EE thread\n");
    }

    static ps2_ctx init;
    init.r[29].sd[0] = (int32_t)(PS2_RAM_SIZE - 0x10);   /* provisional stack until SetupThread */
    init.vf[0].f[3] = 1.0f;                              /* VU0 vf0 = (0, 0, 0, 1) */
    init.ie = 1;
    threads_init(&init);                                 /* main runs as thread 1 */
    ps2_ctx* ctx = thread_ctx();
    if (!dump_regs) CreateThread(NULL, 0, host_timer, NULL, 0, NULL);
    ps2_call(ctx, ps2_entry_point);
    fprintf(stderr, "[ps2] entry point returned\n");
    if (dump_regs)
        for (int i = 1; i < 32; i++)                     /* full 128 bits, high half first */
            printf("r%d=%016llX%016llX\n", i, (unsigned long long)ctx->r[i].ud[1],
                   (unsigned long long)ctx->r[i].ud[0]);
    return 0;
}
