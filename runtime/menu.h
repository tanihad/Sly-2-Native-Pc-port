/* The in-game Options menu ("PC Settings"; notes/pc-menu.md, notes/settings.md).
 *
 * Reached from the game's own pause menu (an entry "PC Settings" added by instruction patches, menu_game.c), with
 * Esc on the keyboard or Select+Start on a controller. It is an overlay: the game stays in its pause menu
 * (paused, still presenting pictures), the menu is drawn over each presented picture by the renderer in use
 * (menu_overlay.c: the GPU renderer's present pass, the software renderer's window presenter, GDI), and the game
 * gets no pad input while it is open. Closing it returns to the game's pause menu.
 *
 * This file is the model: pages, rows, values (through the settings table, settings.h, and a host interface for
 * what only the window / renderer / audio / input know), navigation, the layout every renderer and the mouse share.
 * It has no Windows dependencies, so tests/unit/menu drives it without a window. Thread-safe: every public
 * function takes the menu's lock (the window thread sends keys and the mouse, the EE thread pad navigation, the
 * render thread draws). */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { MENU_PAGE_GRAPHICS, MENU_PAGE_DISPLAY, MENU_PAGE_AUDIO, MENU_PAGE_CONTROLS, MENU_PAGE_N,
       MENU_PAGE_KEYS = MENU_PAGE_N,     /* sub-pages of Controls: keyboard / controller bindings */
       MENU_PAGE_PAD, MENU_PAGE_ALL };

/* navigation events (keyboard, controller, scripted tokens) */
typedef enum {
    MENU_NAV_NONE, MENU_NAV_UP, MENU_NAV_DOWN, MENU_NAV_LEFT, MENU_NAV_RIGHT, MENU_NAV_CONFIRM, MENU_NAV_BACK,
    MENU_NAV_TAB_NEXT, MENU_NAV_TAB_PREV, MENU_NAV_DEFAULTS, MENU_NAV_PAGE_UP, MENU_NAV_PAGE_DOWN, MENU_NAV_CLOSE,
    MENU_NAV_N
} menu_nav;

/* the device the player used last (button hints) */
enum { MENU_DEV_KEYBOARD, MENU_DEV_PLAYSTATION, MENU_DEV_XBOX };

enum { MENU_ITEM_CHOICE, MENU_ITEM_SLIDER, MENU_ITEM_ACTION, MENU_ITEM_BIND, MENU_ITEM_INFO };

#define MENU_MAX_ROWS 40
#define MENU_MAX_CHOICES 24
#define MENU_TEXT 96

/* one row as the menu shows it (built by menu_rows) */
typedef struct {
    int id;                              /* MENU_ROW_* */
    int kind;                            /* MENU_ITEM_* */
    char label[MENU_TEXT];
    char value[MENU_TEXT];               /* the current value's text ("3x - 2048 x 1344", "75") */
    char value2[MENU_TEXT];              /* BIND: the second key / button ("-" none) */
    char desc[4 * MENU_TEXT];            /* the description for the selected row (up to three lines) */
    char note[MENU_TEXT];                /* "Applies at the next start", "GPU renderer only", ... ("" none) */
    int enabled;                         /* 0: shown dimmed, cannot change (note says why) */
    int nchoices, cur;                   /* CHOICE: count and current index (-1 none of them) */
    int lo, hi, step, val;               /* SLIDER */
    int can_left, can_right;             /* arrows shown */
} menu_row;

/* What the menu needs from the rest of the game (menu_host.c in the game; a fake in the unit test). Any member may
 * be NULL (a sensible default is used). */
typedef struct {
    int  (*gpu_active)(void);            /* the GPU renderer draws now */
    int  (*gpu_started)(void);           /* the GPU renderer was started (a live switch is possible) */
    int  (*auto_scale_now)(void);        /* the scale internal_scale = auto runs at now */
    void (*output_size)(int* w, int* h); /* the window's client size (internal_scale native) */
    void (*display_size)(int* w, int* h, int* hz);   /* the monitor the window is on */
    int  (*monitor_count)(void);
    void (*monitor_info)(int k, int* w, int* h, int* hz, int* primary);   /* k = 0..count-1 */
    int  (*window_mode)(void);           /* SETTINGS_FULLSCREEN_* now */
    void (*set_window_mode)(int mode);
    void (*set_client_size)(int w, int h);
    void (*set_monitor)(int k);          /* 0 = keep, 1..n */
    int  (*in_game)(void);               /* the game's audio settings can be reached now (music / effects) */
    void (*request_open)(void);          /* Esc / PS button with the menu closed (menu_game.c: pause the game first) */
    void (*tick)(void);                  /* every menu_tick (menu_game.c: a pending open) */
    void (*closed)(void);                /* the menu closed (menu_game.c: back to the pause menu) */
} menu_host;
void menu_set_host(const menu_host* h);

