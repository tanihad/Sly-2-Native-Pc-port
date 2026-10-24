/* The game's own font for the PC menus (sly_font.h). */
#include "sly_font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static uint32_t rd(const uint8_t* ram, uint32_t size, uint32_t a, int* ok) {
    if (a > size - 4 || (a & 3)) { *ok = 0; return 0; }
    uint32_t v;
    memcpy(&v, ram + a, 4);
    return v;
}

static int ptr_ok(uint32_t p, uint32_t size) { return p >= 0x00100000u && p < size && !(p & 3); }

/* ---- finding the font ------------------------------------------------------------------------------------------ */

static int font_at(const uint8_t* ram, uint32_t size, uint32_t a) {
    int ok = 1;
    uint32_t n = rd(ram, size, a + 0x20, &ok);
    if (!ok || n < 20 || n > 256) return 0;
    uint32_t t = rd(ram, size, a + 0x24, &ok), g = rd(ram, size, a + 0x14, &ok), adv = rd(ram, size, a + 0x1C, &ok);
    if (!ok || !ptr_ok(t, size) || !ptr_ok(g, size) || !ptr_ok(adv, size)) return 0;
    if (t + 4 * n > size || g + 0x40 * (n + 1) > size || adv + 4 * n > size) return 0;
    int has_a = 0, has_0 = 0, has_lower = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = rd(ram, size, t + 4 * i, &ok);
        if (c > 255 || (i && c <= prev)) return 0;
        prev = c;
        has_a |= c == 'A';
        has_0 |= c == '0';
        has_lower |= c == 'a';
    }
    if (!has_a || !has_0 || !has_lower) return 0;
    for (uint32_t i = 0; i < n; i++) {           /* advances: sane floats */
        float f;
        memcpy(&f, ram + adv + 4 * i, 4);
        if (!(f >= 0.0f && f < 500.0f)) return 0;
    }
    return 1;
}

int slyf_find(const uint8_t* ram, uint32_t size, uint32_t* addr) {
    if (!ram || size < 0x200000u) return 0;
    for (uint32_t a = 0x00100000u; a + 0x40 <= size; a += 4) {
        uint32_t n;
        memcpy(&n, ram + a + 0x20, 4);
        if (n < 20 || n > 256) continue;
        if (font_at(ram, size, a)) { *addr = a; return 1; }
    }
    return 0;
}

/* ---- the glyphs' VIF packets -> triangles ----------------------------------------------------------------------
 * A glyph (+0x1C) is a DMA chain (CNT ... RET) of VIF codes for the 2D VU1 program (MSCAL 40): per batch
 *   STROW 0; UNPACK V2-32 @7 = the segment count; STCYCL 2,1; UNPACK V3-8 @8 = per segment (kind, vertices, flag);
 *   UNPACK V4-8 @9 = per segment colour; STCYCL 1,1; STMASK 0x50505050; UNPACK V2-16 (masked) = int16 x, y per
 *   vertex, one unpack per segment; optional STMASK 0x3F3F3F3F + UNPACK V1-8 (masked) at a segment's address = its
 *   vertices' w (non-zero: ADC, the vertex draws no triangle); MSCAL.
 * The VU program draws kind 1 as a triangle fan, kind 2 as a strip (other kinds: lines, not filled). The game adds each
 * triangle's +-2 to a stencil channel and covers the nonzero pixels; reproduced here (nonzero rule): fan triangles count
 * with the sign of their orientation, normalised so that the segment adds (flag 0) or takes away (flag 1: a counter,
 * colour alpha 0); strip triangles are an explicit triangulation: +1, or -1 for a counter (checked against the game's
 * pictures: the holes of O, A, Q, g, @, p). */

typedef struct { int kind, n, flag; } seg_info;

