/* (reimagine) Frame dumps -> an MP4 (H.264) with Windows' own Media Foundation: no third-party encoder.
 *   frames_to_mp4 <out.mp4> <dir> [<dir2>] [--fps 60] [--every 2] [--mbps 40] [--width W --height H] [--first N] [--count N]
 *                 [--title "text"] [--label <file: lines "<frame number> <text>">]
 * <dir>: frame_*.bmp (PS2_DUMP_BMP=1: 32-bit BGRX, top-down or bottom-up) or frame_*.png (8-bit RGB / RGBA, as the
 * game writes them: stored or deflated). Frames in name order; --every k takes every k-th (a 120 fps dump at 60 fps).
 * With <dir2>: side by side (each half the width; both scaled into W/2 x H). The picture is box-scaled to W x H
 * (default: the first frame's size, side by side: its width x half its height). Build (llvm-mingw clang):
 *   clang -O2 -o frames_to_mp4.exe frames_to_mp4.c -lmfplat -lmfreadwrite -lmfuuid -lole32 -lgdi32 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct { int w, h; uint8_t* px; } img;   /* BGRA rows, top-down */

static int read_bmp(const char* path, img* o) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hd[54];
    if (fread(hd, 1, 54, f) != 54 || hd[0] != 'B' || hd[1] != 'M' || (hd[28] != 32 && hd[28] != 24)) { fclose(f); return 0; }
    int32_t w, h; uint32_t off; int bpp = hd[28] / 8;
    memcpy(&off, hd + 10, 4); memcpy(&w, hd + 18, 4); memcpy(&h, hd + 22, 4);
    int flip = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0) { fclose(f); return 0; }
    fseek(f, (long)off, SEEK_SET);
    size_t rs = ((size_t)w * bpp + 3) & ~(size_t)3;
    uint8_t* row = malloc(rs);
    o->w = w; o->h = h; o->px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, rs, f) != rs) { free(row); free(o->px); fclose(f); return 0; }
        uint8_t* d = o->px + (size_t)(flip ? h - 1 - y : y) * w * 4;
        for (int x = 0; x < w; x++) { d[4 * x] = row[bpp * x]; d[4 * x + 1] = row[bpp * x + 1]; d[4 * x + 2] = row[bpp * x + 2]; d[4 * x + 3] = 255; }
    }
    free(row);
    fclose(f);
    return 1;
}

