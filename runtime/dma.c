/* DMA controller. STUB: to be implemented (owner: dma agent). */
#include "hw.h"

void dma_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (phys == 0x1000E010) {                    /* D_STAT lives in hw.c */
        if (is_write) { uint32_t v; memcpy(&v, data, 4); dmac_stat_write(v); }
        else { uint32_t v = dmac_stat_read(); memset(data, 0, size); memcpy(data, &v, size < 4 ? size : 4); }
        return;
    }
    if (!is_write) memset(data, 0, size);
    hw_log_unhandled("dma", phys, data, size, is_write);
}

void dma_service(void) {}
