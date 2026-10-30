/* (r2-flicker) Temporal outliers in a run's consecutive pictures (frame_NNNNNN.png of a frame dump, the runtime's own
 * stored-deflate PNGs: runtime/debug.c debug_write_png).
 *
 * A pixel "spikes" at picture i when pictures i - 1 and i + 1 agree within --same but picture i differs from both by
 * more than --diff: a one-picture blink. Smooth motion and animation do not spike; a frame that draws something wrong
 * (or misses it) for one picture does. Per picture: the spike share, the longest horizontal run of spiking pixels in
 * a row (a "line that flicks"), the rows with >= --rowfrac of their pixels spiking, and 16 x 16 blocks more than half
 * spiking (blobs). Pictures shown twice in a row are counted once.
 *
 *   flicker_check <frames dir> [--same 12] [--diff 40] [--rowfrac 0.25] [--max-share 0.002] [--max-run F]
 *                 [--max-blobs 0] [--out worst.png] [--quiet] [--from N] [--to N]
 * --max-run: the longest run allowed, as a share of the width (default 0.25).
 * Exit 0 when every picture is within the limits, 1 otherwise, 2 on errors.
 * Build: clang -O2 -o build\flicker_check.exe scripts\flicker_check.c (scripts\flicker_check.ps1 does it). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct { int w, h; uint8_t* px; uint32_t vb; } pic;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* the runtime's PNG: 8-bit RGB, filter 0, one zlib stream of stored blocks over one or more IDAT chunks */
static int read_png(const char* path, pic* o) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    int w = 0, h = 0, ok = 0;
    uint8_t* z = malloc((size_t)n);
    size_t zn = 0;
    for (long p = 8; p + 12 <= n;) {
        uint32_t len = be32(b + p);
        const uint8_t* t = b + p + 4;
        if (p + 12 + (long)len > n) break;
        if (!memcmp(t, "IHDR", 4)) {
            w = (int)be32(t + 4); h = (int)be32(t + 8);
            if (t[12] != 8 || t[13] != 2 || t[16] != 0) break;
        } else if (!memcmp(t, "IDAT", 4)) { memcpy(z + zn, t + 4, len); zn += len; }
        else if (!memcmp(t, "IEND", 4)) { ok = 1; break; }
        p += 12 + (long)len;
    }
    free(b);
    if (!ok || w <= 0 || h <= 0 || zn < 2) { free(z); return 0; }
    size_t raw_len = (size_t)h * (1 + 3 * (size_t)w), rn = 0;
    uint8_t* raw = malloc(raw_len);
    size_t q = 2;
    for (;;) {
        if (q + 5 > zn) break;
        int fin = z[q] & 1, type = (z[q] >> 1) & 3;
        if (type != 0) { rn = 0; break; }
        size_t len = z[q + 1] | (size_t)z[q + 2] << 8;
        q += 5;
        if (q + len > zn || rn + len > raw_len) { rn = 0; break; }
        memcpy(raw + rn, z + q, len);
        rn += len; q += len;
        if (fin) break;
    }
    free(z);
    if (rn != raw_len) { free(raw); return 0; }
    o->w = w; o->h = h;
    o->px = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* r = raw + (size_t)y * (1 + 3 * (size_t)w);
        if (r[0] != 0) { free(raw); free(o->px); return 0; }
        memcpy(o->px + (size_t)y * w * 3, r + 1, (size_t)w * 3);
    }
    free(raw);
    return 1;
}

/* a dump with PS2_DUMP_BMP=1: 32-bit BGRX, top-down (runtime/ps2_runtime.c dump_picture) */
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
    uint32_t c = crc(crc(0, (const uint8_t*)t, 4), d, n);
    put32(h, c);
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

