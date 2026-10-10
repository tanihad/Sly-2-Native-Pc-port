/* Unit tests for the GS local memory subsystem (runtime/gs_mem*.c).
 * Layout expectations are typed from the GS User's Manual tables, independently of the
 * implementation, and probed through the public API (gs_mem_write then scan the 4 MB). */
#include "gs_mem_swizzle.h"

#include <stdio.h>
#include <stdlib.h>

static int checks, fails;
#define CHECK(cond, ...)                                                                  \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(cond)) {                                                                    \
            if (fails++ < 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
        }                                                                                 \
    } while (0)

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint64_t bitbltbuf(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t dbp, uint32_t dbw, uint32_t dpsm) {
    return (uint64_t)sbp | (uint64_t)sbw << 16 | (uint64_t)spsm << 24 | (uint64_t)dbp << 32 | (uint64_t)dbw << 48 | (uint64_t)dpsm << 56;
}
static uint64_t trxpos(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t dir) {
    return (uint64_t)sx | (uint64_t)sy << 16 | (uint64_t)dx << 32 | (uint64_t)dy << 48 | (uint64_t)dir << 59;
}
static uint64_t trxreg(uint32_t w, uint32_t h) { return (uint64_t)w | (uint64_t)h << 32; }

static void fill_random(void) { for (uint32_t i = 0; i < GS_VRAM_SIZE; i += 4) { uint32_t r = rnd(); memcpy(gs_vram + i, &r, 4); } }

/* Clear VRAM, write all-ones to one pixel, return the lowest bit address that changed and the
 * number of bits set. */
static int64_t probe(int psm, uint32_t bp, uint32_t bw, int x, int y, int* nbits) {
    memset(gs_vram, 0, GS_VRAM_SIZE);
    gs_mem_write(psm, bp, bw, x, y, 0xFFFFFFFFu);
    int64_t first = -1;
    int n = 0;
    for (uint32_t i = 0; i < GS_VRAM_SIZE; i += 8) {
        uint64_t q;
        memcpy(&q, gs_vram + i, 8);
        if (!q) continue;
        if (first < 0) first = (int64_t)i * 8 + __builtin_ctzll(q);
        n += __builtin_popcountll(q);
    }
    *nbits = n;
    return first;
}

static const char* psm_name(int psm) {
    switch (psm) {
    case PSMCT32: return "CT32"; case PSMCT24: return "CT24"; case PSMCT16: return "CT16"; case PSMCT16S: return "CT16S";
    case PSMT8: return "T8"; case PSMT4: return "T4"; case PSMT8H: return "T8H"; case PSMT4HL: return "T4HL";
    case PSMT4HH: return "T4HH"; case PSMZ32: return "Z32"; case PSMZ24: return "Z24"; case PSMZ16: return "Z16";
    case PSMZ16S: return "Z16S"; default: return "?";
    }
}
static const int all_psm[] = { PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
                               PSMZ32, PSMZ24, PSMZ16, PSMZ16S };
#define NPSM (int)(sizeof all_psm / sizeof all_psm[0])
static int bits_of(int psm) { return gs_psm_bpp(psm); }
static uint32_t mask_of(int psm) { int b = bits_of(psm); return b == 32 ? 0xFFFFFFFFu : (1u << b) - 1; }

/* ---- documented tables (GS User's Manual) ----------------------------------------------------- */

static const int man_blk32[4][8] = {
    { 0, 1, 4, 5, 16, 17, 20, 21 }, { 2, 3, 6, 7, 18, 19, 22, 23 },
    { 8, 9, 12, 13, 24, 25, 28, 29 }, { 10, 11, 14, 15, 26, 27, 30, 31 } };
static const int man_blkz32[4][8] = {
    { 24, 25, 28, 29, 8, 9, 12, 13 }, { 26, 27, 30, 31, 10, 11, 14, 15 },
    { 16, 17, 20, 21, 0, 1, 4, 5 }, { 18, 19, 22, 23, 2, 3, 6, 7 } };
static const int man_blk16[8][4] = {
    { 0, 2, 8, 10 }, { 1, 3, 9, 11 }, { 4, 6, 12, 14 }, { 5, 7, 13, 15 },
    { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 } };
static const int man_blk16s[8][4] = {
    { 0, 2, 16, 18 }, { 1, 3, 17, 19 }, { 8, 10, 24, 26 }, { 9, 11, 25, 27 },
    { 4, 6, 20, 22 }, { 5, 7, 21, 23 }, { 12, 14, 28, 30 }, { 13, 15, 29, 31 } };
static const int man_blkz16[8][4] = {
    { 24, 26, 16, 18 }, { 25, 27, 17, 19 }, { 28, 30, 20, 22 }, { 29, 31, 21, 23 },
    { 8, 10, 0, 2 }, { 9, 11, 1, 3 }, { 12, 14, 4, 6 }, { 13, 15, 5, 7 } };
static const int man_blkz16s[8][4] = {
    { 24, 26, 8, 10 }, { 25, 27, 9, 11 }, { 16, 18, 0, 2 }, { 17, 19, 1, 3 },
    { 28, 30, 12, 14 }, { 29, 31, 13, 15 }, { 20, 22, 4, 6 }, { 21, 23, 5, 7 } };

/* word index inside a 32-bit column (8x2) */
static const int man_col32[2][8] = { { 0, 1, 4, 5, 8, 9, 12, 13 }, { 2, 3, 6, 7, 10, 11, 14, 15 } };
/* halfword index inside a 16-bit column (16x2) */
static const int man_col16[2][16] = {
    { 0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27 },
    { 4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31 } };
/* byte offset inside a PSMT8 block (16x16): rows 0-7 = columns 0 and 1; columns 2/3 = +128 */
static const int man_t8[8][16] = {
    { 0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54 },
    { 8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62 },
    { 33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23 },
    { 41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31 },
    { 96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86 },
    { 104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94 },
    { 65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119 },
    { 73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127 } };
/* nibble offset inside a PSMT4 block (32x16): rows 0-7 = columns 0 and 1; columns 2/3 = +256 */
static const int man_t4[8][32] = {
    { 0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110 },
    { 16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126 },
    { 65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47 },
    { 81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63 },
    { 192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174 },
    { 208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190 },
    { 129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239 },
    { 145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255 } };

/* ---- layout tests ------------------------------------------------------------------------------- */

