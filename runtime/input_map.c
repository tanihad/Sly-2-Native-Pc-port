/* Input mapping, options and names: controller state -> DualShock 2, keyboard -> DualShock 2, the
 * [controls] keys of sly2.ini, menu navigation repeat and port assignment. Pure functions (no I/O);
 * notes/controls.md documents the defaults and the ini keys. */
#include "input_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>                              /* VK_* */
#include <xinput.h>

/* ---- names --------------------------------------------------------------------------------------------- */

static const struct { const char* ini; const char* label; } actions[INPUT_ACT_COUNT] = {
    { "cross", "Cross" }, { "circle", "Circle" }, { "square", "Square" }, { "triangle", "Triangle" },
    { "l1", "L1" }, { "r1", "R1" }, { "l2", "L2" }, { "r2", "R2" }, { "l3", "L3" }, { "r3", "R3" },
    { "select", "Select" }, { "start", "Start" },
    { "up", "D-pad up" }, { "down", "D-pad down" }, { "left", "D-pad left" }, { "right", "D-pad right" },
    { "lstick_up", "Left stick up" }, { "lstick_down", "Left stick down" },
    { "lstick_left", "Left stick left" }, { "lstick_right", "Left stick right" },
    { "rstick_up", "Right stick up (camera)" }, { "rstick_down", "Right stick down (camera)" },
    { "rstick_left", "Right stick left (camera)" }, { "rstick_right", "Right stick right (camera)" },
};

const char* input_action_label(int act) { return act >= 0 && act < INPUT_ACT_COUNT ? actions[act].label : "?"; }
const char* input_action_ini(int act) { return act >= 0 && act < INPUT_ACT_COUNT ? actions[act].ini : "?"; }

/* PS2 button bit and pressure index of each pad action (pressure -1: none) */
static const struct { uint16_t bit; int8_t pressure; } act_pad[INPUT_ACT_PAD_COUNT] = {
    { PAD_CROSS, 6 }, { PAD_CIRCLE, 5 }, { PAD_SQUARE, 7 }, { PAD_TRIANGLE, 4 },
    { PAD_L1, 8 }, { PAD_R1, 9 }, { PAD_L2, 10 }, { PAD_R2, 11 }, { PAD_L3, -1 }, { PAD_R3, -1 },
    { PAD_SELECT, -1 }, { PAD_START, -1 }, { PAD_UP, 2 }, { PAD_DOWN, 3 }, { PAD_LEFT, 1 }, { PAD_RIGHT, 0 },
};

static const struct { const char* ini; const char* ps; const char* xbox; } buttons[INPUT_BTN_COUNT] = {
    { "cross", "Cross", "A" }, { "circle", "Circle", "B" }, { "square", "Square", "X" },
    { "triangle", "Triangle", "Y" }, { "l1", "L1", "LB" }, { "r1", "R1", "RB" }, { "l2", "L2", "LT" },
    { "r2", "R2", "RT" }, { "l3", "L3", "Left stick press" }, { "r3", "R3", "Right stick press" },
    { "select", "Create", "View" }, { "start", "Options", "Menu" }, { "home", "PS button", "Guide" },
    { "touchpad", "Touchpad", "(touchpad)" }, { "mute", "Mute", "(mute)" },
    { "up", "D-pad up", "D-pad up" }, { "down", "D-pad down", "D-pad down" },
    { "left", "D-pad left", "D-pad left" }, { "right", "D-pad right", "D-pad right" },
};
static const struct { const char* name; int btn; } button_alias[] = {
    { "a", INPUT_BTN_CROSS }, { "b", INPUT_BTN_CIRCLE }, { "x", INPUT_BTN_SQUARE }, { "y", INPUT_BTN_TRIANGLE },
    { "lb", INPUT_BTN_L1 }, { "rb", INPUT_BTN_R1 }, { "lt", INPUT_BTN_L2 }, { "rt", INPUT_BTN_R2 },
    { "ls", INPUT_BTN_L3 }, { "rs", INPUT_BTN_R3 }, { "back", INPUT_BTN_SELECT }, { "view", INPUT_BTN_SELECT },
    { "create", INPUT_BTN_SELECT }, { "share", INPUT_BTN_SELECT }, { "options", INPUT_BTN_START },
    { "menu", INPUT_BTN_START }, { "ps", INPUT_BTN_HOME }, { "guide", INPUT_BTN_HOME },
    { "dpad_up", INPUT_BTN_UP }, { "dpad_down", INPUT_BTN_DOWN }, { "dpad_left", INPUT_BTN_LEFT },
    { "dpad_right", INPUT_BTN_RIGHT },
};

