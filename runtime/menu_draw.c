/* The Options menu's picture (menu_draw.h).
 *
 * (pc-menu2) A list of primitives in output pixels, drawn by the GPU at every present (menu_overlay.c) and rasterised
 * here on the CPU for GDI and the tests. The look follows Sly 2's own menus: every word in the game's font (sly_font.c,
 * from the disc at run time), the pause menu's colours (the highlighted entry in its warm orange, the others in its
 * slate blue), a comic red title with a black outline, a night-blue panel with a thin border, sliders and arrows in the
 * highlight colour, button prompts with the controller's symbols. */
#include "menu_draw.h"
#include "menu.h"
#include "sly_font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- colours (0xAARRGGBB, straight alpha) ---------------------------------------------------------------------- */
#define ARGB(a, r, g, b) ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
static const uint32_t C_DIM = ARGB(150, 0, 2, 8);
static const uint32_t C_PANEL_TOP = ARGB(242, 14, 28, 56), C_PANEL_BOT = ARGB(248, 6, 12, 28);
static const uint32_t C_BORDER = ARGB(255, 58, 92, 140), C_BORDER_IN = ARGB(90, 120, 160, 214);
static const uint32_t C_ORANGE = ARGB(255, 242, 166, 74), C_ORANGE_DK = ARGB(255, 208, 120, 44), C_ORANGE_DIM = ARGB(255, 150, 104, 52);
static const uint32_t C_SLATE = ARGB(255, 140, 164, 196), C_TEXT = ARGB(255, 214, 226, 242), C_WHITE = ARGB(255, 255, 255, 255);
static const uint32_t C_DISABLED = ARGB(255, 84, 100, 130), C_NOTE = ARGB(255, 246, 200, 104);
static const uint32_t C_TITLE = ARGB(255, 200, 24, 18), C_INK = ARGB(255, 6, 8, 14);
static const uint32_t C_TRACK = ARGB(255, 24, 38, 66), C_DESC = ARGB(190, 2, 6, 16), C_KEY = ARGB(255, 226, 234, 246);
static const uint32_t C_SEL_A = ARGB(70, 242, 166, 74), C_SEL_B = ARGB(26, 242, 166, 74), C_HOVER = ARGB(22, 200, 220, 255);

static uint32_t with_alpha(uint32_t c, float a) {
    float v = (float)(c >> 24) * a;
    return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f)) << 24 | (c & 0xFFFFFFu);
}

/* ---- the list ----------------------------------------------------------------------------------------------------- */

static ui_prim* ui_add(ui_list* L, int kind) {
    if (L->n >= UI_MAX_PRIMS) return NULL;
    ui_prim* q = &L->p[L->n++];
    memset(q, 0, sizeof *q);
    q->kind = (uint8_t)kind;
    return q;
}
static int cur_clip;

static void box(ui_list* L, float x, float y, float w, float h, float r, uint32_t top, uint32_t bot, float border, uint32_t bc) {
    if (w <= 0 || h <= 0) return;
    ui_prim* q = ui_add(L, UI_BOX);
    if (!q) return;
    q->clip = (uint8_t)cur_clip;
    q->x = x; q->y = y; q->w = w; q->h = h;
    q->c0 = top; q->c1 = bot; q->c2 = bc;
    q->p[0] = fminf(r, fminf(w, h) / 2);
    q->p[1] = border;
}
static void fillr(ui_list* L, float x, float y, float w, float h, float r, uint32_t c) { box(L, x, y, w, h, r, c, c, 0, 0); }
static void ring(ui_list* L, float x, float y, float w, float h, float r, float bw, uint32_t c) { box(L, x, y, w, h, r, 0, 0, bw, c); }

static void capsule(ui_list* L, float ax, float ay, float bx, float by, float wd, uint32_t c) {
    ui_prim* q = ui_add(L, UI_CAPSULE);
    if (!q) return;
    q->clip = (uint8_t)cur_clip;
    q->x = fminf(ax, bx) - wd; q->y = fminf(ay, by) - wd;
    q->w = fabsf(bx - ax) + 2 * wd; q->h = fabsf(by - ay) + 2 * wd;
    q->p[0] = ax; q->p[1] = ay; q->p[2] = bx; q->p[3] = by;
    q->u0 = wd;
    q->c0 = c;
}

static void arrow(ui_list* L, float cx, float cy, float s, int dir, uint32_t c) {
    ui_prim* q = ui_add(L, UI_ARROW);
    if (!q) return;
    q->clip = (uint8_t)cur_clip;
    if (dir == 2 || dir == -2) { q->x = cx - s; q->y = cy - s * 0.6f; q->w = 2 * s; q->h = s * 1.2f; }   /* (down / up) */
    else { q->x = cx - s * 0.6f; q->y = cy - s; q->w = s * 1.2f; q->h = 2 * s; }
    q->p[0] = (float)dir;
    q->c0 = c;
}

static int push_clip(ui_list* L, int x, int y, int w, int h) {
    if (L->nclip < 1) L->nclip = 1;
    if (L->nclip >= UI_MAX_CLIPS) return 0;
    L->clip[L->nclip][0] = x; L->clip[L->nclip][1] = y; L->clip[L->nclip][2] = w; L->clip[L->nclip][3] = h;
    return L->nclip++;
}

/* ---- text ---------------------------------------------------------------------------------------------------------- */

static const slyf_atlas* F;                      /* the font of this build */

enum { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };

static float tw(const char* s, float px) { return slyf_text_width(F, s, px); }