static void test_block_arrangement(void) {
    int nb;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 8; bx++) {
            CHECK(probe(PSMCT32, 0, 1, bx * 8, by * 8, &nb) == man_blk32[by][bx] * 2048 && nb == 32, "CT32 block (%d,%d)", bx, by);
            CHECK(probe(PSMZ32, 0, 1, bx * 8, by * 8, &nb) == man_blkz32[by][bx] * 2048 && nb == 32, "Z32 block (%d,%d)", bx, by);
            CHECK(probe(PSMT8, 0, 2, bx * 16, by * 16, &nb) == man_blk32[by][bx] * 2048 && nb == 8, "T8 block (%d,%d)", bx, by);
        }
    for (int by = 0; by < 8; by++)
        for (int bx = 0; bx < 4; bx++) {
            CHECK(probe(PSMCT16, 0, 1, bx * 16, by * 8, &nb) == man_blk16[by][bx] * 2048 && nb == 16, "CT16 block (%d,%d)", bx, by);
            CHECK(probe(PSMCT16S, 0, 1, bx * 16, by * 8, &nb) == man_blk16s[by][bx] * 2048 && nb == 16, "CT16S block (%d,%d)", bx, by);
            CHECK(probe(PSMZ16, 0, 1, bx * 16, by * 8, &nb) == man_blkz16[by][bx] * 2048 && nb == 16, "Z16 block (%d,%d)", bx, by);
            CHECK(probe(PSMZ16S, 0, 1, bx * 16, by * 8, &nb) == man_blkz16s[by][bx] * 2048 && nb == 16, "Z16S block (%d,%d)", bx, by);
            CHECK(probe(PSMT4, 0, 2, bx * 32, by * 16, &nb) == man_blk16[by][bx] * 2048 && nb == 4, "T4 block (%d,%d)", bx, by);
        }
}

static void test_columns(void) {
    int nb;
    /* 32-bit: 4 columns of 8x2 in an 8x8 block; also inside block (1,1) = 3 with bp = 2 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int64_t want = ((2 + man_blk32[1][1]) * 256 + (y >> 1) * 64 + man_col32[y & 1][x] * 4) * 8;
            CHECK(probe(PSMCT32, 2, 1, 8 + x, 8 + y, &nb) == want, "CT32 column in block 3, bp=2 (%d,%d)", x, y);
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int64_t want = ((y >> 1) * 64 + man_col32[y & 1][x] * 4) * 8;
            CHECK(probe(PSMCT32, 0, 1, x, y, &nb) == want && nb == 32, "CT32 column (%d,%d)", x, y);
            CHECK(probe(PSMCT24, 0, 1, x, y, &nb) == want && nb == 24, "CT24 column (%d,%d)", x, y);
            CHECK(probe(PSMT8H, 0, 1, x, y, &nb) == want + 24 && nb == 8, "T8H (%d,%d)", x, y);
            CHECK(probe(PSMT4HL, 0, 1, x, y, &nb) == want + 24 && nb == 4, "T4HL (%d,%d)", x, y);
            CHECK(probe(PSMT4HH, 0, 1, x, y, &nb) == want + 28 && nb == 4, "T4HH (%d,%d)", x, y);
            CHECK(probe(PSMZ32, 0, 1, x, y, &nb) == want + 24 * 2048 && nb == 32, "Z32 column (%d,%d)", x, y);
            CHECK(probe(PSMZ24, 0, 1, x, y, &nb) == want + 24 * 2048 && nb == 24, "Z24 column (%d,%d)", x, y);
        }
    /* 16-bit: 4 columns of 16x2 in a 16x8 block */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) {
            int64_t want = ((y >> 1) * 64 + man_col16[y & 1][x] * 2) * 8;
            CHECK(probe(PSMCT16, 0, 1, x, y, &nb) == want && nb == 16, "CT16 column (%d,%d)", x, y);
            CHECK(probe(PSMCT16S, 0, 1, x, y, &nb) == want && nb == 16, "CT16S column (%d,%d)", x, y);
            CHECK(probe(PSMZ16, 0, 1, x, y, &nb) == want + 24 * 2048, "Z16 column (%d,%d)", x, y);
            CHECK(probe(PSMZ16S, 0, 1, x, y, &nb) == want + 24 * 2048, "Z16S column (%d,%d)", x, y);
        }
    /* 8-bit: 16x16 block */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int64_t want = (man_t8[y & 7][x] + (y >> 3) * 128) * 8;
            CHECK(probe(PSMT8, 0, 2, x, y, &nb) == want && nb == 8, "T8 column (%d,%d) got %lld want %lld", x, y,
                  (long long)probe(PSMT8, 0, 2, x, y, &nb), (long long)want);
        }
    /* 4-bit: 32x16 block */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 32; x++) {
            int64_t want = (man_t4[y & 7][x] + (y >> 3) * 256) * 4;
            CHECK(probe(PSMT4, 0, 2, x, y, &nb) == want && nb == 4, "T4 column (%d,%d)", x, y);
        }
}

static void test_pages_and_wrap(void) {
    int nb;
    CHECK(probe(PSMCT32, 0, 2, 64, 0, &nb) == 8192 * 8, "CT32 page 1 at x=64 bw=2");
    CHECK(probe(PSMCT32, 0, 2, 0, 32, &nb) == 2 * 8192 * 8, "CT32 page row 1 = page 2 with bw=2");
    CHECK(probe(PSMCT32, 0, 10, 130, 70, &nb) == (int64_t)(2 * 10 + 2) * 8192 * 8 + probe(PSMCT32, 0, 1, 2, 6, &nb), "CT32 page (2,2) bw=10");
    CHECK(probe(PSMCT16, 0, 1, 0, 64, &nb) == 8192 * 8, "CT16 page row 1 bw=1");
    CHECK(probe(PSMCT16, 0, 3, 64, 64, &nb) == 4 * 8192 * 8, "CT16 page (1,1) bw=3");
    CHECK(probe(PSMT8, 0, 2, 0, 64, &nb) == 8192 * 8, "T8 bw=2: one page per row");
    CHECK(probe(PSMT8, 0, 4, 128, 0, &nb) == 8192 * 8, "T8 bw=4: page 1 at x=128");
    CHECK(probe(PSMT8, 0, 4, 0, 64, &nb) == 2 * 8192 * 8, "T8 bw=4: page row 1 = page 2");
    CHECK(probe(PSMT8, 0, 1, 0, 64, &nb) == 8192 * 8, "T8 bw=1: still one page per row");
    CHECK(probe(PSMT4, 0, 2, 0, 128, &nb) == 8192 * 8, "T4 bw=2: page row 1");
    CHECK(probe(PSMT4, 0, 6, 256, 128, &nb) == 5 * 8192 * 8, "T4 bw=6: page (2,1)");
    CHECK(probe(PSMCT32, 1, 1, 0, 0, &nb) == 256 * 8, "bp=1 -> byte 256");
    CHECK(probe(PSMCT32, 3, 1, 8, 0, &nb) == 4 * 256 * 8, "bp=3 + block 1 -> block 4 (not page aligned)");
    CHECK(probe(PSMCT32, 16383, 1, 8, 0, &nb) == 0, "bp=16383 + block 1 wraps to block 0");
    CHECK(probe(PSMCT32, 0, 1, -1, 0, &nb) == probe(PSMCT32, 0, 1, 2047, 0, &nb), "x=-1 wraps to 2047");
    /* the last page of memory, then the wrap */
    CHECK(probe(PSMCT32, 511 * 32, 1, 0, 0, &nb) == (int64_t)511 * 8192 * 8, "page 511");
    CHECK(probe(PSMCT32, 511 * 32, 1, 0, 32, &nb) == 0, "page 512 wraps to page 0");
}

