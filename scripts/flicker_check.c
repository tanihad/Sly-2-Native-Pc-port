/* (r2-flicker) Temporal outliers in a run's consecutive pictures: frame dumps (frame_NNNNNN.png, the runtime's own
 * stored-deflate PNGs, runtime/debug.c; or frame_NNNNNN.bmp with PS2_DUMP_BMP=1, fast enough for every VBlank at 3x).
 *
 * A pixel "spikes" at picture i when pictures i - 1 and i + 1 agree within --same but picture i differs from both by
 * more than --diff: a one-picture blink. Smooth motion and animation do not spike; a frame that draws something wrong
 * (or misses it) for one picture does. Per picture: the spike share, the longest horizontal run of spiking pixels in
 * a row (a "line that flicks"), the rows with >= --rowfrac of their pixels spiking, 16 x 16 blocks more than half
 * spiking (blobs), and blinking seams: a straight vertical / horizontal edge of >= --seam pixels (a tile, page or band
 * boundary) that the pictures before and after do not have within 2 pixels. Pictures shown twice in a row are counted
 * once; a missing VBlank or a size change (render scale) starts again. The pictures at the change can only be judged
 * one-sided (against the two before / after), which cannot tell a blink from the start of a camera move: they are
 * printed but not counted unless --edges.
 *
 *   flicker_check <frames dir> [--same 12] [--diff 40] [--rowfrac 0.25] [--max-share 0.002] [--max-run 0.25]
 *                 [--max-blobs 0] [--max-rows 0] [--seam 160] [--max-seams 2] [--out worst.png] [--heat heat.png]
 *                 [--quiet] [--from N] [--to N] [--edges]
 *   flicker_check --ab <dir A> <dir B> [--diff 40] [--out-dir D]   per-VBlank A/B of two replays of one stream
 *   flicker_check --png <in.bmp|png> <out.png> [x y w h zoom]        a picture (or a crop) to look at
 * --max-run: the longest run allowed, as a share of the width. --heat: the last picture with the pixels that spike in
 * >= 1/8 of the pictures (with PS2_R2_ALT: what modern and exact frames draw differently, persistently).
 * Exit 0 when every picture is within the limits, 1 otherwise, 2 on errors (fewer than 3 pictures).
 * Build: clang -O2 -o build\flicker_check.exe scripts\flicker_check.c (scripts\flicker_run.ps1 does it). */
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

static int read_any(const char* path, pic* p) {
    size_t ln = strlen(path);
    return ln > 4 && !_stricmp(path + ln - 4, ".bmp") ? read_bmp(path, p) : read_png(path, p);
}

/* flicker_check --ab <dir A> <dir B> [--diff 40] [--out-dir D]: the same VBlanks of two replays of one stream (e.g. exact
 * and modern): per picture the share of pixels differing by more than --diff, the longest run of them in a row and the
 * 16 x 16 blobs; frames whose share is far above both neighbours are temporal outliers (a one-frame difference) */
static int ab_main(int argc, char** argv) {
    const char *da = argv[2], *db = argv[3], *od = NULL;
    int diff = 40;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--diff") && i + 1 < argc) diff = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) od = argv[++i];
    }
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\frame_*.*", da);
    WIN32_FIND_DATAA fdata;
    HANDLE hf = FindFirstFileA(pat, &fdata);
    char** names = NULL;
    int nn = 0, cap = 0;
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (nn == cap) { cap = cap ? cap * 2 : 256; names = realloc(names, sizeof *names * (size_t)cap); }
            names[nn++] = _strdup(fdata.cFileName);
        } while (FindNextFileA(hf, &fdata));
        FindClose(hf);
    }
    qsort(names, (size_t)nn, sizeof *names, cmp_name);
    double* sh = calloc((size_t)nn + 1, sizeof(double));
    for (int i = 0; i < nn; i++) {
        char pa[1200], pb[1200];
        snprintf(pa, sizeof pa, "%s\\%s", da, names[i]);
        snprintf(pb, sizeof pb, "%s\\%s", db, names[i]);
        pic a, b;
        if (!read_any(pa, &a)) continue;
        if (!read_any(pb, &b)) { free(a.px); printf("%s: missing in B\n", names[i]); continue; }
        if (a.w != b.w || a.h != b.h) { printf("%s: sizes %dx%d / %dx%d\n", names[i], a.w, a.h, b.w, b.h); free(a.px); free(b.px); continue; }
        int w = a.w, h = a.h, bw = (w + 15) / 16, bh = (h + 15) / 16, longest = 0, lrow = -1, maxd = 0;
        int* blk = calloc((size_t)bw * bh, sizeof(int));
        long long n = 0;
        for (int y = 0; y < h; y++) {
            int run = 0;
            for (int x = 0; x < w; x++) {
                size_t o = ((size_t)y * w + x) * 3;
                int d = dmax(a.px + o, b.px + o);
                if (d > maxd) maxd = d;
                if (d > diff) {
                    n++; blk[(y >> 4) * bw + (x >> 4)]++;
                    if (++run > longest) { longest = run; lrow = y; }
                    if (od) { b.px[o] = 255; b.px[o + 1] = 0; b.px[o + 2] = 255; }
                } else run = 0;
            }
        }
        int blobs = 0;
        for (int k = 0; k < bw * bh; k++) blobs += blk[k] > 128;
        sh[i] = (double)n / ((double)w * h);
        printf("%s: diff>%d %.5f, max %d, longest run %d (row %d), blobs %d\n", names[i], diff, sh[i], maxd, longest, lrow, blobs);
        if (od && (blobs || longest > w / 50)) {
            char po[1200];
            snprintf(po, sizeof po, "%s\\ab_%s.png", od, names[i]);
            size_t l = strlen(po);
            if (l > 8 && !_stricmp(po + l - 8, ".bmp.png")) strcpy(po + l - 8, ".png");
            write_png(po, b.px, w, h);
        }
        free(blk); free(a.px); free(b.px);
    }
    return 0;
}

