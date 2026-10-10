/* Fakes of the IPU's neighbours for the unit tests. */
#include "fakes.h"

int fake_intc[16];
int fake_unhandled;
int test_failures;
fake_dmac fake_dma;

void intc_raise(int cause) { fake_intc[cause & 15]++; }
void dmac_raise(int channel) { (void)channel; }

void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)data;
    fake_unhandled++;
    fprintf(stderr, "[%s] unhandled %s%u 0x%08X\n", unit, is_write ? "write" : "read", size * 8, phys);
}

void dma_service(void) {
    fake_dma.services++;
    if (fake_dma.to_qwc) {                       /* channel 4: memory -> IPU */
        uint32_t n = fake_dma.to_qwc, lim = fake_dma.burst;
        if (fake_dma.jitter) {
            if (!fake_dma.jitter_state) fake_dma.jitter_state = 88172645463325252ull;
            lim = 1 + test_rand(&fake_dma.jitter_state) % fake_dma.jitter;
        }
        if (lim && n > lim) n = lim;
        uint32_t done = ipu_dma_to(fake_dma.to_src, n);
        fake_dma.to_src += 16 * done;
        fake_dma.to_qwc -= done;
        fake_dma.moved += done;
    }
    if (fake_dma.from_qwc) {                     /* channel 3: IPU -> memory */
        uint32_t done = ipu_dma_from(fake_dma.from_dst, fake_dma.from_qwc);
        fake_dma.from_dst += 16 * done;
        fake_dma.from_qwc -= done;
        fake_dma.moved += done;
    }
}
