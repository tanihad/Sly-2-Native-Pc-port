/* User settings file (sly2.ini), its environment overrides and the typed option table. See settings.h and
 * notes/settings.md. */
#include "settings.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

ps2_settings_t ps2_settings = {
    .aspect_w = 16, .aspect_h = 9, .renderer_gpu = 1, .internal_scale = SETTINGS_SCALE_AUTO,
    .motion_blur = -1, .flicker_filter = -1, .interlacing = -1,
    .preset = SETTINGS_PRESET_BALANCED, .auto_max = SETTINGS_SCALE_AUTO_MAX,
    .vol_master = 100, .vol_music = 100, .vol_effects = 100, .audio_background = 1,
    .frame_rate = 60,
};
const char* const settings_scaler_names[SETTINGS_SCALER_N] = { "auto", "point", "box", "lanczos", "catmull-rom", "bilinear" };
const char* const settings_preset_names[SETTINGS_PRESET_N] = { "original", "performance", "balanced", "quality", "custom" };
volatile long settings_serial;

/* ---- the option table ---------------------------------------------------------------------------- */

static const char* const n_hud[] = { "edges", "centered" };
static const char* const n_renderer[] = { "software", "gpu" };
const char* const settings_pacing_names[SETTINGS_PACING_N] = { "auto", "vrr", "display", "paced", "immediate" };   /* (pc-pacing) */
const char* const settings_vsync_names[SETTINGS_VSYNC_N] = { "on", "off", "adaptive" };
#define n_pacing settings_pacing_names
static const char* const n_outlines[] = { "ps2", "thin" };
static const char* const n_filters[] = { "modern", "faithful" };
static const char* const n_fullscreen[] = { "off", "on", "exclusive" };

#define OFF(f) ((int)offsetof(ps2_settings_t, f))
#define BOOL_(sec, key, f, flags, apply, def) { sec, key, SET_BOOL, flags, apply, OFF(f), 0, 1, def, NULL, NULL, NULL }
#define ENUM_(sec, key, f, flags, apply, names, n, def, aliases, env) \
    { sec, key, SET_ENUM, flags, apply, OFF(f), 0, (n) - 1, def, names, aliases, env }
#define INT_(sec, key, f, flags, apply, lo, hi, def, words, env) { sec, key, SET_INT, flags, apply, OFF(f), lo, hi, def, NULL, words, env }
#define TRI_(sec, key, f) { sec, key, SET_TRI, SET_LIVE | SET_PRESET, SET_APPLY_GFX, OFF(f), -1, 1, -1, NULL, NULL, NULL }

static const settings_opt opts[] = {
    { "display", "aspect", SET_ASPECT, SET_LIVE, SET_APPLY_DISPLAY, OFF(aspect_w), 0, 0, 16 << 16 | 9, NULL, NULL, "PS2_ASPECT" },
    ENUM_("display", "hud", hud_centered, SET_LIVE, SET_APPLY_DISPLAY, n_hud, 2, 0, "centred=1", "PS2_HUD"),
    INT_("display", "monitor", monitor, SET_LIVE, SET_APPLY_WINDOW, 0, 16, 0, NULL, NULL),
    /* (pc-pacing) display_mode.c: applied when the window next goes fullscreen (or is activated fullscreen) */
    INT_("display", "fullscreen_refresh", fullscreen_refresh, SET_LIVE, SET_APPLY_NONE, 0, 1000, 0, "desktop=0|auto=-1", NULL),
    /* (pc-fps) frame_rate.c: the game loop's rate; live */
    INT_("display", "frame_rate", frame_rate, SET_LIVE, SET_APPLY_FRAME_RATE, 30, 1000, 60, "unlimited=-1|display=-2|uncapped=-1",
         "PS2_FRAME_RATE"),
    ENUM_("graphics", "preset", preset, SET_LIVE, SET_APPLY_GFX, settings_preset_names, SETTINGS_PRESET_N,
          SETTINGS_PRESET_BALANCED, NULL, NULL),
    ENUM_("graphics", "renderer", renderer_gpu, SET_LIVE, SET_APPLY_RENDERER, n_renderer, 2, 1, "sw=0|hw=1|d3d11=1",
          "PS2_GS_BACKEND"),
    ENUM_("graphics", "pacing", pacing, SET_LIVE | SET_GPU, SET_APPLY_PACING, n_pacing, SETTINGS_PACING_N, SETTINGS_PACING_AUTO,
          "vsync=0|tearing=1", "PS2_GS_PACING"),
    ENUM_("graphics", "vsync", vsync, SET_LIVE | SET_GPU, SET_APPLY_PACING, settings_vsync_names, SETTINGS_VSYNC_N, SETTINGS_VSYNC_ON,
          NULL, "PS2_GS_VSYNC"),   /* (pc-pacing) */
    INT_("graphics", "internal_scale", internal_scale, SET_LIVE | SET_PRESET | SET_GPU, SET_APPLY_GFX, 1, 8, SETTINGS_SCALE_AUTO,
         "auto=0|native=-1", "PS2_GS_SCALE"),
    INT_("graphics", "auto_max", auto_max, SET_LIVE | SET_PRESET | SET_GPU, SET_APPLY_GFX, 1, 8, SETTINGS_SCALE_AUTO_MAX, NULL, NULL),
    ENUM_("graphics", "outlines", outlines_thin, SET_LIVE | SET_PRESET | SET_GPU, SET_APPLY_GFX, n_outlines, 2, 0, NULL, NULL),
    ENUM_("graphics", "filters", filters_faithful, SET_LIVE | SET_PRESET, SET_APPLY_GFX, n_filters, 2, 0, NULL, NULL),
    TRI_("graphics", "motion_blur", motion_blur),
    TRI_("graphics", "flicker_filter", flicker_filter),
    TRI_("graphics", "interlacing", interlacing),
    ENUM_("graphics", "scaler", scaler, SET_LIVE | SET_PRESET | SET_GPU, SET_APPLY_GFX, settings_scaler_names, SETTINGS_SCALER_N,
          SETTINGS_SCALER_AUTO, "catmull=4", "PS2_GS_SCALER"),
    INT_("audio", "master", vol_master, SET_LIVE, SET_APPLY_AUDIO, 0, 100, 100, NULL, NULL),
    INT_("audio", "music", vol_music, SET_LIVE, SET_APPLY_AUDIO, 0, 100, 100, NULL, NULL),
    INT_("audio", "effects", vol_effects, SET_LIVE, SET_APPLY_AUDIO, 0, 100, 100, NULL, NULL),
    BOOL_("audio", "background", audio_background, SET_LIVE, SET_APPLY_AUDIO, 1),
    { "game", "disc", SET_STRING, SET_STATE, SET_APPLY_NONE, OFF(disc), 0, 0, 0, NULL, NULL, NULL },
    ENUM_("window", "fullscreen", win_fullscreen, SET_LIVE, SET_APPLY_WINDOW, n_fullscreen, 3, 0,
          "borderless=1|1=1|true=1|yes=1|0=0|false=0|no=0", NULL),
    BOOL_("window", "show_fps", show_fps, SET_LIVE, SET_APPLY_WINDOW, 0),
    INT_("window", "x", win_x, SET_STATE, SET_APPLY_NONE, -100000, 100000, 0, NULL, NULL),
    INT_("window", "y", win_y, SET_STATE, SET_APPLY_NONE, -100000, 100000, 0, NULL, NULL),
    INT_("window", "width", win_w, SET_STATE, SET_APPLY_NONE, 100, 100000, 0, NULL, NULL),
    INT_("window", "height", win_h, SET_STATE, SET_APPLY_NONE, 100, 100000, 0, NULL, NULL),
    BOOL_("window", "maximized", win_maximized, SET_STATE, SET_APPLY_NONE, 0),
};
#define NOPTS ((int)(sizeof opts / sizeof opts[0]))
/* the env name of show_fps is special (any nonzero number), so it is handled in settings_load */