/* one line at baseline yb; returns its width. maxw > 0: shortened with "..." to fit */
static float text_base(ui_list* L, float x, float yb, float px, const char* s, uint32_t col, int align, float maxw, float outline,
                       uint32_t ocol) {
    if (!F || !s || !*s) return 0;
    char t[512];
    int n = slyf_cp1252(s, t, sizeof t);
    float sc = px / SLYF_EM, w = 0;
    for (int i = 0; i < n; i++) w += F->g[(uint8_t)t[i]].adv * sc;
    if (maxw > 0 && w > maxw) {                  /* shorten */
        float dots = F->g['.'].adv * sc * 3;
        while (n > 0 && w + dots > maxw) w -= F->g[(uint8_t)t[--n]].adv * sc;
        while (n > 0 && t[n - 1] == ' ') w -= F->g[(uint8_t)t[--n]].adv * sc;
        for (int k = 0; k < 3 && n < (int)sizeof t - 1; k++) { t[n++] = '.'; w += F->g['.'].adv * sc; }
    }
    float pen = align == AL_CENTER ? x - w / 2 : align == AL_RIGHT ? x - w : x;
    pen = floorf(pen + 0.5f);
    float W0 = (float)F->w[0], H0 = (float)F->h[0], k = sc / F->units_px;
    for (int i = 0; i < n; i++) {
        const slyf_glyph* g = &F->g[(uint8_t)t[i]];
        if (g->present) {
            ui_prim* q = ui_add(L, UI_GLYPH);
            if (!q) break;
            q->clip = (uint8_t)cur_clip;
            q->x = pen + g->ox * sc;
            q->y = yb + g->oy * sc;
            q->w = (float)g->aw * k;
            q->h = (float)g->ah * k;
            q->u0 = (float)g->ax / W0; q->v0 = (float)g->ay / H0;
            q->u1 = (float)(g->ax + g->aw) / W0; q->v1 = (float)(g->ay + g->ah) / H0;
            q->c0 = col;
            q->c2 = ocol;
            q->p[0] = outline;
        }
        pen += g->adv * sc;
    }
    return w;
}

/* in a box (y, h): the capitals centred vertically */
static float text_box(ui_list* L, float x, float y, float h, float px, const char* s, uint32_t col, int align, float maxw) {
    float cap = F ? F->ascent * px / SLYF_EM : px * 0.7f;
    return text_base(L, x, floorf(y + h / 2 + cap / 2 + 0.5f), px, s, col, align, maxw, 0, 0);
}

/* word-wrapped lines from the top of a box; returns the lines drawn */
static int text_wrap(ui_list* L, float x, float y, float w, float h, float px, const char* s, uint32_t col) {
    if (!F || !s) return 0;
    float lh = px * 0.92f, cap = F->ascent * px / SLYF_EM;
    int lines = 0;
    char line[512];
    const char* p = s;
    while (*p && (lines + 1) * lh <= h + 1) {
        int n = 0, last_space = -1;
        const char* q = p;
        while (*q && *q != '\n' && n < (int)sizeof line - 1) {
            line[n] = *q;
            line[n + 1] = 0;
            if (*q == ' ') last_space = n;
            if (tw(line, px) > w && n > 0) {
                if (last_space > 0) { n = last_space; q = p + last_space + 1; }
                break;
            }
            n++;
            q++;
        }
        line[n] = 0;
        int last_line = (lines + 2) * lh > h + 1 && *q && *q != '\n';
        text_base(L, x, floorf(y + lines * lh + cap + 0.5f), px, line, col, AL_LEFT, last_line ? w : 0, 0, 0);
        lines++;
        p = *q == '\n' ? q + 1 : q;
        while (*p == ' ') p++;
    }
    return lines;
}

/* ---- button glyphs ------------------------------------------------------------------------------------------------- */

enum { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_L1, G_R1, G_START, G_DPAD };

static float glyph_w(int g, float d) { return g == G_START ? d * 1.6f : g == G_L1 || g == G_R1 ? d * 1.3f : d; }

static void pad_glyph(ui_list* L, float x, float y, float d, int g, int xbox) {
    float cx = x + d / 2, cy = y + d / 2;
    if (g == G_L1 || g == G_R1 || g == G_START) {
        float w = glyph_w(g, d);
        box(L, x, y + d * 0.1f, w, d * 0.8f, d * 0.3f, C_KEY, ARGB(255, 196, 208, 228), 0, 0);
        const char* t = g == G_L1 ? (xbox ? "LB" : "L1") : g == G_R1 ? (xbox ? "RB" : "R1") : (xbox ? "Menu" : "Options");
        text_box(L, x + w / 2, y + d * 0.1f, d * 0.8f, d * (g == G_START ? 0.42f : 0.62f), t, C_INK, AL_CENTER, w * 0.92f);
        return;
    }
    if (g == G_DPAD) {
        float a = d * 0.32f;
        fillr(L, cx - a / 2, y + d * 0.06f, a, d * 0.88f, a * 0.25f, C_TEXT);
        fillr(L, x + d * 0.06f, cy - a / 2, d * 0.88f, a, a * 0.25f, C_TEXT);
        return;
    }
    if (xbox) {
        static const char* letters[] = { "A", "B", "X", "Y" };
        static const uint32_t cols[] = { ARGB(255, 60, 160, 70), ARGB(255, 210, 50, 46), ARGB(255, 48, 100, 210), ARGB(255, 214, 160, 30) };
        fillr(L, x, y, d, d, d / 2, cols[g]);
        text_box(L, cx, y, d, d * 0.8f, letters[g], C_WHITE, AL_CENTER, 0);
        return;
    }
    fillr(L, x, y, d, d, d / 2, C_KEY);
    static const uint32_t ink[] = { ARGB(255, 70, 110, 200), ARGB(255, 210, 60, 70), ARGB(255, 200, 90, 170), ARGB(255, 40, 160, 130) };
    float s = d * 0.24f, wd = d * 0.11f;
    switch (g) {
    case G_CROSS: capsule(L, cx - s, cy - s, cx + s, cy + s, wd, ink[0]); capsule(L, cx - s, cy + s, cx + s, cy - s, wd, ink[0]); break;
    case G_CIRCLE: ring(L, cx - s * 1.1f, cy - s * 1.1f, s * 2.2f, s * 2.2f, s * 1.1f, wd, ink[1]); break;
    case G_SQUARE: ring(L, cx - s, cy - s, 2 * s, 2 * s, wd * 0.3f, wd, ink[2]); break;
    case G_TRIANGLE: {
        float t = s * 1.15f, ty = cy + t * 0.15f;
        capsule(L, cx, ty - t, cx - t, ty + t * 0.7f, wd * 0.5f, ink[3]);
        capsule(L, cx - t, ty + t * 0.7f, cx + t, ty + t * 0.7f, wd * 0.5f, ink[3]);
        capsule(L, cx + t, ty + t * 0.7f, cx, ty - t, wd * 0.5f, ink[3]);
        break;
    }
    default: break;
    }
}

