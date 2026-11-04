/* Window, presentation and input.
 *
 * The window lives on its own OS thread with its own message loop. The emulation thread hands
 * finished frames over through a triple buffer: window_present copies the picture into the free
 * buffer, publishes it with a pointer swap under a lock and returns at once. The window thread
 * then shows the newest frame, fitted with black bars at the frame's display aspect (the aspect
 * from sly2.ini - 16:9 by default, the 512-wide picture stretched - or 4:3 for movies and plain
 * window_present callers; notes/widescreen.md), with Direct3D 11 (one textured triangle,
 * bilinear filtering, vsync'd flip-model present) or, if Direct3D 11 is unavailable, with GDI
 * StretchDIBits. Alt+Enter / F11 toggle borderless fullscreen. The window opens where it was when
 * it was last closed (size, position, maximized, fullscreen: [window] in sly2.ini), else centred at
 * a default size; closing it writes that state back and ends the process (through
 * window_exit_hook: launch.c lets a memory card operation in progress finish first).
 * Persistence of the user settings is the window's: it writes the sly2.ini template when it opens
 * and none exists (settings.c, notes/settings.md).
 *
 * Input (window_get_input, called on the emulation thread): runtime/input.c (controllers of every
 * kind, the keyboard while the window has focus, mapping in notes/controls.md), then the PS2_INPUT
 * test scripts below. The window procedure hands every message to input_window_message first (the
 * Options menu's keys and mouse, binding capture).
 *
 * Every DLL beyond kernel32/user32/gdi32 (d3d11, d3dcompiler_47, dwmapi) is loaded at run
 * time: nothing needs adding to the link line, and a missing component only disables its feature.
 *
 * Environment: PS2_WINDOW_GDI=1 forces the GDI presenter.
 */
#define COBJMACROS
#include "aspect.h"
#include "hw.h"
#include "settings.h"
#include "display_mode.h"                 /* (pc-pacing) fullscreen refresh rate */
#include "gfx.h"
#include "input.h"
#include "input_internal.h"
#include "window_gpu.h"

#include <stdarg.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define FRAME_MAX        1024            /* gs_display never produces more than 1024x1024 */
#define DEFAULT_CLIENT_W 1280
#define DEFAULT_CLIENT_H 960
#define WINDOW_CLASS     L"Sly2Window"
#define WINDOW_TITLE     "Sly 2: Band of Thieves"

void (*window_exit_hook)(int code);      /* set by main (launch_exit); NULL: ExitProcess */
#define LIVE_RESIZE_TIMER 1

static void wlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[window] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

static void* dll_proc(HMODULE m, const char* name) { return m ? (void*)GetProcAddress(m, name) : NULL; }

/* ---- scripted input for automated runs ----------------------------------------------------------
 * PS2_INPUT="<frame>:<button>[:<frames held>],..." presses buttons on port 0, e.g.
 * "700:circle,900:start:10". Frames are counted by port-0 reads (padman reads once per VBlank).
 * Buttons: cross circle square triangle start select up down left right l1 l2 r1 r2 l3 r3.
 * Sticks (full deflection): lup ldown lleft lright (left stick), rup rdown rleft rright. */

static const struct { const char* name; uint16_t bit; int pressure; } script_buttons[] = {
    { "cross", PAD_CROSS, 6 }, { "circle", PAD_CIRCLE, 5 }, { "square", PAD_SQUARE, 7 },
    { "triangle", PAD_TRIANGLE, 4 }, { "start", PAD_START, -1 }, { "select", PAD_SELECT, -1 },
    { "up", PAD_UP, 2 }, { "down", PAD_DOWN, 3 }, { "left", PAD_LEFT, 1 }, { "right", PAD_RIGHT, 0 },
    { "l1", PAD_L1, 8 }, { "r1", PAD_R1, 9 }, { "l2", PAD_L2, 10 }, { "r2", PAD_R2, 11 },
    { "l3", PAD_L3, -1 }, { "r3", PAD_R3, -1 },
};
#define SCRIPT_NBUTTONS ((int)(sizeof script_buttons / sizeof script_buttons[0]))
/* stick entries follow the buttons in the script's index space; axis: 1 lx, 2 ly, 3 rx, 4 ry */
static const struct { const char* name; int axis, value; } script_sticks[] = {
    { "lup", 2, 0x00 }, { "ldown", 2, 0xFF }, { "lleft", 1, 0x00 }, { "lright", 1, 0xFF },
    { "rup", 4, 0x00 }, { "rdown", 4, 0xFF }, { "rleft", 3, 0x00 }, { "rright", 3, 0xFF },
};
/* Besides absolute frames, a token can start relative to the token before it:
 *   "+<n>:<button>[:<hold>]"            n frames after the previous token started;
 *   "@<hexaddr>[/<hexra>][+<n>]:<button>[:<hold>]"
 *       n frames after the recompiled function <hexaddr> was called (from the call site returning
 *       to <hexra>, if given) once the previous token has started, e.g. "@187D80/1DD96C+30:start"
 *       = START 30 frames after the title screen first checks for START.
 *   "@!<hexaddr>[/<hexra>][+<n>]:<button>[:<hold>]"
 *       once the function has been called and then not called for n frames (default 30), e.g.
 *       "@!227820+60:none" = when the Binocucom dialogue has been gone for 60 frames.
 * Anchors need the debug build (function tracing), except on the functions with an entry hook
 * (recomp/hooks.py: @187D80/1DD96C, the title's START, works in release builds); the runtime
 * counts their calls (ps2_func_calls). A token waits until the one before it has started. The button "none" presses
 * nothing (a pure wait point for the tokens after it). Partial stick deflection: "lxHH", "lyHH",
 * "rxHH", "ryHH" set that axis to the hex value HH (00 left/up, 80 centre, FF right/down).
 * "ram" writes EE RAM to <dump dir>/ram_<vblank>.bin (debugging). "release" ends every press and
 * stick hold still in progress (e.g. a long "r1:30000" paraglider hold in a live session).
 * "calls:<n>" prints the recompiled functions called in the next n VBlanks, like PS2_CALLS, and
 * "watch:<n>" logs every call of the PS2_WATCH functions in the next n VBlanks (debug builds).
 * "rec" starts the PS2_GS_RECORD GS recording at the next VBlank (gs_record.c; with
 * PS2_GS_RECORD_FROM=-1 only this starts it).
 * "aspect" and "hud" switch the display aspect / the HUD mode as F7 / F8 do (aspect.c).
 * "scaleup", "scaledown", "outlines", "filters", "blur", "flicker", "interlace" change the graphics settings as
 * F2, Shift+F2, F3, F4, F5, F6, Shift+F6 do (gfx.h), without writing sly2.ini.
 * "renderer" switches between the GPU and the software renderer, "losedevice" simulates a GPU device loss
 * (the debug keys F9 / Shift+F9 of window_gpu.h; GPU renderer only).
 *   "t<seconds>:<button>[:<hold>]"      at a wall-clock time since the first pad read, e.g.
 *       "t95:start": scenes synchronised to sound (the intro cutscene follows its music stream)
 *       last a fixed real time, however fast the game runs.
 *   "r<seconds>:<button>[:<hold>]"      (pc-fps) that many seconds after the previous token started.
 *   A hold ending in "s" is in seconds ("r0.5:lup:2.5s"): the same real time at any frame rate (frame_rate.h; pad
 *   frames follow the VBlanks, which follow the frame rate).
 *
 * PS2_INPUT_FILE=<path>: live input. Lines appended to that file while the game runs are read
 * every few frames and added to the script; relative tokens in a new line count from the moment
 * it is read ("+0:lup:60,+60:cross"), so a running game can be steered step by step (anchors work
 * there too: the runtime starts counting a function's calls when the script first asks for them).
 * Lines may be of any length; finished tokens are dropped when a line arrives, so a session can run
 * for hours (at most SCRIPT_MAX tokens pending at a time).
 * "pause" (live input only) stops the game at the next VBlank whose picture is dumped
 * (PS2_DUMP_EVERY; latest.png then shows it) until the next line arrives, e.g. "+0:r1:20,+20:pause":
 * real-time sections can be played step by step however long it takes to look at a frame. The EE
 * waits inside the pad read; meanwhile the whole PS2 stands still (ps2_debug_hold: VBlank clock,
 * COP0 Count, EE timers, IOP time, SPU2 mixer), so no game time passes during the wait. */
enum { SCRIPT_ABS, SCRIPT_REL, SCRIPT_ANCHOR, SCRIPT_QUIET, SCRIPT_TIME, SCRIPT_RTIME };
#define SCRIPT_MAX   16384
static double script_at[SCRIPT_MAX];       /* SCRIPT_TIME tokens: seconds since the first pad read */
static double script_t0;

