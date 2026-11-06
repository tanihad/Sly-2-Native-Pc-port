/* The Options menu's model: pages, rows, values, navigation, layout (menu.h). */
#include "menu.h"
#include "menu_ext.h"
#include "input.h"
#include "settings.h"
#include "gfx.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

extern double frame_rate_display_hz(void) __attribute__((weak));   /* (pc-fps) runtime/frame_rate.c: the display's refresh */
extern int gpu_vrr_status(char* buf, int cap) __attribute__((weak));  /* (pc-fps) gpu/gpu_vrr.c: GPU_VRR_* + text */

static SRWLOCK lock = SRWLOCK_INIT;
static menu_host host;
static struct {
    int open, page, sel, scroll, capture, device;
    int bind_slot;                       /* binding pages: the column (0 / 1) left / right picks */
    int hover_tab, hover_row, hover_btn;
    int visible;                         /* rows that fit (from the last layout) */
    int cw, ch;                          /* client size of the last view */
    uint32_t serial;                     /* bumped by every change of the state */
    int sel_of[MENU_PAGE_ALL];           /* the row each page had selected */
    /* (pc-polish) dialogs, staged changes */
    int dialog, dialog_only;             /* MENU_DLG_*; dialog_only: opened just for it (no panel; closing it closes the menu) */
    int dlg_focus;                       /* (menu) the focused button: 0 yes, 1 alt, 2 no */
    int close_after_keep;                /* (menu) Apply chosen when leaving: close once the display settings are kept */
    unsigned long long keep_deadline;    /* MENU_DLG_KEEP: revert at this time */
    int keep_secs;                       /* the seconds shown last (redraw when it changes) */
    int prev_mode, prev_res, prev_monitor, prev_win;   /* what Revert restores */
    /* (pc-menu2) animations and mouse drags */
    float scroll_f, sel_f;               /* the list's scroll / the selection band as shown (rows) */
    double anim_t;                       /* menu_animate's last time (s; 0: snap) */
    int drag, drag_id;                   /* 1 a slider (its row id), 2 the scroll bar */
    float drag_off;                      /* scroll bar: where it was grabbed (rows) */
} st = { .visible = 10, .hover_tab = -1, .hover_row = -1, .hover_btn = -1 };

/* (menu) every change made in the menu is staged until Apply: options of the settings table (their player's own values,
 * settings_own: the look is staged like any option), the controls configuration (input.c's) and the window size */
static struct {
    unsigned char has[SETTINGS_MAX_OPTS];
    int val[SETTINGS_MAX_OPTS];
    int ctl_has;
    input_config ctl;
    int win_has, win_val;
} sg;

static unsigned long long now_ms(void);

/* (pc-menu2) when the last input reached the menu (QPC seconds; the overlay's latency statistics, PS2_MENU_STATS) */
static volatile double input_t;
static void note_input(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    input_t = (double)t.QuadPart / (double)f.QuadPart;
}
double menu_last_input_time(void) { return input_t; }

#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)

void menu_set_host(const menu_host* h) {
    LOCK();
    if (h) host = *h; else memset(&host, 0, sizeof host);
    st.serial++;
    UNLOCK();
}

static const char* const page_names[MENU_PAGE_ALL] = { "Graphics", "Display", "Audio", "Controls", "Keyboard", "Controller",
                                                        "Advanced graphics", "Advanced controls" };
const char* menu_page_name(int page) { return page >= 0 && page < MENU_PAGE_ALL ? page_names[page] : ""; }
int menu_page_parent(int page) {
    if (page == MENU_PAGE_GFX_ADV) return MENU_PAGE_GRAPHICS;
    if (page >= MENU_PAGE_N) return MENU_PAGE_CONTROLS;
    return page;
}

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
static unsigned long long now_ms(void) { return host.now_ms ? host.now_ms() : GetTickCount64(); }

/* ---- staged values (Apply) ---------------------------------------------------------------------------------- */

static int m_opt(const char* section, const char* key) { return settings_find(section, key); }
/* (pc-menu2) every change made in the menu is the player's choice: recorded in [chosen] (new defaults never override it) */
static int m_change_i(int i, int v) {
    int ok = settings_change(i, v, 1);
    if (ok) settings_mark_chosen(i);
    return ok;
}
static int m_change(const char* section, const char* key, int v) { return m_change_i(m_opt(section, key), v); }

static int cur_winsize(void) { int w, h; h_output(&w, &h); return w << 16 | h; }
static int own_val(int i) { return settings_own(&ps2_settings, i); }
/* option i as the menu shows it: the staged value, else the player's own value in effect */
static int sv(int i) { return i >= 0 && i < SETTINGS_MAX_OPTS && sg.has[i] ? sg.val[i] : own_val(i); }
static int svk(const char* section, const char* key) { return sv(m_opt(section, key)); }
static void stage_opt(int i, int v) {
    if (i < 0 || i >= SETTINGS_MAX_OPTS || !settings_valid(i, v) || settings_env_locked(i)) return;
    sg.has[i] = v != own_val(i);         /* (back to the value in effect: nothing to apply) */
    sg.val[i] = v;
}
static void stage_key(const char* section, const char* key, int v) { stage_opt(m_opt(section, key), v); }
static void ctl_now(input_config* c) { input_config_get(c); }
/* the controls as the menu shows them */
static void ctl_view(input_config* c) { if (sg.ctl_has) *c = sg.ctl; else ctl_now(c); }
/* the differences between two configurations, counted as rows (a binding: one per action and page) */
static int ctl_diff(const input_config* a, const input_config* b, int part) {   /* part: 0 all, 1 options, 2 keys, 3 pad */
    int n = 0;
    if (part <= 1) {
        n += a->vibration != b->vibration;
        n += a->vibration_strength != b->vibration_strength;
        n += a->invert_camera_x != b->invert_camera_x || a->invert_camera_y != b->invert_camera_y;
        n += a->deadzone_left != b->deadzone_left;
        n += a->deadzone_right != b->deadzone_right;
        n += a->lightbar_on != b->lightbar_on;
    }
    if (part == 0 || part == 2) for (int k = 0; k < INPUT_ACT_COUNT; k++) n += memcmp(a->key[k], b->key[k], sizeof a->key[k]) != 0;
    if (part == 0 || part == 3) for (int k = 0; k < INPUT_ACT_PAD_COUNT; k++) n += memcmp(a->pad[k], b->pad[k], sizeof a->pad[k]) != 0;
    return n;
}
static void stage_ctl(const input_config* c) {
    input_config now;
    ctl_now(&now);
    sg.ctl = *c;
    sg.ctl_has = ctl_diff(c, &now, 0) != 0;
}
static void stage_winsize(int v) { sg.win_has = v != cur_winsize(); sg.win_val = v; }

/* the number of changes not applied yet */
static int pending_count(void) {
    int n = 0;
    for (int i = 0; i < SETTINGS_MAX_OPTS && i < settings_count(); i++) n += sg.has[i] && sg.val[i] != own_val(i);
    if (sg.ctl_has) { input_config now; ctl_now(&now); n += ctl_diff(&sg.ctl, &now, 0); }
    n += sg.win_has && sg.win_val != cur_winsize();
    return n;
}
static int pending_any(void) { return pending_count() > 0; }
static void discard_pending(void) { memset(&sg, 0, sizeof sg); }

/* The settings as the staged changes would make them: PO the player's own options (the look aside), PE what would be in
 * effect (the Retro look's original values over PO). Refreshed before the rows are built (lock held). */
static ps2_settings_t PO, PE;
static void preview(void) {
    settings_own_copy(&ps2_settings, &PO);
    int li = m_opt("graphics", "look");
    for (int i = 0; i < settings_count() && i < SETTINGS_MAX_OPTS; i++)
        if (sg.has[i] && i != li) settings_put(&PO, i, sg.val[i]);
    PO.look = SETTINGS_LOOK_FAITHFUL;
    PO.preset = settings_detect_preset(&PO);
    PE = PO;
    settings_set_look(&PE, sv(li));
}
static int retro_view(void) { return PE.look == SETTINGS_LOOK_RETRO; }

/* the display mode / resolution / monitor the rows show: the staged value, else the setting in effect */
static int eff_mode(void) { return svk("display", "mode"); }
static int eff_res(void) { return svk("display", "resolution"); }
static int eff_winsize(void) { return sg.win_has ? sg.win_val : cur_winsize(); }
static int m_get(const char* section, const char* key) { return settings_get(&PE, m_opt(section, key)); }

/* the internal resolution's real size at scale n: rows 448 n; columns 512 n at 4:3, at a wider aspect
 * 512 round(n W) (W = aspect / (4/3): the anamorphic display buffers, notes/widescreen.md) from 2x on */
static void scale_pixels(int n, int* w, int* h) {
    double wide = (double)PE.aspect_w / (double)PE.aspect_h / (4.0 / 3.0);
    int cols = n;
    if (n >= 2 && wide > 1.001) cols = (int)(n * wide + 0.5);
    *w = 512 * cols;
    *h = 448 * n;
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
    /* (values reduced, as settings_parse_aspect stores them: 16:10 = 8:5, 21:9 = 7:3) */
    add(c, 4 << 16 | 3, "4:3 (original)");
    add(c, 8 << 16 | 5, "16:10");
    add(c, 16 << 16 | 9, "16:9 (widescreen)");
    int dw, dh, hz;
    h_display(&dw, &dh, &hz);
    int aw = dw, ah = dh;
    reduce(&aw, &ah);
    double d = (double)dw / dh;
    if (d >= 21.0 / 9.0 - 0.02) add(c, 7 << 16 | 3, "21:9 (ultrawide)");
    if (d >= 32.0 / 9.0 - 0.02) add(c, 32 << 16 | 9, "32:9 (super ultrawide)");
    int own = aw << 16 | ah, found = 0;
    for (int k = 0; k < c->n; k++) found |= c->val[k] == own;
    if (!found && d >= 1.0 && d <= 4.0 && aw < 1000) add(c, own, "%d:%d (your display)", aw, ah);
    int cur = m_get("display", "aspect");
    found = 0;
    for (int k = 0; k < c->n; k++) found |= c->val[k] == cur;
    if (!found) add(c, cur, "%d:%d", cur >> 16, cur & 0xFFFF);
}

