/* The Options menu's picture (menu_draw.h): drawn on the CPU into a premultiplied BGRA bitmap at the output's
 * size - anti-aliased shapes from signed distances, text with a Windows font through GDI (grayscale anti-aliased,
 * drawn white on black into a scratch bitmap and used as coverage) - so every renderer shows exactly the same
 * crisp menu at any resolution, DPI and GPU (menu_overlay.c puts it on the screen). Redrawn only when the menu
 * changes. */
#include "menu_draw.h"
#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef struct { uint32_t* px; int w, h, stride; } canvas;

/* ---- colours (premultiplied on use): Sly 2's night-blue and gold ---------------------------------------------- */
#define RGBA(r, g, b, a) ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
static const uint32_t C_DIM = RGBA(2, 4, 14, 150);
static const uint32_t C_PANEL_TOP = RGBA(16, 30, 66, 244), C_PANEL_BOT = RGBA(7, 13, 34, 248);
static const uint32_t C_GOLD = RGBA(246, 196, 66, 255), C_GOLD_DIM = RGBA(176, 136, 46, 255);
static const uint32_t C_TEXT = RGBA(232, 238, 250, 255), C_MUTED = RGBA(140, 160, 200, 255), C_DISABLED = RGBA(92, 106, 140, 255);
static const uint32_t C_SEL = RGBA(36, 72, 148, 235), C_HOVER = RGBA(28, 50, 102, 200), C_ROWLINE = RGBA(40, 58, 100, 120);
static const uint32_t C_DESC = RGBA(4, 10, 28, 200), C_TRACK = RGBA(40, 56, 98, 255), C_KEY = RGBA(24, 40, 82, 255);
static const uint32_t C_NOTE = RGBA(250, 206, 96, 255);

static inline uint32_t premul(uint32_t c, float cov) {
    float a = (float)(c >> 24) / 255.0f * cov;
    if (a <= 0.0f) return 0;
    if (a > 1.0f) a = 1.0f;
    uint32_t r = (uint32_t)(((c >> 16) & 255) * a + 0.5f), g = (uint32_t)(((c >> 8) & 255) * a + 0.5f),
             b = (uint32_t)((c & 255) * a + 0.5f), A = (uint32_t)(a * 255.0f + 0.5f);
    return A << 24 | r << 16 | g << 8 | b;
}

static inline void blend(uint32_t* d, uint32_t s) {          /* premultiplied src over dst */
    uint32_t sa = s >> 24;
    if (!sa) return;
    if (sa == 255) { *d = s; return; }
    uint32_t inv = 255 - sa, x = *d;
    uint32_t a = sa + (((x >> 24) * inv + 127) / 255), r = ((s >> 16) & 255) + ((((x >> 16) & 255) * inv + 127) / 255),
             g = ((s >> 8) & 255) + ((((x >> 8) & 255) * inv + 127) / 255), b = (s & 255) + (((x & 255) * inv + 127) / 255);
    *d = (a > 255 ? 255 : a) << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static uint32_t lerp_c(uint32_t a, uint32_t b, float t) {
    uint32_t o = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 255) + ((float)((b >> s) & 255) - (float)((a >> s) & 255)) * t;
        o |= (uint32_t)(x + 0.5f) << s;
    }
    return o;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* rounded rectangle, filled (border = 0) or a ring of width border; colour top -> bottom gradient */
static void rrect(canvas* c, float x, float y, float w, float h, float r, float border, uint32_t top, uint32_t bot) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
    for (int py = y0; py < y1; py++) {
        uint32_t col = top == bot ? top : lerp_c(top, bot, clampf(((float)py + 0.5f - y) / h, 0, 1));
        uint32_t* row = c->px + (size_t)py * c->stride;
        float qy = fabsf((float)py + 0.5f - cy) - hy;
        for (int px = x0; px < x1; px++) {
            float qx = fabsf((float)px + 0.5f - cx) - hx;
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
            float cov = clampf(0.5f - d, 0, 1);
            if (border > 0) cov *= clampf(d + border + 0.5f, 0, 1);
            if (cov > 0) blend(&row[px], premul(col, cov));
        }
    }
}

static void fill(canvas* c, float x, float y, float w, float h, uint32_t col) { rrect(c, x, y, w, h, 0, 0, col, col); }