/* a keyboard key cap with its name; returns its width (draw = 0: measure only) */
static float key_cap(ui_list* L, float x, float y, float h, const char* name, int draw) {
    float px = h * 0.72f, w = tw(name, px) + h * 0.55f;
    if (w < h) w = h;
    if (draw) {
        box(L, x, y, w, h, h * 0.22f, C_KEY, ARGB(255, 190, 202, 224), 0, 0);
        text_box(L, x + w / 2, y, h, px, name, C_INK, AL_CENTER, 0);
    }
    return w;
}

/* a prompt: the glyph / key and its label, right-aligned in r */
static void hint(ui_list* L, menu_rect r, int dev, const char* kb, int g, const char* label, float u, int hover) {
    float gh = r.h * 0.8f, px = 34 * u;
    float lw = tw(label, px);
    float gw = dev == MENU_DEV_KEYBOARD ? key_cap(L, 0, 0, gh, kb, 0) : glyph_w(g, gh);
    float total = gw + 10 * u + lw, x = r.x + r.w - total, y = r.y + (r.h - gh) / 2;
    if (hover) fillr(L, x - 12 * u, (float)r.y, total + 24 * u, (float)r.h, r.h * 0.3f, with_alpha(C_ORANGE, 0.22f));
    if (dev == MENU_DEV_KEYBOARD) key_cap(L, x, y, gh, kb, 1);
    else pad_glyph(L, x, y, gh, g, dev == MENU_DEV_XBOX);
    text_box(L, x + gw + 10 * u, (float)r.y, (float)r.h, px, label, hover ? C_ORANGE : C_TEXT, AL_LEFT, 0);
}

/* ---- the menu ------------------------------------------------------------------------------------------------------- */