/* straight edges: per column x the longest run of rows whose pixels x - 1 and x differ by more than t (vertical edges),
 * per row y the longest run of columns whose pixels in rows y - 1 and y do (horizontal edges) */
static void edge_runs(const pic* p, int t, int* vr, int* hr) {
    int w = p->w, h = p->h;
    int* cur = calloc((size_t)w, sizeof(int));
    for (int x = 0; x < w; x++) vr[x] = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = p->px + (size_t)y * w * 3;
        const uint8_t* up = y ? row - (size_t)w * 3 : row;
        int run = 0;
        hr[y] = 0;
        for (int x = 0; x < w; x++) {
            if (x && dmax(row + 3 * x - 3, row + 3 * x) > t) { if (++cur[x] > vr[x]) vr[x] = cur[x]; } else cur[x] = 0;
            if (y && dmax(up + 3 * x, row + 3 * x) > t) { if (++run > hr[y]) hr[y] = run; } else run = 0;
        }
    }
    free(cur);
}

/* a straight edge (a seam: a tile / page / band boundary) in m, at least S.seam pixels long, that r1 and r2 do not have
 * there (less than a quarter of it): the longest such edge, *pos its column (vertical, > 0) or -row (horizontal) */
static int seam_blink(const pic* m, const pic* r1, const pic* r2, int minlen, int* pos) {
    int w = m->w, h = m->h, n = w > h ? w : h, best = 0;
    int *v0 = malloc(sizeof(int) * (size_t)n * 6), *h0 = v0 + n, *v1 = h0 + n, *h1 = v1 + n, *v2 = h1 + n, *h2 = v2 + n;
    edge_runs(m, 24, v0, h0); edge_runs(r1, 24, v1, h1); edge_runs(r2, 24, v2, h2);
    *pos = 0;
    /* (the neighbours' edges within 2 pixels count: an edge that moves is no seam) */
    for (int x = 1; x < w; x++) {
        if (v0[x] < minlen || v0[x] <= best) continue;
        int nb = 0;
        for (int k = x - 2; k <= x + 2; k++) if (k > 0 && k < w) { if (v1[k] > nb) nb = v1[k]; if (v2[k] > nb) nb = v2[k]; }
        if (nb * 4 < v0[x]) { best = v0[x]; *pos = x; }
    }
    for (int y = 1; y < h; y++) {
        if (h0[y] < minlen || h0[y] <= best) continue;
        int nb = 0;
        for (int k = y - 2; k <= y + 2; k++) if (k > 0 && k < h) { if (h1[k] > nb) nb = h1[k]; if (h2[k] > nb) nb = h2[k]; }
        if (nb * 4 < h0[y]) { best = h0[y]; *pos = -y; }
    }
    free(v0);
    return best;
}

/* the picture m against r1 and r2, which agree with each other: its blinking pixels (main's statistics in S) */
static struct {
    int same, diff, quiet, max_blobs, npics, bad, nres, rows_max, blobs_max, ww, wh, want_worst, seam, seams, max_rows, max_seams;
    double rowfrac, max_share, max_run, worst_score, share_max, run_max;
    uint32_t worst_vb;
    uint8_t* worst;
    int* heat; int heat_w, heat_h, heat_n; uint8_t* heat_pic;   /* (--heat: spikes per pixel over the run) */
    int edges, nside;            /* (--edges: one-sided judgements count too; nside: how many were not counted) */
} S = { 12, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 160, 0, 0, 2, 0.25, 0.002, 0.25, -1, 0, 0, 0, NULL };