/* Relations between formats sharing memory (documented layouts imply these). */
static void test_cross_format(void) {
    memset(gs_vram, 0, GS_VRAM_SIZE);
    gs_mem_write(PSMT8, 0, 2, 0, 0, 0x11);
    gs_mem_write(PSMT8, 0, 2, 8, 0, 0x22);
    gs_mem_write(PSMT8, 0, 2, 0, 2, 0x33);
    gs_mem_write(PSMT8, 0, 2, 0, 4, 0x44);
    CHECK(gs_mem_read(PSMCT32, 0, 1, 0, 0) == 0x00220011u, "T8 (0,0),(8,0) in CT32 (0,0): %08X", gs_mem_read(PSMCT32, 0, 1, 0, 0));
    CHECK(gs_mem_read(PSMCT32, 0, 1, 4, 0) == 0x00003300u, "T8 (0,2) in CT32 (4,0) byte 1");
    CHECK(gs_mem_read(PSMCT32, 0, 1, 4, 2) == 0x00000044u, "T8 (0,4) in CT32 (4,2) byte 0");

    memset(gs_vram, 0, GS_VRAM_SIZE);
    gs_mem_write(PSMCT16, 0, 1, 0, 0, 0x1111);
    gs_mem_write(PSMCT16, 0, 1, 8, 0, 0x2222);
    gs_mem_write(PSMCT16, 0, 1, 0, 8, 0x3333);
    gs_mem_write(PSMCT16, 0, 1, 16, 0, 0x4444);
    gs_mem_write(PSMCT16, 0, 1, 0, 32, 0x5555);
    CHECK(gs_mem_read(PSMCT32, 0, 1, 0, 0) == 0x22221111u, "CT16 (0,0),(8,0) share CT32 (0,0)");
    CHECK(gs_mem_read(PSMCT32, 0, 1, 8, 0) == 0x00003333u, "CT16 block 1 = CT32 block 1");
    CHECK(gs_mem_read(PSMCT32, 0, 1, 0, 8) == 0x00004444u, "CT16 block 8 at CT32 (0,8)");
    CHECK(gs_mem_read(PSMCT32, 0, 1, 32, 0) == 0x00005555u, "CT16 block 16 at CT32 (32,0)");

    memset(gs_vram, 0, GS_VRAM_SIZE);
    gs_mem_write(PSMT4, 0, 2, 8, 0, 0xA);
    gs_mem_write(PSMT4, 0, 2, 32, 0, 0xB);
    CHECK(gs_mem_read(PSMCT32, 0, 1, 0, 0) == 0x00000A00u, "T4 (8,0) is nibble 2 of CT32 (0,0)");
    CHECK(gs_mem_read(PSMCT32, 0, 1, 0, 8) == 0x0000000Bu, "T4 block 8 at CT32 (0,8)");
    CHECK(gs_mem_read(PSMT8, 0, 2, 0, 0) == 0 && gs_mem_read(PSMT4, 0, 2, 8, 0) == 0xA, "T4 readback");

    /* sub-word formats keep the other bits */
    fill_random();
    uint32_t before = gs_mem_read(PSMCT32, 64, 3, 70, 40);
    gs_mem_write(PSMCT24, 64, 3, 70, 40, 0xFFABCDEFu);
    CHECK(gs_mem_read(PSMCT32, 64, 3, 70, 40) == ((before & 0xFF000000u) | 0xABCDEFu), "CT24 keeps the top byte");
    gs_mem_write(PSMT8H, 64, 3, 70, 40, 0x5A);
    CHECK(gs_mem_read(PSMCT32, 64, 3, 70, 40) == (0x5A000000u | 0xABCDEFu), "T8H writes the top byte");
    gs_mem_write(PSMT4HL, 64, 3, 70, 40, 0x3);
    CHECK(gs_mem_read(PSMCT32, 64, 3, 70, 40) == (0x53000000u | 0xABCDEFu), "T4HL writes bits 24-27");
    gs_mem_write(PSMT4HH, 64, 3, 70, 40, 0xC);
    CHECK(gs_mem_read(PSMCT32, 64, 3, 70, 40) == (0xC3000000u | 0xABCDEFu), "T4HH writes bits 28-31");
    CHECK(gs_mem_read(PSMT8H, 64, 3, 70, 40) == 0xC3 && gs_mem_read(PSMT4HL, 64, 3, 70, 40) == 3 &&
          gs_mem_read(PSMT4HH, 64, 3, 70, 40) == 0xC && gs_mem_read(PSMCT24, 64, 3, 70, 40) == 0xABCDEF, "8H/4HL/4HH/24 reads");
    uint32_t ztop = gs_mem_read(PSMZ32, 64, 3, 70, 40) & 0xFF000000u;
    gs_mem_write(PSMZ24, 64, 3, 70, 40, 0xFF123456u);
    CHECK(gs_mem_read(PSMZ32, 64, 3, 70, 40) == (ztop | 0x123456u) && gs_mem_read(PSMZ24, 64, 3, 70, 40) == 0x123456,
          "Z24 keeps the top byte");
}

/* Every page table must be a permutation of its page. */
static void test_permutations(void) {
    static uint8_t seen[16384];
    struct { const uint16_t* t; int n; const char* name; } tabs[] = {
        { gs_swz_ct32, 2048, "ct32" }, { gs_swz_z32, 2048, "z32" }, { gs_swz_ct16, 4096, "ct16" },
        { gs_swz_ct16s, 4096, "ct16s" }, { gs_swz_z16, 4096, "z16" }, { gs_swz_z16s, 4096, "z16s" },
        { gs_swz_t8, 8192, "t8" }, { gs_swz_t4, 16384, "t4" } };
    for (size_t k = 0; k < sizeof tabs / sizeof tabs[0]; k++) {
        memset(seen, 0, sizeof seen);
        int ok = 1;
        for (int i = 0; i < tabs[k].n; i++) {
            if (tabs[k].t[i] >= tabs[k].n || seen[tabs[k].t[i]]) ok = 0;
            else seen[tabs[k].t[i]] = 1;
        }
        CHECK(ok, "%s table is a permutation", tabs[k].name);
    }
}

