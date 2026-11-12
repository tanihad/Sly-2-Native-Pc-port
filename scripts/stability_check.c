/* (reimagine) Temporal stability of a moving shot: the share of "flicker" in a run of consecutive pictures (BMP dumps,
 * PS2_DUMP_BMP=1: 32-bit BGRX). The pictures are reduced to 8 x 8 block means of their luma (texture detail moving
 * across pixels averages out; a shadow, a light or a texture that flickers does not), then for every block and every
 * three consecutive pictures a "flicker event" is a reversal of the luma's change - up then down or down then up - with
 * both steps above the threshold. Real motion mostly changes a block one way for several pictures; flicker reverses.
 *   stability_check <dir with frame_*.bmp> [threshold (luma units of 255, default 3)] [dark (luma, default 64)]
 *                   [heat.pgm | -] [top share left out (0..1: the HUD)] [region "x0,y0,x1,y1" (shares; default all)]
 *                   [mask | marks: blocks at / next to pure-red marker pixels (in any picture of the triple) left out |
 *                    the markers only read as luma 128]
 * Prints: pictures, events per 1000 blocks x pictures over all blocks and over the dark blocks (luma < dark: the
 * shadowed regions), and the mean absolute block change (motion energy) for scale. Exit 0. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <windows.h>

/* (shadows2) a marker pixel: pure red (R high, G and B low) - reim_shadow_gate's view 8 paints the game-lit (characters)
 * so; its luma counts as 128 (the view's old flat grey: the unmasked numbers stay comparable with earlier runs) and, with
 * the "mask" argument, a block that holds one in any picture of a triple (or touches one: 1 block around) is left out */
static uint8_t* g_mark;
static int g_marks;                      /* (argv[7] "mask" or "marks": markers read as such; otherwise plain luma) */
static int read_bmp(const char* path, int* ow, int* oh, uint8_t** lum) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hd[54];
    if (fread(hd, 1, 54, f) != 54 || hd[0] != 'B' || hd[1] != 'M' || hd[28] != 32) { fclose(f); return 0; }
    int32_t w, h; uint32_t off;
    memcpy(&off, hd + 10, 4); memcpy(&w, hd + 18, 4); memcpy(&h, hd + 22, 4);
    int flip = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { fclose(f); return 0; }
    fseek(f, (long)off, SEEK_SET);
    uint8_t* row = malloc((size_t)w * 4);
    uint8_t* L = malloc((size_t)w * h);
    free(g_mark);
    g_mark = calloc((size_t)w * h, 1);
    for (int y = 0; y < h; y++) {
        if (fread(row, 4, (size_t)w, f) != (size_t)w) { free(row); free(L); fclose(f); return 0; }
        size_t o = (size_t)(flip ? h - 1 - y : y) * w;
        uint8_t* d = L + o;
        for (int x = 0; x < w; x++) {
            int r = row[4 * x + 2], g = row[4 * x + 1], b = row[4 * x];
            if (g_marks && r > 200 && g < 40 && b < 40) { d[x] = 128; g_mark[o + x] = 1; continue; }
            d[x] = (uint8_t)((54 * r + 183 * g + 19 * b) >> 8);
        }
    }
    free(row);
    fclose(f);
    *ow = w; *oh = h; *lum = L;
    return 1;
}