/* ---- PNG (zlib inflate, filters) ---- */
typedef struct { const uint8_t* p; size_t n, pos; uint32_t bit, cnt; } bs;
static int getbit(bs* b) { if (!b->cnt) { if (b->pos >= b->n) return 0; b->bit = b->p[b->pos++]; b->cnt = 8; } int v = b->bit & 1; b->bit >>= 1; b->cnt--; return v; }
static uint32_t getbits(bs* b, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= (uint32_t)getbit(b) << i; return v; }
typedef struct { uint16_t cnt[16], sym[288]; } huff;
static void hbuild(huff* h, const uint8_t* len, int n) {
    uint16_t offs[16];
    memset(h->cnt, 0, sizeof h->cnt);
    for (int i = 0; i < n; i++) h->cnt[len[i]]++;
    h->cnt[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->cnt[i];
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}
static int hdecode(bs* b, const huff* h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= getbit(b);
        int c = h->cnt[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}
static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static size_t inflate_z(const uint8_t* z, size_t zn, uint8_t* out, size_t cap) {
    bs b = { z + 2, zn - 2, 0, 0, 0 };
    size_t o = 0;
    int last;
    do {
        last = getbit(&b);
        int type = (int)getbits(&b, 2);
        if (type == 0) {
            b.cnt = 0;
            if (b.pos + 4 > b.n) return 0;
            uint32_t len = b.p[b.pos] | b.p[b.pos + 1] << 8;
            b.pos += 4;
            if (b.pos + len > b.n || o + len > cap) return 0;
            memcpy(out + o, b.p + b.pos, len); o += len; b.pos += len;
        } else if (type == 1 || type == 2) {
            huff lh, dh;
            uint8_t len[320];
            if (type == 1) {
                for (int i = 0; i < 144; i++) len[i] = 8;
                for (int i = 144; i < 256; i++) len[i] = 9;
                for (int i = 256; i < 280; i++) len[i] = 7;
                for (int i = 280; i < 288; i++) len[i] = 8;
                hbuild(&lh, len, 288);
                for (int i = 0; i < 30; i++) len[i] = 5;
                hbuild(&dh, len, 30);
            } else {
                int hl = (int)getbits(&b, 5) + 257, hd = (int)getbits(&b, 5) + 1, hc = (int)getbits(&b, 4) + 4;
                static const uint8_t ord[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                uint8_t cl[19] = { 0 };
                for (int i = 0; i < hc; i++) cl[ord[i]] = (uint8_t)getbits(&b, 3);
                huff ch; hbuild(&ch, cl, 19);
                int n = 0;
                while (n < hl + hd) {
                    int s = hdecode(&b, &ch);
                    if (s < 0) return 0;
                    if (s < 16) len[n++] = (uint8_t)s;
                    else {
                        int r = s == 16 ? 3 + (int)getbits(&b, 2) : s == 17 ? 3 + (int)getbits(&b, 3) : 11 + (int)getbits(&b, 7);
                        uint8_t v = s == 16 && n ? len[n - 1] : 0;
                        while (r-- && n < hl + hd) len[n++] = v;
                    }
                }
                hbuild(&lh, len, hl);
                hbuild(&dh, len + hl, hd);
            }
            for (;;) {
                int s = hdecode(&b, &lh);
                if (s < 0) return 0;
                if (s < 256) { if (o >= cap) return 0; out[o++] = (uint8_t)s; }
                else if (s == 256) break;
                else {
                    s -= 257;
                    if (s >= 29) return 0;
                    size_t l = lbase[s] + getbits(&b, lext[s]);
                    int ds = hdecode(&b, &dh);
                    if (ds < 0 || ds >= 30) return 0;
                    size_t d = dbase[ds] + getbits(&b, dext[ds]);
                    if (d > o || o + l > cap) return 0;
                    for (size_t k = 0; k < l; k++, o++) out[o] = out[o - d];
                }
            }
        } else return 0;
    } while (!last);
    return o;
}
static int read_png(const char* path, img* o) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    if (n < 8 || memcmp(d, "\x89PNG", 4)) { free(d); return 0; }
    size_t pos = 8, zn = 0, zc = 0; uint8_t* z = NULL; int w = 0, h = 0, ct = 0;
    while (pos + 8 <= (size_t)n) {
        uint32_t len = (uint32_t)d[pos] << 24 | d[pos + 1] << 16 | d[pos + 2] << 8 | d[pos + 3];
        const uint8_t* t = d + pos + 4;
        if (!memcmp(t, "IHDR", 4)) { w = t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7]; h = t[8] << 24 | t[9] << 16 | t[10] << 8 | t[11]; ct = t[13]; if (t[12] != 8) { free(d); return 0; } }
        else if (!memcmp(t, "IDAT", 4)) { if (zn + len > zc) { zc = (zn + len) * 2; z = realloc(z, zc); } memcpy(z + zn, t + 4, len); zn += len; }
        pos += 12 + len;
    }
    free(d);
    int bpp = ct == 6 ? 4 : ct == 2 ? 3 : 0;
    if (!bpp || !w || !h || !z) { free(z); return 0; }
    size_t stride = (size_t)w * bpp, rn = (stride + 1) * h;
    uint8_t* raw = malloc(rn);
    if (inflate_z(z, zn, raw, rn) != rn) { free(z); free(raw); return 0; }
    free(z);
    o->w = w; o->h = h; o->px = malloc((size_t)w * h * 4);
    uint8_t* prev = calloc(stride, 1);
    for (int y = 0; y < h; y++) {
        uint8_t* r = raw + y * (stride + 1);
        int ft = r[0];
        uint8_t* c = r + 1;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? c[i - bpp] : 0, b = prev[i], cc = i >= (size_t)bpp ? prev[i - bpp] : 0, p;
            switch (ft) {
            case 1: c[i] += a; break;
            case 2: c[i] += b; break;
            case 3: c[i] += (a + b) >> 1; break;
            case 4: { int pp = a + b - cc, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - cc); p = pa <= pb && pa <= pc ? a : pb <= pc ? b : cc; c[i] += p; } break;
            default: break;
            }
        }
        memcpy(prev, c, stride);
        uint8_t* dd = o->px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { dd[4 * x] = c[bpp * x + 2]; dd[4 * x + 1] = c[bpp * x + 1]; dd[4 * x + 2] = c[bpp * x]; dd[4 * x + 3] = 255; }
    }
    free(prev); free(raw);
    return 1;
}

static int load(const char* path, img* o) {
    size_t n = strlen(path);
    return n > 4 && !_stricmp(path + n - 4, ".bmp") ? read_bmp(path, o) : read_png(path, o);
}

/* box-scale src into dst (BGRA) at (dx, dy, dw, dh) */
static void blit(const img* s, uint8_t* dst, int DW, int dx, int dy, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy0 = (int)((int64_t)y * s->h / dh), sy1 = (int)((int64_t)(y + 1) * s->h / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = (int)((int64_t)x * s->w / dw), sx1 = (int)((int64_t)(x + 1) * s->w / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t acc[3] = { 0, 0, 0 }, c = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    const uint8_t* p = s->px + ((size_t)yy * s->w + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; c++;
                }
            uint8_t* d = dst + ((size_t)(dy + y) * DW + dx + x) * 4;
            d[0] = (uint8_t)(acc[0] / c); d[1] = (uint8_t)(acc[1] / c); d[2] = (uint8_t)(acc[2] / c); d[3] = 255;
        }
    }
}