const char* input_button_label(int btn, int xbox_style) {
    if (btn < 0 || btn >= INPUT_BTN_COUNT) return "(none)";
    return xbox_style ? buttons[btn].xbox : buttons[btn].ps;
}
const char* input_button_ini(int btn) { return btn >= 0 && btn < INPUT_BTN_COUNT ? buttons[btn].ini : "none"; }

int input_button_from_name(const char* name) {
    for (int i = 0; i < INPUT_BTN_COUNT; i++) if (!_stricmp(name, buttons[i].ini)) return i;
    for (size_t i = 0; i < sizeof button_alias / sizeof button_alias[0]; i++)
        if (!_stricmp(name, button_alias[i].name)) return button_alias[i].btn;
    return -1;
}

/* Bindable keys: ini name, label. F2..F12 (window, graphics and screenshot keys), Esc (menu), Alt and the Windows
 * keys are not bindable. */
static const struct { uint8_t vk; const char* ini; const char* label; } keys[] = {
    { VK_SPACE, "Space", "Space" }, { VK_RETURN, "Enter", "Enter" }, { VK_BACK, "Backspace", "Backspace" },
    { VK_TAB, "Tab", "Tab" }, { VK_LSHIFT, "LShift", "Left Shift" }, { VK_RSHIFT, "RShift", "Right Shift" },
    { VK_LCONTROL, "LCtrl", "Left Ctrl" }, { VK_RCONTROL, "RCtrl", "Right Ctrl" }, { VK_CAPITAL, "CapsLock", "Caps Lock" },
    { VK_UP, "Up", "Up arrow" }, { VK_DOWN, "Down", "Down arrow" }, { VK_LEFT, "Left", "Left arrow" },
    { VK_RIGHT, "Right", "Right arrow" }, { VK_INSERT, "Insert", "Insert" }, { VK_DELETE, "Delete", "Delete" },
    { VK_HOME, "Home", "Home" }, { VK_END, "End", "End" }, { VK_PRIOR, "PageUp", "Page Up" },
    { VK_NEXT, "PageDown", "Page Down" }, { VK_F1, "F1", "F1" },
    { VK_NUMPAD0, "Numpad0", "Numpad 0" }, { VK_NUMPAD1, "Numpad1", "Numpad 1" }, { VK_NUMPAD2, "Numpad2", "Numpad 2" },
    { VK_NUMPAD3, "Numpad3", "Numpad 3" }, { VK_NUMPAD4, "Numpad4", "Numpad 4" }, { VK_NUMPAD5, "Numpad5", "Numpad 5" },
    { VK_NUMPAD6, "Numpad6", "Numpad 6" }, { VK_NUMPAD7, "Numpad7", "Numpad 7" }, { VK_NUMPAD8, "Numpad8", "Numpad 8" },
    { VK_NUMPAD9, "Numpad9", "Numpad 9" }, { VK_ADD, "NumpadAdd", "Numpad +" }, { VK_SUBTRACT, "NumpadSubtract", "Numpad -" },
    { VK_MULTIPLY, "NumpadMultiply", "Numpad *" }, { VK_DIVIDE, "NumpadDivide", "Numpad /" },
    { VK_DECIMAL, "NumpadDecimal", "Numpad ." }, { VK_OEM_1, "Semicolon", ";" }, { VK_OEM_PLUS, "Equals", "=" },
    { VK_OEM_COMMA, "Comma", "," }, { VK_OEM_MINUS, "Minus", "-" }, { VK_OEM_PERIOD, "Period", "." },
    { VK_OEM_2, "Slash", "/" }, { VK_OEM_3, "Backquote", "`" }, { VK_OEM_4, "LBracket", "[" },
    { VK_OEM_5, "Backslash", "\\" }, { VK_OEM_6, "RBracket", "]" }, { VK_OEM_7, "Quote", "'" },
    { VK_LBUTTON, "Mouse1", "Left mouse button" }, { VK_RBUTTON, "Mouse2", "Right mouse button" },
    { VK_MBUTTON, "Mouse3", "Middle mouse button" }, { VK_XBUTTON1, "Mouse4", "Mouse button 4" },
    { VK_XBUTTON2, "Mouse5", "Mouse button 5" },
};
static char letter_names[36][2];                 /* "A".."Z", "0".."9" */

