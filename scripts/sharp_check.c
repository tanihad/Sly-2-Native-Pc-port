/* (reimagine2) Detail / sharpness of a picture region, for 1:1 crops of Sly (Faithful vs Reimagined): BMP dumps
 * (PS2_DUMP_BMP=1, 32-bit BGRX) or 24/32-bit BMPs. Over the region's luma (0..255):
 *   grad  - mean gradient magnitude (central differences): edge contrast per pixel
 *   lap   - mean |Laplacian| (4-neighbour): fine detail / high frequencies
 *   hf    - share of the luma's variance above a 5x5 box blur (how much of the region's contrast is fine detail)
 *   acut  - mean of the top 5 % gradient magnitudes (edge acutance: how hard the outlines and edges are)
 *   sharp_check <file.bmp> <x> <y> <w> <h>   (the region in the picture's pixels, top-left origin)
 *   sharp_check <dir> <x> <y> <w> <h>        (every frame_*.bmp in dir: the means over the pictures)
 * Prints one line. Exit 0. */
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
    if (fread(hd, 1, 54, f) != 54 || hd[0] != 'B' || hd[1] != 'M' || (hd[28] != 32 && hd[28] != 24)) { fclose(f); return 0; }
    int32_t w, h; uint32_t off;
    int bpp = hd[28] / 8;
    memcpy(&off, hd + 10, 4); memcpy(&w, hd + 18, 4); memcpy(&h, hd + 22, 4);
    int flip = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { fclose(f); return 0; }
    size_t stride = ((size_t)w * bpp + 3) & ~(size_t)3;
    fseek(f, (long)off, SEEK_SET);
    uint8_t* row = malloc(stride);
    uint8_t* L = malloc((size_t)w * h);
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, stride, f) != stride) { free(row); free(L); fclose(f); return 0; }
        uint8_t* d = L + (size_t)(flip ? h - 1 - y : y) * w;
        for (int x = 0; x < w; x++) d[x] = (uint8_t)((54 * row[bpp * x + 2] + 183 * row[bpp * x + 1] + 19 * row[bpp * x]) >> 8);
    }
    free(row);
    fclose(f);
    *ow = w; *oh = h; *lum = L;
    return 1;
}

static int cmpf(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }

/* the region's four measures; 0: not inside the picture */
static int measure(const uint8_t* L, int w, int h, int rx, int ry, int rw, int rh, double* m) {
    if (rx < 2 || ry < 2 || rx + rw > w - 2 || ry + rh > h - 2 || rw < 8 || rh < 8) return 0;
    double g = 0, lp = 0, var = 0, hv = 0, mean = 0;
    float* gs = malloc(sizeof(float) * (size_t)rw * rh);
    int n = 0;
    for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) mean += L[(size_t)y * w + x];
    mean /= (double)rw * rh;
    for (int y = ry; y < ry + rh; y++)
        for (int x = rx; x < rx + rw; x++) {
            const uint8_t* p = L + (size_t)y * w + x;
            double gx = (p[1] - p[-1]) * 0.5, gy = (p[w] - p[-w]) * 0.5;
            double gm = sqrt(gx * gx + gy * gy);
            g += gm;
            gs[n++] = (float)gm;
            lp += fabs(4.0 * p[0] - p[1] - p[-1] - p[w] - p[-w]);
            double b = 0;
            for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) b += p[j * w + i];
            b /= 25.0;
            var += (p[0] - mean) * (p[0] - mean);
            hv += (p[0] - b) * (p[0] - b);
        }
    qsort(gs, (size_t)n, sizeof(float), cmpf);
    double ac = 0;
    int k0 = n - n / 20;
    for (int k = k0; k < n; k++) ac += gs[k];
    ac /= (double)(n - k0 > 0 ? n - k0 : 1);
    free(gs);
    m[0] = g / n; m[1] = lp / n; m[2] = var > 0 ? hv / var : 0; m[3] = ac;
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: sharp_check <file.bmp | dir> <x> <y> <w> <h>\n"); return 2; }
    int rx = atoi(argv[2]), ry = atoi(argv[3]), rw = atoi(argv[4]), rh = atoi(argv[5]);
    double sum[4] = { 0 }, m[4];
    int pics = 0;
    DWORD at = GetFileAttributesA(argv[1]);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
        char pat[1024];
        snprintf(pat, sizeof pat, "%s\\frame_*.bmp", argv[1]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char p[1024];
                snprintf(p, sizeof p, "%s\\%s", argv[1], fd.cFileName);
                int w, hh; uint8_t* L;
                if (!read_bmp(p, &w, &hh, &L)) continue;
                if (measure(L, w, hh, rx, ry, rw, rh, m)) { for (int k = 0; k < 4; k++) sum[k] += m[k]; pics++; }
                free(L);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    } else {
        int w, hh; uint8_t* L;
        if (read_bmp(argv[1], &w, &hh, &L)) {
            if (measure(L, w, hh, rx, ry, rw, rh, m)) { for (int k = 0; k < 4; k++) sum[k] = m[k]; pics = 1; }
            free(L);
        }
    }
    if (!pics) { printf("no pictures / region outside\n"); return 0; }
    printf("pictures %d, region %dx%d at %d,%d: grad %.3f, lap %.3f, hf %.4f, acut %.2f\n", pics, rw, rh, rx, ry,
           sum[0] / pics, sum[1] / pics, sum[2] / pics, sum[3] / pics);
    return 0;
}
