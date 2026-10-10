/* Runtime for recompiled PS2 code: memory, ELF loading, function lookup, BIOS syscalls (HLE). */
#include "ps2_internal.h"

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

static void die(const char* fmt, uint32_t v) {
    fprintf(stderr, "[ps2] fatal: ");
    fprintf(stderr, fmt, v);
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- hardware registers ------------------------------------------------------------------- */

/* Log the first access to each unhandled register address only. */
static int first_time(uint32_t addr) {
    static uint32_t seen[1024];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == addr) return 0;
    if (n < 1024) seen[n++] = addr;
    return 1;
}

void ps2_hw_read(ps2_ctx* ctx, uint32_t addr, void* out, unsigned size) {
    (void)ctx;
    uint32_t phys = addr & 0x1FFFFFFFu;
    memset(out, 0, size);
    if (gs_is_priv(phys)) {
        uint64_t v = gs_read(phys & ~7u);
        memcpy(out, (uint8_t*)&v + (phys & 7), size < 8 ? size : 8);
        return;
    }
    if (first_time(phys)) fprintf(stderr, "[ps2] hw read%u  0x%08X (unhandled, returns 0)\n", size * 8, addr);
}

void ps2_hw_write(ps2_ctx* ctx, uint32_t addr, const void* in, unsigned size) {
    (void)ctx;
    uint32_t phys = addr & 0x1FFFFFFFu;
    uint64_t v = 0;
    memcpy(&v, in, size < 8 ? size : 8);
    if (gs_is_priv(phys)) { gs_write(phys & ~7u, v); return; }
    if (first_time(phys))
        fprintf(stderr, "[ps2] hw write%u 0x%08X = 0x%llX (unhandled)\n", size * 8, addr, (unsigned long long)v);
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

static uint32_t cop0_count;

uint32_t ps2_mfc0(ps2_ctx* ctx, int reg) {
    if (reg == 9) return cop0_count += 1000;     /* Count: just make time move forward */
    if (reg == 12) return 0x70000001u | (ctx->ie ? 0x10000u : 0);   /* Status: CU0-2, IE, EIE */
    return 0;
}

void ps2_mtc0(ps2_ctx* ctx, int reg, uint32_t v) {
    if (reg == 9) cop0_count = v;
    if (reg == 12) ctx->ie = (v >> 16) & 1;      /* Status.EIE */
}

/* ---- timed interrupts ------------------------------------------------------------------------ */

#define IRQ_VBLANK 1u
volatile uint32_t ps2_irq_pending;
static uint32_t vblank_count;

static DWORD WINAPI vblank_timer(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (uint64_t frame = 1;; frame++) {           /* NTSC: 59.94 fields per second */
        int64_t due = start.QuadPart + (int64_t)(frame * 1001 * freq.QuadPart / 60000);
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= due) break;
            Sleep(1);
        }
        InterlockedOr((volatile LONG*)&ps2_irq_pending, IRQ_VBLANK);
    }
    return 0;
}

