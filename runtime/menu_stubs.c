/* Weak stand-ins for the interfaces of menu_ext.h until the pc-input and pc-pacing streams provide them (their
 * strong definitions replace these at link time). They keep the menu usable on its own: the bindings pages show
 * the current fixed mapping (window.c, notes/controls.md) and remember a choice for the session only - the input
 * stream stores and applies bindings; pacing changes apply at the next start. */
#include "menu.h"
#include "menu_ext.h"
#include "hw.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define WEAK __attribute__((weak))

/* keyboard: window.c kb_buttons / kb_axes */
static const struct { const char* name; unsigned vk; } kb_default[] = {
    { "Move forward", 'W' }, { "Move back", 'S' }, { "Move left", 'A' }, { "Move right", 'D' },
    { "Jump (Cross)", VK_SPACE }, { "Attack (Square)", 'F' }, { "Action (Circle)", VK_LCONTROL }, { "Triangle", 'R' },
    { "Camera up", 'I' }, { "Camera down", 'K' }, { "Camera left", 'J' }, { "Camera right", 'L' },
    { "L1", 'Q' }, { "R1", 'E' }, { "L2", '1' }, { "R2", '3' }, { "L3", 'C' }, { "Binocucom (R3)", 'U' },
    { "Pause (Start)", VK_RETURN }, { "Gadgets (Select)", VK_BACK },
    { "D-pad up", VK_UP }, { "D-pad down", VK_DOWN }, { "D-pad left", VK_LEFT }, { "D-pad right", VK_RIGHT },
};
#define NKB ((int)(sizeof kb_default / sizeof kb_default[0]))
static const struct { const char* name; int code; } pad_default[] = {
    { "Jump (Cross)", PAD_CROSS }, { "Action (Circle)", PAD_CIRCLE }, { "Attack (Square)", PAD_SQUARE },
    { "Triangle", PAD_TRIANGLE }, { "L1", PAD_L1 }, { "R1", PAD_R1 }, { "L2", PAD_L2 }, { "R2", PAD_R2 },
    { "L3", PAD_L3 }, { "Binocucom (R3)", PAD_R3 }, { "Pause (Start)", PAD_START }, { "Gadgets (Select)", PAD_SELECT },
};
#define NPAD ((int)(sizeof pad_default / sizeof pad_default[0]))
static unsigned kb_now[NKB];
static int pad_now[NPAD];

WEAK int input_bind_count(int device) { return device ? NPAD : NKB; }

WEAK const char* input_bind_name(int device, int action) {
    if (device) return action >= 0 && action < NPAD ? pad_default[action].name : "";
    return action >= 0 && action < NKB ? kb_default[action].name : "";
}

static void key_name(unsigned vk, char* buf, size_t cap) {
    static const struct { unsigned vk; const char* name; } names[] = {
        { VK_SPACE, "Space" }, { VK_RETURN, "Enter" }, { VK_BACK, "Backspace" }, { VK_LCONTROL, "Left Ctrl" },
        { VK_RCONTROL, "Right Ctrl" }, { VK_LSHIFT, "Left Shift" }, { VK_RSHIFT, "Right Shift" }, { VK_UP, "Up" },
        { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" }, { VK_TAB, "Tab" }, { VK_CONTROL, "Ctrl" },
        { VK_SHIFT, "Shift" }, { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" },
        { VK_PRIOR, "Page Up" }, { VK_NEXT, "Page Down" }, { VK_CAPITAL, "Caps Lock" },
    };
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
        if (names[k].vk == vk) { snprintf(buf, cap, "%s", names[k].name); return; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { snprintf(buf, cap, "%c", (char)vk); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(buf, cap, "Numpad %u", vk - VK_NUMPAD0); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(buf, cap, "F%u", vk - VK_F1 + 1); return; }
    wchar_t w[64];
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), w, 64) > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
    else snprintf(buf, cap, "Key %u", vk);
}

static const char* pad_name(int code, int xbox) {
    static const struct { int code; const char* ps; const char* xb; } names[] = {
        { PAD_CROSS, "Cross", "A" }, { PAD_CIRCLE, "Circle", "B" }, { PAD_SQUARE, "Square", "X" }, { PAD_TRIANGLE, "Triangle", "Y" },
        { PAD_L1, "L1", "LB" }, { PAD_R1, "R1", "RB" }, { PAD_L2, "L2", "LT" }, { PAD_R2, "R2", "RT" },
        { PAD_L3, "L3", "Left stick" }, { PAD_R3, "R3", "Right stick" }, { PAD_START, "Options", "Menu" },
        { PAD_SELECT, "Create", "View" }, { PAD_UP, "D-pad up", "D-pad up" }, { PAD_DOWN, "D-pad down", "D-pad down" },
        { PAD_LEFT, "D-pad left", "D-pad left" }, { PAD_RIGHT, "D-pad right", "D-pad right" },
    };
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
        if (names[k].code == code) return xbox ? names[k].xb : names[k].ps;
    return "?";
}

WEAK void input_bind_text(int device, int action, char* buf, size_t cap) {
    if (!cap) return;
    buf[0] = 0;
    if (device) {
        if (action < 0 || action >= NPAD) return;
        snprintf(buf, cap, "%s", pad_name(pad_now[action] ? pad_now[action] : pad_default[action].code,
                                          input_controller_kind() == MENU_DEV_XBOX));
    } else {
        if (action < 0 || action >= NKB) return;
        key_name(kb_now[action] ? kb_now[action] : kb_default[action].vk, buf, cap);
    }
}

WEAK int input_bind_set(int device, int action, int code) {
    if (device) {
        if (action < 0 || action >= NPAD) return 0;
        pad_now[action] = code;
    } else {
        if (action < 0 || action >= NKB) return 0;
        kb_now[action] = (unsigned)code;
    }
    fprintf(stderr, "[menu] binding %s %d -> %d (this build: shown only; the input subsystem applies bindings)\n",
            device ? "controller" : "keyboard", action, code);
    return 1;
}

WEAK void input_bind_reset(int device) {
    if (device) memset(pad_now, 0, sizeof pad_now);
    else memset(kb_now, 0, sizeof kb_now);
}

WEAK void input_apply_settings(void) {}
WEAK int input_controller_kind(void) { return MENU_DEV_PLAYSTATION; }
WEAK void input_rumble_test(int strength) { (void)strength; }

WEAK int pacing_apply_mode(int pacing) { (void)pacing; return 0; }
WEAK int pacing_exclusive_fullscreen(int on) { (void)on; return 0; }
