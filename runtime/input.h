/* PC input: controllers (DualSense, DualShock 4, XInput, generic HID pads), keyboard and mouse.
 *
 * Everything here uses only Windows' own DLLs, loaded at run time (hid.dll, setupapi.dll,
 * cfgmgr32.dll, xinput1_4.dll): no driver, Steam Input or DS4Windows is needed, and a PC without a
 * controller just plays with the keyboard. Mapping, ini keys and the support matrix:
 * notes/controls.md; the interface for the Options menu: notes/pc-experience.md ("Interfaces: input").
 *
 * Threads: the game (pad.c, on the emulation thread) calls input_get_pad / input_set_rumble once per
 * VBlank; the window thread forwards keyboard and mouse messages (input_window_message); the input
 * thread (input_hid.c) reads HID controllers and handles hot-plug. Every function below may be
 * called from any thread. */
#pragma once
#include <stdint.h>

#include "hw.h"                                  /* pad_input, PAD_* */

/* ---- life cycle ------------------------------------------------------------------------------------ */
void input_init(void);                           /* idempotent; starts the input thread; input_get_pad calls it */
void input_shutdown(void);                       /* motors off (light bar stays); before exiting */
/* `sly2.exe --pad-test [--seconds N] [--print]` (input_padtest.c): argv[0] = "--pad-test". Returns the exit code. */
int input_pad_test(int argc, char** argv);
void input_set_window(void* hwnd);               /* the game window: keyboard only while it has focus */
/* Window thread: every message of the game window (keys, mouse wheel and buttons for the menu, capture).
 * Returns 1 if the message was consumed (the window must not act on it), else 0. */
int input_window_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp);

/* ---- the game ---------------------------------------------------------------------------------------- */
void input_get_pad(int port, pad_input* out);    /* controller of the port + keyboard (port 0); neutral while the menu is open */
void input_set_rumble(int port, uint8_t small_on, uint8_t large_speed);   /* DualShock 2 motors, every VBlank */

/* ---- controllers --------------------------------------------------------------------------------------- */
enum { INPUT_DEV_NONE, INPUT_DEV_DUALSENSE, INPUT_DEV_DUALSENSE_EDGE, INPUT_DEV_DS4, INPUT_DEV_XINPUT,
       INPUT_DEV_GENERIC };
enum { INPUT_BUS_USB, INPUT_BUS_BT, INPUT_BUS_OTHER };

/* Host controller buttons (bits of input_state.buttons), named after a PlayStation controller;
 * Xbox: CROSS = A, CIRCLE = B, SQUARE = X, TRIANGLE = Y, L1/R1 = LB/RB, L2/R2 = LT/RT, L3/R3 = stick
 * clicks, SELECT = Back/View, START = Start/Menu. DualSense: SELECT = Create, START = Options; DS4:
 * SELECT = Share. HOME = PS button (Xbox Guide is not readable through XInput). */
enum {
    INPUT_BTN_CROSS, INPUT_BTN_CIRCLE, INPUT_BTN_SQUARE, INPUT_BTN_TRIANGLE,
    INPUT_BTN_L1, INPUT_BTN_R1, INPUT_BTN_L2, INPUT_BTN_R2, INPUT_BTN_L3, INPUT_BTN_R3,
    INPUT_BTN_SELECT, INPUT_BTN_START, INPUT_BTN_HOME, INPUT_BTN_TOUCHPAD, INPUT_BTN_MUTE,
    INPUT_BTN_UP, INPUT_BTN_DOWN, INPUT_BTN_LEFT, INPUT_BTN_RIGHT,
    INPUT_BTN_COUNT
};
#define INPUT_BTN_NONE 0xFF

/* A controller's state, whatever its kind. Sticks -32768..32767, +x right, +y DOWN (as a PS2 pad),
 * before dead zones. Triggers 0..255. */