static const char* letter(int vk) {
    int i = vk >= 'A' && vk <= 'Z' ? vk - 'A' : vk >= '0' && vk <= '9' ? 26 + vk - '0' : -1;
    if (i < 0) return NULL;
    letter_names[i][0] = (char)vk;
    letter_names[i][1] = 0;
    return letter_names[i];
}

const char* input_key_ini(int vk) {
    const char* l = letter(vk);
    if (l) return l;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) if (keys[i].vk == vk) return keys[i].ini;
    return NULL;
}

const char* input_key_label(int vk) {
    const char* l = letter(vk);
    if (l) return l;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) if (keys[i].vk == vk) return keys[i].label;
    return NULL;
}

int input_key_from_name(const char* name) {
    if (!name || !*name) return 0;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) if (!_stricmp(name, keys[i].ini)) return keys[i].vk;
    static const struct { const char* name; uint8_t vk; } alias[] = {
        { "Return", VK_RETURN }, { "LControl", VK_LCONTROL }, { "RControl", VK_RCONTROL }, { "Ctrl", VK_LCONTROL },
        { "Shift", VK_LSHIFT }, { "PgUp", VK_PRIOR }, { "PgDn", VK_NEXT }, { "Del", VK_DELETE }, { "Ins", VK_INSERT },
    };
    for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++) if (!_stricmp(name, alias[i].name)) return alias[i].vk;
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {            /* raw virtual key, if bindable */
        int vk = (int)strtol(name + 2, NULL, 16);
        return vk > 0 && vk < 256 && input_key_ini(vk) ? vk : 0;
    }
    return 0;
}

/* ---- configuration -------------------------------------------------------------------------------------- */

void input_config_defaults(input_config* c) {
    memset(c, 0, sizeof *c);
    static const uint8_t k[INPUT_ACT_COUNT][2] = {
        { VK_SPACE, VK_NUMPAD2 }, { VK_LCONTROL, VK_NUMPAD6 }, { 'F', VK_NUMPAD4 }, { 'R', VK_NUMPAD8 },
        { 'Q', 0 }, { 'E', 0 }, { '1', 0 }, { '3', 0 }, { 'C', 0 }, { 'U', 0 },
        { VK_BACK, 0 }, { VK_RETURN, 0 }, { VK_UP, 0 }, { VK_DOWN, 0 }, { VK_LEFT, 0 }, { VK_RIGHT, 0 },
        { 'W', 0 }, { 'S', 0 }, { 'A', 0 }, { 'D', 0 }, { 'I', 0 }, { 'K', 0 }, { 'J', 0 }, { 'L', 0 },
    };
    memcpy(c->key, k, sizeof k);
    static const uint8_t p[INPUT_ACT_PAD_COUNT] = {
        INPUT_BTN_CROSS, INPUT_BTN_CIRCLE, INPUT_BTN_SQUARE, INPUT_BTN_TRIANGLE, INPUT_BTN_L1, INPUT_BTN_R1,
        INPUT_BTN_L2, INPUT_BTN_R2, INPUT_BTN_L3, INPUT_BTN_R3, INPUT_BTN_SELECT, INPUT_BTN_START,
        INPUT_BTN_UP, INPUT_BTN_DOWN, INPUT_BTN_LEFT, INPUT_BTN_RIGHT,
    };
    for (int a = 0; a < INPUT_ACT_PAD_COUNT; a++) { c->pad[a][0] = p[a]; c->pad[a][1] = INPUT_BTN_NONE; }
    c->pad[INPUT_ACT_SELECT][1] = INPUT_BTN_TOUCHPAD;         /* touchpad click = SELECT, as on PS4 remasters */
    c->vibration = 1;
    c->vibration_strength = 100;
    c->deadzone_left = c->deadzone_right = 12;
    c->lightbar_on = 1;
    c->lightbar[0] = 0x28; c->lightbar[1] = 0x64; c->lightbar[2] = 0xFF;   /* Sly blue */
}