/* Fill a whole multi-page rectangle with distinct values in each format and read it back. */
static void test_roundtrip(void) {
    for (int k = 0; k < NPSM; k++) {
        int psm = all_psm[k];
        uint32_t bw = 6, bp = 96 + 32 * 3, w = 384, h = 256, m = mask_of(psm);
        fill_random();
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                gs_mem_write(psm, bp, bw, (int)x, (int)y, (x * 2654435761u ^ y * 40503u) + y);
        int bad = 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                if (gs_mem_read(psm, bp, bw, (int)x, (int)y) != (((x * 2654435761u ^ y * 40503u) + y) & m)) bad++;
        CHECK(bad == 0, "%s round trip: %d bad pixels", psm_name(psm), bad);
    }
}

/* ---- transfers -------------------------------------------------------------------------------- */

static uint32_t stream_px(const uint8_t* s, int bpp, uint32_t i) {
    switch (bpp) {
    case 32: return (uint32_t)s[4 * i] | (uint32_t)s[4 * i + 1] << 8 | (uint32_t)s[4 * i + 2] << 16 | (uint32_t)s[4 * i + 3] << 24;
    case 24: return (uint32_t)s[3 * i] | (uint32_t)s[3 * i + 1] << 8 | (uint32_t)s[3 * i + 2] << 16;
    case 16: return (uint32_t)s[2 * i] | (uint32_t)s[2 * i + 1] << 8;
    case 8: return s[i];
    default: return (s[i >> 1] >> ((i & 1) * 4)) & 15u;
    }
}

static uint8_t snap[GS_VRAM_SIZE], result[GS_VRAM_SIZE];