int settings_count(void) { return NOPTS; }
const settings_opt* settings_opt_at(int i) { return i >= 0 && i < NOPTS ? &opts[i] : NULL; }

int settings_find(const char* section, const char* key) {
    for (int i = 0; i < NOPTS; i++)
        if (!_stricmp(opts[i].section, section) && !_stricmp(opts[i].key, key)) return i;
    return -1;
}

static int* field(ps2_settings_t* s, int i) { return (int*)((char*)s + opts[i].offset); }

int settings_get(const ps2_settings_t* s, int i) {
    if (i < 0 || i >= NOPTS || opts[i].type == SET_STRING) return 0;
    const int* f = (const int*)((const char*)s + opts[i].offset);
    if (opts[i].type == SET_ASPECT) return f[0] << 16 | (f[1] & 0xFFFF);
    return *f;
}

void settings_put(ps2_settings_t* s, int i, int v) {
    if (i < 0 || i >= NOPTS || opts[i].type == SET_STRING) return;
    int* f = field(s, i);
    if (opts[i].type == SET_ASPECT) { f[0] = v >> 16; f[1] = v & 0xFFFF; return; }
    *f = v;
}

/* "name=value|name=value": the value of word w (1), or 0 */
static int alias_lookup(const char* aliases, const char* w, int* v) {
    if (!aliases) return 0;
    for (const char* p = aliases; *p;) {
        const char* eq = strchr(p, '=');
        const char* bar = strchr(p, '|');
        if (!eq) break;
        if ((size_t)(eq - p) == strlen(w) && !_strnicmp(p, w, (size_t)(eq - p))) { *v = atoi(eq + 1); return 1; }
        if (!bar) break;
        p = bar + 1;
    }
    return 0;
}

/* the word of value v in aliases (the first one), or NULL */
static const char* alias_word(const char* aliases, int v, char* buf, size_t cap) {
    if (!aliases) return NULL;
    for (const char* p = aliases; *p;) {
        const char* eq = strchr(p, '=');
        const char* bar = strchr(p, '|');
        if (!eq) break;
        if (atoi(eq + 1) == v && (size_t)(eq - p) < cap) {
            memcpy(buf, p, (size_t)(eq - p));
            buf[eq - p] = 0;
            if (!isdigit((unsigned char)buf[0])) return buf;
        }
        if (!bar) break;
        p = bar + 1;
    }
    return NULL;
}

int settings_valid(int i, int v) {
    if (i < 0 || i >= NOPTS) return 0;
    const settings_opt* o = &opts[i];
    char w[32];
    switch (o->type) {
    case SET_BOOL: return v == 0 || v == 1;
    case SET_TRI: return v >= -1 && v <= 1;
    case SET_ENUM: return v >= o->lo && v <= o->hi;
    case SET_INT: return (v >= o->lo && v <= o->hi) || alias_word(o->aliases, v, w, sizeof w) != NULL;
    case SET_ASPECT: {
        int aw = v >> 16, ah = v & 0xFFFF;
        return aw > 0 && ah > 0 && aw >= ah && aw <= 4 * ah;
    }
    default: return 0;
    }
}

const char* settings_value_text(int i, int v, char* buf, size_t cap) {
    const settings_opt* o = settings_opt_at(i);
    if (!o || !cap) return "";
    buf[0] = 0;
    switch (o->type) {
    case SET_BOOL: snprintf(buf, cap, "%s", v ? "on" : "off"); break;
    case SET_TRI: snprintf(buf, cap, "%s", v < 0 ? "preset" : v ? "on" : "off"); break;
    case SET_ENUM: snprintf(buf, cap, "%s", v >= o->lo && v <= o->hi ? o->names[v] : o->names[0]); break;
    case SET_INT: {
        char w[32];
        const char* word = alias_word(o->aliases, v, w, sizeof w);
        if (word) snprintf(buf, cap, "%s", word);
        else snprintf(buf, cap, "%d", v);
        break;
    }
    case SET_ASPECT: {                   /* (stored reduced; written as people name them: 16:10, 21:9) */
        int w = v >> 16, h = v & 0xFFFF;
        if (w == 8 && h == 5) { w = 16; h = 10; }
        else if (w == 7 && h == 3) { w = 21; h = 9; }
        snprintf(buf, cap, "%d:%d", w, h);
        break;
    }
    default: break;
    }
    return buf;
}

static int parse_bool(const char* v, int* out) {
    static const char* yes[] = { "on", "1", "true", "yes" };
    static const char* no[] = { "off", "0", "false", "no" };
    for (int i = 0; i < 4; i++) {
        if (!_stricmp(v, yes[i])) { *out = 1; return 1; }
        if (!_stricmp(v, no[i])) { *out = 0; return 1; }
    }
    return 0;
}

static int parse_int(const char* v, long lo, long hi, int* out) {
    char* end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi) return 0;
    *out = (int)n;
    return 1;
}