typedef struct {
    uint32_t buttons;                            /* 1 << INPUT_BTN_* */
    int16_t lx, ly, rx, ry;
    uint8_t l2, r2;
} input_state;

#define INPUT_MAX_DEVICES 16
typedef struct {
    int id;                                      /* unique for this run (never reused); 0 = none */
    int kind, bus;                               /* INPUT_DEV_*, INPUT_BUS_* */
    int port;                                    /* PS2 port it drives (0 or 1), -1 none */
    uint16_t vid, pid;
    char name[64];                               /* "DualSense (Bluetooth)", "Xbox controller 1", ... */
    input_state state;
    int battery;                                 /* percent, -1 unknown */
    int charging;
    uint32_t reports;                            /* input reports received (HID) / polls (XInput) */
    uint32_t crc_errors;                         /* Bluetooth input reports with a bad CRC (still used) */
    int enhanced;                                /* PlayStation over Bluetooth: full reports (0x31 / 0x11) arrive */
    int can_rumble;
    uint8_t motor_small, motor_large;            /* what is being sent now (0..255) */
} input_device_info;

int input_devices(input_device_info* out, int max);   /* connected controllers, in connection order */
/* Rumble test (pad test, menu "test vibration"): drive a controller's motors for ms milliseconds,
 * ignoring the game and the vibration setting (strength still applies unless raw). */
void input_test_rumble(int device_id, uint8_t small_motor, uint8_t large_motor, int ms);

/* Connect / disconnect notices (for an on-screen message; the text stream may use them). */
typedef struct { int connected; int device_id; int port; char name[64]; } input_notice;
int input_poll_notice(input_notice* out);       /* 1 and the oldest unread notice, else 0 */

/* ---- the Options menu --------------------------------------------------------------------------------- */
enum { INPUT_NAV_NONE, INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_NAV_LEFT, INPUT_NAV_RIGHT, INPUT_NAV_CONFIRM,
       INPUT_NAV_BACK, INPUT_NAV_TAB_NEXT, INPUT_NAV_TAB_PREV, INPUT_NAV_MENU, INPUT_NAV_CLICK,
       INPUT_NAV_POINTER, INPUT_NAV_RELEASE, INPUT_NAV_WHEEL,
       INPUT_NAV_APPLY, INPUT_NAV_DISCARD };     /* (menu) Square / X or F: apply the changes; Triangle / Y or R: discard them */
enum { INPUT_SRC_KEYBOARD, INPUT_SRC_MOUSE, INPUT_SRC_CONTROLLER };
typedef struct {
    int nav;                                     /* INPUT_NAV_* */
    int source;                                  /* INPUT_SRC_* */
    int device_id;                               /* controller id (INPUT_SRC_CONTROLLER) */
    int repeat;                                  /* 1: auto-repeat of a held direction / key */
    int x, y;                                    /* INPUT_NAV_CLICK / POINTER / RELEASE / WHEEL: client pixels */
    int delta;                                   /* (pc-menu2) INPUT_NAV_WHEEL: notches, + up (fractions of a notch add up) */
} input_nav_event;
/* Navigation events from every device, oldest first; 1 if one was returned. Directions repeat while
 * held (400 ms, then every 110 ms). Controllers: d-pad / left stick, Cross confirm, Circle back,
 * L1 / R1 tabs, Options/Start and PS -> MENU. Keyboard: arrows / WASD, Enter / Space confirm,
 * Esc / Backspace back, Tab / Shift+Tab and Q / E / PageUp / PageDown tabs. Mouse: WHEEL (x, y, delta),
 * left click CLICK (x, y) and RELEASE, right click BACK, moves POINTER. Esc and the PS button also report MENU
 * while the menu is closed (so the menu can open on them). Events also accumulate while the menu
 * is closed: call input_nav_flush when opening. */
int input_nav_poll(input_nav_event* out);
void input_nav_flush(void);
/* While on: the game sees a released controller and no keys, rumble stops; buttons still held when
 * it goes off stay hidden from the game until released. */