static int parse_onoff(const char* v, int* out) {
    if (!_stricmp(v, "on") || !_stricmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "yes")) { *out = 1; return 1; }
    if (!_stricmp(v, "off") || !_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no")) { *out = 0; return 1; }
    return 0;
}

static int parse_range(const char* v, int lo, int hi, int* out) {
    char* end;
    long n = strtol(v, &end, 10);
    while (*end == ' ' || *end == '%') end++;
    if (end == v || *end || n < lo || n > hi) return 0;
    *out = (int)n;
    return 1;
}

/* "a, b" -> up to two names; "none" / "" = nothing. Each name goes through conv (0 / -1 = unknown). */
static int parse_list(const char* v, int (*conv)(const char*), int none, int out[INPUT_BIND_SLOTS]) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", v);
    int n = 0;
    out[0] = out[1] = none;
    for (char* t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ' || *t == '\t') t++;
        char* e = t + strlen(t);
        while (e > t && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!*t || !_stricmp(t, "none")) continue;
        int x = conv(t);
        if (x <= 0 && !(x == 0 && conv == input_button_from_name)) return 0;   /* (button 0 = cross is valid) */
        if (n >= INPUT_BIND_SLOTS) return 0;
        out[n++] = x;
    }
    return 1;
}

int input_config_apply(input_config* c, const char* key, const char* v) {
    if (!_stricmp(key, "vibration")) return parse_onoff(v, &c->vibration) ? 0 : 1;
    if (!_stricmp(key, "vibration_strength")) return parse_range(v, 0, 100, &c->vibration_strength) ? 0 : 1;
    if (!_stricmp(key, "invert_camera_x")) return parse_onoff(v, &c->invert_camera_x) ? 0 : 1;
    if (!_stricmp(key, "invert_camera_y")) return parse_onoff(v, &c->invert_camera_y) ? 0 : 1;
    if (!_stricmp(key, "deadzone_left")) return parse_range(v, 0, 50, &c->deadzone_left) ? 0 : 1;
    if (!_stricmp(key, "deadzone_right")) return parse_range(v, 0, 50, &c->deadzone_right) ? 0 : 1;
    if (!_stricmp(key, "lightbar")) return parse_onoff(v, &c->lightbar_on) ? 0 : 1;
    if (!_stricmp(key, "lightbar_color")) {
        char* end;
        unsigned long rgb = strtoul(v, &end, 16);
        if (strlen(v) != 6 || *end) return 1;
        c->lightbar[0] = (uint8_t)(rgb >> 16); c->lightbar[1] = (uint8_t)(rgb >> 8); c->lightbar[2] = (uint8_t)rgb;
        return 0;
    }
    int list[INPUT_BIND_SLOTS];
    for (int a = 0; a < INPUT_ACT_COUNT; a++) {
        char name[32];
        snprintf(name, sizeof name, "key_%s", actions[a].ini);
        if (!_stricmp(key, name)) {
            if (!parse_list(v, input_key_from_name, 0, list)) return 1;
            for (int s = 0; s < INPUT_BIND_SLOTS; s++) c->key[a][s] = (uint8_t)list[s];
            return 0;
        }
        if (a >= INPUT_ACT_PAD_COUNT) continue;
        snprintf(name, sizeof name, "pad_%s", actions[a].ini);
        if (!_stricmp(key, name)) {
            if (!parse_list(v, input_button_from_name, INPUT_BTN_NONE, list)) return 1;
            for (int s = 0; s < INPUT_BIND_SLOTS; s++) c->pad[a][s] = (uint8_t)list[s];
            return 0;
        }
    }
    return -1;
}

