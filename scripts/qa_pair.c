/* (qa-b) Modern-vs-exact pairs of a PS2_R2_ALT run with modern frames (PS2_MFT=1): the hybrid frames are drawn by MFT at the
 * output resolution (e.g. 2560 x 1440) and the all-exact frames on the integer grid (e.g. 2048 x 1344, anamorphic), so
 * consecutive pictures of different sizes are one modern / exact pair of (almost) the same game moment. Both are shrunk
 * to 640 x 360 (box filter, each axis on its own: the grid picture is the 16:9 picture stretched) and compared:
 *   share of pixels differing by more than --diff (default 48; max channel), 8 x 8 cells whose mean difference is above
 *   --cell (default 24; the cells are what a person sees: a missing shadow, a lost effect, a wrong colour) and the
 *   largest connected group of such cells.
 *   qa_pair <frames dir> [--diff 48] [--cell 24] [--out pair.png] [--quiet] [--worst N]
 * --out: the worst pair (most cells) as modern | exact | difference (cells in magenta), 1920 x 360.
 * One line per pair with its VBlank numbers and the totals; exit 0 always (it measures).
 * Baseline: pairs one VBlank apart differ where things move (Sly, particles): compare with a run of a scene known good.
 * Build: clang -O2 -o build\qa_pair.exe scripts\qa_pair.c */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct { int w, h; uint8_t* px; } pic;

static int read_bmp(const char* path, pic* o) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hd[54];
    if (fread(hd, 1, 54, f) != 54 || hd[0] != 'B' || hd[1] != 'M' || hd[28] != 32) { fclose(f); return 0; }
    int32_t w, h;
    uint32_t off;
    memcpy(&off, hd + 10, 4); memcpy(&w, hd + 18, 4); memcpy(&h, hd + 22, 4);
    int flip = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { fclose(f); return 0; }
    fseek(f, (long)off, SEEK_SET);
    uint8_t* row = malloc((size_t)w * 4);
    o->w = w; o->h = h;
    o->px = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        if (fread(row, 4, (size_t)w, f) != (size_t)w) { free(row); free(o->px); fclose(f); return 0; }
        uint8_t* d = o->px + (size_t)(flip ? h - 1 - y : y) * w * 3;
        for (int x = 0; x < w; x++) { d[3 * x] = row[4 * x + 2]; d[3 * x + 1] = row[4 * x + 1]; d[3 * x + 2] = row[4 * x]; }
    }
    free(row);
    fclose(f);
    return 1;
}

