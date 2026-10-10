/* Window, presentation and input.
 *
 * The window lives on its own OS thread with its own message loop. The emulation thread hands
 * finished frames over through a triple buffer: window_present copies the picture into the free
 * buffer, publishes it with a pointer swap under a lock and returns at once. The window thread
 * then shows the newest frame, letterboxed at 4:3, with Direct3D 11 (one textured triangle,
 * bilinear filtering, vsync'd flip-model present) or, if Direct3D 11 is unavailable, with GDI
 * StretchDIBits. Alt+Enter / F11 toggle borderless fullscreen; closing the window ends the process.
 *
 * Input (window_get_input, called on the emulation thread): XInput controllers, the first
 * connected one being port 0 and the second port 1, plus the keyboard on port 0 while the window
 * has focus (mapping in notes/controls.md). Controllers are read directly when the game asks
 * (no added latency); empty XInput slots are probed off the emulation thread, since probing an
 * empty slot is slow (Microsoft recommends checking them only every few seconds).
 *
 * Every DLL beyond kernel32/user32/gdi32 (d3d11, d3dcompiler_47, xinput, dwmapi) is loaded at run
 * time: nothing needs adding to the link line, and a missing component only disables its feature.
 *
 * Environment: PS2_WINDOW_GDI=1 forces the GDI presenter.
 */
#define COBJMACROS
#include "hw.h"

#include <stdarg.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <xinput.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define FRAME_MAX        1024            /* gs_display never produces more than 1024x1024 */
#define DEFAULT_CLIENT_W 1280
#define DEFAULT_CLIENT_H 960
#define WINDOW_CLASS     L"Sly2Window"
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
 * Anchors need the debug build (function tracing); the runtime counts their calls
 * (ps2_func_calls). A token waits until the one before it has started. The button "none" presses
 * nothing (a pure wait point for the tokens after it). */
enum { SCRIPT_ABS, SCRIPT_REL, SCRIPT_ANCHOR, SCRIPT_QUIET };
#define SCRIPT_UNSET 0xFFFFFFFFu
#define SCRIPT_NONE  (-2)
static struct { uint32_t frame, hold, start, addr, ra, base, last; int button, kind, armed, seen; } script[256];
static int script_n = -1;
static uint32_t script_frame;
uint32_t ps2_func_calls(uint32_t addr, uint32_t ra);   /* runtime: calls of an anchor so far */
uint32_t ps2_vblank_count(void);

static void script_parse(const char* s) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", s ? s : "");
    for (char* tok = strtok(buf, ","); tok && script_n < 256; tok = strtok(NULL, ",")) {
        char name[32] = { 0 };
        unsigned frame = 0, hold = 6, addr = 0, ra = 0;
        int kind = SCRIPT_ABS;
        if (tok[0] == '+') {
            kind = SCRIPT_REL;
            if (sscanf(tok + 1, "%u:%31[a-z0-9]:%u", &frame, name, &hold) < 2) continue;
        } else if (tok[0] == '@') {
            kind = tok[1] == '!' ? SCRIPT_QUIET : SCRIPT_ANCHOR;
            char* p = tok + (kind == SCRIPT_QUIET ? 2 : 1);
            addr = (unsigned)strtoul(p, &p, 16);
            if (*p == '/') ra = (unsigned)strtoul(p + 1, &p, 16);
            if (kind == SCRIPT_QUIET) frame = 30;
            if (*p == '+') frame = (unsigned)strtoul(p + 1, &p, 10);
            if (*p != ':' || sscanf(p + 1, "%31[a-z0-9]:%u", name, &hold) < 1) continue;
        } else if (sscanf(tok, "%u:%31[a-z0-9]:%u", &frame, name, &hold) < 2) continue;
        int found = strcmp(name, "none") ? -1 : SCRIPT_NONE;
        for (int b = 0; b < SCRIPT_NBUTTONS; b++)
            if (!strcmp(name, script_buttons[b].name)) found = b;
        for (int b = 0; b < (int)(sizeof script_sticks / sizeof script_sticks[0]); b++)
            if (!strcmp(name, script_sticks[b].name)) found = SCRIPT_NBUTTONS + b;
        if (found == -1) { wlog("PS2_INPUT: unknown button in '%s'", tok); continue; }
        script[script_n].frame = frame; script[script_n].hold = hold; script[script_n].button = found;
        script[script_n].kind = kind; script[script_n].addr = addr; script[script_n].ra = ra;
        script[script_n].armed = script[script_n].seen = 0;
        script[script_n++].start = kind == SCRIPT_ABS ? frame : SCRIPT_UNSET;
    }
}