static void eval(const pic* m, const pic* r1, const pic* r2, const char* tag) {
    int w = m->w, h = m->h, bw = (w + 15) / 16, bh = (h + 15) / 16;
    int* blk = calloc((size_t)bw * bh, sizeof(int));
    uint8_t* mask = calloc((size_t)w * h, 1);
    long long spikes = 0;
    int longest = 0, rows = 0, worst_row = -1;
    for (int y = 0; y < h; y++) {
        int run = 0, rcount = 0;
        for (int x = 0; x < w; x++) {
            size_t o = ((size_t)y * w + x) * 3;
            int s = dmax(r1->px + o, r2->px + o) <= S.same && dmax(r1->px + o, m->px + o) > S.diff && dmax(r2->px + o, m->px + o) > S.diff;
            if (s) {
                mask[(size_t)y * w + x] = 1;
                if (S.heat && S.heat_w == w && S.heat_h == h) S.heat[(size_t)y * w + x]++;
                spikes++; rcount++;
                blk[(y >> 4) * bw + (x >> 4)]++;
                if (++run > longest) { longest = run; worst_row = y; }
            } else run = 0;
        }
        if (rcount >= S.rowfrac * w) rows++;
    }
    if (S.heat_w == w && S.heat_h == h) { S.heat_n++; if (!S.heat_pic) S.heat_pic = malloc((size_t)w * h * 3); memcpy(S.heat_pic, m->px, (size_t)w * h * 3); }
    else if (S.heat_w == -1) { S.heat_w = w; S.heat_h = h; S.heat = calloc((size_t)w * h, sizeof(int)); }
    int blobs = 0;
    for (int k = 0; k < bw * bh; k++) blobs += blk[k] > 128;
    double share = (double)spikes / ((double)w * h), runf = (double)longest / w;
    int spos = 0, seam = S.seam > 0 ? seam_blink(m, r1, r2, S.seam, &spos) : 0;
    /* a one-sided judgement (the picture beside a gap or a size change, against the two on one side) cannot tell a
     * blink from the start of a camera move: informational unless --edges */
    if (tag[0] && !S.edges) {
        S.nside++;
        int o = share > S.max_share || runf > S.max_run || blobs > S.max_blobs || rows > S.max_rows;
        if (!S.quiet || o)
            printf("v%u%s: spikes %.5f, longest run %d, rows %d, blobs %d%s (one-sided: not counted)\n", m->vb, tag, share, longest,
                   rows, blobs, seam ? ", seam" : "");
        free(blk); free(mask);
        return;
    }
    if (seam) S.seams++;
    int over = share > S.max_share || runf > S.max_run || blobs > S.max_blobs || rows > S.max_rows;
    if (over) S.bad++;
    if (share > S.share_max) S.share_max = share;
    if (runf > S.run_max) S.run_max = runf;
    if (rows > S.rows_max) S.rows_max = rows;
    if (blobs > S.blobs_max) S.blobs_max = blobs;
    if (!S.quiet || over)
        printf("v%u%s: spikes %.5f, longest run %d (%.3f of the width, row %d), rows %d, blobs %d%s%s\n", m->vb, tag, share, longest,
               runf, worst_row, rows, blobs, seam ? (spos > 0 ? ", SEAM vertical" : ", SEAM horizontal") : "", over ? "  <-- over" : "");
    if (seam) printf("v%u: seam of %d pixels at %s %d\n", m->vb, seam, spos > 0 ? "column" : "row", spos > 0 ? spos : -spos);
    double score = share + runf + blobs * 0.001;
    if (S.want_worst && score > S.worst_score) {
        S.worst_score = score; S.worst_vb = m->vb; S.ww = w; S.wh = h;
        free(S.worst);
        S.worst = malloc((size_t)w * h * 3);
        memcpy(S.worst, m->px, (size_t)w * h * 3);
        for (size_t k = 0; k < (size_t)w * h; k++) if (mask[k]) { S.worst[k * 3] = 255; S.worst[k * 3 + 1] = 0; S.worst[k * 3 + 2] = 255; }
    }
    free(blk); free(mask);
}