static void test_host_local(void) {
    static uint8_t stream[600 * 1024], back[600 * 1024];
    for (int k = 0; k < NPSM; k++) {
        int psm = all_psm[k], bpp = bits_of(psm);
        for (int pass = 0; pass < 3; pass++) {
            /* Rectangles stay inside the buffer width: wider ones alias themselves through the
             * next page row (as on hardware), and the readback below could not match. */
            uint32_t bp = 32 * (rnd() % 64), bw = 2 + rnd() % 8;
            uint32_t x0 = rnd() % (bw * 32), y0 = rnd() % 300, w = 1 + rnd() % (bw * 64 - x0), h = 1 + rnd() % 200;
            if (pass == 0) { x0 = 0; y0 = 0; w = bw * 64; h = 64; }       /* page-aligned full width */
            if (pass == 2) { x0 = 2040; w = 24; }                         /* wraps at X = 2048 */
            if (bpp == 4 && (w * h) & 1) w = w > 1 ? w - 1 : 2;           /* whole bytes of 4-bit data */
            uint32_t npx = w * h, nbytes = (npx * (uint32_t)bpp + 7) / 8;
            for (uint32_t i = 0; i < nbytes; i++) stream[i] = (uint8_t)rnd();
            fill_random();
            memcpy(snap, gs_vram, GS_VRAM_SIZE);

            gs_mem_xfer_start(bitbltbuf(0, 0, 0, bp, bw, (uint32_t)psm), trxpos(0, 0, x0, y0, 0), trxreg(w, h), 0);
            CHECK(gs_mem_xfer_active(), "%s transfer active", psm_name(psm));
            for (uint32_t off = 0; off < nbytes;) {
                uint32_t n = 1 + rnd() % 37;
                if (n > nbytes - off) n = nbytes - off;
                gs_mem_xfer_write(stream + off, n);
                off += n;
            }
            CHECK(!gs_mem_xfer_active(), "%s transfer finished after %u bytes", psm_name(psm), nbytes);
            memcpy(result, gs_vram, GS_VRAM_SIZE);

            /* reference: the same pixels through gs_mem_write on the original memory */
            memcpy(gs_vram, snap, GS_VRAM_SIZE);
            for (uint32_t i = 0; i < npx; i++)
                gs_mem_write(psm, bp, bw, (int)((x0 + i % w) & 2047), (int)((y0 + i / w) & 2047), stream_px(stream, bpp, i));
            CHECK(memcmp(result, gs_vram, GS_VRAM_SIZE) == 0, "%s host->local %ux%u at (%u,%u) bw=%u matches per-pixel writes",
                  psm_name(psm), w, h, x0, y0, bw);

            /* local->host returns the same stream */
            memcpy(gs_vram, result, GS_VRAM_SIZE);
            gs_mem_xfer_start(bitbltbuf(bp, bw, (uint32_t)psm, 0, 0, 0), trxpos(x0, y0, 0, 0, 0), trxreg(w, h), 1);
            uint32_t got = 0;
            while (got < nbytes) {
                uint32_t n = 1 + rnd() % 61;
                if (n > nbytes - got) n = nbytes - got;
                uint32_t r = gs_mem_xfer_read(back + got, n);
                CHECK(r == n, "%s local->host chunk %u returned %u", psm_name(psm), n, r);
                if (r != n) break;
                got += n;
            }
            CHECK(!gs_mem_xfer_active(), "%s local->host done", psm_name(psm));
            CHECK(memcmp(back, stream, nbytes) == 0, "%s local->host stream matches", psm_name(psm));
            uint8_t extra[16];
            CHECK(gs_mem_xfer_read(extra, 16) == 0, "%s local->host past the end returns 0", psm_name(psm));
        }
    }

    /* X and Y wrap at 2048 */
    memset(gs_vram, 0, GS_VRAM_SIZE);
    uint32_t px[32];
    for (int i = 0; i < 32; i++) px[i] = 0x01000000u + (uint32_t)i;
    gs_mem_xfer_start(bitbltbuf(0, 0, 0, 0, 4, PSMCT32), trxpos(0, 0, 2044, 2047, 0), trxreg(8, 4), 0);
    gs_mem_xfer_write(px, sizeof px);
    CHECK(gs_mem_read(PSMCT32, 0, 4, 2044, 2047) == 0x01000000u && gs_mem_read(PSMCT32, 0, 4, 2047, 2047) == 0x01000003u, "row 0 before wrap");
    CHECK(gs_mem_read(PSMCT32, 0, 4, 0, 2047) == 0x01000004u && gs_mem_read(PSMCT32, 0, 4, 3, 2047) == 0x01000007u, "row 0 wraps to X=0");
    CHECK(gs_mem_read(PSMCT32, 0, 4, 2044, 0) == 0x01000008u && gs_mem_read(PSMCT32, 0, 4, 3, 2) == 0x0100001Fu, "rows wrap to Y=0");

    /* a rectangle wider than the buffer is not clipped: pixel 64 of a bw=1 row is in page 1,
     * which is the next page row of that buffer */
    memset(gs_vram, 0, GS_VRAM_SIZE);
    uint32_t wide[96];
    for (int i = 0; i < 96; i++) wide[i] = 0x02000000u + (uint32_t)i;
    gs_mem_xfer_start(bitbltbuf(0, 0, 0, 0, 1, PSMCT32), trxpos(0, 0, 0, 0, 0), trxreg(96, 1), 0);
    gs_mem_xfer_write(wide, sizeof wide);
    CHECK(gs_mem_read(PSMCT32, 0, 1, 63, 0) == 0x0200003Fu && gs_mem_read(PSMCT32, 0, 1, 0, 32) == 0x02000040u &&
          gs_mem_read(PSMCT32, 0, 1, 31, 32) == 0x0200005Fu, "wide rectangle continues into the next page row");

    /* 24-bit packing in odd chunks */
    memset(gs_vram, 0xEE, GS_VRAM_SIZE);
    const uint8_t p24[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    gs_mem_xfer_start(bitbltbuf(0, 0, 0, 0, 1, PSMCT24), trxpos(0, 0, 0, 0, 0), trxreg(4, 1), 0);
    gs_mem_xfer_write(p24, 2); gs_mem_xfer_write(p24 + 2, 5); gs_mem_xfer_write(p24 + 7, 5);
    CHECK(gs_mem_read(PSMCT32, 0, 1, 0, 0) == 0xEE030201u && gs_mem_read(PSMCT32, 0, 1, 1, 0) == 0xEE060504u &&
          gs_mem_read(PSMCT32, 0, 1, 3, 0) == 0xEE0C0B0Au, "CT24 packed upload keeps the top byte");

    /* 4-bit: low nibble first */
    memset(gs_vram, 0, GS_VRAM_SIZE);
    const uint8_t p4[8] = { 0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB, 0xED, 0x0F };
    gs_mem_xfer_start(bitbltbuf(0, 0, 0, 0, 2, PSMT4), trxpos(0, 0, 0, 0, 0), trxreg(16, 1), 0);
    gs_mem_xfer_write(p4, 8);
    int ok4 = 1;
    for (int i = 0; i < 16; i++) if (gs_mem_read(PSMT4, 0, 2, i, 0) != (uint32_t)((i + 1) & 15)) ok4 = 0;
    CHECK(ok4, "T4 upload nibble order");

    /* data with no transfer is ignored; dir 3 stops a transfer */
    gs_mem_xfer_start(bitbltbuf(0, 0, 0, 0, 1, PSMCT32), trxpos(0, 0, 0, 0, 0), trxreg(8, 8), 0);
    gs_mem_xfer_start(0, 0, 0, 3);
    CHECK(!gs_mem_xfer_active(), "TRXDIR=3 deactivates");
}

static void test_local_local(void) {
    /* plain copy between buffers of different width */
    fill_random();
    gs_mem_xfer_start(bitbltbuf(0, 4, PSMCT32, 32 * 40, 7, PSMCT32), trxpos(10, 20, 100, 3, 0), trxreg(150, 70), 2);
    int bad = 0;
    for (int y = 0; y < 70; y++)
        for (int x = 0; x < 150; x++)
            if (gs_mem_read(PSMCT32, 32 * 40, 7, 100 + x, 3 + y) != gs_mem_read(PSMCT32, 0, 4, 10 + x, 20 + y)) bad++;
    CHECK(bad == 0, "CT32 local->local copy (%d bad)", bad);

    /* same bpp, different layouts */
    struct { int s, d; } pairs[] = { { PSMCT16, PSMCT16S }, { PSMZ16S, PSMCT16 }, { PSMT4, PSMT4 }, { PSMT8, PSMT8 },
                                     { PSMCT32, PSMZ32 }, { PSMT8H, PSMT8H }, { PSMT4HH, PSMT4HL } };
    for (size_t k = 0; k < sizeof pairs / sizeof pairs[0]; k++) {
        fill_random();
        memcpy(snap, gs_vram, GS_VRAM_SIZE);
        gs_mem_xfer_start(bitbltbuf(0, 4, (uint32_t)pairs[k].s, 32 * 64, 4, (uint32_t)pairs[k].d), trxpos(3, 5, 7, 9, 0), trxreg(200, 90), 2);
        bad = 0;
        for (int y = 0; y < 90; y++)
            for (int x = 0; x < 200; x++)
                if (gs_mem_read(pairs[k].d, 32 * 64, 4, 7 + x, 9 + y) != gs_mem_read(pairs[k].s, 0, 4, 3 + x, 5 + y)) bad++;
        CHECK(bad == 0, "%s -> %s local copy (%d bad)", psm_name(pairs[k].s), psm_name(pairs[k].d), bad);
    }

    /* CT24 copy keeps the destination's top byte */
    fill_random();
    uint32_t dtop = gs_mem_read(PSMCT32, 32 * 8, 1, 2, 2) & 0xFF000000u;
    gs_mem_xfer_start(bitbltbuf(0, 1, PSMCT24, 32 * 8, 1, PSMCT24), trxpos(0, 0, 0, 0, 0), trxreg(4, 4), 2);
    CHECK(gs_mem_read(PSMCT32, 32 * 8, 1, 2, 2) == (dtop | gs_mem_read(PSMCT24, 0, 1, 2, 2)), "CT24 local copy keeps top byte");

    /* overlapping copies and DIR */
    for (int dir = 0; dir < 4; dir++) {
        /* horizontal shift right by 1 */
        memset(gs_vram, 0, GS_VRAM_SIZE);
        for (int x = 0; x < 64; x++) gs_mem_write(PSMCT32, 0, 2, x, 0, (uint32_t)x + 1);
        gs_mem_xfer_start(bitbltbuf(0, 2, PSMCT32, 0, 2, PSMCT32), trxpos(0, 0, 1, 0, (uint32_t)dir), trxreg(50, 1), 2);
        int shifted = 1, smeared = 1;
        for (int x = 1; x <= 50; x++) {
            if (gs_mem_read(PSMCT32, 0, 2, x, 0) != (uint32_t)x) shifted = 0;
            if (gs_mem_read(PSMCT32, 0, 2, x, 0) != 1u) smeared = 0;
        }
        if (dir & 2) CHECK(shifted, "DIR=%d (right to left) shifts right correctly", dir);
        else CHECK(smeared, "DIR=%d (left to right) smears when shifting right", dir);

        /* vertical shift down by 1 */
        memset(gs_vram, 0, GS_VRAM_SIZE);
        for (int y = 0; y < 64; y++) gs_mem_write(PSMCT16, 0, 1, 5, y, (uint32_t)y + 1);
        gs_mem_xfer_start(bitbltbuf(0, 1, PSMCT16, 0, 1, PSMCT16), trxpos(5, 0, 5, 1, (uint32_t)dir), trxreg(1, 40), 2);
        shifted = 1; smeared = 1;
        for (int y = 1; y <= 40; y++) {
            if (gs_mem_read(PSMCT16, 0, 1, 5, y) != (uint32_t)y) shifted = 0;
            if (gs_mem_read(PSMCT16, 0, 1, 5, y) != 1u) smeared = 0;
        }
        if (dir & 1) CHECK(shifted, "DIR=%d (bottom to top) shifts down correctly", dir);
        else CHECK(smeared, "DIR=%d (top to bottom) smears when shifting down", dir);
    }
    /* shifting left/up with DIR 0 is exact */
    memset(gs_vram, 0, GS_VRAM_SIZE);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) gs_mem_write(PSMT8, 0, 2, x, y, (uint32_t)(y * 16 + x));
    gs_mem_xfer_start(bitbltbuf(0, 2, PSMT8, 0, 2, PSMT8), trxpos(1, 1, 0, 0, 0), trxreg(15, 7), 2);
    bad = 0;
    for (int y = 0; y < 7; y++) for (int x = 0; x < 15; x++) if (gs_mem_read(PSMT8, 0, 2, x, y) != (uint32_t)((y + 1) * 16 + x + 1)) bad++;
    CHECK(bad == 0, "T8 overlapping copy up-left with DIR=0");
}

