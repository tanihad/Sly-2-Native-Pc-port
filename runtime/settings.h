/* User settings: sly2.ini next to the executable (format and keys: notes/settings.md).
 *
 * Loaded once at start-up (settings_load, before the game runs), overridden per key by environment
 * variables for testing (PS2_ASPECT=16:9|4:3|<w>:<h>), written by window.c (the owner of
 * persistence) as a commented template when the window opens and no file exists yet.
 * settings_parse is pure (no I/O) so the unit tests can drive it directly. */
#pragma once
#include <stddef.h>

typedef struct {
    /* [display] */
    int aspect_w, aspect_h;      /* display aspect, reduced: 16:9 (default) or 4:3, any w:h */
    /* [graphics] - read and kept for the GPU renderer (notes/d3d11-renderer.md section 13);
     * the software renderer ignores them. */
    int internal_scale;          /* 1..8, default 3 */
    int outlines_thin;           /* outlines = ps2 (0, default) | thin (1) */
    int filters_faithful;        /* filters = modern (0, default) | faithful (1): preset for the next three */
    int motion_blur;             /* 1 on, 0 off, -1 (default) the preset's value */
    int flicker_filter;
    int interlacing;
} ps2_settings_t;

/* The preset's value where a toggle is -1: Modern = motion blur on, flicker filter off, progressive;
 * Faithful = all three on (as on a PS2 on a TV). */
static inline int settings_motion_blur(const ps2_settings_t* s) { return s->motion_blur >= 0 ? s->motion_blur : 1; }
static inline int settings_flicker_filter(const ps2_settings_t* s) {
    return s->flicker_filter >= 0 ? s->flicker_filter : s->filters_faithful;
}
static inline int settings_interlacing(const ps2_settings_t* s) {
    return s->interlacing >= 0 ? s->interlacing : s->filters_faithful;
}

extern ps2_settings_t ps2_settings;

void settings_defaults(ps2_settings_t* s);
/* Parse INI text into s (which keeps its values for keys not in the text). Returns the number of
 * problems found; each is reported through warn (if not NULL) as "line N: ...". */
typedef void (*settings_warn_fn)(void* user, const char* msg);
int settings_parse(const char* text, ps2_settings_t* s, settings_warn_fn warn, void* user);
/* "16:9", "4:3", "21:9", "1.7777": 1 and the reduced w:h on success. */
int settings_parse_aspect(const char* v, int* w, int* h);
/* The file's text for s (a commented template with every key). Returns the length written
 * (snprintf semantics). */
int settings_format(const ps2_settings_t* s, char* out, size_t cap);

/* sly2.ini next to the executable, then the environment (PS2_ASPECT). Logs what it uses. */
void settings_load(void);
/* Write sly2.ini next to the executable if there is none (window.c, when the window opens). */
void settings_save_if_missing(void);
