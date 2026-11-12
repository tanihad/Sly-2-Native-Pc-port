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
extern void gpu_mft_config(int on, int pct, int msaa) __attribute__((weak));   /* (r2-m8) gpu_exec_mft.h */
extern void gpu_light_config(int on) __attribute__((weak));   /* (light) gpu_light.h */
extern void gpu_tex_config(int on) __attribute__((weak));     /* (tex) gpu_tex.h */
extern void gpu_look_config(int reimagined) __attribute__((weak));   /* (reimagine) gpu_light.h: Look = Reimagined */
extern void gpu_tex_look(int reimagined) __attribute__((weak));   /* (ai) gpu_tex.h: Reimagined textures */
extern void gpu_reim_lights_config(int level) __attribute__((weak));   /* (local lights) gpu_reim.h: 0 off .. 3 */
extern void gpu_hw_pacing_changed(int pacing) __attribute__((weak));
void (*gfx_output_size)(int* w, int* h);  /* set by window.c (window_client_size) */

volatile int ps2_motion_blur_off;
volatile int ps2_progressive;
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

static int scale_auto(void) { return !env_scale() && ps2_settings.render_scale == SETTINGS_RS_AUTO; }

/* the output's rows (the window's client area / the fullscreen mode; 0 unknown: 1080 is assumed) */
int gfx_output_rows(void) {
    int w = 0, h = 0;
    if (gfx_output_size) gfx_output_size(&w, &h);
    return h > 0 ? h : 0;
}

/* (pc-polish) auto's highest scale: 100 % of the output (3x at 1080p, 4x at 1440p) */
static int auto_max(void) { return settings_scale_for(SETTINGS_RS_AUTO, gfx_output_rows()); }