enum { CK_VIB, CK_VIBSTR, CK_INVX, CK_INVY, CK_DZL, CK_DZR, CK_LB, CK_LBC, CK_FIXED };
int input_config_keys(void) { return CK_FIXED + INPUT_ACT_COUNT + INPUT_ACT_PAD_COUNT; }

void input_config_key(const input_config* c, int i, char* key, size_t kcap, char* value, size_t vcap) {
    static const char* const fixed[CK_FIXED] = { "vibration", "vibration_strength", "invert_camera_x", "invert_camera_y",
                                                 "deadzone_left", "deadzone_right", "lightbar", "lightbar_color" };
    *key = *value = 0;
    if (i < CK_FIXED) {
        snprintf(key, kcap, "%s", fixed[i]);
        switch (i) {
        case CK_VIB: snprintf(value, vcap, "%s", c->vibration ? "on" : "off"); break;
        case CK_VIBSTR: snprintf(value, vcap, "%d", c->vibration_strength); break;
        case CK_INVX: snprintf(value, vcap, "%s", c->invert_camera_x ? "on" : "off"); break;
        case CK_INVY: snprintf(value, vcap, "%s", c->invert_camera_y ? "on" : "off"); break;
        case CK_DZL: snprintf(value, vcap, "%d", c->deadzone_left); break;
        case CK_DZR: snprintf(value, vcap, "%d", c->deadzone_right); break;
        case CK_LB: snprintf(value, vcap, "%s", c->lightbar_on ? "on" : "off"); break;
        default: snprintf(value, vcap, "%02X%02X%02X", c->lightbar[0], c->lightbar[1], c->lightbar[2]); break;
        }
        return;
    }
    i -= CK_FIXED;
    const char* names[INPUT_BIND_SLOTS] = { NULL, NULL };
    int n = 0;
    if (i < INPUT_ACT_COUNT) {
        snprintf(key, kcap, "key_%s", actions[i].ini);
        for (int s = 0; s < INPUT_BIND_SLOTS; s++) if (c->key[i][s] && input_key_ini(c->key[i][s])) names[n++] = input_key_ini(c->key[i][s]);
    } else {
        i -= INPUT_ACT_COUNT;
        snprintf(key, kcap, "pad_%s", actions[i].ini);
        for (int s = 0; s < INPUT_BIND_SLOTS; s++) if (c->pad[i][s] < INPUT_BTN_COUNT) names[n++] = buttons[c->pad[i][s]].ini;
    }
    if (!n) snprintf(value, vcap, "none");
    else if (n == 1) snprintf(value, vcap, "%s", names[0]);
    else snprintf(value, vcap, "%s, %s", names[0], names[1]);
}

/* ---- mapping -------------------------------------------------------------------------------------------- */

void input_pad_clear(pad_input* p) {
    memset(p, 0, sizeof *p);
    p->lx = p->ly = p->rx = p->ry = 0x80;
}

