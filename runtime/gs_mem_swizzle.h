/* GS local memory addressing shared by gs_mem.c, gs_mem_xfer.c and gs_mem_tex.c.
 *
 * Layout (GS User's Manual, "Local memory"): 4 MB = 512 pages of 8 KB; a page is 32 blocks of
 * 256 bytes; a block is 4 columns of 64 bytes. Page sizes: 32-bit formats 64x32 pixels (blocks
 * 8x8), 16-bit 64x64 (blocks 16x8), PSMT8 128x64 (blocks 16x16), PSMT4 128x128 (blocks 32x16).
 * Pages are laid out left to right, (buffer width / page width) per row.
 *
 * Each table below gives, for every pixel of one page, its offset inside the page in that
 * format's storage unit (32-bit words, 16-bit halves, bytes or nibbles). An address is then
 *   (bp * units_per_block + page_index * units_per_page + table[pixel in page]) mod 4 MB,
 * which is the block-pointer addition the hardware does (bp need not be page aligned).
 * Coordinates passed to these helpers are already reduced to 0..2047. */
#pragma once
#include "gs_internal.h"

extern uint16_t gs_swz_ct32[32 * 64];    /* PSMCT32/24, T8H, T4HL/HH: words, [(y&31)<<6 | (x&63)] */
extern uint16_t gs_swz_z32[32 * 64];     /* PSMZ32/24 */
extern uint16_t gs_swz_ct16[64 * 64];    /* PSMCT16: halves, [(y&63)<<6 | (x&63)] */
extern uint16_t gs_swz_ct16s[64 * 64];   /* PSMCT16S */
extern uint16_t gs_swz_z16[64 * 64];     /* PSMZ16 */
extern uint16_t gs_swz_z16s[64 * 64];    /* PSMZ16S */
extern uint16_t gs_swz_t8[64 * 128];     /* PSMT8: bytes, [(y&63)<<7 | (x&127)] */
extern uint16_t gs_swz_t4[128 * 128];    /* PSMT4: nibbles, [(y&127)<<7 | (x&127)] */

/* Print a message the first time warning `id` (0..63) fires. */
void gs_mem_warn(int id, const char* fmt, ...);
enum { GSW_BAD_PSM = 0, GSW_XFER_IDLE_DATA, GSW_XFER_EXCESS, GSW_XFER_BPP, GSW_XFER_BAD_PSM,
       GSW_XFER_READ_IDLE, GSW_CLUT_CPSM, GSW_CLUT_CSM2_PSM, GSW_TEX_PSM };
void gs_mem_bad_psm(int psm);            /* cold path: unknown PSM value (treated as PSMCT32) */

/* 8-bit and 4-bit pages are 128 pixels wide, so a row holds bw/2 pages. bw = 1 (a 64-wide
 * buffer) still needs one page per row. */
static inline uint32_t gs_bw8(uint32_t bw) { return bw > 1 ? bw >> 1 : bw; }

static inline uint32_t gs_addr32(const uint16_t* tab, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    return (bp * 64u + (((y >> 5) * bw + (x >> 6)) << 11) + tab[(y & 31) << 6 | (x & 63)]) & 0xFFFFFu;
}
static inline uint32_t gs_addr16(const uint16_t* tab, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    return (bp * 128u + (((y >> 6) * bw + (x >> 6)) << 12) + tab[(y & 63) << 6 | (x & 63)]) & 0x1FFFFFu;
}
static inline uint32_t gs_addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    return (bp * 256u + (((y >> 6) * gs_bw8(bw) + (x >> 7)) << 13) + gs_swz_t8[(y & 63) << 7 | (x & 127)]) & 0x3FFFFFu;
}
static inline uint32_t gs_addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    return (bp * 512u + (((y >> 7) * gs_bw8(bw) + (x >> 7)) << 14) + gs_swz_t4[(y & 127) << 7 | (x & 127)]) & 0x7FFFFFu;
}

static inline uint32_t gs_vram_rd32(uint32_t w) { uint32_t v; memcpy(&v, gs_vram + ((size_t)w << 2), 4); return v; }
static inline void     gs_vram_wr32(uint32_t w, uint32_t v) { memcpy(gs_vram + ((size_t)w << 2), &v, 4); }
static inline uint32_t gs_vram_rd16(uint32_t h) { uint16_t v; memcpy(&v, gs_vram + ((size_t)h << 1), 2); return v; }
static inline void     gs_vram_wr16(uint32_t h, uint32_t v) { uint16_t s = (uint16_t)v; memcpy(gs_vram + ((size_t)h << 1), &s, 2); }

