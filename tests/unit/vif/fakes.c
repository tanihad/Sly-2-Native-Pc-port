/* Small fakes of the units vif.c talks to: VU memory + VU1 start, GIF PATH2/PATH3 mask,
 * GS local->host data, INTC, logging. State is exposed through fakes.h for the tests. */
#include "fakes.h"

#include <stdio.h>
#include <string.h>

uint8_t fake_vu_data[2][16384];
uint8_t fake_vu_micro[2][16384];

uint8_t* vu_data_mem(int vu) { return fake_vu_data[vu ? 1 : 0]; }
uint8_t* vu_micro_mem(int vu) { return fake_vu_micro[vu ? 1 : 0]; }

int      fake_vu1_starts;
uint32_t fake_vu1_start_addr[64];
int      fake_vu1_busy;
uint32_t fake_tops_top[2], fake_tops_itop[2];
int      fake_tops_calls[2];
uint32_t fake_top_at_start[64];

void vu1_start(uint32_t addr_dw) {
    if (fake_vu1_starts < 64) {
        fake_vu1_start_addr[fake_vu1_starts] = addr_dw;
        fake_top_at_start[fake_vu1_starts] = fake_tops_top[1];
    }
    fake_vu1_starts++;
}
int vu1_busy(void) { return fake_vu1_busy; }
uint32_t fake_vu_tpc[2];
uint32_t vu_get_tpc(int vu) { return fake_vu_tpc[vu ? 1 : 0]; }
int      fake_vu0_starts;
uint32_t fake_vu0_start_addr;
void vu0_callms(ps2_ctx* ctx, uint32_t addr_dw) { (void)ctx; fake_vu0_starts++; fake_vu0_start_addr = addr_dw; }
ps2_ctx* thread_ctx(void) { static ps2_ctx c; return &c; }
void vu_set_vif_tops(int vu, uint32_t top, uint32_t itop) {
    vu = vu ? 1 : 0;
    fake_tops_top[vu] = top;
    fake_tops_itop[vu] = itop;
    fake_tops_calls[vu]++;
}

uint8_t  fake_gif[1 << 20];
uint32_t fake_gif_qwc;
int      fake_gif_limit = -1;      /* qwords accepted per call; -1 = all */
int      fake_gif_misaligned;
int      fake_path3_mask = -1;

uint32_t gif_path2(const void* qw, uint32_t qwc) {
    if ((uintptr_t)qw & 15) fake_gif_misaligned++;
    uint32_t n = qwc;
    if (fake_gif_limit >= 0 && n > (uint32_t)fake_gif_limit) n = (uint32_t)fake_gif_limit;
    if ((fake_gif_qwc + n) * 16 <= sizeof fake_gif) memcpy(fake_gif + fake_gif_qwc * 16, qw, n * 16);
    fake_gif_qwc += n;
    return n;
}
void gif_set_path3_mask(int masked) { fake_path3_mask = masked; }

uint32_t fake_gs_avail;            /* bytes the GS still has to send */
uint32_t fake_gs_sent;
uint32_t gs_image_read(void* data, uint32_t bytes) {
    uint32_t n = bytes < fake_gs_avail ? bytes : fake_gs_avail;
    for (uint32_t i = 0; i < n; i++) ((uint8_t*)data)[i] = (uint8_t)(fake_gs_sent + i);
    fake_gs_sent += n;
    fake_gs_avail -= n;
    return n;
}

uint32_t fake_intc;
int      fake_intc_count[16];
void intc_raise(int cause) { fake_intc |= 1u << cause; fake_intc_count[cause & 15]++; }

int fake_unhandled;
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)data; (void)size; (void)is_write; (void)phys;
    fake_unhandled++;
}

void fakes_reset(void) {
    memset(fake_vu_data, 0, sizeof fake_vu_data);
    memset(fake_vu_micro, 0, sizeof fake_vu_micro);
    fake_vu1_starts = 0;
    fake_vu1_busy = 0;
    memset(fake_tops_top, 0, sizeof fake_tops_top);
    memset(fake_tops_itop, 0, sizeof fake_tops_itop);
    memset(fake_tops_calls, 0, sizeof fake_tops_calls);
    fake_gif_qwc = 0;
    fake_gif_limit = -1;
    fake_gif_misaligned = 0;
    fake_path3_mask = -1;
    fake_gs_avail = 0;
    fake_gs_sent = 0;
    fake_intc = 0;
    memset(fake_intc_count, 0, sizeof fake_intc_count);
    fake_unhandled = 0;
}
