/* IPU: 8x8 inverse DCT (ISO/IEC 13818-2 Annex A), accurate to IEEE 1180-1990.
 *
 * Separable row/column transform in 64-bit fixed point. Each 1-D pass splits the transform into
 * its even part (inputs 0, 2, 4, 6) and odd part (inputs 1, 3, 5, 7):
 *
 *   x[n]     = (E[n] + O[n]) / 2,   x[7 - n] = (E[n] - O[n]) / 2,   n = 0..3
 *   E[0,3]   = (X0 + X4) c4 +- (X2 c2 + X6 c6)
 *   E[1,2]   = (X0 - X4) c4 +- (X2 c6 - X6 c2)
 *   O[0]     = X1 c1 + X3 c3 + X5 c5 + X7 c7
 *   O[1]     = X1 c3 - X3 c7 - X5 c1 - X7 c5
 *   O[2]     = X1 c5 - X3 c1 + X5 c7 + X7 c3
 *   O[3]     = X1 c7 - X3 c5 + X5 c3 - X7 c1        with ck = cos(k pi / 16)
 *
 * Constants carry 16 fraction bits and the row pass keeps 8 extra bits for the column pass, so
 * the result is the exact transform rounded to nearest except within ~2^-8 of a tie (13-bit
 * constants with 2 extra bits, as some decoders use, measurably exceed IEEE 1180's error limits). */
#include "ipu_internal.h"

enum {
    C1 = 64277,  /* round(cos(1 pi / 16) * 65536) */
    C2 = 60547,  /* cos(2 pi / 16) */
    C3 = 54491,  /* cos(3 pi / 16) */
    C4 = 46341,  /* cos(4 pi / 16) */
    C5 = 36410,  /* cos(5 pi / 16) */
    C6 = 25080,  /* cos(6 pi / 16) */
    C7 = 12785,  /* cos(7 pi / 16) */
    CONST_BITS = 16,
    PASS1_BITS = 8,
    ROW_SHIFT = CONST_BITS + 1 - PASS1_BITS,   /* the 1 is the 1-D transform's factor 1/2 */
    COL_SHIFT = CONST_BITS + 1 + PASS1_BITS,
};

/* One 1-D pass over 8 values at stride `is`, writing 8 values at stride `os`. */
static inline void idct_1d(const int64_t* x, int is, int64_t* y, int os, int shift) {
    const int64_t round = (int64_t)1 << (shift - 1);
    int64_t x0 = x[0], x1 = x[is], x2 = x[2 * is], x3 = x[3 * is];
    int64_t x4 = x[4 * is], x5 = x[5 * is], x6 = x[6 * is], x7 = x[7 * is];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {          /* DC only: same result, less work */
        int64_t v = (x0 * C4 + round) >> shift;
        for (int n = 0; n < 8; n++) y[n * os] = v;
        return;
    }
    int64_t a0 = (x0 + x4) * C4, a1 = (x0 - x4) * C4;
    int64_t b0 = x2 * C2 + x6 * C6, b1 = x2 * C6 - x6 * C2;
    int64_t e0 = a0 + b0, e3 = a0 - b0, e1 = a1 + b1, e2 = a1 - b1;
    int64_t o0 = x1 * C1 + x3 * C3 + x5 * C5 + x7 * C7;
    int64_t o1 = x1 * C3 - x3 * C7 - x5 * C1 - x7 * C5;
    int64_t o2 = x1 * C5 - x3 * C1 + x5 * C7 + x7 * C3;
    int64_t o3 = x1 * C7 - x3 * C5 + x5 * C3 - x7 * C1;
    y[0 * os] = (e0 + o0 + round) >> shift;
    y[7 * os] = (e0 - o0 + round) >> shift;
    y[1 * os] = (e1 + o1 + round) >> shift;
    y[6 * os] = (e1 - o1 + round) >> shift;
    y[2 * os] = (e2 + o2 + round) >> shift;
    y[5 * os] = (e2 - o2 + round) >> shift;
    y[3 * os] = (e3 + o3 + round) >> shift;
    y[4 * os] = (e3 - o3 + round) >> shift;
}

void ipu_idct(const int16_t in[64], int16_t* out, int stride) {
    int64_t t[64], r[64];
    for (int i = 0; i < 64; i++) t[i] = in[i];
    for (int row = 0; row < 8; row++)                   /* rows: horizontal frequencies u */
        idct_1d(&t[row * 8], 1, &r[row * 8], 1, ROW_SHIFT);
    for (int col = 0; col < 8; col++)                   /* columns: vertical frequencies v */
        idct_1d(&r[col], 8, &t[col], 8, COL_SHIFT);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int64_t v = t[y * 8 + x];
            out[y * stride + x] = (int16_t)(v < -256 ? -256 : v > 255 ? 255 : v);
        }
}
