/* GS local memory: the 4 MB VRAM, its page/block/column swizzle tables, raw pixel access.
 * Transfers live in gs_mem_xfer.c, CLUT and texel fetch in gs_mem_tex.c.
 *
 * Layouts follow the GS User's Manual (local memory chapter):
 *  - block arrangement inside a page (block numbers by block position):
 *      PSMCT32 / PSMT8 (8 x 4 blocks)     PSMCT16 / PSMT4 (4 x 8)    PSMCT16S (4 x 8)
 *       0  1  4  5 16 17 20 21              0  2  8 10                0  2 16 18
 *       2  3  6  7 18 19 22 23              1  3  9 11                1  3 17 19
 *       8  9 12 13 24 25 28 29              4  6 12 14                8 10 24 26
 *      10 11 14 15 26 27 30 31              5  7 13 15                9 11 25 27
 *                                          16 18 24 26                4  6 20 22
 *                                          17 19 25 27                5  7 21 23
 *                                          20 22 28 30               12 14 28 30
 *                                          21 23 29 31               13 15 29 31
 *    The Z formats use the same shapes with the block number XOR 24 (PSMZ32: 24 25 28 29 8 9 ...).
 *  - columns: a block is 4 columns of 64 bytes stacked vertically (8x2 pixels for 32-bit, 16x2
 *    for 16-bit, 16x4 for 8-bit, 32x4 for 4-bit). In a 32-bit column the 16 words are ordered
 *        0  1  4  5  8  9 12 13
 *        2  3  6  7 10 11 14 15
 *    A 16-bit column stores pixel x (0..15) in the word of 32-bit pixel x&7, low half for x<8.
 *    8-bit and 4-bit columns use the same 16 words: rows 0/1 of the column take the words of
 *    32-bit rows 0/1 and rows 2/3 take them again, with the 8 word positions rotated by 4 on the
 *    rows 2/3 of even columns and on rows 0/1 of odd columns; the byte (nibble) inside the word
 *    is 2*(x/8) + (row/2). */
#include "gs_mem_swizzle.h"

#include <stdarg.h>
#include <stdio.h>

_Alignas(64) uint8_t gs_vram[GS_VRAM_SIZE];

uint16_t gs_swz_ct32[32 * 64], gs_swz_z32[32 * 64];
uint16_t gs_swz_ct16[64 * 64], gs_swz_ct16s[64 * 64], gs_swz_z16[64 * 64], gs_swz_z16s[64 * 64];
uint16_t gs_swz_t8[64 * 128], gs_swz_t4[128 * 128];

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
static const uint8_t col32[2][8] = {
    { 0, 1, 4, 5,  8,  9, 12, 13 },
    { 2, 3, 6, 7, 10, 11, 14, 15 },
};

/* Built before main() so the hot paths need no "initialised?" check. */
__attribute__((constructor)) static void gs_mem_build_tables(void) {
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 64; x++) {
            int blk = blk32[y >> 3][x >> 3], cy = y & 7;
            int in = (cy >> 1) * 16 + col32[cy & 1][x & 7];
            gs_swz_ct32[y << 6 | x] = (uint16_t)(blk * 64 + in);
            gs_swz_z32[y << 6 | x] = (uint16_t)((blk ^ 24) * 64 + in);
        }
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int by = y >> 3, bx = x >> 4, cy = y & 7, cx = x & 15;
            int in = (cy >> 1) * 32 + col32[cy & 1][cx & 7] * 2 + (cx >> 3);
            gs_swz_ct16[y << 6 | x] = (uint16_t)(blk16[by][bx] * 128 + in);
            gs_swz_ct16s[y << 6 | x] = (uint16_t)(blk16s[by][bx] * 128 + in);
            gs_swz_z16[y << 6 | x] = (uint16_t)((blk16[by][bx] ^ 24) * 128 + in);
            gs_swz_z16s[y << 6 | x] = (uint16_t)((blk16s[by][bx] ^ 24) * 128 + in);
        }
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++) {
            int cx = x & 15, cy = y & 15, col = cy >> 2, r = cy & 3;
            int rot = ((r >> 1) ^ col) & 1;
            int word = col32[r & 1][(cx + rot * 4) & 7];
            int byte = (cx >> 3) * 2 + (r >> 1);
            gs_swz_t8[y << 7 | x] = (uint16_t)(blk32[y >> 4][x >> 4] * 256 + col * 64 + word * 4 + byte);
        }
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            int cx = x & 31, cy = y & 15, col = cy >> 2, r = cy & 3;
            int rot = ((r >> 1) ^ col) & 1;
            int word = col32[r & 1][(cx + rot * 4) & 7];
            int nib = (cx >> 3) * 2 + (r >> 1);
            gs_swz_t4[y << 7 | x] = (uint16_t)(blk16[y >> 4][x >> 5] * 512 + col * 128 + word * 8 + nib);
        }
}

void gs_mem_warn(int id, const char* fmt, ...) {
    static uint64_t seen;
    if (id < 0 || id > 63 || (seen >> id & 1)) return;
    seen |= 1ull << id;
    va_list ap;
    va_start(ap, fmt);
    fputs("[gs-mem] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void gs_mem_bad_psm(int psm) {
    gs_mem_warn(GSW_BAD_PSM, "pixel access with invalid PSM 0x%02X (treated as PSMCT32)", psm);
}

uint32_t gs_mem_read(int psm, uint32_t bp, uint32_t bw, int x, int y) {
    return gs_px_read(psm, bp, bw, (uint32_t)x & 2047u, (uint32_t)y & 2047u);
}

void gs_mem_write(int psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t value) {
    gs_px_write(psm, bp, bw, (uint32_t)x & 2047u, (uint32_t)y & 2047u, value);
}
