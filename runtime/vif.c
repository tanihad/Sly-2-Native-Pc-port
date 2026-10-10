/* VIF0/VIF1. STUB: to be implemented (owner: vif agent). */
#include "hw.h"

void vif_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!is_write) memset(data, 0, size);
    hw_log_unhandled("vif", phys, data, size, is_write);
}

uint32_t vif_dma(int vif, const void* qw, uint32_t qwc) { (void)vif; (void)qw; return qwc; }
int vif_dma_tag(int vif, const void* tag_qw) { (void)vif; (void)tag_qw; return 1; }
uint32_t vif1_read(void* qw, uint32_t qwc) { memset(qw, 0, qwc * 16u); return qwc; }