void ps2_service_irq(ps2_ctx* ctx) {
    if (!ctx->ie || in_interrupt) return;          /* stays pending until interrupts are on */
    uint32_t p = (uint32_t)InterlockedExchange((volatile LONG*)&ps2_irq_pending, 0);
    if (p & IRQ_VBLANK) {
        vblank_count++;
        gs_vsync();
        ps2_raise_intc(ctx, 2);                    /* INTC_VBLANK_S */
        ps2_raise_intc(ctx, 3);                    /* INTC_VBLANK_E */
    }
    sif_deliver(ctx);
    thread_reschedule();                           /* handlers may have woken a thread */
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
static handler dmac_handlers[16][4], intc_handlers[16][4];
static uint32_t dmac_enabled, intc_enabled;

static int add_handler(handler list[4], uint32_t fn, uint32_t arg) {
    for (int i = 0; i < 4; i++)
        if (!list[i].used) { list[i] = (handler){ fn, arg, 1 }; return i + 1; }
    fprintf(stderr, "[ps2] too many interrupt handlers\n");
    return -1;
}

/* Run the game's handlers for one interrupt source as a real interrupt would: on a separate
 * stack area, with every register restored afterwards. */
static int raise(ps2_ctx* ctx, handler list[4], uint32_t enabled, int src) {
    if (!(enabled & (1u << src)) || in_interrupt) return 0;
    int any = 0;
    for (int i = 0; i < 4; i++) any |= list[i].used;
    if (!any) return 0;
    in_interrupt = 1;
    static ps2_ctx saved;
    for (int i = 0; i < 4; i++) {
        if (!list[i].used) continue;
        saved = *ctx;
        ctx->r[REG_A0].sd[0] = src;
        ctx->r[REG_A1].sd[0] = (int32_t)list[i].arg;
        ctx->r[REG_A2].sd[0] = 0;
        ctx->r[REG_SP].sd[0] = (int32_t)((ctx->r[REG_SP].uw[0] - 0x4000u) & ~15u);
        ps2_call(ctx, list[i].fn);
        *ctx = saved;
    }
    in_interrupt = 0;
    return 1;
}

int ps2_raise_dmac(ps2_ctx* ctx, int channel) { return raise(ctx, dmac_handlers[channel & 15], dmac_enabled, channel & 15); }
int ps2_raise_intc(ps2_ctx* ctx, int cause)   { return raise(ctx, intc_handlers[cause & 15], intc_enabled, cause & 15); }

static int remove_handler(handler list[4], uint32_t id) {
    if (id < 1 || id > 4 || !list[id - 1].used) return -1;
    list[id - 1].used = 0;
    return 0;
}

/* ---- BIOS syscalls (high-level emulation) --------------------------------------------------- */

int ps2_in_interrupt(void) { return in_interrupt; }

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
        break;
    case 0x10: ret = add_handler(intc_handlers[a0 & 15], a1, ctx->r[REG_A3].uw[0]); break;   /* AddIntcHandler */
    case 0x11: ret = remove_handler(intc_handlers[a0 & 15], a1); break;                     /* RemoveIntcHandler */
    case 0x12: ret = add_handler(dmac_handlers[a0 & 15], a1, ctx->r[REG_A3].uw[0]); break;   /* AddDmacHandler */
    case 0x13: ret = remove_handler(dmac_handlers[a0 & 15], a1); break;                     /* RemoveDmacHandler */
    case 0x14: intc_enabled |= 1u << (a0 & 15); ret = 1; break;                             /* EnableIntc */
    case 0x15: intc_enabled &= ~(1u << (a0 & 15)); ret = 1; break;                          /* DisableIntc */
    case 0x16: dmac_enabled |= 1u << (a0 & 15); ret = 1; break;                             /* EnableDmac */
    case 0x17: dmac_enabled &= ~(1u << (a0 & 15)); ret = 1; break;                          /* DisableDmac */
    case 0x76: ret = -1; break;                  /* SifDmaStat: every transfer completes at once */
    case 0x77: ret = sif_set_dma(ctx, a0, a1); break;                                       /* SifSetDma */
    case 0x78: break;                            /* SifSetDChain: re-arms SIF0 receive; nothing to do */
    case 0x79: sif_set_reg(a0, a1); ret = 1; break;                                         /* SifSetReg */
    case 0x7A: ret = (int32_t)sif_get_reg(a0); break;                                       /* SifGetReg */
    case 0x7C:                                   /* Deci2Call: dev-kit debug channel, absent on retail */
        ret = -1;
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

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <path to original PS2 executable>\n", argv[0]); return 2; }
    ps2_ram = calloc(1, PS2_RAM_SIZE + 16);      /* +16: a 128-bit access at the very end */
    ps2_spr = calloc(1, PS2_SPR_SIZE + 16);
    if (!ps2_ram || !ps2_spr) die("out of memory (%u)", 0);
    load_elf(argv[1]);

    ps2_log_sif = getenv("PS2_LOG_SIF") != NULL;
    const char* wd = getenv("PS2_WATCHDOG");
    if (wd && atoi(wd) > 0) CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)atoi(wd), 0, NULL);

    static ps2_ctx init;
    init.r[29].sd[0] = (int32_t)(PS2_RAM_SIZE - 0x10);   /* provisional stack until SetupThread */
    init.vf[0].f[3] = 1.0f;                              /* VU0 vf0 = (0, 0, 0, 1) */
    init.ie = 1;
    threads_init(&init);                                 /* main runs as thread 1 */
    ps2_ctx* ctx = thread_ctx();
    CreateThread(NULL, 0, vblank_timer, NULL, 0, NULL);
    ps2_call(ctx, ps2_entry_point);
    fprintf(stderr, "[ps2] entry point returned\n");
    if (argc > 2 && strcmp(argv[2], "--dump-regs") == 0)
        for (int i = 1; i < 32; i++)                     /* full 128 bits, high half first */
            printf("r%d=%016llX%016llX\n", i, (unsigned long long)ctx->r[i].ud[1],
                   (unsigned long long)ctx->r[i].ud[0]);
    return 0;
}
