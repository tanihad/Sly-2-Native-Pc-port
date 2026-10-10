/* IPU VLC tables and scan orders, checked in isolation against structural properties of
 * ISO/IEC 13818-2 Annex B and Figures 7-2/7-3. */
#include "fakes.h"

#include <math.h>

static int code_len(const char* s, uint32_t* bits) {
    int n = 0;
    uint32_t v = 0;
    for (; *s; s++) if (*s != ' ') { v = (v << 1) | (uint32_t)(*s == '1'); n++; }
    *bits = v;
    return n;
}

static double kraft(int t) {
    double sum = 0;
    for (int i = 0; i < ipu_vlc_code_count[t]; i++) {
        uint32_t b;
        sum += ldexp(1.0, -code_len(ipu_vlc_codes[t][i].code, &b));
    }
    return sum;
}

/* Every code decodes to itself whatever bits follow it. */
static void check_lookup(int t, const char* name) {
    uint64_t seed = 0x1234567ull + (uint64_t)t;
    for (int i = 0; i < ipu_vlc_code_count[t]; i++) {
        uint32_t c;
        int len = code_len(ipu_vlc_codes[t][i].code, &c);
        for (int rep = 0; rep < 16; rep++) {
            uint32_t tail = rep == 0 ? 0 : rep == 1 ? 0xFFFFFFFFu : test_rand(&seed);
            uint32_t bits = (c << (32 - len)) | (tail >> len);
            ipu_vlc_entry e = ipu_vlc_get(&ipu_vlc_tables[t], bits);
            CHECK(e.len == len && e.val == ipu_vlc_codes[t][i].val, "%s: code %s decodes as len %d val %d",
                  name, ipu_vlc_codes[t][i].code, e.len, e.val);
        }
    }
}

static void test_kraft(void) {
    static const struct { int t; double expect; const char* name; } k[] = {
        { IPU_VLC_MBAI, 1.0 - 2.0 / 256 - 6.0 / 2048, "macroblock_address_increment" },
        { IPU_VLC_MBT_I, 0.75, "macroblock_type I" },
        { IPU_VLC_MBT_P, 1.0 - 1.0 / 64, "macroblock_type P" },
        { IPU_VLC_MBT_B, 1.0 - 1.0 / 64, "macroblock_type B" },
        { IPU_VLC_MBT_D, 0.5, "macroblock_type D" },
        { IPU_VLC_MOTION, 1.0 - 3.0 / 256, "motion_code" },
        { IPU_VLC_DMV, 1.0, "dmvector" },
        { IPU_VLC_CBP, 1.0 - 1.0 / 512, "coded_block_pattern" },
        { IPU_VLC_DC_LUMA, 1.0, "dct_dc_size_luminance" },
        { IPU_VLC_DC_CHROMA, 1.0, "dct_dc_size_chrominance" },
        { IPU_VLC_DCT0, 1.0 - 1.0 / 4096, "DCT table B-14" },          /* only 12 leading zeros unused */
        { IPU_VLC_DCT1, 1.0 - 9.0 / 4096, "DCT table B-15" },          /* + 10 codes B-15 leaves unused */
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        double s = kraft(k[i].t);
        CHECK(fabs(s - k[i].expect) < 1e-12, "%s: Kraft sum %.10f, expected %.10f", k[i].name, s, k[i].expect);
        check_lookup(k[i].t, k[i].name);
    }
}