static void winsize_choices(choices* c) {
    static const int heights[] = { 540, 720, 768, 900, 1080, 1200, 1440, 1600, 1800, 2160, 2880 };
    int dw, dh, hz;
    h_display(&dw, &dh, &hz);
    double a = (double)PE.aspect_w / PE.aspect_h;
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
    case MENU_ROW_PRESET:                /* (menu) Quality; the original look is Look = Retro */
        add(c, SETTINGS_PRESET_LOW, "Low");
        add(c, SETTINGS_PRESET_MEDIUM, "Medium");
        add(c, SETTINGS_PRESET_HIGH, "High");
        add(c, SETTINGS_PRESET_ULTRA, "Ultra");
        break;
    case MENU_ROW_GLOOK:                 /* (menu) Reimagined: offered once there is something behind it (or sly2.ini says so) */
        add(c, SETTINGS_LOOK_RETRO, "Retro");
        add(c, SETTINGS_LOOK_FAITHFUL, "Faithful");
        if (svk("graphics", "look") == SETTINGS_LOOK_REIMAGINED || ps2_settings.look == SETTINGS_LOOK_REIMAGINED)
            add(c, SETTINGS_LOOK_REIMAGINED, "Reimagined");
        break;
    case MENU_ROW_INVERT:                /* (menu) bit 0 horizontal, bit 1 vertical */
        add(c, 0, "Off");
        add(c, 2, "Vertical");
        add(c, 1, "Horizontal");
        add(c, 3, "Both");
        break;
    case MENU_ROW_TVFX:
        add(c, 0, "Off");
        add(c, 1, "On");
        break;
    case MENU_ROW_RENDERER:
        add(c, 1, "GPU (Direct3D 11)");
        add(c, 0, "Software (CPU)");
        break;
    case MENU_ROW_RMODE:
        add(c, 1, "Modern");
        add(c, 0, "Reference (exact)");
        break;
    case MENU_ROW_MSAA:                  /* (r2-m8) */
        add(c, SETTINGS_MSAA_OFF, "Off");
        if (PE.msaa == SETTINGS_MSAA_2X) add(c, SETTINGS_MSAA_2X, "2x");   /* (menu: Off / Low / High; 2x from a file) */
        add(c, SETTINGS_MSAA_4X, "Low");
        add(c, SETTINGS_MSAA_8X, "High");
        break;
    case MENU_ROW_LIGHTING:              /* (light) */
    case MENU_ROW_TEXTURES:              /* (tex; a later "AI" value goes after Enhanced) */
        add(c, 0, "Original");
        add(c, 1, "Enhanced");
        break;
    case MENU_ROW_SCALE: {               /* (pc-polish) the render scale, relative to the output */
        static const int pcts[] = { 50, 67, 75, 100, 125, 150, 200 };
        int ow, oh;
        h_output(&ow, &oh);
        /* (menu) Auto and a few steps in both renderings (a file's other percentage in its place, an exact multiple last) */
        int cur = PE.render_scale, found = cur == SETTINGS_RS_AUTO;
        if (PE.renderer_modern) add(c, SETTINGS_RS_AUTO, "Auto (%d x %d)", ow, oh);
        else add(c, SETTINGS_RS_AUTO, "Auto (up to 100%%)");
        if (!PE.renderer_modern) { add(c, SETTINGS_RS_ORIGINAL, "Original (512 x 448)"); found |= cur == SETTINGS_RS_ORIGINAL; }
        for (size_t k = 0; k < sizeof pcts / sizeof pcts[0]; k++) {
            if (!found && settings_rs_is_pct(cur) && cur < pcts[k]) {
                add(c, cur, "%d%% (%d x %d)", cur, (ow * cur + 50) / 100, (oh * cur + 50) / 100);
                found = 1;
            }
            add(c, pcts[k], "%d%% (%d x %d)", pcts[k], (ow * pcts[k] + 50) / 100, (oh * pcts[k] + 50) / 100);
            found |= cur == pcts[k];
        }
        if (!found && settings_rs_is_pct(cur)) add(c, cur, "%d%% (%d x %d)", cur, (ow * cur + 50) / 100, (oh * cur + 50) / 100);
        else if (!found) {                   /* an exact multiple (F2, an old file's internal_scale; original in Modern) */
            int w, h;
            if (cur == SETTINGS_RS_ORIGINAL) add(c, cur, "Original (512 x 448)");
            else { scale_pixels(cur, &w, &h); add(c, cur, "%dx (%d x %d)", cur, w, h); }
        }
        break;
    }
    case MENU_ROW_RESOLUTION: {          /* (pc-polish) windowed: window sizes; fullscreen: the monitor's modes */
        int m = eff_mode();
        if (m == SETTINGS_FULLSCREEN_EXCLUSIVE) {
            int dw, dh, hz;
            h_display(&dw, &dh, &hz);
            add(c, 0, "Desktop (%d x %d)", dw, dh);
            int modes[64], n = host.display_modes ? host.display_modes(modes, 64) : 0, have = eff_res() == 0;
            for (int k = 0; k < n; k++) {
                if (modes[k] == (dw << 16 | dh)) continue;
                add(c, modes[k], "%d x %d", modes[k] >> 16, modes[k] & 0xFFFF);
                have |= modes[k] == eff_res();
            }
            if (!have) add(c, eff_res(), "%d x %d", eff_res() >> 16, eff_res() & 0xFFFF);
        } else if (m == SETTINGS_FULLSCREEN_OFF) {
            winsize_choices(c);
        }
        break;
    }
    case MENU_ROW_SPEAKERS:
        add(c, SETTINGS_SPEAKERS_STEREO, "Stereo");
        add(c, SETTINGS_SPEAKERS_MONO, "Mono");
        add(c, SETTINGS_SPEAKERS_SURROUND, "Surround (Pro Logic II)");
        break;
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
        add(c, 0, "Original");
        add(c, 1, "Thin");
        break;
    case MENU_ROW_BLUR:                  /* (menu) the effective value: On (the game's own, default) / Off */
        add(c, 1, "On");
        add(c, 0, "Off");
        break;
    case MENU_ROW_MODE:
        add(c, SETTINGS_FULLSCREEN_OFF, "Windowed");
        add(c, SETTINGS_FULLSCREEN_BORDERLESS, "Borderless");
        add(c, SETTINGS_FULLSCREEN_EXCLUSIVE, "Fullscreen");
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
    case MENU_ROW_PACING:                /* (pc-pacing: notes/settings.md graphics.pacing) */
        add(c, SETTINGS_PACING_AUTO, "Automatic");
        add(c, SETTINGS_PACING_PACED, "Even (G-Sync / FreeSync)");
        add(c, SETTINGS_PACING_IMMEDIATE, "Lowest latency");
        add(c, SETTINGS_PACING_DISPLAY, "Match display");
        if (PE.pacing == SETTINGS_PACING_VRR) add(c, SETTINGS_PACING_VRR, "Even, tearing allowed (old setting)");
        break;
    case MENU_ROW_FRAME_RATE: {          /* (pc-fps: frame_rate.h) */
        add(c, 60, "60 fps (original)");
        static const int rates[] = { 120, 144, 165, 200 };
        for (int k = 0; k < 4; k++) add(c, rates[k], "%d fps", rates[k]);
        int cur = PE.frame_rate;
        if (cur > 0 && cur != 60 && cur != 120 && cur != 144 && cur != 165 && cur != 200) add(c, cur, "%d fps", cur);
        double hz = frame_rate_display_hz ? frame_rate_display_hz() : 0;
        if (hz > 0) add(c, -2, "Display (%.0f fps)", hz);
        else add(c, -2, "Display refresh rate");
        add(c, -1, "Unlimited");
        break;
    }
    case MENU_ROW_VSYNC:
        add(c, SETTINGS_VSYNC_ON, "On");
        add(c, SETTINGS_VSYNC_OFF, "Off (tearing allowed)");
        add(c, SETTINGS_VSYNC_ADAPTIVE, "Adaptive");
        break;
    case MENU_ROW_FPS: case MENU_ROW_BACKGROUND: case MENU_ROW_VIBRATION: case MENU_ROW_INVERT_X: case MENU_ROW_INVERT_Y:
    case MENU_ROW_LIGHTBAR: case MENU_ROW_INV_BINO: case MENU_ROW_INV_TURRET:
        add(c, 0, "Off");
        add(c, 1, "On");
        break;
    default: break;
    }
    (void)buf;
}

/* [controls] options: runtime/input.c's configuration (as staged) */
static int ctl_get(int id) {
    input_config c;
    ctl_view(&c);
    switch (id) {
    case MENU_ROW_VIBRATION: return c.vibration;
    case MENU_ROW_VIB_STRENGTH: return c.vibration_strength;
    case MENU_ROW_INVERT_X: return c.invert_camera_x;
    case MENU_ROW_INVERT_Y: return c.invert_camera_y;
    case MENU_ROW_INVERT: return (c.invert_camera_x ? 1 : 0) | (c.invert_camera_y ? 2 : 0);
    case MENU_ROW_DEADZONE: return c.deadzone_left;
    case MENU_ROW_DEADZONE_R: return c.deadzone_right;
    case MENU_ROW_LIGHTBAR: return c.lightbar_on;
    default: return 0;
    }
}

static void ctl_set(int id, int v) {
    input_config c;
    ctl_view(&c);
    switch (id) {
    case MENU_ROW_VIBRATION: c.vibration = v; break;
    case MENU_ROW_VIB_STRENGTH: c.vibration_strength = v; break;
    case MENU_ROW_INVERT_X: c.invert_camera_x = v; break;
    case MENU_ROW_INVERT_Y: c.invert_camera_y = v; break;
    case MENU_ROW_INVERT: c.invert_camera_x = v & 1; c.invert_camera_y = (v >> 1) & 1; break;
    case MENU_ROW_DEADZONE: c.deadzone_left = v; break;
    case MENU_ROW_DEADZONE_R: c.deadzone_right = v; break;
    case MENU_ROW_LIGHTBAR: c.lightbar_on = v; break;
    default: return;
    }
    stage_ctl(&c);
}

/* a short rumble on every controller that can (the strength just applied) */
static void rumble_test(void) {
    input_device_info d[INPUT_MAX_DEVICES];
    int n = input_devices(d, INPUT_MAX_DEVICES);
    for (int k = 0; k < n; k++)
        if (d[k].can_rumble) input_test_rumble(d[k].id, 255, 255, 400);
}