int slyf_extract(const uint8_t* ram, uint32_t size, uint32_t font, slyf_mesh* m) {
    int ok = 1;
    if (!m || !m->tri || !font_at(ram, size, font)) return 0;
    uint32_t n = rd(ram, size, font + 0x20, &ok), t = rd(ram, size, font + 0x24, &ok);
    uint32_t garr = rd(ram, size, font + 0x14, &ok), adv = rd(ram, size, font + 0x1C, &ok);
    m->ntri = 0;
    memset(m->adv, 0, sizeof m->adv);
    memset(m->has, 0, sizeof m->has);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ch = rd(ram, size, t + 4 * i, &ok);
        float av;
        memcpy(&av, ram + adv + 4 * i, 4);
        m->adv[ch] = av;
        m->has[ch] = 1;
        uint32_t g = garr + 0x40 * (i + 1);
        uint32_t chain = rd(ram, size, g + 0x1C, &ok);
        if (!ok) return 0;
        if (!chain) continue;                    /* (no picture: a space) */
        if (!ptr_ok(chain, size)) return 0;
        /* one batch at a time */
        seg_info info[64];
        int ninfo = 0;
        int16_t vx[600], vy[600];
        uint8_t vw[600];
        uint16_t vaddr[64];
        int vstart[64], vcount[64], nun = 0, nv = 0;
        uint32_t a = chain;
        int qwords = 0, done = 0;
        while (!done) {
            uint32_t tag = rd(ram, size, a, &ok);
            if (!ok) return 0;
            uint32_t qwc = tag & 0xFFFF, id = tag >> 28 & 7;
            if (id != 1 && id != 6 && id != 7) return 0;           /* CNT, RET, END */
            if ((qwords += (int)qwc + 1) > 4096) return 0;
            uint32_t end = a + 16 + 16 * qwc, p = a + 8;
            if (end > size) return 0;
            while (p + 4 <= end) {
                uint32_t code = rd(ram, size, p, &ok);
                p += 4;
                uint32_t cmd = code >> 24 & 0x7F, num = code >> 16 & 0xFF, imm = code & 0xFFFF;
                if (cmd >= 0x60) {
                    uint32_t vn = cmd >> 2 & 3, vl = cmd & 3, cnt = num ? num : 256;
                    static const uint32_t vlb[4] = { 4, 2, 1, 2 };
                    uint32_t nb = cnt * (vn + 1) * vlb[vl], nw = (nb + 3) / 4;
                    if (p + 4 * nw > end) return 0;
                    const uint8_t* d = ram + p;
                    p += 4 * nw;
                    uint32_t addr = imm & 0x3FF;
                    if (vn == 2 && vl == 2) {                       /* V3-8: the segments */
                        if (cnt > 64) return 0;
                        ninfo = (int)cnt;
                        for (int k = 0; k < ninfo; k++) info[k] = (seg_info){ d[3 * k], d[3 * k + 1], d[3 * k + 2] };
                    } else if (vn == 1 && vl == 1) {                /* V2-16: vertices */
                        if (nun >= 64 || nv + (int)cnt > 600) return 0;
                        vaddr[nun] = (uint16_t)addr;
                        vstart[nun] = nv;
                        vcount[nun++] = (int)cnt;
                        for (uint32_t k = 0; k < cnt; k++) {
                            int16_t x, y;
                            memcpy(&x, d + 4 * k, 2);
                            memcpy(&y, d + 4 * k + 2, 2);
                            vx[nv] = x; vy[nv] = y; vw[nv++] = 0;
                        }
                    } else if (vn == 0 && vl == 2) {                /* V1-8: ADC flags of a segment's vertices */
                        for (int u = 0; u < nun; u++)
                            if (vaddr[u] == addr)
                                for (uint32_t k = 0; k < cnt && (int)k < vcount[u]; k++) vw[vstart[u] + k] = d[k];
                    }
                } else if (cmd == 0x30 || cmd == 0x31) {
                    p += 16;
                } else if (cmd == 0x20) {
                    p += 4;
                } else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) { /* MSCAL / MSCALF / MSCNT: draw the batch */
                    int v = 0;
                    for (int s = 0; s < ninfo; s++) {
                        int cntv = info[s].n;
                        if (v + cntv > nv) return 0;
                        if (info[s].kind == 1 || info[s].kind == 2) {
                            /* the segment's net orientation: a flag-0 segment adds, a flag-1 one (a counter) takes away */
                            float net = 0;
                            for (int k = 2; k < cntv; k++) {
                                int i0 = info[s].kind == 1 ? v : v + k - 2, i1 = v + k - 1, i2 = v + k;
                                if (vw[i2]) continue;
                                net += (float)(vx[i1] - vx[i0]) * (float)(vy[i2] - vy[i0]) - (float)(vy[i1] - vy[i0]) * (float)(vx[i2] - vx[i0]);
                            }
                            int sgn = (net >= 0 ? 1 : -1) * (info[s].flag ? -1 : 1);
                            for (int k = 2; k < cntv; k++) {
                                int i0 = info[s].kind == 1 ? v : v + k - 2, i1 = v + k - 1, i2 = v + k;
                                if (vw[i2]) continue;               /* ADC: no triangle */
                                if (m->ntri >= SLYF_MAX_TRIS) return 0;
                                slyf_tri* tr = &m->tri[m->ntri];
                                int ids[3] = { i0, i1, i2 };
                                for (int j = 0; j < 3; j++) { tr->x[j] = vx[ids[j]] / 16.0f; tr->y[j] = vy[ids[j]] / 16.0f; }
                                float ar = (tr->x[1] - tr->x[0]) * (tr->y[2] - tr->y[0]) - (tr->y[1] - tr->y[0]) * (tr->x[2] - tr->x[0]);
                                if (ar == 0.0f) continue;
                                /* fan: the outline's winding (signed by orientation); strip: an explicit triangulation */
                                tr->w = (int8_t)(info[s].kind == 2 ? (info[s].flag ? -1 : 1) : (ar > 0 ? 1 : -1) * sgn);
                                tr->ch = (uint8_t)ch;
                                m->ntri++;
                            }
                        }
                        v += cntv;
                    }
                    ninfo = nun = nv = 0;
                } else if (cmd == 0x00 || cmd == 0x01 || cmd == 0x04 || cmd == 0x05 || cmd == 0x10 || cmd == 0x11 ||
                           cmd == 0x13) {
                    /* NOP, STCYCL, ITOP, STMOD, FLUSHE, FLUSH, FLUSHA */
                } else {
                    return 0;
                }
            }
            if (id != 1) done = 1;
            a = end;
        }
    }
    return m->ntri > 0 && m->has['A'] && m->adv['A'] > 0.0f;
}

