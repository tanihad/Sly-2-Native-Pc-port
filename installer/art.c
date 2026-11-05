/* The launcher's pictures and the game's icon: decoding (WIC), resampling, anti-aliased shapes, the icon
 * builder and the choice owner art > disc art > our own (art.h, notes/disc-art.md). Plain C, system DLLs only. */
#include "art.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

/* (local copies: no dependency on which import library defines them) */
static const GUID CLSID_WICImagingFactory_ = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID IID_IWICImagingFactory_ = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID FMT_32PBGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

/* ---- images ---------------------------------------------------------------------------------------------- */

int art_alloc(art_img* a, int w, int h) {
    a->w = w;
    a->h = h;
    a->px = w > 0 && h > 0 ? calloc((size_t)w * (size_t)h, 4) : NULL;
    return a->px != NULL;
}

void art_free(art_img* a) {
    free(a->px);
    a->px = NULL;
    a->w = a->h = 0;
}

/* a file (path) or bytes in memory (mem, n): WIC finds the format by the content */
static int decode(const wchar_t* path, const void* mem, size_t n, art_img* out) {
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = NULL;
    IWICStream* st = NULL;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    int ok = 0;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory_, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory_, (void**)&f))) goto done;
    if (path) {
        if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    } else {
        if (n > 0x7FFFFFFF || FAILED(IWICImagingFactory_CreateStream(f, &st))) goto done;
        if (FAILED(IWICStream_InitializeFromMemory(st, (BYTE*)mem, (DWORD)n))) goto done;   /* (read only by the decoder) */
        if (FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream*)st, NULL, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    }
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv))) goto done;
    if (FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource*)fr, &FMT_32PBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                              WICBitmapPaletteTypeCustom))) goto done;
    if (FAILED(IWICFormatConverter_GetSize(cv, &w, &h)) || !w || !h || w > 16384 || h > 16384 || (uint64_t)w * h > (64u << 20)) goto done;
    if (!art_alloc(out, (int)w, (int)h)) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * h * 4, (BYTE*)out->px))) { art_free(out); goto done; }
    ok = 1;
done:
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    if (f) IWICImagingFactory_Release(f);
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

int art_load(const wchar_t* path, art_img* out) {
    memset(out, 0, sizeof *out);
    if (!path[0] || !file_exists(path)) return 0;
    int ok = decode(path, NULL, 0, out);
    if (!ok) logw(L"[art] cannot read %ls (not a picture, or no decoder for it on this PC)", path);
    return ok;
}