static int cmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static char** list(const char* dir, int* n) {
    char** v = NULL; int cap = 0; *n = 0;
    const char* pats[2] = { "frame_*.bmp", "frame_*.png" };
    for (int k = 0; k < 2; k++) {
        char pat[1024];
        snprintf(pat, sizeof pat, "%s\\%s", dir, pats[k]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (*n == cap) { cap = cap ? cap * 2 : 1024; v = realloc(v, cap * sizeof *v); }
            v[*n] = malloc(strlen(dir) + strlen(fd.cFileName) + 2);
            sprintf(v[*n], "%s\\%s", dir, fd.cFileName);
            (*n)++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (*n) qsort(v, *n, sizeof *v, cmp);
    return v;
}

/* (reimagine2) text over the frame (GDI on a DIB section of the frame's size): --title (every frame, top left) and
 * --label <file> (lines "<frame number> <text>": the text of the frame whose name has that number, e.g. its GPU time) */
typedef struct { long num; char text[120]; } label;
static label* labels;
static int nlabels;
static void load_labels(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char* e = NULL;
        long v = strtol(line, &e, 10);
        if (e == line) continue;
        while (*e == ' ' || *e == '\t') e++;
        size_t l = strlen(e);
        while (l && (e[l - 1] == '\n' || e[l - 1] == '\r')) e[--l] = 0;
        if (nlabels == cap) { cap = cap ? cap * 2 : 1024; labels = realloc(labels, cap * sizeof *labels); }
        labels[nlabels].num = v;
        snprintf(labels[nlabels].text, sizeof labels[nlabels].text, "%s", e);
        nlabels++;
    }
    fclose(f);
}
static const char* label_of(const char* path) {
    const char* u = strrchr(path, '_');
    if (!u || !nlabels) return NULL;
    long v = atol(u + 1);
    const char* best = NULL;
    for (int i = 0; i < nlabels; i++) if (labels[i].num <= v) best = labels[i].text;   /* (the latest at or before it) */
    return best;
}
static void draw_text(uint8_t* frame, int W, int H, int x, int y, const char* t1, const char* t2) {
    if ((!t1 || !*t1) && (!t2 || !*t2)) return;
    static HDC dc;
    static HBITMAP bm;
    static void* bits;
    static HFONT font;
    static int bw, bh;
    if (!dc || bw != W || bh != H) {
        if (dc) { DeleteObject(bm); DeleteDC(dc); }
        dc = CreateCompatibleDC(NULL);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        SelectObject(dc, bm);
        font = CreateFontA(H / 30, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Consolas");
        SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        bw = W; bh = H;
    }
    memcpy(bits, frame, (size_t)W * H * 4);
    int lh = H / 28;
    for (int k = 0; k < 2; k++) {
        const char* t = k ? t2 : t1;
        if (!t || !*t) continue;
        int yy = y + (t1 && *t1 && k ? lh : 0);
        SetTextColor(dc, RGB(0, 0, 0));
        TextOutA(dc, x + 2, yy + 2, t, (int)strlen(t));
        SetTextColor(dc, RGB(255, 235, 80));
        TextOutA(dc, x, yy, t, (int)strlen(t));
    }
    GdiFlush();
    memcpy(frame, bits, (size_t)W * H * 4);
}

#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { fprintf(stderr, "%s failed: 0x%08lX\n", #x, (unsigned long)hr_); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: frames_to_mp4 <out.mp4> <dir> [<dir2>] [--fps 60] [--every 2] [--mbps 40] [--width W --height H] [--first N] [--count N]\n"); return 2; }
    const char* out = argv[1]; const char* d1 = argv[2]; const char* d2 = NULL;
    int fps = 60, every = 1, W = 0, H = 0, first = 0, count = 0; double mbps = 40.0;
    const char* title = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mbps") && i + 1 < argc) mbps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--first") && i + 1 < argc) first = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) load_labels(argv[++i]);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (argv[i][0] != '-' && !d2) d2 = argv[i];
    }
    if (every < 1) every = 1;
    int n1 = 0, n2 = 0;
    char** f1 = list(d1, &n1);
    char** f2 = d2 ? list(d2, &n2) : NULL;
    if (!n1 || (d2 && !n2)) { fprintf(stderr, "no frames\n"); return 1; }
    int nf = d2 && n2 < n1 ? n2 : n1;
    img a;
    if (!load(f1[first < nf ? first : 0], &a)) { fprintf(stderr, "cannot read %s\n", f1[0]); return 1; }
    if (!W || !H) { W = a.w; H = d2 ? a.h / 2 : a.h; }
    W &= ~1; H &= ~1;
    free(a.px);
    CK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    CK(MFStartup(MF_VERSION, 0));
    IMFAttributes* at = NULL;
    CK(MFCreateAttributes(&at, 1));
    CK(IMFAttributes_SetUINT32(at, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    IMFSinkWriter* sw = NULL;
    wchar_t wout[1024];
    MultiByteToWideChar(CP_UTF8, 0, out, -1, wout, 1024);
    DeleteFileW(wout);
    CK(MFCreateSinkWriterFromURL(wout, NULL, at, &sw));
    IMFMediaType *mo = NULL, *mi = NULL;
    CK(MFCreateMediaType(&mo));
    CK(IMFMediaType_SetGUID(mo, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
    CK(IMFMediaType_SetGUID(mo, &MF_MT_SUBTYPE, &MFVideoFormat_H264));
    CK(IMFMediaType_SetUINT32(mo, &MF_MT_AVG_BITRATE, (UINT32)(mbps * 1e6)));
    CK(IMFMediaType_SetUINT32(mo, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CK(IMFMediaType_SetUINT32(mo, &MF_MT_MPEG2_PROFILE, 100 /* eAVEncH264VProfile_High */));
    CK(IMFMediaType_SetUINT64(mo, &MF_MT_FRAME_SIZE, (UINT64)W << 32 | (UINT32)H));
    CK(IMFMediaType_SetUINT64(mo, &MF_MT_FRAME_RATE, (UINT64)fps << 32 | 1));
    CK(IMFMediaType_SetUINT64(mo, &MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1));
    DWORD si = 0;
    CK(IMFSinkWriter_AddStream(sw, mo, &si));
    CK(MFCreateMediaType(&mi));
    CK(IMFMediaType_SetGUID(mi, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
    CK(IMFMediaType_SetGUID(mi, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32));
    CK(IMFMediaType_SetUINT32(mi, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CK(IMFMediaType_SetUINT64(mi, &MF_MT_FRAME_SIZE, (UINT64)W << 32 | (UINT32)H));
    CK(IMFMediaType_SetUINT64(mi, &MF_MT_FRAME_RATE, (UINT64)fps << 32 | 1));
    CK(IMFMediaType_SetUINT64(mi, &MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1));
    CK(IMFMediaType_SetUINT32(mi, &MF_MT_DEFAULT_STRIDE, (UINT32)(W * 4)));   /* (top-down) */
    CK(IMFSinkWriter_SetInputMediaType(sw, si, mi, NULL));
    CK(IMFSinkWriter_BeginWriting(sw));
    LONGLONG dur = 10000000LL / fps;
    uint8_t* frame = malloc((size_t)W * H * 4);
    int written = 0;
    for (int i = first; i < nf; i += every) {
        if (count && written >= count) break;
        memset(frame, 0, (size_t)W * H * 4);
        img x;
        if (!load(f1[i], &x)) { fprintf(stderr, "skip %s\n", f1[i]); continue; }
        if (!d2) blit(&x, frame, W, 0, 0, W, H);
        else {
            int hw = W / 2, hh = (int)((int64_t)hw * x.h / x.w);
            if (hh > H) hh = H;
            blit(&x, frame, W, 0, (H - hh) / 2, hw, hh);
            img y;
            if (load(f2[i], &y)) { blit(&y, frame, W, hw, (H - hh) / 2, hw, hh); free(y.px); }
        }
        free(x.px);
        draw_text(frame, W, H, W / 60, H / 40, title, label_of(f1[i]));
        IMFMediaBuffer* mb = NULL;
        CK(MFCreateMemoryBuffer((DWORD)W * H * 4, &mb));
        BYTE* p = NULL;
        CK(IMFMediaBuffer_Lock(mb, &p, NULL, NULL));
        memcpy(p, frame, (size_t)W * H * 4);
        CK(IMFMediaBuffer_Unlock(mb));
        CK(IMFMediaBuffer_SetCurrentLength(mb, (DWORD)W * H * 4));
        IMFSample* sm = NULL;
        CK(MFCreateSample(&sm));
        CK(IMFSample_AddBuffer(sm, mb));
        CK(IMFSample_SetSampleTime(sm, dur * written));
        CK(IMFSample_SetSampleDuration(sm, dur));
        CK(IMFSinkWriter_WriteSample(sw, si, sm));
        IMFSample_Release(sm); IMFMediaBuffer_Release(mb);
        written++;
    }
    CK(IMFSinkWriter_Finalize(sw));
    IMFSinkWriter_Release(sw);
    MFShutdown();
    printf("%s: %d frames, %dx%d, %d fps, %.0f Mbps\n", out, written, W, H, fps, mbps);
    return 0;
}