uint8_t input_axis_byte(int v) {
    int b = (int)floor(128.0 + v / 256.0);
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

int16_t input_axis_from_byte(uint8_t b) {
    int v = ((int)b - 128) * 258;
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void input_stick(int x, int y, int deadzone_percent, uint8_t* ox, uint8_t* oy) {
    double fx = x, fy = y, mag = sqrt(fx * fx + fy * fy);
    double dz = 32767.0 * (deadzone_percent < 0 ? 0 : deadzone_percent > 90 ? 90 : deadzone_percent) / 100.0;
    if (mag <= dz || mag == 0.0) { *ox = *oy = 0x80; return; }
    double norm = (mag - dz) / (32767.0 - dz);
    if (norm > 1.0) norm = 1.0;
    double k = norm * 32768.0 / mag;
    *ox = input_axis_byte((int)lround(fx * k));
    *oy = input_axis_byte((int)lround(fy * k));
}

/* Host button held (L2/R2 also from the analog trigger) and its pressure (0..255). */
static int host_pressed(const input_state* s, int btn, uint8_t* pressure) {
    *pressure = 0;
    if (btn < 0 || btn >= INPUT_BTN_COUNT) return 0;
    if (btn == INPUT_BTN_L2 || btn == INPUT_BTN_R2) {
        uint8_t a = btn == INPUT_BTN_L2 ? s->l2 : s->r2;
        int down = (s->buttons >> btn & 1) || a > INPUT_TRIGGER_THRESHOLD;
        if (down) *pressure = a ? a : 0xFF;
        return down;
    }
    if (!(s->buttons >> btn & 1)) return 0;
    *pressure = 0xFF;
    return 1;
}

void input_state_to_pad(const input_state* s, const input_config* c, pad_input* p) {
    for (int a = 0; a < INPUT_ACT_PAD_COUNT; a++)
        for (int k = 0; k < INPUT_BIND_SLOTS; k++) {
            uint8_t pr;
            if (!host_pressed(s, c->pad[a][k], &pr)) continue;
            p->buttons |= act_pad[a].bit;
            if (act_pad[a].pressure >= 0 && pr > p->pressure[act_pad[a].pressure]) p->pressure[act_pad[a].pressure] = pr;
        }
    input_stick(s->lx, s->ly, c->deadzone_left, &p->lx, &p->ly);
    input_stick(s->rx, s->ry, c->deadzone_right, &p->rx, &p->ry);
}

void input_keys_to_pad(const uint8_t down[256], const input_config* c, pad_input* p) {
    for (int a = 0; a < INPUT_ACT_PAD_COUNT; a++)
        for (int k = 0; k < INPUT_BIND_SLOTS; k++)
            if (c->key[a][k] && down[c->key[a][k]]) {
                p->buttons |= act_pad[a].bit;
                if (act_pad[a].pressure >= 0) p->pressure[act_pad[a].pressure] = 0xFF;
            }
    uint8_t* axes[4] = { &p->ly, &p->lx, &p->ry, &p->rx };      /* up/down, left/right per stick */
    for (int ax = 0; ax < 4; ax++) {
        int neg_act = INPUT_ACT_LSTICK_UP + ax * 2, pos_act = neg_act + 1;
        int neg = 0, pos = 0;
        for (int k = 0; k < INPUT_BIND_SLOTS; k++) {
            if (c->key[neg_act][k] && down[c->key[neg_act][k]]) neg = 1;
            if (c->key[pos_act][k] && down[c->key[pos_act][k]]) pos = 1;
        }
        if (neg != pos) *axes[ax] = neg ? 0x00 : 0xFF;
    }
}

static uint8_t invert_byte(uint8_t v) { return (uint8_t)(v == 0 ? 0xFF : 256 - v); }   /* 0x80 stays */

void input_invert_camera(pad_input* p, const input_config* c) {
    if (c->invert_camera_x) p->rx = invert_byte(p->rx);
    if (c->invert_camera_y) p->ry = invert_byte(p->ry);
}

void input_xinput_to_state(uint16_t b, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                           input_state* s) {
    static const struct { uint16_t xi; uint8_t btn; } map[] = {
        { XINPUT_GAMEPAD_DPAD_UP, INPUT_BTN_UP }, { XINPUT_GAMEPAD_DPAD_DOWN, INPUT_BTN_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, INPUT_BTN_LEFT }, { XINPUT_GAMEPAD_DPAD_RIGHT, INPUT_BTN_RIGHT },
        { XINPUT_GAMEPAD_START, INPUT_BTN_START }, { XINPUT_GAMEPAD_BACK, INPUT_BTN_SELECT },
        { XINPUT_GAMEPAD_LEFT_THUMB, INPUT_BTN_L3 }, { XINPUT_GAMEPAD_RIGHT_THUMB, INPUT_BTN_R3 },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, INPUT_BTN_L1 }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, INPUT_BTN_R1 },
        { XINPUT_GAMEPAD_A, INPUT_BTN_CROSS }, { XINPUT_GAMEPAD_B, INPUT_BTN_CIRCLE },
        { XINPUT_GAMEPAD_X, INPUT_BTN_SQUARE }, { XINPUT_GAMEPAD_Y, INPUT_BTN_TRIANGLE },
    };
    memset(s, 0, sizeof *s);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (b & map[i].xi) s->buttons |= 1u << map[i].btn;
    s->l2 = lt;
    s->r2 = rt;
    if (lt > INPUT_TRIGGER_THRESHOLD) s->buttons |= 1u << INPUT_BTN_L2;
    if (rt > INPUT_TRIGGER_THRESHOLD) s->buttons |= 1u << INPUT_BTN_R2;
    s->lx = lx;
    s->ly = (int16_t)(ly == -32768 ? 32767 : -ly);            /* XInput: +y up */
    s->rx = rx;
    s->ry = (int16_t)(ry == -32768 ? 32767 : -ry);
}