int art_load_resource(const wchar_t* name, art_img* out) {
    memset(out, 0, sizeof *out);
    HMODULE m = GetModuleHandleW(NULL);
    HRSRC r = FindResourceW(m, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return 0;                                /* (a build without --art: nothing embedded) */
    HGLOBAL g = LoadResource(m, r);
    const void* p = g ? LockResource(g) : NULL;
    DWORD n = SizeofResource(m, r);
    int ok = p && n && decode(NULL, p, n, out);
    if (!ok) logw(L"[art] cannot read the embedded picture %ls (no decoder for it on this PC?)", name);
    return ok;
}

/* ---- resampling: separable tent filter ------------------------------------------------------------------- */

typedef struct { int first, n; float* w; } taps;

static void make_taps(int outn, double s0, double slen, int inn, taps* t) {
    double scale = slen / outn, sup = scale > 1 ? scale : 1;
    int maxn = (int)ceil(sup * 2) + 3;
    for (int i = 0; i < outn; i++) {
        double c = s0 + (i + 0.5) * scale;
        int a = (int)floor(c - sup - 0.5), b = (int)ceil(c + sup - 0.5);
        if (b - a + 1 > maxn) b = a + maxn - 1;
        t[i].first = a;
        t[i].n = b - a + 1;
        t[i].w = calloc((size_t)t[i].n, sizeof(float));
        double sum = 0;
        for (int j = a; j <= b; j++) {
            double v = 1 - fabs((j + 0.5) - c) / sup;
            if (v < 0) v = 0;
            t[i].w[j - a] = (float)v;
            sum += v;
        }
        if (sum <= 0) { t[i].w[0] = 1; sum = 1; }
        for (int k = 0; k < t[i].n; k++) t[i].w[k] = (float)(t[i].w[k] / sum);
        (void)inn;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void art_resample(const art_img* src, double sx, double sy, double sw, double sh, art_img* dst) {
    if (!src->px || !dst->px || sw <= 0 || sh <= 0) return;
    int W = dst->w, H = dst->h;
    taps* tx = calloc((size_t)W, sizeof *tx);
    taps* ty = calloc((size_t)H, sizeof *ty);
    make_taps(W, sx, sw, src->w, tx);
    make_taps(H, sy, sh, src->h, ty);
    int r0 = ty[0].first, r1 = ty[H - 1].first + ty[H - 1].n - 1;
    int rows = r1 - r0 + 1;
    float* tmp = malloc((size_t)rows * W * 4 * sizeof(float));
    for (int r = 0; r < rows; r++) {                 /* horizontal pass over the rows the vertical one needs */
        const uint32_t* s = src->px + (size_t)clampi(r0 + r, 0, src->h - 1) * src->w;
        float* o = tmp + (size_t)r * W * 4;
        for (int x = 0; x < W; x++) {
            float b = 0, g = 0, rr = 0, a = 0;
            for (int k = 0; k < tx[x].n; k++) {
                uint32_t p = s[clampi(tx[x].first + k, 0, src->w - 1)];
                float wv = tx[x].w[k];
                b += wv * (float)(p & 255);
                g += wv * (float)(p >> 8 & 255);
                rr += wv * (float)(p >> 16 & 255);
                a += wv * (float)(p >> 24);
            }
            o[x * 4] = b; o[x * 4 + 1] = g; o[x * 4 + 2] = rr; o[x * 4 + 3] = a;
        }
    }
    for (int y = 0; y < H; y++) {
        uint32_t* d = dst->px + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            float c[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < ty[y].n; k++) {
                const float* o = tmp + ((size_t)(ty[y].first + k - r0) * W + x) * 4;
                float wv = ty[y].w[k];
                c[0] += wv * o[0]; c[1] += wv * o[1]; c[2] += wv * o[2]; c[3] += wv * o[3];
            }
            int v[4];
            for (int i = 0; i < 4; i++) v[i] = clampi((int)(c[i] + 0.5f), 0, 255);
            for (int i = 0; i < 3; i++) if (v[i] > v[3]) v[i] = v[3];      /* (premultiplied stays valid) */
            d[x] = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
        }
    }
    free(tmp);
    for (int i = 0; i < W; i++) free(tx[i].w);
    for (int i = 0; i < H; i++) free(ty[i].w);
    free(tx);
    free(ty);
}

static inline uint32_t over(uint32_t s, uint32_t d, unsigned k /* 0..256: extra opacity */) {
    unsigned sa = ((s >> 24) * k) >> 8;
    if (!sa) return d;
    unsigned inv = 255 - sa;
    unsigned b = (((s & 255) * k) >> 8) + ((d & 255) * inv + 127) / 255;
    unsigned g = (((s >> 8 & 255) * k) >> 8) + ((d >> 8 & 255) * inv + 127) / 255;
    unsigned r = (((s >> 16 & 255) * k) >> 8) + ((d >> 16 & 255) * inv + 127) / 255;
    unsigned a = sa + ((d >> 24) * inv + 127) / 255;
    if (b > 255) b = 255;
    if (g > 255) g = 255;
    if (r > 255) r = 255;
    if (a > 255) a = 255;
    return b | g << 8 | r << 16 | a << 24;
}

void art_blend(art_img* dst, const art_img* src, int x, int y, double opacity) {
    if (!dst->px || !src->px) return;
    unsigned k = (unsigned)(opacity * 256 + 0.5);
    if (k > 256) k = 256;
    for (int j = 0; j < src->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= dst->h) continue;
        for (int i = 0; i < src->w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= dst->w) continue;
            uint32_t* d = &dst->px[(size_t)yy * dst->w + xx];
            *d = over(src->px[(size_t)j * src->w + i], *d, k);
        }
    }
}