/* the option a row shows (-1: none of the table's) */
static int row_opt(int id) {
    switch (id) {
    case MENU_ROW_PRESET: return m_opt("graphics", "preset");
    case MENU_ROW_GLOOK: return m_opt("graphics", "look");
    case MENU_ROW_RENDERER: return m_opt("graphics", "renderer");
    case MENU_ROW_RMODE: return m_opt("graphics", "renderer_mode");
    case MENU_ROW_MSAA: return m_opt("graphics", "msaa");
    case MENU_ROW_LIGHTING: return m_opt("graphics", "lighting");
    case MENU_ROW_TEXTURES: return m_opt("graphics", "textures");
    case MENU_ROW_SCALE: return m_opt("graphics", "render_scale");
    case MENU_ROW_SCALER: return m_opt("graphics", "scaler");
    case MENU_ROW_OUTLINES: return m_opt("graphics", "outlines");
    case MENU_ROW_TVFX: return m_opt("graphics", "filters");
    case MENU_ROW_BLUR: return m_opt("graphics", "motion_blur");
    case MENU_ROW_MODE: return m_opt("display", "mode");
    case MENU_ROW_MONITOR: return m_opt("display", "monitor");
    case MENU_ROW_ASPECT: return m_opt("display", "aspect");
    case MENU_ROW_HUD: return m_opt("display", "hud");
    case MENU_ROW_PACING: return m_opt("graphics", "pacing");
    case MENU_ROW_VSYNC: return m_opt("graphics", "vsync");
    case MENU_ROW_FRAME_RATE: return m_opt("display", "frame_rate");
    case MENU_ROW_FPS: return m_opt("window", "show_fps");
    case MENU_ROW_BACKGROUND: return m_opt("audio", "background");
    case MENU_ROW_MASTER: return m_opt("audio", "master");
    case MENU_ROW_MUSIC: return m_opt("audio", "music");
    case MENU_ROW_EFFECTS: return m_opt("audio", "effects");
    case MENU_ROW_SPEAKERS: return m_opt("audio", "speakers");
    case MENU_ROW_INV_BINO: return m_opt("game", "invert_binocucom");
    case MENU_ROW_INV_TURRET: return m_opt("game", "invert_turret");
    default: return -1;
    }
}

/* the current value of a row (matched against its choices): as staged, the look's values while it is Retro */
static int row_value(int id) {
    switch (id) {
    case MENU_ROW_VIBRATION: case MENU_ROW_VIB_STRENGTH: case MENU_ROW_INVERT_X: case MENU_ROW_INVERT_Y: case MENU_ROW_INVERT:
    case MENU_ROW_DEADZONE: case MENU_ROW_DEADZONE_R: case MENU_ROW_LIGHTBAR:
        return ctl_get(id);
    case MENU_ROW_PRESET: return PE.preset;
    case MENU_ROW_GLOOK: return PE.look;
    case MENU_ROW_RESOLUTION: return eff_mode() == SETTINGS_FULLSCREEN_EXCLUSIVE ? eff_res() : eff_winsize();
    case MENU_ROW_AUTO_MAX: return PE.auto_max;
    case MENU_ROW_BLUR: return settings_motion_blur(&PE);
    case MENU_ROW_MODE: { int i = row_opt(id); return sg.has[i] ? sg.val[i] : h_window_mode(); }
    case MENU_ROW_WINSIZE: return eff_winsize();
    default: {
        int i = row_opt(id);
        return i >= 0 ? settings_get(&PE, i) : 0;
    }
    }
}

/* set a row's value (choice value or slider value): staged until Apply */
static void row_set(int id, int v) {
    switch (id) {
    case MENU_ROW_PRESET:                /* (menu) a Quality restages the options below it (applied with Apply) */
        if (v >= 0 && v < SETTINGS_PRESET_CUSTOM && v != SETTINGS_PRESET_ORIGINAL) {
            ps2_settings_t t = PO;
            settings_apply_preset(&t, v);
            for (int i = 0; i < settings_count() && i < SETTINGS_MAX_OPTS; i++) {
                const settings_opt* o = settings_opt_at(i);
                if ((o->flags & SET_PRESET) && !(o->flags & SET_LEGACY)) stage_opt(i, settings_get(&t, i));
            }
        }
        break;
    case MENU_ROW_RESOLUTION:
        if (eff_mode() == SETTINGS_FULLSCREEN_EXCLUSIVE) stage_key("display", "resolution", v);
        else stage_winsize(v);
        break;
    case MENU_ROW_WINSIZE: stage_winsize(v); break;
    case MENU_ROW_BLUR: {                /* On: the game's own (preset), or an explicit on a file has */
        int i = row_opt(id);
        stage_opt(i, v ? (own_val(i) == 1 ? 1 : -1) : 0);
        break;
    }
    case MENU_ROW_TVFX:                  /* the TV effects; the per-effect overrides follow them again */
        stage_key("graphics", "filters", v);
        stage_key("graphics", "flicker_filter", -1);
        stage_key("graphics", "interlacing", -1);
        break;
    case MENU_ROW_VIBRATION: case MENU_ROW_VIB_STRENGTH: case MENU_ROW_INVERT_X: case MENU_ROW_INVERT_Y: case MENU_ROW_INVERT:
    case MENU_ROW_DEADZONE: case MENU_ROW_DEADZONE_R: case MENU_ROW_LIGHTBAR:
        ctl_set(id, v);
        break;
    default: {
        int i = row_opt(id);
        if (i >= 0) stage_opt(i, v);
        break;
    }
    }
}

/* a row has a change not applied yet */
static int row_changed(int id) {
    input_config now, view;
    switch (id) {
    case MENU_ROW_PRESET: return PE.preset != ps2_settings.preset;
    case MENU_ROW_RESOLUTION: { int i = m_opt("display", "resolution"); return (sg.has[i] && sg.val[i] != own_val(i)) || (sg.win_has && sg.win_val != cur_winsize()); }
    case MENU_ROW_TVFX: {
        int k[3] = { m_opt("graphics", "filters"), m_opt("graphics", "flicker_filter"), m_opt("graphics", "interlacing") };
        for (int j = 0; j < 3; j++) if (sg.has[k[j]] && sg.val[k[j]] != own_val(k[j])) return 1;
        return 0;
    }
    case MENU_ROW_VIBRATION: case MENU_ROW_VIB_STRENGTH: case MENU_ROW_INVERT_X: case MENU_ROW_INVERT_Y: case MENU_ROW_INVERT:
    case MENU_ROW_DEADZONE: case MENU_ROW_DEADZONE_R: case MENU_ROW_LIGHTBAR: {
        if (!sg.ctl_has) return 0;
        int was = ctl_get(id);
        ctl_now(&now);
        input_config save = sg.ctl;
        sg.ctl_has = 0;
        int is = ctl_get(id);
        sg.ctl = save;
        sg.ctl_has = 1;
        return was != is;
    }
    case MENU_ROW_KEYS: case MENU_ROW_PAD:
        if (!sg.ctl_has) return 0;
        ctl_now(&now);
        ctl_view(&view);
        return ctl_diff(&view, &now, id == MENU_ROW_KEYS ? 2 : 3) != 0;
    case MENU_ROW_GFX_ADV: {
        static const char* keys[] = { "renderer", "renderer_mode", "lighting", "textures", "scaler", "motion_blur", "filters",
                                      "flicker_filter", "interlacing" };
        for (size_t j = 0; j < sizeof keys / sizeof keys[0]; j++) { int i = m_opt("graphics", keys[j]); if (sg.has[i] && sg.val[i] != own_val(i)) return 1; }
        int h = m_opt("display", "hud");
        return sg.has[h] && sg.val[h] != own_val(h);
    }
    case MENU_ROW_CTL_ADV: {
        int b = m_opt("game", "invert_binocucom"), t = m_opt("game", "invert_turret");
        if ((sg.has[b] && sg.val[b] != own_val(b)) || (sg.has[t] && sg.val[t] != own_val(t))) return 1;
        return row_changed(MENU_ROW_VIB_STRENGTH) || row_changed(MENU_ROW_DEADZONE) || row_changed(MENU_ROW_DEADZONE_R) ||
               row_changed(MENU_ROW_LIGHTBAR);
    }
    default: {
        if (id >= MENU_ROW_BIND0) {
            if (!sg.ctl_has) return 0;
            int a = id - MENU_ROW_BIND0;
            ctl_now(&now);
            ctl_view(&view);
            if (st.page == MENU_PAGE_PAD) return a < INPUT_ACT_PAD_COUNT && memcmp(view.pad[a], now.pad[a], sizeof now.pad[a]) != 0;
            return a < INPUT_ACT_COUNT && memcmp(view.key[a], now.key[a], sizeof now.key[a]) != 0;
        }
        int i = row_opt(id);
        return i >= 0 && sg.has[i] && sg.val[i] != own_val(i);
    }
    }
}

/* ---- Apply / Keep / Revert (pc-polish; menu: everything) ------------------------------------------------------ */

static void do_close(void);

/* lock held: every staged change into the settings at once; a display change asks "Keep these display settings?" */
static void apply_pending(void) {
    unsigned char has[SETTINGS_MAX_OPTS];
    int val[SETTINGS_MAX_OPTS], n = settings_count() < SETTINGS_MAX_OPTS ? settings_count() : SETTINGS_MAX_OPTS;
    memcpy(has, sg.has, sizeof has);
    memcpy(val, sg.val, sizeof val);
    int ctl_has = sg.ctl_has, win_has = sg.win_has && sg.win_val != cur_winsize(), win_val = sg.win_val;
    input_config ctl = sg.ctl;
    discard_pending();
    for (int i = 0; i < n; i++) if (has[i] && val[i] == own_val(i)) has[i] = 0;
    int mi = m_opt("display", "mode"), ri = m_opt("display", "resolution"), oi = m_opt("display", "monitor");
    int li = m_opt("graphics", "look"), gi = m_opt("graphics", "renderer");
    int disp = has[mi] || has[ri] || has[oi] || win_has;
    if (disp) {
        st.prev_mode = ps2_settings.win_fullscreen;
        st.prev_res = ps2_settings.fullscreen_res;
        st.prev_monitor = ps2_settings.monitor;
        st.prev_win = cur_winsize();
    }
    /* the look: options the Retro look controls change outside it (left first, entered last: they become the player's
     * own values) */
    int look = has[li] ? val[li] : ps2_settings.look, retro_opts = 0;
    for (int i = 0; i < n; i++) retro_opts |= has[i] && settings_retro_controls(i);
    if (ps2_settings.look == SETTINGS_LOOK_RETRO && (look != SETTINGS_LOOK_RETRO || retro_opts))
        m_change_i(li, look == SETTINGS_LOOK_RETRO ? SETTINGS_LOOK_FAITHFUL : look);
    for (int i = 0; i < n; i++) if (has[i] && i != li && i != gi) m_change_i(i, val[i]);
    if (ps2_settings.look != look) m_change_i(li, look);
    if (win_has && host.set_client_size) host.set_client_size(win_val >> 16, win_val & 0xFFFF);
    if (has[gi]) m_change_i(gi, val[gi]);                  /* (the renderer last: a switch of renderer) */
    if (ctl_has) {
        input_config now;
        ctl_now(&now);
        int strength = ctl.vibration_strength != now.vibration_strength && ctl.vibration;
        input_config_set(&ctl, 1);
        settings_touch();
        if (strength) rumble_test();                       /* (the new strength, felt) */
    }
    if (disp) {
        st.dialog = MENU_DLG_KEEP;
        st.dlg_focus = 0;
        st.keep_deadline = now_ms() + MENU_KEEP_SECONDS * 1000ull;
        st.keep_secs = MENU_KEEP_SECONDS;
    } else if (st.dialog == MENU_DLG_PENDING) {
        st.dialog = MENU_DLG_NONE;
    }
}