static void draw_row(ui_list* L, const menu_view* v, const menu_layout* Y, int k, float u) {
    int i = Y->first + k;
    const menu_row* row = &v->rows[i];
    menu_rect r = Y->rows[k];
    int sel = i == v->sel;
    uint32_t lc = !row->enabled ? C_DISABLED : sel ? C_ORANGE : C_TEXT;
    float px = 38 * u, vpx = 36 * u;
    text_box(L, r.x + 28 * u, (float)r.y, (float)r.h, px, row->label, lc, AL_LEFT, (float)(Y->value_x - r.x) - 64 * u);
    if (row->changed) {                          /* (menu) a change not applied yet: an orange dot before the value */
        float d = 12 * u;
        fillr(L, Y->value_x - 30 * u, r.y + (r.h - d) / 2.0f, d, d, d / 2, C_ORANGE);
    }
    menu_rect lft = Y->left[k], rgt = Y->right[k];
    float vx0 = (float)(lft.x + lft.w), vx1 = (float)rgt.x, ay = r.y + r.h / 2.0f, as = 11 * u;
    uint32_t vc = row->enabled ? (sel ? C_WHITE : C_TEXT) : C_DISABLED;
    uint32_t ac = sel ? C_ORANGE : C_ORANGE_DIM;
    switch (row->kind) {
    case MENU_ITEM_CHOICE:
        if (row->can_left) arrow(L, lft.x + lft.w / 2.0f, ay, as, -1, ac);
        if (row->can_right) arrow(L, rgt.x + rgt.w / 2.0f, ay, as, 1, ac);
        text_box(L, (vx0 + vx1) / 2, (float)r.y, (float)r.h, vpx, row->value, vc, AL_CENTER, vx1 - vx0 - 8 * u);
        break;
    case MENU_ITEM_SLIDER: {
        if (row->can_left) arrow(L, lft.x + lft.w / 2.0f, ay, as, -1, ac);
        if (row->can_right) arrow(L, rgt.x + rgt.w / 2.0f, ay, as, 1, ac);
        float nw = 96 * u, tx0 = vx0 + 10 * u, tx1 = vx1 - nw - 10 * u;   /* (menu.c slider_drag_to: the same track) */
        float th = 10 * u, frac = row->hi > row->lo ? (float)(row->val - row->lo) / (float)(row->hi - row->lo) : 0;
        fillr(L, tx0, ay - th / 2, tx1 - tx0, th, th / 2, C_TRACK);
        if (frac > 0) box(L, tx0, ay - th / 2, (tx1 - tx0) * frac, th, th / 2, row->enabled ? C_ORANGE : C_DISABLED,
                          row->enabled ? C_ORANGE_DK : C_DISABLED, 0, 0);
        float kr = (sel && v->drag == 1 ? 15 : 13) * u, kx = tx0 + (tx1 - tx0) * frac;
        box(L, kx - kr, ay - kr, 2 * kr, 2 * kr, kr, row->enabled ? ARGB(255, 255, 246, 232) : C_DISABLED,
            row->enabled ? ARGB(255, 236, 214, 186) : C_DISABLED, 2.5f * u, row->enabled ? ARGB(255, 120, 72, 24) : C_TRACK);
        text_box(L, vx1 - 4 * u, (float)r.y, (float)r.h, vpx, row->value, vc, AL_RIGHT, nw);
        break;
    }
    case MENU_ITEM_INFO:
        text_box(L, (vx0 + vx1) / 2, (float)r.y, (float)r.h, vpx * 0.92f, row->value, row->enabled ? C_NOTE : C_DISABLED, AL_CENTER,
                 vx1 - vx0 + lft.w);
        break;
    case MENU_ITEM_ACTION:
        arrow(L, rgt.x + rgt.w / 2.0f, ay, as, 1, ac);
        break;
    case MENU_ITEM_BIND: {
        if (v->capture && sel) {
            text_box(L, (vx0 + vx1) / 2, (float)r.y, (float)r.h, vpx, v->page == MENU_PAGE_KEYS ? "Press a key...  (Esc: cancel)" :
                     "Press a button...  (PS button: cancel)", C_NOTE, AL_CENTER, vx1 - vx0 + lft.w);
        } else {
            float kh = r.h * 0.70f, colw = (vx1 - vx0) / 2;
            for (int s = 0; s < 2; s++) {
                const char* t = s ? row->value2 : row->value;
                float cx0 = vx0 + s * colw;
                if (sel && s == v->bind_slot) ring(L, cx0 + 6 * u, r.y + 3 * u, colw - 12 * u, r.h - 6 * u, 10 * u, 2.5f * u, C_ORANGE);
                if (!strcmp(t, "-")) { text_box(L, cx0 + colw / 2, (float)r.y, (float)r.h, vpx, "-", C_DISABLED, AL_CENTER, 0); continue; }
                float kw = key_cap(L, 0, 0, kh, t, 0);
                if (kw > colw - 20 * u) kw = colw - 20 * u;
                key_cap(L, cx0 + colw / 2 - kw / 2, r.y + (r.h - kh) / 2, kh, t, 1);
            }
        }
        break;
    }
    default: break;
    }
}

/* the title: the game's lettering in red with a heavy black outline and a drop shadow (as its comic headers) */
static float title(ui_list* L, float x, float yb, float px, const char* s) {
    text_base(L, x + px * 0.05f, yb + px * 0.06f, px, s, ARGB(140, 0, 0, 0), AL_LEFT, 0, px * 0.09f, ARGB(140, 0, 0, 0));
    return text_base(L, x, yb, px, s, C_TITLE, AL_LEFT, 0, px * 0.075f, C_INK);
}

static void panel(ui_list* L, float x, float y, float w, float h, float u) {
    fillr(L, x - 4 * u, y + 12 * u, w + 8 * u, h, 28 * u, ARGB(110, 0, 0, 0));             /* shadow */
    box(L, x, y, w, h, 24 * u, C_PANEL_TOP, C_PANEL_BOT, 4 * u, C_BORDER);
    ring(L, x + 8 * u, y + 8 * u, w - 16 * u, h - 16 * u, 17 * u, fmaxf(1.0f, 1.5f * u), C_BORDER_IN);
}

static void draw_dialog(ui_list* L, const menu_view* v, const menu_layout* Y, float u) {
    int dev = v->device;
    menu_rect B = Y->dlg;
    panel(L, (float)B.x, (float)B.y, (float)B.w, (float)B.h, u);
    float ty = B.y + 30 * u;
    text_box(L, B.x + B.w / 2.0f, ty, 60 * u, 50 * u, v->dlg_title, C_ORANGE, AL_CENTER, B.w - 60 * u);
    if (v->dlg_text[0]) text_wrap(L, B.x + 44 * u, ty + 76 * u, B.w - 88 * u, 96 * u, 34 * u, v->dlg_text, C_TEXT);
    /* the buttons: yes (gold), [alt,] no; (menu) the focused one is lit (left / right move it, Confirm presses it); each
     * shows its own shortcut */
    int three = v->dlg_buttons == 3;
    for (int k = 0; k < (three ? 3 : 2); k++) {
        int which = three ? k : (k ? 2 : 0);    /* 0 yes, 1 alt, 2 no */
        menu_rect b = three ? Y->dlg3[k] : k ? Y->dlg_no : Y->dlg_yes;
        int hov = v->hover_btn == (which == 0 ? 3 : which == 2 ? 4 : 7), foc = v->dlg_focus == which;
        int lit = hov || foc;
        box(L, (float)b.x, (float)b.y, (float)b.w, (float)b.h, 14 * u, lit ? with_alpha(C_ORANGE, 0.30f) : ARGB(255, 20, 34, 62),
            lit ? with_alpha(C_ORANGE, 0.16f) : ARGB(255, 12, 22, 42), (foc ? 3.5f : 2.5f) * u, which == 0 || foc ? C_ORANGE : C_SLATE);
        const char* kb = three ? (which == 0 ? "F" : which == 1 ? "R" : "Esc") : which ? "Esc" : "Enter";
        int g = three ? (which == 0 ? G_SQUARE : which == 1 ? G_TRIANGLE : G_CIRCLE) : which ? G_CIRCLE : G_CROSS;
        float gh = b.h * 0.62f, gx = b.x + 18 * u, gy = b.y + (b.h - gh) / 2;
        float gw = dev == MENU_DEV_KEYBOARD ? key_cap(L, gx, gy, gh, kb, 1) : (pad_glyph(L, gx, gy, gh, g, dev == MENU_DEV_XBOX), gh);
        float lx0 = gx + gw + 8 * u, lx1 = b.x + b.w - 14 * u;
        const char* t = which == 0 ? v->dlg_yes : which == 1 ? v->dlg_alt : v->dlg_no;
        text_box(L, (lx0 + lx1) / 2, (float)b.y, (float)b.h, 38 * u, t, lit || which == 0 ? C_ORANGE : C_TEXT, AL_CENTER, lx1 - lx0);
    }
}

