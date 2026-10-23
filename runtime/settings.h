/* User settings: sly2.ini in the user data folder (format and keys: notes/settings.md).
 *
 * The user data folder (settings_user_dir) holds sly2.ini, the memory card and the log:
 * PS2_USER_DIR if set; else the executable's folder for a console program (every development build
 * and test: data stays next to the exe as it always did) or when a file "portable.txt" is next to
 * the executable; else (the player's GUI build, scripts/build_game.py --package) %LOCALAPPDATA%\Sly2.
 *
 * The settings model (pc-menu): every option is one row of a typed table (settings_opt: section, key, type,
 * range or value names, default, live or at restart, the subsystem that applies a change). The ini parser, the
 * template writer, the Options menu (menu.c) and the hotkeys (gfx.c, window.c) all go through it:
 * settings_change(option, value) validates a value, stores it in ps2_settings, writes it to sly2.ini (keeping
 * every other line and comment) and calls the subsystem's apply function (registered with settings_on_apply, so
 * this file links alone into the unit tests). Graphics presets (Original / Performance / Balanced / Quality /
 * Custom) are a function of the graphics options (settings_detect_preset / settings_apply_preset).
 *
 * Loaded once at start-up (settings_load, before the game runs), overridden per key by environment
 * variables for testing (PS2_ASPECT=16:9|4:3|<w>:<h>, PS2_SHOW_FPS=1, ...), written by window.c (the owner
 * of the window state) as a commented template when the window opens and no file exists yet. Values the
 * game remembers are written back key by key (settings_set), keeping the rest of the file - comments
 * included - as the user left it; environment overrides are never written. settings_parse,
 * settings_update_text and the preset functions are pure (no I/O) so the unit tests can drive them. */
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
                                    immediate (4): SETTINGS_PACING_* (pc-pacing: notes/settings.md, gpu/gpu_pace.h) */
    int internal_scale;          /* 1..8, 0 = auto (default since M11: 3x, lowered while the GPU cannot keep up),
                                    -1 = native (the smallest scale whose height covers the window's) */
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
    int win_maximized, win_fullscreen;   /* win_fullscreen: SETTINGS_FULLSCREEN_* (0 windowed, 1 borderless, 2 exclusive) */
    int show_fps;                /* frame rate in the title bar and in a corner of the picture */
    /* ---- (pc-menu) appended so older initialisers keep their meaning ---- */
    int preset;                  /* [graphics] preset: SETTINGS_PRESET_* (derived from the options; see below) */
    int auto_max;                /* [graphics] auto_max = 1..8 (default 3): the highest scale internal_scale = auto uses */
    /* [audio] */
    int vol_master;              /* master = 0..100 (default 100): the native mixer's output (audio_out.c) */
    int vol_music;               /* music = 0..100 (default 100): the game's own music volume */
    int vol_effects;             /* effects = 0..100 (default 100): the game's own effects volume */
    int audio_background;        /* background = on (default): keep playing sound while the window is not active */
    /* ([controls] belongs to runtime/input.c: input_config_get / input_config_set) */
    /* [display] (pc-menu) */
    int monitor;                 /* monitor = 0 (default: where the window is) | 1..n: fullscreen / window on monitor n */
    /* (pc-pacing) */
    int vsync;                   /* [graphics] vsync = on (0, default) | off (1: tearing allowed) | adaptive (2): SETTINGS_VSYNC_* */
    int fullscreen_refresh;      /* [display] fullscreen_refresh = desktop (0, default) | auto (-1) | <Hz>: display_mode.h */
} ps2_settings_t;

/* graphics.pacing (pc-pacing): auto = pictures presented at an even 59.94 Hz cadence (their VBlank's time + a measured delay:
 * smooth on VRR displays) until the display is known; on a fixed refresh as soon as they are drawn, and on 60 / 120 / 240 Hz
 * displays the display's VBlanks drive the game; paced = the even cadence always; immediate = each picture as soon as it is
 * drawn (lowest latency); display = the display's VBlanks drive the game (60 / 120 / 240 Hz); vrr = the old setting:
 * paced + vsync off. */
enum { SETTINGS_PACING_AUTO, SETTINGS_PACING_VRR, SETTINGS_PACING_DISPLAY, SETTINGS_PACING_PACED, SETTINGS_PACING_IMMEDIATE,
       SETTINGS_PACING_N };
