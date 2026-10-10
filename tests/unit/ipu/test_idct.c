/* IPU inverse DCT against the IEEE 1180-1990 accuracy procedure. */
#include "fakes.h"

#include <math.h>

/* the random generator specified by IEEE 1180 */
static long randx;
static long ieee_rand(long L, long H) {
    static const double z = (double)0x7fffffff;
    randx = (long)(int32_t)((uint32_t)randx * 1103515245u + 12345u);   /* 32-bit long arithmetic */
    long i = randx & 0x7ffffffe;
    double x = ((double)i) / z;
    x *= (L + H + 1);
    long j = (long)x;
    return j - L;
}

static double c_tab[8][8];               /* c_tab[k][n] = C(k)/2 cos((2n+1) k pi / 16) */

static void fdct_ref(const double in[64], double out[64]) {
    double tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            double s = 0;
            for (int x = 0; x < 8; x++) s += c_tab[u][x] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            double s = 0;
            for (int y = 0; y < 8; y++) s += c_tab[v][y] * tmp[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

static void idct_ref(const int16_t in[64], double out[64]) {
    double tmp[64];
    for (int v = 0; v < 8; v++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int u = 0; u < 8; u++) s += c_tab[u][x] * in[v * 8 + u];
            tmp[v * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int v = 0; v < 8; v++) s += c_tab[v][y] * tmp[v * 8 + x];
            out[y * 8 + x] = s;
        }
}

static int clampi(double v, int lo, int hi) {
    long r = lround(v);                  /* round half away from zero, as the procedure says */
    return r < lo ? lo : r > hi ? hi : (int)r;
}

static void run(long L, long H, int sign, int blocks) {
    randx = 1;
    double pe[64] = { 0 }, pme[64] = { 0 };
    int peak = 0;
    for (int b = 0; b < blocks; b++) {
        double block[64], coef_d[64], ref[64];
        int16_t coef[64], out[64];
        for (int i = 0; i < 64; i++) block[i] = (double)(ieee_rand(L, H) * sign);
        fdct_ref(block, coef_d);
        for (int i = 0; i < 64; i++) coef[i] = (int16_t)clampi(coef_d[i], -2048, 2047);
        idct_ref(coef, ref);
        ipu_idct(coef, out, 8);
        for (int i = 0; i < 64; i++) {
            int r = clampi(ref[i], -256, 255);
            int e = out[i] - r;
            if (abs(e) > peak) peak = abs(e);
            pe[i] += e;
            pme[i] += (double)e * e;
        }
    }
    double omse = 0, ome = 0, max_pmse = 0, max_pme = 0;
    for (int i = 0; i < 64; i++) {
        double mse = pme[i] / blocks, me = pe[i] / blocks;
        omse += mse; ome += me;
        if (mse > max_pmse) max_pmse = mse;
        if (fabs(me) > max_pme) max_pme = fabs(me);
    }
    omse /= 64; ome /= 64;
    printf("  L=%ld H=%ld sign=%+d: peak %d, max pmse %.4f, omse %.4f, max pme %.4f, ome %.5f\n",
           L, H, sign, peak, max_pmse, omse, max_pme, ome);
    CHECK(peak <= 1, "peak error %d > 1", peak);
    CHECK(max_pmse <= 0.06, "per-pixel mean square error %.4f > 0.06", max_pmse);
    CHECK(omse <= 0.02, "overall mean square error %.4f > 0.02", omse);
    CHECK(max_pme <= 0.015, "per-pixel mean error %.4f > 0.015", max_pme);
    CHECK(fabs(ome) <= 0.0015, "overall mean error %.5f > 0.0015", ome);
}

int main(void) {
    for (int k = 0; k < 8; k++)
        for (int n = 0; n < 8; n++)
            c_tab[k][n] = (k ? 0.5 : 0.5 / sqrt(2.0)) * cos((2 * n + 1) * k * 3.14159265358979323846 / 16);

    /* the integer constants are the cosines in 13-bit fixed point */
    /* (checked indirectly by the accuracy runs; zero in must give zero out) */
    int16_t zero[64] = { 0 }, out[64];
    ipu_idct(zero, out, 8);
    int nz = 0;
    for (int i = 0; i < 64; i++) nz |= out[i];
    CHECK(nz == 0, "IDCT of zero is not zero");

    /* DC-only blocks: exact value DC / 8, rounded */
    for (int dc = -2048; dc <= 2047; dc++) {
        int16_t in[64] = { 0 };
        in[0] = (int16_t)dc;
        ipu_idct(in, out, 8);
        int expect = clampi(dc / 8.0, -256, 255);
        if (out[0] != expect && abs(out[0] - expect) > 1) CHECK(0, "DC %d -> %d, expected %d", dc, out[0], expect);
    }

    /* output stride (as used for field DCT and 16-wide luminance) */
    int16_t in[64] = { 0 }, wide[8 * 32];
    in[0] = 800; in[1] = -300; in[9] = 120;
    ipu_idct(in, out, 8);
    ipu_idct(in, wide, 32);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) CHECK(wide[y * 32 + x] == out[y * 8 + x], "stride mismatch at %d,%d", x, y);

    printf("IEEE 1180-1990 (10000 blocks per case):\n");
    run(256, 255, 1, 10000);
    run(256, 255, -1, 10000);
    run(5, 5, 1, 10000);
    run(5, 5, -1, 10000);
    run(300, 300, 1, 10000);
    run(300, 300, -1, 10000);
    printf("test_idct: %s (%d failures)\n", test_failures ? "FAILED" : "ok", test_failures);
    return test_failures != 0;
}
