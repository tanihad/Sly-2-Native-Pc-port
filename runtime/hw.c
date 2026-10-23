/* Hardware register dispatch, interrupt controller state (INTC + DMAC D_STAT), debug UART. */
#include "hw.h"
#include "gfx_async.h"

#include <windows.h>

/* ---- logging --------------------------------------------------------------------------------- */

void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    static uint32_t seen[2048];
    static int n;
    uint32_t key = phys ^ (is_write ? 0x80000000u : 0);
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 2048) seen[n++] = key;
    uint64_t v = 0;
    if (is_write) memcpy(&v, data, size < 8 ? size : 8);
    if (is_write)
        fprintf(stderr, "[%s] unhandled write%u 0x%08X = 0x%llX\n", unit, size * 8, phys, (unsigned long long)v);
    else
        fprintf(stderr, "[%s] unhandled read%u 0x%08X\n", unit, size * 8, phys);
}

/* ---- interrupt controller -------------------------------------------------------------------- */

static uint32_t i_stat, i_mask;          /* INTC */
static uint32_t d_stat;                  /* DMAC: CIS 0-9, SIS 13, MEIS 14, BEIS 15 | CIM 16-25, SIM 29, MEIM 30 */

static void mark_pending(void) { InterlockedOr((volatile LONG*)&ps2_irq_pending, PS2_IRQ_HW); }

/* Pending DMAC causes: channel CIS bits 0-9 and SIS (13) / MEIS (14) under their masks, plus
 * BEIS (15, bus error), which cannot be masked. */
uint32_t dmac_pending(void) { return (d_stat & 0x63FFu & (d_stat >> 16)) | (d_stat & 0x8000u); }

/* (pc-fps2) raised on the graphics worker (gfx_async.h): applied on the EE when its job ends */
void intc_raise(int cause) {
    if (gfx_on_worker()) { gfx_defer_fn(GFX_EV_INTC, (uint32_t)cause, 0); return; }
    if (gfx_quiet && (cause == INTC_GS || cause == INTC_VIF1)) return;
    i_stat |= 1u << cause;
    if (i_mask & (1u << cause)) mark_pending();
}
void dmac_raise(int ch) {
    if (gfx_on_worker()) { gfx_defer_fn(GFX_EV_DMAC, (uint32_t)ch, 0); return; }
    if (gfx_quiet && (ch == DMA_VIF1 || ch == DMA_GIF)) return;
    d_stat |= 1u << ch;
    if (dmac_pending() & (1u << ch)) mark_pending();
}

uint32_t dmac_stat_read(void) { return d_stat; }
void dmac_stat_write(uint32_t v) {
    d_stat &= ~(v & 0xFFFFu);            /* status bits: write 1 to clear */
    d_stat ^= v & 0xFFFF0000u;           /* mask bits: write 1 to toggle */
    if (dmac_pending()) mark_pending();
}

uint32_t intc_pending(void) { return i_stat & i_mask; }
void     intc_ack(int cause) { i_stat &= ~(1u << cause); }
void     intc_set_mask(int cause, int on) {
    if (on) i_mask |= 1u << cause; else i_mask &= ~(1u << cause);
    if (i_stat & i_mask) mark_pending();
}
void     dmac_ack(int ch) { d_stat &= ~(1u << ch); }
void     dmac_set_mask(int ch, int on) {
    if (on) d_stat |= 1u << (ch + 16); else d_stat &= ~(1u << (ch + 16));
    if (dmac_pending()) mark_pending();
}
int      dmac_mask_enabled(int ch) { return (d_stat >> (ch + 16)) & 1; }

static void intc_access(uint32_t phys, void* data, unsigned size, int is_write) {
    uint32_t* reg = (phys & 0xF0) == 0x00 ? &i_stat : (phys & 0xF0) == 0x10 ? &i_mask : NULL;
    if (!reg) { hw_log_unhandled("intc", phys, data, size, is_write); if (!is_write) memset(data, 0, size); return; }
    if (!is_write) { memset(data, 0, size); memcpy(data, reg, size < 4 ? size : 4); return; }
    uint32_t v = 0;
    memcpy(&v, data, size < 4 ? size : 4);
    if (reg == &i_stat) i_stat &= ~v;     /* write 1 to acknowledge */
    else i_mask ^= v;                     /* write 1 to toggle */
    if (i_stat & i_mask) mark_pending();
}