void input_set_menu_active(int on);
int input_menu_active(void);

/* Remapping: capture the next key (keyboard) or button (any controller). While capturing, no nav
 * events are produced. Esc (keyboard) or the PS button cancels. */
enum { INPUT_CAPTURE_KEYBOARD = 1, INPUT_CAPTURE_CONTROLLER = 2 };
enum { INPUT_CAPTURE_PENDING, INPUT_CAPTURE_KEY, INPUT_CAPTURE_BUTTON, INPUT_CAPTURE_CANCELLED, INPUT_CAPTURE_IDLE };
void input_capture_begin(int what);
int input_capture_poll(int* code);               /* INPUT_CAPTURE_*; code = virtual key or INPUT_BTN_* */
void input_capture_cancel(void);

/* ---- bindings and options ([controls] in sly2.ini) ---------------------------------------------------- */
/* Game actions: the 16 DualShock 2 buttons, then the 8 stick directions (keyboard only). */
enum {
    INPUT_ACT_CROSS, INPUT_ACT_CIRCLE, INPUT_ACT_SQUARE, INPUT_ACT_TRIANGLE,
    INPUT_ACT_L1, INPUT_ACT_R1, INPUT_ACT_L2, INPUT_ACT_R2, INPUT_ACT_L3, INPUT_ACT_R3,
    INPUT_ACT_SELECT, INPUT_ACT_START, INPUT_ACT_UP, INPUT_ACT_DOWN, INPUT_ACT_LEFT, INPUT_ACT_RIGHT,
    INPUT_ACT_PAD_COUNT,                         /* (16) */
    INPUT_ACT_LSTICK_UP = INPUT_ACT_PAD_COUNT, INPUT_ACT_LSTICK_DOWN, INPUT_ACT_LSTICK_LEFT, INPUT_ACT_LSTICK_RIGHT,
    INPUT_ACT_RSTICK_UP, INPUT_ACT_RSTICK_DOWN, INPUT_ACT_RSTICK_LEFT, INPUT_ACT_RSTICK_RIGHT,
    INPUT_ACT_COUNT                              /* (24) */
};
#define INPUT_BIND_SLOTS 2                       /* two keys / two controller buttons per action */

typedef struct {
    uint8_t key[INPUT_ACT_COUNT][INPUT_BIND_SLOTS];        /* virtual keys, 0 = none */
    uint8_t pad[INPUT_ACT_PAD_COUNT][INPUT_BIND_SLOTS];    /* INPUT_BTN_*, INPUT_BTN_NONE = none */
    int vibration;                               /* 1 on (default) */
    int vibration_strength;                      /* 0..100 % (default 100) */
    int invert_camera_x, invert_camera_y;        /* right stick axes (default off) */
    int deadzone_left, deadzone_right;           /* 0..50 % of the stick's travel (default 12) */
    int lightbar_on;                             /* DualSense / DS4 light bar (default on) */
    uint8_t lightbar[3];                         /* RGB (default Sly blue 0x28 0x64 0xFF) */
} input_config;

void input_config_defaults(input_config* c);
void input_config_get(input_config* c);         /* the current configuration */
/* Apply c from now on; save != 0 also writes every [controls] key to sly2.ini. Returns 1 on success. */
int input_config_set(const input_config* c, int save);
void input_config_reload(void);                  /* re-read [controls] from sly2.ini */

/* Names (static strings): action labels ("Cross", "Left stick up"), ini key suffixes ("cross",
 * "lstick_up"), key names ("Space", "Left Ctrl", "Numpad 2"; NULL if the key cannot be bound) and
 * controller button names in a style: 0 PlayStation ("Cross", "Create"), 1 Xbox ("A", "View"). */
const char* input_action_label(int act);
const char* input_action_ini(int act);
const char* input_key_label(int vk);
const char* input_button_label(int btn, int xbox_style);