/* ---- state ---- */
void menu_open(void);
void menu_close(void);
int  menu_is_open(void);
/* a navigation event from a device (dev: MENU_DEV_*, the button hints follow it); 1 if the menu used it */
int  menu_nav_event(menu_nav nav, int dev);
/* The window thread, every turn of its loop (~60 times a second while the menu is open): the navigation events of
 * every device (runtime/input.c input_nav_poll: keyboard, mouse, controllers; Esc / the PS button open the menu
 * through host.request_open) and a binding being captured (input_capture_poll). */
void menu_tick(void);
/* a binding capture finished (INPUT_CAPTURE_KEY / BUTTON / CANCELLED, the key or button) - menu_tick, tests */
void menu_capture_result(int r, int code);
/* mouse, client pixels: move (hover), button down (1 left, 2 right), wheel (+1 up / -1 down) */
void menu_mouse_move(int x, int y);
void menu_mouse_button(int x, int y, int button);
void menu_mouse_wheel(int x, int y, int delta);
/* the device of the last input (hints) */
int  menu_device(void);
/* Changes since the last call (anything that changes the picture: state, settings_serial, the size): the
 * overlay redraws when this differs from what it drew. */
uint32_t menu_version(void);
/* binding capture: 1 while the menu waits for a key / button for a binding */
int  menu_capturing(void);

/* ---- what to draw (menu_draw.c) ---- */
typedef struct {
    int open;
    int page;                            /* MENU_PAGE_* (KEYS / PAD: sub-pages of Controls) */
    int sel;                             /* selected row */
    int scroll;                          /* first visible row */
    int nrows;
    menu_row rows[MENU_MAX_ROWS];
    int capture;                         /* waiting for a key/button (sel row) */
    int bind_slot;                       /* binding pages: the selected column (0 / 1) */
    int confirm_reset;                   /* "Reset <page> to defaults?" shown */
    int device;
    int hover_tab, hover_row, hover_btn; /* mouse (-1 none) */
    char title[MENU_TEXT];               /* "PC Settings" */
    char page_title[MENU_TEXT];          /* the sub-page's title */
} menu_view;
void menu_get_view(menu_view* v, int client_w, int client_h);
const char* menu_page_name(int page);

/* ---- layout (shared by drawing and the mouse) ---- */
typedef struct { int x, y, w, h; } menu_rect;
typedef struct {
    float u;                             /* the unit: 1 at a 1080-pixel-high output */
    menu_rect panel, header, tabs[MENU_PAGE_N], list, desc, footer;
    int row_h, visible;                  /* row height, rows that fit */
    menu_rect rows[MENU_MAX_ROWS];       /* visible rows (index = row - scroll) */
    menu_rect left[MENU_MAX_ROWS], right[MENU_MAX_ROWS];   /* the value's arrows */
    menu_rect btn[3];                    /* footer buttons, right to left: 0 back (close on a main page), 1 close
                                            (controller hint; 2 unused) */
    menu_rect tab_hint[2];               /* the page-switch hints left and right of the tabs (L1 / R1, Q / E) */
    menu_rect dlg, dlg_yes, dlg_no;      /* the "reset to defaults?" dialog and its buttons */
    menu_rect scrollbar;                 /* right of the rows (when they do not all fit) */
    int value_x;                         /* where the values column starts */
} menu_layout;
void menu_compute_layout(int client_w, int client_h, int nrows, int scroll, menu_layout* L);

/* ---- choices of a row (tests, the drawing of a value list) ---- */
int  menu_row_choice_text(int row_id, int k, char* buf, size_t cap);

/* row ids */
enum {
    MENU_ROW_PRESET, MENU_ROW_RENDERER, MENU_ROW_SCALE, MENU_ROW_AUTO_MAX, MENU_ROW_SCALER, MENU_ROW_OUTLINES,
    MENU_ROW_LOOK, MENU_ROW_BLUR, MENU_ROW_FLICKER, MENU_ROW_INTERLACE,
    MENU_ROW_MODE, MENU_ROW_WINSIZE, MENU_ROW_MONITOR, MENU_ROW_ASPECT, MENU_ROW_HUD, MENU_ROW_PACING, MENU_ROW_VSYNC, MENU_ROW_FPS,
    MENU_ROW_MASTER, MENU_ROW_MUSIC, MENU_ROW_EFFECTS, MENU_ROW_BACKGROUND,
    MENU_ROW_VIBRATION, MENU_ROW_VIB_STRENGTH, MENU_ROW_INVERT_X, MENU_ROW_INVERT_Y, MENU_ROW_DEADZONE,
    MENU_ROW_DEADZONE_R, MENU_ROW_LIGHTBAR,
    MENU_ROW_KEYS, MENU_ROW_PAD,
    MENU_ROW_RESET,                      /* "Reset to defaults" (every page) */
    MENU_ROW_BIND0 = 100,                /* binding rows: MENU_ROW_BIND0 + action */
    MENU_ROW_BIND_RESET = 99,
};
