/* The Options menu's model: pages, rows, values, navigation, layout (menu.h). */
#include "menu.h"
#include "menu_ext.h"
#include "settings.h"
#include "gfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static SRWLOCK lock = SRWLOCK_INIT;
static menu_host host;
static struct {
    int open, page, sel, scroll, capture, confirm_reset, device;
    int hover_tab, hover_row, hover_btn;
    int visible;                         /* rows that fit (from the last layout) */
    int cw, ch;                          /* client size of the last view */
    uint32_t serial;                     /* bumped by every change of the state */
    int sel_of[MENU_PAGE_ALL];           /* the row each page had selected */
} st = { .visible = 10, .hover_tab = -1, .hover_row = -1, .hover_btn = -1 };

#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)

void menu_set_host(const menu_host* h) {
    LOCK();
    if (h) host = *h; else memset(&host, 0, sizeof host);
    st.serial++;
    UNLOCK();
}

static const char* const page_names[MENU_PAGE_ALL] = { "Graphics", "Display", "Audio", "Controls", "Keyboard", "Controller" };
const char* menu_page_name(int page) { return page >= 0 && page < MENU_PAGE_ALL ? page_names[page] : ""; }

/* ---- host defaults ------------------------------------------------------------------------------------------ */

static int h_gpu_active(void) { return host.gpu_active ? host.gpu_active() : ps2_settings.renderer_gpu; }
static int h_gpu_started(void) { return host.gpu_started ? host.gpu_started() : h_gpu_active(); }
static void h_output(int* w, int* h) {
    *w = *h = 0;
    if (host.output_size) host.output_size(w, h);
    if (*w <= 0 || *h <= 0) { *w = st.cw > 0 ? st.cw : 1920; *h = st.ch > 0 ? st.ch : 1080; }
}
static void h_display(int* w, int* h, int* hz) {
    *w = *h = *hz = 0;
    if (host.display_size) host.display_size(w, h, hz);
    if (*w <= 0 || *h <= 0) { *w = 1920; *h = 1080; }
}
static int h_monitors(void) { int n = host.monitor_count ? host.monitor_count() : 1; return n < 1 ? 1 : n; }
static int h_window_mode(void) { return host.window_mode ? host.window_mode() : ps2_settings.win_fullscreen; }

/* ---- values -------------------------------------------------------------------------------------------------- */

static int opt(const char* section, const char* key) { return settings_find(section, key); }
static int get(const char* section, const char* key) { return settings_get(&ps2_settings, opt(section, key)); }
static int change(const char* section, const char* key, int v) { return settings_change(opt(section, key), v, 1); }

/* the internal resolution's real size at scale n: rows 448 n; columns 512 n at 4:3, at a wider aspect
 * 512 round(n W) (W = aspect / (4/3): the anamorphic display buffers, notes/widescreen.md) from 2x on */
static void scale_pixels(int n, int* w, int* h) {
    double wide = (double)ps2_settings.aspect_w / (double)ps2_settings.aspect_h / (4.0 / 3.0);
    int cols = n;
    if (n >= 2 && wide > 1.001) cols = (int)(n * wide + 0.5);
    *w = 512 * cols;
    *h = 448 * n;
}

static int native_scale(void) {
    int w, h;
    h_output(&w, &h);
    return gfx_native_scale(h);
}

/* choices of each CHOICE row: values and texts */
typedef struct { int n; int val[MENU_MAX_CHOICES]; char text[MENU_MAX_CHOICES][MENU_TEXT]; } choices;

