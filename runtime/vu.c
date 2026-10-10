/* VU0/VU1 micro mode and VU memory. STUB: to be implemented (owner: vu agent). */
#include "hw.h"

static uint8_t vu0_data[4096], vu0_micro[4096], vu1_data[16384], vu1_micro[16384];

uint8_t* vu_data_mem(int vu) { return vu ? vu1_data : vu0_data; }
uint8_t* vu_micro_mem(int vu) { return vu ? vu1_micro : vu0_micro; }

void vu_mem_access(uint32_t phys, void* data, unsigned size, int is_write) {
    uint32_t off = phys - 0x11000000u;
    uint8_t* mem = NULL;
    uint32_t len = 0;
    if (off < 0x4000)       { mem = vu0_micro; len = sizeof vu0_micro; }
    else if (off < 0x8000)  { mem = vu0_data;  len = sizeof vu0_data;  off -= 0x4000; }
    else if (off < 0xC000)  { mem = vu1_micro; len = sizeof vu1_micro; off -= 0x8000; }
    else                    { mem = vu1_data;  len = sizeof vu1_data;  off -= 0xC000; }
    off &= len - 1;
    if (off + size > len) { if (!is_write) memset(data, 0, size); return; }
    if (is_write) memcpy(mem + off, data, size); else memcpy(data, mem + off, size);
}

void vu1_start(uint32_t addr_dw) { (void)addr_dw; }
int vu1_busy(void) { return 0; }
void vu_set_vif_tops(int vu, uint32_t top, uint32_t itop) { (void)vu; (void)top; (void)itop; }

void vu0_callms(ps2_ctx* ctx, uint32_t addr_dw) {
    (void)ctx;
    static int logged;
    if (!logged++) fprintf(stderr, "[vu] VU0 microprogram at 0x%X not run (stub)\n", addr_dw * 8);
}
uint32_t vu0_cfc2_ctrl(ps2_ctx* ctx, int reg) { (void)ctx; (void)reg; return 0; }
void vu0_ctc2_ctrl(ps2_ctx* ctx, int reg, uint32_t v) { (void)ctx; (void)reg; (void)v; }
