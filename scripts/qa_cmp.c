/* (qa-a) Modern vs Exact (Reference) pictures of the same VBlank: where do they differ more than anti-aliasing and
 * resolution explain? Block means are compared (16 x 16 by default), so edge softness, MSAA vs supersampling and
 * outline thickness drop out; what stays is a missing / extra / misplaced object, a wrong shadow, colour or seam.
 *
 *   qa_cmp --diff A B [--blk 16] [--thr 24] [--top 8] [--min 2] [--out diff.png]
 *       connected clusters of blocks whose mean differs by more than --thr (max channel, 0..255); one line per cluster
 *       "box x y w h  blocks N  mean D" in pixels of A's size, worst first; --out: A | B | marked A, half size
 *   qa_cmp --pair A B <out.png> x y w h [zoom]
 *       a crop of A and the same region of B (resampled to A's size when they differ) and their difference
 *       (amplified x4), side by side, zoomed by an integer factor
 *   qa_cmp --crop A <out.png> x y w h [zoom]      one crop
 * A, B: .bmp (PS2_DUMP_BMP, 32-bit) or the runtime's .png. Output PNGs are uncompressed: shrink them with
 * scripts\shot_small.py (zlib) before committing.
 * Build: clang -O2 -o build\qa_cmp.exe scripts\qa_cmp.c (scripts\qa_run.ps1 does it). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int w, h; uint8_t* px; } pic;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

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

static int read_any(const char* path, pic* p) {
    size_t ln = strlen(path);
    int ok = ln > 4 && !_stricmp(path + ln - 4, ".bmp") ? read_bmp(path, p) : read_png(path, p);
    if (!ok) fprintf(stderr, "cannot read %s\n", path);
    return ok;
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
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13];
    put32(ih, (uint32_t)w); put32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    size_t rl = 1 + 3 * (size_t)w, raw_len = (size_t)h * rl;
    uint8_t* raw = malloc(raw_len);
    for (int y = 0; y < h; y++) { raw[(size_t)y * rl] = 0; memcpy(raw + (size_t)y * rl + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
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

/* B resampled to w x h (box filter when shrinking, bilinear otherwise) */
static void fit(pic* b, int w, int h) {
    if (b->w == w && b->h == h) return;
    uint8_t* o = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        double y0 = (double)y * b->h / h, y1 = (double)(y + 1) * b->h / h;
        for (int x = 0; x < w; x++) {
            double x0 = (double)x * b->w / w, x1 = (double)(x + 1) * b->w / w, s[3] = { 0, 0, 0 }, wt = 0;
            for (int yy = (int)y0; yy < b->h && yy < y1 + 0.999; yy++) {
                double wy = (yy + 1 < y1 ? yy + 1 : y1) - (yy > y0 ? yy : y0);
                if (wy <= 0) continue;
                for (int xx = (int)x0; xx < b->w && xx < x1 + 0.999; xx++) {
                    double wx = (xx + 1 < x1 ? xx + 1 : x1) - (xx > x0 ? xx : x0);
                    if (wx <= 0) continue;
                    const uint8_t* p = b->px + ((size_t)yy * b->w + xx) * 3;
                    s[0] += wx * wy * p[0]; s[1] += wx * wy * p[1]; s[2] += wx * wy * p[2]; wt += wx * wy;
                }
            }
            for (int c = 0; c < 3; c++) o[((size_t)y * w + x) * 3 + c] = (uint8_t)(wt > 0 ? s[c] / wt + 0.5 : 0);
        }
    }
    free(b->px);
    b->px = o; b->w = w; b->h = h;
}

static int crop_args(int argc, char** argv, int i, int* x, int* y, int* w, int* h, int* z) {
    if (i + 4 > argc) return 0;
    *x = atoi(argv[i]); *y = atoi(argv[i + 1]); *w = atoi(argv[i + 2]); *h = atoi(argv[i + 3]);
    *z = i + 4 < argc ? atoi(argv[i + 4]) : 1;
    if (*z < 1) *z = 1;
    return 1;
}