/* ---- CLUT and texel fetch ------------------------------------------------------------------- */

static gs_tex0 tex0(uint32_t psm, uint32_t tbp, uint32_t tbw, uint32_t cbp, uint32_t cpsm, uint32_t csm, uint32_t csa, uint32_t cld) {
    gs_tex0 t;
    memset(&t, 0, sizeof t);
    t.tbp0 = tbp; t.tbw = tbw; t.psm = psm; t.tw = 8; t.th = 8; t.tcc = 1;
    t.cbp = cbp; t.cpsm = cpsm; t.csm = csm; t.csa = csa; t.cld = cld;
    return t;
}
static uint32_t exp16(uint32_t c, const gs_texa* ta) {
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    uint32_t a = (c & 0x8000) ? ta->ta1 : (ta->aem && !(c & 0x7FFF)) ? 0 : ta->ta0;
    return r | g << 8 | b << 16 | a << 24;
}

static void test_clut(void) {
    gs_texclut tc = { 0, 0, 0 };
    gs_texa ta = { 0x80, 0, 0x40 };
    const uint32_t TBP = 32 * 100, CBP = 32 * 200, CBP2 = 32 * 210;
    fill_random();

    /* T4 texture with indices 0..15 along a row */
    for (int i = 0; i < 16; i++) gs_mem_write(PSMT4, TBP, 2, i, 0, (uint32_t)i);
    /* CSM1, CT32, 16 entries: 8x2 */
    uint32_t c32[256];
    for (int i = 0; i < 256; i++) c32[i] = rnd();
    for (int i = 0; i < 16; i++) gs_mem_write(PSMCT32, CBP, 1, i & 7, i >> 3, c32[i]);
    gs_tex0 t = tex0(PSMT4, TBP, 2, CBP, PSMCT32, 0, 3, 1);
    gs_clut_on_tex0(&t, &tc);
    int ok = 1;
    for (int i = 0; i < 16; i++) if (gs_tex_fetch(&t, &ta, i, 0) != c32[i]) ok = 0;
    CHECK(ok, "T4 CSM1 CT32 CSA=3");
    const uint16_t* cb = gs_clut_buffer();
    ok = 1;
    for (int i = 0; i < 16; i++) if (cb[48 + i] != (uint16_t)c32[i] || cb[256 + 48 + i] != (uint16_t)(c32[i] >> 16)) ok = 0;
    CHECK(ok, "CT32 entries split into low half at CSA*16+i and high half at +256");
    /* the same buffer seen as CT16 at CSA 3 (low halves) and 19 (high halves) */
    gs_tex0 v16 = tex0(PSMT4, TBP, 2, CBP, PSMCT16, 0, 3, 0);
    gs_tex0 v16h = tex0(PSMT4, TBP, 2, CBP, PSMCT16, 0, 19, 0);
    gs_clut_on_tex0(&v16, &tc);
    ok = 1;
    for (int i = 0; i < 16; i++)
        if (gs_tex_fetch(&v16, &ta, i, 0) != exp16(c32[i] & 0xFFFF, &ta) || gs_tex_fetch(&v16h, &ta, i, 0) != exp16(c32[i] >> 16, &ta)) ok = 0;
    CHECK(ok, "CT16 view of a CT32 CLUT: CSA 3 = low halves, CSA 19 = high halves");

    /* CSM1, CT16, 16 entries at CSA 21; CT32 entries at CSA 3 untouched */
    uint16_t c16[256];
    for (int i = 0; i < 256; i++) c16[i] = (uint16_t)rnd();
    for (int i = 0; i < 16; i++) gs_mem_write(PSMCT16, CBP2, 1, i & 7, i >> 3, c16[i]);
    t = tex0(PSMT4, TBP, 2, CBP2, PSMCT16, 0, 21, 1);
    gs_clut_on_tex0(&t, &tc);
    ok = 1;
    for (int i = 0; i < 16; i++) if (gs_tex_fetch(&t, &ta, i, 0) != exp16(c16[i], &ta)) ok = 0;
    CHECK(ok, "T4 CSM1 CT16 CSA=21");
    t = tex0(PSMT4, TBP, 2, CBP, PSMCT32, 0, 3, 0);
    ok = 1;
    for (int i = 0; i < 16; i++) if (gs_tex_fetch(&t, &ta, i, 0) != c32[i]) ok = 0;
    CHECK(ok, "CT32 CLUT at CSA 3 survives a CT16 load at CSA 21 (halves 336.. are outside 48..63/304..319)");

    /* CSM1, T8, CT32: 16x16 with the documented entry arrangement */
    static const int man_t8clut_row[2][16] = {     /* first two rows of the 16x16 CLUT image */
        { 0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23 },
        { 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31 } };
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            gs_mem_write(PSMCT32, CBP, 1, x, y, c32[man_t8clut_row[y & 1][x] + (y >> 1) * 32]);
    for (int i = 0; i < 256; i++) gs_mem_write(PSMT8, TBP, 2, i & 15, i >> 4, (uint32_t)i);
    t = tex0(PSMT8, TBP, 2, CBP, PSMCT32, 0, 0, 1);
    gs_clut_on_tex0(&t, &tc);
    ok = 1;
    for (int i = 0; i < 256; i++) if (gs_tex_fetch(&t, &ta, i & 15, i >> 4) != c32[i]) ok = 0;
    CHECK(ok, "T8 CSM1 CT32 swizzled 16x16 CLUT");
    CHECK(gs_tex_fetch(&t, &ta, 8, 0) == gs_mem_read(PSMCT32, CBP, 1, 0, 1), "index 8 comes from CLUT pixel (0,1)");
    CHECK(gs_tex_fetch(&t, &ta, 0, 1) == gs_mem_read(PSMCT32, CBP, 1, 8, 0), "index 16 comes from CLUT pixel (8,0)");

    /* CSM1, T8, CT16 */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            gs_mem_write(PSMCT16S, CBP2, 1, x, y, c16[man_t8clut_row[y & 1][x] + (y >> 1) * 32]);
    t = tex0(PSMT8, TBP, 2, CBP2, PSMCT16S, 0, 0, 1);
    gs_clut_on_tex0(&t, &tc);
    ok = 1;
    for (int i = 0; i < 256; i++) if (gs_tex_fetch(&t, &ta, i & 15, i >> 4) != exp16(c16[i], &ta)) ok = 0;
    CHECK(ok, "T8 CSM1 CT16S swizzled CLUT");

    /* CSM2: a line at (COU*16, COV) in a CBW-wide CT16 buffer */
    gs_texclut tc2 = { 5, 3, 17 };
    for (int i = 0; i < 256; i++) gs_mem_write(PSMCT16, CBP, 5, 48 + i, 17, c16[255 - i]);
    t = tex0(PSMT8, TBP, 2, CBP, PSMCT16, 1, 0, 1);
    gs_clut_on_tex0(&t, &tc2);
    ok = 1;
    for (int i = 0; i < 256; i++) if (gs_tex_fetch(&t, &ta, i & 15, i >> 4) != exp16(c16[255 - i], &ta)) ok = 0;
    CHECK(ok, "T8 CSM2 CT16 CBW=5 COU=3 COV=17");
    gs_texclut tc3 = { 2, 1, 0 };
    for (int i = 0; i < 16; i++) gs_mem_write(PSMT4, TBP, 2, i, 0, (uint32_t)i);   /* T8 data overwrote it */
    for (int i = 0; i < 16; i++) gs_mem_write(PSMCT16, CBP2, 2, 16 + i, 0, c16[i + 100]);
    t = tex0(PSMT4, TBP, 2, CBP2, PSMCT16, 1, 0, 1);
    gs_clut_on_tex0(&t, &tc3);
    ok = 1;
    for (int i = 0; i < 16; i++) if (gs_tex_fetch(&t, &ta, i, 0) != exp16(c16[i + 100], &ta)) ok = 0;
    CHECK(ok, "T4 CSM2 CT16 COU=1");

    /* T8H / T4HL / T4HH index the CLUT from the top byte / nibbles */
    t = tex0(PSMT8, TBP, 2, CBP, PSMCT32, 0, 0, 0);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) gs_mem_write(PSMCT32, CBP, 1, x, y, c32[man_t8clut_row[y & 1][x] + (y >> 1) * 32]);
    t.cld = 1;
    gs_clut_on_tex0(&t, &tc);
    gs_mem_write(PSMCT32, TBP, 1, 0, 0, 0xA5123456u);
    t.psm = PSMT8H; t.cld = 0;
    CHECK(gs_tex_fetch(&t, &ta, 0, 0) == c32[0xA5], "T8H index from bits 24-31");
    t.psm = PSMT4HL;
    CHECK(gs_tex_fetch(&t, &ta, 0, 0) == c32[0x5], "T4HL index from bits 24-27");
    t.psm = PSMT4HH;
    CHECK(gs_tex_fetch(&t, &ta, 0, 0) == c32[0xA], "T4HH index from bits 28-31");
    t.csa = 2;
    CHECK(gs_tex_fetch(&t, &ta, 0, 0) == c32[0x2A], "T4HH with CSA=2");
}