static int cmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: stability_check <dir> [threshold] [dark]\n"); return 2; }
    double thr = argc > 2 ? atof(argv[2]) : 3.0, dark = argc > 3 ? atof(argv[3]) : 64.0;
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\frame_*.bmp", argv[1]);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no pictures\n"); return 0; }
    char** names = NULL; int n = 0, cap = 0;
    do {
        if (n == cap) { cap = cap ? cap * 2 : 256; names = realloc(names, cap * sizeof *names); }
        names[n] = malloc(strlen(argv[1]) + strlen(fd.cFileName) + 2);
        sprintf(names[n], "%s\\%s", argv[1], fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, n, sizeof *names, cmp);
    int bw = 0, bh = 0;
    float *b0 = NULL, *b1 = NULL, *b2 = NULL;
    uint32_t* heat = NULL;   /* (argv[4]: a picture of where the reversals are, one pixel per block) */
    double ev = 0, evd = 0, cnt = 0, cntd = 0, mot = 0, j2 = 0, j1 = 0, j2d = 0, j1d = 0, pops = 0;   /* (jitter: |second difference| / |first differences|, contrast-free) */
    int pics = 0, run = 0, total = 0, top = 0;
    double topf = argc > 5 ? atof(argv[5]) : 0.0;   /* (reimagine2) the top share of the picture left out (the HUD's
                                                       icons and ticker: the same in both looks, they only dilute) */
    double rx0 = 0.0, ry0 = 0.0, rx1 = 1.0, ry1 = 1.0;   /* (shadows) argv[6] "x0,y0,x1,y1": only the blocks in that share of
                                                           the picture (e.g. the third-person character and his shadow) */
    if (argc > 6 && sscanf(argv[6], "%lf,%lf,%lf,%lf", &rx0, &ry0, &rx1, &ry1) != 4) { rx0 = ry0 = 0.0; rx1 = ry1 = 1.0; }
    long prevnum = -10;   /* (only triples of consecutive pictures - frame numbers in the names - are compared) */
    int usemask = argc > 7 && !strcmp(argv[7], "mask");   /* (shadows2) blocks at or next to a marker pixel left out */
    g_marks = argc > 7 && (usemask || !strcmp(argv[7], "marks"));
    uint8_t *m0 = NULL, *m1 = NULL, *m2 = NULL;
    double skipped = 0;
    for (int i = 0; i < n; i++) {
        int w, hh; uint8_t* L;
        const char* bn = strrchr(names[i], '_');
        long num = bn ? atol(bn + 1) : prevnum + 1;
        if (!read_bmp(names[i], &w, &hh, &L)) continue;
        if (num != prevnum + 1) run = 0;
        prevnum = num;
        int nbw = w / 8, nbh = hh / 8;
        if (!b0) {
            bw = nbw; bh = nbh; b0 = calloc(bw * bh, 4); b1 = calloc(bw * bh, 4); b2 = calloc(bw * bh, 4); heat = calloc(bw * bh, 4);
            top = (int)(topf * bh + 0.5);
            if (top < 0 || top >= bh) top = 0;
        }
        if (nbw != bw || nbh != bh) { free(L); continue; }
        if (!m0) { m0 = calloc(bw * bh, 1); m1 = calloc(bw * bh, 1); m2 = calloc(bw * bh, 1); }
        float* t = b0; b0 = b1; b1 = b2; b2 = t;   /* b2: the newest */
        uint8_t* tm = m0; m0 = m1; m1 = m2; m2 = tm;
        memset(m2, 0, bw * bh);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                int s = 0, mk = 0;
                for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { size_t o = (size_t)(by * 8 + y) * w + bx * 8 + x; s += L[o]; mk |= g_mark[o]; }
                b2[by * bw + bx] = s / 64.0f;
                if (mk)                  /* (the block and the ones around it) */
                    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++)
                        if (by + j >= 0 && by + j < bh && bx + i >= 0 && bx + i < bw) m2[(by + j) * bw + bx + i] = 1;
            }
        free(L);
        pics++; run++; total++;
        if (run < 3) continue;
        for (int k = top * bw; k < bw * bh; k++) {
            if (k % bw < (int)(rx0 * bw) || k % bw >= (int)(rx1 * bw + 0.5) || k / bw < (int)(ry0 * bh) || k / bw >= (int)(ry1 * bh + 0.5)) continue;
            if (usemask && (m0[k] | m1[k] | m2[k])) { skipped++; continue; }
            float d1 = b1[k] - b0[k], d2 = b2[k] - b1[k];
            int e = (d1 > thr && d2 < -thr) || (d1 < -thr && d2 > thr);
            ev += e; cnt++; heat[k] += e;
            mot += fabsf(d2);
            j2 += fabsf(d2 - d1); j1 += fabsf(d1) + fabsf(d2);
            if (fabsf(d2) > 16.0f && fabsf(d1) < 2.0f) pops++;   /* (a still block that jumps: a shadow popping in or out) */
            if (b1[k] < dark) { j2d += fabsf(d2 - d1); j1d += fabsf(d1) + fabsf(d2); }
            if (b1[k] < dark) { evd += e; cntd++; }
        }
    }
    if (argc > 4 && heat && strcmp(argv[4], "-")) {
        FILE* o = fopen(argv[4], "wb");
        if (o) { fprintf(o, "P5 %d %d 255\n", bw, bh); for (int k = 0; k < bw * bh; k++) { uint32_t v = pics > 2 ? heat[k] * 255u * 4u / (uint32_t)(pics - 2) : 0; fputc(v > 255 ? 255 : (int)v, o); } fclose(o); }
    }
    printf("jitter %.4f (all), %.4f (dark); pops %.3f per 1000; ", j1 > 0 ? j2 / j1 : 0.0, j1d > 0 ? j2d / j1d : 0.0,
           cnt ? pops * 1000.0 / cnt : 0.0);
    printf("pictures %d, blocks %dx%d: flicker %.3f per 1000 (all), %.3f per 1000 (dark < %.0f, %.2f%% of blocks), motion %.3f\n",
           pics, bw, bh, cnt ? ev * 1000.0 / cnt : 0.0, cntd ? evd * 1000.0 / cntd : 0.0, dark, cnt ? 100.0 * cntd / cnt : 0.0,
           cnt ? mot / cnt : 0.0);
    if (usemask) printf("masked: %.2f%% of the region's blocks left out (markers)\n", cnt + skipped > 0 ? 100.0 * skipped / (cnt + skipped) : 0.0);
    return 0;
}