int main(int argc, char** argv) {
    const char* dir = NULL, *out = NULL, *heat_out = NULL;
    uint32_t from = 0, to = 0xFFFFFFFFu;
    if (argc >= 4 && !strcmp(argv[1], "--ab")) return ab_main(argc, argv);
    if ((argc == 4 || argc == 9) && !strcmp(argv[1], "--png")) {   /* --png <in .bmp/.png> <out.png> [x y w h zoom]: to look at */
        pic p;
        if (!read_any(argv[2], &p)) return 2;
        if (argc == 9) {
            int cx = atoi(argv[4]), cy = atoi(argv[5]), cw = atoi(argv[6]), ch = atoi(argv[7]), z = atoi(argv[8]);
            if (z < 1) z = 1;
            uint8_t* o = malloc((size_t)cw * z * ch * z * 3);
            for (int y = 0; y < ch * z; y++)
                for (int x = 0; x < cw * z; x++) {
                    int sx = cx + x / z, sy = cy + y / z;
                    uint8_t* d = o + ((size_t)y * cw * z + x) * 3;
                    if (sx < 0 || sy < 0 || sx >= p.w || sy >= p.h) { d[0] = d[1] = d[2] = 0; continue; }
                    memcpy(d, p.px + ((size_t)sy * p.w + sx) * 3, 3);
                }
            write_png(argv[3], o, cw * z, ch * z);
            return 0;
        }
        write_png(argv[3], p.px, p.w, p.h);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--same") && i + 1 < argc) S.same = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) S.diff = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rowfrac") && i + 1 < argc) S.rowfrac = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-share") && i + 1 < argc) S.max_share = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-run") && i + 1 < argc) S.max_run = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-blobs") && i + 1 < argc) S.max_blobs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) S.quiet = 1;
        else if (!strcmp(argv[i], "--edges")) S.edges = 1;
        else if (!strcmp(argv[i], "--seam") && i + 1 < argc) S.seam = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-rows") && i + 1 < argc) S.max_rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-seams") && i + 1 < argc) S.max_seams = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--heat") && i + 1 < argc) { heat_out = argv[++i]; S.heat_w = -1; }
        else dir = argv[i];
    }
    if (!dir) { fprintf(stderr, "usage: flicker_check <frames dir> [options]\n"); return 2; }
    S.want_worst = out != NULL;
    int quiet = S.quiet;
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
    int have = 0, fresh = 1;
    uint32_t last_vb = 0;
    for (int i = 0; i < nn; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        pic p;
        if (!read_any(path, &p)) { if (!quiet) printf("%s: unreadable\n", names[i]); continue; }
        p.vb = (uint32_t)strtoul(names[i] + 6, NULL, 10);
        if (have && p.vb != last_vb + 1) {   /* (a VBlank missing from the dump: start again) */
            S.nres++;
            for (int k = 0; k < have; k++) free(win[k].px);
            have = 0; fresh = 1;
        }
        last_vb = p.vb;
        if (have && win[have - 1].w == p.w && win[have - 1].h == p.h && !memcmp(win[have - 1].px, p.px, (size_t)p.w * p.h * 3)) { free(p.px); continue; }
        S.npics++;
        if (have && (win[have - 1].w != p.w || win[have - 1].h != p.h)) {   /* (a new size, e.g. a render scale change) */
            if (!quiet) printf("v%u: size %dx%d -> %dx%d\n", p.vb, win[have - 1].w, win[have - 1].h, p.w, p.h);
            S.nres++;
            if (have == 3) eval(&win[2], &win[0], &win[1], " (last before a size change)");   /* (one-sided) */
            for (int k = 0; k < have; k++) free(win[k].px);
            have = 0; fresh = 1;
        }
        if (have == 3) { free(win[0].px); win[0] = win[1]; win[1] = win[2]; have = 2; }
        win[have++] = p;
        if (have < 3) continue;
        if (fresh) eval(&win[0], &win[1], &win[2], " (first)");                /* (one-sided: the first picture) */
        fresh = 0;
        eval(&win[1], &win[0], &win[2], "");
    }
    int bad = S.bad + (S.seams > S.max_seams ? 1 : 0), npics = S.npics;
    uint8_t* worst = S.worst;    if (out && worst) write_png(out, worst, S.ww, S.wh);
    if (heat_out && S.heat_pic && S.heat_n) {   /* pixels spiking in >= 1/8 of the pictures: magenta */
        long long hot = 0;
        for (size_t k = 0; k < (size_t)S.heat_w * S.heat_h; k++)
            if (S.heat[k] * 8 >= S.heat_n) { S.heat_pic[k * 3] = 255; S.heat_pic[k * 3 + 1] = 0; S.heat_pic[k * 3 + 2] = 255; hot++; }
        write_png(heat_out, S.heat_pic, S.heat_w, S.heat_h);
        printf("heat: %lld pixels spike in >= 1/8 of %d pictures -> %s\n", hot, S.heat_n, heat_out);
    }
    printf("flicker: %s - %d pictures, %d over the limits (spike share max %.5f, longest run max %.3f of the width, rows max %d, "
           "blobs max %d, size changes / gaps %d (one-sided pictures not counted %d), blinking seams %d)%s\n", bad ? "FAIL" : (npics >= 3 ? "PASS" : "NO DATA"), npics, bad, S.share_max,
           S.run_max, S.rows_max, S.blobs_max, S.nres, S.nside, S.seams, out && worst ? "" : "");
    if (out && worst) printf("worst: v%u -> %s\n", S.worst_vb, out);
    return npics < 3 ? 2 : bad ? 1 : 0;
}
