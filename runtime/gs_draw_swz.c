/* Direct frame/Z buffer addressing with the GS manual's local-memory layout, a start-up check of
 * that layout against gs_mem, and per-page VRAM write tracking for the texture cache.
 *
 * Layout (GS User's Manual, "Local memory"): memory is split into 8 KB pages, a buffer of width
 * BW*64 pixels lays its pages out row-major. A page is 32 blocks of 256 bytes; the block order
 * inside a page depends on the format, the Z formats use the colour block order XOR 24. A block is
 * 4 columns of 64 bytes, each covering 2 rows (8x2 pixels at 32 bits, 16x2 at 16 bits). */
#include "gs_draw.h"

int gs_swz_fast;
uint32_t gs_page_gen[512];
uint32_t gs_vram_serial;

static uint16_t tab_ct32[32 * 64], tab_z32[32 * 64];
static uint16_t tab_ct16[64 * 64], tab_ct16s[64 * 64], tab_z16[64 * 64], tab_z16s[64 * 64];

static const gs_fmt fmt_ct32 = { tab_ct32, 5, 2, 32 }, fmt_ct24 = { tab_ct32, 5, 2, 24 };
static const gs_fmt fmt_z32 = { tab_z32, 5, 2, 32 }, fmt_z24 = { tab_z32, 5, 2, 24 };
static const gs_fmt fmt_ct16 = { tab_ct16, 6, 1, 16 }, fmt_ct16s = { tab_ct16s, 6, 1, 16 };
static const gs_fmt fmt_z16 = { tab_z16, 6, 1, 16 }, fmt_z16s = { tab_z16s, 6, 1, 16 };

/* block number at (block column, block row) inside a page */
static const uint8_t blk32[4][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
};
static const uint8_t blk16[8][4] = {
    {  0,  2,  8, 10 }, {  1,  3,  9, 11 }, {  4,  6, 12, 14 }, {  5,  7, 13, 15 },
    { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 },
};
static const uint8_t blk16s[8][4] = {
    {  0,  2, 16, 18 }, {  1,  3, 17, 19 }, {  8, 10, 24, 26 }, {  9, 11, 25, 27 },
    {  4,  6, 20, 22 }, {  5,  7, 21, 23 }, { 12, 14, 28, 30 }, { 13, 15, 29, 31 },
};

/* 32-bit column (8x2): word order 0 1 4 5 8 9 12 13 / 2 3 6 7 10 11 14 15 */
static unsigned col32(unsigned x, unsigned y) { return ((x >> 1) << 2) | ((y & 1) << 1) | (x & 1); }

static void build32(uint16_t* tab, unsigned zx) {
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 64; x++) {
            unsigned blk = blk32[y >> 3][x >> 3] ^ zx;
            tab[y * 64 + x] = (uint16_t)(blk * 64 + ((y & 7) >> 1) * 16 + col32(x & 7, y));
        }
}

/* 16-bit column (16x2): each 32-bit word of the 32-bit column order holds pixel x (low half) and
 * pixel x+8 (high half). */
static void build16(uint16_t* tab, const uint8_t b[8][4], unsigned zx) {
    for (unsigned y = 0; y < 64; y++)
        for (unsigned x = 0; x < 64; x++) {
            unsigned blk = b[y >> 3][x >> 4] ^ zx, bx = x & 15;
            unsigned k = col32(bx & 7, y) * 2 + (bx >> 3);
            tab[y * 64 + x] = (uint16_t)(blk * 128 + ((y & 7) >> 1) * 32 + k);
        }
}

const gs_fmt* gs_fmt_of(int psm) {
    switch (psm) {
    case PSMCT32: return &fmt_ct32;
    case PSMCT24: return &fmt_ct24;
    case PSMCT16: return &fmt_ct16;
    case PSMCT16S: return &fmt_ct16s;
    case PSMZ32: return &fmt_z32;
    case PSMZ24: return &fmt_z24;
    case PSMZ16: return &fmt_z16;
    case PSMZ16S: return &fmt_z16s;
    default: return NULL;
    }
}