int gfx_native_scale(int out_h) {
    if (out_h <= 0) return SETTINGS_SCALE_AUTO_MAX;
    int n = (out_h + 447) / 448;          /* the smallest N with N * 448 rows >= the output's rows */
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

int gfx_scale(void) {
    int e = env_scale();
    if (e) return e;
    if (scale_auto()) {                  /* (M11) the scale auto runs at (the renderer lowers it while the GPU is too slow) */
        int n = auto_was_on && gpu_hw_auto_scale ? gpu_hw_auto_scale() : 0;
        if (n < 1 || n > auto_max()) n = auto_max();
        return n;
    }
    return settings_scale_for(ps2_settings.render_scale, gfx_output_rows());   /* (pc-polish) % of the output, original, Nx */
}

uint32_t gfx_bits(void) {
    const ps2_settings_t* s = &ps2_settings;
    uint32_t g = 0;
    if (settings_interlacing(s)) g |= GPU_GFX_FIELDS;       /* field display as the PS2's, field-resolution targets */
    else g |= GPU_GFX_PROGRESSIVE;                          /* progressive: display targets 2N rows per native row */
    if (!settings_flicker_filter(s)) g |= GPU_GFX_FLICKER_OFF;
    if (s->outlines_thin) g |= GPU_GFX_THIN;
    else if (settings_look(s) != SETTINGS_LOOK_RETRO) g |= GPU_GFX_SMOOTH;   /* (outlines) PS2 outlines, smoothed (Retro: as the PS2) */
    int sc = s->scaler >= 0 && s->scaler < SETTINGS_SCALER_N ? s->scaler : 0;
    g |= (uint32_t)sc << GPU_GFX_SCALER_SHIFT;
    return g;
}

void gfx_describe(char* out, int cap) {
    const ps2_settings_t* s = &ps2_settings;
    char rs[32];
    settings_value_text(settings_find("graphics", "render_scale"), s->render_scale, rs, sizeof rs);
    snprintf(out, (size_t)cap, "render scale %s: internal scale %s%dx%s, outlines %s, filters %s (motion blur %s, flicker filter %s, %s), scaler %s, lighting %s, textures %s, preset %s%s",
             rs, scale_auto() ? "now " : "", gfx_scale(),
             env_scale() ? " (PS2_GS_SCALE)" : "", s->outlines_thin ? "thin" : "ps2",
             s->filters_faithful ? "faithful" : "modern",
             settings_motion_blur(s) ? "on" : "off", settings_flicker_filter(s) ? "on" : "off",
             settings_interlacing(s) ? "interlaced" : "progressive",
             settings_scaler_names[s->scaler >= 0 && s->scaler < SETTINGS_SCALER_N ? s->scaler : 0],
             s->lighting_enhanced ? "enhanced" : "original", s->textures_enhanced ? "enhanced" : "original",
             settings_preset_names[s->preset >= 0 && s->preset < SETTINGS_PRESET_N ? s->preset : SETTINGS_PRESET_CUSTOM],
             s->look == SETTINGS_LOOK_RETRO ? ", look retro" : s->look == SETTINGS_LOOK_REIMAGINED ? ", look reimagined" : "");
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
    {                                    /* (PS2_PROGRESSIVE_FIX=0: the game's field offsets in any mode - A/B tests) */
        static int env = -2;
        if (env == -2) { const char* e = getenv("PS2_PROGRESSIVE_FIX"); env = e && *e ? atoi(e) != 0 : -1; }
        ps2_progressive = env >= 0 ? env : !settings_interlacing(&ps2_settings);
    }
    if (gpu_hw_set_auto_scale) gpu_hw_set_auto_scale(is_auto ? auto_max() : 0);
    auto_was_on = is_auto;
    if (gpu_hw_set_graphics) gpu_hw_set_graphics(scale, bits);
    {   /* (r2-m8, R8) modern frames: Modern with a percentage (or auto) render scale draws at that share of the screen with
         * MSAA; an exact multiple (2x..8x, original, PS2_GS_SCALE) keeps the integer grid */
        int rs = ps2_settings.render_scale;
        int on = ps2_settings.renderer_modern && !env_scale() && (rs == SETTINGS_RS_AUTO || settings_rs_is_pct(rs));
        if (gpu_mft_config) gpu_mft_config(on, settings_rs_is_pct(rs) ? rs : 100, settings_msaa_samples(ps2_settings.msaa));
        if (gpu_light_config) gpu_light_config(ps2_settings.lighting_enhanced);   /* (light: modern frames only) */
        if (gpu_tex_config) gpu_tex_config(ps2_settings.textures_enhanced);       /* (tex: modern frames only) */
        if (gpu_look_config) gpu_look_config(settings_look(&ps2_settings) == SETTINGS_LOOK_REIMAGINED);   /* (reimagine) */
        if (gpu_tex_look) gpu_tex_look(settings_look(&ps2_settings) == SETTINGS_LOOK_REIMAGINED);   /* (ai) */
        if (gpu_reim_lights_config) {    /* (local lights) by the Quality: Low none, Medium fewer, High, Ultra the most */
            int p = ps2_settings.preset;
            gpu_reim_lights_config(p == SETTINGS_PRESET_LOW ? 0 : p == SETTINGS_PRESET_MEDIUM ? 1 : p == SETTINGS_PRESET_ULTRA ? 3 : 2);
        }
    }
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
    int rs = ps2_settings.render_scale;          /* (a percentage and auto's maximum follow the output) */
    if ((rs == SETTINGS_RS_AUTO || settings_rs_is_pct(rs)) && !env_scale()) gfx_apply();
}

static void change(const char* key, int v, int persist_it) {
    if (!settings_change_key("graphics", key, v, persist_it)) fprintf(stderr, "[gfx] graphics.%s: bad value %d\n", key, v);
}

int gfx_key(unsigned vk, int shift, int persist_it) {
    ps2_settings_t* s = &ps2_settings;
    switch (vk) {
    case 0x71: {                         /* F2: internal scale up (Shift: down), 1..8 round, from the scale in use; Ctrl: auto */
        int v;                           /* (pc-polish) render_scale: Nx (1x = original), Ctrl: auto */
        if (shift & 2) {
            v = SETTINGS_RS_AUTO;
        } else {
            int cur = env_scale() ? settings_scale_for(s->render_scale, gfx_output_rows()) : gfx_scale();
            int n = cur + ((shift & 1) ? -1 : 1);
            v = n < 1 ? 8 : n > 8 ? 1 : n;
        }
        change("render_scale", v, persist_it);
        if (env_scale()) fprintf(stderr, "[gfx] PS2_GS_SCALE=%d is set: render_scale %d is used once it is not\n", env_scale(), v);
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
    case 0x79:                           /* (r2-m2) F10: Rendering Modern / Reference (live: the next frame) */
        change("renderer_mode", !s->renderer_modern, persist_it);
        fprintf(stderr, "[gfx] rendering: %s%s\n", s->renderer_modern ? "modern" : "reference",
                getenv("PS2_R2") && *getenv("PS2_R2") ? " (PS2_R2 is set: it decides)" : "");
        return 1;
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