/* lock held: back to the display settings before Apply */
static void revert_display(void) {
    st.dialog = MENU_DLG_NONE;
    st.close_after_keep = 0;
    if (ps2_settings.win_fullscreen != st.prev_mode) m_change("display", "mode", st.prev_mode);
    if (ps2_settings.fullscreen_res != st.prev_res) m_change("display", "resolution", st.prev_res);
    if (ps2_settings.monitor != st.prev_monitor) m_change("display", "monitor", st.prev_monitor);
    if (st.prev_mode == SETTINGS_FULLSCREEN_OFF && st.prev_win != cur_winsize() && host.set_client_size)
        host.set_client_size(st.prev_win >> 16, st.prev_win & 0xFFFF);
}

static void do_quit(void) {
    st.dialog = MENU_DLG_NONE;
    do_close();
    if (host.quit) host.quit();
}

int menu_row_choice_text(int row_id, int k, char* buf, size_t cap) {
    LOCK();
    choices c;
    preview();
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
        return "The Retro look sets every option to the original: change the Look to choose a Quality.";
    case SETTINGS_PRESET_LOW:
        return "75% of your screen's resolution, no anti-aliasing, original lighting and textures. For slower graphics cards.";
    case SETTINGS_PRESET_MEDIUM:
        return "Your screen's resolution with anti-aliasing (Low), enhanced lighting and textures. The default.";
    case SETTINGS_PRESET_HIGH:
        return "Your screen's resolution with anti-aliasing (High), enhanced lighting and textures, thin outlines.";
    case SETTINGS_PRESET_ULTRA:
        return "Drawn above your screen's resolution and scaled down, with anti-aliasing (High), enhanced lighting and textures, "
               "thin outlines: the sharpest picture. Needs a fast graphics card.";
    default:
        return "Your own combination of the options below. Choose a Quality to set them all at once.";
    }
}

/* (menu) a row the Retro look sets: shown with the original value, locked */
static void retro_lock(menu_row* r) {
    if (r && retro_view()) disable(r, "Set by the Retro look");
}

static void sub_row(menu_row* rows, int* n, int id, const char* label, const char* desc) {
    menu_row* r = row_new(rows, n, id, MENU_ITEM_ACTION, label, desc);
    if (r) r->changed = row_changed(id);
}

static int build_rows(int page, menu_row* rows) {
    int n = 0;
    menu_row* r;
    preview();
    int gpu = h_gpu_active();
    const char* gpu_only = "GPU renderer only";
    int modern_pct = PE.renderer_modern && (PE.render_scale == SETTINGS_RS_AUTO || settings_rs_is_pct(PE.render_scale));
    const char* modern_only = "Modern rendering with a percentage resolution scale only";
    switch (page) {
    case MENU_PAGE_GRAPHICS:
        choice_row(rows, &n, MENU_ROW_GLOOK, "Look",
                   "Faithful: the game's own look, drawn sharp and clean by a modern renderer at your screen's resolution. Retro: "
                   "exactly as the original console drew it - its pixel grid, 4:3, TV flicker filter and interlacing.");
        env_note(last(rows, n), "graphics", "look");
        choice_row(rows, &n, MENU_ROW_PRESET, "Quality", "");
        r = last(rows, n);
        snprintf(r->desc, sizeof r->desc, "%s", preset_desc(PE.preset));
        if (PE.preset == SETTINGS_PRESET_ORIGINAL) snprintf(r->value, sizeof r->value, "Original");
        else if (r->cur < 0) { snprintf(r->value, sizeof r->value, "Custom"); r->can_left = r->can_right = 1; }
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_SCALE, "Resolution scale", "");
        r = last(rows, n);
        if (r && modern_pct) {               /* (r2-m8) Modern: a share of the screen's own pixels */
            int ow, oh, p = settings_rs_is_pct(PE.render_scale) ? PE.render_scale : 100;
            h_output(&ow, &oh);
            snprintf(r->desc, sizeof r->desc,
                     "How sharp the game is drawn, relative to your screen: Auto and 100%% draw exactly your screen's pixels; "
                     "lower is faster and softer, higher is sharper and needs a faster graphics card. Rendering at %d x %d -> "
                     "shown at %d x %d.", (ow * p + 50) / 100, (oh * p + 50) / 100, ow, oh);
        } else if (r) {                      /* the integer grid: what is drawn and what is shown, in pixels */
            int ow, oh, w, h, nn = gfx_scale();
            h_output(&ow, &oh);
            gfx_scale_info si;
            gfx_scale_status(&si);
            if (PE.render_scale == SETTINGS_RS_AUTO && host.auto_scale_now && host.auto_scale_now() >= 1) nn = host.auto_scale_now();
            if (PE.render_scale != SETTINGS_RS_AUTO) nn = settings_scale_for(PE.render_scale, oh);
            scale_pixels(nn, &w, &h);
            snprintf(r->desc, sizeof r->desc,
                     "How sharp the game is drawn, relative to your screen: higher is sharper and needs a faster graphics card. "
                     "Rendering at %d x %d -> shown at %d x %d.%s", w, h, ow, oh,
                     PE.render_scale == SETTINGS_RS_AUTO ? " Auto goes as high as your graphics card keeps up with, up to 100%."
                     : si.limited ? " Drawn lower for now: the graphics card cannot keep up." : "");
        }
        if (!gpu) disable(r, gpu_only);
        env_note(r, "graphics", "render_scale");
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_MSAA, "Anti-aliasing",
                   "Smooths jagged edges (multisampling): Low (4x) costs little, High (8x) is smoother and slower, Off is the fastest.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        else if (!modern_pct) disable(r, modern_only);
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_OUTLINES, "Outlines",
                   "The cel-shading outlines. Original: as the game drew them, one original pixel wide (bolder at high "
                   "resolutions). Thin: fine lines at your screen's resolution.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        retro_lock(r);
        sub_row(rows, &n, MENU_ROW_GFX_ADV, "Advanced graphics",
                "Lighting, textures, the renderer, rendering mode, scaling filter, motion blur, TV effects and the HUD layout.");
        break;
    case MENU_PAGE_GFX_ADV:
        choice_row(rows, &n, MENU_ROW_LIGHTING, "Lighting",
                   "Enhanced: soft shadowing where surfaces meet and a gentle glow around lamps, windows and other bright lights. "
                   "Original: the game's own lighting, unchanged.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        else if (!modern_pct) disable(r, modern_only);
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_TEXTURES, "Textures",
                   "Enhanced: the game's own textures upscaled to four times their resolution with sharp, clean edges, crisp at "
                   "a distance without shimmering. Original: the textures exactly as the game has them.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        else if (!modern_pct) disable(r, modern_only);
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_RENDERER, "Renderer",
                   "GPU: Direct3D 11, any resolution. Software: the original renderer on the processor, at the original "
                   "resolution - for PCs whose graphics card cannot run the GPU renderer.");
        r = last(rows, n);
        if (row_changed(MENU_ROW_RENDERER) && !h_gpu_started()) snprintf(r->note, sizeof r->note, "Needs a restart of the game");
        else if (!h_gpu_started() && ps2_settings.renderer_gpu != gpu) snprintf(r->note, sizeof r->note, "Restart the game to use it");
        else if (!h_gpu_started() && ps2_settings.renderer_gpu) snprintf(r->note, sizeof r->note, "GPU renderer unavailable: see sly2.log");
        env_note(r, "graphics", "renderer");
        choice_row(rows, &n, MENU_ROW_RMODE, "Rendering",
                   "Modern: the game's scenery drawn by a modern graphics pipeline - faster, any resolution. Reference: every "
                   "pixel computed exactly as the original hardware did, on its pixel grid; slower. F10 switches while playing.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        else if (getenv("PS2_R2") && *getenv("PS2_R2")) disable(r, "Set by PS2_R2 (environment)");
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_SCALER, "Scaling filter",
                   "How the picture is scaled to your screen when it is drawn at another size. Automatic: sharp when shrinking, "
                   "smooth when enlarging.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        env_note(r, "graphics", "scaler");
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_BLUR, "Motion blur", "The game's own motion blur (the previous picture blended into the new one).");
        retro_lock(last(rows, n));
        choice_row(rows, &n, MENU_ROW_TVFX, "TV effects",
                   "The original TV output: the flicker filter (a slightly soft picture) and interlacing (half the lines per "
                   "picture). Off: every line sharp in every picture.");
        r = last(rows, n);
        if (r) {                             /* (the effective value: the overrides of a file included) */
            int on = settings_flicker_filter(&PE) && settings_interlacing(&PE), off = !settings_flicker_filter(&PE) && !settings_interlacing(&PE);
            if (!on && !off) { snprintf(r->value, sizeof r->value, "Custom"); r->cur = -1; r->can_left = r->can_right = 1; }
        }
        if (!gpu) disable(r, gpu_only);
        retro_lock(r);
        choice_row(rows, &n, MENU_ROW_HUD, "HUD layout",
                   "Widescreen: the health meter and counters at the screen's edges, or kept in the centre 4:3 area as originally.");
        if (PE.aspect_w * 3 == PE.aspect_h * 4) disable(last(rows, n), "Widescreen only");
        env_note(last(rows, n), "display", "hud");
        break;
    case MENU_PAGE_DISPLAY: {
        int mode = eff_mode();
        choice_row(rows, &n, MENU_ROW_MODE, "Display mode",
                   "Borderless (default): fills the monitor at its own resolution and refresh rate (fast Alt+Tab). Windowed. Fullscreen: the game sets the "
                   "monitor to the resolution below. Alt+Enter or F11 switch between windowed and borderless.");
        r = last(rows, n);
        if (!row_changed(MENU_ROW_MODE) && h_window_mode() != ps2_settings.win_fullscreen && ps2_settings.win_fullscreen == SETTINGS_FULLSCREEN_EXCLUSIVE)
            snprintf(r->note, sizeof r->note, "Fullscreen is not available here: borderless is used");
        choice_row(rows, &n, MENU_ROW_RESOLUTION, "Resolution", "");
        r = last(rows, n);
        if (r) {
            int dw, dh, hz;
            h_display(&dw, &dh, &hz);
            if (mode == SETTINGS_FULLSCREEN_BORDERLESS) {
                snprintf(r->value, sizeof r->value, "%d x %d (desktop)", dw, dh);
                snprintf(r->desc, sizeof r->desc, "The size of the picture on your screen. Borderless always uses the desktop's "
                         "resolution: choose Fullscreen to pick another one, or Windowed for a window size.");
                disable(r, "Borderless uses the desktop's resolution");
            } else {
                if (r->cur < 0) {
                    int v = row_value(MENU_ROW_RESOLUTION);
                    snprintf(r->value, sizeof r->value, "%d x %d", v >> 16, v & 0xFFFF);
                    r->can_left = r->can_right = r->nchoices > 0;
                }
                snprintf(r->desc, sizeof r->desc, "%s", mode == SETTINGS_FULLSCREEN_OFF
                         ? "The size of the game's window (its picture area). The resolution scale (Graphics) is relative to it."
                         : "The resolution Fullscreen sets the monitor to. The resolution scale (Graphics) is relative to it.");
            }
        }
        if (h_monitors() >= 2)               /* (menu) only where there is a choice */
            choice_row(rows, &n, MENU_ROW_MONITOR, "Monitor", "The monitor for fullscreen (and the window). Automatic: where the window is.");
        choice_row(rows, &n, MENU_ROW_ASPECT, "Aspect ratio",
                   "Widescreen shows more of the world at the sides (the HUD and menus keep their shape). 4:3 is the original picture.");
        env_note(last(rows, n), "display", "aspect");
        retro_lock(last(rows, n));
        choice_row(rows, &n, MENU_ROW_FRAME_RATE, "Frame rate limit",
                   "How many times per second the game runs and draws. 60: as originally. Higher is smoother on a high-refresh "
                   "display; the game's speed stays the same. Movies stay at 60. Not every PC reaches every rate in every scene.");
        env_note(last(rows, n), "display", "frame_rate");
        choice_row(rows, &n, MENU_ROW_VSYNC, "VSync",
                   "On: no tearing. Off: a picture is shown at once, tearing allowed. Adaptive: synced unless a picture is late. "
                   "Frame pacing and G-Sync / FreeSync are handled automatically.");
        r = last(rows, n);
        if (!gpu) disable(r, gpu_only);
        env_note(r, "graphics", "vsync");
        choice_row(rows, &n, MENU_ROW_FPS, "Show frame rate", "The frame rate in the top left corner of the picture and in the title bar.");
        env_note(last(rows, n), "window", "show_fps");
        break;
    }
    case MENU_PAGE_AUDIO:
        slider_row(rows, &n, MENU_ROW_MASTER, "Master volume", "The volume of everything the game plays.", 0, 100, 5, "%");
        slider_row(rows, &n, MENU_ROW_MUSIC, "Music volume", "The game's music.", 0, 100, 5, "%");
        if (host.in_game && !host.in_game()) snprintf(last(rows, n)->note, sizeof last(rows, n)->note, "Applies when a game is loaded");
        slider_row(rows, &n, MENU_ROW_EFFECTS, "Effects volume", "Sound effects and voices.", 0, 100, 5, "%");
        if (host.in_game && !host.in_game()) snprintf(last(rows, n)->note, sizeof last(rows, n)->note, "Applies when a game is loaded");
        choice_row(rows, &n, MENU_ROW_SPEAKERS, "Speakers",
                   "The game's sound mix. Stereo for speakers and headphones; Surround: the game's matrix surround, for a receiver "
                   "with Dolby Pro Logic II.");
        choice_row(rows, &n, MENU_ROW_BACKGROUND, "Sound in the background",
                   "Keep playing sound while another window is active. Off: silent until you come back.");
        break;
    case MENU_PAGE_CONTROLS:
        choice_row(rows, &n, MENU_ROW_INVERT, "Invert camera",
                   "The right stick turns the camera the other way: up / down (Vertical), left / right (Horizontal) or both.");
        choice_row(rows, &n, MENU_ROW_VIBRATION, "Vibration", "Controller vibration (the game's rumble).");
        sub_row(rows, &n, MENU_ROW_KEYS, "Keyboard controls", "Choose the keys for each button (two per button).");
        sub_row(rows, &n, MENU_ROW_PAD, "Controller buttons", "Choose the controller buttons for each of the game's buttons.");
        sub_row(rows, &n, MENU_ROW_CTL_ADV, "Advanced controls",
                "Vibration strength, aiming inversion, stick dead zones and the light bar.");
        break;
    case MENU_PAGE_CTL_ADV:
        slider_row(rows, &n, MENU_ROW_VIB_STRENGTH, "Vibration strength",
                   "How strongly the controller vibrates. Applying a new strength gives a short test rumble.", 0, 100, 10, "%");
        if (!ctl_get(MENU_ROW_VIBRATION)) disable(last(rows, n), "Vibration is off");
        choice_row(rows, &n, MENU_ROW_INV_BINO, "Invert Binocucom aiming", "The game's own option: aiming the Binocucom up / down the other way.");
        choice_row(rows, &n, MENU_ROW_INV_TURRET, "Invert turret aiming", "The game's own option: aiming turrets up / down the other way.");
        slider_row(rows, &n, MENU_ROW_DEADZONE, "Left stick dead zone",
                   "How far the left stick must move before Sly does. Raise it if he creeps with the stick let go.", 0, 50, 2, "%");
        slider_row(rows, &n, MENU_ROW_DEADZONE_R, "Right stick dead zone",
                   "How far the right stick must move before the camera turns.", 0, 50, 2, "%");
        choice_row(rows, &n, MENU_ROW_LIGHTBAR, "Light bar", "The DualSense / DualShock 4 light bar (Sly blue).");
        break;
    case MENU_PAGE_KEYS:
    case MENU_PAGE_PAD: {
        int dev = page == MENU_PAGE_PAD, cnt = dev ? INPUT_ACT_PAD_COUNT : INPUT_ACT_COUNT;
        int xbox = st.device == MENU_DEV_XBOX;
        input_config c;
        ctl_view(&c);
        for (int a = 0; a < cnt && n < MENU_MAX_ROWS - 1; a++) {
            r = row_new(rows, &n, MENU_ROW_BIND0 + a, MENU_ITEM_BIND, input_action_label(a),
                        dev ? "Confirm, then press the controller button for this button of the game. Left / right: first or second button."
                            : "Confirm, then press the key for this button of the game. Left / right: first or second key.");
            for (int s = 0; s < INPUT_BIND_SLOTS && s < 2; s++) {
                char* out = s ? r->value2 : r->value;
                const char* t = NULL;
                if (dev) t = c.pad[a][s] != INPUT_BTN_NONE ? input_button_label(c.pad[a][s], xbox) : NULL;
                else t = c.key[a][s] ? input_key_label(c.key[a][s]) : NULL;
                snprintf(out, MENU_TEXT, "%s", t ? t : "-");
            }
            r->can_left = st.bind_slot > 0;
            r->can_right = st.bind_slot < 1;
        }
        row_new(rows, &n, MENU_ROW_BIND_RESET, MENU_ITEM_ACTION, "Reset to defaults",
                dev ? "The controller's original buttons." : "The original keys (README.txt).");
        break;
    }
    default: break;
    }
    if (page < MENU_PAGE_N) {
        row_new(rows, &n, MENU_ROW_RESET, MENU_ITEM_ACTION, "Reset to defaults", "Every option on this page back to its default (with Apply).");
        row_new(rows, &n, MENU_ROW_QUIT, MENU_ITEM_ACTION, "Quit to desktop",
                "Close the game (asks first). Progress since the last save is lost: save from the pause menu first.");
    } else if (page == MENU_PAGE_GFX_ADV || page == MENU_PAGE_CTL_ADV) {
        row_new(rows, &n, MENU_ROW_RESET, MENU_ITEM_ACTION, "Reset to defaults", "Every option on this page back to its default (with Apply).");
    }
    for (int k = 0; k < n; k++)              /* (menu) the changes not applied yet */
        if (rows[k].kind != MENU_ITEM_ACTION) rows[k].changed = row_changed(rows[k].id);
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
    st.capture = 0;
    st.drag = 0;
    st.anim_t = 0;                               /* (pc-menu2) a new page appears in place: no scroll animation */
    if (st.dialog == MENU_DLG_RESET) st.dialog = MENU_DLG_NONE;
    keep_visible(rows_now());
}