static menu_view V;                              /* (big: not on a render thread's stack) */

static void build_menu(ui_list* L, const menu_view* v, int w, int h) {
    menu_layout Y;
    menu_compute_layout_f(w, h, v->nrows, v->scroll_f, &Y);
    float u = Y.u;
    cur_clip = 0;
    fillr(L, 0, 0, (float)w, (float)h, 0, C_DIM);
    if (v->dialog_only) { draw_dialog(L, v, &Y, u); return; }
    menu_rect P = Y.panel;
    panel(L, (float)P.x, (float)P.y, (float)P.w, (float)P.h, u);
    /* title, sub-page title */
    menu_rect H = Y.header;
    float tpx = 104 * u, tyb = H.y + H.h * 0.5f + F->ascent * tpx / SLYF_EM * 0.5f;
    float tw0 = title(L, (float)H.x + 4 * u, tyb, tpx, "OPTIONS");
    if (v->page_title[0]) text_base(L, H.x + tw0 + 30 * u, tyb, 46 * u, v->page_title, C_TEXT, AL_LEFT, 0, 0, 0);
    /* tabs */
    int main_page = menu_page_parent(v->page);
    for (int k = 0; k < MENU_PAGE_N; k++) {
        menu_rect t = Y.tabs[k];
        int on = k == main_page, hov = k == v->hover_tab;
        if (hov && !on) fillr(L, t.x + 6 * u, (float)t.y, t.w - 12 * u, (float)t.h, 12 * u, C_HOVER);
        text_box(L, t.x + t.w / 2.0f, (float)t.y - 2 * u, (float)t.h, (on ? 46 : 42) * u, menu_page_name(k), on ? C_ORANGE : hov ? C_TEXT : C_SLATE,
                 AL_CENTER, t.w - 8 * u);
        if (on) capsule(L, t.x + t.w * 0.22f, t.y + t.h - 4 * u, t.x + t.w * 0.78f, t.y + t.h - 6 * u, 3 * u, C_ORANGE);
    }
    fillr(L, (float)Y.list.x, Y.tabs[0].y + Y.tabs[0].h + 6 * u, (float)Y.list.w, fmaxf(1.0f, 1.5f * u), 0, with_alpha(C_BORDER, 0.8f));
    for (int k = 0; k < 2; k++) {                 /* page switch hints */
        menu_rect t = Y.tab_hint[k];
        float gh = t.h * 0.66f;
        if (v->hover_btn == 5 + k) fillr(L, (float)t.x, (float)t.y, (float)t.w, (float)t.h, 12 * u, C_HOVER);
        if (v->device == MENU_DEV_KEYBOARD) {
            float kw = key_cap(L, 0, 0, gh, k ? "E" : "Q", 0);
            key_cap(L, t.x + (t.w - kw) / 2, t.y + (t.h - gh) / 2, gh, k ? "E" : "Q", 1);
        } else {
            float gw = glyph_w(G_L1, gh);
            pad_glyph(L, t.x + (t.w - gw) / 2, t.y + (t.h - gh) / 2, gh, k ? G_R1 : G_L1, v->device == MENU_DEV_XBOX);
        }
    }
    /* rows (clipped to the list: it scrolls by pixels) */
    cur_clip = push_clip(L, Y.list.x, Y.list.y, Y.list.w, Y.list.h);
    for (int k = 0; k <= Y.visible && Y.first + k < v->nrows && k < MENU_MAX_ROWS; k++)
        if (Y.first + k != v->sel && Y.first + k == v->hover_row)
            fillr(L, (float)Y.rows[k].x, (float)Y.rows[k].y, (float)Y.rows[k].w, (float)Y.rows[k].h, 12 * u, C_HOVER);
    if (v->nrows > 0) {                           /* the selection band glides between rows */
        float by = Y.list.y + (v->sel_f - v->scroll_f) * Y.row_h;
        float bw = (float)(Y.rows[0].w), bh = (float)(Y.row_h - (int)(4 * u + 0.5f));
        box(L, (float)Y.list.x, by, bw, bh, 12 * u, C_SEL_A, C_SEL_B, 0, 0);
        capsule(L, Y.list.x + 8 * u, by + 10 * u, Y.list.x + 8 * u, by + bh - 10 * u, 3.5f * u, C_ORANGE);
    }
    for (int k = 0; k <= Y.visible && Y.first + k < v->nrows && k < MENU_MAX_ROWS; k++) draw_row(L, v, &Y, k, u);
    cur_clip = 0;
    if (Y.scrollbar.w) {
        menu_rect s = Y.scrollbar;
        fillr(L, (float)s.x, (float)s.y, (float)s.w, (float)s.h, s.w / 2.0f, ARGB(200, 24, 38, 66));
        float frac = (float)Y.visible / v->nrows, pos = v->scroll_f / v->nrows;
        fillr(L, (float)s.x, s.y + s.h * pos, (float)s.w, s.h * frac, s.w / 2.0f, v->drag == 2 ? C_ORANGE : C_ORANGE_DIM);
    }
    /* description */
    menu_rect D = Y.desc;
    fillr(L, (float)D.x, (float)D.y, (float)D.w, (float)D.h, 14 * u, C_DESC);
    if (v->sel >= 0 && v->sel < v->nrows) {
        const menu_row* row = &v->rows[v->sel];
        float pad = 20 * u, nh = row->note[0] ? 36 * u : 0;
        text_wrap(L, D.x + pad, D.y + 12 * u, D.w - 2 * pad, D.h - 16 * u - nh, 31 * u, row->desc, ARGB(255, 186, 202, 226));
        if (nh) text_box(L, D.x + pad, D.y + D.h - nh - 8 * u, nh, 31 * u, row->note, C_NOTE, AL_LEFT, D.w - 2 * pad);
    }
    /* footer: navigation on the left, buttons on the right */
    menu_rect Fr = Y.footer;
    int dev = v->device;
    if (v->pending) {                             /* (menu) changes staged: how many, Apply / Discard */
        char t[64];
        if (v->pending == 1) snprintf(t, sizeof t, "1 change not applied");
        else snprintf(t, sizeof t, "%d changes not applied", v->pending);
        float d = 12 * u;
        fillr(L, (float)Fr.x + 4 * u, Fr.y + (Fr.h - d) / 2.0f, d, d, d / 2, C_ORANGE);
        text_box(L, Fr.x + 26 * u, (float)Fr.y, (float)Fr.h, 33 * u, t, C_NOTE, AL_LEFT, (float)(Y.btn[2].x - Fr.x) - 40 * u);
    } else {
        float gh = Fr.h * 0.8f, x = (float)Fr.x, y = Fr.y + (Fr.h - gh) / 2, px = 33 * u;
        if (dev == MENU_DEV_KEYBOARD) {
            float kw = gh * 1.5f;                     /* (the arrow keys: drawn as keys with arrows) */
            box(L, x, y, kw, gh, gh * 0.22f, C_KEY, ARGB(255, 190, 202, 224), 0, 0);
            arrow(L, x + kw * 0.32f, y + gh / 2, gh * 0.2f, -2, C_INK);
            arrow(L, x + kw * 0.68f, y + gh / 2, gh * 0.2f, 2, C_INK);
            x += kw + 10 * u;
            x += text_box(L, x, (float)Fr.y, (float)Fr.h, px, "Move", C_TEXT, AL_LEFT, 0) + 26 * u;
            box(L, x, y, kw, gh, gh * 0.22f, C_KEY, ARGB(255, 190, 202, 224), 0, 0);
            arrow(L, x + kw * 0.32f, y + gh / 2, gh * 0.2f, -1, C_INK);
            arrow(L, x + kw * 0.68f, y + gh / 2, gh * 0.2f, 1, C_INK);
            x += kw + 10 * u;
            x += text_box(L, x, (float)Fr.y, (float)Fr.h, px, "Change", C_TEXT, AL_LEFT, 0) + 26 * u;
        } else {
            pad_glyph(L, x, y, gh, G_DPAD, 0);
            x += gh + 10 * u;
            text_box(L, x, (float)Fr.y, (float)Fr.h, px, "Move / Change", C_TEXT, AL_LEFT, 0);
        }
    }
    int sub = v->page >= MENU_PAGE_N;
    hint(L, Y.btn[0], dev, "Esc", G_CIRCLE, sub ? "Back" : "Close", u, v->hover_btn == 0);
    if (v->pending) {
        hint(L, Y.btn[1], dev, "R", G_TRIANGLE, "Discard", u, v->hover_btn == 1);
        hint(L, Y.btn[2], dev, "F", G_SQUARE, "Apply", u, v->hover_btn == 2);
    } else if (dev != MENU_DEV_KEYBOARD && sub) hint(L, Y.btn[1], dev, "", G_START, "Close", u, 0);
    else if (dev != MENU_DEV_KEYBOARD) hint(L, Y.btn[1], dev, "", G_CROSS, "Select", u, 0);
    else hint(L, Y.btn[1], dev, "Enter", G_CROSS, "Select", u, 0);
    if (v->dialog) {
        fillr(L, (float)P.x, (float)P.y, (float)P.w, (float)P.h, 24 * u, ARGB(130, 0, 0, 0));
        draw_dialog(L, v, &Y, u);
    }
}

