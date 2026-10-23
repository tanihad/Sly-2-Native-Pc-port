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
extern void gpu_hw_scale_status(gpu_hw_scale_info* s) __attribute__((weak));   /* (pc-pacing) */
extern void gpu_present_set_mode(int mode, int vsync) __attribute__((weak));
extern void gpu_hw_pacing_changed(int pacing) __attribute__((weak));
void (*gfx_output_size)(int* w, int* h);  /* set by window.c (window_client_size) */

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

static int auto_max(void) {
    int m = ps2_settings.auto_max;
    return m >= 1 && m <= 8 ? m : SETTINGS_SCALE_AUTO_MAX;
}

int gfx_native_scale(int out_h) {
    if (out_h <= 0) return SETTINGS_SCALE_AUTO_MAX;
    int n = (out_h + 447) / 448;          /* the smallest N with N * 448 rows >= the output's rows */
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

int gfx_scale(void) {
    int e = env_scale();
    int n = e ? e : ps2_settings.internal_scale;
    if (scale_auto()) {                  /* (M11) the scale auto runs at (the renderer lowers it while the GPU is too slow) */
        n = auto_was_on && gpu_hw_auto_scale ? gpu_hw_auto_scale() : 0;
        if (n < 1 || n > auto_max()) n = auto_max();
    } else if (!e && n == SETTINGS_SCALE_NATIVE) {   /* (pc-menu) native: from the window's height */
        int w = 0, h = 0;
        if (gfx_output_size) gfx_output_size(&w, &h);
        n = gfx_native_scale(h);
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

void gfx_describe(char* out, int cap) {
    const ps2_settings_t* s = &ps2_settings;
    snprintf(out, (size_t)cap, "internal scale %s%dx%s, outlines %s, filters %s (motion blur %s, flicker filter %s, %s), scaler %s, preset %s",
             scale_auto() ? "auto, now " : s->internal_scale == SETTINGS_SCALE_NATIVE && !env_scale() ? "native, " : "", gfx_scale(),
             env_scale() ? " (PS2_GS_SCALE)" : "", s->outlines_thin ? "thin" : "ps2",
             s->filters_faithful ? "faithful" : "modern",
             settings_motion_blur(s) ? "on" : "off", settings_flicker_filter(s) ? "on" : "off",
             settings_interlacing(s) ? "interlaced" : "progressive",
             settings_scaler_names[s->scaler >= 0 && s->scaler < SETTINGS_SCALER_N ? s->scaler : 0],
             settings_preset_names[s->preset >= 0 && s->preset < SETTINGS_PRESET_N ? s->preset : SETTINGS_PRESET_CUSTOM]);
}

static void gfx_applier(int option) { (void)option; gfx_apply(); }

/* (pc-pacing) graphics.pacing / vsync -> the presenter (gpu_present.c); live. Returns 1 if a presenter took it. */
int gfx_apply_pacing(void) {
    if (!gpu_present_set_mode) return 0;
    int mode = PACE_MODE_AUTO, vs = ps2_settings.vsync;
    switch (ps2_settings.pacing) {
    case SETTINGS_PACING_PACED: mode = PACE_MODE_PACED; break;
    case SETTINGS_PACING_IMMEDIATE: mode = PACE_MODE_IMMEDIATE; break;
    case SETTINGS_PACING_DISPLAY: mode = PACE_MODE_DISPLAY; break;
    case SETTINGS_PACING_VRR: mode = PACE_MODE_PACED; vs = SETTINGS_VSYNC_OFF; break;   /* (the old "vrr": Present(0) + tearing) */
    default: break;
    }
    gpu_present_set_mode(mode, vs);
    if (gpu_hw_pacing_changed) gpu_hw_pacing_changed(ps2_settings.pacing);   /* (the display clock: auto / display) */
    return 1;
}

void gfx_apply(void) {
    static int logged_scale = -1;
    static uint32_t logged_bits = 0xFFFFFFFFu;
    static int logged_blur = -1, logged_auto = -1, logged_preset = -1;
    settings_on_apply(SET_APPLY_GFX, gfx_applier);
    int is_auto = scale_auto();
    int scale = gfx_scale();             /* (auto: the scale it runs at now; switching to auto starts at its maximum) */
    uint32_t bits = gfx_bits();
    int off = !settings_motion_blur(&ps2_settings);
    ps2_motion_blur_off = off;
    if (gpu_hw_set_auto_scale) gpu_hw_set_auto_scale(is_auto ? auto_max() : 0);
    auto_was_on = is_auto;
    if (gpu_hw_set_graphics) gpu_hw_set_graphics(scale, bits);
    gfx_apply_pacing();
    if (scale != logged_scale || bits != logged_bits || off != logged_blur || is_auto != logged_auto ||
        ps2_settings.preset != logged_preset) {
        logged_auto = is_auto;
        logged_preset = ps2_settings.preset;
        char d[256];
        gfx_describe(d, sizeof d);
        fprintf(stderr, "[gfx] %s\n", d);
        logged_scale = scale;
        logged_bits = bits;
        logged_blur = off;
    }
}

void gfx_output_resized(void) {
    if (ps2_settings.internal_scale == SETTINGS_SCALE_NATIVE && !env_scale()) gfx_apply();
}

static void change(const char* key, int v, int persist_it) {
    if (!settings_change_key("graphics", key, v, persist_it)) fprintf(stderr, "[gfx] graphics.%s: bad value %d\n", key, v);
}

int gfx_key(unsigned vk, int shift, int persist_it) {
    ps2_settings_t* s = &ps2_settings;
    switch (vk) {
    case 0x71: {                         /* F2: internal scale up (Shift: down), 1..8 round, from the scale in use; Ctrl: auto */
        int v;
        if (shift & 2) {
            v = SETTINGS_SCALE_AUTO;
        } else {
            int n = gfx_scale() + ((shift & 1) ? -1 : 1);
            if (env_scale()) n = (s->internal_scale <= SETTINGS_SCALE_AUTO ? auto_max() : s->internal_scale) + ((shift & 1) ? -1 : 1);
            v = n < 1 ? 8 : n > 8 ? 1 : n;
        }
        change("internal_scale", v, persist_it);
        if (env_scale()) fprintf(stderr, "[gfx] PS2_GS_SCALE=%d is set: internal_scale %d is used once it is not\n", env_scale(), v);
        break;
    }
    case 0x72:                           /* F3: outlines */
        change("outlines", !s->outlines_thin, persist_it);
        break;
    case 0x73:                           /* F4: filters preset; the toggles follow it again */
        change("motion_blur", -1, persist_it);
        change("flicker_filter", -1, persist_it);
        change("interlacing", -1, persist_it);
        change("filters", !s->filters_faithful, persist_it);
        break;
    case 0x74:                           /* F5: motion blur */
        change("motion_blur", !settings_motion_blur(s), persist_it);
        break;
    case 0x75:                           /* F6: flicker filter; Shift+F6: interlacing */
        if (shift & 1) change("interlacing", !settings_interlacing(s), persist_it);
        else change("flicker_filter", !settings_flicker_filter(s), persist_it);
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

void gfx_scale_status(gfx_scale_info* st) {
    memset(st, 0, sizeof *st);
    st->requested = gfx_scale();
    st->drawn = st->requested;
    st->automatic = scale_auto();
    if (gpu_hw_scale_status) {
        gpu_hw_scale_info g;
        gpu_hw_scale_status(&g);
        if (g.requested > 0) {
            st->requested = g.requested;
            st->drawn = g.drawn;
            st->limited = g.limited;
            st->load = g.load;
            st->stall_ms = g.stall_ms;
        }
    }
    pace_picture_size(st->drawn, ps2_settings.aspect_w, ps2_settings.aspect_h, &st->w, &st->h);
}