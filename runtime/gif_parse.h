/* (pc-fps2) GIF data -> GS register writes, shared by gif.c (PATH1/2/3 on the EE or the graphics worker) and
 * gs_thread.c (whole PATH1 packets parsed on the GS thread: gif_packet_run). One definition of the conversions, so both
 * produce the same register stream. The output functions are compile-time constants at every call site (inlined). */
#pragma once
#include <stdint.h>
#include <string.h>

enum { GIFP_PRIM = 0x00, GIFP_RGBAQ = 0x01, GIFP_ST = 0x02, GIFP_UV = 0x03, GIFP_XYZF2 = 0x04, GIFP_XYZ2 = 0x05,
       GIFP_FOG = 0x0A, GIFP_XYZF3 = 0x0C, GIFP_XYZ3 = 0x0D, GIFP_HWREG = 0x54 };

typedef void (*gifp_reg_fn)(uint32_t r, uint64_t v);
typedef void (*gifp_img_fn)(const void* p, uint32_t bytes);

/* PS2_GIF_CHECK: a hash of the register stream (register, value) and the IMAGE bytes (independent of their chunking) */
#define GIFP_HASH_SEED 0xCBF29CE484222325ull
static inline uint64_t gifp_hash_reg(uint64_t h, uint32_t r, uint64_t v) {
    h = (h ^ r) * 0x100000001B3ull;
    return (h ^ v) * 0x100000001B3ull;
}
static inline uint64_t gifp_hash_bytes(uint64_t h, const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/* A+D: HWREG carries host->local transfer data */
static inline __attribute__((always_inline)) void gifp_ad(uint32_t reg, uint64_t v, gifp_reg_fn out, gifp_img_fn img) {
    if (reg == GIFP_HWREG) img(&v, 8);
    else out(reg, v);
}

/* One PACKED descriptor; *q is the packed-mode Q (float bits) */
static inline __attribute__((always_inline)) void gifp_packed(uint32_t* q, uint32_t desc, uint64_t d0, uint64_t d1,
                                                               gifp_reg_fn out, gifp_img_fn img) {
    switch (desc) {
    case 0x0: out(GIFP_PRIM, d0 & 0x7FF); break;                              /* PRIM */
    case 0x1:                                                                  /* RGBAQ */
        out(GIFP_RGBAQ, (d0 & 0xFF) | ((d0 >> 24) & 0xFF00) | ((d1 & 0xFF) << 16) | ((d1 >> 8) & 0xFF000000ull) |
                        ((uint64_t)*q << 32));
        break;
    case 0x2: *q = (uint32_t)d1; out(GIFP_ST, d0); break;                     /* ST (+ Q for the next RGBAQ) */
    case 0x3: out(GIFP_UV, (d0 & 0x3FFF) | ((d0 >> 16) & 0x3FFF0000ull)); break;   /* UV */
    case 0x4:                                                                  /* XYZF2 / XYZF3 */
        out((d1 >> 47) & 1 ? GIFP_XYZF3 : GIFP_XYZF2,
            (d0 & 0xFFFF) | ((d0 >> 16) & 0xFFFF0000ull) | (((d1 >> 4) & 0xFFFFFF) << 32) | (((d1 >> 36) & 0xFF) << 56));
        break;
    case 0x5:                                                                  /* XYZ2 / XYZ3 */
        out((d1 >> 47) & 1 ? GIFP_XYZ3 : GIFP_XYZ2, (d0 & 0xFFFF) | ((d0 >> 16) & 0xFFFF0000ull) | (d1 << 32));
        break;
    case 0xA: out(GIFP_FOG, ((d1 >> 36) & 0xFF) << 56); break;                /* FOG */
    case 0xE: gifp_ad((uint32_t)d1 & 0xFF, d0, out, img); break;              /* A+D */
    case 0xF: break;                                                           /* NOP */
    default: out(desc, d0); break;                                             /* TEX0_1/2, CLAMP_1/2, 0xB, XYZF3, XYZ3 */
    }
}

/* Length in qwords of the GIF packet at qword `addr` of a ring of `ring_qw` qwords (VU1 data memory), up to and including
 * its EOP tag; 0 = no EOP within `max` qwords, or the packet must not leave the EE's timeline: an A+D write of SIGNAL /
 * FINISH / LABEL (0x60-0x62; they change CSR on the EE) - then it goes the register-by-register way. *last_tag gets the
 * packet's last GIFtag (GIF_TAG0-3). */
static inline uint32_t gifp_packet_len(const uint8_t* mem, uint32_t addr, uint32_t ring_qw, uint32_t max, uint64_t last_tag[2]) {
    uint32_t n = 0;
    for (;;) {
        if (n >= max) return 0;
        const uint8_t* t = mem + (size_t)((addr + n) & (ring_qw - 1)) * 16;
        uint64_t lo, hi;
        memcpy(&lo, t, 8);
        memcpy(&hi, t + 8, 8);
        last_tag[0] = lo;
        last_tag[1] = hi;
        n++;
        uint32_t nloop = (uint32_t)lo & 0x7FFF, flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60);
        if (!nreg) nreg = 16;
        uint32_t data;
        if (flg >= 2) data = nloop;                                            /* IMAGE */
        else if (flg == 1) data = (nloop * nreg + 1) / 2;                      /* REGLIST (A+D outputs nothing) */
        else {                                                                 /* PACKED */
            data = nloop * nreg;
            for (uint32_t r = 0; r < nreg; r++) {
                if (((hi >> (r * 4)) & 0xF) != 0xE) continue;
                for (uint32_t l = 0; l < nloop && n + l * nreg + r < max; l++) {
                    uint8_t reg = mem[(size_t)((addr + n + l * nreg + r) & (ring_qw - 1)) * 16 + 8];
                    if (reg >= 0x60 && reg <= 0x62) return 0;
                }
            }
        }
        n += data;
        if ((lo >> 15) & 1) return n <= max ? n : 0;                           /* EOP */
    }
}