/* Bits per pixel of a PSM as stored/transferred (T8H = 8, T4HL/HH = 4); 0 if not a valid PSM. */
static inline int gs_psm_bpp(int psm) {
    switch (psm) {
    case PSMCT32: case PSMZ32: return 32;
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    default: return 0;
    }
}

/* Raw pixel read/write; x and y must already be in 0..2047. */
static inline uint32_t gs_px_read(int psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    switch (psm) {
    case PSMCT32: return gs_vram_rd32(gs_addr32(gs_swz_ct32, bp, bw, x, y));
    case PSMCT24: return gs_vram_rd32(gs_addr32(gs_swz_ct32, bp, bw, x, y)) & 0xFFFFFFu;
    case PSMCT16: return gs_vram_rd16(gs_addr16(gs_swz_ct16, bp, bw, x, y));
    case PSMCT16S: return gs_vram_rd16(gs_addr16(gs_swz_ct16s, bp, bw, x, y));
    case PSMT8: return gs_vram[gs_addr8(bp, bw, x, y)];
    case PSMT4: { uint32_t n = gs_addr4(bp, bw, x, y); return (gs_vram[n >> 1] >> ((n & 1) << 2)) & 15u; }
    case PSMT8H: return gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3];
    case PSMT4HL: return gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3] & 15u;
    case PSMT4HH: return (uint32_t)gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3] >> 4;
    case PSMZ32: return gs_vram_rd32(gs_addr32(gs_swz_z32, bp, bw, x, y));
    case PSMZ24: return gs_vram_rd32(gs_addr32(gs_swz_z32, bp, bw, x, y)) & 0xFFFFFFu;
    case PSMZ16: return gs_vram_rd16(gs_addr16(gs_swz_z16, bp, bw, x, y));
    case PSMZ16S: return gs_vram_rd16(gs_addr16(gs_swz_z16s, bp, bw, x, y));
    default: gs_mem_bad_psm(psm); return gs_vram_rd32(gs_addr32(gs_swz_ct32, bp, bw, x, y));
    }
}

static inline void gs_px_write24(uint32_t w, uint32_t v) {
    uint8_t* p = gs_vram + ((size_t)w << 2);   /* bytes 0-2; the top byte is preserved */
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
}

static inline void gs_px_write(int psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v) {
    switch (psm) {
    case PSMCT32: gs_vram_wr32(gs_addr32(gs_swz_ct32, bp, bw, x, y), v); return;
    case PSMCT24: gs_px_write24(gs_addr32(gs_swz_ct32, bp, bw, x, y), v); return;
    case PSMCT16: gs_vram_wr16(gs_addr16(gs_swz_ct16, bp, bw, x, y), v); return;
    case PSMCT16S: gs_vram_wr16(gs_addr16(gs_swz_ct16s, bp, bw, x, y), v); return;
    case PSMT8: gs_vram[gs_addr8(bp, bw, x, y)] = (uint8_t)v; return;
    case PSMT4: {
        uint32_t n = gs_addr4(bp, bw, x, y), sh = (n & 1) << 2;
        uint8_t* p = &gs_vram[n >> 1];
        *p = (uint8_t)((*p & ~(15u << sh)) | ((v & 15u) << sh));
        return;
    }
    case PSMT8H: gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3] = (uint8_t)v; return;
    case PSMT4HL: { uint8_t* p = &gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3]; *p = (uint8_t)((*p & 0xF0u) | (v & 15u)); return; }
    case PSMT4HH: { uint8_t* p = &gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3]; *p = (uint8_t)((*p & 0x0Fu) | ((v & 15u) << 4)); return; }
    case PSMZ32: gs_vram_wr32(gs_addr32(gs_swz_z32, bp, bw, x, y), v); return;
    case PSMZ24: gs_px_write24(gs_addr32(gs_swz_z32, bp, bw, x, y), v); return;
    case PSMZ16: gs_vram_wr16(gs_addr16(gs_swz_z16, bp, bw, x, y), v); return;
    case PSMZ16S: gs_vram_wr16(gs_addr16(gs_swz_z16s, bp, bw, x, y), v); return;
    default: gs_mem_bad_psm(psm); gs_vram_wr32(gs_addr32(gs_swz_ct32, bp, bw, x, y), v); return;
    }
}

/* CLUT buffer (gs_mem_tex.c), for tests and debugging: 512 16-bit halves. A PSMCT32 entry at
 * position p keeps its low half at [p] and its high half at [p ^ 256]. */
const uint16_t* gs_clut_buffer(void);