/* ---- shapes ----------------------------------------------------------------------------------------------- */

static uint32_t premul(uint32_t argb, double cov) {
    double a = (argb >> 24) / 255.0 * cov;
    unsigned A = (unsigned)(a * 255 + 0.5);
    unsigned r = (unsigned)((argb >> 16 & 255) * a + 0.5), g = (unsigned)((argb >> 8 & 255) * a + 0.5), b = (unsigned)((argb & 255) * a + 0.5);
    return b | g << 8 | r << 16 | A << 24;
}

/* signed distance from the pixel centre (px, py) to a rounded box */
static double rr_dist(double px, double py, double x, double y, double w, double h, double r) {
    double cx = x + w / 2, cy = y + h / 2;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    double qx = fabs(px - cx) - (w / 2 - r), qy = fabs(py - cy) - (h / 2 - r);
    double mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    double in = qx > qy ? qx : qy;
    return sqrt(mx * mx + my * my) + (in < 0 ? in : 0) - r;
}

static double cov_of(double d) { return d <= -0.5 ? 1 : d >= 0.5 ? 0 : 0.5 - d; }

static uint32_t mix(uint32_t a, uint32_t b, double t) {
    uint32_t o = 0;
    for (int s = 0; s < 32; s += 8) {
        double v = (a >> s & 255) * (1 - t) + (b >> s & 255) * t;
        o |= (uint32_t)(v + 0.5) << s;
    }
    return o;
}

static void rrect_fill(art_img* d, double x, double y, double w, double h, double r, uint32_t top, uint32_t bottom, double ring) {
    int x0 = (int)floor(x) - 1, x1 = (int)ceil(x + w) + 1, y0 = (int)floor(y) - 1, y1 = (int)ceil(y + h) + 1;
    for (int j = y0; j < y1; j++) {
        if (j < 0 || j >= d->h) continue;
        double t = h > 0 ? ((j + 0.5) - y) / h : 0;
        uint32_t c = mix(top, bottom, t < 0 ? 0 : t > 1 ? 1 : t);
        for (int i = x0; i < x1; i++) {
            if (i < 0 || i >= d->w) continue;
            double dist = rr_dist(i + 0.5, j + 0.5, x, y, w, h, r);
            double cv = cov_of(dist);
            if (ring > 0) cv -= cov_of(dist + ring);
            if (cv <= 0.002) continue;
            uint32_t* p = &d->px[(size_t)j * d->w + i];
            *p = over(premul(c, cv), *p, 256);
        }
    }
}

void art_rrect(art_img* d, double x, double y, double w, double h, double r, uint32_t argb) { rrect_fill(d, x, y, w, h, r, argb, argb, 0); }
void art_rrect_v(art_img* d, double x, double y, double w, double h, double r, uint32_t top, uint32_t bottom) { rrect_fill(d, x, y, w, h, r, top, bottom, 0); }
void art_rrect_ring(art_img* d, double x, double y, double w, double h, double r, double width, uint32_t argb) { rrect_fill(d, x, y, w, h, r, argb, argb, width); }

void art_line(art_img* d, double x0, double y0, double x1, double y1, double width, uint32_t argb) {
    double r = width / 2;
    int ax = (int)floor((x0 < x1 ? x0 : x1) - r) - 1, bx = (int)ceil((x0 > x1 ? x0 : x1) + r) + 1;
    int ay = (int)floor((y0 < y1 ? y0 : y1) - r) - 1, by = (int)ceil((y0 > y1 ? y0 : y1) + r) + 1;
    double dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int j = ay; j <= by; j++) {
        if (j < 0 || j >= d->h) continue;
        for (int i = ax; i <= bx; i++) {
            if (i < 0 || i >= d->w) continue;
            double px = i + 0.5 - x0, py = j + 0.5 - y0;
            double t = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            double ex = px - t * dx, ey = py - t * dy;
            double cv = cov_of(sqrt(ex * ex + ey * ey) - r);
            if (cv <= 0.002) continue;
            uint32_t* p = &d->px[(size_t)j * d->w + i];
            *p = over(premul(argb, cv), *p, 256);
        }
    }
}