/* Run a whole packet (contiguous qwords, starting with a GIFtag, ending with its EOP tag's data): exactly what gif.c's
 * PATH1 parser sends for it. */
static inline __attribute__((always_inline)) void gifp_packet_run(const uint8_t* src, uint32_t qwc, gifp_reg_fn out, gifp_img_fn img) {
    uint32_t i = 0;
    while (i < qwc) {
        uint64_t lo, hi;
        memcpy(&lo, src + (size_t)i * 16, 8);
        memcpy(&hi, src + (size_t)i * 16 + 8, 8);
        i++;
        uint32_t nloop = (uint32_t)lo & 0x7FFF, flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60);
        if (!nreg) nreg = 16;
        if (flg > 2) flg = 2;
        uint32_t q = 0x3F800000u;                                              /* every GIFtag resets Q to 1.0f */
        if (nloop && flg == 0 && ((lo >> 46) & 1)) out(GIFP_PRIM, (lo >> 47) & 0x7FF);   /* PRE */
        if (!nloop) continue;
        if (flg == 2) {                                                        /* IMAGE */
            uint32_t n = qwc - i < nloop ? qwc - i : nloop;
            img(src + (size_t)i * 16, n * 16);
            i += n;
        } else if (flg == 0) {                                                 /* PACKED */
            for (uint32_t l = 0; l < nloop && i < qwc; l++)
                for (uint32_t r = 0; r < nreg && i < qwc; r++, i++) {
                    uint64_t d0, d1;
                    memcpy(&d0, src + (size_t)i * 16, 8);
                    memcpy(&d1, src + (size_t)i * 16 + 8, 8);
                    gifp_packed(&q, (uint32_t)(hi >> (r * 4)) & 0xF, d0, d1, out, img);
                }
        } else {                                                               /* REGLIST: two registers per qword */
            uint32_t total = nloop * nreg, k = 0;
            while (k < total && i < qwc) {
                uint64_t d[2];
                memcpy(d, src + (size_t)i * 16, 16);
                for (int h = 0; h < 2 && k < total; h++, k++) {
                    uint32_t desc = (uint32_t)(hi >> ((k % nreg) * 4)) & 0xF;
                    if (desc < 0xE) out(desc, d[h]);
                }
                i++;
            }
        }
    }
}