#define SETTINGS_PACING_VSYNC SETTINGS_PACING_AUTO   /* (the old default's name: "vsync" in sly2.ini is auto) */
enum { SETTINGS_VSYNC_ON, SETTINGS_VSYNC_OFF, SETTINGS_VSYNC_ADAPTIVE, SETTINGS_VSYNC_N };
extern const char* const settings_pacing_names[SETTINGS_PACING_N];   /* "auto", "vrr", "display", "paced", "immediate" */
extern const char* const settings_vsync_names[SETTINGS_VSYNC_N];     /* "on", "off", "adaptive" */
enum { SETTINGS_SCALER_AUTO, SETTINGS_SCALER_POINT, SETTINGS_SCALER_BOX, SETTINGS_SCALER_LANCZOS, SETTINGS_SCALER_CATMULL,
       SETTINGS_SCALER_BILINEAR, SETTINGS_SCALER_N };
extern const char* const settings_scaler_names[SETTINGS_SCALER_N];   /* "auto", "point", ... (sly2.ini values) */
#define SETTINGS_SCALE_AUTO 0            /* internal_scale = auto */
#define SETTINGS_SCALE_NATIVE (-1)       /* internal_scale = native (match the window / display) */
#define SETTINGS_SCALE_AUTO_MAX 3        /* the scale auto aims for by default (the owner's decision: 3x on capable GPUs) */
enum { SETTINGS_FULLSCREEN_OFF, SETTINGS_FULLSCREEN_BORDERLESS, SETTINGS_FULLSCREEN_EXCLUSIVE };

/* Graphics presets (notes/settings.md "Presets"; the menu's Graphics > Preset):
 *   Original    - the PS2's look: internal_scale 1, filters faithful (flicker filter, interlacing, motion blur),
 *                 outlines ps2, aspect 4:3.
 *   Performance - internal_scale auto with auto_max 2 (2x, lowered to 1x on a slow GPU), Modern, outlines ps2, wide.
 *   Balanced    - (default) internal_scale auto with auto_max 3 (3x, lowered while the GPU cannot keep up), Modern,
 *                 outlines ps2, wide.
 *   Quality     - internal_scale native (at least the window's height: 4x on 1440p, 5x on 2160p), Modern, thin
 *                 outlines, wide.
 *   Custom      - anything else.
 * Every preset also sets the three filter toggles to `preset` and scaler = auto; "wide" keeps the current wide
 * aspect (16:9 when it was 4:3). The renderer is not part of a preset. */
enum { SETTINGS_PRESET_ORIGINAL, SETTINGS_PRESET_PERFORMANCE, SETTINGS_PRESET_BALANCED, SETTINGS_PRESET_QUALITY,
       SETTINGS_PRESET_CUSTOM, SETTINGS_PRESET_N };
extern const char* const settings_preset_names[SETTINGS_PRESET_N];   /* "original", ..., "custom" */
int  settings_detect_preset(const ps2_settings_t* s);
/* Set s's preset options to the preset's values (CUSTOM: nothing); s->preset = p. */
void settings_apply_preset(ps2_settings_t* s, int p);

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

/* ---- the option table ------------------------------------------------------------------------------- */

enum {                                   /* settings_opt.type */
    SET_BOOL,                            /* on/off (also 1/0, true/false, yes/no) */
    SET_ENUM,                            /* names[value]; aliases "name=value|..." */
    SET_INT,                             /* lo..hi; words "auto=0|native=-1" in aliases */
    SET_TRI,                             /* preset (-1) / off (0) / on (1) */
    SET_ASPECT,                          /* W:H, stored in aspect_w / aspect_h; value = w << 16 | h */
    SET_STRING,                          /* disc path (not a menu value) */
};
enum {                                   /* settings_opt.flags */
    SET_LIVE = 1,                        /* applies while the game runs (else at the next start) */
    SET_PRESET = 2,                      /* part of the graphics presets: a change re-derives graphics.preset */
    SET_GPU = 4,                         /* the GPU renderer only (the software renderer ignores it) */
    SET_STATE = 8,                       /* remembered state, not a setting (window placement, disc): no reset */
};
enum {                                   /* settings_opt.apply: who applies a change (settings_on_apply) */
    SET_APPLY_NONE, SET_APPLY_GFX, SET_APPLY_DISPLAY, SET_APPLY_WINDOW, SET_APPLY_AUDIO, SET_APPLY_INPUT,
    SET_APPLY_PACING, SET_APPLY_RENDERER, SET_APPLY_N
};
typedef struct {
    const char* section;
    const char* key;
    unsigned char type, flags, apply;
    int offset;                          /* of the int in ps2_settings_t (SET_ASPECT: aspect_w; SET_STRING: disc) */
    int lo, hi;                          /* SET_INT range; SET_ENUM: 0..count-1 */
    int def;                             /* default value */
    const char* const* names;            /* SET_ENUM value names (as written to sly2.ini) */
    const char* aliases;                 /* "name=value|name=value": more accepted words (SET_ENUM, SET_INT) */
    const char* env;                     /* environment variable that overrides it (never written), or NULL */
} settings_opt;