static double script_seconds(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
#define SCRIPT_UNSET 0xFFFFFFFFu
#define SCRIPT_NONE  (-2)
#define SCRIPT_AXIS  (-3)                 /* lxHH etc.: axis/value in the token */
#define SCRIPT_RAM   (-4)                 /* "ram": dump EE RAM (debug) */
#define SCRIPT_REC   (-11)                /* "rec": start the GS recording (gs_record.c) */
#define SCRIPT_GPUSWITCH (-12)            /* "renderer": GPU <-> software renderer (as F9, window_gpu.h) */
#define SCRIPT_GPULOSE (-13)              /* "losedevice": simulate a GPU device loss (as Shift+F9) */
static void script_debug_key(int key);
void gs_record_start_now(void);
#define SCRIPT_PAUSE (-5)                 /* "pause": stop until the next live input line */
#define SCRIPT_RELEASE (-6)               /* "release": end the holds in progress */
#define SCRIPT_CALLS (-7)                 /* "calls:<n>": call profile of the next n VBlanks (debug) */
#define SCRIPT_WATCH (-8)                 /* "watch:<n>": PS2_WATCH logs every call for n VBlanks */
#define SCRIPT_ASPECT (-9)                /* "aspect": as F7 (wide <-> 4:3) */
#define SCRIPT_HUD   (-10)                /* "hud": as F8 (HUD edges <-> centred) */
/* graphics keys (gfx.h): "scaleup" F2, "scaledown" Shift+F2, "scaleauto" Ctrl+F2, "outlines" F3, "filters" F4, "blur" F5,
 * "flicker" F6, "interlace" Shift+F6 (not written to sly2.ini) */
#define SCRIPT_GFX0  (-20)
static const struct { const char* name; unsigned vk; int shift; } script_gfx[] = {
    { "scaleup", VK_F2, 0 }, { "scaledown", VK_F2, 1 }, { "outlines", VK_F3, 0 }, { "filters", VK_F4, 0 },
    { "blur", VK_F5, 0 }, { "flicker", VK_F6, 0 }, { "interlace", VK_F6, 1 }, { "scaleauto", VK_F2, 2 } };
#define SCRIPT_NGFX ((int)(sizeof script_gfx / sizeof script_gfx[0]))
/* (pc-menu) the Options menu (menu_game.c menu_script): "menu" opens it as Esc does (or closes it), "mup" "mdown" "mleft"
 * "mright" "mok" "mback" "mtabn" "mtabp" navigate as a controller does, "menushot" writes the next presented picture with
 * the menu to PS2_MENU_SHOTS (folder; default the frame dump folder or .) as menu_<n>.png, (pc-polish) "mquit" = the
 * window's close button (Quit to desktop?; a second one quits), (pc-shot) "screenshot" = F12 (menu_overlay.h) */
#define SCRIPT_MENU0 (-40)
static const char* const script_menu[] = { "menu", "mup", "mdown", "mleft", "mright", "mok", "mback", "mtabn", "mtabp", "menushot",
                                           "mquit", "screenshot" };
#define SCRIPT_NMENU ((int)(sizeof script_menu / sizeof script_menu[0]))
extern void menu_script(int k) __attribute__((weak));
extern void overlay_request_screenshot(void) __attribute__((weak));
static void switch_aspect(void);
static void switch_hud(void);
static int script_pause_req;
void ps2_request_ram_dump(void);
void ps2_request_calls_window(uint32_t frames);
void ps2_request_watch_window(uint32_t frames);
void ps2_debug_hold(int on);              /* runtime: EE timers, IOP time and the SPU2 stand still */
static struct { uint32_t frame, hold, start, addr, ra, base, last; int button, kind, armed, seen, axis, value;
                double hold_s, start_s; }     /* (pc-fps) hold_s > 0: held for that many seconds ("...:2.5s") */
    script[SCRIPT_MAX];
static int script_n = -1;
static uint32_t script_frame;
uint32_t ps2_func_calls(uint32_t addr, uint32_t ra);   /* runtime: calls of an anchor so far */
uint32_t ps2_vblank_count(void);

static void script_parse(const char* s) {
    size_t len = strlen(s ? s : "");
    char* buf = malloc(len + 1);
    if (!buf) return;
    memcpy(buf, s ? s : "", len + 1);
    for (char* tok = strtok(buf, ", \t\r\n"); tok; tok = strtok(NULL, ", \t\r\n")) {
        if (script_n >= SCRIPT_MAX) {
            wlog("PS2_INPUT: more than %d pending tokens; '%s' and the rest are dropped", SCRIPT_MAX, tok);
            break;
        }
        for (char* c = tok; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';   /* "lxFF" = "lxff" */
        char name[32] = { 0 };
        unsigned frame = 0, hold = 6, addr = 0, ra = 0;
        int kind = SCRIPT_ABS;
        double at = 0;
        if (tok[0] == '+') {
            kind = SCRIPT_REL;
            if (sscanf(tok + 1, "%u:%31[a-z0-9]:%u", &frame, name, &hold) < 2) continue;
        } else if (tok[0] == 'r' && (isdigit((unsigned char)tok[1]) || tok[1] == '.')) {   /* (pc-fps) r<seconds>: relative time */
            kind = SCRIPT_RTIME;
            if (sscanf(tok + 1, "%lf:%31[a-z0-9]:%u", &at, name, &hold) < 2) continue;
        } else if (tok[0] == 't') {
            kind = SCRIPT_TIME;
            if (sscanf(tok + 1, "%lf:%31[a-z0-9]:%u", &at, name, &hold) < 2) continue;
        } else if (tok[0] == '@') {
            kind = tok[1] == '!' ? SCRIPT_QUIET : SCRIPT_ANCHOR;
            char* p = tok + (kind == SCRIPT_QUIET ? 2 : 1);
            addr = (unsigned)strtoul(p, &p, 16);
            if (*p == '/') ra = (unsigned)strtoul(p + 1, &p, 16);
            if (kind == SCRIPT_QUIET) frame = 30;
            if (*p == '+') frame = (unsigned)strtoul(p + 1, &p, 10);
            if (*p != ':' || sscanf(p + 1, "%31[a-z0-9]:%u", name, &hold) < 1) continue;
        } else if (sscanf(tok, "%u:%31[a-z0-9]:%u", &frame, name, &hold) < 2) continue;
        int found = !strcmp(name, "none") ? SCRIPT_NONE : !strcmp(name, "ram") ? SCRIPT_RAM :
                    !strcmp(name, "release") ? SCRIPT_RELEASE : !strcmp(name, "pause") ? SCRIPT_PAUSE :
                    !strcmp(name, "calls") ? SCRIPT_CALLS : !strcmp(name, "watch") ? SCRIPT_WATCH :
                    !strcmp(name, "aspect") ? SCRIPT_ASPECT : !strcmp(name, "hud") ? SCRIPT_HUD :
                    !strcmp(name, "rec") ? SCRIPT_REC : !strcmp(name, "renderer") ? SCRIPT_GPUSWITCH :
                    !strcmp(name, "losedevice") ? SCRIPT_GPULOSE : -1,
            axis = 0, value = 0x80;
        for (int g = 0; g < SCRIPT_NGFX; g++)
            if (!strcmp(name, script_gfx[g].name)) found = SCRIPT_GFX0 - g;
        for (int g = 0; g < SCRIPT_NMENU; g++)
            if (!strcmp(name, script_menu[g])) found = SCRIPT_MENU0 - g;
        for (int b = 0; b < SCRIPT_NBUTTONS; b++)
            if (!strcmp(name, script_buttons[b].name)) found = b;
        for (int b = 0; b < (int)(sizeof script_sticks / sizeof script_sticks[0]); b++)
            if (!strcmp(name, script_sticks[b].name)) found = SCRIPT_NBUTTONS + b;
        static const char* const axes[] = { "lx", "ly", "rx", "ry" };
        for (int a = 0; a < 4; a++)
            if (!strncmp(name, axes[a], 2) && strlen(name) == 4) {
                found = SCRIPT_AXIS; axis = a + 1; value = (int)strtoul(name + 2, NULL, 16);
            }
        if (found == -1) { wlog("PS2_INPUT: unknown button in '%s'", tok); continue; }
        /* (pc-fps) a hold in seconds ("t3:lup:2.5s", "+0:cross:0.2s"): the same real time at any frame rate */
        double hold_s = 0;
        const char* last_colon = strrchr(tok, ':');
        size_t tl = strlen(tok);
        if (last_colon && last_colon != strchr(tok, ':') && tl > 1 && tok[tl - 1] == 's')
            hold_s = atof(last_colon + 1);
        if (hold_s > 0) hold = 0xFFFFFFF;   /* (until its seconds are over: script_apply) */
        script[script_n].hold_s = hold_s; script[script_n].start_s = 0;
        script[script_n].axis = axis; script[script_n].value = value;
        script[script_n].frame = frame; script[script_n].hold = hold; script[script_n].button = found;
        script[script_n].kind = kind; script[script_n].addr = addr; script[script_n].ra = ra;
        script[script_n].armed = script[script_n].seen = 0;
        script_at[script_n] = at;
        script[script_n++].start = kind == SCRIPT_ABS ? frame : SCRIPT_UNSET;
    }
    free(buf);
}

/* Drop the tokens that are over (started, hold elapsed) and that the next token no longer needs
 * (it has its start frame), keeping the order: a live session can then send any number of lines. */
static void script_compact(uint32_t now) {
    int k = 0;
    for (int i = 0; i < script_n; i++) {
        int over = script[i].start != SCRIPT_UNSET && now >= script[i].start + script[i].hold &&
                   (i + 1 == script_n || script[i + 1].start != SCRIPT_UNSET);
        if (over) continue;
        if (k != i) { script[k] = script[i]; script_at[k] = script_at[i]; }
        k++;
    }
    script_n = k;
}

static const char* script_name(int b) {
    if (b <= SCRIPT_GFX0 && b > SCRIPT_GFX0 - SCRIPT_NGFX) return script_gfx[SCRIPT_GFX0 - b].name;
    if (b <= SCRIPT_MENU0 && b > SCRIPT_MENU0 - SCRIPT_NMENU) return script_menu[SCRIPT_MENU0 - b];
    return b == SCRIPT_NONE ? "none" : b == SCRIPT_AXIS ? "axis" : b == SCRIPT_RAM ? "ram" : b == SCRIPT_PAUSE ? "pause" :
           b == SCRIPT_RELEASE ? "release" : b == SCRIPT_CALLS ? "calls" : b == SCRIPT_WATCH ? "watch" :
           b == SCRIPT_ASPECT ? "aspect" : b == SCRIPT_HUD ? "hud" : b == SCRIPT_REC ? "rec" :
           b == SCRIPT_GPUSWITCH ? "renderer" : b == SCRIPT_GPULOSE ? "losedevice" :
           b >= SCRIPT_NBUTTONS ? script_sticks[b - SCRIPT_NBUTTONS].name : script_buttons[b].name;
}

/* PS2_INPUT_FILE: read lines appended since the last look; each line starts at "now". */
static const char* script_path = (const char*)1;              /* (const char*)1: not looked up yet */
static long script_offset;

static void script_poll_file(uint32_t now, int force) {
    if (script_path == (const char*)1) {
        script_path = getenv("PS2_INPUT_FILE");
        if (script_path) {                                    /* ignore what is already there */
            FILE* f = fopen(script_path, "rb");
            if (f) { fseek(f, 0, SEEK_END); script_offset = ftell(f); fclose(f); }
            wlog("live input from %s", script_path);
        }
    }
    if (!script_path || (now % 8 && !force)) return;
    long offset = script_offset;
    FILE* f = fopen(script_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    char* buf = end > offset ? malloc((size_t)(end - offset) + 1) : NULL;
    size_t n = 0;
    if (buf) {
        fseek(f, offset, SEEK_SET);
        n = fread(buf, 1, (size_t)(end - offset), f);
    }
    fclose(f);
    while (n && buf[n - 1] != '\n') n--;                      /* complete lines only */
    if (!n) { free(buf); return; }
    buf[n] = 0;
    script_offset = offset + (long)n;
    script_compact(now);
    for (char *line = buf, *next; *line; line = next) {      /* each line: its own time origin */
        next = strchr(line, '\n');
        *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (script_n < SCRIPT_MAX) {
            memset(&script[script_n], 0, sizeof script[0]);
            script[script_n].button = SCRIPT_NONE;
            script[script_n].frame = script[script_n].start = now;
            script[script_n++].hold = 1;
        }
        int first = script_n;
        script_parse(line);
        wlog("pad frame %u (VBlank %u): %d live tokens: %.300s%s", now, ps2_vblank_count(), script_n - first, line,
             len > 300 ? " ..." : "");
    }
    free(buf);
}

/* "pause": due at a VBlank whose picture is dumped, so latest.png shows where the game stopped. */
static int script_pause_due(void) {
    static int every = -1;
    if (every < 0) {
        const char* e = getenv("PS2_DUMP_EVERY");
        every = !getenv("PS2_DUMP_FRAMES") ? 1 : e && atoi(e) > 0 ? atoi(e) : 60;
    }
    return ps2_vblank_count() % (uint32_t)every == 0;
}

/* The EE waits here, inside the pad read, until a complete line follows what was read. */
static void script_wait_line(uint32_t now) {
    wlog("pad frame %u (VBlank %u): paused until the next line in %s", now, ps2_vblank_count(), script_path);
    ps2_debug_hold(1);                                        /* no PS2 time passes while paused */
    for (;;) {
        FILE* f = fopen(script_path, "rb");
        int line = 0;
        if (f) {
            if (fseek(f, script_offset, SEEK_SET) == 0)
                for (int c; !line && (c = fgetc(f)) != EOF;) line = c == '\n';
            fclose(f);
        }
        if (line) { ps2_debug_hold(0); return; }
        Sleep(20);
    }
}

/* Give relative and anchored tokens their start frame once the token before them has started. */
static void script_resolve(uint32_t now) {
    double now_s = script_seconds() - script_t0;               /* (pc-fps) when each token started, in seconds */
    if (now_s <= 0) now_s = 1e-9;
    for (int i = 0; i < script_n; i++)
        if (script[i].start != SCRIPT_UNSET && now >= script[i].start && script[i].start_s <= 0) script[i].start_s = now_s;
    for (int i = 0; i < script_n; i++) {
        if (script[i].start != SCRIPT_UNSET) continue;
        uint32_t prev = i ? script[i - 1].start : 0;
        if (prev == SCRIPT_UNSET || now < prev) continue;        /* previous one hasn't started */
        if (script[i].kind == SCRIPT_REL) { script[i].start = prev + script[i].frame; continue; }
        if (script[i].kind == SCRIPT_RTIME) {                    /* (pc-fps) seconds after the previous token started */
            double ps = i ? script[i - 1].start_s : 1e-9;
            if (ps <= 0 || now_s < ps + script_at[i]) continue;
            script[i].start = now;
            script[i].start_s = now_s;
            continue;
        }
        if (script[i].kind == SCRIPT_TIME) {
            double secs = script_seconds() - script_t0;
            if (secs < script_at[i]) continue;
            script[i].start = now;
            wlog("scripted press at %.1f s: pad frame %u, '%s'", secs, now, script_name(script[i].button));
            continue;
        }
        uint32_t calls = ps2_func_calls(script[i].addr, script[i].ra);
        if (!script[i].armed) { script[i].armed = 1; script[i].base = calls; script[i].last = now; }
        if (script[i].kind == SCRIPT_QUIET) {                    /* called, then silent for n frames */
            if (calls != script[i].base) { script[i].base = calls; script[i].last = now; script[i].seen = 1; }
            if (!script[i].seen || now - script[i].last < script[i].frame) continue;
            script[i].start = now;
            wlog("pad frame %u: function %08X silent for %u frames, '%s'", now, script[i].addr,
                 script[i].frame, script_name(script[i].button));
            continue;
        }
        if (calls == script[i].base) continue;                   /* still waiting for the call */
        script[i].start = now + script[i].frame;
        wlog("pad frame %u: function %08X called, '%s' at %u", now, script[i].addr,
             script_name(script[i].button), script[i].start);
    }
}

static void script_apply(int port, pad_input* in) {
    if (script_n < 0) {
        script_n = 0;
        script_t0 = script_seconds();
        const char* s = getenv("PS2_INPUT");
        script_parse(s);
        if (s) wlog("%d scripted presses", script_n);
    }
    if (port != 0) return;
    uint32_t now = script_frame++;
    int resumed = 0;
    if (script_pause_req && script_path && script_path != (const char*)1 && script_pause_due()) {
        script_pause_req = 0;
        script_wait_line(now);
        resumed = 1;                                          /* take the new line at once */
    }
    script_poll_file(now, resumed);
    if (!script_n) return;
    script_resolve(now);
    for (int i = 0; i < script_n; i++)                       /* "release": cut the holds before it */
        if (script[i].button == SCRIPT_RELEASE && script[i].start == now)
            for (int j = 0; j < i; j++)
                if (script[j].start != SCRIPT_UNSET && script[j].start < now && now < script[j].start + script[j].hold)
                    script[j].hold = now - script[j].start;
    for (int i = 0; i < script_n; i++) {
        if (script[i].start == SCRIPT_UNSET) continue;
        if (script[i].hold_s > 0 && now >= script[i].start && now < script[i].start + script[i].hold) {   /* (pc-fps) */
            double secs = script_seconds() - script_t0;
            if (script[i].start_s <= 0) script[i].start_s = secs > 0 ? secs : 1e-9;
            if (secs - script[i].start_s >= script[i].hold_s) script[i].hold = now > script[i].start ? now - script[i].start : 1;
        }
        if (now < script[i].start || now >= script[i].start + script[i].hold) continue;
        int b = script[i].button;
        if (now == script[i].start && b != SCRIPT_NONE)       /* calibrate pad frames vs VBlanks */
            wlog("pad frame %u (VBlank %u): %s (%u frames)", now, ps2_vblank_count(), script_name(b), script[i].hold);
        if (b == SCRIPT_NONE || b == SCRIPT_RELEASE) continue;
        if (b == SCRIPT_RAM) { if (now == script[i].start) ps2_request_ram_dump(); continue; }
        if (b == SCRIPT_PAUSE) { if (now == script[i].start) script_pause_req = 1; continue; }
        if (b == SCRIPT_CALLS) { if (now == script[i].start) ps2_request_calls_window(script[i].hold); continue; }
        if (b == SCRIPT_WATCH) { if (now == script[i].start) ps2_request_watch_window(script[i].hold); continue; }
        if (b == SCRIPT_ASPECT) { if (now == script[i].start) switch_aspect(); continue; }
        if (b <= SCRIPT_GFX0 && b > SCRIPT_GFX0 - SCRIPT_NGFX) {
            if (now == script[i].start) gfx_key(script_gfx[SCRIPT_GFX0 - b].vk, script_gfx[SCRIPT_GFX0 - b].shift, 0);
            continue;
        }
        if (b <= SCRIPT_MENU0 && b > SCRIPT_MENU0 - SCRIPT_NMENU) {
            if (now == script[i].start && menu_script) menu_script(SCRIPT_MENU0 - b);
            continue;
        }
        if (b == SCRIPT_HUD) { if (now == script[i].start) switch_hud(); continue; }
        if (b == SCRIPT_REC) { if (now == script[i].start) gs_record_start_now(); continue; }
        if (b == SCRIPT_GPUSWITCH || b == SCRIPT_GPULOSE) {
            if (now == script[i].start) script_debug_key(b == SCRIPT_GPUSWITCH ? WINDOW_KEY_SWITCH : WINDOW_KEY_LOSE_DEVICE);
            continue;
        }
        if (b == SCRIPT_AXIS) {
            uint8_t v = (uint8_t)script[i].value;
            switch (script[i].axis) {
            case 1: in->lx = v; break;
            case 2: in->ly = v; break;
            case 3: in->rx = v; break;
            default: in->ry = v; break;
            }
            continue;
        }
        if (b >= SCRIPT_NBUTTONS) {
            uint8_t v = (uint8_t)script_sticks[b - SCRIPT_NBUTTONS].value;
            switch (script_sticks[b - SCRIPT_NBUTTONS].axis) {
            case 1: in->lx = v; break;
            case 2: in->ly = v; break;
            case 3: in->rx = v; break;
            default: in->ry = v; break;
            }
            continue;
        }
        in->buttons |= script_buttons[b].bit;
        if (script_buttons[b].pressure >= 0) in->pressure[script_buttons[b].pressure] = 0xFF;
    }
}

/* ==== pure helpers =============================================================================== */

typedef struct { int x, y, w, h; } wrect;

/* The largest aw:ah rectangle centred in a cw x ch client area (widths and heights rounded to the
 * nearest pixel). */
static wrect fit_aspect(int cw, int ch, int aw, int ah) {
    wrect r = { 0, 0, cw, ch };
    if (cw <= 0 || ch <= 0 || aw <= 0 || ah <= 0) { r.w = r.h = 0; return r; }
    if ((int64_t)cw * ah > (int64_t)ch * aw) {       /* wider than aw:ah: bars left and right */
        r.w = (int)(((int64_t)ch * aw * 2 + ah) / (2 * (int64_t)ah));
        r.x = (cw - r.w) / 2;
    } else {                                         /* taller: bars top and bottom */
        r.h = (int)(((int64_t)cw * ah * 2 + aw) / (2 * (int64_t)aw));
        r.y = (ch - r.h) / 2;
    }
    return r;
}

/* The largest 4:3 rectangle centred in a cw x ch client area. */
static wrect letterbox(int cw, int ch) { return fit_aspect(cw, ch, 4, 3); }

static void pad_clear(pad_input* p) { input_pad_clear(p); }   /* (scripts, tests) */

/* ==== frame hand-off: triple buffer ============================================================== */

/* fb_write belongs to the producer (window_present), fb_read to the window thread; fb_ready and
 * fb_fresh are shared and only touched under fb_lock. Pixels are copied outside the lock (the
 * producer owns its buffer), so the lock is held for an index swap only. */
static uint32_t fb_pix[3][FRAME_MAX * FRAME_MAX];
static int fb_w[3], fb_h[3];
static int fb_aw[3] = { 4, 4, 4 }, fb_ah[3] = { 3, 3, 3 };   /* display aspect of each frame */
static int fb_write = 0, fb_ready = 1, fb_read = 2, fb_fresh;
static SRWLOCK fb_lock = SRWLOCK_INIT;
static HANDLE fb_event;                  /* auto-reset: a fresh frame is waiting */
static volatile LONG fb_presented;       /* window_present calls since the last FPS update */
static volatile LONG win_accepting;      /* window_init ran: window_present stores frames */

static int fb_setup(void) {
    if (!fb_event) fb_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    return fb_event != NULL;
}

/* Window thread: make the newest published frame the read buffer. Returns 1 if there was one. */
static int fb_take(void) {
    AcquireSRWLockExclusive(&fb_lock);
    int got = fb_fresh;
    if (got) { int t = fb_read; fb_read = fb_ready; fb_ready = t; fb_fresh = 0; }
    ReleaseSRWLockExclusive(&fb_lock);
    return got;
}

void window_present_aspect(const uint32_t* bgrx, int w, int h, int stride, int aspect_w, int aspect_h);

void window_present(const uint32_t* bgrx, int w, int h, int stride) { window_present_aspect(bgrx, w, h, stride, 4, 3); }

/* ==== external presenter (window_gpu.h) =========================================================== */
static volatile LONG ext_on;             /* a renderer of its own presents on the window (no device here) */
static window_present_fn volatile ext_fn;
static volatile LONG ext_changed;        /* window thread: (re)create or drop the own presenter */
static volatile LONG client_w, client_h; /* as of the last WM_SIZE */
static volatile LONG client_known;
static volatile LONG resize_serial;
static volatile LONG debug_keys;         /* WINDOW_KEY_* pressed since the last window_take_debug_keys */
static int debug_keys_on = -1;           /* PS2_GS_DEBUG_KEYS */

void window_set_external_presenter(int on, window_present_fn fn) {
    ext_fn = fn;
    if ((LONG)(on != 0) != ext_on) {
        InterlockedExchange(&ext_on, on != 0);
        InterlockedExchange(&ext_changed, 1);
        if (fb_event) SetEvent(fb_event);       /* (wake the window thread) */
    }
}
int window_external_presenter(void) { return ext_on != 0; }

void window_note_presented(void) { InterlockedIncrement(&fb_presented); }
uint32_t window_resize_serial(void) { return (uint32_t)resize_serial; }
int window_take_debug_keys(void) { return (int)InterlockedExchange(&debug_keys, 0); }
static void script_debug_key(int key) { InterlockedOr(&debug_keys, key); }   /* (scripts: no PS2_GS_DEBUG_KEYS needed) */

/* window_present for a picture shown at aspect_w:aspect_h (widescreen: the display aspect, or 4:3 for
 * pictures of 4:3 content, which are then pillarboxed; ps2_runtime.c's present_frame decides). */
void window_present_aspect(const uint32_t* bgrx, int w, int h, int stride, int aspect_w, int aspect_h) {
    if (ext_on) {                        /* the external renderer shows it */
        window_present_fn fn = ext_fn;
        /* (the external renderer counts what it shows: window_note_presented) */
        if (fn && bgrx && w > 0 && h > 0 && stride >= w) fn(bgrx, w, h, stride, aspect_w, aspect_h);
        return;
    }
    if (!win_accepting) return;
    if (aspect_w <= 0 || aspect_h <= 0) { aspect_w = 4; aspect_h = 3; }
    if (!bgrx || w <= 0 || h <= 0 || stride < w) {
        static int logged;
        if (!logged) { logged = 1; wlog("window_present: bad frame %dx%d stride %d ignored", w, h, stride); }
        return;
    }
    if (w > FRAME_MAX || h > FRAME_MAX) {
        static int logged;
        if (!logged) { logged = 1; wlog("window_present: %dx%d frame cropped to %d", w, h, FRAME_MAX); }
        if (w > FRAME_MAX) w = FRAME_MAX;
        if (h > FRAME_MAX) h = FRAME_MAX;
    }
    int b = fb_write;                    /* only this thread changes fb_write */
    uint32_t* dst = fb_pix[b];
    if (stride == w) memcpy(dst, bgrx, (size_t)w * (size_t)h * 4);
    else for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, bgrx + (size_t)y * stride, (size_t)w * 4);
    fb_w[b] = w;
    fb_h[b] = h;
    fb_aw[b] = aspect_w;
    fb_ah[b] = aspect_h;

    AcquireSRWLockExclusive(&fb_lock);   /* publish: the written buffer becomes "ready" */
    fb_write = fb_ready;
    fb_ready = b;
    fb_fresh = 1;
    ReleaseSRWLockExclusive(&fb_lock);
    InterlockedIncrement(&fb_presented);
    SetEvent(fb_event);
}

/* ==== input (runtime/input.c: controllers, keyboard, the menu's input) ============================= */

static HWND volatile g_hwnd;             /* published once the window and renderer are ready */

extern void pcmenu_pad(int port, void* pad) __attribute__((weak));   /* (pc-menu2) menu_game.c: Esc / PS on the game's menus */

void window_get_input(int port, pad_input* out) {
    input_get_pad(port, out);
    script_apply(port, out);                                  /* PS2_INPUT test scripts */
    if (pcmenu_pad) pcmenu_pad(port, out);
    if (input_menu_active()) input_pad_clear(out);            /* (scripted presses too: the Options menu is open) */
}

/* ==== Direct3D 11 presenter ====================================================================== */

/* Interface IDs (defined locally so no dxguid/uuid import library is needed). */
static const IID IID_IDXGIDevice_   = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const IID IID_IDXGIDevice1_  = { 0x77db970f, 0x6276, 0x48ba, { 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c } };
static const IID IID_IDXGIFactory_  = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const IID IID_IDXGIFactory2_ = { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
static const IID IID_ID3D11Texture2D_ = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

/* A triangle covering the viewport (vertex positions from SV_VertexID, no vertex buffer). The
 * texture holds the BGRX bytes as R8G8B8A8, so .bgr is the colour; alpha is ignored. */
static const char d3d_shader_src[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs_main(uint id : SV_VertexID) {\n"
    "    V o;\n"
    "    float2 t = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);\n"
    "    o.uv = t;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_main(V i) : SV_Target {\n"
    "    float4 c = tex.Sample(smp, i.uv);\n"
    "    return float4(c.b, c.g, c.r, 1.0);\n"
    "}\n";

static struct {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    IDXGISwapChain* sc;
    ID3D11RenderTargetView* rtv;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11SamplerState* smp;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    int tex_w, tex_h;
    int bb_w, bb_h;                      /* back buffer size (-1: resize before the next draw) */
    unsigned tex_seq;                    /* frame sequence number held by tex (0: none) */
    D3D_FEATURE_LEVEL level;
    const char* model;                   /* swap model, for the log */
} d3d;

/* (pc-menu) the Options menu / frame rate counter over the picture (menu_overlay.c; weak: not in the window test) */
extern void overlay_d3d11_draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h)
    __attribute__((weak));
extern void overlay_d3d11_release(ID3D11Device* dev) __attribute__((weak));
extern void overlay_gdi_draw(void* hdc, int w, int h) __attribute__((weak));
extern void overlay_set_fps(double fps) __attribute__((weak));
extern void menu_tick(void) __attribute__((weak));
extern int menu_is_open(void) __attribute__((weak));
extern void menu_focus(int active) __attribute__((weak));

static void d3d_shutdown(void) {
    if (d3d.dev && overlay_d3d11_release) overlay_d3d11_release(d3d.dev);
    if (d3d.ctx) { ID3D11DeviceContext_ClearState(d3d.ctx); ID3D11DeviceContext_Flush(d3d.ctx); }
    RELEASE(d3d.srv); RELEASE(d3d.tex); RELEASE(d3d.rtv); RELEASE(d3d.smp); RELEASE(d3d.ps);
    RELEASE(d3d.vs); RELEASE(d3d.sc); RELEASE(d3d.ctx); RELEASE(d3d.dev);
    memset(&d3d, 0, sizeof d3d);
}

static int d3d_make_rtv(void) {
    ID3D11Texture2D* bb = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(d3d.sc, 0, &IID_ID3D11Texture2D_, (void**)&bb);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRenderTargetView(d3d.dev, (ID3D11Resource*)bb, NULL, &d3d.rtv);
        ID3D11Texture2D_Release(bb);
    }
    return SUCCEEDED(hr);
}

static int d3d_create_swapchain(HWND hwnd) {
    IDXGIDevice* dxdev = NULL;
    IDXGIDevice1* dxdev1 = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIFactory* factory = NULL;
    IDXGIFactory2* factory2 = NULL;
    HRESULT hr = ID3D11Device_QueryInterface(d3d.dev, &IID_IDXGIDevice_, (void**)&dxdev);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxdev, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory_, (void**)&factory);
    if (SUCCEEDED(hr) && SUCCEEDED(IDXGIFactory_QueryInterface(factory, &IID_IDXGIFactory2_, (void**)&factory2))) {
        /* Flip model (Windows 8+; FLIP_DISCARD needs Windows 10). Size 0 = the client area. */
        static const struct { DXGI_SWAP_EFFECT effect; const char* name; } effects[] = {
            { DXGI_SWAP_EFFECT_FLIP_DISCARD, "flip-discard" },
            { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "flip-sequential" },
        };
        DXGI_SWAP_CHAIN_DESC1 desc;
        memset(&desc, 0, sizeof desc);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        for (size_t i = 0; i < sizeof effects / sizeof effects[0] && !d3d.sc; i++) {
            IDXGISwapChain1* sc1 = NULL;
            desc.SwapEffect = effects[i].effect;
            if (SUCCEEDED(IDXGIFactory2_CreateSwapChainForHwnd(factory2, (IUnknown*)d3d.dev, hwnd, &desc,
                                                               NULL, NULL, &sc1))) {
                d3d.sc = (IDXGISwapChain*)sc1;   /* IDXGISwapChain1 extends IDXGISwapChain */
                d3d.model = effects[i].name;
            }
        }
    }
    if (SUCCEEDED(hr) && !d3d.sc) {      /* Windows 7: blit model */
        DXGI_SWAP_CHAIN_DESC desc;
        memset(&desc, 0, sizeof desc);
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = hwnd;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (SUCCEEDED(IDXGIFactory_CreateSwapChain(factory, (IUnknown*)d3d.dev, &desc, &d3d.sc))) d3d.model = "blit";
    }
    if (d3d.sc) {
        /* We do our own (borderless) fullscreen; keep DXGI from reacting to Alt+Enter. */
        IDXGIFactory_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        /* One queued frame: Present waits for the previous frame's vsync instead of running ahead. */
        if (dxdev && SUCCEEDED(IDXGIDevice_QueryInterface(dxdev, &IID_IDXGIDevice1_, (void**)&dxdev1)))
            IDXGIDevice1_SetMaximumFrameLatency(dxdev1, 1);
    }
    RELEASE(dxdev1); RELEASE(factory2); RELEASE(factory); RELEASE(adapter); RELEASE(dxdev);
    return d3d.sc != NULL;
}

static const char* level_name(D3D_FEATURE_LEVEL l) {
    switch (l) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default: return "?";
    }
}

/* Set up device, shaders, sampler and swap chain. On failure everything is released and the
 * reason logged. */
static int d3d_init(HWND hwnd) {
    static HMODULE m_d3d, m_comp;
    const char* why = NULL;
    if (!m_d3d) m_d3d = LoadLibraryW(L"d3d11.dll");
    if (!m_comp) m_comp = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3D11_CREATE_DEVICE create = (PFN_D3D11_CREATE_DEVICE)dll_proc(m_d3d, "D3D11CreateDevice");
    pD3DCompile compile = (pD3DCompile)dll_proc(m_comp, "D3DCompile");
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr = E_FAIL;
    memset(&d3d, 0, sizeof d3d);
    if (!create) { why = "d3d11.dll not found"; goto fail; }
    if (!compile) { why = "d3dcompiler_47.dll not found"; goto fail; }

    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    static const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    for (size_t i = 0; i < 2 && !d3d.dev; i++) {
        hr = create(NULL, drivers[i], NULL, 0, levels, 4, D3D11_SDK_VERSION, &d3d.dev, &d3d.level, &d3d.ctx);
        if (hr == E_INVALIDARG)          /* runtimes without 11_1 reject the whole list */
            hr = create(NULL, drivers[i], NULL, 0, levels + 1, 3, D3D11_SDK_VERSION, &d3d.dev, &d3d.level, &d3d.ctx);
    }
    if (!d3d.dev) { why = "no Direct3D 10/11 capable device"; goto fail; }

    hr = compile(d3d_shader_src, sizeof d3d_shader_src - 1, "window", NULL, NULL, "vs_main", "vs_4_0",
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = compile(d3d_shader_src, sizeof d3d_shader_src - 1, "window", NULL, NULL, "ps_main", "ps_4_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (FAILED(hr)) {
        if (err) wlog("shader: %s", (const char*)ID3D10Blob_GetBufferPointer(err));
        why = "shader compilation failed";
        goto fail;
    }
    hr = ID3D11Device_CreateVertexShader(d3d.dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
                                         NULL, &d3d.vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(d3d.dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
                                            NULL, &d3d.ps);
    if (FAILED(hr)) { why = "cannot create shaders"; goto fail; }

    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(d3d.dev, &sd, &d3d.smp))) { why = "cannot create sampler"; goto fail; }

    if (!d3d_create_swapchain(hwnd)) { why = "cannot create swap chain"; goto fail; }
    d3d.bb_w = d3d.bb_h = -1;
    RELEASE(vsb); RELEASE(psb); RELEASE(err);
    wlog("Direct3D 11 presenter (feature level %s, %s swap chain)", level_name(d3d.level), d3d.model);
    return 1;

fail:
    RELEASE(vsb); RELEASE(psb); RELEASE(err);
    d3d_shutdown();
    wlog("Direct3D 11 unavailable (%s, hr=0x%08lX): using GDI", why, (unsigned long)hr);
    return 0;
}

static HRESULT d3d_resize(int w, int h) {
    ID3D11DeviceContext_ClearState(d3d.ctx);         /* drop every reference to the old buffers */
    RELEASE(d3d.rtv);
    ID3D11DeviceContext_Flush(d3d.ctx);
    HRESULT hr = IDXGISwapChain_ResizeBuffers(d3d.sc, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr) && !d3d_make_rtv()) hr = E_FAIL;
    /* On failure there is no render target any more: force another resize before the next draw,
     * even if the client area goes back to the old back-buffer size. */
    d3d.bb_w = SUCCEEDED(hr) ? w : -1;
    d3d.bb_h = SUCCEEDED(hr) ? h : -1;
    return hr;
}