static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE* f, const char* t, const uint8_t* d, size_t n) {
    uint8_t h[8];
    put32(h, (uint32_t)n); memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    put32(h, crc(crc(0, (const uint8_t*)t, 4), d, n));
    fwrite(h, 1, 4, f);
}
static void write_png(const char* path, const uint8_t* rgb, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13];
    put32(ih, (uint32_t)w); put32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    size_t raw_len = (size_t)h * (1 + 3 * (size_t)w);
    uint8_t* raw = malloc(raw_len);
    for (int y = 0; y < h; y++) { raw[(size_t)y * (1 + 3 * (size_t)w)] = 0; memcpy(raw + (size_t)y * (1 + 3 * (size_t)w) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t blocks = (raw_len + 65534) / 65535, zl = 2 + raw_len + blocks * 5 + 4;
    uint8_t* z = malloc(zl), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *q++ = off + n == raw_len; *q++ = (uint8_t)n; *q++ = (uint8_t)(n >> 8); *q++ = (uint8_t)~n; *q++ = (uint8_t)(~n >> 8);
        memcpy(q, raw + off, n); q += n;
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
    }
    put32(q, b << 16 | a); q += 4;
    chunk(f, "IDAT", z, (size_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}

#define OW 640
#define OH 360
/* box-shrink to OW x OH (each axis on its own) */
static void shrink(const pic* p, uint8_t* out) {
    for (int y = 0; y < OH; y++) {
        int y0 = (int)((long long)y * p->h / OH), y1 = (int)((long long)(y + 1) * p->h / OH);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < OW; x++) {
            int x0 = (int)((long long)x * p->w / OW), x1 = (int)((long long)(x + 1) * p->w / OW);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned s[3] = { 0, 0, 0 };
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { const uint8_t* q = p->px + ((size_t)yy * p->w + xx) * 3; s[0] += q[0]; s[1] += q[1]; s[2] += q[2]; }
            unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
            uint8_t* d = out + ((size_t)y * OW + x) * 3;
            d[0] = (uint8_t)(s[0] / n); d[1] = (uint8_t)(s[1] / n); d[2] = (uint8_t)(s[2] / n);
        }
    }
}

static int cmp_name(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: qa_pair <frames dir> [--diff 48] [--cell 24] [--out pair.png] [--quiet]\n"); return 2; }
    int diff = 48, cellth = 24, quiet = 0;
    const char* out = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--diff") && i + 1 < argc) diff = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cell") && i + 1 < argc) cellth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\frame_*.bmp", argv[1]);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "no frames\n"); return 2; }
    char** names = NULL; int n = 0;
    do { names = realloc(names, (size_t)(n + 1) * sizeof *names); names[n++] = _strdup(fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, sizeof *names, cmp_name);
    pic prev = { 0 };
    char prevname[MAX_PATH] = "";
    uint8_t* A = malloc(OW * OH * 3), *B = malloc(OW * OH * 3), *D = malloc(OW * OH * 3 * 3), *best = malloc(OW * OH * 3 * 3);
    int best_cells = -1, pairs = 0, worst_group = 0, sum_cells = 0;
    double sum_share = 0;
    static int cell[(OW / 8) * (OH / 8)], lab[(OW / 8) * (OH / 8)];
    for (int i = 0; i < n; i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", argv[1], names[i]);
        pic cur = { 0 };
        if (!read_bmp(path, &cur)) continue;
        if (prev.px && (prev.w != cur.w || prev.h != cur.h)) {
            /* a pair: the larger-width-aspect (MFT, 16:9 pixels) picture is "modern"; the other is the grid */
            const pic* mo = (long long)prev.w * 9 >= (long long)prev.h * 16 - 16 && (long long)prev.w * 9 <= (long long)prev.h * 16 + 16 ? &prev : &cur;
            const pic* ex = mo == &prev ? &cur : &prev;
            shrink(mo, A); shrink(ex, B);
            long over = 0;
            int cw = OW / 8, ch = OH / 8;
            for (int cy = 0; cy < ch; cy++) for (int cx = 0; cx < cw; cx++) {
                long s = 0;
                for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                    size_t o = ((size_t)(cy * 8 + y) * OW + cx * 8 + x) * 3;
                    int d0 = abs(A[o] - B[o]), d1 = abs(A[o + 1] - B[o + 1]), d2 = abs(A[o + 2] - B[o + 2]);
                    int d = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
                    if (d > diff) over++;
                    s += d;
                }
                cell[cy * cw + cx] = s / 64 > cellth;
            }
            int cells = 0, grp = 0;
            memset(lab, 0, sizeof lab);
            for (int c = 0; c < cw * ch; c++) cells += cell[c];
            /* largest 4-connected group */
            static int stack[(OW / 8) * (OH / 8)];
            for (int c = 0; c < cw * ch; c++) if (cell[c] && !lab[c]) {
                int sp = 0, sz = 0;
                stack[sp++] = c; lab[c] = 1;
                while (sp) {
                    int q = stack[--sp]; sz++;
                    int qx = q % cw, qy = q / cw;
                    if (qx > 0 && cell[q - 1] && !lab[q - 1]) { lab[q - 1] = 1; stack[sp++] = q - 1; }
                    if (qx < cw - 1 && cell[q + 1] && !lab[q + 1]) { lab[q + 1] = 1; stack[sp++] = q + 1; }
                    if (qy > 0 && cell[q - cw] && !lab[q - cw]) { lab[q - cw] = 1; stack[sp++] = q - cw; }
                    if (qy < ch - 1 && cell[q + cw] && !lab[q + cw]) { lab[q + cw] = 1; stack[sp++] = q + cw; }
                }
                if (sz > grp) grp = sz;
            }
            double share = (double)over / (OW * OH);
            if (!quiet) printf("%s / %s: %dx%d vs %dx%d: over %.4f, cells %d, largest group %d\n", prevname, names[i], mo->w, mo->h, ex->w, ex->h, share, cells, grp);
            pairs++; sum_cells += cells; sum_share += share;
            if (grp > worst_group) worst_group = grp;
            if (cells > best_cells) {
                best_cells = cells;
                for (int y = 0; y < OH; y++) {
                    memcpy(best + (size_t)y * OW * 9, A + (size_t)y * OW * 3, OW * 3);
                    memcpy(best + (size_t)y * OW * 9 + OW * 3, B + (size_t)y * OW * 3, OW * 3);
                    for (int x = 0; x < OW; x++) {
                        size_t o = ((size_t)y * OW + x) * 3;
                        int d = abs(A[o] - B[o]) + abs(A[o + 1] - B[o + 1]) + abs(A[o + 2] - B[o + 2]);
                        d = d > 255 ? 255 : d;
                        uint8_t* q = best + (size_t)y * OW * 9 + OW * 6 + x * 3;
                        if (cell[(y / 8) * cw + x / 8]) { q[0] = 255; q[1] = (uint8_t)(d / 4); q[2] = 255; }
                        else q[0] = q[1] = q[2] = (uint8_t)d;
                    }
                }
            }
        }
        free(prev.px);
        prev = cur;
        snprintf(prevname, sizeof prevname, "%s", names[i]);
    }
    printf("pairs: %d, mean over %.4f, mean cells %.1f, worst largest group %d, worst pair cells %d%s%s\n", pairs, pairs ? sum_share / pairs : 0.0,
           pairs ? (double)sum_cells / pairs : 0.0, worst_group, best_cells, out && best_cells >= 0 ? " -> " : "", out && best_cells >= 0 ? out : "");
    if (out && best_cells >= 0) write_png(out, best, OW * 3, OH);
    return 0;
}