static const char* script_name(int b) {
    return b == SCRIPT_NONE ? "none" : b >= SCRIPT_NBUTTONS ? script_sticks[b - SCRIPT_NBUTTONS].name
                                                            : script_buttons[b].name;
}

/* Give relative and anchored tokens their start frame once the token before them has started. */
static void script_resolve(uint32_t now) {
    for (int i = 0; i < script_n; i++) {
        if (script[i].start != SCRIPT_UNSET) continue;
        uint32_t prev = i ? script[i - 1].start : 0;
        if (prev == SCRIPT_UNSET || now < prev) return;          /* previous one hasn't started */
        if (script[i].kind == SCRIPT_REL) { script[i].start = prev + script[i].frame; continue; }
        uint32_t calls = ps2_func_calls(script[i].addr, script[i].ra);
        if (!script[i].armed) { script[i].armed = 1; script[i].base = calls; script[i].last = now; }
        if (script[i].kind == SCRIPT_QUIET) {                    /* called, then silent for n frames */
            if (calls != script[i].base) { script[i].base = calls; script[i].last = now; script[i].seen = 1; }
            if (!script[i].seen || now - script[i].last < script[i].frame) return;
            script[i].start = now;
            wlog("pad frame %u: function %08X silent for %u frames, '%s'", now, script[i].addr,
                 script[i].frame, script_name(script[i].button));
            continue;
        }
        if (calls == script[i].base) return;                     /* still waiting for the call */
        script[i].start = now + script[i].frame;
        wlog("pad frame %u: function %08X called, '%s' at %u", now, script[i].addr,
             script_name(script[i].button), script[i].start);
    }
}

static void script_apply(int port, pad_input* in) {
    if (script_n < 0) {
        script_n = 0;
        const char* s = getenv("PS2_INPUT");
        script_parse(s);
        if (s) wlog("%d scripted presses", script_n);
    }
    if (port != 0 || !script_n) return;
    uint32_t now = script_frame++;
    script_resolve(now);
    for (int i = 0; i < script_n; i++) {
        if (script[i].start == SCRIPT_UNSET) continue;
        if (now < script[i].start || now >= script[i].start + script[i].hold) continue;
        int b = script[i].button;
        if (now == script[i].start)                           /* calibrate pad frames vs VBlanks */
            wlog("pad frame %u (VBlank %u): %s (%u frames)", now, ps2_vblank_count(), script_name(b), script[i].hold);
        if (b == SCRIPT_NONE) continue;
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

/* The largest 4:3 rectangle centred in a cw x ch client area. */
static wrect letterbox(int cw, int ch) {
    wrect r = { 0, 0, cw, ch };
    if (cw <= 0 || ch <= 0) { r.w = r.h = 0; return r; }
    if ((int64_t)cw * 3 > (int64_t)ch * 4) {         /* wider than 4:3: bars left and right */
        r.w = (int)(((int64_t)ch * 4 + 1) / 3);
        r.x = (cw - r.w) / 2;
    } else {                                         /* taller: bars top and bottom */
        r.h = (int)(((int64_t)cw * 3 + 2) / 4);
        r.y = (ch - r.h) / 2;
    }
    return r;
}

/* Indices into pad_input.pressure */
enum { PR_RIGHT, PR_LEFT, PR_UP, PR_DOWN, PR_TRIANGLE, PR_CIRCLE, PR_CROSS, PR_SQUARE,
       PR_L1, PR_R1, PR_L2, PR_R2, PR_NONE = -1 };

static void pad_clear(pad_input* p) {
    memset(p, 0, sizeof *p);
    p->lx = p->ly = p->rx = p->ry = 0x80;
}

static uint8_t axis_byte(float v) {                  /* -1..1 -> 0..255, 0 -> 0x80 */
    int b = (int)floorf(128.0f + v * 128.0f);
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

/* XInput stick (-32768..32767, +y = up) -> DualShock 2 axes (0..255, centre 0x80, +y = down),
 * with a scaled radial dead zone so small drift reads as centred and full tilt still reaches the end. */
static void xi_stick(int sx, int sy, int deadzone, uint8_t* ox, uint8_t* oy) {
    float x = (float)sx, y = (float)sy;
    float mag = sqrtf(x * x + y * y);
    if (mag <= (float)deadzone) { *ox = *oy = 0x80; return; }
    float norm = (mag - (float)deadzone) / (32767.0f - (float)deadzone);
    if (norm > 1.0f) norm = 1.0f;
    float k = norm / mag;
    *ox = axis_byte(x * k);
    *oy = axis_byte(-y * k);
}

static const struct { uint16_t xi, pad; int8_t pr; } xi_buttons[] = {
    { XINPUT_GAMEPAD_DPAD_UP,        PAD_UP,       PR_UP },
    { XINPUT_GAMEPAD_DPAD_DOWN,      PAD_DOWN,     PR_DOWN },
    { XINPUT_GAMEPAD_DPAD_LEFT,      PAD_LEFT,     PR_LEFT },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     PAD_RIGHT,    PR_RIGHT },
    { XINPUT_GAMEPAD_A,              PAD_CROSS,    PR_CROSS },
    { XINPUT_GAMEPAD_B,              PAD_CIRCLE,   PR_CIRCLE },
    { XINPUT_GAMEPAD_X,              PAD_SQUARE,   PR_SQUARE },
    { XINPUT_GAMEPAD_Y,              PAD_TRIANGLE, PR_TRIANGLE },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  PAD_L1,       PR_L1 },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, PAD_R1,       PR_R1 },
    { XINPUT_GAMEPAD_LEFT_THUMB,     PAD_L3,       PR_NONE },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    PAD_R3,       PR_NONE },
    { XINPUT_GAMEPAD_BACK,           PAD_SELECT,   PR_NONE },
    { XINPUT_GAMEPAD_START,          PAD_START,    PR_NONE },
};