void input_generic_to_state(uint32_t b, int hat, const int axes[6], input_state* s) {
    /* DirectInput layout most pads use (a DualShock 4's own): 1 Square, 2 Cross, 3 Circle, 4 Triangle,
     * 5 L1, 6 R1, 7 L2, 8 R2, 9 Select, 10 Start, 11 L3, 12 R3, 13 Home, 14 Touchpad */
    static const uint8_t order[14] = { INPUT_BTN_SQUARE, INPUT_BTN_CROSS, INPUT_BTN_CIRCLE, INPUT_BTN_TRIANGLE,
                                       INPUT_BTN_L1, INPUT_BTN_R1, INPUT_BTN_L2, INPUT_BTN_R2, INPUT_BTN_SELECT,
                                       INPUT_BTN_START, INPUT_BTN_L3, INPUT_BTN_R3, INPUT_BTN_HOME, INPUT_BTN_TOUCHPAD };
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 14; i++) if (b >> i & 1) s->buttons |= 1u << order[i];
    static const uint8_t hatbits[8] = { 1, 1 | 8, 8, 8 | 2, 2, 2 | 4, 4, 4 | 1 };
    if (hat >= 0 && hat < 8) {
        if (hatbits[hat] & 1) s->buttons |= 1u << INPUT_BTN_UP;
        if (hatbits[hat] & 2) s->buttons |= 1u << INPUT_BTN_DOWN;
        if (hatbits[hat] & 4) s->buttons |= 1u << INPUT_BTN_LEFT;
        if (hatbits[hat] & 8) s->buttons |= 1u << INPUT_BTN_RIGHT;
    }
#define AX(i) (axes[i] == INPUT_AXIS_ABSENT ? 0 : axes[i])
    s->lx = (int16_t)AX(0);
    s->ly = (int16_t)AX(1);
    if (axes[2] != INPUT_AXIS_ABSENT && axes[5] != INPUT_AXIS_ABSENT) {   /* right stick on Z / Rz */
        s->rx = (int16_t)axes[2];
        s->ry = (int16_t)axes[5];
        if (axes[3] != INPUT_AXIS_ABSENT) s->l2 = (uint8_t)((axes[3] + 32768) >> 8);   /* triggers on Rx / Ry */
        if (axes[4] != INPUT_AXIS_ABSENT) s->r2 = (uint8_t)((axes[4] + 32768) >> 8);
    } else {
        s->rx = (int16_t)AX(3);
        s->ry = (int16_t)AX(4);
    }
#undef AX
}

int input_state_active(const input_state* s) {
    const int half = 16384;
    return s->buttons != 0 || abs(s->lx) > half || abs(s->ly) > half || abs(s->rx) > half || abs(s->ry) > half ||
           s->l2 > 128 || s->r2 > 128;
}

/* ---- menu navigation ------------------------------------------------------------------------------------ */

