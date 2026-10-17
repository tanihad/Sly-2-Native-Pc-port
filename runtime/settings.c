/* User settings file (sly2.ini) and its environment overrides. See settings.h and notes/settings.md. */
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

ps2_settings_t ps2_settings = { 16, 9, 0, 0, 0, 3, 0, 0, -1, -1, -1, 0, "", 0, 0, 0, 0, 0, 0, 0 };
const char* const settings_scaler_names[SETTINGS_SCALER_N] = { "auto", "point", "box", "lanczos", "catmull-rom", "bilinear" };

/* renderer / pacing values (sly2.ini and the PS2_GS_BACKEND / PS2_GS_PACING overrides) */
int settings_parse_renderer(const char* v, int* gpu) {
    if (!_stricmp(v, "software") || !_stricmp(v, "sw")) { *gpu = 0; return 1; }
    if (!_stricmp(v, "gpu") || !_stricmp(v, "hw") || !_stricmp(v, "d3d11")) { *gpu = 1; return 1; }
    return 0;
}
int settings_parse_pacing(const char* v, int* pacing) {
    if (!_stricmp(v, "vsync")) { *pacing = SETTINGS_PACING_VSYNC; return 1; }
    if (!_stricmp(v, "vrr") || !_stricmp(v, "tearing")) { *pacing = SETTINGS_PACING_VRR; return 1; }
    if (!_stricmp(v, "display")) { *pacing = SETTINGS_PACING_DISPLAY; return 1; }
    return 0;
}