/* ---- atlas ----------------------------------------------------------------------------------------------------- */

#define PAD 12                                   /* pixels around each glyph (mipmaps: 2^levels) */

static int atlas_alloc(slyf_atlas* a, int w, int h) {
    a->w[0] = w;
    a->h[0] = h;
    a->px[0] = calloc((size_t)w * h, 1);
    return a->px[0] != NULL;
}

static void atlas_mips(slyf_atlas* a) {
    int k = 1;
    for (; k < SLYF_LEVELS; k++) {
        int pw = a->w[k - 1], ph = a->h[k - 1], w = (pw + 1) / 2, h = (ph + 1) / 2;
        if (pw <= 1 && ph <= 1) break;
        uint8_t* d = malloc((size_t)w * h);
        if (!d) break;
        const uint8_t* s = a->px[k - 1];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int x0 = 2 * x, y0 = 2 * y, x1 = x0 + 1 < pw ? x0 + 1 : x0, y1 = y0 + 1 < ph ? y0 + 1 : y0;
                d[(size_t)y * w + x] = (uint8_t)((s[(size_t)y0 * pw + x0] + s[(size_t)y0 * pw + x1] + s[(size_t)y1 * pw + x0] +
                                                  s[(size_t)y1 * pw + x1] + 2) / 4);
            }
        a->px[k] = d;
        a->w[k] = w;
        a->h[k] = h;
    }
    a->levels = k;
}

void slyf_free(slyf_atlas* a) {
    for (int k = 0; k < SLYF_LEVELS; k++) free(a->px[k]);
    memset(a, 0, sizeof *a);
}

/* shelf packing of nboxes rectangles (w, h with PAD) into a width-W atlas; returns the height */
static int pack(int W, const int* bw, const int* bh, int n, int* bx, int* by) {
    int x = 0, y = 0, row = 0;
    for (int i = 0; i < n; i++) {
        if (!bw[i]) { bx[i] = by[i] = 0; continue; }
        if (x + bw[i] > W) { x = 0; y += row; row = 0; }
        bx[i] = x;
        by[i] = y;
        x += bw[i];
        if (bh[i] > row) row = bh[i];
    }
    return y + row;
}