void art_glow(art_img* d, double cx, double cy, double rx, double ry, uint32_t argb) {
    int x0 = (int)(cx - rx), x1 = (int)(cx + rx) + 1, y0 = (int)(cy - ry), y1 = (int)(cy + ry) + 1;
    for (int j = y0; j <= y1; j++) {
        if (j < 0 || j >= d->h) continue;
        for (int i = x0; i <= x1; i++) {
            if (i < 0 || i >= d->w) continue;
            double u = (i + 0.5 - cx) / rx, v = (j + 0.5 - cy) / ry, q = 1 - (u * u + v * v);
            if (q <= 0) continue;
            uint32_t* p = &d->px[(size_t)j * d->w + i];
            *p = over(premul(argb, q * q), *p, 256);
        }
    }
}

static void darken(uint32_t* p, double a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    unsigned k = (unsigned)((1 - a) * 256 + 0.5);
    uint32_t v = *p;
    unsigned b = ((v & 255) * k) >> 8, g = ((v >> 8 & 255) * k) >> 8, r = ((v >> 16 & 255) * k) >> 8;
    unsigned A = (unsigned)((v >> 24) * (1 - a) + 255 * a + 0.5);
    *p = b | g << 8 | r << 16 | (A > 255 ? 255 : A) << 24;
}

static double ramp(int v, int v0, int v1, double a0, double a1) {
    if (v1 == v0) return v < v0 ? a0 : a1;
    double t = ((v + 0.5) - v0) / (double)(v1 - v0);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    t = t * t * (3 - 2 * t);                           /* smoothstep: no visible band edges */
    return a0 + (a1 - a0) * t;
}

void art_shade_h(art_img* d, int x0, int x1, double a0, double a1) {
    for (int x = 0; x < d->w; x++) {
        double a = ramp(x, x0, x1, a0, a1);
        for (int y = 0; y < d->h; y++) darken(&d->px[(size_t)y * d->w + x], a);
    }
}

void art_shade_v(art_img* d, int y0, int y1, double a0, double a1) {
    for (int y = 0; y < d->h; y++) {
        double a = ramp(y, y0, y1, a0, a1);
        for (int x = 0; x < d->w; x++) darken(&d->px[(size_t)y * d->w + x], a);
    }
}

/* ---- files ------------------------------------------------------------------------------------------------- */

int art_find(const wchar_t* dir, const wchar_t* base, wchar_t* out) {
    static const wchar_t* ext[] = { L"png", L"webp", L"jpg", L"jpeg", L"bmp" };
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        wchar_t n[64];
        wfmt(n, 64, L"%ls.%ls", base, ext[i]);
        wpath(out, MAXP, dir, n);
        if (file_exists(out)) return 1;
    }
    out[0] = 0;
    return 0;
}

void art_user_dir(setup_t* s, wchar_t* out) {
    wchar_t ud[MAXP];
    setup_user_data_dir(s, ud);
    wpath(out, MAXP, ud, L"art");
}

/* ---- the logo's raccoon mask ------------------------------------------------------------------------------ */

/* The official "Sly 2: Band of Thieves" logo (2000 x 1187 or the same picture at another size): the outline of
 * the Cooper mask between "SLY" and "2" (the mask, its white muzzle and nose), traced on that picture. */
#define LOGO_W 2000.0
#define LOGO_H 1187.0
static const double MASK_POLY[][2] = {
    { 1162.5, 513 }, { 1130, 515 }, { 1100, 521 }, { 1060, 535 }, { 981, 507 }, { 1005, 562 }, { 979, 578 }, { 1015, 602 },
    { 1075, 640 }, { 1125, 667 }, { 1162.5, 689 }, { 1200, 667 }, { 1250, 640 }, { 1300, 614 }, { 1347, 585 }, { 1321, 564 },
    { 1351, 513 }, { 1266, 536 }, { 1230, 523 }, { 1195, 515 },
};
#define MASK_N ((int)(sizeof MASK_POLY / sizeof MASK_POLY[0]))
#define MASK_X0 977.0
#define MASK_Y0 505.0
#define MASK_X1 1353.0
#define MASK_Y1 691.0

