/* IPU MPEG decoder. STUB: to be implemented (owner: ipu agent). */
#include "hw.h"

void ipu_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!is_write) memset(data, 0, size);
    hw_log_unhandled("ipu", phys, data, size, is_write);
}

uint32_t ipu_dma_to(const void* qw, uint32_t qwc) { (void)qw; return qwc; }
uint32_t ipu_dma_from(void* qw, uint32_t qwc) { (void)qw; (void)qwc; return 0; }
