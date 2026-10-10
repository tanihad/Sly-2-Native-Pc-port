/* Fakes of the IPU's neighbours (INTC, DMAC, logging) for the IPU unit tests. */
#pragma once
#include "hw.h"
#include "ipu_internal.h"

#include <stdio.h>
#include <stdlib.h>

extern int fake_intc[16];                /* intc_raise() counts per cause */
extern int fake_unhandled;               /* hw_log_unhandled() calls */

/* A minimal DMAC: channel 4 (memory -> IPU) and channel 3 (IPU -> memory), serviced by
 * dma_service(). burst limits the qwords channel 4 offers per call (0 = unlimited). */
typedef struct {
    const uint8_t* to_src;  uint32_t to_qwc;
    uint8_t*       from_dst; uint32_t from_qwc;
    uint32_t       burst;
    uint32_t       jitter;               /* if set, each call offers 1..jitter qwords instead */
    uint64_t       jitter_state;
    uint64_t       moved;                /* qwords moved by either channel */
    uint64_t       services;             /* dma_service() calls */
} fake_dmac;
extern fake_dmac fake_dma;

/* register helpers */
enum { IPU_CMD = 0x10002000, IPU_CTRL = 0x10002010, IPU_BP = 0x10002020, IPU_TOP = 0x10002030,
       IPU_OUT_FIFO = 0x10007000, IPU_IN_FIFO = 0x10007010 };
static inline uint32_t ipu_rd32(uint32_t a) { uint32_t v; ipu_access(a, &v, 4, 0); return v; }
static inline uint64_t ipu_rd64(uint32_t a) { uint64_t v; ipu_access(a, &v, 8, 0); return v; }
static inline void ipu_wr32(uint32_t a, uint32_t v) { ipu_access(a, &v, 4, 1); }
static inline void ipu_fifo_write(const void* qw) { uint8_t b[16]; memcpy(b, qw, 16); ipu_access(IPU_IN_FIFO, b, 16, 1); }
static inline void ipu_fifo_read(void* qw) { ipu_access(IPU_OUT_FIFO, qw, 16, 0); }

/* test bookkeeping */
extern int test_failures;
#define CHECK(cond, ...) do { if (!(cond)) { test_failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* Deterministic random numbers (xorshift64*) */
static inline uint32_t test_rand(uint64_t* s) {
    *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
    return (uint32_t)((*s * 0x2545F4914F6CDD1DULL) >> 32);
}

/* Bit writer for building test bitstreams (MSB first, memory byte order). */
typedef struct { uint8_t* buf; size_t cap, bits; } bitwriter;
static inline void bw_put(bitwriter* w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        size_t byte = w->bits >> 3;
        if (byte >= w->cap) { fprintf(stderr, "bitwriter overflow\n"); exit(2); }
        if (!(w->bits & 7)) w->buf[byte] = 0;
        if (i < 32 && ((v >> i) & 1)) w->buf[byte] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}
static inline void bw_code(bitwriter* w, const char* code) {
    for (; *code; code++) if (*code != ' ') bw_put(w, *code == '1', 1);
}