int  settings_count(void);
const settings_opt* settings_opt_at(int i);
int  settings_find(const char* section, const char* key);   /* index or -1 */
int  settings_get(const ps2_settings_t* s, int i);          /* the option's value in s */
void settings_put(ps2_settings_t* s, int i, int v);         /* store (no validation, no I/O) */
int  settings_valid(int i, int v);                          /* v is a value of option i */
/* The value as sly2.ini writes it ("auto", "16:9", "on", "4"). Returns buf. */
const char* settings_value_text(int i, int v, char* buf, size_t cap);
/* Parse one sly2.ini value of option i: 1 and *v on success. */
int  settings_parse_value(int i, const char* text, int* v);
/* The environment override of option i is set (its value is not written to sly2.ini, not changeable in menus). */
int  settings_env_locked(int i);

/* Change option i to v in ps2_settings: validated (0 if not a value), stored, written to sly2.ini when persist
 * (with graphics.preset when it changes), applied through the registered applier. A preset change also sets and
 * writes its options. Returns 1 when the value is in effect (or will be at restart). Thread: the caller's (the
 * appliers say which they accept; the window thread for the menu and the hotkeys). */
int  settings_change(int i, int v, int persist);
/* by name; convenience for the hotkeys */
int  settings_change_key(const char* section, const char* key, int v, int persist);
/* Every option of one section back to its default (persisted, applied); SET_STATE options are kept. */
void settings_reset_section(const char* section);
typedef void (*settings_apply_fn)(int option);
void settings_on_apply(int apply, settings_apply_fn fn);    /* register the subsystem that applies SET_APPLY_x */
/* Bumped by every change (settings_change, the hotkeys, an edited file read again): the menu redraws. */
extern volatile long settings_serial;
void settings_touch(void);

void settings_defaults(ps2_settings_t* s);
/* Parse INI text into s (which keeps its values for keys not in the text). Returns the number of
 * problems found; each is reported through warn (if not NULL) as "line N: ...". A `[graphics] preset` other than
 * custom is applied after the other keys (it wins: set preset = custom to choose the options by hand); without a
 * preset key (files of older builds) it is derived from the options. */
typedef void (*settings_warn_fn)(void* user, const char* msg);
int settings_parse(const char* text, ps2_settings_t* s, settings_warn_fn warn, void* user);
/* "16:9", "4:3", "21:9", "1.7777": 1 and the reduced w:h on success. */
int settings_parse_aspect(const char* v, int* w, int* h);
/* "software"/"sw" -> 0, "gpu"/"hw"/"d3d11" -> 1; "vsync" / "vrr" ("tearing") / "display" -> SETTINGS_PACING_* */
int settings_parse_renderer(const char* v, int* gpu);
int settings_parse_pacing(const char* v, int* pacing);
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
/* The options of [graphics] (except renderer and pacing), [audio] and [controls] as sly2.ini says now (copied into
 * *g, which keeps its other fields): 1 if the file is there (gfx.c applies the graphics ones, window.c the rest) */
int settings_read_graphics(ps2_settings_t* g);
/* Write sly2.ini if there is none (window.c, when the window opens). */
void settings_save_if_missing(void);
/* Sections owned by another module ([controls]: runtime/input.c): every key = value of [section], in
 * file order, passed to fn (key without the section). settings_parse ignores [controls] keys.
 * settings_parse_section is pure (unit tests); settings_read_section reads sly2.ini now (1 if it exists). */
typedef void (*settings_key_fn)(void* user, const char* key, const char* value, int line);
int settings_parse_section(const char* text, const char* section, settings_key_fn fn, void* user);
int settings_read_section(const char* section, settings_key_fn fn, void* user);
/* Write keys into sly2.ini (created from the template if missing): n triples section, key, value.
 * Returns 1 on success. Does not change ps2_settings. */
int settings_set(const char* const kv[][3], int n);
