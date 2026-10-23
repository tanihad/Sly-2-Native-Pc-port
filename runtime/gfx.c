/* Graphics settings applied while the game runs (gfx.h). */
#include "gfx.h"
#include "settings.h"
#include "gpu/gpu_front.h"                /* GPU_GFX_*, GPU_SCALER_* */
#include "gpu/gpu_hw.h"                   /* (pc-pacing) gpu_hw_scale_info */
#include "gpu/gpu_pace.h"                 /* (pc-pacing) PACE_MODE_*, PACE_VSYNC_*, picture sizes */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the GPU renderer is linked into the game, not into every test program: weak) */
extern void gpu_hw_set_graphics(int scale, uint32_t gfx) __attribute__((weak));
extern void gpu_hw_set_auto_scale(int max) __attribute__((weak));
extern int gpu_hw_auto_scale(void) __attribute__((weak));
extern void gpu_hw_set_native(int which) __attribute__((weak));               /* (pc-pacing) */
extern int gpu_hw_native_scale(void) __attribute__((weak));
extern void gpu_hw_scale_status(gpu_hw_scale_info* s) __attribute__((weak));
extern void gpu_present_set_mode(int mode, int vsync) __attribute__((weak));
void window_client_size(int* w, int* h);                                  /* (window.c: always linked with this file) */

volatile int ps2_motion_blur_off;
static int auto_was_on;                  /* (M11) internal_scale was auto at the last gfx_apply */

_Static_assert((int)SETTINGS_SCALER_N == (int)GPU_SCALER_N && (int)SETTINGS_SCALER_LANCZOS == (int)GPU_SCALER_LANCZOS &&
               (int)SETTINGS_SCALER_CATMULL == (int)GPU_SCALER_CATMULL, "scaler values (settings.h, gpu_front.h)");
_Static_assert((int)SETTINGS_VSYNC_ON == (int)PACE_VSYNC_ON && (int)SETTINGS_VSYNC_OFF == (int)PACE_VSYNC_OFF &&
               (int)SETTINGS_VSYNC_ADAPTIVE == (int)PACE_VSYNC_ADAPTIVE, "vsync values (settings.h, gpu_pace.h)");

static int env_scale(void) {
    const char* e = getenv("PS2_GS_SCALE");
    int n = e && *e ? atoi(e) : 0;
    return n >= 1 && n <= 8 ? n : 0;
}

static int scale_auto(void) { return !env_scale() && ps2_settings.internal_scale == SETTINGS_SCALE_AUTO; }
static int scale_native(void) { return !env_scale() && ps2_settings.internal_scale == SETTINGS_SCALE_NATIVE; }

/* (pc-pacing) the window's picture area and the display aspect (headless: 1280 x 960) */
static void out_size(int* w, int* h, int* aw, int* ah) {
    *w = 1280;
    *h = 960;
    {
        int cw = 0, ch = 0;
        window_client_size(&cw, &ch);
        if (cw > 0 && ch > 0) { *w = cw; *h = ch; }
    }
    *aw = ps2_settings.aspect_w > 0 ? ps2_settings.aspect_w : 4;
    *ah = ps2_settings.aspect_h > 0 ? ps2_settings.aspect_h : 3;
}

int gfx_native_scale(void) {
    if (gpu_hw_native_scale) return gpu_hw_native_scale();
    int w, h, aw, ah;
    out_size(&w, &h, &aw, &ah);
    return pace_native_scale(w, h, aw, ah);
}

/* the highest scale auto picks (internal_scale_max; native resolved for the window) */
static int auto_max(void) {
    int m = ps2_settings.scale_max;
    if (m == SETTINGS_SCALE_NATIVE) return gfx_native_scale();
    return m >= 1 && m <= 8 ? m : SETTINGS_SCALE_AUTO_MAX;
}