static void test_cld(void) {
    gs_texclut tc = { 0, 0, 0 };
    gs_texa ta = { 0x80, 0, 0x40 };
    const uint32_t TBP = 32 * 100, A = 32 * 300, B = 32 * 301;
    memset(gs_vram, 0, GS_VRAM_SIZE);
    gs_mem_write(PSMT4, TBP, 2, 0, 0, 0);
    gs_mem_write(PSMCT32, A, 1, 0, 0, 0xAAAA0001u);
    gs_mem_write(PSMCT32, B, 1, 0, 0, 0xBBBB0001u);
    gs_tex0 t;
#define LOAD(cbp_, cld_) (t = tex0(PSMT4, TBP, 2, cbp_, PSMCT32, 0, 0, cld_), gs_clut_on_tex0(&t, &tc), gs_tex_fetch(&t, &ta, 0, 0))
    CHECK(LOAD(A, 1) == 0xAAAA0001u, "CLD=1 loads");
    CHECK(LOAD(B, 0) == 0xAAAA0001u, "CLD=0 keeps");
    CHECK(LOAD(B, 6) == 0xAAAA0001u && LOAD(B, 7) == 0xAAAA0001u, "CLD=6/7 keep");
    CHECK(LOAD(B, 2) == 0xBBBB0001u, "CLD=2 loads (CBP0=B)");
    gs_mem_write(PSMCT32, B, 1, 0, 0, 0xBBBB0002u);
    CHECK(LOAD(B, 4) == 0xBBBB0001u, "CLD=4 with CBP==CBP0 does not reload");
    CHECK(LOAD(A, 4) == 0xAAAA0001u, "CLD=4 with CBP!=CBP0 loads (CBP0=A)");
    CHECK(LOAD(A, 4) == 0xAAAA0001u, "CLD=4 again: no load");
    CHECK(LOAD(B, 3) == 0xBBBB0002u, "CLD=3 loads (CBP1=B)");
    gs_mem_write(PSMCT32, B, 1, 0, 0, 0xBBBB0003u);
    CHECK(LOAD(A, 0) == 0xBBBB0002u, "CLD=0 keeps");
    CHECK(LOAD(B, 5) == 0xBBBB0002u, "CLD=5 with CBP==CBP1 does not reload");
    CHECK(LOAD(B, 4) == 0xBBBB0003u, "CLD=4 compares CBP0 (A), not CBP1: loads");
    CHECK(LOAD(A, 5) == 0xAAAA0001u, "CLD=5 with CBP!=CBP1 loads (CBP1=A)");
    gs_mem_write(PSMCT32, A, 1, 0, 0, 0xAAAA0002u);
    CHECK(LOAD(A, 5) == 0xAAAA0001u, "CLD=5 again: no load");
    /* non-indexed textures never touch the CLUT */
    t = tex0(PSMCT32, TBP, 2, A, PSMCT32, 0, 0, 1);
    gs_clut_on_tex0(&t, &tc);
    CHECK(LOAD(A, 0) == 0xAAAA0001u, "PSMCT32 TEX0 with CLD=1 does not load");
#undef LOAD
}