/* Merge an XInput pad into p (buttons OR'd, sticks and trigger pressures overwritten). */
static void xi_to_pad(const XINPUT_GAMEPAD* g, pad_input* p) {
    for (size_t i = 0; i < sizeof xi_buttons / sizeof xi_buttons[0]; i++)
        if (g->wButtons & xi_buttons[i].xi) {
            p->buttons |= xi_buttons[i].pad;
            if (xi_buttons[i].pr != PR_NONE) p->pressure[xi_buttons[i].pr] = 0xFF;
        }
    if (g->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) { p->buttons |= PAD_L2; p->pressure[PR_L2] = g->bLeftTrigger; }
    if (g->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) { p->buttons |= PAD_R2; p->pressure[PR_R2] = g->bRightTrigger; }
    xi_stick(g->sThumbLX, g->sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &p->lx, &p->ly);
    xi_stick(g->sThumbRX, g->sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, &p->rx, &p->ry);
}

/* Keyboard mapping for port 0 (keep notes/controls.md in sync). Virtual-key codes; VK_LCONTROL
 * is the left Ctrl key only. Numpad keys need Num Lock on. */
static const struct { uint8_t vk; uint16_t pad; int8_t pr; } kb_buttons[] = {
    { VK_UP,      PAD_UP,       PR_UP },
    { VK_DOWN,    PAD_DOWN,     PR_DOWN },
    { VK_LEFT,    PAD_LEFT,     PR_LEFT },
    { VK_RIGHT,   PAD_RIGHT,    PR_RIGHT },
    { VK_SPACE,   PAD_CROSS,    PR_CROSS },
    { VK_LCONTROL,PAD_CIRCLE,   PR_CIRCLE },
    { 'F',        PAD_SQUARE,   PR_SQUARE },
    { 'R',        PAD_TRIANGLE, PR_TRIANGLE },
    { VK_NUMPAD2, PAD_CROSS,    PR_CROSS },
    { VK_NUMPAD6, PAD_CIRCLE,   PR_CIRCLE },
    { VK_NUMPAD4, PAD_SQUARE,   PR_SQUARE },
    { VK_NUMPAD8, PAD_TRIANGLE, PR_TRIANGLE },
    { 'Q',        PAD_L1,       PR_L1 },
    { 'E',        PAD_R1,       PR_R1 },
    { '1',        PAD_L2,       PR_L2 },
    { '3',        PAD_R2,       PR_R2 },
    { 'C',        PAD_L3,       PR_NONE },
    { 'U',        PAD_R3,       PR_NONE },
    { VK_RETURN,  PAD_START,    PR_NONE },
    { VK_BACK,    PAD_SELECT,   PR_NONE },
};
/* Stick keys: negative / positive direction per axis (lx, ly, rx, ry). */
static const uint8_t kb_axes[4][2] = { { 'A', 'D' }, { 'W', 'S' }, { 'J', 'L' }, { 'I', 'K' } };

