/* Graphics settings applied while the game runs (gfx.h). */
#include "gfx.h"
#include "settings.h"
#include "gpu/gpu_front.h"                /* GPU_GFX_*, GPU_SCALER_* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the GPU renderer is linked into the game, not into every test program: weak) */
extern void gpu_hw_set_graphics(int scale, uint32_t gfx) __attribute__((weak));

volatile int ps2_motion_blur_off;

_Static_assert((int)SETTINGS_SCALER_N == (int)GPU_SCALER_N && (int)SETTINGS_SCALER_LANCZOS == (int)GPU_SCALER_LANCZOS &&
               (int)SETTINGS_SCALER_CATMULL == (int)GPU_SCALER_CATMULL, "scaler values (settings.h, gpu_front.h)");

static int env_scale(void) {
    const char* e = getenv("PS2_GS_SCALE");
    int n = e && *e ? atoi(e) : 0;
    return n >= 1 && n <= 8 ? n : 0;
}

int gfx_scale(void) {
    int e = env_scale();
    int n = e ? e : ps2_settings.internal_scale;
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
    snprintf(out, (size_t)cap, "internal scale %dx%s, outlines %s, filters %s (motion blur %s, flicker filter %s, %s), scaler %s",
             gfx_scale(), env_scale() ? " (PS2_GS_SCALE)" : "", s->outlines_thin ? "thin" : "ps2", s->filters_faithful ? "faithful" : "modern",
             settings_motion_blur(s) ? "on" : "off", settings_flicker_filter(s) ? "on" : "off",
             settings_interlacing(s) ? "interlaced" : "progressive",
             settings_scaler_names[s->scaler >= 0 && s->scaler < SETTINGS_SCALER_N ? s->scaler : 0]);
}

void gfx_apply(void) {
    static int logged_scale = -1;
    static uint32_t logged_bits = 0xFFFFFFFFu;
    static int logged_blur = -1;
    int scale = gfx_scale();
    uint32_t bits = gfx_bits();
    int off = !settings_motion_blur(&ps2_settings);
    ps2_motion_blur_off = off;
    if (gpu_hw_set_graphics) gpu_hw_set_graphics(scale, bits);
    if (scale != logged_scale || bits != logged_bits || off != logged_blur) {
        char d[256];
        gfx_describe(d, sizeof d);
        fprintf(stderr, "[gfx] %s\n", d);
        logged_scale = scale;
        logged_bits = bits;
        logged_blur = off;
    }
}

static void persist(const char* key, const char* value) {
    const char* const kv[][3] = { { "graphics", key, value } };
    if (!settings_set(kv, 1)) fprintf(stderr, "[gfx] cannot write graphics.%s to sly2.ini\n", key);
}

static const char* toggle_text(int v) { return v < 0 ? "preset" : v ? "on" : "off"; }

int gfx_key(unsigned vk, int shift, int persist_it) {
    ps2_settings_t* s = &ps2_settings;
    char v[16];
    switch (vk) {
    case 0x71: {                         /* F2: internal scale up (Shift: down), 1..8 round */
        int n = s->internal_scale + (shift ? -1 : 1);
        s->internal_scale = n < 1 ? 8 : n > 8 ? 1 : n;
        snprintf(v, sizeof v, "%d", s->internal_scale);
        if (persist_it) persist("internal_scale", v);
        if (env_scale()) fprintf(stderr, "[gfx] PS2_GS_SCALE=%d is set: internal_scale %d is used once it is not\n", env_scale(), s->internal_scale);
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
        if (shift) {
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