/* ---- debug UART (SIO at 0x1000F100): games' printf output ----------------------------------- */

static void sio_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!is_write) {
        memset(data, 0, size);
        if (phys == 0x1000F130) { uint32_t v = 0x00000000; memcpy(data, &v, size < 4 ? size : 4); }  /* ISR: TX ready */
        return;
    }
    if (phys == 0x1000F180) {             /* TXFIFO */
        static char line[512];
        static int len;
        char c = *(const char*)data;
        if (c == '\n' || len == (int)sizeof line - 1) { line[len] = 0; fprintf(stderr, "[ee] %s\n", line); len = 0; }
        else if (c != '\r') line[len++] = c;
    }
}

/* ---- dispatch ---------------------------------------------------------------------------------- */

static void rom_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!is_write) memset(data, 0, size);      /* no BIOS image: reads return zeros */
    hw_log_unhandled("rom", phys, data, size, is_write);
}

static const struct { uint32_t lo, hi; hw_access_fn fn; } regions[] = {
    { 0x10000000, 0x10002000, timer_access },
    { 0x10002000, 0x10003000, ipu_access },
    { 0x10003000, 0x10003800, gif_access },
    { 0x10003800, 0x10004000, vif_access },
    { 0x10004000, 0x10006000, vif_access },    /* VIF0 FIFO 0x10004000, VIF1 FIFO 0x10005000 */
    { 0x10006000, 0x10007000, gif_access },    /* GIF FIFO */
    { 0x10007000, 0x10008000, ipu_access },    /* IPU out FIFO 0x10007000, in FIFO 0x10007010 */
    { 0x10008000, 0x1000F000, dma_access },
    { 0x1000F000, 0x1000F100, intc_access },
    { 0x1000F100, 0x1000F500, sio_access },
    { 0x1000F500, 0x1000F600, dma_access },    /* D_ENABLER 0x1000F520, D_ENABLEW 0x1000F590 */
    { 0x11000000, 0x11010000, vu_mem_access },
    { 0x12000000, 0x12002000, gs_priv_access },
    { 0x1FC00000, 0x20000000, rom_access },
};

static hw_access_fn find(uint32_t phys) {
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; i++)
        if (phys >= regions[i].lo && phys < regions[i].hi) return regions[i].fn;
    return NULL;
}

void ps2_hw_read(ps2_ctx* ctx, uint32_t addr, void* out, unsigned size) {
    (void)ctx;
    uint32_t phys = addr & 0x1FFFFFFFu;
    hw_access_fn fn = find(phys);
    if (fn) { fn(phys, out, size, 0); return; }
    memset(out, 0, size);
    hw_log_unhandled("hw", phys, out, size, 0);
}

void ps2_hw_write(ps2_ctx* ctx, uint32_t addr, const void* in, unsigned size) {
    (void)ctx;
    uint32_t phys = addr & 0x1FFFFFFFu;
    hw_access_fn fn = find(phys);
    uint8_t buf[16];
    memcpy(buf, in, size);                     /* handlers take a mutable buffer */
    if (fn) { fn(phys, buf, size, 1); return; }
    hw_log_unhandled("hw", phys, in, size, 1);
}

/* ---- DMA memory -------------------------------------------------------------------------------- */

uint8_t* hw_dma_ptr(uint32_t addr, uint32_t bytes) {
    if (addr & 0x80000000u) {                  /* scratchpad */
        uint32_t off = addr & 0x3FF0u;
        return off + bytes <= PS2_SPR_SIZE ? ps2_spr + off : NULL;
    }
    uint32_t a = addr & 0x0FFFFFFFu;
    return a + bytes <= PS2_RAM_SIZE ? ps2_ram + a : NULL;
}