/* (menu) back from a sub-page to its parent, on the row that opened it */
static void page_back(void) {
    int back_row = st.page == MENU_PAGE_KEYS ? MENU_ROW_KEYS : st.page == MENU_PAGE_PAD ? MENU_ROW_PAD :
                   st.page == MENU_PAGE_GFX_ADV ? MENU_ROW_GFX_ADV : MENU_ROW_CTL_ADV;
    set_page(menu_page_parent(st.page));
    int m = rows_now();
    for (int k = 0; k < m; k++) if (rows_buf[k].id == back_row) st.sel = k;
}

/* stage an option's default (env-locked options stay) */
static void stage_default(const char* section, const char* key) {
    int i = m_opt(section, key);
    if (i >= 0) stage_opt(i, settings_opt_at(i)->def);
}

/* (menu) Reset to defaults: the page's defaults are staged (Apply uses them, Discard forgets them) */
static void reset_page(void) {
    input_config c, d;
    ctl_view(&c);
    input_config_defaults(&d);
    switch (st.page) {
    case MENU_PAGE_KEYS: memcpy(c.key, d.key, sizeof c.key); stage_ctl(&c); break;
    case MENU_PAGE_PAD: memcpy(c.pad, d.pad, sizeof c.pad); stage_ctl(&c); break;
    case MENU_PAGE_CONTROLS:
        c.invert_camera_x = d.invert_camera_x;
        c.invert_camera_y = d.invert_camera_y;
        c.vibration = d.vibration;
        stage_ctl(&c);
        break;
    case MENU_PAGE_CTL_ADV:
        c.vibration_strength = d.vibration_strength;
        c.deadzone_left = d.deadzone_left;
        c.deadzone_right = d.deadzone_right;
        c.lightbar_on = d.lightbar_on;
        stage_ctl(&c);
        stage_default("game", "invert_binocucom");
        stage_default("game", "invert_turret");
        break;
    case MENU_PAGE_GRAPHICS:                     /* the default look and Quality (Medium sets the rows of this page) */
        stage_default("graphics", "look");
        preview();
        row_set(MENU_ROW_PRESET, SETTINGS_PRESET_MEDIUM);
        break;
    case MENU_PAGE_GFX_ADV:
        stage_default("graphics", "renderer");
        stage_default("graphics", "renderer_mode");
        stage_default("graphics", "scaler");
        stage_default("graphics", "motion_blur");
        stage_default("graphics", "filters");
        stage_default("graphics", "flicker_filter");
        stage_default("graphics", "interlacing");
        stage_default("display", "hud");
        preview();                               /* lighting and textures: as the Quality has them */
        {
            ps2_settings_t t = PO;
            settings_apply_preset(&t, PO.preset >= SETTINGS_PRESET_LOW && PO.preset <= SETTINGS_PRESET_ULTRA ? PO.preset : SETTINGS_PRESET_MEDIUM);
            stage_key("graphics", "lighting", t.lighting_enhanced);
            stage_key("graphics", "textures", t.textures_enhanced);
        }
        break;
    case MENU_PAGE_DISPLAY:                      /* (display mode, resolution and monitor stay) */
        stage_default("display", "aspect");
        stage_default("display", "frame_rate");
        stage_default("graphics", "vsync");
        stage_default("graphics", "pacing");
        stage_default("window", "show_fps");
        break;
    case MENU_PAGE_AUDIO:
        stage_default("audio", "master");
        stage_default("audio", "music");
        stage_default("audio", "effects");
        stage_default("audio", "background");
        stage_default("audio", "speakers");
        break;
    default: break;
    }
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
    case MENU_ROW_RESET: case MENU_ROW_BIND_RESET: reset_page(); break;   /* (menu: staged, no question) */
    case MENU_ROW_KEYS: set_page(MENU_PAGE_KEYS); break;
    case MENU_ROW_PAD: set_page(MENU_PAGE_PAD); break;
    case MENU_ROW_GFX_ADV: set_page(MENU_PAGE_GFX_ADV); break;
    case MENU_ROW_CTL_ADV: set_page(MENU_PAGE_CTL_ADV); break;
    case MENU_ROW_QUIT: st.dialog = MENU_DLG_QUIT; st.dlg_focus = 0; break;
    default: break;
    }
}