/* Merge keyboard state (down[vk] != 0 = held) into p. A stick axis driven by keys overrides the
 * controller; opposite keys cancel. */
static void kb_to_pad(const uint8_t down[256], pad_input* p) {
    for (size_t i = 0; i < sizeof kb_buttons / sizeof kb_buttons[0]; i++)
        if (down[kb_buttons[i].vk]) {
            p->buttons |= kb_buttons[i].pad;
            if (kb_buttons[i].pr != PR_NONE) p->pressure[kb_buttons[i].pr] = 0xFF;
        }
    uint8_t* axes[4] = { &p->lx, &p->ly, &p->rx, &p->ry };
    for (int a = 0; a < 4; a++) {
        int neg = down[kb_axes[a][0]] != 0, pos = down[kb_axes[a][1]] != 0;
        if (neg != pos) *axes[a] = neg ? 0x00 : 0xFF;
    }
}

/* ==== frame hand-off: triple buffer ============================================================== */

/* fb_write belongs to the producer (window_present), fb_read to the window thread; fb_ready and
 * fb_fresh are shared and only touched under fb_lock. Pixels are copied outside the lock (the
 * producer owns its buffer), so the lock is held for an index swap only. */
static uint32_t fb_pix[3][FRAME_MAX * FRAME_MAX];
static int fb_w[3], fb_h[3];
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

void window_present(const uint32_t* bgrx, int w, int h, int stride) {
    if (!win_accepting) return;
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

    AcquireSRWLockExclusive(&fb_lock);   /* publish: the written buffer becomes "ready" */
    fb_write = fb_ready;
    fb_ready = b;
    fb_fresh = 1;
    ReleaseSRWLockExclusive(&fb_lock);
    InterlockedIncrement(&fb_presented);
    SetEvent(fb_event);
}

/* ==== XInput ===================================================================================== */

typedef DWORD (WINAPI* xinput_get_state_fn)(DWORD, XINPUT_STATE*);
static xinput_get_state_fn xi_get_state;
static INIT_ONCE xi_once = INIT_ONCE_STATIC_INIT;
static volatile LONG xi_connected[XUSER_MAX_COUNT];
static SRWLOCK xi_lock[XUSER_MAX_COUNT];   /* serialises XInputGetState per slot (zero = SRWLOCK_INIT) */
static volatile LONG xi_probing_thread;    /* the window thread probes empty slots */

/* Read one slot; updates its connected flag. */
static int xi_poll(DWORD slot, XINPUT_STATE* st) {
    XINPUT_STATE tmp;
    AcquireSRWLockExclusive(&xi_lock[slot]);
    DWORD r = xi_get_state(slot, st ? st : &tmp);
    ReleaseSRWLockExclusive(&xi_lock[slot]);
    LONG ok = r == ERROR_SUCCESS;
    if (InterlockedExchange(&xi_connected[slot], ok) != ok)
        wlog("XInput controller %lu %s", (unsigned long)slot, ok ? "connected" : "disconnected");
    return (int)ok;
}

static BOOL CALLBACK xi_load(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    static const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (size_t i = 0; i < sizeof dlls / sizeof dlls[0] && !xi_get_state; i++)
        xi_get_state = (xinput_get_state_fn)dll_proc(LoadLibraryW(dlls[i]), "XInputGetState");
    if (!xi_get_state) { wlog("XInput unavailable: controllers disabled"); return TRUE; }
    for (DWORD s = 0; s < XUSER_MAX_COUNT; s++) xi_poll(s, NULL);
    return TRUE;
}