int slyf_raster(const slyf_mesh* m, float units_px, slyf_atlas* out) {
    memset(out, 0, sizeof *out);
    out->from_game = 1;
    out->units_px = units_px;
    float bx0[256], by0[256], bx1[256], by1[256];
    for (int c = 0; c < 256; c++) { bx0[c] = by0[c] = 1e9f; bx1[c] = by1[c] = -1e9f; }
    float asc = 0, desc = 0;
    for (int i = 0; i < m->ntri; i++) {
        const slyf_tri* t = &m->tri[i];
        for (int j = 0; j < 3; j++) {
            if (t->x[j] < bx0[t->ch]) bx0[t->ch] = t->x[j];
            if (t->x[j] > bx1[t->ch]) bx1[t->ch] = t->x[j];
            if (t->y[j] < by0[t->ch]) by0[t->ch] = t->y[j];
            if (t->y[j] > by1[t->ch]) by1[t->ch] = t->y[j];
        }
    }
    int bw[256], bh[256], px0[256], py0[256];
    for (int c = 0; c < 256; c++) {
        out->g[c].adv = m->adv[c];
        bw[c] = bh[c] = 0;
        if (!m->has[c] || bx1[c] < bx0[c]) continue;
        if (c >= 'A' && c <= 'Z' && -by0[c] > asc) asc = -by0[c];
        if ((c == 'g' || c == 'p' || c == 'q' || c == 'y') && by1[c] > desc) desc = by1[c];
        px0[c] = (int)floorf(bx0[c] * units_px);
        py0[c] = (int)floorf(by0[c] * units_px);
        bw[c] = (int)ceilf(bx1[c] * units_px) - px0[c] + 1 + 2 * PAD;
        bh[c] = (int)ceilf(by1[c] * units_px) - py0[c] + 1 + 2 * PAD;
    }
    out->ascent = asc > 0 ? asc : 46.0f;
    out->descent = desc > 0 ? desc : 13.0f;
    int ax[256], ay[256];
    const int W = 2048;
    int H = pack(W, bw, bh, 256, ax, ay);
    if (H <= 0 || !atlas_alloc(out, W, H)) return 0;
    /* per glyph: a winding count per sample (4 x 4 per pixel), then the coverage */
    for (int c = 0; c < 256; c++) {
        if (!bw[c]) continue;
        slyf_glyph* g = &out->g[c];
        g->ax = (int16_t)ax[c]; g->ay = (int16_t)ay[c]; g->aw = (int16_t)bw[c]; g->ah = (int16_t)bh[c];
        g->ox = (float)(px0[c] - PAD) / units_px;
        g->oy = (float)(py0[c] - PAD) / units_px;
        g->present = 1;
        int sw = bw[c] * 4, sh = bh[c] * 4;
        int16_t* acc = calloc((size_t)sw * sh, sizeof *acc);
        if (!acc) { slyf_free(out); return 0; }
        float ox = (float)(px0[c] - PAD), oy = (float)(py0[c] - PAD);
        for (int i = 0; i < m->ntri; i++) {
            const slyf_tri* t = &m->tri[i];
            if (t->ch != c) continue;
            float X[3], Y[3];                    /* in samples */
            for (int j = 0; j < 3; j++) { X[j] = (t->x[j] * units_px - ox) * 4.0f; Y[j] = (t->y[j] * units_px - oy) * 4.0f; }
            float ar = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
            if (ar == 0.0f) continue;
            if (ar < 0) { float tx = X[1], ty = Y[1]; X[1] = X[2]; Y[1] = Y[2]; X[2] = tx; Y[2] = ty; ar = -ar; }
            int x0 = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), x1 = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
            int y0 = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), y1 = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > sw) x1 = sw;
            if (y1 > sh) y1 = sh;
            for (int sy = y0; sy < y1; sy++) {
                float py = sy + 0.5f;
                for (int sx = x0; sx < x1; sx++) {
                    float pxf = sx + 0.5f;
                    float e0 = (X[1] - X[0]) * (py - Y[0]) - (Y[1] - Y[0]) * (pxf - X[0]);
                    float e1 = (X[2] - X[1]) * (py - Y[1]) - (Y[2] - Y[1]) * (pxf - X[1]);
                    float e2 = (X[0] - X[2]) * (py - Y[2]) - (Y[0] - Y[2]) * (pxf - X[2]);
                    if (e0 >= 0 && e1 >= 0 && e2 >= 0) acc[(size_t)sy * sw + sx] += t->w;
                }
            }
        }
        for (int y = 0; y < bh[c]; y++)
            for (int x = 0; x < bw[c]; x++) {
                int n = 0;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++) n += acc[(size_t)(4 * y + j) * sw + 4 * x + i] != 0;
                out->px[0][(size_t)(ay[c] + y) * W + ax[c] + x] = (uint8_t)((n * 255 + 8) / 16);
            }
        free(acc);
    }
    atlas_mips(out);
    return 1;
}