static int d3d_upload(const uint32_t* px, int w, int h) {
    if (!d3d.tex || d3d.tex_w != w || d3d.tex_h != h) {      /* frame size changed: new texture */
        RELEASE(d3d.srv);
        RELEASE(d3d.tex);
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(d3d.dev, &td, NULL, &d3d.tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(d3d.dev, (ID3D11Resource*)d3d.tex, NULL, &d3d.srv))) {
            RELEASE(d3d.srv);
            RELEASE(d3d.tex);
            return 0;
        }
        d3d.tex_w = w;
        d3d.tex_h = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(d3d.ctx, (ID3D11Resource*)d3d.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    if (m.RowPitch == (UINT)w * 4) memcpy(m.pData, px, (size_t)w * (size_t)h * 4);
    else for (int y = 0; y < h; y++) memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, px + (size_t)y * w, (size_t)w * 4);
    ID3D11DeviceContext_Unmap(d3d.ctx, (ID3D11Resource*)d3d.tex, 0);
    return 1;
}

/* Draw the current frame (px may be NULL: black) and present with vsync. */
static HRESULT d3d_render(HWND hwnd, const uint32_t* px, int fw, int fh, unsigned seq, int aw, int ah) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return S_OK;        /* minimised */
    HRESULT hr;
    if (rc.right != d3d.bb_w || rc.bottom != d3d.bb_h) {
        hr = d3d_resize(rc.right, rc.bottom);
        if (FAILED(hr)) return hr;
    }
    if (px && d3d.tex_seq != seq && d3d_upload(px, fw, fh)) d3d.tex_seq = seq;

    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(d3d.ctx, 1, &d3d.rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(d3d.ctx, d3d.rtv, black);
    if (px && d3d.srv) {
        wrect r = fit_aspect(rc.right, rc.bottom, aw, ah);
        D3D11_VIEWPORT vp = { (FLOAT)r.x, (FLOAT)r.y, (FLOAT)r.w, (FLOAT)r.h, 0.0f, 1.0f };
        ID3D11DeviceContext_RSSetViewports(d3d.ctx, 1, &vp);
        ID3D11DeviceContext_IASetInputLayout(d3d.ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(d3d.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(d3d.ctx, d3d.vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(d3d.ctx, d3d.ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(d3d.ctx, 0, 1, &d3d.srv);
        ID3D11DeviceContext_PSSetSamplers(d3d.ctx, 0, 1, &d3d.smp);
        ID3D11DeviceContext_Draw(d3d.ctx, 3, 0);
    }
    if (overlay_d3d11_draw) overlay_d3d11_draw(d3d.dev, d3d.ctx, d3d.rtv, rc.right, rc.bottom);
    return IDXGISwapChain_Present(d3d.sc, 1, 0);
}

/* ==== GDI presenter (fallback) =================================================================== */

typedef HRESULT (WINAPI* dwm_flush_fn)(void);
static dwm_flush_fn dwm_flush;

static void gdi_render(HWND hwnd, const uint32_t* px, int fw, int fh, int aw, int ah) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (px) {
        wrect r = fit_aspect(rc.right, rc.bottom, aw, ah);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = fw;
        bi.bmiHeader.biHeight = -fh;     /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, r.x, r.y, r.w, r.h, 0, 0, fw, fh, px, &bi, DIB_RGB_COLORS, SRCCOPY);
        /* Everything around the picture. When the spare width or height is odd, the right/bottom
         * bar is one pixel wider than the left/top one, and may be the only bar (1 pixel). */
        const RECT bars[4] = { { 0, 0, r.x, rc.bottom }, { r.x + r.w, 0, rc.right, rc.bottom },
                               { r.x, 0, r.x + r.w, r.y }, { r.x, r.y + r.h, r.x + r.w, rc.bottom } };
        for (int i = 0; i < 4; i++) if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top) FillRect(dc, &bars[i], black);
    } else {
        FillRect(dc, &rc, black);
    }
    if (overlay_gdi_draw) overlay_gdi_draw(dc, rc.right, rc.bottom);   /* (pc-menu) the Options menu */
    ReleaseDC(hwnd, dc);
    if (dwm_flush) dwm_flush();          /* pace to the compositor, like a vsync'd present */
}

/* ==== window thread ============================================================================== */

static int use_d3d;                      /* window thread: 1 = Direct3D 11, 0 = GDI */
static int win_ready;                    /* window thread: renderer set up */
static int win_fullscreen;
static WINDOWPLACEMENT win_placement;    /* windowed placement saved while fullscreen */
static unsigned cur_seq;                 /* sequence number of the frame in fb_pix[fb_read] (0: none yet) */
static volatile LONG frames_shown;       /* new frames presented by the window thread (statistics) */

static void render(HWND hwnd) {
    if (!win_ready || ext_on) return;    /* (an external renderer's swap chain: nothing drawn here) */
    const uint32_t* px = cur_seq ? fb_pix[fb_read] : NULL;
    int fw = fb_w[fb_read], fh = fb_h[fb_read], aw = fb_aw[fb_read], ah = fb_ah[fb_read];
    if (!use_d3d) { gdi_render(hwnd, px, fw, fh, aw, ah); return; }

    HRESULT hr = d3d_render(hwnd, px, fw, fh, cur_seq, aw, ah);
    if (SUCCEEDED(hr)) return;
    static int logged, losses;
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
        /* e.g. a failed resize: retried with the next frame */
        if (!logged) { logged = 1; wlog("Direct3D present failed (hr=0x%08lX)", (unsigned long)hr); }
        return;
    }
    wlog("Direct3D device lost (hr=0x%08lX): recreating it", (unsigned long)hr);
    d3d_shutdown();
    use_d3d = ++losses <= 3 && d3d_init(hwnd);   /* a device that keeps dying: settle for GDI */
    if (!use_d3d) {
        dwm_flush = (dwm_flush_fn)dll_proc(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
        gdi_render(hwnd, px, fw, fh, aw, ah);
    } else {
        d3d_render(hwnd, px, fw, fh, cur_seq, aw, ah);
    }
}

/* Show the newest frame if one arrived. */
static void pump_frame(HWND hwnd) {
    if (!fb_take()) return;
    if (++cur_seq == 0) cur_seq = 1;
    render(hwnd);
    InterlockedIncrement(&frames_shown);
}

/* ---- monitors (the Options menu: Display > Monitor) ---------------------------------------------- */
#define WIN_MAX_MONITORS 16
static struct { HMONITOR h; RECT r; int hz, primary; } mons[WIN_MAX_MONITORS];
static int nmons;

static BOOL CALLBACK mon_enum(HMONITOR h, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)r; (void)lp;
    if (nmons >= WIN_MAX_MONITORS) return FALSE;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(h, (MONITORINFO*)&mi)) return TRUE;
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    mons[nmons].h = h;
    mons[nmons].r = mi.rcMonitor;
    mons[nmons].hz = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
    mons[nmons].primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    nmons++;
    return TRUE;
}

