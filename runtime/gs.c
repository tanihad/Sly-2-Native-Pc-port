/* GS registers, drawing and display. STUB: to be implemented (owner: gs-draw agent).
 * Keeps the privileged-register behaviour the boot already relies on (CSR VSync events). */
#include "gs_internal.h"

#define CSR_VSINT (1u << 3)
#define CSR_FIELD (1u << 13)

static uint64_t regs_lo[16];     /* 0x12000000-0x120000F0: PMODE ... BGCOLOR */
static uint64_t regs_hi[16];     /* 0x12001000-0x120010F0: CSR, IMR, BUSDIR, SIGLBLID */
static uint64_t csr = 0x0000551B00000000ull | (2u << 10);

void gs_priv_access(uint32_t phys, void* data, unsigned size, int is_write) {
    uint32_t off = phys - 0x12000000u;
    uint64_t* reg = off < 0x100 ? &regs_lo[(off >> 4) & 15]
                  : (off >= 0x1000 && off < 0x1100) ? &regs_hi[((off - 0x1000) >> 4) & 15] : NULL;
    if (off == 0x1000) reg = &csr;
    if (!reg) { if (!is_write) memset(data, 0, size); return; }
    unsigned sub = phys & 7, n = size > 8 - sub ? 8 - sub : size;
    if (!is_write) { memset(data, 0, size); memcpy(data, (uint8_t*)reg + sub, n); return; }
    uint64_t v = 0;
    memcpy((uint8_t*)&v + sub, data, n);
    if (reg == &csr) csr &= ~(v & 0x1Fu);        /* write 1 to acknowledge an event */
    else *reg = v;
}

void gs_write_reg(uint32_t reg, uint64_t value) { (void)reg; (void)value; }
void gs_image_write(const void* data, uint32_t bytes) { (void)data; (void)bytes; }
uint32_t gs_image_read(void* data, uint32_t bytes) { memset(data, 0, bytes); return bytes; }

void gs_vsync(void) {
    csr ^= CSR_FIELD;
    csr |= CSR_VSINT;
}

int gs_display(uint32_t* dst, int stride, int* w, int* h) { (void)dst; (void)stride; *w = *h = 0; return 0; }