/* a line segment of width wd (round caps) */
static void seg(canvas* c, float ax, float ay, float bx, float by, float wd, uint32_t col) {
    int x0 = (int)floorf(fminf(ax, bx) - wd), x1 = (int)ceilf(fmaxf(ax, bx) + wd);
    int y0 = (int)floorf(fminf(ay, by) - wd), y1 = (int)ceilf(fmaxf(ay, by) + wd);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float qx = (float)px + 0.5f - ax, qy = (float)py + 0.5f - ay;
            float t = len2 > 0 ? clampf((qx * dx + qy * dy) / len2, 0, 1) : 0;
            float ex = qx - t * dx, ey = qy - t * dy;
            float cov = clampf(wd / 2 - sqrtf(ex * ex + ey * ey) + 0.5f, 0, 1);
            if (cov > 0) blend(&c->px[(size_t)py * c->stride + px], premul(col, cov));
        }
}

/* filled triangle (anti-aliased edges) */
static void tri(canvas* c, float ax, float ay, float bx, float by, float cx, float cy, uint32_t col) {
    int x0 = (int)floorf(fminf(ax, fminf(bx, cx))) - 1, x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx))) + 1;
    int y0 = (int)floorf(fminf(ay, fminf(by, cy))) - 1, y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy))) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    float sgn = area < 0 ? -1.0f : 1.0f;
    float P[3][2] = { { ax, ay }, { bx, by }, { cx, cy } };
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float x = (float)px + 0.5f, y = (float)py + 0.5f, m = 1e9f;
            for (int e = 0; e < 3; e++) {
                float ex = P[(e + 1) % 3][0] - P[e][0], ey = P[(e + 1) % 3][1] - P[e][1];
                float len = sqrtf(ex * ex + ey * ey);
                if (len <= 0) continue;
                float d = sgn * (ex * (y - P[e][1]) - ey * (x - P[e][0])) / len;
                if (d < m) m = d;
            }
            float cov = clampf(m + 0.5f, 0, 1);
            if (cov > 0) blend(&c->px[(size_t)py * c->stride + px], premul(col, cov));
        }
}

/* ring (circle outline) or disc (wd <= 0) */
static void circle(canvas* c, float cx, float cy, float rad, float wd, uint32_t col) {
    int x0 = (int)floorf(cx - rad - 1), x1 = (int)ceilf(cx + rad + 1), y0 = (int)floorf(cy - rad - 1), y1 = (int)ceilf(cy + rad + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx, dy = (float)py + 0.5f - cy, d = sqrtf(dx * dx + dy * dy) - rad;
            float cov = clampf(0.5f - d, 0, 1);
            if (wd > 0) cov *= clampf(d + wd + 0.5f, 0, 1);
            if (cov > 0) blend(&c->px[(size_t)py * c->stride + px], premul(col, cov));
        }
}

/* ---- text (GDI) ------------------------------------------------------------------------------------------------ */

static HDC tdc;
static HBITMAP tbmp;
static uint32_t* tbits;
static int tbw, tbh;
static struct { int px, weight, italic, face; HFONT f; } fonts[24];
static int nfonts;

enum { FACE_UI, FACE_TITLE };

static HFONT font(int px, int weight, int italic, int face) {
    for (int k = 0; k < nfonts; k++)
        if (fonts[k].px == px && fonts[k].weight == weight && fonts[k].italic == italic && fonts[k].face == face) return fonts[k].f;
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, (DWORD)italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face == FACE_TITLE ? L"Segoe UI Black" : L"Segoe UI");
    if (nfonts == (int)(sizeof fonts / sizeof fonts[0])) {   /* full: drop them all (sizes changed with the window) */
        for (int k = 0; k < nfonts; k++) DeleteObject(fonts[k].f);
        nfonts = 0;
    }
    fonts[nfonts].px = px; fonts[nfonts].weight = weight; fonts[nfonts].italic = italic; fonts[nfonts].face = face;
    fonts[nfonts++].f = f;
    return f;
}

static int text_setup(int w, int h) {
    if (!tdc) tdc = CreateCompatibleDC(NULL);
    if (!tdc) return 0;
    if (tbmp && tbw >= w && tbh >= h) return 1;
    if (tbmp) DeleteObject(tbmp);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    tbmp = CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!tbmp) { tbw = tbh = 0; return 0; }
    tbits = bits;
    tbw = w;
    tbh = h;
    SelectObject(tdc, tbmp);
    SetBkMode(tdc, TRANSPARENT);
    SetTextColor(tdc, RGB(255, 255, 255));
    return 1;
}