static int in_poly(double x, double y, const double (*p)[2], int n) {
    int c = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[i][1] > y) != (p[j][1] > y) && x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0]) c = !c;
    return c;
}

int art_logo_mask(const art_img* logo, art_img* out, int width) {
    memset(out, 0, sizeof *out);
    if (!logo->px || width < 4) return 0;
    double aspect = (double)logo->w / logo->h;
    if (fabs(aspect / (LOGO_W / LOGO_H) - 1) > 0.02 || logo->w < 400) return 0;
    double k = logo->w / LOGO_W;
    int height = (int)ceil(width * (MASK_Y1 - MASK_Y0) / (MASK_X1 - MASK_X0));
    if (!art_alloc(out, width, height)) return 0;
    art_resample(logo, MASK_X0 * k, MASK_Y0 * k, (MASK_X1 - MASK_X0) * k, (MASK_Y1 - MASK_Y0) * k, out);
    /* everything outside the outline goes (4 x 4 samples per pixel) */
    double sx = (MASK_X1 - MASK_X0) / width, sy = (MASK_Y1 - MASK_Y0) / height;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            int in = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    in += in_poly(MASK_X0 + (x + (i + 0.5) / 4) * sx, MASK_Y0 + (y + (j + 0.5) / 4) * sy, MASK_POLY, MASK_N);
            uint32_t* p = &out->px[(size_t)y * width + x];
            if (in < 16) {
                unsigned kk = (unsigned)(in * 16);   /* in/16 * 256 */
                uint32_t v = *p;
                *p = (((v & 255) * kk) >> 8) | ((((v >> 8) & 255) * kk) >> 8) << 8 | ((((v >> 16) & 255) * kk) >> 8) << 16 |
                     ((((v >> 24) & 255) * kk) >> 8) << 24;
            }
        }
    return 1;
}

/* ---- the icon ----------------------------------------------------------------------------------------------- */

static const int ICON_SIZES[] = { 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 };
#define ICON_N ((int)(sizeof ICON_SIZES / sizeof ICON_SIZES[0]))

/* one size: kind 0 the picture fitted (contain), kind 1 the logo's mask on a round Sly-blue badge */
static int icon_image(const art_img* src, int kind, int n, art_img* out) {
    if (!art_alloc(out, n, n)) return 0;
    if (kind == 0) {
        double s = (double)n / (src->w > src->h ? src->w : src->h);
        int w = (int)(src->w * s + 0.5), h = (int)(src->h * s + 0.5);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        art_img t;
        if (!art_alloc(&t, w, h)) return 0;
        art_resample(src, 0, 0, src->w, src->h, &t);
        art_blend(out, &t, (n - w) / 2, (n - h) / 2, 1);
        art_free(&t);
        return 1;
    }
    double e = n <= 24 ? 0.6 : n * 0.022;              /* dark outer edge (reads on light backgrounds) */
    double ring = n <= 24 ? 1.0 : n * 0.05;            /* the logo's white sticker ring */
    art_rrect(out, 0, 0, n, n, n / 2.0, 0xF0060B1E);
    art_rrect(out, e, e, n - 2 * e, n - 2 * e, n / 2.0, 0xFFF4F6FF);
    double b = e + ring;
    art_rrect_v(out, b, b, n - 2 * b, n - 2 * b, n / 2.0, 0xFF3C78EE, 0xFF0B1D58);
    int mw = (int)(n * (n <= 24 ? 0.94 : n <= 48 ? 0.86 : 0.80) + 0.5);   /* (small: as large as the badge allows) */
    art_img m;
    if (!art_logo_mask(src, &m, mw)) return 0;
    art_blend(out, &m, (n - m.w) / 2, (int)(n * 0.53 - m.h / 2.0 + 0.5), 1);
    art_free(&m);
    return 1;
}