/* ---- the Windows stand-in -------------------------------------------------------------------------------------- */

int slyf_system(float units_px, slyf_atlas* out) {
    memset(out, 0, sizeof *out);
    out->units_px = units_px;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    int em = (int)(SLYF_EM * units_px + 0.5f);
    HFONT f = CreateFontW(-(int)(em * 0.78f), 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    out->ascent = (float)(tm.tmAscent - tm.tmInternalLeading) / units_px;
    out->descent = (float)tm.tmDescent / units_px;
    static const MAT2 ident = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    uint8_t* bmp[256] = { 0 };
    int bw[256], bh[256], gox[256], goy[256];
    for (int c = 0; c < 256; c++) {
        bw[c] = bh[c] = 0;
        if (c < 0x20 || (c >= 0x7F && c < 0xA0)) continue;
        wchar_t wc;
        char cc = (char)c;
        if (MultiByteToWideChar(1252, 0, &cc, 1, &wc, 1) != 1) continue;
        GLYPHMETRICS gm;
        DWORD need = GetGlyphOutlineW(dc, wc, GGO_GRAY8_BITMAP, &gm, 0, NULL, &ident);
        if (need == GDI_ERROR) continue;
        out->g[c].adv = (float)gm.gmCellIncX / units_px;
        if (!need || c == ' ') continue;
        bmp[c] = malloc(need);
        if (!bmp[c] || GetGlyphOutlineW(dc, wc, GGO_GRAY8_BITMAP, &gm, need, bmp[c], &ident) == GDI_ERROR) {
            free(bmp[c]); bmp[c] = NULL; continue;
        }
        gox[c] = gm.gmptGlyphOrigin.x;
        goy[c] = gm.gmptGlyphOrigin.y;
        bw[c] = (int)gm.gmBlackBoxX + 2 * PAD;
        bh[c] = (int)gm.gmBlackBoxY + 2 * PAD;
    }
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    int ax[256], ay[256];
    const int W = 2048;
    int H = pack(W, bw, bh, 256, ax, ay);
    int okk = H > 0 && atlas_alloc(out, W, H);
    for (int c = 0; c < 256; c++) {
        if (!bmp[c]) continue;
        if (okk) {
            slyf_glyph* g = &out->g[c];
            int gw = bw[c] - 2 * PAD, gh = bh[c] - 2 * PAD, pitch = (gw + 3) & ~3;
            g->ax = (int16_t)ax[c]; g->ay = (int16_t)ay[c]; g->aw = (int16_t)bw[c]; g->ah = (int16_t)bh[c];
            g->ox = (float)(gox[c] - PAD) / units_px;
            g->oy = (float)(-goy[c] - PAD) / units_px;
            g->present = 1;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    int v = bmp[c][(size_t)y * pitch + x];   /* 0..64 */
                    out->px[0][(size_t)(ay[c] + PAD + y) * W + ax[c] + PAD + x] = (uint8_t)(v >= 64 ? 255 : v * 4);
                }
        }
        free(bmp[c]);
    }
    if (!okk) { slyf_free(out); return 0; }
    atlas_mips(out);
    return 1;
}

/* ---- the current font ------------------------------------------------------------------------------------------ */

#define UNITS_PX 2.5f                            /* a 60-unit em is 150 pixels in the atlas: crisp up to 4K */
static slyf_atlas* volatile cur;
static volatile LONG cur_serial = 1;
static SRWLOCK cur_lock = SRWLOCK_INIT;

void slyf_publish(slyf_atlas* a) {
    AcquireSRWLockExclusive(&cur_lock);
    cur = a;                                     /* (the previous one stays allocated: a renderer may still read it) */
    InterlockedIncrement(&cur_serial);
    ReleaseSRWLockExclusive(&cur_lock);
}