#define NAV_BIT(n) (1u << (n))
#define NAV_DIRS (NAV_BIT(INPUT_NAV_UP) | NAV_BIT(INPUT_NAV_DOWN) | NAV_BIT(INPUT_NAV_LEFT) | NAV_BIT(INPUT_NAV_RIGHT))

uint32_t input_state_nav_bits(const input_state* s) {
    const int half = 16384;
    uint32_t n = 0, b = s->buttons;
    if ((b >> INPUT_BTN_UP & 1) || s->ly < -half) n |= NAV_BIT(INPUT_NAV_UP);
    if ((b >> INPUT_BTN_DOWN & 1) || s->ly > half) n |= NAV_BIT(INPUT_NAV_DOWN);
    if ((b >> INPUT_BTN_LEFT & 1) || s->lx < -half) n |= NAV_BIT(INPUT_NAV_LEFT);
    if ((b >> INPUT_BTN_RIGHT & 1) || s->lx > half) n |= NAV_BIT(INPUT_NAV_RIGHT);
    if (b >> INPUT_BTN_CROSS & 1) n |= NAV_BIT(INPUT_NAV_CONFIRM);
    if (b >> INPUT_BTN_CIRCLE & 1) n |= NAV_BIT(INPUT_NAV_BACK);
    if (b >> INPUT_BTN_R1 & 1) n |= NAV_BIT(INPUT_NAV_TAB_NEXT);
    if (b >> INPUT_BTN_L1 & 1) n |= NAV_BIT(INPUT_NAV_TAB_PREV);
    if ((b >> INPUT_BTN_START & 1) || (b >> INPUT_BTN_HOME & 1)) n |= NAV_BIT(INPUT_NAV_MENU);
    return n;
}

int input_nav_step(input_nav_tracker* t, uint32_t now_bits, uint32_t pressed, uint64_t now_ms, int* navs, int* repeats,
                   int max) {
    int n = 0;
    for (int nav = 1; nav < 16; nav++) {
        uint32_t bit = NAV_BIT(nav);
        int held = (now_bits & bit) != 0, was = (t->held & bit) != 0;
        if ((held && !was) || ((pressed & bit) && !was) || ((pressed & bit) && !held)) {
            if (n < max) { navs[n] = nav; repeats[n] = 0; n++; }
            t->next[nav] = now_ms + INPUT_NAV_DELAY_MS;
        } else if (held && was && (bit & NAV_DIRS) && now_ms >= t->next[nav]) {
            if (n < max) { navs[n] = nav; repeats[n] = 1; n++; }
            t->next[nav] = now_ms + INPUT_NAV_REPEAT_MS;
        }
    }
    t->held = now_bits;
    return n;
}

/* ---- ports ---------------------------------------------------------------------------------------------- */

uint32_t input_assign_ports(input_port_dev* d, int n, uint64_t now_ms) {
    uint32_t changed = 0;
    int owner[2] = { -1, -1 };
    for (int i = 0; i < n; i++) {
        if (d[i].active) d[i].last_active = now_ms;
        if (d[i].port == 0 || d[i].port == 1) {
            if (owner[d[i].port] >= 0) d[i].port = -1;              /* (duplicate: keep the first) */
            else owner[d[i].port] = i;
        } else {
            d[i].port = -1;
        }
    }
    for (int i = 0; i < n; i++) {
        if (!d[i].active || d[i].port == 0) continue;
        int o = owner[0];
        if (o < 0 || now_ms - d[o].last_active >= INPUT_TAKEOVER_MS) {
            int old = d[i].port;
            if (old == 1) owner[1] = -1;
            if (o >= 0) {                                      /* the idle owner moves to the freed port */
                d[o].port = old;
                if (old == 1) owner[1] = o;
                changed |= 1u << o;
            }
            d[i].port = 0;
            owner[0] = i;
            changed |= 1u << i;
        } else if (d[i].port < 0 && owner[1] < 0) {
            d[i].port = 1;
            owner[1] = i;
            changed |= 1u << i;
        }
    }
    return changed;
}