static void monitors_scan(void) {
    nmons = 0;
    EnumDisplayMonitors(NULL, NULL, mon_enum, 0);
}

int window_monitor_count(void) { monitors_scan(); return nmons; }

void window_monitor_info(int k, int* w, int* h, int* hz, int* primary) {
    if (k < 0 || k >= nmons) { *w = *h = *hz = *primary = 0; return; }
    *w = mons[k].r.right - mons[k].r.left;
    *h = mons[k].r.bottom - mons[k].r.top;
    *hz = mons[k].hz;
    *primary = mons[k].primary;
}

/* the monitor the game uses: [display] monitor (1..n) if it exists, else the window's */
static HMONITOR game_monitor(HWND hwnd) {
    int k = ps2_settings.monitor;
    if (k > 0) {
        monitors_scan();
        if (k <= nmons) return mons[k - 1].h;
    }
    return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
}

void window_display_size(int* w, int* h, int* hz) {
    *w = *h = *hz = 0;
    HWND hwnd = g_hwnd;
    HMONITOR m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(m, (MONITORINFO*)&mi)) return;
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) *hz = (int)dm.dmDisplayFrequency;
}

/* Borderless fullscreen on the window's monitor (or [display] monitor), and back to the saved placement. */
static void toggle_fullscreen(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    settings_touch();                    /* (the Options menu shows the display mode) */
    if (!win_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        win_placement.length = sizeof win_placement;
        if (!GetWindowPlacement(hwnd, &win_placement) ||
            !GetMonitorInfoW(game_monitor(hwnd), &mi)) return;
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        win_fullscreen = 1;
        display_mode_fullscreen(hwnd, 1);   /* (pc-pacing) [display] fullscreen_refresh */
    } else {
        win_fullscreen = 0;
        display_mode_fullscreen(hwnd, 0);
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &win_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }                                    /* the resulting WM_SIZE redraws */
}