/* Write unique values through gs_mem and look for them where our layout says they are. */
static int check_layout(int psm) {
    const gs_fmt* f = gs_fmt_of(psm);
    const int W = 192, H = 2 << f->ph_shift;          /* base page 1, BW 3: 6 pages */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t n = (uint32_t)(y * W + x) + 1;
            gs_mem_write(psm, 32, 3, x, y, f->bits == 16 ? n : 0xA5000000u | n);
        }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t n = (uint32_t)(y * W + x) + 1, a = gs_swz_addr(f, 1, 3, x, y), got;
            if (f->bits == 16) { uint16_t h; memcpy(&h, gs_vram + a, 2); got = h; }
            else { memcpy(&got, gs_vram + a, 4); n |= 0xA5000000u; }
            if (got != n) return 0;
        }
    return 1;
}

void gs_swz_init(void) {
    static int inited;
    if (inited) return;
    inited = 1;
    build32(tab_ct32, 0);
    build32(tab_z32, 24);
    build16(tab_ct16, blk16, 0);
    build16(tab_ct16s, blk16s, 0);
    build16(tab_z16, blk16, 24);
    build16(tab_z16s, blk16s, 24);

    static uint8_t saved[16 * 8192];
    memcpy(saved, gs_vram, sizeof saved);
    static const int fmts[] = { PSMCT32, PSMZ32, PSMCT16, PSMCT16S, PSMZ16, PSMZ16S };
    gs_swz_fast = 1;
    for (unsigned i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
        if (!check_layout(fmts[i])) {
            gs_mem_write(PSMCT32, 0, 1, 5, 3, 0x12345678u);
            if (gs_mem_read(PSMCT32, 0, 1, 5, 3) != 0x12345678u)
                fprintf(stderr, "[gs] gs_mem does not store pixels (stub?): nothing will be visible\n");
            else
                fprintf(stderr, "[gs] gs_mem layout of PSM 0x%02X differs from the documented one: "
                                "frame/Z buffers use the slow gs_mem path\n", fmts[i]);
            gs_swz_fast = 0;
            break;
        }
    memcpy(gs_vram, saved, sizeof saved);
}

/* ---- page write tracking ------------------------------------------------------------------------ */

void gs_page_dims(int psm, int* pw, int* ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *pw = 64; *ph = 64; break;
    case PSMT8: *pw = 128; *ph = 64; break;
    case PSMT4: *pw = 128; *ph = 128; break;
    default: *pw = 64; *ph = 32; break;                 /* 32/24-bit, T8H, T4HL, T4HH */
    }
}

void gs_mark_rect(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 <= x0 || y1 <= y0) return;
    int pw, ph;
    gs_page_dims(psm, &pw, &ph);
    uint32_t ppr = bw * 64 / (uint32_t)pw;
    if (!ppr) ppr = 1;
    uint32_t serial = gs_vram_serial + 1;
    int hit = 0;
    int r0 = y0 / ph, r1 = (y1 - 1) / ph, c0 = x0 / pw, c1 = (x1 - 1) / pw;
    if ((uint32_t)(r1 - r0 + 1) * (uint32_t)(c1 - c0 + 1) >= 512) {
        for (int p = 0; p < 512; p++)
            if (gs_page_texrefs[p]) { gs_page_gen[p] = serial; hit = 1; }
    } else {
        uint32_t base = bp >> 5, extra = (bp & 31) != 0;     /* block offset: content spills into p+1 */
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                uint32_t p = (base + (uint32_t)r * ppr + (uint32_t)c) & 511;
                if (gs_page_texrefs[p]) { gs_page_gen[p] = serial; hit = 1; }
                if (extra && gs_page_texrefs[(p + 1) & 511]) { gs_page_gen[(p + 1) & 511] = serial; hit = 1; }
            }
    }
    if (hit) gs_vram_serial = serial;
}
