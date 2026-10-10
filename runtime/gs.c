/* GS (Graphics Synthesizer) privileged registers at 0x12000000. Writes are stored; reads give
 * them back, with CSR reflecting VSync events. No drawing yet. */
#include "ps2_internal.h"

#define CSR_VSINT (1u << 3)
#define CSR_FIELD (1u << 13)

static uint64_t regs_lo[16];     /* 0x12000000-0x120000F0: PMODE ... BGCOLOR */
static uint64_t regs_hi[16];     /* 0x12001000-0x120010F0: CSR, IMR, BUSDIR, SIGLBLID */
static uint64_t csr = 0x0000551B00000000ull | (2u << 10);   /* revision/id fields, FIFO empty */

static uint64_t* slot(uint32_t phys) {
    uint32_t off = phys - 0x12000000u;
    if (off < 0x100) return &regs_lo[off >> 4];
    if (off >= 0x1000 && off < 0x1100) return &regs_hi[(off - 0x1000) >> 4];
    return NULL;
}

int gs_is_priv(uint32_t phys) { return phys >= 0x12000000u && phys < 0x12002000u; }

uint64_t gs_read(uint32_t phys) {
    if (phys == 0x12001000u) return csr;
    uint64_t* s = slot(phys);
    return s ? *s : 0;
}

void gs_write(uint32_t phys, uint64_t v) {
    if (phys == 0x12001000u) {
        csr &= ~(v & 0x1Fu);                    /* writing 1 to an event bit acknowledges it */
        return;                                  /* (RESET, FLUSH etc. have nothing to do yet) */
    }
    uint64_t* s = slot(phys);
    if (s) *s = v;
}

/* Called at each vertical blank: flip the interlace field and raise the VSync event bit. */
void gs_vsync(void) {
    csr ^= CSR_FIELD;
    csr |= CSR_VSINT;
}