/* Probe one empty slot, at most every 500 ms (so each slot every ~2 s). Any thread. */
static void xi_probe_step(void) {
    static volatile LONG busy;
    static ULONGLONG next;
    static int slot;
    if (!xi_get_state || InterlockedCompareExchange(&busy, 1, 0)) return;
    ULONGLONG now = GetTickCount64();
    if (now >= next) {
        next = now + 500;
        for (int k = 0; k < XUSER_MAX_COUNT; k++) {
            int s = (slot + k) % XUSER_MAX_COUNT;
            if (!xi_connected[s]) { xi_poll((DWORD)s, NULL); slot = s + 1; break; }
        }
    }
    InterlockedExchange(&busy, 0);
}

/* Port n = the n-th connected XInput slot. */
static int xi_slot_for_port(int port) {
    for (int s = 0, n = 0; s < XUSER_MAX_COUNT; s++)
        if (xi_connected[s] && n++ == port) return s;
    return -1;
}

/* ==== keyboard =================================================================================== */

static HWND volatile g_hwnd;             /* published once the window and renderer are ready */

/* Snapshot of the mapped keys, if the window has focus. Alt held = window shortcut, not input. */
static int kb_read(uint8_t down[256]) {
    HWND h = g_hwnd;
    if (!h || GetForegroundWindow() != h || GetAsyncKeyState(VK_MENU) < 0) return 0;
    memset(down, 0, 256);
    for (size_t i = 0; i < sizeof kb_buttons / sizeof kb_buttons[0]; i++)
        down[kb_buttons[i].vk] = GetAsyncKeyState(kb_buttons[i].vk) < 0;
    for (int a = 0; a < 4; a++)
        for (int d = 0; d < 2; d++) down[kb_axes[a][d]] = GetAsyncKeyState(kb_axes[a][d]) < 0;
    return 1;
}