static int cmp_name(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

static int dmax(const uint8_t* a, const uint8_t* b) {
    int d0 = abs(a[0] - b[0]), d1 = abs(a[1] - b[1]), d2 = abs(a[2] - b[2]);
    return d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
}

int main(int argc, char** argv) {
    const char* dir = NULL, *out = NULL;
    int same = 12, diff = 40, quiet = 0, max_blobs = 0;
    double rowfrac = 0.25, max_share = 0.002, max_run = 0.25;
    uint32_t from = 0, to = 0xFFFFFFFFu;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--same") && i + 1 < argc) same = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) diff = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rowfrac") && i + 1 < argc) rowfrac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-share") && i + 1 < argc) max_share = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-run") && i + 1 < argc) max_run = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-blobs") && i + 1 < argc) max_blobs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else dir = argv[i];
    }
    if (!dir) { fprintf(stderr, "usage: flicker_check <frames dir> [options]\n"); return 2; }
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\frame_*.*", dir);
    WIN32_FIND_DATAA fdata;
    HANDLE hf = FindFirstFileA(pat, &fdata);
    char** names = NULL;
    int nn = 0, cap = 0;
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            uint32_t vb = (uint32_t)strtoul(fdata.cFileName + 6, NULL, 10);
            if (vb < from || vb > to) continue;
            if (nn == cap) { cap = cap ? cap * 2 : 256; names = realloc(names, sizeof *names * (size_t)cap); }
            names[nn++] = _strdup(fdata.cFileName);
        } while (FindNextFileA(hf, &fdata));
        FindClose(hf);
    }
    qsort(names, (size_t)nn, sizeof *names, cmp_name);
    pic win[3];
    int have = 0, npics = 0, bad = 0, nres = 0;
    double worst_score = -1, share_max = 0, run_max = 0;
    int rows_max = 0, blobs_max = 0;
    uint32_t worst_vb = 0, last_vb = 0;
    uint8_t* worst = NULL;
    int ww = 0, wh = 0;
    for (int i = 0; i < nn; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        pic p;
        size_t ln = strlen(path);
        int isbmp = ln > 4 && !_stricmp(path + ln - 4, ".bmp");
        if (!(isbmp ? read_bmp(path, &p) : read_png(path, &p))) { if (!quiet) printf("%s: unreadable\n", names[i]); continue; }
        p.vb = (uint32_t)strtoul(names[i] + 6, NULL, 10);
        if (have && p.vb != last_vb + 1) {   /* (a VBlank missing from the dump: start again) */
            nres++;
            for (int k = 0; k < have; k++) free(win[k].px);
            have = 0;
        }
        last_vb = p.vb;
        if (have && win[have - 1].w == p.w && win[have - 1].h == p.h && !memcmp(win[have - 1].px, p.px, (size_t)p.w * p.h * 3)) { free(p.px); continue; }
        npics++;
        if (have && (win[have - 1].w != p.w || win[have - 1].h != p.h)) {   /* (a new size: start again) */
            if (!quiet) printf("v%u: size %dx%d -> %dx%d\n", p.vb, win[have - 1].w, win[have - 1].h, p.w, p.h);
            nres++;
            for (int k = 0; k < have; k++) free(win[k].px);
            have = 0;
        }
        if (have == 3) { free(win[0].px); win[0] = win[1]; win[1] = win[2]; have = 2; }
        win[have++] = p;
        if (have < 3) continue;
        const pic *a = &win[0], *m = &win[1], *c = &win[2];
        int w = m->w, h = m->h, bw = (w + 15) / 16, bh = (h + 15) / 16;
        int* blk = calloc((size_t)bw * bh, sizeof(int));
        uint8_t* mask = calloc((size_t)w * h, 1);
        long long spikes = 0;
        int longest = 0, rows = 0, worst_row = -1;
        for (int y = 0; y < h; y++) {
            int run = 0, rcount = 0;
            for (int x = 0; x < w; x++) {
                size_t o = ((size_t)y * w + x) * 3;
                int s = dmax(a->px + o, c->px + o) <= same && dmax(a->px + o, m->px + o) > diff && dmax(c->px + o, m->px + o) > diff;
                if (s) {
                    mask[(size_t)y * w + x] = 1;
                    spikes++; rcount++;
                    blk[(y >> 4) * bw + (x >> 4)]++;
                    if (++run > longest) { longest = run; worst_row = y; }
                } else run = 0;
            }
            if (rcount >= rowfrac * w) rows++;
        }
        int blobs = 0;
        for (int k = 0; k < bw * bh; k++) blobs += blk[k] > 128;
        double share = (double)spikes / ((double)w * h), runf = (double)longest / w;
        int over = share > max_share || runf > max_run || blobs > max_blobs;
        if (over) bad++;
        if (share > share_max) share_max = share;
        if (runf > run_max) run_max = runf;
        if (rows > rows_max) rows_max = rows;
        if (blobs > blobs_max) blobs_max = blobs;
        if (!quiet || over)
            printf("v%u: spikes %.5f, longest run %d (%.3f of the width, row %d), rows %d, blobs %d%s\n", m->vb, share, longest,
                   runf, worst_row, rows, blobs, over ? "  <-- over" : "");
        double score = share + runf + blobs * 0.001;
        if (out && score > worst_score) {
            worst_score = score; worst_vb = m->vb; ww = w; wh = h;
            free(worst);
            worst = malloc((size_t)w * h * 3);
            memcpy(worst, m->px, (size_t)w * h * 3);
            for (size_t k = 0; k < (size_t)w * h; k++) if (mask[k]) { worst[k * 3] = 255; worst[k * 3 + 1] = 0; worst[k * 3 + 2] = 255; }
        }
        free(blk); free(mask);
    }
    if (out && worst) write_png(out, worst, ww, wh);
    printf("flicker: %s - %d pictures, %d over the limits (spike share max %.5f, longest run max %.3f of the width, rows max %d, "
           "blobs max %d, size changes / gaps %d)%s\n", bad ? "FAIL" : (npics >= 3 ? "PASS" : "NO DATA"), npics, bad, share_max,
           run_max, rows_max, blobs_max, nres, out && worst ? "" : "");
    if (out && worst) printf("worst: v%u -> %s\n", worst_vb, out);
    return npics < 3 ? 2 : bad ? 1 : 0;
}