static int utf16(const char* s, wchar_t* w, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    if (n <= 0) { w[0] = 0; return 0; }
    return n - 1;
}

enum { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2, AL_WRAP = 4, AL_TOP = 8 };

static int text_w(const char* s, int px, int weight, int face) {
    if (!text_setup(16, 16)) return 0;
    wchar_t w[512];
    int n = utf16(s, w, 512);
    SelectObject(tdc, font(px, weight, 0, face));
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(tdc, w, n, &sz);
    return sz.cx;
}

/* text in a box; returns the width drawn (single line) */
static int text(canvas* c, int x, int y, int w, int h, const char* s, int px, int weight, int italic, int face, uint32_t col, int align) {
    if (w <= 0 || h <= 0 || !s || !*s) return 0;
    if (!text_setup(c->w, c->h)) return 0;
    RECT r = { x, y, x + w, y + h };
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > c->w) r.right = c->w;
    if (r.bottom > c->h) r.bottom = c->h;
    if (r.right <= r.left || r.bottom <= r.top) return 0;
    for (int py = r.top; py < r.bottom; py++) memset(tbits + (size_t)py * tbw + r.left, 0, (size_t)(r.right - r.left) * 4);
    GdiFlush();
    wchar_t ws[1024];
    int n = utf16(s, ws, 1024);
    SelectObject(tdc, font(px, weight, italic, face));
    UINT fmt = DT_NOPREFIX | ((align & 3) == AL_CENTER ? DT_CENTER : (align & 3) == AL_RIGHT ? DT_RIGHT : DT_LEFT);
    if (align & AL_WRAP) fmt |= DT_WORDBREAK;
    else fmt |= DT_SINGLELINE | DT_END_ELLIPSIS | ((align & AL_TOP) ? DT_TOP : DT_VCENTER);
    RECT dr = { x, y, x + w, y + h };
    DrawTextW(tdc, ws, n, &dr, fmt);
    GdiFlush();
    for (int py = r.top; py < r.bottom; py++) {
        const uint32_t* src = tbits + (size_t)py * tbw;
        uint32_t* dst = c->px + (size_t)py * c->stride;
        for (int px2 = r.left; px2 < r.right; px2++) {
            uint32_t v = src[px2];
            if (!v) continue;
            uint32_t g = ((v >> 16 & 255) + (v >> 8 & 255) + (v & 255)) / 3;   /* (grayscale: all three equal) */
            if (g) blend(&dst[px2], premul(col, (float)g / 255.0f));
        }
    }
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(tdc, ws, n, &sz);
    return sz.cx < w ? sz.cx : w;
}

/* ---- button glyphs ----------------------------------------------------------------------------------------- */

enum { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_L1, G_R1, G_START, G_DPAD };

/* a controller button glyph in a d x d box at (x, y) */
static void pad_glyph(canvas* c, float x, float y, float d, int g, int xbox) {
    float cx = x + d / 2, cy = y + d / 2, r = d / 2;
    if (g == G_L1 || g == G_R1 || g == G_START) {
        float w = g == G_START ? d * 1.5f : d * 1.25f;
        rrect(c, x, y + d * 0.12f, w, d * 0.76f, d * 0.22f, 0, RGBA(230, 236, 248, 255), RGBA(200, 210, 230, 255));
        const char* t = g == G_L1 ? (xbox ? "LB" : "L1") : g == G_R1 ? (xbox ? "RB" : "R1") : (xbox ? "MENU" : "OPTIONS");
        text(c, (int)x, (int)(y + d * 0.12f), (int)w, (int)(d * 0.76f), t, (int)(d * (g == G_START ? 0.30f : 0.46f)), FW_BOLD, 0,
             FACE_UI, RGBA(12, 20, 40, 255), AL_CENTER);
        return;
    }
    if (g == G_DPAD) {
        float a = d * 0.30f;
        rrect(c, cx - a / 2, y + d * 0.08f, a, d * 0.84f, a * 0.25f, 0, C_TEXT, C_TEXT);
        rrect(c, x + d * 0.08f, cy - a / 2, d * 0.84f, a, a * 0.25f, 0, C_TEXT, C_TEXT);
        return;
    }
    circle(c, cx, cy, r, 0, RGBA(230, 236, 248, 255));
    uint32_t ink = RGBA(18, 26, 48, 255);
    if (xbox) {
        static const char* letters[] = { "A", "B", "X", "Y" };
        static const uint32_t cols[] = { RGBA(40, 150, 60, 255), RGBA(200, 40, 40, 255), RGBA(40, 90, 200, 255), RGBA(200, 150, 20, 255) };
        circle(c, cx, cy, r * 0.86f, 0, cols[g]);
        text(c, (int)x, (int)y, (int)d, (int)d, letters[g], (int)(d * 0.62f), FW_BOLD, 0, FACE_UI, RGBA(255, 255, 255, 255), AL_CENTER);
        return;
    }
    float s = d * 0.26f, wd = d * 0.10f;
    switch (g) {
    case G_CROSS: seg(c, cx - s, cy - s, cx + s, cy + s, wd, ink); seg(c, cx - s, cy + s, cx + s, cy - s, wd, ink); break;
    case G_CIRCLE: circle(c, cx, cy, s * 1.08f, wd, ink); break;
    case G_SQUARE: rrect(c, cx - s, cy - s, 2 * s, 2 * s, wd * 0.3f, wd, ink, ink); break;
    case G_TRIANGLE: {
        float t = s * 1.15f, ty = cy + t * 0.12f;
        seg(c, cx, ty - t, cx - t, ty + t * 0.72f, wd, ink);
        seg(c, cx - t, ty + t * 0.72f, cx + t, ty + t * 0.72f, wd, ink);
        seg(c, cx + t, ty + t * 0.72f, cx, ty - t, wd, ink);
        break;
    }
    default: break;
    }
}

