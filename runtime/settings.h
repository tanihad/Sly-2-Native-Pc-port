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
    int renderer_gpu;            /* renderer = gpu (1, default since M11: the Direct3D 11 GS backend; the software
                                    renderer when it cannot start) | software (0) */
    int pacing;                  /* pacing = auto (0, default; "vsync" = the same) | vrr (1) | display (2) | paced (3) |
                                    immediate (4): SETTINGS_PACING_* (notes/settings.md, gpu/gpu_pace.h) */
    int internal_scale;          /* 1..8, or 0 = auto (default since M11: 3x, lowered while the GPU cannot keep up) */
    int outlines_thin;           /* outlines = ps2 (0, default) | thin (1) */
    int filters_faithful;        /* filters = modern (0, default) | faithful (1): preset for the next three */
    int motion_blur;             /* 1 on, 0 off, -1 (default) the preset's value */
    int flicker_filter;
    int interlacing;
    int scaler;                  /* scaler = auto (0, default) | point | box | lanczos | catmull-rom | bilinear: the GPU
                                    renderer's picture -> window filter (SETTINGS_SCALER_*, = GPU_SCALER_*) */
    /* [game] */
    char disc[1024];             /* disc image path (UTF-8), "" = not chosen yet (runtime/launch.c) */
    /* [window] - remembered when the window closes (window.c) */
    int win_x, win_y, win_w, win_h;   /* the window's normal (not maximized) rectangle; win_w 0 = none */
    int win_maximized, win_fullscreen;
    int show_fps;                /* frame rate in the title bar */
    /* (pc-pacing; kept last: ps2_settings' initializer is positional) */
    int vsync;                   /* [graphics] vsync = on (0, default) | off (1: tearing allowed) | adaptive (2): SETTINGS_VSYNC_* */
    int scale_max;               /* [graphics] internal_scale_max = 1..8 | native: the highest scale auto picks (0: the default,
                                    SETTINGS_SCALE_AUTO_MAX) */
    int fullscreen_refresh;      /* [display] fullscreen_refresh = desktop (0, default) | auto (-1) | <Hz>: display_mode.h */
} ps2_settings_t;

/* graphics.pacing: auto = pictures presented at an even 59.94 Hz cadence (their VBlank's time + a measured delay: smooth
 * on VRR displays), the game's clock from the display when it runs at 60 / 120 / 240 Hz; paced = the same with the
 * 59.94 Hz timer always; immediate = each picture as soon as it is drawn (lowest latency, uneven); display = the
 * display's VBlanks drive the game (60 / 120 / 240 Hz displays); vrr = the old setting: paced + vsync off */
enum { SETTINGS_PACING_AUTO, SETTINGS_PACING_VRR, SETTINGS_PACING_DISPLAY, SETTINGS_PACING_PACED, SETTINGS_PACING_IMMEDIATE,
       SETTINGS_PACING_N };
#define SETTINGS_PACING_VSYNC SETTINGS_PACING_AUTO   /* (the old default's name) */
enum { SETTINGS_VSYNC_ON, SETTINGS_VSYNC_OFF, SETTINGS_VSYNC_ADAPTIVE, SETTINGS_VSYNC_N };
extern const char* const settings_pacing_names[SETTINGS_PACING_N];   /* "auto", "vrr", "display", "paced", "immediate" */
extern const char* const settings_vsync_names[SETTINGS_VSYNC_N];     /* "on", "off", "adaptive" */
enum { SETTINGS_SCALER_AUTO, SETTINGS_SCALER_POINT, SETTINGS_SCALER_BOX, SETTINGS_SCALER_LANCZOS, SETTINGS_SCALER_CATMULL,
       SETTINGS_SCALER_BILINEAR, SETTINGS_SCALER_N };
extern const char* const settings_scaler_names[SETTINGS_SCALER_N];   /* "auto", "point", ... (sly2.ini values) */
#define SETTINGS_SCALE_AUTO 0            /* internal_scale = auto */
#define SETTINGS_SCALE_NATIVE 100        /* (pc-pacing) internal_scale = native: the smallest scale whose picture covers the
                                            window's picture area in both directions (1440p 16:9 -> 4x: 2560 x 1792) */
#define SETTINGS_SCALE_AUTO_MAX 3        /* the scale auto aims for (the owner's decision: 3x on capable GPUs) */

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
/* "software"/"sw" -> 0, "gpu"/"hw"/"d3d11" -> 1; "vsync" / "vrr" ("tearing") / "display" -> SETTINGS_PACING_* */
int settings_parse_renderer(const char* v, int* gpu);
int settings_parse_pacing(const char* v, int* pacing);
int settings_parse_vsync(const char* v, int* vsync);   /* on / off / adaptive */
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
/* [graphics] internal_scale, outlines, filters, motion_blur, flicker_filter, interlacing and scaler as sly2.ini
 * says now (copied into *g, which keeps its other fields): 1 if the file is there (runtime/gfx.c applies them) */
int settings_read_graphics(ps2_settings_t* g);
/* Write sly2.ini if there is none (window.c, when the window opens). */
void settings_save_if_missing(void);
/* Write keys into sly2.ini (created from the template if missing): n triples section, key, value.
 * Returns 1 on success. Does not change ps2_settings. */
int settings_set(const char* const kv[][3], int n);