/* ---- PNG (for the 256 px icon image: what Windows itself uses at that size; zlib "stored" blocks, no codec) -- */

static uint32_t crc_tab[256];
static uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t c) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_tab[i] = v;
        }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* straight-alpha RGBA PNG of a premultiplied image; malloc'd, *n = size */
static uint8_t* png_of(const art_img* im, size_t* n) {
    size_t row = (size_t)im->w * 4 + 1, raw = row * im->h;
    size_t blocks = (raw + 65534) / 65535, z = 2 + raw + blocks * 5 + 4;
    size_t total = 8 + 25 + 12 + z + 12;
    uint8_t* f = malloc(total);
    uint8_t* r = malloc(raw);
    if (!f || !r) { free(f); free(r); return NULL; }
    for (int y = 0; y < im->h; y++) {
        uint8_t* o = r + row * y;
        *o++ = 0;
        for (int x = 0; x < im->w; x++) {
            uint32_t v = im->px[(size_t)y * im->w + x];
            unsigned a = v >> 24, b = v & 255, g = v >> 8 & 255, rr = v >> 16 & 255;
            if (a && a < 255) { b = b * 255 / a; g = g * 255 / a; rr = rr * 255 / a; }
            *o++ = (uint8_t)(rr > 255 ? 255 : rr);
            *o++ = (uint8_t)(g > 255 ? 255 : g);
            *o++ = (uint8_t)(b > 255 ? 255 : b);
            *o++ = (uint8_t)a;
        }
    }
    uint8_t* p = f;
    memcpy(p, "\x89PNG\r\n\x1a\n", 8);
    p += 8;
    be32(p, 13);
    memcpy(p + 4, "IHDR", 4);
    be32(p + 8, (uint32_t)im->w);
    be32(p + 12, (uint32_t)im->h);
    p[16] = 8; p[17] = 6; p[18] = 0; p[19] = 0; p[20] = 0;
    be32(p + 21, crc32_of(p + 4, 17, 0));
    p += 25;
    be32(p, (uint32_t)z);
    memcpy(p + 4, "IDAT", 4);
    uint8_t* d = p + 8;
    *d++ = 0x78; *d++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw; off += 65535) {
        size_t len = raw - off > 65535 ? 65535 : raw - off;
        *d++ = off + len >= raw;
        d[0] = (uint8_t)len; d[1] = (uint8_t)(len >> 8); d[2] = (uint8_t)~len; d[3] = (uint8_t)(~len >> 8);
        d += 4;
        memcpy(d, r + off, len);
        for (size_t i = 0; i < len; i++) { s1 = (s1 + r[off + i]) % 65521; s2 = (s2 + s1) % 65521; }
        d += len;
    }
    be32(d, s2 << 16 | s1);
    d += 4;
    be32(d, crc32_of(p + 4, z + 4, 0));
    p = d + 4;
    be32(p, 0);
    memcpy(p + 4, "IEND", 4);
    be32(p + 8, crc32_of(p + 4, 4, 0));
    free(r);
    *n = total;
    return f;
}