/* a keyboard key cap with its name; returns its width */
static int key_cap(canvas* c, int x, int y, int h, const char* name, float u, int draw) {
    int px = (int)(h * 0.50f), tw = text_w(name, px, FW_SEMIBOLD, FACE_UI), w = tw + (int)(h * 0.6f);
    if (w < h) w = h;
    if (draw) {
        rrect(c, (float)x, (float)y, (float)w, (float)h, h * 0.2f, 0, RGBA(230, 236, 248, 255), RGBA(196, 206, 228, 255));
        text(c, x, y, w, h, name, px, FW_SEMIBOLD, 0, FACE_UI, RGBA(12, 20, 40, 255), AL_CENTER);
    }
    (void)u;
    return w;
}

/* hint: glyph(s) + label, right-aligned in r (dev: MENU_DEV_*); kb = key name, g = pad glyph */
static void hint(canvas* c, menu_rect r, int dev, const char* kb, int g, const char* label, float u, int hover) {
    int gh = (int)(r.h * 0.74f), px = (int)(24 * u);
    int lw = text_w(label, px, FW_SEMIBOLD, FACE_UI);
    int gw = dev == MENU_DEV_KEYBOARD ? key_cap(c, 0, 0, gh, kb, u, 0) : (g == G_START ? (int)(gh * 1.5f) : g == G_L1 || g == G_R1 ? (int)(gh * 1.25f) : gh);
    int total = gw + (int)(10 * u) + lw, x = r.x + r.w - total, y = r.y + (r.h - gh) / 2;
    if (hover) rrect(c, (float)(x - 10 * u), (float)r.y, (float)(total + 20 * u), (float)r.h, r.h * 0.3f, 0, C_HOVER, C_HOVER);
    if (dev == MENU_DEV_KEYBOARD) key_cap(c, x, y, gh, kb, u, 1);
    else pad_glyph(c, (float)x, (float)y, (float)gh, g, dev == MENU_DEV_XBOX);
    text(c, x + gw + (int)(10 * u), r.y, lw + 4, r.h, label, px, FW_SEMIBOLD, 0, FACE_UI, C_TEXT, AL_LEFT);
}

/* ---- the menu ------------------------------------------------------------------------------------------------- */