static void do_close(void) {
    if (!st.open) return;
    st.open = 0;
    if (st.capture) input_capture_cancel();
    st.capture = 0;
    st.dialog = MENU_DLG_NONE;
    st.dialog_only = 0;
    st.close_after_keep = 0;
    discard_pending();                   /* (what was not applied is forgotten) */
    input_set_menu_active(0);
    if (host.closed) host.closed();
}

/* Back / Close on a main page: changes not applied yet ask first (Apply / Discard / Cancel) */
static void request_close(void) {
    if (pending_any()) { st.dialog = MENU_DLG_PENDING; st.dlg_focus = 0; }
    else do_close();
}

static int dlg_buttons(void) { return st.dialog == MENU_DLG_PENDING ? 3 : 2; }

/* a dialog's answer, lock held: b = 0 yes (the gold button), 1 alt (Discard), 2 no (Back / Cancel) */
static void dialog_answer(int b) {
    switch (st.dialog) {
    case MENU_DLG_RESET:
        st.dialog = MENU_DLG_NONE;
        if (b == 0) reset_page();
        break;
    case MENU_DLG_QUIT:
        if (b == 0) { do_quit(); break; }
        st.dialog = MENU_DLG_NONE;
        if (st.dialog_only) do_close();  /* (opened just to ask: back to the game) */
        break;
    case MENU_DLG_KEEP:
        if (b == 0) {
            st.dialog = MENU_DLG_NONE;
            if (st.close_after_keep) do_close();
        } else revert_display();
        break;
    case MENU_DLG_PENDING:
        if (b == 0) {                    /* Apply, then leave (after "Keep these display settings?" when one shows) */
            apply_pending();
            if (st.dialog == MENU_DLG_KEEP) st.close_after_keep = 1;
            else do_close();
        } else if (b == 1) {
            do_close();                  /* Discard: forgotten, closed */
        } else {
            st.dialog = MENU_DLG_NONE;   /* Cancel: back to the menu */
        }
        break;
    default: st.dialog = MENU_DLG_NONE; break;
    }
}

/* one event, lock held */
static int nav(menu_nav ev) {
    if (!st.open) return 0;
    int n = rows_now();
    menu_row* r = st.sel >= 0 && st.sel < n ? &rows_buf[st.sel] : NULL;
    if (st.capture) {                            /* waiting for a key: only Back cancels */
        if (ev == MENU_NAV_BACK || ev == MENU_NAV_CLOSE) { st.capture = 0; input_capture_cancel(); }
        return 1;
    }
    if (st.dialog) {
        int nb = dlg_buttons();
        if (ev == MENU_NAV_LEFT || ev == MENU_NAV_RIGHT) {   /* (menu) the focus moves between the buttons */
            int order[3] = { 0, 1, 2 }, cnt = nb == 3 ? 3 : 2, at = 0;
            if (nb == 2) order[1] = 2;
            for (int k = 0; k < cnt; k++) if (order[k] == st.dlg_focus) at = k;
            at += ev == MENU_NAV_RIGHT ? 1 : -1;
            if (at < 0) at = 0;
            if (at >= cnt) at = cnt - 1;
            st.dlg_focus = order[at];
        } else if (ev == MENU_NAV_CONFIRM) dialog_answer(st.dlg_focus);
        else if (ev == MENU_NAV_APPLY && st.dialog == MENU_DLG_PENDING) dialog_answer(0);
        else if (ev == MENU_NAV_DISCARD && st.dialog == MENU_DLG_PENDING) dialog_answer(1);
        else if (ev == MENU_NAV_BACK || ev == MENU_NAV_CLOSE || (ev == MENU_NAV_DEFAULTS && st.dialog == MENU_DLG_RESET)) dialog_answer(2);
        if (st.open) keep_visible(rows_now());
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
        if (r->kind == MENU_ITEM_BIND) st.bind_slot = dir > 0 ? 1 : 0;
        else if (r->kind == MENU_ITEM_CHOICE) step_choice(r, dir, 0);
        else if (r->kind == MENU_ITEM_SLIDER) step_slider(r, dir);
        break;
    }
    case MENU_NAV_CONFIRM:
        if (!r || !r->enabled) break;
        if (r->kind == MENU_ITEM_CHOICE) step_choice(r, 1, 1);
        else if (r->kind == MENU_ITEM_ACTION) activate(r);
        else if (r->kind == MENU_ITEM_BIND) {
            st.capture = 1;
            input_capture_begin(st.page == MENU_PAGE_KEYS ? INPUT_CAPTURE_KEYBOARD : INPUT_CAPTURE_CONTROLLER);
        }
        break;
    case MENU_NAV_BACK:
        if (st.page >= MENU_PAGE_N) page_back();
        else request_close();
        break;
    case MENU_NAV_TAB_NEXT:
    case MENU_NAV_TAB_PREV: {
        int p = menu_page_parent(st.page);
        p = (p + (ev == MENU_NAV_TAB_NEXT ? 1 : MENU_PAGE_N - 1)) % MENU_PAGE_N;
        set_page(p);
        break;
    }
    case MENU_NAV_DEFAULTS: reset_page(); break;
    case MENU_NAV_CLOSE: request_close(); break;
    case MENU_NAV_APPLY: if (pending_any()) apply_pending(); break;
    case MENU_NAV_DISCARD: discard_pending(); break;
    default: return 0;
    }
    if (st.open) keep_visible(rows_now());
    return 1;
}

void menu_open(void) {
    LOCK();
    if (!st.open) {
        st.open = 1;
        st.capture = 0;
        st.dialog = MENU_DLG_NONE;
        st.dialog_only = 0;
        st.page = menu_page_parent(st.page);
        discard_pending();                      /* (nothing staged from an earlier visit) */
        keep_visible(rows_now());
        st.anim_t = 0;
        st.drag = 0;
        st.serial++;
        input_set_menu_active(1);               /* the game gets a released pad and no keys */
        input_nav_flush();
    } else if (st.dialog_only && st.dialog == MENU_DLG_QUIT) {   /* (the quit question, then Options: the whole menu) */
        st.dialog = MENU_DLG_NONE;
        st.dialog_only = 0;
        st.serial++;
    }
    UNLOCK();
}

/* (pc-small) open on a main page (the pause menu's "Controls..." -> Controls) */
void menu_open_page(int page) {
    if (page >= 0 && page < MENU_PAGE_N) {
        LOCK();
        if (!st.open) set_page(page);
        UNLOCK();
    }
    menu_open();
}

int menu_quit_request(void) {
    LOCK();
    if (!st.open) {
        st.open = 1;
        st.capture = 0;
        st.dialog_only = 1;
        input_set_menu_active(1);
        input_nav_flush();
    } else if (st.capture) {
        st.capture = 0;
        input_capture_cancel();
    }
    st.dialog = MENU_DLG_QUIT;
    st.dlg_focus = 0;
    st.serial++;
    UNLOCK();
    return 1;
}

int menu_quit_showing(void) { return st.open && *(volatile int*)&st.dialog == MENU_DLG_QUIT; }
int menu_pending(void) {
    LOCK();
    int n = pending_count();
    UNLOCK();
    return n;
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
    note_input();
    LOCK();
    if (dev != st.device) { st.device = dev; st.serial++; }
    st.hover_row = st.hover_tab = st.hover_btn = -1;      /* (keys / buttons: the mouse highlight goes) */
    int used = nav(ev);
    if (used) st.serial++;
    UNLOCK();
    return used;
}

/* a binding captured by runtime/input.c (r: INPUT_CAPTURE_KEY / BUTTON / CANCELLED), lock held */
static void capture_done(int r, int code) {
    if (!st.capture) return;
    st.capture = 0;
    if (r != INPUT_CAPTURE_KEY && r != INPUT_CAPTURE_BUTTON) return;
    int n = rows_now();
    if (st.sel >= n || rows_buf[st.sel].kind != MENU_ITEM_BIND) return;
    int a = rows_buf[st.sel].id - MENU_ROW_BIND0, s = st.bind_slot ? 1 : 0;
    input_config c;
    ctl_view(&c);                                         /* (menu) staged like every change */
    if (st.page == MENU_PAGE_KEYS && r == INPUT_CAPTURE_KEY && a < INPUT_ACT_COUNT) {
        for (int k = 0; k < INPUT_ACT_COUNT; k++)        /* a key does one thing: taken from where it was */
            for (int j = 0; j < INPUT_BIND_SLOTS; j++) if (c.key[k][j] == (uint8_t)code) c.key[k][j] = 0;
        c.key[a][s] = (uint8_t)code;
    } else if (st.page == MENU_PAGE_PAD && r == INPUT_CAPTURE_BUTTON && a < INPUT_ACT_PAD_COUNT) {
        for (int k = 0; k < INPUT_ACT_PAD_COUNT; k++)
            for (int j = 0; j < INPUT_BIND_SLOTS; j++) if (c.pad[k][j] == (uint8_t)code) c.pad[k][j] = INPUT_BTN_NONE;
        c.pad[a][s] = (uint8_t)code;
    } else {
        return;
    }
    stage_ctl(&c);
}

void menu_capture_result(int r, int code) {
    LOCK();
    capture_done(r, code);
    st.serial++;
    UNLOCK();
}

static int device_of(const input_nav_event* ev) {
    if (ev->source != INPUT_SRC_CONTROLLER) return MENU_DEV_KEYBOARD;
    input_device_info d[INPUT_MAX_DEVICES];
    int n = input_devices(d, INPUT_MAX_DEVICES);
    for (int k = 0; k < n; k++)
        if (d[k].id == ev->device_id) return d[k].kind == INPUT_DEV_XINPUT ? MENU_DEV_XBOX : MENU_DEV_PLAYSTATION;
    return MENU_DEV_PLAYSTATION;
}