int gfx_scale(void) {
    int e = env_scale();
    int n = e ? e : ps2_settings.internal_scale;
    if (scale_native()) n = gfx_native_scale();
    if (scale_auto()) {                  /* (M11) the scale auto runs at (the renderer lowers it while the GPU is too slow) */
        int max = auto_max();
        n = auto_was_on && gpu_hw_auto_scale ? gpu_hw_auto_scale() : 0;
        if (n < 1 || n > max) n = max;
    }
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

uint32_t gfx_bits(void) {
    const ps2_settings_t* s = &ps2_settings;
    uint32_t g = 0;
    if (settings_interlacing(s)) g |= GPU_GFX_FIELDS;       /* field display as the PS2's, field-resolution targets */
    else g |= GPU_GFX_PROGRESSIVE;                          /* progressive: display targets 2N rows per native row */
    if (!settings_flicker_filter(s)) g |= GPU_GFX_FLICKER_OFF;
    if (s->outlines_thin) g |= GPU_GFX_THIN;
    int sc = s->scaler >= 0 && s->scaler < SETTINGS_SCALER_N ? s->scaler : 0;
    g |= (uint32_t)sc << GPU_GFX_SCALER_SHIFT;
    return g;
}

static const char* pacing_name(void) {
    int p = ps2_settings.pacing;
    return settings_pacing_names[p >= 0 && p < SETTINGS_PACING_N ? p : 0];
}
static const char* vsync_name(void) {
    int v = ps2_settings.vsync;
    return settings_vsync_names[v >= 0 && v < SETTINGS_VSYNC_N ? v : 0];
}

void gfx_describe(char* out, int cap) {
    const ps2_settings_t* s = &ps2_settings;
    char sc[48];
    if (scale_auto()) snprintf(sc, sizeof sc, "auto (up to %dx), now %dx", auto_max(), gfx_scale());
    else if (scale_native()) snprintf(sc, sizeof sc, "native, now %dx", gfx_scale());
    else snprintf(sc, sizeof sc, "%dx%s", gfx_scale(), env_scale() ? " (PS2_GS_SCALE)" : "");
    snprintf(out, (size_t)cap, "internal scale %s, outlines %s, filters %s (motion blur %s, flicker filter %s, %s), scaler %s, "
             "pacing %s, vsync %s",
             sc, s->outlines_thin ? "thin" : "ps2", s->filters_faithful ? "faithful" : "modern",
             settings_motion_blur(s) ? "on" : "off", settings_flicker_filter(s) ? "on" : "off",
             settings_interlacing(s) ? "interlaced" : "progressive",
             settings_scaler_names[s->scaler >= 0 && s->scaler < SETTINGS_SCALER_N ? s->scaler : 0], pacing_name(), vsync_name());
}

/* (pc-pacing) settings pacing / vsync -> the presenter (gpu_present.c) */
static void apply_pacing(void) {
    if (!gpu_present_set_mode) return;
    int mode = PACE_MODE_AUTO, vs = ps2_settings.vsync;
    switch (ps2_settings.pacing) {
    case SETTINGS_PACING_PACED: mode = PACE_MODE_PACED; break;
    case SETTINGS_PACING_IMMEDIATE: mode = PACE_MODE_IMMEDIATE; break;
    case SETTINGS_PACING_DISPLAY: mode = PACE_MODE_DISPLAY; break;
    case SETTINGS_PACING_VRR: mode = PACE_MODE_PACED; vs = SETTINGS_VSYNC_OFF; break;   /* (the old "vrr": Present(0) + tearing) */
    default: break;
    }
    gpu_present_set_mode(mode, vs);
}

void gfx_apply(void) {
    static int logged_scale = -1;
    static uint32_t logged_bits = 0xFFFFFFFFu;
    static int logged_blur = -1, logged_auto = -1, logged_pacing = -1, logged_vsync = -1;
    int is_auto = scale_auto();
    int scale = gfx_scale();             /* (auto: the scale it runs at now; switching to auto starts at its maximum) */
    uint32_t bits = gfx_bits();
    int off = !settings_motion_blur(&ps2_settings);
    ps2_motion_blur_off = off;
    /* native follows the window (gpu_hw.c resolves it); auto up to native likewise */
    if (gpu_hw_set_native) gpu_hw_set_native(scale_native() ? 1 : is_auto && ps2_settings.scale_max == SETTINGS_SCALE_NATIVE ? 2 : 0);
    if (gpu_hw_set_auto_scale) gpu_hw_set_auto_scale(is_auto ? auto_max() : 0);
    auto_was_on = is_auto;
    if (gpu_hw_set_graphics) gpu_hw_set_graphics(scale, bits);
    apply_pacing();
    int mode = is_auto ? 1 : scale_native() ? 2 : 0;
    if (scale != logged_scale || bits != logged_bits || off != logged_blur || mode != logged_auto ||
        ps2_settings.pacing != logged_pacing || ps2_settings.vsync != logged_vsync) {
        logged_auto = mode;
        char d[320];
        gfx_describe(d, sizeof d);
        fprintf(stderr, "[gfx] %s\n", d);
        logged_scale = scale;
        logged_bits = bits;
        logged_blur = off;
        logged_pacing = ps2_settings.pacing;
        logged_vsync = ps2_settings.vsync;
    }
}

static void persist(const char* key, const char* value) {
    const char* const kv[][3] = { { "graphics", key, value } };
    if (!settings_set(kv, 1)) fprintf(stderr, "[gfx] cannot write graphics.%s to sly2.ini\n", key);
}

static const char* toggle_text(int v) { return v < 0 ? "preset" : v ? "on" : "off"; }

static void scale_text(int setting, char* v, size_t cap) {
    if (setting == SETTINGS_SCALE_AUTO) snprintf(v, cap, "auto");
    else if (setting == SETTINGS_SCALE_NATIVE) snprintf(v, cap, "native");
    else snprintf(v, cap, "%d", setting);
}

int gfx_key(unsigned vk, int shift, int persist_it) {
    ps2_settings_t* s = &ps2_settings;
    char v[16];
    switch (vk) {
    case 0x71: {                         /* F2: internal scale up (Shift: down), 1..8 round, from the scale in use; Ctrl: auto */
        if (shift & 2) {
            s->internal_scale = SETTINGS_SCALE_AUTO;
            snprintf(v, sizeof v, "auto");
        } else {
            int n = gfx_scale() + ((shift & 1) ? -1 : 1);
            if (env_scale()) n = (s->internal_scale == SETTINGS_SCALE_AUTO || s->internal_scale == SETTINGS_SCALE_NATIVE ?
                                  SETTINGS_SCALE_AUTO_MAX : s->internal_scale) + ((shift & 1) ? -1 : 1);
            s->internal_scale = n < 1 ? 8 : n > 8 ? 1 : n;
            snprintf(v, sizeof v, "%d", s->internal_scale);
        }
        if (persist_it) persist("internal_scale", v);
        if (env_scale()) fprintf(stderr, "[gfx] PS2_GS_SCALE=%d is set: internal_scale %s is used once it is not\n", env_scale(), v);
        break;
    }
    case 0x72:                           /* F3: outlines */
        s->outlines_thin = !s->outlines_thin;
        if (persist_it) persist("outlines", s->outlines_thin ? "thin" : "ps2");
        break;
    case 0x73:                           /* F4: filters preset; the toggles follow it again */
        s->filters_faithful = !s->filters_faithful;
        s->motion_blur = s->flicker_filter = s->interlacing = -1;
        if (persist_it) {
            const char* const kv[][3] = { { "graphics", "filters", s->filters_faithful ? "faithful" : "modern" },
                                          { "graphics", "motion_blur", "preset" }, { "graphics", "flicker_filter", "preset" },
                                          { "graphics", "interlacing", "preset" } };
            if (!settings_set(kv, 4)) fprintf(stderr, "[gfx] cannot write the filters to sly2.ini\n");
        }
        break;
    case 0x74:                           /* F5: motion blur */
        s->motion_blur = !settings_motion_blur(s);
        if (persist_it) persist("motion_blur", toggle_text(s->motion_blur));
        break;
    case 0x75:                           /* F6: flicker filter; Shift+F6: interlacing */
        if (shift & 1) {
            s->interlacing = !settings_interlacing(s);
            if (persist_it) persist("interlacing", toggle_text(s->interlacing));
        } else {
            s->flicker_filter = !settings_flicker_filter(s);
            if (persist_it) persist("flicker_filter", toggle_text(s->flicker_filter));
        }
        break;
    default:
        return 0;
    }
    gfx_apply();
    return 1;
}

void gfx_reread(void) {
    if (settings_read_graphics(&ps2_settings)) gfx_apply();
}

/* ---- (pc-pacing) interfaces for the Options menu ---------------------------------------------------------------- */
void gfx_set_internal_res(int setting, int max, int persist_it) {
    ps2_settings_t* s = &ps2_settings;
    if (setting != SETTINGS_SCALE_AUTO && setting != SETTINGS_SCALE_NATIVE && (setting < 1 || setting > 8)) return;
    s->internal_scale = setting;
    if (max == SETTINGS_SCALE_NATIVE || (max >= 1 && max <= 8)) s->scale_max = max;
    if (persist_it) {
        char v[16], m[16];
        scale_text(setting, v, sizeof v);
        scale_text(s->scale_max >= 1 ? s->scale_max : SETTINGS_SCALE_AUTO_MAX, m, sizeof m);
        const char* const kv[][3] = { { "graphics", "internal_scale", v }, { "graphics", "internal_scale_max", m } };
        if (!settings_set(kv, 2)) fprintf(stderr, "[gfx] cannot write the internal scale to sly2.ini\n");
    }
    gfx_apply();
}

void gfx_set_pacing(int pacing, int vsync, int persist_it) {
    ps2_settings_t* s = &ps2_settings;
    if (pacing >= 0 && pacing < SETTINGS_PACING_N) s->pacing = pacing;
    if (vsync >= 0 && vsync < SETTINGS_VSYNC_N) s->vsync = vsync;
    if (persist_it) {
        const char* const kv[][3] = { { "graphics", "pacing", pacing_name() }, { "graphics", "vsync", vsync_name() } };
        if (!settings_set(kv, 2)) fprintf(stderr, "[gfx] cannot write pacing / vsync to sly2.ini\n");
    }
    gfx_apply();
}

int gfx_res_options(gfx_res_option* out, int max) {
    int w, h, aw, ah;
    out_size(&w, &h, &aw, &ah);
    int native = gfx_native_scale(), n = 0;
    if (n < max) {
        int am = auto_max(), pw, ph;
        pace_picture_size(am, aw, ah, &pw, &ph);
        out[n].setting = SETTINGS_SCALE_AUTO;
        out[n].scale = am;
        out[n].w = pw;
        out[n].h = ph;
        snprintf(out[n].label, sizeof out[n].label, "Auto (up to %d x %d)", pw, ph);
        n++;
    }
    if (n < max) {
        int pw, ph;
        pace_picture_size(native, aw, ah, &pw, &ph);
        out[n].setting = SETTINGS_SCALE_NATIVE;
        out[n].scale = native;
        out[n].w = pw;
        out[n].h = ph;
        snprintf(out[n].label, sizeof out[n].label, "Native: %d x %d (%dx)", pw, ph, native);
        n++;
    }
    for (int k = 1; k <= 8 && n < max; k++, n++) {
        int pw, ph;
        pace_picture_size(k, aw, ah, &pw, &ph);
        out[n].setting = k;
        out[n].scale = k;
        out[n].w = pw;
        out[n].h = ph;
        snprintf(out[n].label, sizeof out[n].label, "%d x %d (%dx%s)", pw, ph, k, k == 1 ? ", PS2" : k == native ? ", native" : "");
    }
    return n;
}

void gfx_scale_status(gfx_scale_info* st) {
    memset(st, 0, sizeof *st);
    st->requested = gfx_scale();
    st->drawn = st->requested;
    st->automatic = scale_auto();
    if (gpu_hw_scale_status) {
        gpu_hw_scale_info g;
        gpu_hw_scale_status(&g);
        st->requested = g.requested;
        st->drawn = g.drawn;
        st->limited = g.limited;
        st->load = g.load;
        st->stall_ms = g.stall_ms;
    }
    int w, h, aw, ah;
    out_size(&w, &h, &aw, &ah);
    pace_picture_size(st->drawn, aw, ah, &st->w, &st->h);
}

void gfx_preset_apply(int preset, int persist_it) {
    ps2_settings_t* s = &ps2_settings;
    switch (preset) {
    case GFX_PRESET_ORIGINAL: s->internal_scale = 1; break;
    case GFX_PRESET_PERFORMANCE: s->internal_scale = SETTINGS_SCALE_AUTO; s->scale_max = 2; break;
    case GFX_PRESET_BALANCED: s->internal_scale = SETTINGS_SCALE_AUTO; s->scale_max = 3; break;
    case GFX_PRESET_QUALITY: s->internal_scale = SETTINGS_SCALE_NATIVE; break;   /* (the most the GPU draws in real time) */
    default: return;
    }
    gfx_set_internal_res(s->internal_scale, s->scale_max ? s->scale_max : SETTINGS_SCALE_AUTO_MAX, persist_it);
}