static void build_fps(ui_list* L, int w, int h, double fps) {
    (void)w;
    float u = (float)h / 1080.0f;
    if (u < 0.4f) u = 0.4f;
    char t[32];
    snprintf(t, sizeof t, "%.0f FPS", fps);
    float px = 34 * u, bw = tw(t, px) + 28 * u, bh = 46 * u;
    cur_clip = 0;
    fillr(L, 14 * u, 14 * u, bw, bh, 10 * u, ARGB(150, 0, 0, 0));
    text_box(L, 14 * u + bw / 2, 14 * u, bh, px, t, fps >= 58.0 ? ARGB(255, 150, 240, 150) : C_NOTE, AL_CENTER, 0);
}

static char toast_text[48];
void menu_set_toast(const char* text) { snprintf(toast_text, sizeof toast_text, "%s", text ? text : ""); }

/* (pc-shot) a small notice, bottom centre, over everything */
static void build_toast(ui_list* L, int w, int h) {
    float u = (float)h / 1080.0f;
    if (u < 0.4f) u = 0.4f;
    float px = 30 * u, bw = tw(toast_text, px) + 40 * u, bh = 46 * u;
    cur_clip = 0;
    fillr(L, ((float)w - bw) / 2, (float)h - 70 * u - bh, bw, bh, 10 * u, ARGB(170, 0, 0, 0));
    text_box(L, (float)w / 2, (float)h - 70 * u - bh, bh, px, toast_text, ARGB(255, 240, 240, 240), AL_CENTER, 0);
}