static void test_tex_fetch(void) {
    gs_tex0 t;
    gs_texa ta = { 0x30, 0, 0x70 }, tae = { 0x30, 1, 0x70 };
    const uint32_t TBP = 32 * 50;
    fill_random();
    t = tex0(PSMCT32, TBP, 3, 0, 0, 0, 0, 0);
    gs_mem_write(PSMCT32, TBP, 3, 100, 70, 0x81C0FFEEu);
    CHECK(gs_tex_fetch(&t, &ta, 100, 70) == 0x81C0FFEEu, "CT32 fetch");

    t.psm = PSMCT24;
    gs_mem_write(PSMCT32, TBP, 3, 1, 1, 0xFF123456u);
    gs_mem_write(PSMCT32, TBP, 3, 2, 1, 0xFF000000u);
    CHECK(gs_tex_fetch(&t, &ta, 1, 1) == 0x30123456u, "CT24: alpha = TA0");
    CHECK(gs_tex_fetch(&t, &ta, 2, 1) == 0x30000000u, "CT24 black, AEM=0: TA0");
    CHECK(gs_tex_fetch(&t, &tae, 2, 1) == 0x00000000u, "CT24 black, AEM=1: 0");
    CHECK(gs_tex_fetch(&t, &tae, 1, 1) == 0x30123456u, "CT24 non-black, AEM=1: TA0");

    t.psm = PSMCT16;
    gs_mem_write(PSMCT16, TBP, 3, 5, 5, 0x7FFF);
    gs_mem_write(PSMCT16, TBP, 3, 6, 5, 0x8000);
    gs_mem_write(PSMCT16, TBP, 3, 7, 5, 0x0000);
    gs_mem_write(PSMCT16, TBP, 3, 8, 5, 0x801F | (0x11 << 5) | (0x0A << 10));
    CHECK(gs_tex_fetch(&t, &ta, 5, 5) == 0x30F8F8F8u, "CT16 white, A=0: TA0, c<<3");
    CHECK(gs_tex_fetch(&t, &tae, 6, 5) == 0x70000000u, "CT16 black, A=1, AEM=1: TA1");
    CHECK(gs_tex_fetch(&t, &tae, 7, 5) == 0x00000000u, "CT16 black, A=0, AEM=1: 0");
    CHECK(gs_tex_fetch(&t, &ta, 7, 5) == 0x30000000u, "CT16 black, A=0, AEM=0: TA0");
    CHECK(gs_tex_fetch(&t, &ta, 8, 5) == 0x705088F8u, "CT16 channels: R=0x1F G=0x11 B=0x0A");
    t.psm = PSMCT16S;
    gs_mem_write(PSMCT16S, TBP, 3, 70, 33, 0x801F);
    CHECK(gs_tex_fetch(&t, &ta, 70, 33) == 0x700000F8u, "CT16S fetch");
    t.psm = PSMZ32;
    gs_mem_write(PSMZ32, TBP, 3, 9, 9, 0x12345678u);
    CHECK(gs_tex_fetch(&t, &ta, 9, 9) == 0x12345678u, "Z32 as texture");
    t.psm = PSMZ16;
    gs_mem_write(PSMZ16, TBP, 3, 9, 9, 0x8001);
    CHECK(gs_tex_fetch(&t, &ta, 9, 9) == 0x70000008u, "Z16 as texture");

    /* 16-bit CLUT goes through TEXA */
    gs_texclut tc = { 0, 0, 0 };
    const uint32_t CBP = 32 * 60;
    gs_mem_write(PSMCT16, CBP, 1, 0, 0, 0x0000);
    gs_mem_write(PSMCT16, CBP, 1, 1, 0, 0x8000);
    gs_mem_write(PSMCT16, CBP, 1, 2, 0, 0x03E0);
    gs_mem_write(PSMT4, TBP, 2, 0, 0, 0); gs_mem_write(PSMT4, TBP, 2, 1, 0, 1); gs_mem_write(PSMT4, TBP, 2, 2, 0, 2);
    t = tex0(PSMT4, TBP, 2, CBP, PSMCT16, 0, 0, 1);
    gs_clut_on_tex0(&t, &tc);
    CHECK(gs_tex_fetch(&t, &tae, 0, 0) == 0 && gs_tex_fetch(&t, &tae, 1, 0) == 0x70000000u &&
          gs_tex_fetch(&t, &tae, 2, 0) == 0x3000F800u, "CT16 CLUT entries expanded through TEXA");
}

/* Not a check: rough costs of the hot paths. */
#include <time.h>
static void bench(void) {
    static uint8_t frame[640 * 448 * 4];
    for (size_t i = 0; i < sizeof frame; i++) frame[i] = (uint8_t)(i * 7);
    clock_t t0 = clock();
    for (int k = 0; k < 20; k++) {
        gs_mem_xfer_start(bitbltbuf(0, 0, 0, 0, 10, PSMCT32), trxpos(0, 0, 0, 0, 0), trxreg(640, 448), 0);
        for (size_t off = 0; off < sizeof frame; off += 4096) gs_mem_xfer_write(frame + off, 4096);
    }
    double up = (double)(clock() - t0) / CLOCKS_PER_SEC / 20;
    gs_tex0 t = tex0(PSMT8, 32 * 300, 4, 32 * 400, PSMCT32, 0, 0, 1);
    gs_texclut tc = { 0, 0, 0 };
    gs_texa ta = { 0x80, 0, 0x80 };
    gs_clut_on_tex0(&t, &tc);
    uint32_t acc = 0;
    t0 = clock();
    for (int k = 0; k < 40; k++)
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++) acc += gs_tex_fetch(&t, &ta, x, y);
    double tf = (double)(clock() - t0) / CLOCKS_PER_SEC / (40.0 * 65536) * 1e9;
    t0 = clock();
    for (int k = 0; k < 40; k++)
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++) gs_mem_write(PSMCT32, 0, 10, x, y, gs_mem_read(PSMZ24, 32 * 100, 10, x, y) + acc);
    double rw = (double)(clock() - t0) / CLOCKS_PER_SEC / (40.0 * 65536) * 1e9;
    printf("bench: 640x448 CT32 upload %.2f ms, T8 tex fetch %.1f ns, Z24 read + CT32 write %.1f ns\n", up * 1e3, tf, rw);
}

int main(void) {
    test_permutations();
    test_block_arrangement();
    test_columns();
    test_pages_and_wrap();
    test_cross_format();
    test_roundtrip();
    test_host_local();
    test_local_local();
    test_clut();
    test_cld();
    test_tex_fetch();
    bench();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