const slyf_atlas* slyf_current(void) {
    slyf_atlas* a = cur;
    if (a) return a;
    AcquireSRWLockExclusive(&cur_lock);
    if (!cur) {
        slyf_atlas* s = calloc(1, sizeof *s);
        if (s && slyf_system(UNITS_PX, s)) { cur = s; InterlockedIncrement(&cur_serial); }
        else free(s);
    }
    a = cur;
    ReleaseSRWLockExclusive(&cur_lock);
    return a;
}

uint32_t slyf_serial(void) { return (uint32_t)cur_serial; }

static volatile LONG game_state;                 /* 0 looking, 1 filling the atlas, 2 done */

static DWORD WINAPI raster_thread(LPVOID arg) {
    slyf_mesh* m = arg;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    slyf_atlas* a = calloc(1, sizeof *a);
    if (a && slyf_raster(m, UNITS_PX, a)) {
        QueryPerformanceCounter(&t1);
        fprintf(stderr, "[menu] the game's font: %d triangles -> atlas %dx%d (%d levels) in %.0f ms\n", m->ntri, a->w[0], a->h[0],
                a->levels, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
        slyf_publish(a);
        InterlockedExchange(&game_state, 2);
    } else {
        free(a);
        fprintf(stderr, "[menu] the game's font could not be drawn (out of memory)\n");
        InterlockedExchange(&game_state, 0);
    }
    free(m->tri);
    free(m);
    return 0;
}

void slyf_ee_poll(const uint8_t* ram, uint32_t size) {
    static unsigned frames;
    static int failed_logged;
    if (game_state || (frames++ % 60) != 0) return;
    const char* e = getenv("PS2_MENU_FONT");                   /* (PS2_MENU_FONT=system: the stand-in, for comparisons) */
    if (e && !_stricmp(e, "system")) { game_state = 2; return; }
    uint32_t addr = 0;
    if (!slyf_find(ram, size, &addr)) return;
    slyf_mesh* m = calloc(1, sizeof *m);
    if (m) m->tri = malloc(sizeof *m->tri * SLYF_MAX_TRIS);
    if (!m || !m->tri || !slyf_extract(ram, size, addr, m)) {
        if (!failed_logged) { failed_logged = 1; fprintf(stderr, "[menu] a font at 0x%08X does not parse: the menu keeps its stand-in\n", addr); }
        if (m) free(m->tri);
        free(m);
        return;
    }
    fprintf(stderr, "[menu] the game's font found at 0x%08X\n", addr);
    game_state = 1;
    HANDLE t = CreateThread(NULL, 0, raster_thread, m, 0, NULL);
    if (t) CloseHandle(t);
    else raster_thread(m);
}

/* ---- text ------------------------------------------------------------------------------------------------------ */

int slyf_cp1252(const char* s, char* out, int cap) {
    int n = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && n < cap - 1) {
        unsigned c = *p;
        if (c < 0x80) { out[n++] = (char)c; p++; continue; }
        unsigned cp = 0, len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        cp = len == 1 ? c : c & (0x7F >> len);
        for (unsigned k = 1; k < len; k++) {
            if ((p[k] & 0xC0) != 0x80) { len = k; break; }
            cp = cp << 6 | (p[k] & 0x3F);
        }
        p += len;
        char o = '?';
        if (cp >= 0xA0 && cp <= 0xFF) o = (char)cp;
        else {
            wchar_t w = (wchar_t)cp;
            char b;
            BOOL bad = FALSE;
            if (cp < 0x10000 && WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, &w, 1, &b, 1, NULL, &bad) == 1 && !bad) o = b;
            else if (cp == 0x2190) o = '<';
            else if (cp == 0x2192) o = '>';
            else if (cp == 0x2191 || cp == 0x2193) o = '|';
        }
        out[n++] = o;
    }
    out[n] = 0;
    return n;
}

float slyf_text_width(const slyf_atlas* a, const char* s, float px) {
    if (!a || !s) return 0;
    char t[1024];
    int n = slyf_cp1252(s, t, sizeof t);
    float w = 0;
    for (int i = 0; i < n; i++) w += a->g[(uint8_t)t[i]].adv;
    return w * px / SLYF_EM;
}