int menu_build(ui_list* L, int w, int h, double now_s, double fps) {    L->n = 0;
    L->nclip = 1;
    L->w = w;
    L->h = h;
    F = slyf_current();
    L->font_serial = slyf_serial();
    if (!F || w <= 0 || h <= 0) return 0;
    if (now_s > 0) menu_animate(now_s);
    menu_get_view(&V, w, h);
    if (V.open) build_menu(L, &V, w, h);
    else if (fps >= 0) build_fps(L, w, h, fps);
    if (toast_text[0] && now_s > 0) build_toast(L, w, h);
    return L->n;
}

/* ---- the CPU rasteriser (GDI presenter, tests) ----------------------------------------------------------------------
 * The same coverage functions as the GPU's pixel shader (menu_overlay.c): signed distances with a one-pixel ramp, the
 * glyph atlas sampled bilinearly from the mip level that matches the scale (and the next, blended). */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void blend_px(uint32_t* d, float r, float g, float b, float a) {   /* premultiplied src over dst */
    if (a <= 0.0f) return;
    uint32_t x = *d;
    float inv = 1.0f - a;
    float dr = (float)(x >> 16 & 255), dg = (float)(x >> 8 & 255), db = (float)(x & 255), da = (float)(x >> 24);
    int R = (int)(r * 255 + dr * inv + 0.5f), G = (int)(g * 255 + dg * inv + 0.5f), B = (int)(b * 255 + db * inv + 0.5f),
        A = (int)(a * 255 + da * inv + 0.5f);
    *d = (uint32_t)(A > 255 ? 255 : A) << 24 | (uint32_t)(R > 255 ? 255 : R) << 16 | (uint32_t)(G > 255 ? 255 : G) << 8 |
         (uint32_t)(B > 255 ? 255 : B);
}

static void col_f(uint32_t c, float* r, float* g, float* b, float* a) {
    *a = (float)(c >> 24) / 255.0f;
    *r = (float)(c >> 16 & 255) / 255.0f * *a;
    *g = (float)(c >> 8 & 255) / 255.0f * *a;
    *b = (float)(c & 255) / 255.0f * *a;
}

static float atlas_at(int lv, float u, float v) {
    int w = F->w[lv], h = F->h[lv];
    float x = u * w - 0.5f, y = v * h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    float s = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int xx = x0 + i, yy = y0 + j;
            if (xx < 0) xx = 0;
            if (yy < 0) yy = 0;
            if (xx >= w) xx = w - 1;
            if (yy >= h) yy = h - 1;
            s += (float)F->px[lv][(size_t)yy * w + xx] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        }
    return s / 255.0f;
}

static float atlas_tri(float lod, float u, float v) {
    if (lod < 0) lod = 0;
    if (lod > (float)(F->levels - 1)) lod = (float)(F->levels - 1);
    int l0 = (int)lod;
    float f = lod - l0;
    float a = atlas_at(l0, u, v);
    return f > 0 && l0 + 1 < F->levels ? a + (atlas_at(l0 + 1, u, v) - a) * f : a;
}

