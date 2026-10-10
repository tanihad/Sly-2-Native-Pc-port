/* IPU: colour space conversion (CSC, IDEC output) and PACK.
 *
 * YCbCr -> RGB follows ITU-R BT.601 (Y in 16..235, Cb/Cr in 16..240, centred on 128), with each
 * chroma sample covering 2x2 pixels. Alpha comes from the SETTH thresholds: 0 when R, G and B are
 * all below TH0, 0x40 when all are below TH1, else 0x80. RGB16 output (PSMCT16 layout: R in bits
 * 0-4, G 5-9, B 10-14, A 15) optionally adds the 4x4 ordered dither before dropping 3 bits. */
#include "ipu_internal.h"

/* 16.16 fixed-point BT.601 coefficients: 255/219, 1.402*255/224, 0.344136*255/224,
 * 0.714136*255/224, 1.772*255/224 */
enum { KY = 76309, KRV = 104597, KGU = 25675, KGV = 53279, KBU = 132201 };

static const int8_t dither[4][4] = {
    { -4,  0, -3,  1 },
    {  2, -2,  3, -1 },
    { -3,  1, -4,  0 },
    {  3, -1,  2, -2 },
};

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline uint16_t to16(int r, int g, int b, int a, int d) {
    r = clamp255(r + d) >> 3;
    g = clamp255(g + d) >> 3;
    b = clamp255(b + d) >> 3;
    return (uint16_t)(r | (g << 5) | (b << 10) | ((a == 0x40) << 15));
}

void ipu_csc_mb(const uint8_t* raw8, uint8_t* out, int rgb16, int dte, int sgn, int th0, int th1) {
    const uint8_t* Y = raw8;
    const uint8_t* Cb = raw8 + 256;
    const uint8_t* Cr = raw8 + 320;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int c = (y >> 1) * 8 + (x >> 1);
            int u = Cb[c] - 128, v = Cr[c] - 128;
            int l = (Y[y * 16 + x] - 16) * KY + 32768;
            int r = clamp255((l + KRV * v) >> 16);
            int g = clamp255((l - KGU * u - KGV * v) >> 16);
            int b = clamp255((l + KBU * u) >> 16);
            int a = (r < th0 && g < th0 && b < th0) ? 0 : (r < th1 && g < th1 && b < th1) ? 0x40 : 0x80;
            if (sgn) { r ^= 0x80; g ^= 0x80; b ^= 0x80; }   /* subtract 128, modulo 256 */
            if (rgb16) {
                uint16_t p = to16(r, g, b, a, dte ? dither[y & 3][x & 3] : 0);
                out[2 * (y * 16 + x)] = (uint8_t)p;
                out[2 * (y * 16 + x) + 1] = (uint8_t)(p >> 8);
            } else {
                uint8_t* p = out + 4 * (y * 16 + x);
                p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = (uint8_t)a;
            }
        }
}

/* PACK: one macroblock of RGB32 (16x16, 1024 bytes) to RGB16 (512 bytes) or to 4-bit indices into
 * the SETVQ colour table (128 bytes, first pixel of each pair in the low nibble). The index is the
 * table entry nearest to the pixel in RGB555 space (lowest index on ties). */
void ipu_pack_mb(const uint8_t* rgb32, uint8_t* out, int indx4, int dte, const uint16_t clut[16]) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            const uint8_t* p = rgb32 + 4 * (y * 16 + x);
            uint16_t c = to16(p[0], p[1], p[2], p[3], dte ? dither[y & 3][x & 3] : 0);
            if (!indx4) {
                out[2 * (y * 16 + x)] = (uint8_t)c;
                out[2 * (y * 16 + x) + 1] = (uint8_t)(c >> 8);
                continue;
            }
            int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, best = 0, best_d = 1 << 30;
            for (int i = 0; i < 16; i++) {
                int dr = r - (clut[i] & 31), dg = g - ((clut[i] >> 5) & 31), db = b - ((clut[i] >> 10) & 31);
                int d = dr * dr + dg * dg + db * db;
                if (d < best_d) { best_d = d; best = i; }
            }
            uint8_t* o = out + (y * 16 + x) / 2;
            if (x & 1) *o = (uint8_t)((*o & 0x0F) | (best << 4));
            else *o = (uint8_t)((*o & 0xF0) | best);
        }
}