static void add(choices* c, int v, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void add(choices* c, int v, const char* fmt, ...) {
    if (c->n >= MENU_MAX_CHOICES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->text[c->n], MENU_TEXT, fmt, ap);
    va_end(ap);
    c->val[c->n++] = v;
}

static int reduce(int* w, int* h) {
    int a = *w, b = *h;
    while (b) { int t = a % b; a = b; b = t; }
    if (a > 1) { *w /= a; *h /= a; }
    return 1;
}

static void aspect_choices(choices* c) {
    add(c, 4 << 16 | 3, "4:3 (original)");
    add(c, 16 << 16 | 10, "16:10");
    add(c, 16 << 16 | 9, "16:9 (widescreen)");
    int dw, dh, hz;
    h_display(&dw, &dh, &hz);
    int aw = dw, ah = dh;
    reduce(&aw, &ah);
    double d = (double)dw / dh;
    if (d >= 21.0 / 9.0 - 0.02) add(c, 21 << 16 | 9, "21:9 (ultrawide)");
    if (d >= 32.0 / 9.0 - 0.02) add(c, 32 << 16 | 9, "32:9 (super ultrawide)");
    int own = aw << 16 | ah, found = 0;
    for (int k = 0; k < c->n; k++) found |= c->val[k] == own;
    if (!found && d >= 1.0 && d <= 4.0 && aw < 1000) add(c, own, "%d:%d (your display)", aw, ah);
    int cur = get("display", "aspect");
    found = 0;
    for (int k = 0; k < c->n; k++) found |= c->val[k] == cur;
    if (!found) add(c, cur, "%d:%d", cur >> 16, cur & 0xFFFF);
}

static void winsize_choices(choices* c) {
    static const int heights[] = { 540, 720, 768, 900, 1080, 1200, 1440, 1600, 1800, 2160, 2880 };
    int dw, dh, hz;
    h_display(&dw, &dh, &hz);
    double a = (double)ps2_settings.aspect_w / ps2_settings.aspect_h;
    for (size_t k = 0; k < sizeof heights / sizeof heights[0]; k++) {
        int h = heights[k], w = (int)(h * a + 0.5);
        if (w <= dw * 95 / 100 && h <= dh * 92 / 100) add(c, w << 16 | h, "%d x %d", w, h);
    }
    if (!c->n) add(c, 1280 << 16 | 720, "1280 x 720");
}

static void build_choices(int id, choices* c) {
    memset(c, 0, sizeof *c);
    char buf[64];
    switch (id) {
    case MENU_ROW_PRESET:
        add(c, SETTINGS_PRESET_ORIGINAL, "Original");
        add(c, SETTINGS_PRESET_PERFORMANCE, "Performance");
        add(c, SETTINGS_PRESET_BALANCED, "Balanced");
        add(c, SETTINGS_PRESET_QUALITY, "Quality");
        break;
    case MENU_ROW_RENDERER:
        add(c, 1, "GPU (Direct3D 11)");
        add(c, 0, "Software (CPU)");
        break;
    case MENU_ROW_SCALE: {
        int w, h, now = host.auto_scale_now ? host.auto_scale_now() : 0;
        if (now < 1 || now > 8) now = ps2_settings.auto_max;
        scale_pixels(now, &w, &h);
        add(c, SETTINGS_SCALE_AUTO, "Auto - now %dx (%d x %d)", now, w, h);
        for (int n = 1; n <= 8; n++) {
            scale_pixels(n, &w, &h);
            if (n == 1) add(c, 1, "1x - original (%d x %d)", w, h);
            else add(c, n, "%dx (%d x %d)", n, w, h);
        }
        int nn = native_scale();
        scale_pixels(nn, &w, &h);
        add(c, SETTINGS_SCALE_NATIVE, "Native - %dx (%d x %d)", nn, w, h);
        break;
    }
    case MENU_ROW_AUTO_MAX:
        for (int n = 1; n <= 8; n++) {
            int w, h;
            scale_pixels(n, &w, &h);
            add(c, n, "%dx (%d x %d)%s", n, w, h, n == SETTINGS_SCALE_AUTO_MAX ? " - default" : "");
        }
        break;
    case MENU_ROW_SCALER:
        add(c, SETTINGS_SCALER_AUTO, "Automatic (sharp)");
        add(c, SETTINGS_SCALER_LANCZOS, "Lanczos (sharp)");
        add(c, SETTINGS_SCALER_CATMULL, "Catmull-Rom (balanced)");
        add(c, SETTINGS_SCALER_BOX, "Box (area average)");
        add(c, SETTINGS_SCALER_BILINEAR, "Bilinear (soft)");
        add(c, SETTINGS_SCALER_POINT, "Nearest (pixelated)");
        break;
    case MENU_ROW_OUTLINES:
        add(c, 0, "Original (PS2)");
        add(c, 1, "Thin (high resolution)");
        break;
    case MENU_ROW_LOOK:
        add(c, 0, "Modern");
        add(c, 1, "Faithful (PS2 on a TV)");
        break;
    case MENU_ROW_BLUR:
    case MENU_ROW_FLICKER:
    case MENU_ROW_INTERLACE: {
        int def = id == MENU_ROW_BLUR ? 1 : ps2_settings.filters_faithful;
        add(c, -1, "Auto (%s: %s)", ps2_settings.filters_faithful ? "Faithful" : "Modern", def ? "on" : "off");
        add(c, 0, "Off");
        add(c, 1, "On");
        break;
    }
    case MENU_ROW_MODE:
        add(c, SETTINGS_FULLSCREEN_OFF, "Windowed");
        add(c, SETTINGS_FULLSCREEN_BORDERLESS, "Borderless fullscreen");
        add(c, SETTINGS_FULLSCREEN_EXCLUSIVE, "Exclusive fullscreen");
        break;
    case MENU_ROW_WINSIZE: winsize_choices(c); break;
    case MENU_ROW_MONITOR: {
        add(c, 0, "Automatic (where the window is)");
        int n = h_monitors();
        for (int k = 0; k < n && n > 1; k++) {
            int w = 0, h = 0, hz = 0, pri = 0;
            if (host.monitor_info) host.monitor_info(k, &w, &h, &hz, &pri);
            if (hz > 0) add(c, k + 1, "%d: %d x %d, %d Hz%s", k + 1, w, h, hz, pri ? " (main)" : "");
            else add(c, k + 1, "%d: %d x %d%s", k + 1, w, h, pri ? " (main)" : "");
        }
        break;
    }
    case MENU_ROW_ASPECT: aspect_choices(c); break;
    case MENU_ROW_HUD:
        add(c, 0, "At the screen's edges");
        add(c, 1, "Centered (4:3 area)");
        break;
    case MENU_ROW_PACING:
        add(c, SETTINGS_PACING_VSYNC, "VSync");
        add(c, SETTINGS_PACING_VRR, "Variable refresh (G-Sync / FreeSync)");
        add(c, SETTINGS_PACING_DISPLAY, "Display-synced (60 Hz displays)");
        break;
    case MENU_ROW_FPS: case MENU_ROW_BACKGROUND: case MENU_ROW_VIBRATION: case MENU_ROW_INVERT_X: case MENU_ROW_INVERT_Y:
        add(c, 0, "Off");
        add(c, 1, "On");
        break;
    default: break;
    }
    (void)buf;
}

/* the current value of a CHOICE row (matched against its choices) */
static int row_value(int id) {
    switch (id) {
    case MENU_ROW_PRESET: return ps2_settings.preset;
    case MENU_ROW_RENDERER: return ps2_settings.renderer_gpu;
    case MENU_ROW_SCALE: return ps2_settings.internal_scale;
    case MENU_ROW_AUTO_MAX: return ps2_settings.auto_max;
    case MENU_ROW_SCALER: return ps2_settings.scaler;
    case MENU_ROW_OUTLINES: return ps2_settings.outlines_thin;
    case MENU_ROW_LOOK: return ps2_settings.filters_faithful;
    case MENU_ROW_BLUR: return ps2_settings.motion_blur;
    case MENU_ROW_FLICKER: return ps2_settings.flicker_filter;
    case MENU_ROW_INTERLACE: return ps2_settings.interlacing;
    case MENU_ROW_MODE: return h_window_mode();
    case MENU_ROW_WINSIZE: { int w, h; h_output(&w, &h); return w << 16 | h; }
    case MENU_ROW_MONITOR: return ps2_settings.monitor;
    case MENU_ROW_ASPECT: return get("display", "aspect");
    case MENU_ROW_HUD: return ps2_settings.hud_centered;
    case MENU_ROW_PACING: return ps2_settings.pacing;
    case MENU_ROW_FPS: return ps2_settings.show_fps;
    case MENU_ROW_BACKGROUND: return ps2_settings.audio_background;
    case MENU_ROW_VIBRATION: return ps2_settings.vibration;
    case MENU_ROW_INVERT_X: return ps2_settings.invert_x;
    case MENU_ROW_INVERT_Y: return ps2_settings.invert_y;
    case MENU_ROW_MASTER: return ps2_settings.vol_master;
    case MENU_ROW_MUSIC: return ps2_settings.vol_music;
    case MENU_ROW_EFFECTS: return ps2_settings.vol_effects;
    case MENU_ROW_VIB_STRENGTH: return ps2_settings.vibration_strength;
    case MENU_ROW_DEADZONE: return ps2_settings.deadzone;
    default: return 0;
    }
}

/* set a row's value (choice value or slider value) */
static void row_set(int id, int v) {
    switch (id) {
    case MENU_ROW_PRESET: change("graphics", "preset", v); break;
    case MENU_ROW_RENDERER: change("graphics", "renderer", v); break;
    case MENU_ROW_SCALE: change("graphics", "internal_scale", v); break;
    case MENU_ROW_AUTO_MAX: change("graphics", "auto_max", v); break;
    case MENU_ROW_SCALER: change("graphics", "scaler", v); break;
    case MENU_ROW_OUTLINES: change("graphics", "outlines", v); break;
    case MENU_ROW_LOOK: change("graphics", "filters", v); break;
    case MENU_ROW_BLUR: change("graphics", "motion_blur", v); break;
    case MENU_ROW_FLICKER: change("graphics", "flicker_filter", v); break;
    case MENU_ROW_INTERLACE: change("graphics", "interlacing", v); break;
    case MENU_ROW_MODE: change("window", "fullscreen", v); break;
    case MENU_ROW_WINSIZE: if (host.set_client_size) host.set_client_size(v >> 16, v & 0xFFFF); break;
    case MENU_ROW_MONITOR: change("display", "monitor", v); break;
    case MENU_ROW_ASPECT: change("display", "aspect", v); break;
    case MENU_ROW_HUD: change("display", "hud", v); break;
    case MENU_ROW_PACING: change("graphics", "pacing", v); break;
    case MENU_ROW_FPS: change("window", "show_fps", v); break;
    case MENU_ROW_BACKGROUND: change("audio", "background", v); break;
    case MENU_ROW_VIBRATION: change("controls", "vibration", v); break;
    case MENU_ROW_INVERT_X: change("controls", "invert_camera_x", v); break;
    case MENU_ROW_INVERT_Y: change("controls", "invert_camera_y", v); break;
    case MENU_ROW_MASTER: change("audio", "master", v); break;
    case MENU_ROW_MUSIC: change("audio", "music", v); break;
    case MENU_ROW_EFFECTS: change("audio", "effects", v); break;
    case MENU_ROW_VIB_STRENGTH: change("controls", "vibration_strength", v); input_rumble_test(v); break;
    case MENU_ROW_DEADZONE: change("controls", "stick_deadzone", v); break;
    default: break;
    }
}

int menu_row_choice_text(int row_id, int k, char* buf, size_t cap) {
    LOCK();
    choices c;
    build_choices(row_id, &c);
    int ok = k >= 0 && k < c.n;
    snprintf(buf, cap, "%s", ok ? c.text[k] : "");
    UNLOCK();
    return ok;
}

/* ---- rows of a page ------------------------------------------------------------------------------------------ */

static menu_row* row_new(menu_row* rows, int* n, int id, int kind, const char* label, const char* desc) {
    if (*n >= MENU_MAX_ROWS) return NULL;
    menu_row* r = &rows[(*n)++];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->kind = kind;
    r->enabled = 1;
    r->cur = -1;
    snprintf(r->label, sizeof r->label, "%s", label);
    snprintf(r->desc, sizeof r->desc, "%s", desc);
    return r;
}

static void choice_row(menu_row* rows, int* n, int id, const char* label, const char* desc) {
    menu_row* r = row_new(rows, n, id, MENU_ITEM_CHOICE, label, desc);
    if (!r) return;
    choices c;
    build_choices(id, &c);
    int v = row_value(id);
    r->nchoices = c.n;
    for (int k = 0; k < c.n; k++) if (c.val[k] == v) r->cur = k;
    if (r->cur >= 0) snprintf(r->value, sizeof r->value, "%s", c.text[r->cur]);
    r->can_left = r->cur != 0 && c.n > 1;
    r->can_right = r->cur != c.n - 1 && c.n > 1;
}

static void slider_row(menu_row* rows, int* n, int id, const char* label, const char* desc, int lo, int hi, int step,
                       const char* unit) {
    menu_row* r = row_new(rows, n, id, MENU_ITEM_SLIDER, label, desc);
    if (!r) return;
    r->lo = lo; r->hi = hi; r->step = step;
    r->val = row_value(id);
    snprintf(r->value, sizeof r->value, "%d%s", r->val, unit);
    r->can_left = r->val > lo;
    r->can_right = r->val < hi;
}

static void disable(menu_row* r, const char* why) {
    if (!r) return;
    r->enabled = 0;
    r->can_left = r->can_right = 0;
    snprintf(r->note, sizeof r->note, "%s", why);
}

static menu_row* last(menu_row* rows, int n) { return n ? &rows[n - 1] : NULL; }

static void env_note(menu_row* r, const char* section, const char* key) {
    int i = settings_find(section, key);
    if (r && settings_env_locked(i)) {
        char why[MENU_TEXT];
        snprintf(why, sizeof why, "Set by %s (environment)", settings_opt_at(i)->env ? settings_opt_at(i)->env : "the environment");
        disable(r, why);
    }
}

static const char* preset_desc(int p) {
    switch (p) {
    case SETTINGS_PRESET_ORIGINAL:
        return "The PS2's look: the PS2's resolution (512 x 448), 4:3, the TV flicker filter and interlacing, PS2 outlines.";
    case SETTINGS_PRESET_PERFORMANCE:
        return "2x resolution, lowered to 1x while the graphics card cannot keep up. Modern look. For slower graphics cards.";
    case SETTINGS_PRESET_BALANCED:
        return "3x resolution, lowered while the graphics card cannot keep up. Modern look. The default.";
    case SETTINGS_PRESET_QUALITY:
        return "At least your display's resolution, thin high-resolution outlines, Modern look. Needs a fast graphics card.";
    default:
        return "Your own combination of the options below. Choose a preset to set them all at once.";
    }
}

static int build_rows(int page, menu_row* rows) {
    int n = 0;
    menu_row* r;
    int gpu = h_gpu_active();
    const char* gpu_only = "GPU renderer only";
    switch (page) {
    case MENU_PAGE_GRAPHICS:
        choice_row(rows, &n, MENU_ROW_PRESET, "Preset", "");
        r = last(rows, n);
        snprintf(r->desc, sizeof r->desc, "%s", preset_desc(ps2_settings.preset));
        if (r->cur < 0) { snprintf(r->value, sizeof r->value, "Custom"); r->can_left = r->can_right = 1; }
        choice_row(rows, &n, MENU_ROW_RENDERER, "Renderer",
                   "GPU: Direct3D 11, any resolution. Software: the PS2's renderer on the processor, at the PS2's resolution.");
        r = last(rows, n);
        if (!h_gpu_started() && ps2_settings.renderer_gpu != gpu) snprintf(r->note, sizeof r->note, "Applies at the next start");
        else if (!h_gpu_started() && ps2_settings.renderer_gpu) snprintf(r->note, sizeof r->note, "GPU renderer unavailable: see sly2.log");
        env_note(r, "graphics", "renderer");
        choice_row(rows, &n, MENU_ROW_SCALE, "Internal resolution",
                   "The resolution the game is drawn at before it is scaled to your screen: higher is sharper and needs a faster "
                   "graphics card. Auto lowers it while the card cannot keep up; Native matches your window.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        env_note(r, "graphics", "internal_scale");
        if (ps2_settings.internal_scale == SETTINGS_SCALE_AUTO) {
            choice_row(rows, &n, MENU_ROW_AUTO_MAX, "Auto: highest resolution",
                       "The highest resolution Auto uses. Auto goes lower (down to 1x) while the graphics card cannot keep up.");
            if (!gpu) disable(last(rows, n), gpu_only);
        }
        choice_row(rows, &n, MENU_ROW_SCALER, "Scaling filter",
                   "How the picture is scaled to your window. Automatic: sharp when shrinking (Lanczos), smooth when enlarging.");
        if (!gpu) disable(last(rows, n), gpu_only);
        env_note(last(rows, n), "graphics", "scaler");
        choice_row(rows, &n, MENU_ROW_OUTLINES, "Outlines",
                   "Original: the cel-shading outlines one PS2 pixel wide, exactly as on a PS2 (thicker at high resolutions). "
                   "Thin: fine lines drawn at the high resolution.");
        if (!gpu) disable(last(rows, n), gpu_only);
        choice_row(rows, &n, MENU_ROW_LOOK, "Look",
                   "Modern: a sharp progressive picture. Faithful: as a PS2 on a TV (flicker filter, interlacing). "
                   "Sets the three options below when they are on Auto.");
        choice_row(rows, &n, MENU_ROW_BLUR, "Motion blur", "The game's own motion blur (the previous picture blended into the new one).");
        choice_row(rows, &n, MENU_ROW_FLICKER, "TV flicker filter",
                   "The PS2's flicker filter: softens the picture vertically, as the PS2 did for TVs.");
        if (!gpu) disable(last(rows, n), gpu_only);
        choice_row(rows, &n, MENU_ROW_INTERLACE, "Interlacing",
                   "On: fields as the PS2 sent them to a TV (half the lines per picture). Off: every line in every picture.");
        if (!gpu) disable(last(rows, n), gpu_only);
        break;
    case MENU_PAGE_DISPLAY: {
        int mode = h_window_mode();
        choice_row(rows, &n, MENU_ROW_MODE, "Display mode",
                   "Windowed, borderless fullscreen (fast Alt+Tab) or exclusive fullscreen. Alt+Enter or F11 switch too.");
        r = last(rows, n);
        if (mode == SETTINGS_FULLSCREEN_EXCLUSIVE && ps2_settings.win_fullscreen != SETTINGS_FULLSCREEN_EXCLUSIVE)
            snprintf(r->note, sizeof r->note, "Exclusive fullscreen is not available: borderless is used");
        choice_row(rows, &n, MENU_ROW_WINSIZE, "Window size", "The size of the game's picture area in windowed mode.");
        r = last(rows, n);
        if (r && r->cur < 0) {
            int w, h;
            h_output(&w, &h);
            snprintf(r->value, sizeof r->value, "%d x %d", w, h);
            r->can_left = r->can_right = r->nchoices > 0;
        }
        if (mode != SETTINGS_FULLSCREEN_OFF) disable(r, "Windowed mode only");
        choice_row(rows, &n, MENU_ROW_MONITOR, "Monitor", "The monitor for fullscreen (and the window). Automatic: where the window is.");
        if (h_monitors() < 2) disable(last(rows, n), "Only one monitor is connected");
        choice_row(rows, &n, MENU_ROW_ASPECT, "Aspect ratio",
                   "Widescreen shows more of the world at the sides (the HUD and menus keep their shape). 4:3 is the PS2's picture.");
        env_note(last(rows, n), "display", "aspect");
        choice_row(rows, &n, MENU_ROW_HUD, "HUD layout",
                   "Widescreen: the health meter and counters at the screen's edges, or kept in the centre 4:3 area as on a PS2.");
        if (ps2_settings.aspect_w * 3 == ps2_settings.aspect_h * 4) disable(last(rows, n), "Widescreen only");
        env_note(last(rows, n), "display", "hud");
        choice_row(rows, &n, MENU_ROW_PACING, "Frame pacing",
                   "VSync: every picture on a refresh of the display. Variable refresh: G-Sync / FreeSync displays. "
                   "Display-synced: the game follows a 60 Hz display exactly.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        else if (!pacing_apply_mode(-1)) snprintf(r->note, sizeof r->note, "Applies at the next start");
        env_note(r, "graphics", "pacing");
        choice_row(rows, &n, MENU_ROW_FPS, "Show frame rate", "The frame rate in the top left corner of the picture and in the title bar.");
        env_note(last(rows, n), "window", "show_fps");
        break;
    }
    case MENU_PAGE_AUDIO:
        slider_row(rows, &n, MENU_ROW_MASTER, "Master volume", "The volume of everything the game plays.", 0, 100, 5, "%");
        slider_row(rows, &n, MENU_ROW_MUSIC, "Music volume", "The game's music (the same setting as in the game's own Options).",
                   0, 100, 5, "%");
        if (host.in_game && !host.in_game()) snprintf(last(rows, n)->note, sizeof last(rows, n)->note, "Applies when a game is loaded");
        slider_row(rows, &n, MENU_ROW_EFFECTS, "Effects volume",
                   "Sound effects and voices (the same setting as in the game's own Options).", 0, 100, 5, "%");
        if (host.in_game && !host.in_game()) snprintf(last(rows, n)->note, sizeof last(rows, n)->note, "Applies when a game is loaded");
        choice_row(rows, &n, MENU_ROW_BACKGROUND, "Sound in the background",
                   "Keep playing sound while another window is active. Off: silent until you come back.");
        break;
    case MENU_PAGE_CONTROLS:
        choice_row(rows, &n, MENU_ROW_VIBRATION, "Vibration", "Controller vibration.");
        slider_row(rows, &n, MENU_ROW_VIB_STRENGTH, "Vibration strength", "How strongly the controller vibrates.", 0, 100, 10, "%");
        if (!ps2_settings.vibration) disable(last(rows, n), "Vibration is off");
        choice_row(rows, &n, MENU_ROW_INVERT_X, "Invert camera horizontally", "The right stick turns the camera the other way.");
        choice_row(rows, &n, MENU_ROW_INVERT_Y, "Invert camera vertically", "The right stick tilts the camera the other way.");
        slider_row(rows, &n, MENU_ROW_DEADZONE, "Stick dead zone",
                   "How far a stick must move before the game notices. Raise it if Sly creeps with the stick let go.", 0, 50, 2, "%");
        row_new(rows, &n, MENU_ROW_KEYS, MENU_ITEM_ACTION, "Keyboard controls", "Choose the keys for each of Sly's moves.");
        row_new(rows, &n, MENU_ROW_PAD, MENU_ITEM_ACTION, "Controller buttons", "Choose the controller button for each of Sly's moves.");
        break;
    case MENU_PAGE_KEYS:
    case MENU_PAGE_PAD: {
        int dev = page == MENU_PAGE_KEYS ? 0 : 1, cnt = input_bind_count(dev);
        for (int a = 0; a < cnt && n < MENU_MAX_ROWS - 1; a++) {
            r = row_new(rows, &n, MENU_ROW_BIND0 + a, MENU_ITEM_BIND, input_bind_name(dev, a),
                        dev ? "Select, then press the controller button for this move."
                            : "Select, then press the key for this move.");
            input_bind_text(dev, a, r->value, sizeof r->value);
        }
        row_new(rows, &n, MENU_ROW_BIND_RESET, MENU_ITEM_ACTION, "Reset to defaults",
                dev ? "The controller's original buttons." : "The original keys (notes in README.txt).");
        return n;
    }
    default: break;
    }
    row_new(rows, &n, MENU_ROW_RESET, MENU_ITEM_ACTION, "Reset to defaults", "Every option on this page back to its default.");
    return n;
}

/* ---- navigation ---------------------------------------------------------------------------------------------- */

static menu_row rows_buf[MENU_MAX_ROWS];

static int rows_now(void) { return build_rows(st.page, rows_buf); }

static void keep_visible(int n) {
    int vis = st.visible > 0 ? st.visible : 1;
    if (st.sel >= n) st.sel = n - 1;
    if (st.sel < 0) st.sel = 0;
    if (st.sel < st.scroll) st.scroll = st.sel;
    if (st.sel >= st.scroll + vis) st.scroll = st.sel - vis + 1;
    if (st.scroll > n - vis) st.scroll = n - vis;
    if (st.scroll < 0) st.scroll = 0;
}

static void set_page(int page) {
    st.sel_of[st.page] = st.sel;
    st.page = page;
    st.sel = page < MENU_PAGE_N ? st.sel_of[page] : 0;
    st.scroll = 0;
    st.capture = st.confirm_reset = 0;
    keep_visible(rows_now());
}

static const char* page_section(int page) {
    switch (page) {
    case MENU_PAGE_GRAPHICS: return "graphics";
    case MENU_PAGE_DISPLAY: return "display";
    case MENU_PAGE_AUDIO: return "audio";
    case MENU_PAGE_CONTROLS: return "controls";
    default: return NULL;
    }
}

static void reset_page(void) {
    if (st.page == MENU_PAGE_KEYS || st.page == MENU_PAGE_PAD) { input_bind_reset(st.page == MENU_PAGE_PAD); return; }
    const char* sec = page_section(st.page);
    if (!sec) return;
    if (st.page == MENU_PAGE_DISPLAY) {          /* [display] + the [window] options (not the placement) */
        settings_reset_section("display");
        if (!settings_env_locked(settings_find("window", "show_fps")) && ps2_settings.show_fps) change("window", "show_fps", 0);
        if (ps2_settings.pacing != SETTINGS_PACING_VSYNC && !settings_env_locked(settings_find("graphics", "pacing")))
            change("graphics", "pacing", SETTINGS_PACING_VSYNC);
        return;
    }
    if (st.page == MENU_PAGE_GRAPHICS) {         /* the default preset sets every graphics option */
        if (!settings_env_locked(settings_find("graphics", "renderer")) && !ps2_settings.renderer_gpu) change("graphics", "renderer", 1);
        if (ps2_settings.aspect_w * 3 == ps2_settings.aspect_h * 4 && ps2_settings.preset == SETTINGS_PRESET_ORIGINAL)
            change("display", "aspect", 16 << 16 | 9);
        change("graphics", "preset", SETTINGS_PRESET_BALANCED);
        return;
    }
    settings_reset_section(sec);
}

static void step_choice(menu_row* r, int dir, int wrap) {
    choices c;
    build_choices(r->id, &c);
    if (!c.n) return;
    int k = r->cur;
    if (k < 0) k = dir > 0 ? 0 : c.n - 1;
    else {
        k += dir;
        if (k < 0) { if (!wrap) return; k = c.n - 1; }
        if (k >= c.n) { if (!wrap) return; k = 0; }
    }
    row_set(r->id, c.val[k]);
}

static void step_slider(menu_row* r, int dir) {
    int v = r->val + dir * r->step;
    if (v < r->lo) v = r->lo;
    if (v > r->hi) v = r->hi;
    if (v != r->val) row_set(r->id, v);
}

static void activate(menu_row* r) {
    switch (r->id) {
    case MENU_ROW_RESET: case MENU_ROW_BIND_RESET: st.confirm_reset = 1; break;
    case MENU_ROW_KEYS: set_page(MENU_PAGE_KEYS); break;
    case MENU_ROW_PAD: set_page(MENU_PAGE_PAD); break;
    default: break;
    }
}

static void do_close(void) {
    if (!st.open) return;
    st.open = 0;
    st.capture = st.confirm_reset = 0;
    if (host.closed) host.closed();
}

/* one event, lock held */
static int nav(menu_nav ev) {
    if (!st.open) return 0;
    int n = rows_now();
    menu_row* r = st.sel >= 0 && st.sel < n ? &rows_buf[st.sel] : NULL;
    if (st.capture) {                            /* waiting for a key: only Back cancels */
        if (ev == MENU_NAV_BACK || ev == MENU_NAV_CLOSE) st.capture = 0;
        return 1;
    }
    if (st.confirm_reset) {
        if (ev == MENU_NAV_CONFIRM) { st.confirm_reset = 0; reset_page(); }
        else if (ev == MENU_NAV_BACK || ev == MENU_NAV_CLOSE || ev == MENU_NAV_DEFAULTS) st.confirm_reset = 0;
        return 1;
    }
    switch (ev) {
    case MENU_NAV_UP: st.sel = st.sel > 0 ? st.sel - 1 : n - 1; break;
    case MENU_NAV_DOWN: st.sel = st.sel < n - 1 ? st.sel + 1 : 0; break;
    case MENU_NAV_PAGE_UP: st.sel -= st.visible > 1 ? st.visible - 1 : 1; if (st.sel < 0) st.sel = 0; break;
    case MENU_NAV_PAGE_DOWN: st.sel += st.visible > 1 ? st.visible - 1 : 1; if (st.sel >= n) st.sel = n - 1; break;
    case MENU_NAV_LEFT:
    case MENU_NAV_RIGHT: {
        int dir = ev == MENU_NAV_RIGHT ? 1 : -1;
        if (!r || !r->enabled) break;
        if (r->kind == MENU_ITEM_CHOICE) step_choice(r, dir, 0);
        else if (r->kind == MENU_ITEM_SLIDER) step_slider(r, dir);
        break;
    }
    case MENU_NAV_CONFIRM:
        if (!r || !r->enabled) break;
        if (r->kind == MENU_ITEM_CHOICE) step_choice(r, 1, 1);
        else if (r->kind == MENU_ITEM_ACTION) activate(r);
        else if (r->kind == MENU_ITEM_BIND) st.capture = 1;
        break;
    case MENU_NAV_BACK:
        if (st.page == MENU_PAGE_KEYS || st.page == MENU_PAGE_PAD) {
            int back_row = st.page == MENU_PAGE_KEYS ? MENU_ROW_KEYS : MENU_ROW_PAD;
            set_page(MENU_PAGE_CONTROLS);
            int m = rows_now();
            for (int k = 0; k < m; k++) if (rows_buf[k].id == back_row) st.sel = k;
        } else {
            do_close();
        }
        break;
    case MENU_NAV_TAB_NEXT:
    case MENU_NAV_TAB_PREV: {
        int p = st.page >= MENU_PAGE_N ? MENU_PAGE_CONTROLS : st.page;
        p = (p + (ev == MENU_NAV_TAB_NEXT ? 1 : MENU_PAGE_N - 1)) % MENU_PAGE_N;
        set_page(p);
        break;
    }
    case MENU_NAV_DEFAULTS: st.confirm_reset = 1; break;
    case MENU_NAV_CLOSE: do_close(); break;
    default: return 0;
    }
    if (st.open) keep_visible(rows_now());
    return 1;
}

void menu_open(void) {
    LOCK();
    if (!st.open) {
        st.open = 1;
        st.capture = st.confirm_reset = 0;
        if (st.page >= MENU_PAGE_N) st.page = MENU_PAGE_CONTROLS;
        keep_visible(rows_now());
        st.serial++;
    }
    UNLOCK();
}

void menu_close(void) {
    LOCK();
    do_close();
    st.serial++;
    UNLOCK();
}

int menu_is_open(void) { return *(volatile int*)&st.open; }
int menu_capturing(void) { return st.open && *(volatile int*)&st.capture; }
int menu_device(void) { return st.device; }

int menu_nav_event(menu_nav ev, int dev) {
    LOCK();
    if (dev != st.device) { st.device = dev; st.serial++; }
    int used = nav(ev);
    if (used) st.serial++;
    UNLOCK();
    return used;
}

/* keyboard: arrows / WASD move, Enter / Space confirm, Esc / Backspace back, Tab / Shift+Tab or Q / E pages,
 * Page Up / Down, Home / End, R or Delete: defaults; while capturing, any key binds (Esc cancels) */
int menu_key(unsigned vk, int shift) {
    LOCK();
    int used = 0;
    if (st.open) {
        if (st.device != MENU_DEV_KEYBOARD) { st.device = MENU_DEV_KEYBOARD; st.serial++; }
        if (st.capture) {
            if (vk == VK_ESCAPE) st.capture = 0;
            else if (st.page == MENU_PAGE_KEYS) {
                int n = rows_now();
                if (st.sel < n && rows_buf[st.sel].kind == MENU_ITEM_BIND) input_bind_set(0, rows_buf[st.sel].id - MENU_ROW_BIND0, (int)vk);
                st.capture = 0;
            }
            st.serial++;
            UNLOCK();
            return 1;
        }
        menu_nav ev = MENU_NAV_NONE;
        switch (vk) {
        case VK_UP: case 'W': ev = MENU_NAV_UP; break;
        case VK_DOWN: case 'S': ev = MENU_NAV_DOWN; break;
        case VK_LEFT: case 'A': ev = MENU_NAV_LEFT; break;
        case VK_RIGHT: case 'D': ev = MENU_NAV_RIGHT; break;
        case VK_RETURN: case VK_SPACE: ev = MENU_NAV_CONFIRM; break;
        case VK_ESCAPE: case VK_BACK: ev = MENU_NAV_BACK; break;
        case VK_TAB: ev = (shift & 1) ? MENU_NAV_TAB_PREV : MENU_NAV_TAB_NEXT; break;
        case 'Q': ev = MENU_NAV_TAB_PREV; break;
        case 'E': ev = MENU_NAV_TAB_NEXT; break;
        case VK_PRIOR: ev = MENU_NAV_PAGE_UP; break;
        case VK_NEXT: ev = MENU_NAV_PAGE_DOWN; break;
        case VK_HOME: st.sel = 0; ev = MENU_NAV_NONE; used = 1; break;
        case VK_END: st.sel = MENU_MAX_ROWS; ev = MENU_NAV_NONE; used = 1; break;
        case 'R': case VK_DELETE: ev = MENU_NAV_DEFAULTS; break;
        default: break;
        }
        if (ev != MENU_NAV_NONE) used = nav(ev);
        else if (used) keep_visible(rows_now());
        if (used) st.serial++;
    }
    UNLOCK();
    return used;
}

void menu_capture_button(int code) {
    LOCK();
    if (st.open && st.capture && st.page == MENU_PAGE_PAD) {
        int n = rows_now();
        if (st.sel < n && rows_buf[st.sel].kind == MENU_ITEM_BIND) input_bind_set(1, rows_buf[st.sel].id - MENU_ROW_BIND0, code);
        st.capture = 0;
        st.serial++;
    }
    UNLOCK();
}

uint32_t menu_version(void) {
    return (uint32_t)st.serial * 2654435761u ^ (uint32_t)settings_serial ^ (uint32_t)st.open << 31;
}

/* ---- layout ------------------------------------------------------------------------------------------------- */

static int I(float v) { return (int)(v + 0.5f); }

void menu_compute_layout(int cw, int ch, int nrows, int scroll, menu_layout* L) {
    memset(L, 0, sizeof *L);
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    float u = (float)ch / 1080.0f;               /* sizes follow the height ... */
    if (u * 1000.0f > (float)cw * 0.94f) u = (float)cw * 0.94f / 1000.0f;   /* ... unless the window is narrow */
    if (u < 0.30f) u = 0.30f;
    L->u = u;
    int pw = I(1000 * u), ph = I(860 * u);
    if (pw > cw) pw = cw;
    if (ph > ch) ph = ch;
    L->panel = (menu_rect){ (cw - pw) / 2, (ch - ph) / 2, pw, ph };
    int pad = I(36 * u);
    int x0 = L->panel.x + pad, iw = pw - 2 * pad;
    L->header = (menu_rect){ x0, L->panel.y + I(26 * u), iw, I(64 * u) };
    int ty = L->header.y + L->header.h + I(8 * u), th = I(50 * u), hint = I(64 * u);
    int tw = (iw - 2 * hint) / MENU_PAGE_N;
    for (int k = 0; k < MENU_PAGE_N; k++) L->tabs[k] = (menu_rect){ x0 + hint + k * tw, ty, tw, th };
    L->tab_hint[0] = (menu_rect){ x0, ty, hint, th };
    L->tab_hint[1] = (menu_rect){ x0 + iw - hint, ty, hint, th };
    L->footer = (menu_rect){ x0, L->panel.y + ph - I(64 * u), iw, I(44 * u) };
    L->desc = (menu_rect){ x0, L->footer.y - I(108 * u), iw, I(96 * u) };
    int ly = ty + th + I(18 * u);
    L->list = (menu_rect){ x0, ly, iw, L->desc.y - I(10 * u) - ly };
    L->row_h = I(52 * u);
    if (L->row_h < 8) L->row_h = 8;
    L->visible = L->list.h / L->row_h;
    if (L->visible < 1) L->visible = 1;
    L->value_x = x0 + iw * 44 / 100;
    int arrow = I(40 * u), sbw = nrows > L->visible ? I(14 * u) : 0;
    if (sbw) L->scrollbar = (menu_rect){ x0 + iw - sbw, ly, sbw, L->visible * L->row_h - I(4 * u) };
    for (int k = 0; k < L->visible && scroll + k < nrows && k < MENU_MAX_ROWS; k++) {
        menu_rect r = { x0, ly + k * L->row_h, iw - (sbw ? sbw + I(10 * u) : 0), L->row_h - I(4 * u) };
        L->rows[k] = r;
        L->left[k] = (menu_rect){ L->value_x, r.y, arrow, r.h };
        L->right[k] = (menu_rect){ r.x + r.w - arrow - I(8 * u), r.y, arrow, r.h };
    }
    /* footer buttons, right to left: back / close, reset to defaults, close (mouse targets with their hints) */
    int bw = I(230 * u), gap = I(14 * u);
    for (int k = 0; k < 3; k++) L->btn[k] = (menu_rect){ x0 + iw - (k + 1) * bw - k * gap, L->footer.y, bw, L->footer.h };
    /* the reset dialog, centred on the panel */
    int dw = I(620 * u), dh = I(220 * u);
    L->dlg = (menu_rect){ L->panel.x + (pw - dw) / 2, L->panel.y + (ph - dh) / 2, dw, dh };
    int bbw = I(240 * u), bbh = I(56 * u), by = L->dlg.y + dh - bbh - I(26 * u);
    L->dlg_yes = (menu_rect){ L->dlg.x + dw / 2 - bbw - I(12 * u), by, bbw, bbh };
    L->dlg_no = (menu_rect){ L->dlg.x + dw / 2 + I(12 * u), by, bbw, bbh };
}

static int inside(menu_rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static void layout_now(menu_layout* L, int n) {
    menu_compute_layout(st.cw > 0 ? st.cw : 1920, st.ch > 0 ? st.ch : 1080, n, st.scroll, L);
}

void menu_mouse_move(int x, int y) {
    LOCK();
    if (st.open) {
        int n = rows_now();
        menu_layout L;
        layout_now(&L, n);
        int ht = -1, hr = -1, hb = -1;
        for (int k = 0; k < MENU_PAGE_N; k++) if (inside(L.tabs[k], x, y)) ht = k;
        for (int k = 0; k < L.visible && st.scroll + k < n; k++) if (inside(L.rows[k], x, y)) hr = st.scroll + k;
        for (int k = 0; k < 3; k++) if (inside(L.btn[k], x, y)) hb = k;
        if (st.confirm_reset) hb = inside(L.dlg_yes, x, y) ? 3 : inside(L.dlg_no, x, y) ? 4 : -1;
        for (int k = 0; k < 2; k++) if (inside(L.tab_hint[k], x, y)) hb = 5 + k;
        if (ht != st.hover_tab || hr != st.hover_row || hb != st.hover_btn || (hr >= 0 && hr != st.sel && !st.capture && !st.confirm_reset)) {
            st.hover_tab = ht; st.hover_row = hr; st.hover_btn = hb;
            if (hr >= 0 && !st.capture && !st.confirm_reset) st.sel = hr;
            if (st.device != MENU_DEV_KEYBOARD) st.device = MENU_DEV_KEYBOARD;
            st.serial++;
        }
    }
    UNLOCK();
}

void menu_mouse_button(int x, int y, int button) {
    LOCK();
    if (st.open) {
        st.device = MENU_DEV_KEYBOARD;
        int n = rows_now();
        menu_layout L;
        layout_now(&L, n);
        if (button == 2) nav(MENU_NAV_BACK);
        else if (st.confirm_reset) {             /* the dialog: Reset, or anywhere else cancels */
            nav(inside(L.dlg_yes, x, y) ? MENU_NAV_CONFIRM : MENU_NAV_BACK);
        } else if (st.capture) {
            nav(MENU_NAV_BACK);
        } else {
            int done = 0;
            for (int k = 0; k < MENU_PAGE_N && !done; k++)
                if (inside(L.tabs[k], x, y)) { if (k != st.page) set_page(k); done = 1; }
            for (int k = 0; k < 2 && !done; k++)
                if (inside(L.tab_hint[k], x, y)) { nav(k ? MENU_NAV_TAB_NEXT : MENU_NAV_TAB_PREV); done = 1; }
            for (int k = 0; k < L.visible && st.scroll + k < n && !done; k++) {
                if (!inside(L.rows[k], x, y)) continue;
                st.sel = st.scroll + k;
                menu_row* r = &rows_buf[st.sel];
                if (inside(L.left[k], x, y) && (r->kind == MENU_ITEM_CHOICE || r->kind == MENU_ITEM_SLIDER)) nav(MENU_NAV_LEFT);
                else if (inside(L.right[k], x, y) && (r->kind == MENU_ITEM_CHOICE || r->kind == MENU_ITEM_SLIDER)) nav(MENU_NAV_RIGHT);
                else if (r->kind == MENU_ITEM_SLIDER && x >= L.value_x) {   /* a click on the bar sets the value */
                    int x0 = L.left[k].x + L.left[k].w, x1 = L.right[k].x;
                    if (x1 > x0 && r->enabled) {
                        int v = r->lo + (int)((double)(x - x0) * (r->hi - r->lo) / (x1 - x0) + 0.5);
                        v = (v + r->step / 2) / r->step * r->step;
                        if (v < r->lo) v = r->lo;
                        if (v > r->hi) v = r->hi;
                        row_set(r->id, v);
                    }
                } else nav(MENU_NAV_CONFIRM);
                done = 1;
            }
            if (!done && inside(L.btn[0], x, y)) nav(MENU_NAV_BACK);
            else if (!done && inside(L.btn[1], x, y)) nav(MENU_NAV_DEFAULTS);
            else if (!done && inside(L.btn[2], x, y)) nav(MENU_NAV_CLOSE);
        }
        if (st.open) keep_visible(rows_now());
        st.serial++;
    }
    UNLOCK();
}

void menu_mouse_wheel(int x, int y, int delta) {
    (void)x; (void)y;
    LOCK();
    if (st.open && !st.capture && !st.confirm_reset) {
        nav(delta > 0 ? MENU_NAV_UP : MENU_NAV_DOWN);
        st.serial++;
    }
    UNLOCK();
}

/* ---- view ---------------------------------------------------------------------------------------------------- */

void menu_get_view(menu_view* v, int cw, int ch) {
    LOCK();
    memset(v, 0, sizeof *v);
    if (cw != st.cw || ch != st.ch) { st.cw = cw; st.ch = ch; }
    v->open = st.open;
    v->nrows = build_rows(st.page, v->rows);
    menu_layout L;
    menu_compute_layout(cw, ch, v->nrows, 0, &L);
    st.visible = L.visible;
    keep_visible(v->nrows);
    v->page = st.page;
    v->sel = st.sel;
    v->scroll = st.scroll;
    v->capture = st.capture;
    v->confirm_reset = st.confirm_reset;
    v->device = st.device;
    v->hover_tab = st.hover_tab;
    v->hover_row = st.hover_row;
    v->hover_btn = st.hover_btn;
    snprintf(v->title, sizeof v->title, "PC Settings");
    if (st.page >= MENU_PAGE_N) snprintf(v->page_title, sizeof v->page_title, "%s", st.page == MENU_PAGE_KEYS ? "Keyboard controls" : "Controller buttons");
    UNLOCK();
}