static void draw_row(canvas* c, const menu_view* v, const menu_layout* L, int k, float u) {
    int i = v->scroll + k;
    const menu_row* row = &v->rows[i];
    menu_rect r = L->rows[k];
    int sel = i == v->sel;
    if (sel) {
        rrect(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, 10 * u, 0, C_SEL, lerp_c(C_SEL, C_PANEL_BOT, 0.35f));
        rrect(c, (float)r.x, (float)r.y + 6 * u, 6 * u, (float)r.h - 12 * u, 3 * u, 0, C_GOLD, C_GOLD);
    } else {
        fill(c, (float)r.x + 12 * u, (float)(r.y + r.h), (float)r.w - 24 * u, fmaxf(1.0f, u), C_ROWLINE);
    }
    uint32_t lc = row->enabled ? (sel ? RGBA(255, 255, 255, 255) : C_TEXT) : C_DISABLED;
    int px = (int)(27 * u);
    text(c, r.x + (int)(26 * u), r.y, L->value_x - r.x - (int)(30 * u), r.h, row->label, px, sel ? FW_SEMIBOLD : FW_NORMAL, 0,
         FACE_UI, lc, AL_LEFT);
    menu_rect lft = L->left[k], rgt = L->right[k];
    int vx0 = lft.x + lft.w, vx1 = rgt.x;
    uint32_t vc = row->enabled ? RGBA(255, 255, 255, 255) : C_DISABLED;
    float ay = (float)r.y + r.h / 2.0f, as = 9 * u;
    switch (row->kind) {
    case MENU_ITEM_CHOICE:
        if (row->can_left) tri(c, lft.x + lft.w / 2.0f - as * 0.6f, ay, lft.x + lft.w / 2.0f + as * 0.6f, ay - as, lft.x + lft.w / 2.0f + as * 0.6f, ay + as,
                               sel ? C_GOLD : C_GOLD_DIM);
        if (row->can_right) tri(c, rgt.x + rgt.w / 2.0f + as * 0.6f, ay, rgt.x + rgt.w / 2.0f - as * 0.6f, ay + as, rgt.x + rgt.w / 2.0f - as * 0.6f, ay - as,
                                sel ? C_GOLD : C_GOLD_DIM);
        text(c, vx0, r.y, vx1 - vx0, r.h, row->value, px, sel ? FW_SEMIBOLD : FW_NORMAL, 0, FACE_UI, vc, AL_CENTER);
        break;
    case MENU_ITEM_SLIDER: {
        if (row->can_left) tri(c, lft.x + lft.w / 2.0f - as * 0.6f, ay, lft.x + lft.w / 2.0f + as * 0.6f, ay - as, lft.x + lft.w / 2.0f + as * 0.6f, ay + as,
                               sel ? C_GOLD : C_GOLD_DIM);
        if (row->can_right) tri(c, rgt.x + rgt.w / 2.0f + as * 0.6f, ay, rgt.x + rgt.w / 2.0f - as * 0.6f, ay + as, rgt.x + rgt.w / 2.0f - as * 0.6f, ay - as,
                                sel ? C_GOLD : C_GOLD_DIM);
        int nw = (int)(84 * u), tx0 = vx0 + (int)(8 * u), tx1 = vx1 - nw - (int)(8 * u);
        float th = 8 * u, frac = row->hi > row->lo ? (float)(row->val - row->lo) / (float)(row->hi - row->lo) : 0;
        rrect(c, (float)tx0, ay - th / 2, (float)(tx1 - tx0), th, th / 2, 0, C_TRACK, C_TRACK);
        if (frac > 0) rrect(c, (float)tx0, ay - th / 2, (tx1 - tx0) * frac, th, th / 2, 0, row->enabled ? C_GOLD : C_DISABLED,
                            row->enabled ? C_GOLD_DIM : C_DISABLED);
        circle(c, tx0 + (tx1 - tx0) * frac, ay, 11 * u, 0, row->enabled ? RGBA(255, 255, 255, 255) : C_DISABLED);
        text(c, tx1, r.y, nw + (int)(8 * u), r.h, row->value, px, FW_SEMIBOLD, 0, FACE_UI, vc, AL_RIGHT);
        break;
    }
    case MENU_ITEM_ACTION: {
        float cx = (float)(rgt.x + rgt.w / 2), s = 8 * u;
        seg(c, cx - s * 0.5f, ay - s, cx + s * 0.5f, ay, 3 * u, sel ? C_GOLD : C_GOLD_DIM);
        seg(c, cx + s * 0.5f, ay, cx - s * 0.5f, ay + s, 3 * u, sel ? C_GOLD : C_GOLD_DIM);
        break;
    }
    case MENU_ITEM_BIND: {
        if (v->capture && sel) {
            text(c, vx0, r.y, vx1 - vx0 + lft.w, r.h, v->page == MENU_PAGE_KEYS ? "Press a key...  (Esc: cancel)" :
                 "Press a button...  (PS button: cancel)", px, FW_SEMIBOLD, 1, FACE_UI, C_NOTE, AL_CENTER);
        } else {                                  /* two columns: the first and the second key / button */
            int kh = (int)(r.h * 0.70f), colw = (vx1 - vx0) / 2;
            for (int s = 0; s < 2; s++) {
                const char* t = s ? row->value2 : row->value;
                int cx0 = vx0 + s * colw;
                if (sel && s == v->bind_slot)
                    rrect(c, cx0 + 6 * u, r.y + 3 * u, colw - 12 * u, r.h - 6 * u, 8 * u, 2 * u, C_GOLD, C_GOLD);
                if (!strcmp(t, "-")) {
                    text(c, cx0, r.y, colw, r.h, "-", px, FW_NORMAL, 0, FACE_UI, C_DISABLED, AL_CENTER);
                    continue;
                }
                int kw = key_cap(c, 0, 0, kh, t, u, 0);
                if (kw > colw - (int)(20 * u)) kw = colw - (int)(20 * u);
                key_cap(c, cx0 + colw / 2 - kw / 2, r.y + (r.h - kh) / 2, kh, t, u, 1);
            }
        }
        break;
    }
    default: break;
    }
}

