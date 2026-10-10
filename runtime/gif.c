/* GIF. STUB: to be implemented (owner: gif agent). */
#include "hw.h"

void gif_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!is_write) memset(data, 0, size);
    hw_log_unhandled("gif", phys, data, size, is_write);
}

uint32_t gif_path3_dma(const void* qw, uint32_t qwc) { (void)qw; return qwc; }
uint32_t gif_path2(const void* qw, uint32_t qwc) { (void)qw; return qwc; }
void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) { (void)vu1_data; (void)qw_addr; }
void gif_set_path3_mask(int masked) { (void)masked; }
