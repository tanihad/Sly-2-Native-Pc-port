/* User settings file (sly2.ini) and its environment overrides. See settings.h and notes/settings.md. */
#include "settings.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

ps2_settings_t ps2_settings = { 16, 9, 3, 0, 0, -1, -1, -1 };

void settings_defaults(ps2_settings_t* s) {
    s->aspect_w = 16;
    s->aspect_h = 9;
    s->internal_scale = 3;
    s->outlines_thin = 0;
    s->filters_faithful = 0;
    s->motion_blur = s->flicker_filter = s->interlacing = -1;
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

static int parse_bool(const char* v, int* out) {
    static const char* yes[] = { "on", "1", "true", "yes" };
    static const char* no[] = { "off", "0", "false", "no" };
    for (int i = 0; i < 4; i++) {
        if (!_stricmp(v, yes[i])) { *out = 1; return 1; }
        if (!_stricmp(v, no[i])) { *out = 0; return 1; }
    }
    return 0;
}

/* One key = value pair (section already folded into key as "section.key"). */
static int apply(ps2_settings_t* s, const char* key, const char* v, settings_warn_fn warn, void* user, int line) {
    int b;
    if (!_stricmp(key, "display.aspect")) {
        int w, h;
        if (!settings_parse_aspect(v, &w, &h)) { warnf(warn, user, line, "aspect '%s' is not W:H (16:9, 4:3)", v); return 1; }
        s->aspect_w = w; s->aspect_h = h;
        return 0;
    }
    if (!_stricmp(key, "graphics.internal_scale")) {
        char* end;
        long n = strtol(v, &end, 10);
        if (*end || n < 1 || n > 8) { warnf(warn, user, line, "internal_scale '%s' is not 1..8", v); return 1; }
        s->internal_scale = (int)n;
        return 0;
    }
    if (!_stricmp(key, "graphics.outlines")) {
        if (!_stricmp(v, "ps2")) s->outlines_thin = 0;
        else if (!_stricmp(v, "thin")) s->outlines_thin = 1;
        else { warnf(warn, user, line, "outlines '%s' is not ps2 or thin", v); return 1; }
        return 0;
    }
    if (!_stricmp(key, "graphics.filters")) {
        if (!_stricmp(v, "modern")) s->filters_faithful = 0;
        else if (!_stricmp(v, "faithful")) s->filters_faithful = 1;
        else { warnf(warn, user, line, "filters '%s' is not modern or faithful", v); return 1; }
        return 0;
    }
    int* toggle = !_stricmp(key, "graphics.motion_blur") ? &s->motion_blur
                : !_stricmp(key, "graphics.flicker_filter") ? &s->flicker_filter
                : !_stricmp(key, "graphics.interlacing") ? &s->interlacing : NULL;
    if (toggle) {
        if (!_stricmp(v, "preset") || !*v) { *toggle = -1; return 0; }
        if (!parse_bool(v, &b)) { warnf(warn, user, line, "'%s' is not on/off/preset", v); return 1; }
        *toggle = b;
        return 0;
    }
    warnf(warn, user, line, "unknown setting '%s' (ignored)", key);
    return 1;
}

int settings_parse(const char* text, ps2_settings_t* s, settings_warn_fn warn, void* user) {
    char section[32] = "display";                  /* keys before any [section] are display keys */
    int problems = 0, line = 0;
    const char* p = text;
    while (p && *p) {
        line++;
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char buf[256];
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
        for (char* c = buf; *c; c++)                       /* comments: ';' or '#' to the end of the line */
            if (*c == ';' || *c == '#') { *c = 0; break; }
        char* b = buf;
        while (isspace((unsigned char)*b)) b++;
        char* e = b + strlen(b);
        while (e > b && isspace((unsigned char)e[-1])) *--e = 0;
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
        char* key = b;
        char* ke = eq;
        while (ke > key && isspace((unsigned char)ke[-1])) *--ke = 0;
        char* v = eq + 1;
        while (isspace((unsigned char)*v)) v++;
        char full[96];
        snprintf(full, sizeof full, "%s.%s", section, key);
        problems += apply(s, full, v, warn, user, line);
    }
    return problems;
}

int settings_format(const ps2_settings_t* s, char* out, size_t cap) {
    static const char* tog[] = { "preset", "off", "on" };
    return snprintf(out, cap,
        "; Sly 2 PC settings. Lines starting with ; are comments. Delete this file to get the defaults back.\n"
        "\n"
        "[display]\n"
        "; Display aspect ratio: 16:9 (default) or 4:3. Other ratios (e.g. 21:9) work the same way.\n"
        "; 4:3 shows the game exactly as on a PS2. Environment override: PS2_ASPECT=4:3\n"
        "aspect = %d:%d\n"
        "\n"
        "[graphics]\n"
        "; Used by the Direct3D 11 renderer (the software renderer ignores them).\n"
        "; Internal resolution scale, 1 (PS2: 512x448) to 8.\n"
        "internal_scale = %d\n"
        "; Cel-shading outlines: ps2 (one PS2 pixel wide, exact) or thin (high-resolution lines).\n"
        "outlines = %s\n"
        "; Image filters preset: modern (motion blur on, no flicker filter, progressive) or\n"
        "; faithful (as on a PS2 on a TV). The three toggles below override the preset: on, off or preset.\n"
        "filters = %s\n"
        "motion_blur = %s\n"
        "flicker_filter = %s\n"
        "interlacing = %s\n",
        s->aspect_w, s->aspect_h, s->internal_scale, s->outlines_thin ? "thin" : "ps2",
        s->filters_faithful ? "faithful" : "modern", tog[s->motion_blur + 1], tog[s->flicker_filter + 1],
        tog[s->interlacing + 1]);
}

/* ---- file next to the executable ---------------------------------------------------------------- */

#ifdef _WIN32
static int settings_path(char* out, size_t cap) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return 0;
    char* slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0; else exe[0] = 0;
    return snprintf(out, cap, "%ssly2.ini", exe) < (int)cap;
}

static void log_warn(void* user, const char* msg) {
    fprintf(stderr, "[settings] %s: %s\n", (const char*)user, msg);
}

void settings_load(void) {
    settings_defaults(&ps2_settings);
    char path[MAX_PATH + 16];
    if (settings_path(path, sizeof path)) {
        FILE* f = fopen(path, "rb");
        if (f) {
            static char text[16384];
            size_t n = fread(text, 1, sizeof text - 1, f);
            text[n] = 0;
            fclose(f);
            settings_parse(text, &ps2_settings, log_warn, path);
        }
    }
    const char* e = getenv("PS2_ASPECT");
    if (e && *e) {
        int w, h;
        if (settings_parse_aspect(e, &w, &h)) { ps2_settings.aspect_w = w; ps2_settings.aspect_h = h; }
        else fprintf(stderr, "[settings] PS2_ASPECT='%s' is not W:H (16:9, 4:3): ignored\n", e);
    }
    fprintf(stderr, "[settings] aspect %d:%d\n", ps2_settings.aspect_w, ps2_settings.aspect_h);
}

void settings_save_if_missing(void) {
    char path[MAX_PATH + 16];
    if (!settings_path(path, sizeof path) || GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    ps2_settings_t d;
    settings_defaults(&d);
    char text[4096];
    int n = settings_format(&d, text, sizeof text);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, (size_t)n, f);
    fclose(f);
}
#endif
