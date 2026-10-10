/* Runtime for recompiled PS2 code: memory, ELF loading, function lookup, BIOS syscalls (HLE). */
#include "ps2_runtime.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t* ps2_ram;
uint8_t* ps2_spr;

static void die(const char* fmt, uint32_t v) {
    fprintf(stderr, "[ps2] fatal: ");
    fprintf(stderr, fmt, v);
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- hardware registers: log for now ----------------------------------------------------- */

void ps2_hw_read(ps2_ctx* ctx, uint32_t addr, void* out, unsigned size) {
    (void)ctx;
    fprintf(stderr, "[ps2] hw read%u  0x%08X\n", size * 8, addr);
    memset(out, 0, size);
}

void ps2_hw_write(ps2_ctx* ctx, uint32_t addr, const void* in, unsigned size) {
    (void)ctx;
    uint64_t v = 0;
    memcpy(&v, in, size < 8 ? size : 8);
    fprintf(stderr, "[ps2] hw write%u 0x%08X = 0x%llX\n", size * 8, addr, (unsigned long long)v);
}

/* ---- function lookup ---------------------------------------------------------------------- */

void ps2_call(ps2_ctx* ctx, uint32_t addr) {
    size_t lo = 0, hi = ps2_func_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ps2_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < ps2_func_count && ps2_funcs[lo].addr == addr) { ps2_funcs[lo].fn(ctx); return; }
    die("call to 0x%08X, which is not the start of a recompiled function", addr);
}

/* ---- COP0 / unimplemented ------------------------------------------------------------------ */

static uint32_t cop0_count;

uint32_t ps2_mfc0(ps2_ctx* ctx, int reg) {
    (void)ctx;
    if (reg == 9) return cop0_count += 1000;     /* Count: just make time move forward */
    return 0;
}

void ps2_mtc0(ps2_ctx* ctx, int reg, uint32_t v) {
    (void)ctx;
    if (reg == 9) cop0_count = v;
}

void ps2_unimpl(ps2_ctx* ctx, uint32_t pc, const char* op) {
    (void)ctx;
    static uint32_t seen[256]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == pc) return;
    if (n < 256) seen[n++] = pc;
    fprintf(stderr, "[ps2] unimplemented %s at 0x%08X\n", op, pc);
}

/* ---- BIOS syscalls (high-level emulation) --------------------------------------------------- */

#define REG_V0 2
#define REG_V1 3
#define REG_A0 4
#define REG_A1 5
#define REG_A2 6
#define REG_GP 28

void ps2_syscall(ps2_ctx* ctx, uint32_t pc) {
    int32_t n = ctx->r[REG_V1].sw[0];
    if (n < 0) n = -n;                           /* negative = interrupt-context ("i") variant */
    uint32_t a0 = ctx->r[REG_A0].uw[0], a1 = ctx->r[REG_A1].uw[0], a2 = ctx->r[REG_A2].uw[0];
    int64_t ret = 0;

    switch (n) {
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
    default:
        fprintf(stderr, "[ps2] syscall 0x%X (a0=0x%08X a1=0x%08X) at 0x%08X not implemented\n",
                (unsigned)n, a0, a1, pc);
        break;
    }
    ctx->r[REG_V0].sd[0] = ret;
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

    static ps2_ctx ctx;
    ctx.r[29].sd[0] = (int32_t)(PS2_RAM_SIZE - 0x10);    /* provisional stack until SetupThread */
    ps2_call(&ctx, ps2_entry_point);
    fprintf(stderr, "[ps2] entry point returned\n");
    if (argc > 2 && strcmp(argv[2], "--dump-regs") == 0)
        for (int i = 1; i < 32; i++) printf("r%d=%016llX\n", i, (unsigned long long)ctx.r[i].ud[0]);
    return 0;
}
