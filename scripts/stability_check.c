/* (reimagine) Temporal stability of a moving shot: the share of "flicker" in a run of consecutive pictures (BMP dumps,
 * PS2_DUMP_BMP=1: 32-bit BGRX). The pictures are reduced to 8 x 8 block means of their luma (texture detail moving
 * across pixels averages out; a shadow, a light or a texture that flickers does not), then for every block and every
 * three consecutive pictures a "flicker event" is a reversal of the luma's change - up then down or down then up - with
 * both steps above the threshold. Real motion mostly changes a block one way for several pictures; flicker reverses.
 *   stability_check <dir with frame_*.bmp> [threshold (luma units of 255, default 3)] [dark (luma, default 64)]
 * Prints: pictures, events per 1000 blocks x pictures over all blocks and over the dark blocks (luma < dark: the
 * shadowed regions), and the mean absolute block change (motion energy) for scale. Exit 0. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <windows.h>

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
    for (int y = 0; y < h; y++) {
        if (fread(row, 4, (size_t)w, f) != (size_t)w) { free(row); free(L); fclose(f); return 0; }
        uint8_t* d = L + (size_t)(flip ? h - 1 - y : y) * w;
        for (int x = 0; x < w; x++) d[x] = (uint8_t)((54 * row[4 * x + 2] + 183 * row[4 * x + 1] + 19 * row[4 * x]) >> 8);
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
    double ev = 0, evd = 0, cnt = 0, cntd = 0, mot = 0;
    int pics = 0;
    for (int i = 0; i < n; i++) {
        int w, hh; uint8_t* L;
        if (!read_bmp(names[i], &w, &hh, &L)) continue;
        int nbw = w / 8, nbh = hh / 8;
        if (!b0) { bw = nbw; bh = nbh; b0 = calloc(bw * bh, 4); b1 = calloc(bw * bh, 4); b2 = calloc(bw * bh, 4); heat = calloc(bw * bh, 4); }
        if (nbw != bw || nbh != bh) { free(L); continue; }
        float* t = b0; b0 = b1; b1 = b2; b2 = t;   /* b2: the newest */
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                int s = 0;
                for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) s += L[(size_t)(by * 8 + y) * w + bx * 8 + x];
                b2[by * bw + bx] = s / 64.0f;
            }
        free(L);
        pics++;
        if (pics < 3) continue;
        for (int k = 0; k < bw * bh; k++) {
            float d1 = b1[k] - b0[k], d2 = b2[k] - b1[k];
            int e = (d1 > thr && d2 < -thr) || (d1 < -thr && d2 > thr);
            ev += e; cnt++; heat[k] += e;
            mot += fabsf(d2);
            if (b1[k] < dark) { evd += e; cntd++; }
        }
    }
    if (argc > 4 && heat) {
        FILE* o = fopen(argv[4], "wb");
        if (o) { fprintf(o, "P5 %d %d 255\n", bw, bh); for (int k = 0; k < bw * bh; k++) { uint32_t v = pics > 2 ? heat[k] * 255u * 4u / (uint32_t)(pics - 2) : 0; fputc(v > 255 ? 255 : (int)v, o); } fclose(o); }
    }
    printf("pictures %d, blocks %dx%d: flicker %.3f per 1000 (all), %.3f per 1000 (dark < %.0f, %.0f%% of blocks), motion %.3f\n",
           pics, bw, bh, cnt ? ev * 1000.0 / cnt : 0.0, cntd ? evd * 1000.0 / cntd : 0.0, dark, cnt ? 100.0 * cntd / cnt : 0.0,
           cnt ? mot / cnt : 0.0);
    return 0;
}