void window_get_input(int port, pad_input* out) {
    pad_clear(out);
    if (port < 0 || port > 1) return;
    InitOnceExecuteOnce(&xi_once, xi_load, NULL, NULL);
    if (xi_get_state) {
        if (!xi_probing_thread) xi_probe_step();    /* headless: no window thread to do it */
        int slot = xi_slot_for_port(port);
        XINPUT_STATE st;
        if (slot >= 0 && xi_poll((DWORD)slot, &st)) xi_to_pad(&st.Gamepad, out);
    }
    uint8_t down[256];
    if (port == 0 && kb_read(down)) kb_to_pad(down, out);
    script_apply(port, out);                                  /* PS2_INPUT test scripts */
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

static void d3d_shutdown(void) {
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
static HRESULT d3d_render(HWND hwnd, const uint32_t* px, int fw, int fh, unsigned seq) {
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
        wrect r = letterbox(rc.right, rc.bottom);
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
    return IDXGISwapChain_Present(d3d.sc, 1, 0);
}

/* ==== GDI presenter (fallback) =================================================================== */

typedef HRESULT (WINAPI* dwm_flush_fn)(void);
static dwm_flush_fn dwm_flush;

static void gdi_render(HWND hwnd, const uint32_t* px, int fw, int fh) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (px) {
        wrect r = letterbox(rc.right, rc.bottom);
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
    if (!win_ready) return;
    const uint32_t* px = cur_seq ? fb_pix[fb_read] : NULL;
    int fw = fb_w[fb_read], fh = fb_h[fb_read];
    if (!use_d3d) { gdi_render(hwnd, px, fw, fh); return; }

    HRESULT hr = d3d_render(hwnd, px, fw, fh, cur_seq);
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
        gdi_render(hwnd, px, fw, fh);
    } else {
        d3d_render(hwnd, px, fw, fh, cur_seq);
    }
}

/* Show the newest frame if one arrived. */
static void pump_frame(HWND hwnd) {
    if (!fb_take()) return;
    if (++cur_seq == 0) cur_seq = 1;
    render(hwnd);
    InterlockedIncrement(&frames_shown);
}

/* Borderless fullscreen on the window's monitor, and back to the saved placement. */
static void toggle_fullscreen(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!win_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        win_placement.length = sizeof win_placement;
        if (!GetWindowPlacement(hwnd, &win_placement) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        win_fullscreen = 1;
    } else {
        win_fullscreen = 0;
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &win_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }                                    /* the resulting WM_SIZE redraws */
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        ExitProcess(0);
    case WM_ERASEBKGND:
        return 1;                        /* every pixel is drawn by render() */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        render(hwnd);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) render(hwnd);   /* redraw at the new size right away */
        return 0;
    case WM_ENTERSIZEMOVE:               /* DefWindowProc runs a modal loop: keep frames coming */
        SetTimer(hwnd, LIVE_RESIZE_TIMER, 10, NULL);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(hwnd, LIVE_RESIZE_TIMER);
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
        if (win_fullscreen && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {                /* Alt+Enter */
            if (!(lp & (1 << 30))) toggle_fullscreen(hwnd);       /* not on auto-repeat */
            return 0;
        }
        break;                           /* Alt+F4 etc.: default handling */
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1 << 30))) toggle_fullscreen(hwnd);
        else if (wp == VK_ESCAPE && win_fullscreen) toggle_fullscreen(hwnd);
        return 0;                        /* game keys are read with GetAsyncKeyState */
    case WM_SYSCHAR:
        if (wp != ' ') return 0;         /* no beep for Alt+key; Alt+Space still opens the system menu */
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;    /* a lone Alt/F10 must not enter menu mode */
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Default 1280x960 client area (or the largest 4:3 that fits the work area), centred. */
static void place_window(HWND hwnd) {
    RECT wr, cr;
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    int ncw = (wr.right - wr.left) - cr.right, nch = (wr.bottom - wr.top) - cr.bottom;
    int aw = mi.rcWork.right - mi.rcWork.left - ncw, ah = mi.rcWork.bottom - mi.rcWork.top - nch;
    int cw = DEFAULT_CLIENT_W, ch = DEFAULT_CLIENT_H;
    if (cw > aw || ch > ah) { wrect r = letterbox(aw, ah); cw = r.w; ch = r.h; }
    if (cw < 320 || ch < 240) { cw = 320; ch = 240; }
    int x = mi.rcWork.left + (aw - cw) / 2, y = mi.rcWork.top + (ah - ch) / 2;
    SetWindowPos(hwnd, NULL, x, y, cw + ncw, ch + nch, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void set_title(HWND hwnd, double fps) {
    char t[64];
    snprintf(t, sizeof t, "Sly 2 - %.1f fps%s", fps, use_d3d ? "" : " (GDI)");
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
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS, L"Sly 2", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                DEFAULT_CLIENT_W, DEFAULT_CLIENT_H, NULL, NULL, inst, NULL);
    if (!hwnd) { wlog("CreateWindow failed (error %lu): no window", (unsigned long)GetLastError()); return 0; }
    place_window(hwnd);

    const char* force_gdi = getenv("PS2_WINDOW_GDI");
    use_d3d = !(force_gdi && *force_gdi && *force_gdi != '0') && d3d_init(hwnd);
    if (!use_d3d) {
        dwm_flush = (dwm_flush_fn)dll_proc(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
        if (force_gdi) wlog("GDI presenter (PS2_WINDOW_GDI)");
    }
    win_ready = 1;
    set_title(hwnd, 0.0);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    render(hwnd);
    g_hwnd = hwnd;
    InterlockedExchange(&xi_probing_thread, 1);

    ULONGLONG fps_t0 = GetTickCount64();
    InterlockedExchange(&fb_presented, 0);   /* frames handed over while starting up are not in this interval */
    for (;;) {
        MsgWaitForMultipleObjectsEx(1, &fb_event, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        pump_frame(hwnd);
        xi_probe_step();
        ULONGLONG now = GetTickCount64();
        if (now - fps_t0 >= 1000) {
            LONG n = InterlockedExchange(&fb_presented, 0);
            set_title(hwnd, (double)n * 1000.0 / (double)(now - fps_t0));
            fps_t0 = now;
        }
    }
}

void window_init(void) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    if (!fb_setup()) { wlog("CreateEvent failed: no window"); return; }
    InitOnceExecuteOnce(&xi_once, xi_load, NULL, NULL);
    HANDLE t = CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    if (!t) { wlog("cannot start the window thread (error %lu)", (unsigned long)GetLastError()); return; }
    CloseHandle(t);
    InterlockedExchange(&win_accepting, 1);
}