/* a .ico: 32-bit DIB images up to 128 px (every icon API reads those), PNG at 256 px (as Windows' own icons) */
static int write_ico(const wchar_t* path, art_img* imgs, int count) {
    uint8_t* pngs[32] = { 0 };
    size_t png_n[32] = { 0 };
    size_t total = 6 + 16 * (size_t)count;
    for (int i = 0; i < count && i < 32; i++) {
        int n = imgs[i].w;
        if (n >= 256 && (pngs[i] = png_of(&imgs[i], &png_n[i]))) { total += png_n[i]; continue; }
        total += 40 + (size_t)n * n * 4 + (size_t)((n + 31) / 32 * 4) * n;
    }
    uint8_t* f = calloc(1, total);
    if (!f) return 0;
    uint16_t hd[3] = { 0, 1, (uint16_t)count };
    memcpy(f, hd, 6);
    size_t off = 6 + 16 * (size_t)count;
    for (int i = 0; i < count; i++) {
        int n = imgs[i].w;
        size_t mask_row = (size_t)((n + 31) / 32 * 4), bytes = pngs[i] ? png_n[i] : 40 + (size_t)n * n * 4 + mask_row * n;
        uint8_t* e = f + 6 + 16 * i;
        e[0] = (uint8_t)(n >= 256 ? 0 : n);
        e[1] = (uint8_t)(n >= 256 ? 0 : n);
        uint16_t planes = 1, bpp = 32;
        uint32_t sz = (uint32_t)bytes, o = (uint32_t)off;
        memcpy(e + 4, &planes, 2);
        memcpy(e + 6, &bpp, 2);
        memcpy(e + 8, &sz, 4);
        memcpy(e + 12, &o, 4);
        if (pngs[i]) {
            memcpy(f + off, pngs[i], png_n[i]);
            free(pngs[i]);
            off += bytes;
            continue;
        }
        BITMAPINFOHEADER bi;
        memset(&bi, 0, sizeof bi);
        bi.biSize = 40;
        bi.biWidth = n;
        bi.biHeight = n * 2;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biSizeImage = (DWORD)(n * n * 4 + mask_row * n);
        memcpy(f + off, &bi, 40);
        uint8_t* px = f + off + 40;
        uint8_t* mk = px + (size_t)n * n * 4;
        for (int y = 0; y < n; y++) {
            const uint32_t* row = imgs[i].px + (size_t)(n - 1 - y) * n;   /* bottom-up */
            for (int x = 0; x < n; x++) {
                uint32_t v = row[x];
                unsigned a = v >> 24, b = v & 255, g = v >> 8 & 255, r = v >> 16 & 255;
                if (a && a < 255) { b = b * 255 / a; g = g * 255 / a; r = r * 255 / a; }   /* straight alpha */
                uint8_t* q = px + ((size_t)y * n + x) * 4;
                q[0] = (uint8_t)(b > 255 ? 255 : b);
                q[1] = (uint8_t)(g > 255 ? 255 : g);
                q[2] = (uint8_t)(r > 255 ? 255 : r);
                q[3] = (uint8_t)a;
                if (!a) mk[(size_t)y * mask_row + x / 8] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
        off += bytes;
    }
    int ok = write_file(path, f, total);             /* (via <path>.tmp: never half a file) */
    free(f);
    return ok;
}

int art_write_icon(const art_img* src, int kind, const wchar_t* path) {
    art_img imgs[ICON_N];
    memset(imgs, 0, sizeof imgs);
    int ok = 1;
    for (int i = 0; i < ICON_N && ok; i++) ok = icon_image(src, kind, ICON_SIZES[i], &imgs[i]);
    if (ok) ok = write_ico(path, imgs, ICON_N);
    for (int i = 0; i < ICON_N; i++) art_free(&imgs[i]);
    return ok;
}

HICON art_make_hicon(const art_img* src, int kind, int n) {
    art_img im;
    memset(&im, 0, sizeof im);
    if (n < 1 || n > 512 || !icon_image(src, kind, n, &im)) { art_free(&im); return NULL; }
    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = n;
    bi.bV5Height = -n;                                 /* top-down, as art_img */
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    HICON h = NULL;
    uint8_t* zero = calloc((size_t)((n + 15) / 16 * 2) * n, 1);   /* the AND mask: all 0 (alpha decides) */
    HBITMAP mask = zero ? CreateBitmap(n, n, 1, 1, zero) : NULL;
    if (color && bits && mask) {
        uint32_t* d = bits;
        for (size_t i = 0; i < (size_t)n * n; i++) {    /* icons take straight alpha */
            uint32_t v = im.px[i];
            unsigned a = v >> 24, b = v & 255, g = v >> 8 & 255, r = v >> 16 & 255;
            if (a && a < 255) { b = b * 255 / a; g = g * 255 / a; r = r * 255 / a; }
            d[i] = (b > 255 ? 255 : b) | (g > 255 ? 255 : g) << 8 | (r > 255 ? 255 : r) << 16 | a << 24;
        }
        GdiFlush();
        ICONINFO ii = { TRUE, 0, 0, mask, color };
        h = CreateIconIndirect(&ii);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    free(zero);
    art_free(&im);
    return h;
}

/* ---- the choice ------------------------------------------------------------------------------------------- */

static void signature(const wchar_t* path, const wchar_t* kind, wchar_t* out, size_t cap) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    memset(&a, 0, sizeof a);
    GetFileAttributesExW(path, GetFileExInfoStandard, &a);
    wfmt(out, cap, L"v1 %ls %lu %lu %lu %ls", kind, a.nFileSizeLow, a.ftLastWriteTime.dwLowDateTime, a.ftLastWriteTime.dwHighDateTime, path);
}

void art_icon_location(setup_t* s, wchar_t* out) {
    wpath(out, MAXP, s->dir, L"art\\sly2.ico");
    if (!file_exists(out)) wpath(out, MAXP, s->dir, LAUNCHER_EXE);
}

int art_icon_refresh(setup_t* s, const wchar_t** what) {
    static const wchar_t* none = L"";
    if (what) *what = none;
    wchar_t ud[MAXP], ad[MAXP], ico[MAXP], sigp[MAXP], cand[4][MAXP], sig[MAXP + 128];
    art_user_dir(s, ud);
    wpath(ad, MAXP, s->dir, L"art");
    wpath(ico, MAXP, ad, L"sly2.ico");
    wpath(sigp, MAXP, ad, L"sly2.ico.source");
    /* the candidates, best first: 0 icon.ico (as it is), 1 icon.<picture>, 2 logo.<picture> (its mask), 3 the disc's */
    wpath(cand[0], MAXP, ud, L"icon.ico");
    if (!file_exists(cand[0])) cand[0][0] = 0;
    art_find(ud, L"icon", cand[1]);
    art_find(ud, L"logo", cand[2]);
    wpath(cand[3], MAXP, s->dir, GAME_DIR L"\\art\\sly2.ico");
    if (!file_exists(cand[3])) cand[3][0] = 0;
    static const wchar_t* kinds[] = { L"owner-ico", L"owner-picture", L"owner-logo", L"disc" };
    char* old = NULL;
    size_t on = 0;
    read_file(sigp, &old, &on);
    for (int i = 0; i < 4; i++) {
        if (!cand[i][0]) continue;
        signature(cand[i], kinds[i], sig, MAXP + 128);
        char* sig8 = to_utf8(sig);
        int same = old && file_exists(ico) && on == strlen(sig8) && !memcmp(old, sig8, on);
        if (same) { free(sig8); free(old); if (what) *what = i < 3 ? L"owner" : L"disc"; return 0; }
        mkdir_p(ad);
        int ok = 0;
        if (i == 0 || i == 3) ok = CopyFileW(cand[i], ico, FALSE);
        else {
            art_img src;
            if (art_load(cand[i], &src)) {
                ok = art_write_icon(&src, i == 2 ? 1 : 0, ico);
                if (!ok && i == 2) logw(L"[art] %ls is not the logo whose mask makes the icon: put an icon.png next to it", cand[i]);
                art_free(&src);
            }
        }
        if (ok) {
            write_file(sigp, sig8, strlen(sig8));
            free(sig8);
            free(old);
            logw(L"[art] game icon from %ls (%ls)", cand[i], kinds[i]);
            if (what) *what = i < 3 ? L"owner" : L"disc";
            return 1;
        }
        free(sig8);
    }
    free(old);
    if (file_exists(ico)) {                         /* no source any more: back to the launcher's own icon */
        DeleteFileW(ico);
        DeleteFileW(sigp);
        RemoveDirectoryW(ad);
        logw(L"[art] game icon: the launcher's own");
        return 1;
    }
    return 0;
}