static void draw_menu(canvas* c, const menu_view* v) {
    menu_layout L;
    menu_compute_layout(c->w, c->h, v->nrows, v->scroll, &L);
    float u = L.u;
    fill(c, 0, 0, (float)c->w, (float)c->h, C_DIM);
    menu_rect P = L.panel;
    /* shadow, panel, gold frame */
    rrect(c, P.x - 2 * u, P.y + 10 * u, P.w + 4 * u, (float)P.h, 26 * u, 0, RGBA(0, 0, 0, 90), RGBA(0, 0, 0, 90));
    rrect(c, (float)P.x, (float)P.y, (float)P.w, (float)P.h, 22 * u, 0, C_PANEL_TOP, C_PANEL_BOT);
    rrect(c, (float)P.x, (float)P.y, (float)P.w, (float)P.h, 22 * u, 3 * u, C_GOLD, C_GOLD_DIM);
    rrect(c, P.x + 7 * u, P.y + 7 * u, P.w - 14 * u, P.h - 14 * u, 16 * u, fmaxf(1.0f, 1.2f * u), RGBA(246, 196, 66, 60), RGBA(246, 196, 66, 30));
    /* title */
    menu_rect H = L.header;
    char title[2 * MENU_TEXT];
    snprintf(title, sizeof title, "%s", "PC SETTINGS");
    int tw = text(c, H.x, H.y, H.w, H.h, title, (int)(50 * u), FW_BLACK, 1, FACE_TITLE, C_GOLD, AL_LEFT);
    if (v->page_title[0])
        text(c, H.x + tw + (int)(22 * u), H.y + (int)(4 * u), H.w - tw, H.h, v->page_title, (int)(30 * u), FW_SEMIBOLD, 0, FACE_UI, C_TEXT, AL_LEFT);
    text(c, H.x, H.y, H.w, H.h, "Sly 2: Band of Thieves", (int)(22 * u), FW_NORMAL, 1, FACE_UI, C_MUTED, AL_RIGHT);
    /* tabs */
    int main_page = v->page >= MENU_PAGE_N ? MENU_PAGE_CONTROLS : v->page;
    for (int k = 0; k < MENU_PAGE_N; k++) {
        menu_rect t = L.tabs[k];
        int on = k == main_page;
        if (on) rrect(c, t.x + 4 * u, (float)t.y, t.w - 8 * u, (float)t.h, 10 * u, 0, RGBA(30, 56, 116, 200), RGBA(30, 56, 116, 60));
        else if (k == v->hover_tab) rrect(c, t.x + 4 * u, (float)t.y, t.w - 8 * u, (float)t.h, 10 * u, 0, C_HOVER, RGBA(28, 50, 102, 40));
        text(c, t.x, t.y, t.w, t.h, menu_page_name(k), (int)(28 * u), on ? FW_BOLD : FW_SEMIBOLD, 0, FACE_UI, on ? RGBA(255, 255, 255, 255) : C_MUTED, AL_CENTER);
        if (on) rrect(c, t.x + 18 * u, t.y + t.h - 5 * u, t.w - 36 * u, 5 * u, 2.5f * u, 0, C_GOLD, C_GOLD);
    }
    fill(c, (float)L.list.x, (float)(L.tabs[0].y + L.tabs[0].h + 2 * u), (float)L.list.w, fmaxf(1.0f, u), RGBA(246, 196, 66, 80));
    for (int k = 0; k < 2; k++) {                 /* page switch hints */
        menu_rect t = L.tab_hint[k];
        int gh = (int)(t.h * 0.62f);
        if (v->hover_btn == 5 + k) rrect(c, (float)t.x, (float)t.y, (float)t.w, (float)t.h, 10 * u, 0, C_HOVER, C_HOVER);
        if (v->device == MENU_DEV_KEYBOARD) {
            int kw = key_cap(c, 0, 0, gh, k ? "E" : "Q", u, 0);
            key_cap(c, t.x + (t.w - kw) / 2, t.y + (t.h - gh) / 2, gh, k ? "E" : "Q", u, 1);
        } else {
            float gw = gh * 1.25f;
            pad_glyph(c, t.x + (t.w - gw) / 2, (float)(t.y + (t.h - gh) / 2), (float)gh, k ? G_R1 : G_L1, v->device == MENU_DEV_XBOX);
        }
    }
    /* rows */
    for (int k = 0; k < L.visible && v->scroll + k < v->nrows; k++)
        if (v->scroll + k != v->sel && v->scroll + k == v->hover_row)
            rrect(c, (float)L.rows[k].x, (float)L.rows[k].y, (float)L.rows[k].w, (float)L.rows[k].h, 10 * u, 0, C_HOVER, C_HOVER);
    for (int k = 0; k < L.visible && v->scroll + k < v->nrows; k++) draw_row(c, v, &L, k, u);
    if (L.scrollbar.w) {
        menu_rect s = L.scrollbar;
        rrect(c, (float)s.x, (float)s.y, (float)s.w, (float)s.h, s.w / 2.0f, 0, RGBA(30, 44, 80, 200), RGBA(30, 44, 80, 200));
        float frac = (float)L.visible / v->nrows, pos = (float)v->scroll / v->nrows;
        rrect(c, (float)s.x, s.y + s.h * pos, (float)s.w, s.h * frac, s.w / 2.0f, 0, C_GOLD_DIM, C_GOLD_DIM);
    }
    /* description */
    menu_rect D = L.desc;
    rrect(c, (float)D.x, (float)D.y, (float)D.w, (float)D.h, 12 * u, 0, C_DESC, C_DESC);
    if (v->sel >= 0 && v->sel < v->nrows) {
        const menu_row* row = &v->rows[v->sel];
        int pad = (int)(18 * u), nh = row->note[0] ? (int)(30 * u) : 0;
        text(c, D.x + pad, D.y + (int)(12 * u), D.w - 2 * pad, D.h - (int)(16 * u) - nh, row->desc, (int)(23 * u), FW_NORMAL, 0, FACE_UI,
             RGBA(200, 212, 238, 255), AL_LEFT | AL_WRAP);
        if (nh) text(c, D.x + pad, D.y + D.h - nh - (int)(8 * u), D.w - 2 * pad, nh, row->note, (int)(22 * u), FW_SEMIBOLD, 1, FACE_UI,
                     C_NOTE, AL_LEFT);
    }
    /* footer: navigation on the left, buttons on the right */
    menu_rect F = L.footer;
    int dev = v->device;
    {
        int gh = (int)(F.h * 0.74f), x = F.x, y = F.y + (F.h - gh) / 2, px = (int)(23 * u);
        if (dev == MENU_DEV_KEYBOARD) {
            x += key_cap(c, x, y, gh, "\xE2\x86\x91\xE2\x86\x93", u, 1) + (int)(8 * u);   /* the arrows */
            x += text(c, x, F.y, (int)(160 * u), F.h, "Move", px, FW_SEMIBOLD, 0, FACE_UI, C_TEXT, AL_LEFT) + (int)(22 * u);
            x += key_cap(c, x, y, gh, "\xE2\x86\x90\xE2\x86\x92", u, 1) + (int)(8 * u);
            text(c, x, F.y, (int)(160 * u), F.h, "Change", px, FW_SEMIBOLD, 0, FACE_UI, C_TEXT, AL_LEFT);
        } else {
            pad_glyph(c, (float)x, (float)y, (float)gh, G_DPAD, 0);
            x += gh + (int)(8 * u);
            x += text(c, x, F.y, (int)(260 * u), F.h, "Move / Change", px, FW_SEMIBOLD, 0, FACE_UI, C_TEXT, AL_LEFT) + (int)(22 * u);
        }
    }
    int sub = v->page >= MENU_PAGE_N;
    hint(c, L.btn[0], dev, "Esc", G_CIRCLE, sub ? "Back" : "Close", u, v->hover_btn == 0);
    if (dev != MENU_DEV_KEYBOARD) hint(c, L.btn[1], dev, "", G_START, sub ? "Close" : "Close", u, 0);
    else hint(c, L.btn[1], dev, "Enter", G_CROSS, "Select", u, 0);
    /* the reset dialog */
    if (v->confirm_reset) {
        fill(c, (float)P.x, (float)P.y, (float)P.w, (float)P.h, RGBA(0, 0, 0, 0));
        rrect(c, (float)P.x, (float)P.y, (float)P.w, (float)P.h, 22 * u, 0, RGBA(0, 0, 0, 120), RGBA(0, 0, 0, 120));
        menu_rect B = L.dlg;
        rrect(c, (float)B.x, (float)B.y, (float)B.w, (float)B.h, 18 * u, 0, C_PANEL_TOP, C_PANEL_BOT);
        rrect(c, (float)B.x, (float)B.y, (float)B.w, (float)B.h, 18 * u, 3 * u, C_GOLD, C_GOLD_DIM);
        char q[2 * MENU_TEXT];
        const char* pn = v->page == MENU_PAGE_KEYS ? "the keyboard controls" : v->page == MENU_PAGE_PAD ? "the controller buttons"
                                                                                                       : menu_page_name(v->page);
        snprintf(q, sizeof q, "Reset %s to the defaults?", pn);
        text(c, B.x + (int)(20 * u), B.y + (int)(28 * u), B.w - (int)(40 * u), (int)(70 * u), q, (int)(30 * u), FW_SEMIBOLD, 0, FACE_UI,
             C_TEXT, AL_CENTER);
        for (int k = 0; k < 2; k++) {
            menu_rect b = k ? L.dlg_no : L.dlg_yes;
            int hov = v->hover_btn == 3 + k;
            rrect(c, (float)b.x, (float)b.y, (float)b.w, (float)b.h, 12 * u, 0, hov ? C_SEL : C_KEY, hov ? C_SEL : C_KEY);
            rrect(c, (float)b.x, (float)b.y, (float)b.w, (float)b.h, 12 * u, 2 * u, k ? C_MUTED : C_GOLD, k ? C_MUTED : C_GOLD);
            int gh = (int)(b.h * 0.6f), gx = b.x + (int)(18 * u), gy = b.y + (b.h - gh) / 2;
            if (dev == MENU_DEV_KEYBOARD) key_cap(c, gx, gy, gh, k ? "Esc" : "Enter", u, 1);
            else pad_glyph(c, (float)gx, (float)gy, (float)gh, k ? G_CIRCLE : G_CROSS, dev == MENU_DEV_XBOX);
            text(c, b.x + (int)(60 * u), b.y, b.w - (int)(70 * u), b.h, k ? "Cancel" : "Reset", (int)(26 * u), FW_BOLD, 0, FACE_UI, C_TEXT,
                 AL_CENTER);
        }
    }
}

int menu_draw(uint32_t* px, int w, int h, int stride) {
    for (int y = 0; y < h; y++) memset(px + (size_t)y * stride, 0, (size_t)w * 4);
    static menu_view v;                           /* (big: not on the stack of a render thread) */
    menu_get_view(&v, w, h);
    if (!v.open) return 0;
    canvas c = { px, w, h, stride };
    draw_menu(&c, &v);
    return 1;
}

void menu_draw_fps(uint32_t* px, int w, int h, int stride, double fps, float u) {
    for (int y = 0; y < h; y++) memset(px + (size_t)y * stride, 0, (size_t)w * 4);
    canvas c = { px, w, h, stride };
    char t[32];
    snprintf(t, sizeof t, "%.0f FPS", fps);
    rrect(&c, 0, 0, (float)w, (float)h, 8 * u, 0, RGBA(0, 0, 0, 150), RGBA(0, 0, 0, 150));
    text(&c, 0, 0, w, h, t, (int)(h * 0.62f), FW_BOLD, 0, FACE_UI, fps >= 58.0 ? RGBA(140, 240, 140, 255) : RGBA(250, 210, 90, 255), AL_CENTER);
}