void menu_tick(void) {
    if (host.tick) host.tick();
    input_nav_event ev;
    while (input_nav_poll(&ev)) {
        if (!menu_is_open()) {                   /* closed: Esc / the PS button open it (host: via the pause menu) */
            if (ev.nav == INPUT_NAV_MENU) {
                if (host.request_open) host.request_open();
                else menu_open();
            }
            continue;
        }
        int dev = device_of(&ev);
        switch (ev.nav) {
        case INPUT_NAV_UP: menu_nav_event(MENU_NAV_UP, dev); break;
        case INPUT_NAV_DOWN: menu_nav_event(MENU_NAV_DOWN, dev); break;
        case INPUT_NAV_LEFT: menu_nav_event(MENU_NAV_LEFT, dev); break;
        case INPUT_NAV_RIGHT: menu_nav_event(MENU_NAV_RIGHT, dev); break;
        case INPUT_NAV_CONFIRM: menu_nav_event(MENU_NAV_CONFIRM, dev); break;
        case INPUT_NAV_BACK: menu_nav_event(MENU_NAV_BACK, dev); break;
        case INPUT_NAV_TAB_NEXT: menu_nav_event(MENU_NAV_TAB_NEXT, dev); break;
        case INPUT_NAV_TAB_PREV: menu_nav_event(MENU_NAV_TAB_PREV, dev); break;
        case INPUT_NAV_MENU: menu_nav_event(MENU_NAV_CLOSE, dev); break;
        case INPUT_NAV_APPLY: menu_nav_event(MENU_NAV_APPLY, dev); break;
        case INPUT_NAV_DISCARD: menu_nav_event(MENU_NAV_DISCARD, dev); break;
        case INPUT_NAV_CLICK: menu_mouse_button(ev.x, ev.y, 1); break;
        case INPUT_NAV_POINTER: menu_mouse_move(ev.x, ev.y); break;
        case INPUT_NAV_RELEASE: menu_mouse_up(ev.x, ev.y); break;
        case INPUT_NAV_WHEEL: menu_mouse_wheel(ev.x, ev.y, ev.delta); break;
        default: break;
        }
    }
    if (menu_capturing()) {
        int code = 0, r = input_capture_poll(&code);
        if (r == INPUT_CAPTURE_KEY || r == INPUT_CAPTURE_BUTTON || r == INPUT_CAPTURE_CANCELLED || r == INPUT_CAPTURE_IDLE)
            menu_capture_result(r, code);
    }
    menu_keep_tick();
}

/* "Keep these display settings?": the countdown; at 0 the previous ones come back */
void menu_keep_tick(void) {
    LOCK();
    if (st.open && st.dialog == MENU_DLG_KEEP) {
        unsigned long long t = now_ms();
        if (t >= st.keep_deadline) {
            revert_display();
            st.serial++;
        } else {
            int secs = (int)((st.keep_deadline - t + 999) / 1000);
            if (secs != st.keep_secs) { st.keep_secs = secs; st.serial++; }
        }
    }
    UNLOCK();
}

uint32_t menu_version(void) {
    return (uint32_t)st.serial * 2654435761u ^ (uint32_t)settings_serial ^ (uint32_t)st.open << 31;
}

/* ---- layout ------------------------------------------------------------------------------------------------- */

static int I(float v) { return (int)floorf(v + 0.5f); }

/* (pc-menu2) the Sly-styled panel: the title in the game's comic lettering at the top left, the page tabs under it, the
 * rows, the description, the button prompts; one unit u = 1 pixel at 1080 rows (narrower outputs shrink it) */
void menu_compute_layout_f(int cw, int ch, int nrows, float scroll, menu_layout* L) {
    memset(L, 0, sizeof *L);
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    float u = (float)ch / 1080.0f;               /* sizes follow the height ... */
    if (u * 1180.0f > (float)cw * 0.96f) u = (float)cw * 0.96f / 1180.0f;   /* ... unless the output is narrow (4:3) */
    if (u < 0.30f) u = 0.30f;
    L->u = u;
    int pw = I(1180 * u), ph = I(960 * u);
    if (pw > cw) pw = cw;
    if (ph > ch) ph = ch;
    L->panel = (menu_rect){ (cw - pw) / 2, (ch - ph) / 2, pw, ph };
    int pad = I(40 * u);
    int x0 = L->panel.x + pad, iw = pw - 2 * pad;
    L->header = (menu_rect){ x0, L->panel.y + I(14 * u), iw, I(96 * u) };
    int ty = L->header.y + L->header.h + I(4 * u), th = I(58 * u), hint = I(70 * u);
    int tw = (iw - 2 * hint) / MENU_PAGE_N;
    for (int k = 0; k < MENU_PAGE_N; k++) L->tabs[k] = (menu_rect){ x0 + hint + k * tw, ty, tw, th };
    L->tab_hint[0] = (menu_rect){ x0, ty, hint, th };
    L->tab_hint[1] = (menu_rect){ x0 + iw - hint, ty, hint, th };
    L->footer = (menu_rect){ x0, L->panel.y + ph - I(66 * u), iw, I(48 * u) };
    L->desc = (menu_rect){ x0, L->footer.y - I(146 * u), iw, I(132 * u) };
    int ly = ty + th + I(16 * u);
    L->list = (menu_rect){ x0, ly, iw, L->desc.y - I(12 * u) - ly };
    L->row_h = I(54 * u);
    if (L->row_h < 8) L->row_h = 8;
    L->visible = L->list.h / L->row_h;
    if (L->visible < 1) L->visible = 1;
    L->value_x = x0 + iw * 45 / 100;
    if (scroll < 0) scroll = 0;
    L->scroll_f = scroll;
    L->first = (int)floorf(scroll);
    float frac = scroll - (float)L->first;
    int arrow = I(44 * u), sbw = nrows > L->visible ? I(12 * u) : 0;
    if (sbw) L->scrollbar = (menu_rect){ x0 + iw - sbw, ly, sbw, L->visible * L->row_h - I(4 * u) };
    for (int k = 0; k <= L->visible && L->first + k < nrows && k < MENU_MAX_ROWS; k++) {
        menu_rect r = { x0, ly + I(((float)k - frac) * (float)L->row_h), iw - (sbw ? sbw + I(12 * u) : 0), L->row_h - I(4 * u) };
        L->rows[k] = r;
        L->left[k] = (menu_rect){ L->value_x, r.y, arrow, r.h };
        L->right[k] = (menu_rect){ r.x + r.w - arrow - I(6 * u), r.y, arrow, r.h };
    }
    /* footer buttons, right to left: back / close, select (changes staged: Discard), (changes staged) Apply */
    int bw = I(220 * u), gap = I(14 * u);
    for (int k = 0; k < 3; k++) L->btn[k] = (menu_rect){ x0 + iw - (k + 1) * bw - k * gap, L->footer.y, bw, L->footer.h };
    /* dialogs, centred on the panel; two buttons or (menu) three */
    int dw = I(860 * u), dh = I(300 * u);
    if (dw > cw) dw = cw;
    L->dlg = (menu_rect){ L->panel.x + (pw - dw) / 2, L->panel.y + (ph - dh) / 2, dw, dh };
    int bbw = I(260 * u), bbh = I(60 * u), by = L->dlg.y + dh - bbh - I(28 * u);
    L->dlg_yes = (menu_rect){ L->dlg.x + dw / 2 - bbw - I(14 * u), by, bbw, bbh };
    L->dlg_no = (menu_rect){ L->dlg.x + dw / 2 + I(14 * u), by, bbw, bbh };
    int b3 = I(250 * u), g3 = I(20 * u), x3 = L->dlg.x + (dw - 3 * b3 - 2 * g3) / 2;
    for (int k = 0; k < 3; k++) L->dlg3[k] = (menu_rect){ x3 + k * (b3 + g3), by, b3, bbh };
}

void menu_compute_layout(int cw, int ch, int nrows, int scroll, menu_layout* L) {
    menu_compute_layout_f(cw, ch, nrows, (float)scroll, L);
}

