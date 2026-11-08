/* (reimagine2) How alike two runs' pictures are (determinism of a scripted run): the k-th frame_*.bmp of dir A against
 * the k-th of dir B (name order), mean |luma difference| per picture over 8 x 8 block means (texture noise averages out;
 * a different camera or Sly elsewhere does not).
 *   pic_diff <dirA> <dirB>   -> pairs, mean of the per-picture means, the largest, the share of pictures above 4 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <windows.h>

static float* blocks(const char* path, int* bw, int* bh) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t hd[54];
    if (fread(hd, 1, 54, f) != 54 || hd[0] != 'B' || hd[1] != 'M' || hd[28] != 32) { fclose(f); return NULL; }
    int32_t w, h; uint32_t off;
    memcpy(&off, hd + 10, 4); memcpy(&w, hd + 18, 4); memcpy(&h, hd + 22, 4);
    if (h < 0) h = -h;
    fseek(f, (long)off, SEEK_SET);
    *bw = w / 8; *bh = h / 8;
    float* b = calloc((size_t)*bw * *bh, sizeof(float));
    uint8_t* row = malloc((size_t)w * 4);
    for (int y = 0; y < h; y++) {
        if (fread(row, 4, (size_t)w, f) != (size_t)w) break;
        int by = y / 8;
        if (by >= *bh) continue;
        for (int x = 0; x < *bw * 8; x++)
            b[by * *bw + x / 8] += (54 * row[4 * x + 2] + 183 * row[4 * x + 1] + 19 * row[4 * x]) / 256.0f / 64.0f;
    }
    free(row);
    fclose(f);
    return b;
}

static int cmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static char** list(const char* dir, int* n) {
    char pat[1024]; snprintf(pat, sizeof pat, "%s\\frame_*.bmp", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    char** v = NULL; int cap = 0; *n = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (*n == cap) { cap = cap ? cap * 2 : 256; v = realloc(v, cap * sizeof *v); }
        v[*n] = malloc(strlen(dir) + strlen(fd.cFileName) + 2);
        sprintf(v[*n], "%s\\%s", dir, fd.cFileName);
        (*n)++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(v, *n, sizeof *v, cmp);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: pic_diff <dirA> <dirB>\n"); return 2; }
    int na, nb;
    char** a = list(argv[1], &na);
    char** b = list(argv[2], &nb);
    int n = na < nb ? na : nb;
    double sum = 0, mx = 0;
    int big = 0, pairs = 0;
    for (int i = 0; i < n; i++) {
        int w1, h1, w2, h2;
        float* p = blocks(a[i], &w1, &h1);
        float* q = blocks(b[i], &w2, &h2);
        if (p && q && w1 == w2 && h1 == h2) {
            double d = 0;
            for (int k = 0; k < w1 * h1; k++) d += fabs(p[k] - q[k]);
            d /= (double)w1 * h1;
            sum += d; pairs++;
            if (d > mx) mx = d;
            if (d > 4.0) big++;
        }
        free(p); free(q);
    }
    printf("pairs %d (of %d / %d): mean block difference %.3f, largest %.3f, %d pictures above 4\n", pairs, na, nb,
           pairs ? sum / pairs : 0.0, mx, big);
    return 0;
}