/* [window] in sly2.ini: the windowed placement (the one saved before fullscreen, if fullscreen),
 * maximized, fullscreen. */
static void save_window_state(HWND hwnd) {
    WINDOWPLACEMENT wp;
    wp.length = sizeof wp;
    if (win_fullscreen) wp = win_placement;
    else if (!GetWindowPlacement(hwnd, &wp)) return;
    const RECT* r = &wp.rcNormalPosition;
    if (r->right - r->left < 100 || r->bottom - r->top < 100) return;
    int maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    char x[16], y[16], w[16], h[16];
    snprintf(x, sizeof x, "%ld", r->left);
    snprintf(y, sizeof y, "%ld", r->top);
    snprintf(w, sizeof w, "%ld", r->right - r->left);
    snprintf(h, sizeof h, "%ld", r->bottom - r->top);
    const char* fs = !win_fullscreen ? "windowed" : ps2_settings.win_fullscreen == SETTINGS_FULLSCREEN_EXCLUSIVE ? "fullscreen" : "borderless";
    const char* const kv[][3] = { { "display", "mode", fs }, { "window", "x", x },   /* (pc-polish: was [window] fullscreen) */
                                  { "window", "y", y }, { "window", "width", w }, { "window", "height", h },
                                  { "window", "maximized", maximized ? "on" : "off" } };
    if (!settings_set(kv, 6)) wlog("cannot write the window state to sly2.ini");
}