static void panel(uint8_t* out, int ow, int ox, const pic* p, int x, int y, int w, int h, int z, const pic* diff_with) {
    for (int yy = 0; yy < h * z; yy++)
        for (int xx = 0; xx < w * z; xx++) {
            int sx = x + xx / z, sy = y + yy / z;
            uint8_t v[3] = { 0, 0, 0 };
            if (sx >= 0 && sy >= 0 && sx < p->w && sy < p->h) {
                const uint8_t* s = p->px + ((size_t)sy * p->w + sx) * 3;
                if (diff_with) {
                    const uint8_t* t = diff_with->px + ((size_t)sy * p->w + sx) * 3;
                    for (int c = 0; c < 3; c++) { int d = abs(s[c] - t[c]) * 4; v[c] = (uint8_t)(d > 255 ? 255 : d); }
                } else memcpy(v, s, 3);
            }
            memcpy(out + ((size_t)yy * ow + ox + xx) * 3, v, 3);
        }
}

typedef struct { int x0, y0, x1, y1, n; double sum; } cluster;

int main(int argc, char** argv) {
    if (argc >= 6 && !strcmp(argv[1], "--sheet")) {          /* --sheet out.png cols cellw cellh file...: a contact sheet */
        int cols = atoi(argv[3]), cw = atoi(argv[4]), ch = atoi(argv[5]), n = argc - 6;
        if (cols < 1 || cw < 8 || ch < 8 || n < 1) return 2;
        int rows = (n + cols - 1) / cols, ow = cols * cw, oh = rows * ch;
        uint8_t* o = calloc((size_t)ow * oh, 3);
        for (int i = 0; i < n; i++) {
            pic a;
            if (!read_any(argv[6 + i], &a)) continue;
            pic c = a;
            c.px = malloc((size_t)a.w * a.h * 3); memcpy(c.px, a.px, (size_t)a.w * a.h * 3);
            fit(&c, cw - 2, ch - 2);
            int cx = (i % cols) * cw + 1, cy = (i / cols) * ch + 1;
            for (int y = 0; y < c.h; y++) memcpy(o + ((size_t)(cy + y) * ow + cx) * 3, c.px + (size_t)y * c.w * 3, (size_t)c.w * 3);
            free(c.px); free(a.px);
        }
        write_png(argv[2], o, ow, oh);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "--crop")) {
        pic a; int x, y, w, h, z;
        if (!read_any(argv[2], &a) || !crop_args(argc, argv, 4, &x, &y, &w, &h, &z)) return 2;
        uint8_t* o = malloc((size_t)w * z * h * z * 3);
        panel(o, w * z, 0, &a, x, y, w, h, z, NULL);
        write_png(argv[3], o, w * z, h * z);
        return 0;
    }
    if (argc >= 9 && !strcmp(argv[1], "--pair")) {
        pic a, b; int x, y, w, h, z;
        if (!read_any(argv[2], &a) || !read_any(argv[3], &b) || !crop_args(argc, argv, 5, &x, &y, &w, &h, &z)) return 2;
        fit(&b, a.w, a.h);
        int pw = w * z, gap = 4, ow = pw * 3 + gap * 2, oh = h * z;
        uint8_t* o = malloc((size_t)ow * oh * 3);
        memset(o, 128, (size_t)ow * oh * 3);
        panel(o, ow, 0, &a, x, y, w, h, z, NULL);
        panel(o, ow, pw + gap, &b, x, y, w, h, z, NULL);
        panel(o, ow, 2 * (pw + gap), &a, x, y, w, h, z, &b);
        write_png(argv[4], o, ow, oh);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--diff")) {
        int blk = 16, thr = 24, top = 8, minb = 2;
        const char* out = NULL;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--blk") && i + 1 < argc) blk = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--thr") && i + 1 < argc) thr = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--top") && i + 1 < argc) top = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--min") && i + 1 < argc) minb = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        }
        pic a, b;
        if (!read_any(argv[2], &a) || !read_any(argv[3], &b)) return 2;
        fit(&b, a.w, a.h);
        int bw = (a.w + blk - 1) / blk, bh = (a.h + blk - 1) / blk;
        uint8_t* d = calloc((size_t)bw * bh, 1);
        int* lab = calloc((size_t)bw * bh, sizeof(int));
        double* dv = calloc((size_t)bw * bh, sizeof(double));
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                double sa[3] = { 0, 0, 0 }, sb[3] = { 0, 0, 0 };
                int n = 0;
                for (int y = by * blk; y < (by + 1) * blk && y < a.h; y++)
                    for (int x = bx * blk; x < (bx + 1) * blk && x < a.w; x++) {
                        const uint8_t* p = a.px + ((size_t)y * a.w + x) * 3, *q = b.px + ((size_t)y * a.w + x) * 3;
                        for (int c = 0; c < 3; c++) { sa[c] += p[c]; sb[c] += q[c]; }
                        n++;
                    }
                double m = 0;
                for (int c = 0; c < 3; c++) { double v = fabs(sa[c] - sb[c]) / n; if (v > m) m = v; }
                dv[by * bw + bx] = m;
                d[by * bw + bx] = m > thr;
            }
        cluster* cl = calloc((size_t)bw * bh / 2 + 4, sizeof *cl);
        int ncl = 0, *stack = malloc(sizeof(int) * (size_t)bw * bh);
        long total = 0;
        for (int i = 0; i < bw * bh; i++) {
            if (!d[i] || lab[i]) continue;
            cluster c = { bw, bh, -1, -1, 0, 0 };
            int sp = 0;
            stack[sp++] = i; lab[i] = ++ncl;
            while (sp) {
                int k = stack[--sp], kx = k % bw, ky = k / bw;
                if (kx < c.x0) c.x0 = kx;
                if (ky < c.y0) c.y0 = ky;
                if (kx > c.x1) c.x1 = kx;
                if (ky > c.y1) c.y1 = ky;
                c.n++; c.sum += dv[k];
                static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
                for (int j = 0; j < 8; j++) {
                    int nx = kx + dx[j], ny = ky + dy[j];
                    if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                    int nk = ny * bw + nx;
                    if (d[nk] && !lab[nk]) { lab[nk] = ncl; stack[sp++] = nk; }
                }
            }
            if (c.n >= minb) { cl[ncl - 1] = c; total += c.n; } else { cl[ncl - 1].n = 0; }
        }
        /* worst first: by blocks x mean */
        int printed = 0;
        printf("blocks %d x %d (%d px), differing > %d: %ld in clusters >= %d blocks\n", bw, bh, blk, thr, total, minb);
        for (int pass = 0; pass < top; pass++) {
            int best = -1;
            double bs = 0;
            for (int i = 0; i < ncl; i++) if (cl[i].n >= minb && cl[i].sum > bs) { bs = cl[i].sum; best = i; }
            if (best < 0) break;
            cluster* c = &cl[best];
            printf("box %d %d %d %d  blocks %d  mean %.0f\n", c->x0 * blk, c->y0 * blk, (c->x1 - c->x0 + 1) * blk, (c->y1 - c->y0 + 1) * blk, c->n, c->sum / c->n);
            c->n = 0; c->sum = 0;
            printed++;
        }
        if (out) {
            int ow = a.w / 2, oh = a.h / 2, gap = 2, tw = ow * 3 + gap * 2;
            uint8_t* o = malloc((size_t)tw * oh * 3);
            memset(o, 128, (size_t)tw * oh * 3);
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++) {
                    const uint8_t* p = a.px + ((size_t)(2 * y) * a.w + 2 * x) * 3, *q = b.px + ((size_t)(2 * y) * a.w + 2 * x) * 3;
                    memcpy(o + ((size_t)y * tw + x) * 3, p, 3);
                    memcpy(o + ((size_t)y * tw + ow + gap + x) * 3, q, 3);
                    uint8_t m[3] = { p[0], p[1], p[2] };
                    int bi = (2 * y / blk) * bw + 2 * x / blk;
                    if (d[bi]) { m[0] = 255; m[1] = (uint8_t)(m[1] / 2); m[2] = (uint8_t)(m[2] / 2); }
                    memcpy(o + ((size_t)y * tw + 2 * (ow + gap) + x) * 3, m, 3);
                }
            write_png(out, o, tw, oh);
        }
        return 0;
    }
    fprintf(stderr, "usage: qa_cmp --diff A B [--blk 16] [--thr 24] [--top 8] [--min 2] [--out f.png] | --pair A B out.png x y w h [zoom] | --crop A out.png x y w h [zoom]\n");
    return 2;
}