static void test_value_sets(void) {
    /* coded_block_pattern: every pattern 0..63 exactly once */
    int seen[64] = { 0 };
    for (int i = 0; i < ipu_vlc_code_count[IPU_VLC_CBP]; i++) seen[ipu_vlc_codes[IPU_VLC_CBP][i].val & 63]++;
    for (int v = 0; v < 64; v++) CHECK(seen[v] == 1, "cbp %d appears %d times", v, seen[v]);

    /* macroblock_address_increment: 1..33 once, plus stuffing and escape */
    int mb[64] = { 0 };
    for (int i = 0; i < ipu_vlc_code_count[IPU_VLC_MBAI]; i++) mb[ipu_vlc_codes[IPU_VLC_MBAI][i].val]++;
    for (int v = 1; v <= 35; v++) CHECK(mb[v] == 1, "mbai %d appears %d times", v, mb[v]);

    /* motion_code: -16..16 once; -m is +m with the sign (last) bit set */
    for (int m = -16; m <= 16; m++) {
        const char* code = NULL;
        int n = 0;
        for (int i = 0; i < ipu_vlc_code_count[IPU_VLC_MOTION]; i++)
            if (ipu_vlc_codes[IPU_VLC_MOTION][i].val == m) { code = ipu_vlc_codes[IPU_VLC_MOTION][i].code; n++; }
        CHECK(n == 1, "motion_code %d appears %d times", m, n);
        if (m <= 0 || !code) continue;
        for (int i = 0; i < ipu_vlc_code_count[IPU_VLC_MOTION]; i++)
            if (ipu_vlc_codes[IPU_VLC_MOTION][i].val == -m) {
                uint32_t a, b;
                int la = code_len(code, &a), lb = code_len(ipu_vlc_codes[IPU_VLC_MOTION][i].code, &b);
                CHECK(la == lb && (a ^ b) == 1 && (b & 1), "motion_code %d / %d are not sign pairs", m, -m);
            }
    }

    /* DCT tables: both code exactly the (run, level) pairs of Table B-14's structure */
    static const int max_level[32] = { 40, 18, 5, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                       1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    for (int t = IPU_VLC_DCT0; t <= IPU_VLC_DCT1; t++) {
        int count[32][41] = { { 0 } }, eob = 0, esc = 0;
        for (int i = 0; i < ipu_vlc_code_count[t]; i++) {
            int v = ipu_vlc_codes[t][i].val;
            if (v == IPU_DCT_EOB) { eob++; continue; }
            if (v == IPU_DCT_ESCAPE) { esc++; continue; }
            int run = v & 0xFF, level = v >> 8;
            CHECK(run < 32 && level >= 1 && level <= 40, "table %d: bad entry %d,%d", t, run, level);
            if (run < 32 && level <= 40) count[run][level]++;
        }
        CHECK(eob == 1 && esc == 1, "table %d: %d EOB, %d escape codes", t, eob, esc);
        for (int r = 0; r < 32; r++)
            for (int l = 1; l <= 40; l++)
                CHECK(count[r][l] == (l <= max_level[r]), "table %d: (run %d, level %d) coded %d times", t, r, l, count[r][l]);
    }
}

static void test_dct_symbols(void) {
    int run, level, len;
    /* B-14 first coefficient of a non-intra block: "1s" */
    len = ipu_dct_symbol(0x80000000u, 0, 1, 0, &run, &level);
    CHECK(len == 2 && run == 0 && level == 1, "first '10' -> len %d run %d level %d", len, run, level);
    len = ipu_dct_symbol(0xC0000000u, 0, 1, 0, &run, &level);
    CHECK(len == 2 && run == 0 && level == -1, "first '11' -> len %d run %d level %d", len, run, level);
    /* ... otherwise "10" is end of block and "11s" is (0, 1) */
    len = ipu_dct_symbol(0x80000000u, 0, 0, 0, &run, &level);
    CHECK(len == 2 && run == -1, "'10' -> len %d run %d", len, run);
    len = ipu_dct_symbol(0xE0000000u, 0, 0, 0, &run, &level);
    CHECK(len == 3 && run == 0 && level == -1, "'111' -> len %d run %d level %d", len, run, level);
    /* B-15 end of block "0110"; B-15 is not affected by `first` */
    len = ipu_dct_symbol(0x60000000u, 1, 1, 0, &run, &level);
    CHECK(len == 4 && run == -1, "B-15 '0110' -> len %d run %d", len, run);
    /* "0010 0110 s" = (0, 5) in B-14, (3, 2) in B-15 */
    len = ipu_dct_symbol(0x26800000u, 0, 0, 0, &run, &level);
    CHECK(len == 9 && run == 0 && level == -5, "B-14 '0010 0110 1' -> len %d run %d level %d", len, run, level);
    len = ipu_dct_symbol(0x26000000u, 1, 0, 0, &run, &level);
    CHECK(len == 9 && run == 3 && level == 2, "B-15 '0010 0110 0' -> len %d run %d level %d", len, run, level);
    /* MPEG-2 escape: 000001 + 6-bit run + 12-bit signed level */
    bitwriter w; uint8_t buf[8] = { 0 }; w.buf = buf; w.cap = 8; w.bits = 0;
    bw_code(&w, "000001"); bw_put(&w, 17, 6); bw_put(&w, (uint32_t)-1000 & 0xFFF, 12); bw_put(&w, 0, 8);
    uint32_t bits = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
    len = ipu_dct_symbol(bits, 0, 0, 0, &run, &level);
    CHECK(len == 24 && run == 17 && level == -1000, "MPEG-2 escape -> len %d run %d level %d", len, run, level);
    /* MPEG-1 escapes: 8-bit level, 0x00 + 8 bits (128..255), 0x80 + 8 bits (-255..-128) */
    static const struct { int level, len; } m1[] = { { 5, 20 }, { -127, 20 }, { 200, 28 }, { -200, 28 }, { 128, 28 }, { -128, 28 } };
    for (size_t i = 0; i < sizeof m1 / sizeof m1[0]; i++) {
        memset(buf, 0, sizeof buf); w.bits = 0;
        bw_code(&w, "000001"); bw_put(&w, 3, 6);
        int l = m1[i].level;
        if (l >= -127 && l <= 127) bw_put(&w, (uint32_t)l & 0xFF, 8);
        else if (l > 0) { bw_put(&w, 0, 8); bw_put(&w, (uint32_t)l, 8); }
        else { bw_put(&w, 0x80, 8); bw_put(&w, (uint32_t)(l + 256), 8); }
        bits = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
        len = ipu_dct_symbol(bits, 0, 0, 1, &run, &level);
        CHECK(len == m1[i].len && run == 3 && level == l, "MPEG-1 escape %d -> len %d run %d level %d", l, len, run, level);
    }
}

static void test_scans(void) {
    /* zigzag: walk the anti-diagonals, alternating direction */
    uint8_t zz[64];
    int n = 0;
    for (int d = 0; d < 15; d++)
        for (int k = 0; k <= d; k++) {
            int v = (d & 1) ? k : d - k, u = d - v;           /* even diagonals run bottom-left to top-right */
            if (u < 8 && v < 8) zz[n++] = (uint8_t)(v * 8 + u);
        }
    CHECK(n == 64 && memcmp(zz, ipu_scan[0], 64) == 0, "zigzag scan differs from the diagonal walk");

    /* alternate scan: Figure 7-3 gives the scan position of each coefficient (raster order) */
    static const uint8_t fig73[64] = {
         0,  4,  6, 20, 22, 36, 38, 52,  1,  5,  7, 21, 23, 37, 39, 53,
         2,  8, 19, 24, 34, 40, 50, 54,  3,  9, 18, 25, 35, 41, 51, 55,
        10, 17, 26, 30, 42, 46, 56, 60, 11, 16, 27, 31, 43, 47, 57, 61,
        12, 15, 28, 32, 44, 48, 58, 62, 13, 14, 29, 33, 45, 49, 59, 63,
    };
    for (int r = 0; r < 64; r++) CHECK(ipu_scan[1][fig73[r]] == r, "alternate scan position %d", fig73[r]);

    /* the default intra matrix as sceMpeg sends it to SETIQ (zigzag order, from the game's data) */
    static const uint8_t zigzag_default[64] = {
        0x08, 0x10, 0x10, 0x13, 0x10, 0x13, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x1a, 0x18, 0x1a, 0x1b,
        0x1b, 0x1b, 0x1a, 0x1a, 0x1a, 0x1a, 0x1b, 0x1b, 0x1b, 0x1d, 0x1d, 0x1d, 0x22, 0x22, 0x22, 0x1d,
        0x1d, 0x1d, 0x1b, 0x1b, 0x1d, 0x1d, 0x20, 0x20, 0x22, 0x22, 0x25, 0x26, 0x25, 0x23, 0x23, 0x22,
        0x23, 0x26, 0x26, 0x28, 0x28, 0x28, 0x30, 0x30, 0x2e, 0x2e, 0x38, 0x38, 0x3a, 0x45, 0x45, 0x53,
    };
    static const uint8_t raster_default[64] = {           /* ISO/IEC 13818-2 6.3.11 */
         8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
        19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
        22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
        26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
    };
    for (int i = 0; i < 64; i++)
        CHECK(raster_default[ipu_scan[0][i]] == zigzag_default[i], "default matrix at zigzag %d", i);
}

int main(void) {
    for (int t = 0; t < IPU_VLC_COUNT; t++) {
        ipu_vlc_table tmp;
        CHECK(ipu_vlc_build(&tmp, ipu_vlc_codes[t], ipu_vlc_code_count[t]) == 0, "table %d is not prefix-free", t);
    }
    static const ipu_vlc_code bad[] = { { "01", 1 }, { "011", 2 } };
    ipu_vlc_table tmp;
    CHECK(ipu_vlc_build(&tmp, bad, 2) != 0, "a prefix collision was not detected");
    ipu_vlc_init();
    test_kraft();
    test_value_sets();
    test_dct_symbols();
    test_scans();
    printf("test_tables: %s (%d failures)\n", test_failures ? "FAILED" : "ok", test_failures);
    return test_failures != 0;
}