int settings_parse_value(int i, const char* text, int* v) {
    const settings_opt* o = settings_opt_at(i);
    if (!o) return 0;
    switch (o->type) {
    case SET_BOOL: return parse_bool(text, v);
    case SET_TRI:
        if (!_stricmp(text, "preset") || !*text) { *v = -1; return 1; }
        return parse_bool(text, v);
    case SET_ENUM:
        for (int k = o->lo; k <= o->hi; k++)
            if (!_stricmp(text, o->names[k])) { *v = k; return 1; }
        return alias_lookup(o->aliases, text, v);
    case SET_INT:
        if (alias_lookup(o->aliases, text, v)) return 1;
        return parse_int(text, o->lo, o->hi, v);
    case SET_ASPECT: {
        int w, h;
        if (!settings_parse_aspect(text, &w, &h)) return 0;
        *v = w << 16 | h;
        return 1;
    }
    default: return 0;
    }
}

int settings_env_locked(int i) {
    const settings_opt* o = settings_opt_at(i);
    if (!o) return 0;
    const char* name = o->env;
    if (!name && !strcmp(o->key, "show_fps")) name = "PS2_SHOW_FPS";
    if (!name && !strcmp(o->key, "pacing")) name = "PS2_VBLANK";
    const char* e = name ? getenv(name) : NULL;
    return e && *e;
}

/* ---- presets --------------------------------------------------------------------------------------- */

static int is_43(const ps2_settings_t* s) { return s->aspect_w * 3 == s->aspect_h * 4; }

int settings_detect_preset(const ps2_settings_t* s) {
    if (s->motion_blur != -1 || s->flicker_filter != -1 || s->interlacing != -1 || s->scaler != SETTINGS_SCALER_AUTO)
        return SETTINGS_PRESET_CUSTOM;
    if (s->internal_scale == 1 && s->filters_faithful && !s->outlines_thin && is_43(s)) return SETTINGS_PRESET_ORIGINAL;
    if (s->filters_faithful) return SETTINGS_PRESET_CUSTOM;
    if (s->internal_scale == SETTINGS_SCALE_AUTO && !s->outlines_thin && s->auto_max == 2) return SETTINGS_PRESET_PERFORMANCE;
    if (s->internal_scale == SETTINGS_SCALE_AUTO && !s->outlines_thin && s->auto_max == SETTINGS_SCALE_AUTO_MAX)
        return SETTINGS_PRESET_BALANCED;
    if (s->internal_scale == SETTINGS_SCALE_NATIVE && s->outlines_thin) return SETTINGS_PRESET_QUALITY;
    return SETTINGS_PRESET_CUSTOM;
}

void settings_apply_preset(ps2_settings_t* s, int p) {
    if (p < 0 || p >= SETTINGS_PRESET_N) return;
    s->preset = p;
    if (p == SETTINGS_PRESET_CUSTOM) return;
    s->motion_blur = s->flicker_filter = s->interlacing = -1;
    s->scaler = SETTINGS_SCALER_AUTO;
    s->filters_faithful = p == SETTINGS_PRESET_ORIGINAL;
    s->outlines_thin = p == SETTINGS_PRESET_QUALITY;
    s->auto_max = SETTINGS_SCALE_AUTO_MAX;
    switch (p) {
    case SETTINGS_PRESET_ORIGINAL: s->internal_scale = 1; s->aspect_w = 4; s->aspect_h = 3; break;
    case SETTINGS_PRESET_PERFORMANCE: s->internal_scale = SETTINGS_SCALE_AUTO; s->auto_max = 2; break;
    case SETTINGS_PRESET_BALANCED: s->internal_scale = SETTINGS_SCALE_AUTO; break;
    default: s->internal_scale = SETTINGS_SCALE_NATIVE; break;
    }
}

/* ---- values ---------------------------------------------------------------------------------------- */

/* renderer / pacing values (sly2.ini and the PS2_GS_BACKEND / PS2_GS_PACING overrides) */
int settings_parse_renderer(const char* v, int* gpu) {
    if (!_stricmp(v, "software") || !_stricmp(v, "sw")) { *gpu = 0; return 1; }
    if (!_stricmp(v, "gpu") || !_stricmp(v, "hw") || !_stricmp(v, "d3d11")) { *gpu = 1; return 1; }
    return 0;
}
int settings_parse_pacing(const char* v, int* pacing) {
    if (!_stricmp(v, "vsync")) { *pacing = SETTINGS_PACING_AUTO; return 1; }   /* (the old default) */
    if (!_stricmp(v, "tearing")) { *pacing = SETTINGS_PACING_VRR; return 1; }
    for (int i = 0; i < SETTINGS_PACING_N; i++) if (!_stricmp(v, settings_pacing_names[i])) { *pacing = i; return 1; }
    return 0;
}

void settings_defaults(ps2_settings_t* s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < NOPTS; i++)
        if (opts[i].type != SET_STRING) settings_put(s, i, opts[i].def);
}