void settings_defaults(ps2_settings_t* s) {
    memset(s, 0, sizeof *s);
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

static int parse_int(const char* v, long lo, long hi, int* out) {
    char* end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi) return 0;
    *out = (int)n;
    return 1;
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
    if (!_stricmp(key, "display.hud")) {
        if (!_stricmp(v, "edges")) s->hud_centered = 0;
        else if (!_stricmp(v, "centered") || !_stricmp(v, "centred")) s->hud_centered = 1;
        else { warnf(warn, user, line, "hud '%s' is not edges or centered", v); return 1; }
        return 0;
    }
    if (!_stricmp(key, "graphics.renderer")) {
        if (!settings_parse_renderer(v, &s->renderer_gpu)) { warnf(warn, user, line, "renderer '%s' is not software or gpu", v); return 1; }
        return 0;
    }
    if (!_stricmp(key, "graphics.pacing")) {
        if (!settings_parse_pacing(v, &s->pacing)) { warnf(warn, user, line, "pacing '%s' is not vsync, vrr or display", v); return 1; }
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
    if (!_stricmp(key, "graphics.scaler")) {
        for (int i = 0; i < SETTINGS_SCALER_N; i++)
            if (!_stricmp(v, settings_scaler_names[i])) { s->scaler = i; return 0; }
        if (!_stricmp(v, "catmull")) { s->scaler = SETTINGS_SCALER_CATMULL; return 0; }
        warnf(warn, user, line, "scaler '%s' is not auto, lanczos, catmull-rom, box, bilinear or point", v);
        return 1;
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
    if (!_stricmp(key, "game.disc")) {
        if (strlen(v) >= sizeof s->disc) { warnf(warn, user, line, "disc path too long (ignored)"); return 1; }
        strcpy(s->disc, v);
        return 0;
    }
    int* coord = !_stricmp(key, "window.x") ? &s->win_x : !_stricmp(key, "window.y") ? &s->win_y : NULL;
    if (coord) {
        if (!parse_int(v, -100000, 100000, coord)) { warnf(warn, user, line, "'%s' is not a position", v); return 1; }
        return 0;
    }
    int* size = !_stricmp(key, "window.width") ? &s->win_w : !_stricmp(key, "window.height") ? &s->win_h : NULL;
    if (size) {
        if (!parse_int(v, 100, 100000, size)) { warnf(warn, user, line, "'%s' is not a window size", v); return 1; }
        return 0;
    }
    int* flag = !_stricmp(key, "window.maximized") ? &s->win_maximized
              : !_stricmp(key, "window.fullscreen") ? &s->win_fullscreen
              : !_stricmp(key, "window.show_fps") ? &s->show_fps : NULL;
    if (flag) {
        if (!parse_bool(v, flag)) { warnf(warn, user, line, "'%s' is not on/off", v); return 1; }
        return 0;
    }
    warnf(warn, user, line, "unknown setting '%s' (ignored)", key);
    return 1;
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
    int problems = 0, line = 0;
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
        char full[96];
        snprintf(full, sizeof full, "%s.%s", section, key);
        problems += apply(s, full, v, warn, user, line);
    }
    return problems;
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

int settings_format(const ps2_settings_t* s, char* out, size_t cap) {
    static const char* tog[] = { "preset", "off", "on" };
    char disc[sizeof s->disc + 2];
    char win[256] = "";
    if (s->win_w > 0)
        snprintf(win, sizeof win, "x = %d\ny = %d\nwidth = %d\nheight = %d\nmaximized = %s\n", s->win_x, s->win_y,
                 s->win_w, s->win_h, s->win_maximized ? "on" : "off");
    return snprintf(out, cap,
        "; Sly 2 PC settings. Lines starting with ; are comments. Delete this file to get the defaults back.\n"
        "\n"
        "[display]\n"
        "; Display aspect ratio: 16:9 (default) or 4:3. Other ratios (e.g. 21:9) work the same way.\n"
        "; 4:3 shows the game exactly as on a PS2. Environment override: PS2_ASPECT=4:3\n"
        "; F7 in the game switches between 16:9 and 4:3 (and remembers it here).\n"
        "aspect = %d:%d\n"
        "; Widescreen HUD: edges (default: the HUD's corner widgets move to the screen's edges) or\n"
        "; centered (the whole HUD stays in the centre 4:3 area). F8 in the game switches it.\n"
        "hud = %s\n"
        "\n"
        "[graphics]\n"
        "; Renderer: software (default) or gpu (Direct3D 11, needs a GPU with rasterizer-ordered views;\n"
        "; falls back to software when unavailable). Environment override: PS2_GS_BACKEND=sw|hw\n"
        "renderer = %s\n"
        "; gpu renderer: frame pacing. vsync (default), vrr (G-Sync/FreeSync: every picture shown once,\n"
        "; no wait for the display), display (the game runs at the display's 60 Hz instead of 59.94 Hz).\n"
        "pacing = %s\n"
        "; The settings below are used by the Direct3D 11 renderer (the software renderer ignores them, except\n"
        "; motion_blur, the game's own effect). They change while the game runs too: F2 / Shift+F2 scale,\n"
        "; F3 outlines, F4 filters preset, F5 motion blur, F6 flicker filter, Shift+F6 interlacing.\n"
        "; Internal resolution scale, 1 (PS2: 512x448) to 8. Environment override: PS2_GS_SCALE=N\n"
        "internal_scale = %d\n"
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
        "[game]\n"
        "; Your Sly 2: Band of Thieves (USA) disc image (.iso). Asked for on the first start when empty.\n"
        "disc = %s\n"
        "\n"
        "[window]\n"
        "; Remembered when the game closes: fullscreen (Alt+Enter or F11 switch) and the window's place.\n"
        "fullscreen = %s\n"
        "; Show the frame rate in the title bar.\n"
        "show_fps = %s\n"
        "%s",
        s->aspect_w, s->aspect_h, s->hud_centered ? "centered" : "edges", s->renderer_gpu ? "gpu" : "software",
        s->pacing == SETTINGS_PACING_VRR ? "vrr" : s->pacing == SETTINGS_PACING_DISPLAY ? "display" : "vsync",
        s->internal_scale, s->outlines_thin ? "thin" : "ps2",
        s->filters_faithful ? "faithful" : "modern", tog[s->motion_blur + 1], tog[s->flicker_filter + 1],
        tog[s->interlacing + 1], settings_scaler_names[s->scaler >= 0 && s->scaler < SETTINGS_SCALER_N ? s->scaler : 0],
        ini_value(s->disc, disc, sizeof disc), s->win_fullscreen ? "on" : "off",
        s->show_fps ? "on" : "off", win);
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
        fprintf(stderr, "[settings] PS2_GS_PACING='%s' is not vsync, vrr or display: ignored\n", e);
    e = getenv("PS2_VBLANK");                    /* (the design's name for the display-driven VBlank) */
    if (e && !_stricmp(e, "display")) ps2_settings.pacing = SETTINGS_PACING_DISPLAY;
    fprintf(stderr, "[settings] aspect %d:%d, hud %s, renderer %s\n", ps2_settings.aspect_w, ps2_settings.aspect_h,
            ps2_settings.hud_centered ? "centered" : "edges", ps2_settings.renderer_gpu ? "gpu" : "software");
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
    settings_parse(text, &s, NULL, NULL);
    g->internal_scale = s.internal_scale;
    g->outlines_thin = s.outlines_thin;
    g->filters_faithful = s.filters_faithful;
    g->motion_blur = s.motion_blur;
    g->flicker_filter = s.flicker_filter;
    g->interlacing = s.interlacing;
    g->scaler = s.scaler;
    return 1;
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
    wchar_t path[MAX_PATH + 80];
    if (!settings_path(path, sizeof path / sizeof path[0])) return 0;
    static char a[65536], b[65536];
    if (read_text(path, a, sizeof a) == (size_t)-1) {
        ps2_settings_t d;
        settings_defaults(&d);
        int len = settings_format(&d, a, sizeof a);
        if (len < 0 || (size_t)len >= sizeof a) return 0;
    }
    for (int i = 0; i < n; i++) {
        if (settings_update_text(a, kv[i][0], kv[i][1], kv[i][2], b, sizeof b) < 0) return 0;
        memcpy(a, b, strlen(b) + 1);
    }
    return write_text(path, a, strlen(a));
}
#endif
