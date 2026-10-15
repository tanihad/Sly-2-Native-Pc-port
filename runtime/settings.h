/* User settings: sly2.ini in the user data folder (format and keys: notes/settings.md).
 *
 * The user data folder (settings_user_dir) holds sly2.ini, the memory card and the log:
 * PS2_USER_DIR if set; else the executable's folder for a console program (every development build
 * and test: data stays next to the exe as it always did) or when a file "portable.txt" is next to
 * the executable; else (the player's GUI build, scripts/build_game.py --package) %LOCALAPPDATA%\Sly2.
 *
 * Loaded once at start-up (settings_load, before the game runs), overridden per key by environment
 * variables for testing (PS2_ASPECT=16:9|4:3|<w>:<h>, PS2_SHOW_FPS=1), written by window.c (the owner
 * of persistence) as a commented template when the window opens and no file exists yet. Values the
 * game remembers (the disc image, the window's placement) are written back key by key
 * (settings_set), keeping the rest of the file - comments included - as the user left it; environment
 * overrides are never written. settings_parse and settings_update_text are pure (no I/O) so the unit
 * tests can drive them directly. */
#pragma once
#include <stddef.h>
#include <wchar.h>

typedef struct {
    /* [display] */
    int aspect_w, aspect_h;      /* display aspect, reduced: 16:9 (default) or 4:3, any w:h */
    int hud_centered;            /* hud = edges (0, default: HUD widgets at the screen's edges) | centered (1) */
    /* [graphics] - read and kept for the GPU renderer (notes/d3d11-renderer.md section 13);
     * the software renderer ignores them. */
    int internal_scale;          /* 1..8, default 3 */
    int outlines_thin;           /* outlines = ps2 (0, default) | thin (1) */
    int filters_faithful;        /* filters = modern (0, default) | faithful (1): preset for the next three */
    int motion_blur;             /* 1 on, 0 off, -1 (default) the preset's value */
    int flicker_filter;
    int interlacing;
    /* [game] */
    char disc[1024];             /* disc image path (UTF-8), "" = not chosen yet (runtime/launch.c) */
    /* [window] - remembered when the window closes (window.c) */
    int win_x, win_y, win_w, win_h;   /* the window's normal (not maximized) rectangle; win_w 0 = none */
    int win_maximized, win_fullscreen;
    int show_fps;                /* frame rate in the title bar */
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
/* Set `key` in `[section]` to `value` in INI text: the key's line is rewritten (a comment after the
 * value is kept), or added at the end of its section, or the section is appended. Every other line
 * stays as it was. Values with ; # or leading/trailing blanks are written in double quotes. Returns
 * the new length, or -1 if it does not fit in cap. */
int settings_update_text(const char* text, const char* section, const char* key, const char* value,
                         char* out, size_t cap);

/* The user data folder (created if missing; no trailing backslash), and a file in it. */
const wchar_t* settings_user_dir(void);
int settings_user_path(const wchar_t* name, wchar_t* out, size_t cap);
int settings_portable(void);              /* 1: not %LOCALAPPDATA%\Sly2 (exe folder, or PS2_USER_DIR) */

/* sly2.ini from the user data folder, then the environment (PS2_ASPECT, PS2_HUD, PS2_SHOW_FPS). Logs what it uses. */
void settings_load(void);
/* [display] aspect and hud as sly2.ini has them now (1), for a file edited while the game runs;
 * keys overridden by the environment keep the current values. 0: no file. */
int settings_read_display(int* aspect_w, int* aspect_h, int* hud_centered);
/* Write sly2.ini if there is none (window.c, when the window opens). */
void settings_save_if_missing(void);
/* Write keys into sly2.ini (created from the template if missing): n triples section, key, value.
 * Returns 1 on success. Does not change ps2_settings. */
int settings_set(const char* const kv[][3], int n);