static int gcd(int a, int b) {
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

int settings_parse_aspect(const char* v, int* w, int* h) {
    while (*v == ' ' || *v == '\t') v++;
    char* end;
    double a = strtod(v, &end);
    if (end == v || !(a > 0)) return 0;
    double b = 1.0;
    if (*end == ':' || *end == '/' || *end == 'x' || *end == 'X') {
        const char* q = end + 1;
        b = strtod(q, &end);
        if (end == q || !(b > 0)) return 0;
    }
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return 0;
    double r = a / b;
    if (r < 1.0 || r > 4.0) return 0;              /* 1:1 .. 4:1: anything else is a typo */
    int iw, ih;
    if (a == (int)a && b == (int)b && a < 10000 && b < 10000) {
        iw = (int)a; ih = (int)b;
    } else {                                       /* a ratio like 1.7778: to 1/1000 */
        iw = (int)(r * 1000.0 + 0.5); ih = 1000;
    }
    int g = gcd(iw, ih);
    *w = iw / g;
    *h = ih / g;
    return 1;
}

static void warnf(settings_warn_fn warn, void* user, int line, const char* fmt, ...) {
    if (!warn) return;
    char msg[256];
    int n = snprintf(msg, sizeof msg, "line %d: ", line);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
    va_end(ap);
    warn(user, msg);
}

/* the accepted values of option i, for a warning */
static void expected(int i, char* out, size_t cap) {
    const settings_opt* o = &opts[i];
    out[0] = 0;
    switch (o->type) {
    case SET_BOOL: snprintf(out, cap, "on or off"); break;
    case SET_TRI: snprintf(out, cap, "on, off or preset"); break;
    case SET_ASPECT: snprintf(out, cap, "W:H (16:9, 4:3)"); break;
    case SET_ENUM: {
        size_t n = 0;
        for (int k = o->lo; k <= o->hi && n + 1 < cap; k++)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", k == o->lo ? "" : k == o->hi ? " or " : ", ", o->names[k]);
        break;
    }
    case SET_INT: {
        char w[32];
        const char* word = NULL;
        for (int k = -1; k <= 0 && !word; k++) word = alias_word(o->aliases, k, w, sizeof w);
        snprintf(out, cap, "%s%s%d..%d", word ? word : "", word ? " or " : "", o->lo, o->hi);
        break;
    }
    default: break;
    }
}

/* One key = value pair; returns the number of problems (0 or 1). *preset_seen: a [graphics] preset line. */
static int apply(ps2_settings_t* s, const char* section, const char* key, const char* v, settings_warn_fn warn, void* user,
                 int line, int* preset_seen) {
    int i = settings_find(section, key);
    if (i < 0 && !_stricmp(section, "controls")) return 0;   /* [controls]: runtime/input.c reads it (settings_read_section) */
    if (i < 0) { warnf(warn, user, line, "unknown setting '%s.%s' (ignored)", section, key); return 1; }
    if (opts[i].type == SET_STRING) {
        if (strlen(v) >= sizeof s->disc) { warnf(warn, user, line, "disc path too long (ignored)"); return 1; }
        strcpy(s->disc, v);
        return 0;
    }
    int x;
    if (!settings_parse_value(i, v, &x)) {
        char e[160];
        expected(i, e, sizeof e);
        warnf(warn, user, line, "%s '%s' is not %s (ignored)", key, v, e);
        return 1;
    }
    settings_put(s, i, x);
    if (!strcmp(opts[i].key, "preset")) *preset_seen = 1;
    return 0;
}

/* Cut a line at its comment (';' or '#' outside double quotes); returns where the comment began. */
static char* cut_comment(char* s) {
    int q = 0;
    for (char* c = s; *c; c++) {
        if (*c == '"') q = !q;
        else if (!q && (*c == ';' || *c == '#')) return c;
    }
    return NULL;
}

static char* trim(char* b) {
    while (isspace((unsigned char)*b)) b++;
    char* e = b + strlen(b);
    while (e > b && isspace((unsigned char)e[-1])) *--e = 0;
    return b;
}

int settings_parse(const char* text, ps2_settings_t* s, settings_warn_fn warn, void* user) {
    char section[32] = "display";                  /* keys before any [section] are display keys */
    int problems = 0, line = 0, preset_seen = 0;
    const char* p = text;
    while (p && *p) {
        line++;
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char buf[1200];
        if (len >= sizeof buf) {
            warnf(warn, user, line, "line too long (ignored)");
            problems++;
            p = eol ? eol + 1 : NULL;
            continue;
        }
        memcpy(buf, p, len);
        buf[len] = 0;
        p = eol ? eol + 1 : NULL;
        if (line == 1 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
            memmove(buf, buf + 3, strlen(buf + 3) + 1);   /* UTF-8 BOM (Notepad) */
        char* c = cut_comment(buf);                         /* comments: ';' or '#' to the end of the line */
        if (c) *c = 0;
        char* b = trim(buf);
        if (!*b) continue;
        if (*b == '[') {
            char* close = strchr(b, ']');
            if (!close || close[1] || close - b - 1 <= 0 || close - b - 1 >= (int)sizeof section) {
                warnf(warn, user, line, "bad section header '%s'", b);
                problems++;
                continue;
            }
            *close = 0;
            snprintf(section, sizeof section, "%s", b + 1);
            continue;
        }
        char* eq = strchr(b, '=');
        if (!eq) { warnf(warn, user, line, "'%s' is not key = value", b); problems++; continue; }
        *eq = 0;
        char* key = trim(b);
        char* v = trim(eq + 1);
        size_t vl = strlen(v);
        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }   /* "quoted value" */
        problems += apply(s, section, key, v, warn, user, line, &preset_seen);
    }
    /* a preset other than custom decides its options; otherwise the preset is what the options make */
    if (preset_seen && s->preset != SETTINGS_PRESET_CUSTOM) settings_apply_preset(s, s->preset);
    else s->preset = settings_detect_preset(s);
    return problems;
}

/* Every key = value of [section], in file order (comments, quotes and blanks as settings_parse
 * handles them). Returns the number of keys passed to fn. */
int settings_parse_section(const char* text, const char* section, settings_key_fn fn, void* user) {
    char cur[32] = "display";
    int n = 0, line = 0;
    for (const char* p = text; p && *p;) {
        line++;
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char buf[1200];
        int fits = len < sizeof buf;
        if (fits) { memcpy(buf, p, len); buf[len] = 0; }
        p = eol ? eol + 1 : NULL;
        if (!fits) continue;
        if (line == 1 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
            memmove(buf, buf + 3, strlen(buf + 3) + 1);
        char* c = cut_comment(buf);
        if (c) *c = 0;
        char* b = trim(buf);
        if (!*b) continue;
        if (*b == '[') {
            char* close = strchr(b, ']');
            if (close && !close[1] && close - b - 1 > 0 && close - b - 1 < (int)sizeof cur) { *close = 0; snprintf(cur, sizeof cur, "%s", b + 1); }
            continue;
        }
        char* eq = strchr(b, '=');
        if (!eq || _stricmp(cur, section)) continue;
        *eq = 0;
        char* key = trim(b);
        char* v = trim(eq + 1);
        size_t vl = strlen(v);
        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
        fn(user, key, v, line);
        n++;
    }
    return n;
}

/* A value as written to the file: in double quotes if it would not read back otherwise. */
static const char* ini_value(const char* v, char* buf, size_t cap) {
    size_t n = strlen(v);
    int quote = strpbrk(v, ";#") != NULL || (n && (isspace((unsigned char)v[0]) || isspace((unsigned char)v[n - 1]))) ||
                (n >= 2 && v[0] == '"' && v[n - 1] == '"');
    if (!quote) return v;
    snprintf(buf, cap, "\"%s\"", v);
    return buf;
}

static const char* text_of(const ps2_settings_t* s, const char* section, const char* key, char* buf, size_t cap) {
    int i = settings_find(section, key);
    return settings_value_text(i, settings_get(s, i), buf, cap);
}

int settings_format(const ps2_settings_t* s, char* out, size_t cap) {
    char disc[sizeof s->disc + 2];
    char win[256] = "";
    char t[24][24];
#define T(n, sec, key) text_of(s, sec, key, t[n], sizeof t[0])
    if (s->win_w > 0)
        snprintf(win, sizeof win, "x = %d\ny = %d\nwidth = %d\nheight = %d\nmaximized = %s\n", s->win_x, s->win_y,
                 s->win_w, s->win_h, s->win_maximized ? "on" : "off");
    return snprintf(out, cap,
        "; Sly 2 PC settings. Lines starting with ; are comments. Delete this file to get the defaults back.\n"
        "; Most of these can also be changed in the game: pause (START / Esc) -> PC Settings.\n"
        "\n"
        "[display]\n"
        "; Display aspect ratio: 16:9 (default) or 4:3. Other ratios (e.g. 21:9) work the same way.\n"
        "; 4:3 shows the game exactly as on a PS2. Environment override: PS2_ASPECT=4:3\n"
        "; F7 in the game switches between 16:9 and 4:3 (and remembers it here).\n"
        "aspect = %d:%d\n"
        "; Widescreen HUD: edges (default: the HUD's corner widgets move to the screen's edges) or\n"
        "; centered (the whole HUD stays in the centre 4:3 area). F8 in the game switches it.\n"
        "hud = %s\n"
        "; Monitor for fullscreen: 0 (default: the one the window is on) or 1, 2, ...\n"
        "monitor = %s\n"
        "; Fullscreen refresh rate: desktop (default: never changed), auto (a multiple of the game's 59.94 Hz, e.g. 120 Hz on a\n"
        "; 144/165 Hz display without G-Sync/FreeSync: no judder) or a rate in Hz; only while fullscreen and in front.\n"
        "fullscreen_refresh = %s\n"
        "; Frame rate: 60 (default: the PS2's 59.94 frames per second), 120, 144, 165, 200 (or any rate from 30 to 1000),\n"
        "; display (the display's refresh rate) or unlimited. The game runs at that rate; movies stay at the PS2's.\n"
        "frame_rate = %s\n"
        "\n"
        "[graphics]\n"
        "; Preset: original (the PS2's look: 1x, 4:3, TV filters), performance (2x, lowered on a slow\n"
        "; graphics card), balanced (default: 3x, lowered on a slow graphics card), quality (at least the\n"
        "; display's resolution, thin outlines) or custom. A preset other than custom sets the options\n"
        "; below when the game starts: set preset = custom to choose them one by one.\n"
        "preset = %s\n"
        "; Renderer: gpu (default: Direct3D 11, needs a Direct3D 11 graphics card - feature level 11_0) or\n"
        "; software (the PS2's renderer on the CPU, at the PS2's resolution). When the GPU renderer cannot\n"
        "; start, the software renderer is used and sly2.log says why. Environment override: PS2_GS_BACKEND=sw|hw\n"
        "renderer = %s\n"
        "; gpu renderer: frame pacing. auto (default: an even 59.94 Hz cadence - smooth on G-Sync/FreeSync displays;\n"
        "; on a fixed refresh each picture as soon as it is drawn, and 60/120/240 Hz displays drive the game), paced (the\n"
        "; even cadence always), immediate (lowest latency), display (the display's 60/120/240 Hz drives the game).\n"
        "pacing = %s\n"
        "; gpu renderer: vertical sync. on (default), off (tearing allowed), adaptive (synced unless a picture is late).\n"
        "vsync = %s\n"
        "; The settings below are used by the Direct3D 11 renderer (the software renderer ignores them, except\n"
        "; motion_blur, the game's own effect). They change while the game runs too: F2 / Shift+F2 scale,\n"
        "; Ctrl+F2 scale auto, F3 outlines, F4 filters preset, F5 motion blur, F6 flicker filter, Shift+F6 interlacing.\n"
        "; Internal resolution scale: auto (default: up to auto_max, lowered while the graphics card cannot keep\n"
        "; up), native (the smallest scale at least as high as the window) or 1 (PS2: 512x448) to 8.\n"
        "; Environment override: PS2_GS_SCALE=N\n"
        "internal_scale = %s\n"
        "auto_max = %s\n"
        "; Cel-shading outlines: ps2 (one PS2 pixel wide, exact) or thin (high-resolution lines).\n"
        "outlines = %s\n"
        "; Image filters preset: modern (motion blur on, no flicker filter, progressive) or\n"
        "; faithful (as on a PS2 on a TV). The three toggles below override the preset: on, off or preset.\n"
        "filters = %s\n"
        "motion_blur = %s\n"
        "flicker_filter = %s\n"
        "interlacing = %s\n"
        "; Picture to window: auto (default: Lanczos when shrinking, Catmull-Rom when enlarging), lanczos,\n"
        "; catmull-rom, box, bilinear or point.\n"
        "scaler = %s\n"
        "\n"
        "[audio]\n"
        "; Volumes 0..100. master: everything (the PC's mixer); music and effects: the game's own volumes.\n"
        "master = %s\n"
        "music = %s\n"
        "effects = %s\n"
        "; Keep playing sound while the game's window is not the active one: on (default) or off.\n"
        "background = %s\n"
        "\n"
        "[game]\n"
        "; Your Sly 2: Band of Thieves (USA) disc image (.iso). Asked for on the first start when empty.\n"
        "disc = %s\n"
        "\n"
        "[window]\n"
        "; Remembered when the game closes: fullscreen (off, on = borderless, exclusive; Alt+Enter or F11 switch)\n"
        "; and the window's place.\n"
        "fullscreen = %s\n"
        "; Show the frame rate in the title bar and in the top left corner of the picture.\n"
        "show_fps = %s\n"
        "%s",
        s->aspect_w, s->aspect_h, T(0, "display", "hud"), T(1, "display", "monitor"), T(16, "display", "fullscreen_refresh"), T(18, "display", "frame_rate"),
        settings_preset_names[settings_detect_preset(s)], T(2, "graphics", "renderer"), T(3, "graphics", "pacing"), T(17, "graphics", "vsync"),
        T(4, "graphics", "internal_scale"), T(5, "graphics", "auto_max"), T(6, "graphics", "outlines"), T(7, "graphics", "filters"),
        T(8, "graphics", "motion_blur"), T(9, "graphics", "flicker_filter"), T(10, "graphics", "interlacing"),
        T(11, "graphics", "scaler"),
        T(12, "audio", "master"), T(13, "audio", "music"), T(14, "audio", "effects"), T(15, "audio", "background"),
        ini_value(s->disc, disc, sizeof disc), T(21, "window", "fullscreen"), T(22, "window", "show_fps"), win);
#undef T
}

/* ---- key-by-key updates -------------------------------------------------------------------------- */

int settings_update_text(const char* text, const char* section, const char* key, const char* value,
                         char* out, size_t cap) {
    const char* nl = strstr(text, "\r\n") ? "\r\n" : "\n";
    char qbuf[1100];
    const char* v = ini_value(value, qbuf, sizeof qbuf);
    char cur[32] = "display";
    long found_at = -1, found_end = -1, insert_at = -1;
    const char* found_comment = NULL;
    size_t found_comment_len = 0;
    const char* p = text;
    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        size_t next = (size_t)(p - text) + len + (eol ? 1 : 0);
        char buf[1200];
        if (len < sizeof buf) {
            memcpy(buf, p, len);
            buf[len] = 0;
            if (p == text && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
                buf[0] = buf[1] = buf[2] = ' ';
            char* c = cut_comment(buf);
            size_t comment_off = c ? (size_t)(c - buf) : len;
            if (c) *c = 0;
            char* b = trim(buf);
            if (*b == '[') {
                char* close = strchr(b, ']');
                if (close && !close[1] && close - b - 1 > 0 && close - b - 1 < (int)sizeof cur) {
                    *close = 0;
                    snprintf(cur, sizeof cur, "%s", b + 1);
                    if (!_stricmp(cur, section)) insert_at = (long)next;
                }
            } else if (*b && strchr(b, '=')) {
                *strchr(b, '=') = 0;
                if (!_stricmp(cur, section)) {
                    insert_at = (long)next;
                    if (found_at < 0 && !_stricmp(trim(b), key)) {
                        found_at = (long)(p - text);
                        found_end = (long)(p - text) + (long)len;
                        if (len && p[len - 1] == '\r') found_end--;
                        if (comment_off < len) {
                            found_comment = p + comment_off;
                            found_comment_len = (size_t)(found_end - found_at) - comment_off;
                        }
                    }
                }
            }
        }
        p = text + next;
    }

    size_t tl = strlen(text), n = 0;
    char line[1300];
    if (found_comment) snprintf(line, sizeof line, "%s = %s   %.*s", key, v, (int)found_comment_len, found_comment);
    else snprintf(line, sizeof line, "%s = %s", key, v);
#define PUT(src, len) do { size_t l_ = (len); if (n + l_ >= cap) return -1; memcpy(out + n, (src), l_); n += l_; } while (0)
    if (found_at >= 0) {
        PUT(text, (size_t)found_at);
        PUT(line, strlen(line));
        PUT(text + found_end, tl - (size_t)found_end);
    } else if (insert_at >= 0) {
        PUT(text, (size_t)insert_at);
        if (insert_at > 0 && text[insert_at - 1] != '\n') PUT(nl, strlen(nl));
        PUT(line, strlen(line));
        PUT(nl, strlen(nl));
        PUT(text + insert_at, tl - (size_t)insert_at);
    } else {
        PUT(text, tl);
        if (tl && text[tl - 1] != '\n') PUT(nl, strlen(nl));
        size_t e = tl;                             /* a blank line before the new section, unless one is there */
        while (e > 0 && (text[e - 1] == '\n' || text[e - 1] == '\r')) e--;
        int newlines = 0;
        for (size_t k = e; k < tl; k++) newlines += text[k] == '\n';
        if (e > 0 && newlines < 2) PUT(nl, strlen(nl));
        char hdr[64];
        snprintf(hdr, sizeof hdr, "[%s]", section);
        PUT(hdr, strlen(hdr));
        PUT(nl, strlen(nl));
        PUT(line, strlen(line));
        PUT(nl, strlen(nl));
    }
#undef PUT
    out[n] = 0;
    return (int)n;
}

/* ---- changes (the menu, the hotkeys) ---------------------------------------------------------------- */

static settings_apply_fn appliers[SET_APPLY_N];

void settings_on_apply(int apply, settings_apply_fn fn) {
    if (apply > 0 && apply < SET_APPLY_N) appliers[apply] = fn;
}

void settings_touch(void) {
#ifdef _WIN32
    InterlockedIncrement(&settings_serial);
#else
    settings_serial++;
#endif
}

static void run_applier(int apply, int i) {
    settings_apply_fn fn = apply > 0 && apply < SET_APPLY_N ? appliers[apply] : NULL;
    if (fn) fn(i);
}

typedef struct { const char* kv[48][3]; char text[48][32]; int n; } kv_list;

static void kv_add(kv_list* l, int i) {
    if (i < 0 || settings_env_locked(i)) return;
    for (int k = 0; k < l->n; k++)
        if (!strcmp(l->kv[k][0], opts[i].section) && !strcmp(l->kv[k][1], opts[i].key)) {   /* (already listed: new value) */
            settings_value_text(i, settings_get(&ps2_settings, i), l->text[k], sizeof l->text[0]);
            return;
        }
    if (l->n >= 48) return;
    settings_value_text(i, settings_get(&ps2_settings, i), l->text[l->n], sizeof l->text[0]);
    l->kv[l->n][0] = opts[i].section;
    l->kv[l->n][1] = opts[i].key;
    l->kv[l->n][2] = l->text[l->n];
    l->n++;
}

int settings_set(const char* const kv[][3], int n);

int settings_change(int i, int v, int persist) {
    if (!settings_valid(i, v)) return 0;
    const settings_opt* o = &opts[i];
    static kv_list l;
    l.n = 0;
    int is_preset = !strcmp(o->key, "preset") && !strcmp(o->section, "graphics");
    int old_aspect = settings_get(&ps2_settings, settings_find("display", "aspect"));
    if (is_preset) {
        int prev = ps2_settings.preset;
        settings_apply_preset(&ps2_settings, v);
        /* leaving Original (4:3) for a modern preset: back to widescreen */
        if (v != SETTINGS_PRESET_ORIGINAL && v != SETTINGS_PRESET_CUSTOM && prev == SETTINGS_PRESET_ORIGINAL && is_43(&ps2_settings) &&
            !settings_env_locked(settings_find("display", "aspect"))) {
            ps2_settings.aspect_w = 16;
            ps2_settings.aspect_h = 9;
        }
        for (int k = 0; k < NOPTS; k++)
            if (opts[k].flags & SET_PRESET) kv_add(&l, k);
        kv_add(&l, i);
    } else {
        settings_put(&ps2_settings, i, v);
        kv_add(&l, i);
        if (o->flags & SET_PRESET || !strcmp(o->key, "aspect")) {
            int p = settings_detect_preset(&ps2_settings);
            if (p != ps2_settings.preset) {
                ps2_settings.preset = p;
                kv_add(&l, settings_find("graphics", "preset"));
            }
        }
    }
    int new_aspect = settings_get(&ps2_settings, settings_find("display", "aspect"));
    if (new_aspect != old_aspect && is_preset) kv_add(&l, settings_find("display", "aspect"));
#ifdef _WIN32
    if (persist && l.n && !settings_set((const char* const (*)[3])l.kv, l.n)) fprintf(stderr, "[settings] cannot write %s.%s to sly2.ini\n", o->section, o->key);
#else
    (void)persist;
#endif
    settings_touch();
    run_applier(o->apply, i);
    if (new_aspect != old_aspect && o->apply != SET_APPLY_DISPLAY) run_applier(SET_APPLY_DISPLAY, settings_find("display", "aspect"));
    return 1;
}

int settings_change_key(const char* section, const char* key, int v, int persist) {
    return settings_change(settings_find(section, key), v, persist);
}

void settings_reset_section(const char* section) {
    for (int i = 0; i < NOPTS; i++) {
        if (_stricmp(opts[i].section, section) || opts[i].flags & SET_STATE || opts[i].type == SET_STRING) continue;
        if (!strcmp(opts[i].key, "renderer") || !strcmp(opts[i].key, "pacing") || !strcmp(opts[i].key, "fullscreen")) continue;
        if (settings_env_locked(i)) continue;
        if (settings_get(&ps2_settings, i) != opts[i].def) settings_change(i, opts[i].def, 1);
    }
}

/* ---- the user data folder and the file in it ---------------------------------------------------- */

#ifdef _WIN32
static wchar_t user_dir[MAX_PATH + 64];
static int user_portable;

static int exe_dir(wchar_t* out, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return 0;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0; else out[0] = 0;
    return 1;
}

static void mkdir_p(const wchar_t* dir) {
    wchar_t tmp[MAX_PATH + 64];
    if (wcslen(dir) >= sizeof tmp / sizeof tmp[0]) return;
    wcscpy(tmp, dir);
    for (wchar_t* c = tmp + 1; *c; c++) {
        if (*c != L'\\' && *c != L'/') continue;
        if (c[-1] == L':') continue;                 /* "C:\" */
        wchar_t keep = *c;
        *c = 0;
        CreateDirectoryW(tmp, NULL);
        *c = keep;
    }
    CreateDirectoryW(tmp, NULL);
}

/* A console program (every development build and test) rather than the player's GUI build
 * (scripts/build_game.py --package): the subsystem in the executable's own PE header. */
static int exe_is_console(void) {
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 1;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI;
}

const wchar_t* settings_user_dir(void) {
    if (user_dir[0]) return user_dir;
    const size_t cap = sizeof user_dir / sizeof user_dir[0];
    wchar_t exe[MAX_PATH + 64] = L"";
    exe_dir(exe, MAX_PATH + 64);
    const wchar_t* env = _wgetenv(L"PS2_USER_DIR");
    wchar_t probe[MAX_PATH + 80];
    _snwprintf(probe, sizeof probe / sizeof probe[0], L"%ls\\portable.txt", exe);
    probe[sizeof probe / sizeof probe[0] - 1] = 0;
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (env && *env && wcslen(env) < cap - 16) {
        wcscpy(user_dir, env);
        user_portable = 1;
    } else if (exe[0] && (exe_is_console() || GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES)) {
        wcscpy(user_dir, exe);
        user_portable = 1;
    } else if (local && *local && wcslen(local) < cap - 16) {
        _snwprintf(user_dir, cap, L"%ls\\Sly2", local);
    } else {
        wcscpy(user_dir, exe[0] ? exe : L".");
        user_portable = 1;
    }
    size_t n = wcslen(user_dir);
    while (n > 3 && (user_dir[n - 1] == L'\\' || user_dir[n - 1] == L'/')) user_dir[--n] = 0;
    mkdir_p(user_dir);
    return user_dir;
}

int settings_portable(void) {
    settings_user_dir();
    return user_portable;
}

int settings_user_path(const wchar_t* name, wchar_t* out, size_t cap) {
    int n = _snwprintf(out, cap, L"%ls\\%ls", settings_user_dir(), name);
    return n > 0 && (size_t)n < cap;
}

static int settings_path(wchar_t* out, size_t cap) { return settings_user_path(L"sly2.ini", out, cap); }

static void log_warn(void* user, const char* msg) {
    fprintf(stderr, "[settings] %s: %s\n", (const char*)user, msg);
}

static size_t read_text(const wchar_t* path, char* text, size_t cap) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return (size_t)-1;
    size_t n = fread(text, 1, cap - 1, f);
    text[n] = 0;
    fclose(f);
    return n;
}

void settings_load(void) {
    settings_defaults(&ps2_settings);
    wchar_t path[MAX_PATH + 80];
    char upath[3 * (MAX_PATH + 80)] = "sly2.ini";
    if (settings_path(path, sizeof path / sizeof path[0])) {
        WideCharToMultiByte(CP_UTF8, 0, path, -1, upath, sizeof upath, NULL, NULL);
        static char text[65536];
        if (read_text(path, text, sizeof text) != (size_t)-1) settings_parse(text, &ps2_settings, log_warn, upath);
    }
    const char* e = getenv("PS2_ASPECT");
    if (e && *e) {
        int w, h;
        if (settings_parse_aspect(e, &w, &h)) { ps2_settings.aspect_w = w; ps2_settings.aspect_h = h; }
        else fprintf(stderr, "[settings] PS2_ASPECT='%s' is not W:H (16:9, 4:3): ignored\n", e);
    }
    e = getenv("PS2_HUD");
    if (e && *e) {
        if (!_stricmp(e, "edges")) ps2_settings.hud_centered = 0;
        else if (!_stricmp(e, "centered") || !_stricmp(e, "centred")) ps2_settings.hud_centered = 1;
        else fprintf(stderr, "[settings] PS2_HUD='%s' is not edges or centered: ignored\n", e);
    }
    e = getenv("PS2_SHOW_FPS");
    if (e && *e) ps2_settings.show_fps = atoi(e) != 0;
    e = getenv("PS2_GS_BACKEND");
    if (e && *e && !settings_parse_renderer(e, &ps2_settings.renderer_gpu))
        fprintf(stderr, "[settings] PS2_GS_BACKEND='%s' is not sw or hw: ignored\n", e);
    e = getenv("PS2_GS_PACING");
    if (e && *e && !settings_parse_pacing(e, &ps2_settings.pacing))
        fprintf(stderr, "[settings] PS2_GS_PACING='%s' is not auto, paced, immediate, display or vrr: ignored\n", e);
    e = getenv("PS2_GS_VSYNC");                  /* (pc-pacing) */
    if (e && *e) {
        int vi = settings_find("graphics", "vsync"), v;
        if (settings_parse_value(vi, e, &v)) ps2_settings.vsync = v;
        else fprintf(stderr, "[settings] PS2_GS_VSYNC='%s' is not on, off or adaptive: ignored\n", e);
    }
    e = getenv("PS2_FRAME_RATE");                /* (pc-fps) */
    if (e && *e) {
        int fi = settings_find("display", "frame_rate"), v;
        if (settings_parse_value(fi, e, &v)) ps2_settings.frame_rate = v;
        else fprintf(stderr, "[settings] PS2_FRAME_RATE='%s' is not 30..1000, display or unlimited: ignored\n", e);
    }
    e = getenv("PS2_VBLANK");                    /* (the design's name for the display-driven VBlank) */
    if (e && !_stricmp(e, "display")) ps2_settings.pacing = SETTINGS_PACING_DISPLAY;
    fprintf(stderr, "[settings] aspect %d:%d, hud %s, renderer %s%s, preset %s\n", ps2_settings.aspect_w, ps2_settings.aspect_h,
            ps2_settings.hud_centered ? "centered" : "edges", ps2_settings.renderer_gpu ? "gpu" : "software",
            getenv("PS2_GS_BACKEND") && *getenv("PS2_GS_BACKEND") ? " (PS2_GS_BACKEND)" : "",
            settings_preset_names[ps2_settings.preset]);
    settings_touch();
}

/* [display] as sly2.ini says now (the player may edit it while the game runs): 1 and aspect / hud
 * if the file is there. A key set by an environment override (PS2_ASPECT, PS2_HUD) keeps the
 * current value. */
int settings_read_display(int* aspect_w, int* aspect_h, int* hud_centered) {
    wchar_t path[MAX_PATH + 80];
    static char text[65536];
    if (!settings_path(path, sizeof path / sizeof path[0]) || read_text(path, text, sizeof text) == (size_t)-1) return 0;
    ps2_settings_t s;
    settings_defaults(&s);
    settings_parse(text, &s, NULL, NULL);
    const char* e = getenv("PS2_ASPECT");
    *aspect_w = e && *e ? ps2_settings.aspect_w : s.aspect_w;
    *aspect_h = e && *e ? ps2_settings.aspect_h : s.aspect_h;
    e = getenv("PS2_HUD");
    *hud_centered = e && *e ? ps2_settings.hud_centered : s.hud_centered;
    return 1;
}

int settings_read_graphics(ps2_settings_t* g) {
    wchar_t path[MAX_PATH + 80];
    static char text[65536];
    if (!settings_path(path, sizeof path / sizeof path[0]) || read_text(path, text, sizeof text) == (size_t)-1) return 0;
    ps2_settings_t s;
    settings_defaults(&s);
    s.aspect_w = g->aspect_w;                    /* (a preset in the file decides from the aspect in use) */
    s.aspect_h = g->aspect_h;
    settings_parse(text, &s, NULL, NULL);
    for (int i = 0; i < NOPTS; i++) {
        const settings_opt* o = &opts[i];
        int ours = !strcmp(o->section, "audio") || !strcmp(o->section, "controls") ||
                   (!strcmp(o->section, "graphics") && strcmp(o->key, "renderer") && strcmp(o->key, "pacing"));
        if (ours && !settings_env_locked(i)) settings_put(g, i, settings_get(&s, i));
    }
    g->preset = settings_detect_preset(g);
    settings_touch();
    return 1;
}

int settings_read_section(const char* section, settings_key_fn fn, void* user) {
    wchar_t path[MAX_PATH + 80];
    static char text[65536];
    static SRWLOCK lock = SRWLOCK_INIT;                 /* (text is shared) */
    if (!settings_path(path, sizeof path / sizeof path[0])) return 0;
    AcquireSRWLockExclusive(&lock);
    int ok = read_text(path, text, sizeof text) != (size_t)-1;
    if (ok) settings_parse_section(text, section, fn, user);
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

static int write_text(const wchar_t* path, const char* text, size_t n) {
    wchar_t tmp[MAX_PATH + 96];
    _snwprintf(tmp, sizeof tmp / sizeof tmp[0], L"%ls.tmp", path);
    tmp[sizeof tmp / sizeof tmp[0] - 1] = 0;
    FILE* f = _wfopen(tmp, L"wb");
    if (!f) return 0;
    int ok = fwrite(text, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp);
    return ok;
}

void settings_save_if_missing(void) {
    wchar_t path[MAX_PATH + 80];
    if (!settings_path(path, sizeof path / sizeof path[0]) || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    ps2_settings_t d;
    settings_defaults(&d);
    static char text[8192];
    int n = settings_format(&d, text, sizeof text);
    if (n > 0 && (size_t)n < sizeof text) write_text(path, text, (size_t)n);
}

int settings_set(const char* const kv[][3], int n) {
    static SRWLOCK lock = SRWLOCK_INIT;          /* (the window thread and the EE thread may both write) */
    wchar_t path[MAX_PATH + 80];
    if (!settings_path(path, sizeof path / sizeof path[0])) return 0;
    AcquireSRWLockExclusive(&lock);
    static char a[65536], b[65536];
    int ok = 1;
    if (read_text(path, a, sizeof a) == (size_t)-1) {
        ps2_settings_t d;
        settings_defaults(&d);
        int len = settings_format(&d, a, sizeof a);
        if (len < 0 || (size_t)len >= sizeof a) ok = 0;
    }
    for (int i = 0; ok && i < n; i++) {
        if (settings_update_text(a, kv[i][0], kv[i][1], kv[i][2], b, sizeof b) < 0) ok = 0;
        else memcpy(a, b, strlen(b) + 1);
    }
    if (ok) ok = write_text(path, a, strlen(a));
    ReleaseSRWLockExclusive(&lock);
    return ok;
}
#endif