/* ---- run-time display switches (aspect.c applies them at the game's next frame) -------------------
 * F7: wide <-> 4:3 (back to the wide aspect the game had last, 16:9 if it started at 4:3); F8: HUD at
 * the edges <-> centred. Remembered in sly2.ini ([display] aspect / hud) unless the key comes from an
 * environment override or there is no window (headless runs, PS2_INPUT "aspect"/"hud" tokens). An
 * edit of sly2.ini's [display] while the game runs applies when its window is activated again. */
static int wide_w = 16, wide_h = 9;

/* (pc-menu) [display] aspect / hud changed through the settings table (the Options menu, F7, F8): the game takes
 * it at its next frame (aspect.c) */
static void display_applier(int option) {
    (void)option;
    if (ps2_settings.aspect_w * 3 != ps2_settings.aspect_h * 4) { wide_w = ps2_settings.aspect_w; wide_h = ps2_settings.aspect_h; }
    aspect_request(ps2_settings.aspect_w, ps2_settings.aspect_h, ps2_settings.hud_centered);
}

static void switch_aspect(void) {
    int w, h, hud;
    settings_on_apply(SET_APPLY_DISPLAY, display_applier);
    aspect_target(&w, &h, &hud);
    if (w * 3 != h * 4) { wide_w = w; wide_h = h; w = 4; h = 3; }
    else { w = wide_w; h = wide_h; }
    wlog("aspect %d:%d (F7)", w, h);
    /* (remembered in sly2.ini when there is a window and PS2_ASPECT is not set: settings_change) */
    settings_change_key("display", "aspect", w << 16 | h, g_hwnd != NULL);
}

static void switch_hud(void) {
    int w, h, hud;
    settings_on_apply(SET_APPLY_DISPLAY, display_applier);
    aspect_target(&w, &h, &hud);
    wlog("hud %s (F8)", hud ? "edges" : "centered");
    settings_change_key("display", "hud", !hud, g_hwnd != NULL);
}

/* ---- (pc-menu) the window options of the Options menu: display mode, size, monitor -------------------
 * Any thread asks (settings applier, the menu); the window thread does it (WM_APP_WINDOW). */
#define WM_APP_WINDOW (WM_APP + 0x31)
#define WM_APP_CLIENT_SIZE (WM_APP + 0x32)
extern int pacing_exclusive_fullscreen(int on) __attribute__((weak));
static int win_exclusive;                /* exclusive fullscreen is on (pacing_exclusive_fullscreen) */

int window_mode(void) {
    return !window_fullscreen() ? SETTINGS_FULLSCREEN_OFF : win_exclusive ? SETTINGS_FULLSCREEN_EXCLUSIVE : SETTINGS_FULLSCREEN_BORDERLESS;
}

static void window_applier(int option) {
    (void)option;
    HWND h = g_hwnd;
    if (h) PostMessageW(h, WM_APP_WINDOW, 0, 0);
    settings_touch();
}

void window_set_client_size(int w, int h) {
    HWND hw = g_hwnd;
    if (hw) PostMessageW(hw, WM_APP_CLIENT_SIZE, (WPARAM)w, (LPARAM)h);
}

/* (pc-polish) the monitor rectangle again after a display mode change (Fullscreen's resolution) */
static void fit_fullscreen(HWND hwnd) {
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    RECT wr;
    if (win_fullscreen && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) && GetWindowRect(hwnd, &wr) &&
        !EqualRect(&wr, &mi.rcMonitor))
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