static int inside(menu_rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* the layout as shown now (the animated scroll): what the mouse points at is what is on the screen */
static void layout_now(menu_layout* L, int n) {
    menu_compute_layout_f(st.cw > 0 ? st.cw : 1920, st.ch > 0 ? st.ch : 1080, n, st.anim_t > 0 ? st.scroll_f : (float)st.scroll, L);
}

/* the row (index) under (x, y), -1 none */
static int row_at(const menu_layout* L, int n, int x, int y) {
    if (!inside(L->list, x, y)) return -1;
    for (int k = 0; k <= L->visible && L->first + k < n && k < MENU_MAX_ROWS; k++)
        if (inside(L->rows[k], x, y)) return L->first + k;
    return -1;
}

/* a slider's value at x (lock held) */
static void slider_drag_to(const menu_layout* L, menu_row* r, int k, int x) {
    int x0 = L->left[k].x + L->left[k].w, x1 = L->right[k].x;
    int nw = (int)(96 * L->u), tx0 = x0 + (int)(10 * L->u), tx1 = x1 - nw - (int)(10 * L->u);   /* (menu_draw.c's track) */
    if (tx1 <= tx0 || !r->enabled) return;
    double f = (double)(x - tx0) / (tx1 - tx0);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int v = r->lo + (int)(f * (r->hi - r->lo) + 0.5);
    v = (v - r->lo + r->step / 2) / r->step * r->step + r->lo;
    if (v < r->lo) v = r->lo;
    if (v > r->hi) v = r->hi;
    if (v != r->val) row_set(r->id, v);
}

/* the scroll bar: the list's first row for a thumb dragged to y */
static void scrollbar_drag_to(const menu_layout* L, int n, int y) {
    menu_rect s = L->scrollbar;
    if (!s.h || n <= L->visible) return;
    float rows_px = (float)s.h / (float)n;
    int top = (int)floorf(((float)(y - s.y)) / rows_px - st.drag_off + 0.5f);
    if (top < 0) top = 0;
    if (top > n - L->visible) top = n - L->visible;
    st.scroll = top;
    if (st.sel < top) st.sel = top;
    if (st.sel >= top + L->visible) st.sel = top + L->visible - 1;
}

void menu_mouse_move(int x, int y) {
    note_input();
    LOCK();
    if (st.open) {
        int n = rows_now();
        menu_layout L;
        layout_now(&L, n);
        if (st.drag == 1) {                      /* (pc-menu2) dragging a slider */
            for (int k = 0; k <= L.visible && L.first + k < n && k < MENU_MAX_ROWS; k++)
                if (rows_buf[L.first + k].id == st.drag_id) slider_drag_to(&L, &rows_buf[L.first + k], k, x);
            st.serial++;
            UNLOCK();
            return;
        }
        if (st.drag == 2) {                      /* the scroll bar */
            scrollbar_drag_to(&L, n, y);
            st.serial++;
            UNLOCK();
            return;
        }
        int ht = -1, hr = -1, hb = -1;
        for (int k = 0; k < MENU_PAGE_N; k++) if (inside(L.tabs[k], x, y)) ht = k;
        hr = row_at(&L, n, x, y);
        if (inside(L.btn[0], x, y)) hb = 0;
        if (pending_any() && inside(L.btn[1], x, y)) hb = 1;   /* (menu) Discard */
        if (pending_any() && inside(L.btn[2], x, y)) hb = 2;   /* Apply */
        if (st.dialog && dlg_buttons() == 3) {
            hb = inside(L.dlg3[0], x, y) ? 3 : inside(L.dlg3[1], x, y) ? 7 : inside(L.dlg3[2], x, y) ? 4 : -1;
            hr = -1; ht = -1;
        } else if (st.dialog) { hb = inside(L.dlg_yes, x, y) ? 3 : inside(L.dlg_no, x, y) ? 4 : -1; hr = -1; ht = -1; }
        for (int k = 0; k < 2; k++) if (!st.dialog && inside(L.tab_hint[k], x, y)) hb = 5 + k;
        if (ht != st.hover_tab || hr != st.hover_row || hb != st.hover_btn || (hr >= 0 && hr != st.sel && !st.capture && !st.dialog)) {
            st.hover_tab = ht; st.hover_row = hr; st.hover_btn = hb;
            if (hr >= 0 && !st.capture && !st.dialog) st.sel = hr;   /* (no keep_visible: the list does not jump under the mouse) */
            if (st.device != MENU_DEV_KEYBOARD) st.device = MENU_DEV_KEYBOARD;
            st.serial++;
        }
    }
    UNLOCK();
}

void menu_mouse_button(int x, int y, int button) {
    note_input();
    LOCK();
    if (st.open) {
        st.device = MENU_DEV_KEYBOARD;
        int n = rows_now();
        menu_layout L;
        layout_now(&L, n);
        int keep = 1;
        if (button == 2) nav(MENU_NAV_BACK);
        else if (st.dialog && dlg_buttons() == 3) {   /* (menu) Apply / Discard / Cancel */
            for (int k = 0; k < 3; k++) if (inside(L.dlg3[k], x, y)) { dialog_answer(k); break; }
        } else if (st.dialog) {                  /* a dialog: its buttons (a click elsewhere does nothing) */
            if (inside(L.dlg_yes, x, y)) dialog_answer(0);
            else if (inside(L.dlg_no, x, y)) dialog_answer(2);
        } else if (st.capture) {
            nav(MENU_NAV_BACK);
        } else {
            int done = 0;
            for (int k = 0; k < MENU_PAGE_N && !done; k++)
                if (inside(L.tabs[k], x, y)) { if (k != st.page) set_page(k); done = 1; }   /* (a sub-page's tab: its main page) */
            for (int k = 0; k < 2 && !done; k++)
                if (inside(L.tab_hint[k], x, y)) { nav(k ? MENU_NAV_TAB_NEXT : MENU_NAV_TAB_PREV); done = 1; }
            if (!done && L.scrollbar.w && inside((menu_rect){ L.scrollbar.x - L.scrollbar.w, L.scrollbar.y, 3 * L.scrollbar.w, L.scrollbar.h }, x, y)) {
                float rows_px = (float)L.scrollbar.h / (float)n, thumb = (float)L.scrollbar.y + (float)st.scroll * rows_px;
                float at = ((float)y - thumb) / rows_px;     /* grabbed on the thumb: keep the grab point; else centre it */
                st.drag_off = at >= 0 && at < (float)L.visible ? at : (float)L.visible / 2.0f;
                st.drag = 2;
                scrollbar_drag_to(&L, n, y);
                keep = 0;
                done = 1;
            }
            int r_i = done ? -1 : row_at(&L, n, x, y);
            if (r_i >= 0) {
                int k = r_i - L.first;
                st.sel = r_i;
                menu_row* r = &rows_buf[st.sel];
                if (inside(L.left[k], x, y) && (r->kind == MENU_ITEM_CHOICE || r->kind == MENU_ITEM_SLIDER)) nav(MENU_NAV_LEFT);
                else if (inside(L.right[k], x, y) && (r->kind == MENU_ITEM_CHOICE || r->kind == MENU_ITEM_SLIDER)) nav(MENU_NAV_RIGHT);
                else if (r->kind == MENU_ITEM_SLIDER && x >= L.value_x) {   /* the bar: the value there, then drag */
                    slider_drag_to(&L, r, k, x);
                    st.drag = 1;
                    st.drag_id = r->id;
                } else nav(MENU_NAV_CONFIRM);
                keep = 0;                        /* (a click on a row never scrolls the list under the mouse) */
                done = 1;
            }
            if (!done && inside(L.btn[0], x, y)) nav(MENU_NAV_BACK);
            else if (!done && pending_any() && inside(L.btn[2], x, y)) apply_pending();
            else if (!done && pending_any() && inside(L.btn[1], x, y)) discard_pending();
        }
        if (st.open && keep) keep_visible(rows_now());
        st.serial++;
    }
    UNLOCK();
}

void menu_mouse_up(int x, int y) {
    (void)x; (void)y;
    LOCK();
    if (st.drag) { st.drag = 0; st.serial++; }
    UNLOCK();
}

/* (pc-menu2) the wheel scrolls the list three rows a notch; the selection stays where it is unless it would leave the
 * list (then it is the row under the mouse, or the nearest visible one) */
void menu_mouse_wheel(int x, int y, int delta) {
    LOCK();
    if (st.open && !st.capture && !st.dialog && delta) {
        int n = rows_now(), vis = st.visible > 0 ? st.visible : 1;
        int top = st.scroll - 3 * delta;
        if (top > n - vis) top = n - vis;
        if (top < 0) top = 0;
        if (top != st.scroll) {
            st.scroll = top;
            if (st.sel < top) st.sel = top;
            if (st.sel >= top + vis) st.sel = top + vis - 1;
            menu_layout L;
            menu_compute_layout(st.cw > 0 ? st.cw : 1920, st.ch > 0 ? st.ch : 1080, n, top, &L);
            int r = row_at(&L, n, x, y);
            if (r >= 0) { st.sel = r; st.hover_row = r; }
            st.serial++;
        }
    }
    UNLOCK();
}

/* (pc-menu2) the list's scroll and the selection band glide to where they belong (a 60 ms time constant: quick, never
 * a jump); snapped when the menu opens or the page changes */
int menu_animate(double now_s) {
    LOCK();
    int moving = 0;
    if (st.open) {
        double dt = st.anim_t > 0 ? now_s - st.anim_t : 1.0;
        if (dt < 0) dt = 0;
        if (dt > 0.25) dt = 0.25;
        float k = st.anim_t > 0 ? 1.0f - expf((float)(-dt / 0.06)) : 1.0f;
        float ts = (float)st.scroll, tsel = (float)st.sel;
        st.scroll_f += (ts - st.scroll_f) * k;
        st.sel_f += (tsel - st.sel_f) * k;
        if (fabsf(ts - st.scroll_f) < 0.002f) st.scroll_f = ts;
        if (fabsf(tsel - st.sel_f) < 0.002f) st.sel_f = tsel;
        moving = st.scroll_f != ts || st.sel_f != tsel;
        st.anim_t = now_s > 0 ? now_s : 1e-9;
    }
    UNLOCK();
    return moving;
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
    v->bind_slot = st.bind_slot;
    v->confirm_reset = st.dialog == MENU_DLG_RESET;
    v->dialog = st.dialog;
    v->dialog_only = st.dialog_only;
    v->pending = pending_count();
    v->dlg_buttons = dlg_buttons();
    v->dlg_focus = st.dlg_focus;
    switch (st.dialog) {
    case MENU_DLG_RESET: {
        const char* pn = st.page == MENU_PAGE_KEYS ? "the keyboard controls" : st.page == MENU_PAGE_PAD ? "the controller buttons"
                                                                                                         : menu_page_name(st.page);
        snprintf(v->dlg_title, sizeof v->dlg_title, "Reset %s to the defaults?", pn);
        snprintf(v->dlg_yes, sizeof v->dlg_yes, "Reset");
        snprintf(v->dlg_no, sizeof v->dlg_no, "Cancel");
        break;
    }
    case MENU_DLG_QUIT:
        snprintf(v->dlg_title, sizeof v->dlg_title, "Quit to desktop?");
        snprintf(v->dlg_text, sizeof v->dlg_text, "Unsaved progress since the last save will be lost.");
        snprintf(v->dlg_yes, sizeof v->dlg_yes, "Quit");
        snprintf(v->dlg_no, sizeof v->dlg_no, "Cancel");
        break;
    case MENU_DLG_KEEP:
        snprintf(v->dlg_title, sizeof v->dlg_title, "Keep these display settings?");
        snprintf(v->dlg_text, sizeof v->dlg_text, "Reverting to the previous settings in %d s.", st.keep_secs);
        snprintf(v->dlg_yes, sizeof v->dlg_yes, "Keep");
        snprintf(v->dlg_no, sizeof v->dlg_no, "Revert");
        break;
    case MENU_DLG_PENDING:
        snprintf(v->dlg_title, sizeof v->dlg_title, "Apply the changed settings?");
        if (v->pending == 1) snprintf(v->dlg_text, sizeof v->dlg_text, "1 change is not applied yet.");
        else snprintf(v->dlg_text, sizeof v->dlg_text, "%d changes are not applied yet.", v->pending);
        snprintf(v->dlg_yes, sizeof v->dlg_yes, "Apply");
        snprintf(v->dlg_alt, sizeof v->dlg_alt, "Discard");
        snprintf(v->dlg_no, sizeof v->dlg_no, "Cancel");
        break;
    default: break;
    }
    v->scroll_f = st.anim_t > 0 ? st.scroll_f : (float)st.scroll;
    v->sel_f = st.anim_t > 0 ? st.sel_f : (float)st.sel;
    v->drag = st.drag;
    v->device = st.device;
    v->hover_tab = st.hover_tab;
    v->hover_row = st.hover_row;
    v->hover_btn = st.hover_btn;
    snprintf(v->title, sizeof v->title, "Options");
    if (st.page >= MENU_PAGE_N)
        snprintf(v->page_title, sizeof v->page_title, "%s", st.page == MENU_PAGE_KEYS ? "Keyboard controls" :
                 st.page == MENU_PAGE_PAD ? "Controller buttons" : menu_page_name(st.page));
    UNLOCK();
}