void ui_raster(const ui_list* L, uint32_t* px, int w, int h, int stride) {
    F = slyf_current();
    for (int i = 0; i < L->n; i++) {
        const ui_prim* q = &L->p[i];
        int x0 = (int)floorf(q->x), y0 = (int)floorf(q->y), x1 = (int)ceilf(q->x + q->w), y1 = (int)ceilf(q->y + q->h);
        if (q->clip && q->clip < L->nclip) {
            const int* c = L->clip[q->clip];
            if (x0 < c[0]) x0 = c[0];
            if (y0 < c[1]) y0 = c[1];
            if (x1 > c[0] + c[2]) x1 = c[0] + c[2];
            if (y1 > c[1] + c[3]) y1 = c[1] + c[3];
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        float r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2;
        col_f(q->c0, &r0, &g0, &b0, &a0);
        col_f(q->c1, &r1, &g1, &b1, &a1);
        col_f(q->c2, &r2, &g2, &b2, &a2);
        float cx = q->x + q->w / 2, cy = q->y + q->h / 2;
        float lod = 0;
        if (q->kind == UI_GLYPH && F) lod = log2f(fmaxf(1e-6f, (q->u1 - q->u0) * F->w[0] / q->w));
        for (int y = y0; y < y1; y++) {
            uint32_t* row = px + (size_t)y * stride;
            for (int x = x0; x < x1; x++) {
                float sx = x + 0.5f, sy = y + 0.5f;
                switch (q->kind) {
                case UI_BOX: {
                    float hx = q->w / 2, hy = q->h / 2, rad = q->p[0];
                    float qx = fabsf(sx - cx) - hx + rad, qy = fabsf(sy - cy) - hy + rad;
                    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
                    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - rad;
                    float cov = clampf(0.5f - d, 0, 1);
                    if (cov <= 0) break;
                    float t = clampf((sy - q->y) / q->h, 0, 1);
                    float fr = r0 + (r1 - r0) * t, fg = g0 + (g1 - g0) * t, fb = b0 + (b1 - b0) * t, fa = a0 + (a1 - a0) * t;
                    if (q->p[1] > 0) {
                        float in = clampf(0.5f - (d + q->p[1]), 0, 1), ri = cov - in;
                        blend_px(&row[x], fr * in + r2 * ri, fg * in + g2 * ri, fb * in + b2 * ri, fa * in + a2 * ri);
                    } else {
                        blend_px(&row[x], fr * cov, fg * cov, fb * cov, fa * cov);
                    }
                    break;
                }
                case UI_CAPSULE: {
                    float ax = q->p[0], ay = q->p[1], bx = q->p[2], by = q->p[3];
                    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                    float t = l2 > 0 ? clampf(((sx - ax) * dx + (sy - ay) * dy) / l2, 0, 1) : 0;
                    float ex = sx - ax - t * dx, ey = sy - ay - t * dy;
                    float cov = clampf(q->u0 / 2 - sqrtf(ex * ex + ey * ey) + 0.5f, 0, 1);
                    blend_px(&row[x], r0 * cov, g0 * cov, b0 * cov, a0 * cov);
                    break;
                }
                case UI_ARROW: {
                    int vert = q->p[0] > 1.5f || q->p[0] < -1.5f;
                    float hx = vert ? q->h / 2 : q->w / 2, hy = vert ? q->w / 2 : q->h / 2;
                    float lx = (vert ? sy - cy : sx - cx) * (q->p[0] < 0 ? -1.0f : 1.0f), ly = vert ? sx - cx : sy - cy;
                    float n1x = hy, n1y = -2 * hx, n2x = hy, n2y = 2 * hx, l = sqrtf(n1x * n1x + n1y * n1y);
                    float d1 = ((lx - hx) * n1x + ly * n1y) / l, d2 = ((lx - hx) * n2x + ly * n2y) / l, d3 = -hx - lx;
                    float d = fmaxf(d1, fmaxf(d2, d3));
                    float cov = clampf(0.5f - d, 0, 1);
                    blend_px(&row[x], r0 * cov, g0 * cov, b0 * cov, a0 * cov);
                    break;
                }
                case UI_GLYPH: {
                    if (!F) break;
                    float u = q->u0 + (q->u1 - q->u0) * (sx - q->x) / q->w, v = q->v0 + (q->v1 - q->v0) * (sy - q->y) / q->h;
                    float a = atlas_tri(lod, u, v);
                    if (q->p[0] > 0) {           /* outline: the coverage grown by p[0] pixels */
                        float du = (q->u1 - q->u0) / q->w * q->p[0], dv = (q->v1 - q->v0) / q->h * q->p[0], o = a;
                        for (int k = 0; k < 12; k++) {
                            float an = (float)k * 0.5235988f;
                            o = fmaxf(o, atlas_tri(lod, u + du * cosf(an), v + dv * sinf(an)));
                            o = fmaxf(o, atlas_tri(lod, u + du * 0.5f * cosf(an), v + dv * 0.5f * sinf(an)));
                        }
                        float ri = o - a > 0 ? o - a : 0;
                        blend_px(&row[x], r0 * a + r2 * ri, g0 * a + g2 * ri, b0 * a + b2 * ri, a0 * a + a2 * ri);
                    } else {
                        blend_px(&row[x], r0 * a, g0 * a, b0 * a, a0 * a);
                    }
                    break;
                }
                default: break;
                }
            }
        }
    }
}

/* ---- the old interface (GDI, tests) --------------------------------------------------------------------------------- */

static ui_list cpu_list;

static int list_ready(void) {
    if (!cpu_list.p) cpu_list.p = malloc(sizeof *cpu_list.p * UI_MAX_PRIMS);
    return cpu_list.p != NULL;
}

int menu_draw(uint32_t* px, int w, int h, int stride) {
    for (int y = 0; y < h; y++) memset(px + (size_t)y * stride, 0, (size_t)w * 4);
    if (!list_ready() || !menu_is_open()) return 0;
    if (!menu_build(&cpu_list, w, h, 0, -1)) return 0;
    ui_raster(&cpu_list, px, w, h, stride);
    return 1;
}

void menu_draw_fps(uint32_t* px, int w, int h, int stride, double fps, float u) {
    (void)u;
    for (int y = 0; y < h; y++) memset(px + (size_t)y * stride, 0, (size_t)w * 4);
    if (!list_ready()) return;
    cpu_list.n = 0;
    cpu_list.nclip = 1;
    F = slyf_current();
    if (!F) return;
    char t[32];
    snprintf(t, sizeof t, "%.0f FPS", fps);
    float px_ = h * 0.7f;
    cur_clip = 0;
    fillr(&cpu_list, 0, 0, (float)w, (float)h, h * 0.2f, ARGB(150, 0, 0, 0));
    text_box(&cpu_list, w / 2.0f, 0, (float)h, px_, t, fps >= 58.0 ? ARGB(255, 150, 240, 150) : C_NOTE, AL_CENTER, (float)w);
    ui_raster(&cpu_list, px, w, h, stride);
}