/* window thread: make the window what ps2_settings says (fullscreen mode, monitor, Fullscreen's resolution) */
static void apply_window_settings(HWND hwnd) {
    int want = ps2_settings.win_fullscreen;
    static int res_used = -1;            /* the resolution the display was set to for Fullscreen */
    if (win_exclusive && want != SETTINGS_FULLSCREEN_EXCLUSIVE) {
        if (pacing_exclusive_fullscreen) pacing_exclusive_fullscreen(0);
        win_exclusive = 0;
    }
    if (win_exclusive && res_used >= 0 && res_used != ps2_settings.fullscreen_res) {   /* a new resolution: the mode again */
        display_mode_fullscreen(hwnd, 0);
        display_mode_fullscreen(hwnd, 1);
        fit_fullscreen(hwnd);
    }
    res_used = ps2_settings.fullscreen_res;
    if ((want != SETTINGS_FULLSCREEN_OFF) != (win_fullscreen != 0)) toggle_fullscreen(hwnd);
    else if (win_fullscreen) {           /* fullscreen already: on the chosen monitor */
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        RECT wr;
        if (GetMonitorInfoW(game_monitor(hwnd), &mi) && GetWindowRect(hwnd, &wr) && !EqualRect(&wr, &mi.rcMonitor))
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (ps2_settings.monitor > 0) {   /* windowed: centred on the chosen monitor if it is not on it */
        HMONITOR m = game_monitor(hwnd);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        RECT wr;
        if (m != MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) && GetMonitorInfoW(m, &mi) && GetWindowRect(hwnd, &wr)) {
            int w = wr.right - wr.left, h = wr.bottom - wr.top;
            SetWindowPos(hwnd, NULL, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }
    }
    if (want == SETTINGS_FULLSCREEN_EXCLUSIVE && win_fullscreen && !win_exclusive) {
        win_exclusive = pacing_exclusive_fullscreen && pacing_exclusive_fullscreen(1);
        if (!win_exclusive) wlog("exclusive fullscreen is not available: borderless fullscreen");
        fit_fullscreen(hwnd);
    }
    settings_touch();
}

/* window thread: the client area w x h (windowed; a maximized window is restored first), centred where the window was */
static void apply_client_size(HWND hwnd, int w, int h) {
    if (win_fullscreen || w < 240 || h < 200) return;
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);   /* (pc-polish: Resolution chosen in the menu) */
    RECT wr, cr;
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr)) return;
    int ncw = (wr.right - wr.left) - cr.right, nch = (wr.bottom - wr.top) - cr.bottom;
    int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
    int W = w + ncw, H = h + nch, x = cx - W / 2, y = cy - H / 2;
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {   /* keep it on the work area */
        if (x + W > mi.rcWork.right) x = mi.rcWork.right - W;
        if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    SetWindowPos(hwnd, NULL, x, y, W, H, SWP_NOZORDER);
    gfx_output_resized();
    settings_touch();
}

static void reread_display(void) {
    int w, h, hud, cw, ch, chud;
    if (!settings_read_display(&w, &h, &hud)) return;
    aspect_target(&cw, &ch, &chud);
    if (w * ch == h * cw && hud == chud) return;
    if (w * 3 != h * 4) { wide_w = w; wide_h = h; }
    aspect_request(w, h, hud);
    wlog("sly2.ini changed: aspect %d:%d, hud %s", w, h, hud ? "centered" : "edges");
}

/* The remembered placement, if it is still on a monitor (one may have been unplugged); 1 if used.
 * The window stays hidden; window_thread shows it (maximized if it was). */
static int restore_window_state(HWND hwnd) {
    const ps2_settings_t* s = &ps2_settings;
    if (s->win_w < 100 || s->win_h < 100) return 0;
    RECT r = { s->win_x, s->win_y, s->win_x + s->win_w, s->win_y + s->win_h };
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    RECT vis;
    if (!mon || !GetMonitorInfoW(mon, &mi) || !IntersectRect(&vis, &r, &mi.rcWork) ||
        vis.right - vis.left < 64 || vis.bottom - vis.top < 32) return 0;   /* too little of it visible */
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof wp);
    wp.length = sizeof wp;
    wp.showCmd = SW_HIDE;
    wp.rcNormalPosition = r;
    return SetWindowPlacement(hwnd, &wp) != 0;
}

static void close_window(HWND hwnd) {
    save_window_state(hwnd);
    input_shutdown();                    /* motors off, light bar dark */
    if (win_fullscreen) display_mode_fullscreen(hwnd, 0);   /* (the desktop's display mode back at once) */
    if (window_exit_hook) window_exit_hook(0);
    ExitProcess(0);
}

/* ---- (pc-polish) Quit to desktop ---------------------------------------------------------------------------------
 * The close button / Alt+F4 ask first (menu.c's quit question, over the paused game); a second close while it shows quits
 * at once (a game that no longer answers can always be closed). menu_game.c's quit thread asks the window to close. */
#define WM_APP_QUIT (WM_APP + 0x33)
extern void pcmenu_quit_request(void) __attribute__((weak));
extern int menu_quit_showing(void) __attribute__((weak));

int window_request_quit(void) {
    HWND h = g_hwnd;
    return h && PostMessageW(h, WM_APP_QUIT, 0, 0);
}

/* the game monitor's resolutions, largest first (w << 16 | h, unique, at least 800 x 600, the desktop's bit depth) */
int window_display_modes(int* wh, int max) {
    HWND hwnd = g_hwnd;
    HMONITOR m = hwnd ? game_monitor(hwnd) : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(m, (MONITORINFO*)&mi)) return 0;
    DEVMODEW cur, dm;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur)) return 0;
    int n = 0;
    for (DWORD i = 0; n < max; i++) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(mi.szDevice, i, &dm)) break;
        if (dm.dmBitsPerPel != cur.dmBitsPerPel || dm.dmPelsWidth < 800 || dm.dmPelsHeight < 600 || dm.dmPelsWidth > 16384) continue;
        int v = (int)(dm.dmPelsWidth << 16 | dm.dmPelsHeight), dup = 0;
        for (int k = 0; k < n; k++) dup |= wh[k] == v;
        if (!dup) wh[n++] = v;
    }
    for (int a = 0; a < n; a++)          /* largest first */
        for (int b = a + 1; b < n; b++)
            if ((int64_t)(wh[b] >> 16) * (wh[b] & 0xFFFF) > (int64_t)(wh[a] >> 16) * (wh[a] & 0xFFFF)) { int t = wh[a]; wh[a] = wh[b]; wh[b] = t; }
    return n;
}

static int in_size_move;                 /* the user is dragging the window's frame */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (input_window_message(hwnd, msg, wp, lp)) return 0;   /* the Options menu / binding capture took it */
    switch (msg) {
    case WM_CLOSE:                       /* (pc-polish) Quit to desktop? first; again while it shows: at once */
        if (!pcmenu_quit_request || !menu_quit_showing || menu_quit_showing()) { close_window(hwnd); return 0; }
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        pcmenu_quit_request();
        return 0;
    case WM_APP_QUIT:                    /* (pc-polish) the quit was confirmed (menu_game.c, after a save in progress) */
        close_window(hwnd);
        return 0;
    case WM_DISPLAYCHANGE:               /* (pc-polish) a display mode change: fullscreen covers the monitor again */
        fit_fullscreen(hwnd);
        break;
    case WM_QUERYENDSESSION:             /* Windows is logging off / shutting down */
        return TRUE;
    case WM_ENDSESSION:
        if (wp) close_window(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;                        /* every pixel is drawn by render() */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        render(hwnd);
        return 0;
    }
    case WM_SIZE: {
        LONG w = wp == SIZE_MINIMIZED ? 0 : (LONG)LOWORD(lp), h = wp == SIZE_MINIMIZED ? 0 : (LONG)HIWORD(lp);
        if (w != client_w || h != client_h || !client_known) {
            InterlockedExchange(&client_w, w);
            InterlockedExchange(&client_h, h);
            InterlockedExchange(&client_known, 1);
            InterlockedIncrement(&resize_serial);
        }
        if (wp != SIZE_MINIMIZED) render(hwnd);   /* redraw at the new size right away */
        if (wp != SIZE_MINIMIZED && !in_size_move) gfx_output_resized();   /* (internal_scale = native follows) */
        return 0;
    }
    case WM_ENTERSIZEMOVE:               /* DefWindowProc runs a modal loop: keep frames coming */
        in_size_move = 1;
        SetTimer(hwnd, LIVE_RESIZE_TIMER, 10, NULL);
        return 0;
    case WM_EXITSIZEMOVE:
        in_size_move = 0;
        KillTimer(hwnd, LIVE_RESIZE_TIMER);
        gfx_output_resized();
        return 0;
    case WM_APP_WINDOW:                  /* (pc-menu) display mode / monitor from the settings */
        apply_window_settings(hwnd);
        return 0;
    case WM_APP_CLIENT_SIZE:
        apply_client_size(hwnd, (int)wp, (int)lp);
        return 0;
    case WM_TIMER:
        if (wp == LIVE_RESIZE_TIMER) { pump_frame(hwnd); return 0; }
        break;
    case WM_DPICHANGED:
        if (!win_fullscreen) {
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize.x = 240;
        ((MINMAXINFO*)lp)->ptMinTrackSize.y = 200;
        return 0;
    case WM_SETCURSOR:
        if (win_fullscreen && LOWORD(lp) == HTCLIENT) {   /* (the pointer only for the Options menu) */
            SetCursor(menu_is_open && menu_is_open() ? LoadCursorW(NULL, (LPCWSTR)IDC_ARROW) : NULL);
            return TRUE;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {                /* Alt+Enter */
            if (!(lp & (1 << 30))) { toggle_fullscreen(hwnd); settings_mark_chosen(settings_find("display", "mode")); }   /* not on auto-repeat; (pc-menu2) the player's choice */
            return 0;
        }
        if (wp == VK_F10) {                                      /* (r2-m2) F10 (a system key): Rendering Modern / Reference */
            if (!(lp & (1 << 30))) gfx_key(VK_F10, 0, 1);
            return 0;
        }
        break;                           /* Alt+F4 etc.: default handling */
    case WM_KEYDOWN:
        if (wp == VK_F9 && !(lp & (1 << 30))) {                  /* GPU backend debug keys (window_gpu.h) */
            if (debug_keys_on < 0) { const char* e = getenv("PS2_GS_DEBUG_KEYS"); debug_keys_on = e && *e && *e != '0'; }
            if (debug_keys_on) InterlockedOr(&debug_keys, GetKeyState(VK_SHIFT) < 0 ? WINDOW_KEY_LOSE_DEVICE : WINDOW_KEY_SWITCH);
            return 0;
        }
        /* (Esc no longer leaves fullscreen: it opens the Options menu - runtime/input.c INPUT_NAV_MENU) */
        if (wp == VK_F11 && !(lp & (1 << 30))) { toggle_fullscreen(hwnd); settings_mark_chosen(settings_find("display", "mode")); }
        else if (wp == VK_F12 && !(lp & (1 << 30))) { if (overlay_request_screenshot) overlay_request_screenshot(); }   /* (pc-shot) */
        else if (wp == VK_F7 && !(lp & (1 << 30))) switch_aspect();
        else if (wp == VK_F8 && !(lp & (1 << 30))) switch_hud();
        else if (wp >= VK_F2 && wp <= VK_F6 && !(lp & (1 << 30)))   /* graphics settings (gfx.h), remembered in sly2.ini */
            gfx_key((unsigned)wp, (GetKeyState(VK_SHIFT) < 0) | (GetKeyState(VK_CONTROL) < 0) << 1, 1);   /* (Ctrl+F2: scale auto) */
        return 0;                        /* game keys are read with GetAsyncKeyState */
    case WM_ACTIVATEAPP:
        if (wp) { reread_display(); gfx_reread(); }   /* sly2.ini may have been edited meanwhile */
        if (win_fullscreen) display_mode_fullscreen(hwnd, wp != 0);   /* (pc-pacing) the desktop's rate while in the background */
        if (menu_focus) menu_focus(wp != 0);          /* (pc-menu: silent in the background with [audio] background = off) */
        break;
    case WM_SYSCHAR:
        if (wp != ' ') return 0;         /* no beep for Alt+key; Alt+Space still opens the system menu */
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;    /* a lone Alt/F10 must not enter menu mode */
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* The display aspect (settings.h, notes/settings.md): 4:3 or wider. */
static void display_aspect(int* w, int* h) {
    *w = ps2_settings.aspect_w;
    *h = ps2_settings.aspect_h;
    if (*w <= 0 || *h <= 0) { *w = 4; *h = 3; }
}

/* Default client area DEFAULT_CLIENT_H (960) high at the display aspect (1280x960 at 4:3, 1707x960
 * at 16:9), or the largest of that aspect that fits the work area; centred. */
static void place_window(HWND hwnd) {
    RECT wr, cr;
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    int ncw = (wr.right - wr.left) - cr.right, nch = (wr.bottom - wr.top) - cr.bottom;
    int aw = mi.rcWork.right - mi.rcWork.left - ncw, ah = mi.rcWork.bottom - mi.rcWork.top - nch;
    int dw, dh;
    display_aspect(&dw, &dh);
    int cw = (int)(((int64_t)DEFAULT_CLIENT_H * dw * 2 + dh) / (2 * (int64_t)dh)), ch = DEFAULT_CLIENT_H;
    if (cw > aw || ch > ah) { wrect r = fit_aspect(aw, ah, dw, dh); cw = r.w; ch = r.h; }
    if (cw < 320 || ch < 240) { cw = 320; ch = 240; }
    int x = mi.rcWork.left + (aw - cw) / 2, y = mi.rcWork.top + (ah - ch) / 2;
    SetWindowPos(hwnd, NULL, x, y, cw + ncw, ch + nch, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* "Sly 2: Band of Thieves", with the frame rate when [window] show_fps is on (PS2_SHOW_FPS=1). */
static void set_title(HWND hwnd, double fps) {
    char t[96];
    if (ps2_settings.show_fps) snprintf(t, sizeof t, WINDOW_TITLE " - %.1f fps%s", fps, ext_on ? " (GPU)" : use_d3d ? "" : " (GDI)");
    else snprintf(t, sizeof t, WINDOW_TITLE);
    char cur[96];
    if (GetWindowTextA(hwnd, cur, sizeof cur) && !strcmp(cur, t)) return;
    SetWindowTextA(hwnd, t);
}

static DWORD WINAPI window_thread(LPVOID arg) {
    (void)arg;
    /* Per-monitor DPI awareness for this thread's windows: real pixels, no bitmap stretching. */
    typedef HANDLE (WINAPI* set_dpi_fn)(HANDLE);
    set_dpi_fn set_dpi = (set_dpi_fn)dll_proc(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (set_dpi && !set_dpi((HANDLE)(LONG_PTR)-4)) set_dpi((HANDLE)(LONG_PTR)-3);   /* PMv2, else PMv1 */
    /* A gamepad doesn't count as user input: keep the display from sleeping while the game runs. */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    /* the executable's icon (resource 1: the packaged build, scripts/build_game.py --package) */
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS, L"" WINDOW_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                DEFAULT_CLIENT_W, DEFAULT_CLIENT_H, NULL, NULL, inst, NULL);
    if (!hwnd) { wlog("CreateWindow failed (error %lu): no window", (unsigned long)GetLastError()); return 0; }
    if (!restore_window_state(hwnd)) place_window(hwnd);
    settings_save_if_missing();          /* sly2.ini template in the user data folder (settings.c) */

    const char* force_gdi = getenv("PS2_WINDOW_GDI");
    InterlockedExchange(&ext_changed, 0);
    if (ext_on) {                        /* an external renderer presents (window_gpu.h): no device here */
        use_d3d = 0;
        wlog("presenting through the GPU backend's swap chain");
    } else {
        use_d3d = !(force_gdi && *force_gdi && *force_gdi != '0') && d3d_init(hwnd);
        if (!use_d3d) {
            dwm_flush = (dwm_flush_fn)dll_proc(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
            if (force_gdi) wlog("GDI presenter (PS2_WINDOW_GDI)");
        }
    }
    {
        RECT rc;
        if (GetClientRect(hwnd, &rc) && !client_known) {
            InterlockedExchange(&client_w, rc.right);
            InterlockedExchange(&client_h, rc.bottom);
            InterlockedExchange(&client_known, 1);
        }
    }
    win_ready = 1;
    set_title(hwnd, 0.0);
    ShowWindow(hwnd, ps2_settings.win_w && ps2_settings.win_maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    if (ps2_settings.win_fullscreen) apply_window_settings(hwnd);
    SetForegroundWindow(hwnd);
    render(hwnd);
    g_hwnd = hwnd;
    input_set_window(hwnd);              /* keyboard input while it has focus */

    ULONGLONG fps_t0 = GetTickCount64();
    InterlockedExchange(&fb_presented, 0);   /* frames handed over while starting up are not in this interval */
    int menu_was_open = 0;
    for (;;) {
        /* (pc-menu) the Options menu reads its input every turn; while it is closed often enough for the PS button (no
         * window message) to open it without a noticeable delay. (pc-menu2) Mouse and keyboard messages wake the loop at
         * once; while the menu is open the controllers are read every 4 ms, and this presenter (software renderer)
         * draws the menu at the display's refresh between the game's pictures (render below; Present waits for it) */
        int menu_open_now = menu_is_open && menu_is_open();
        if (menu_open_now != menu_was_open) {    /* (pc-menu2) the pointer shows / hides at once, not at the next move */
            menu_was_open = menu_open_now;
            POINT pt;
            RECT rc;
            if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && GetClientRect(hwnd, &rc) && PtInRect(&rc, pt))
                SendMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
        MsgWaitForMultipleObjectsEx(1, &fb_event, menu_open_now ? (ext_on ? 4 : 1) : 40, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (InterlockedExchange(&ext_changed, 0)) {
            if (ext_on) {                /* an external renderer takes over: drop the own presenter */
                if (use_d3d) d3d_shutdown();
                use_d3d = 0;
            } else if (!use_d3d) {       /* back to the window's own presenter (the external one is gone) */
                use_d3d = !(force_gdi && *force_gdi && *force_gdi != '0') && d3d_init(hwnd);
                if (!use_d3d) dwm_flush = (dwm_flush_fn)dll_proc(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
                wlog("own presenter again (%s)", use_d3d ? "Direct3D 11" : "GDI");
                render(hwnd);
            }
        }
        if (menu_tick) menu_tick();
        unsigned seq0 = cur_seq;
        pump_frame(hwnd);
        if (menu_open_now && !ext_on && cur_seq == seq0) render(hwnd);   /* (pc-menu2) the menu between the game's pictures */
        ULONGLONG now = GetTickCount64();
        if (now - fps_t0 >= 1000) {
            LONG n = InterlockedExchange(&fb_presented, 0);
            double fps = (double)n * 1000.0 / (double)(now - fps_t0);
            set_title(hwnd, fps);
            if (overlay_set_fps) overlay_set_fps(fps);    /* (the counter in the picture, show_fps) */
            fps_t0 = now;
        }
    }
}

void* window_hwnd(int wait_ms) {
    for (int t = 0; !g_hwnd && t < wait_ms; t += 5) Sleep(5);
    return (void*)g_hwnd;
}

void window_client_size(int* w, int* h) {
    if (!client_known && g_hwnd) {
        RECT rc;
        if (GetClientRect(g_hwnd, &rc)) { *w = rc.right; *h = rc.bottom; return; }
    }
    *w = (int)client_w;
    *h = (int)client_h;
}

int window_fullscreen(void) { return *(volatile int*)&win_fullscreen; }

void window_init(void) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    if (!fb_setup()) { wlog("CreateEvent failed: no window"); return; }
    settings_on_apply(SET_APPLY_DISPLAY, display_applier);   /* (pc-menu) the settings the window applies */
    settings_on_apply(SET_APPLY_WINDOW, window_applier);
    gfx_output_size = window_client_size;                    /* internal_scale = native */
    input_init();                        /* controllers: found while the window opens */
    HANDLE t = CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    if (!t) { wlog("cannot start the window thread (error %lu)", (unsigned long)GetLastError()); return; }
    CloseHandle(t);
    InterlockedExchange(&win_accepting, 1);
}
