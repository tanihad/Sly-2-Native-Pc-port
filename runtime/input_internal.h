/* Internals of the input code shared between input*.c and the unit tests (tests/unit/input).
 * Everything declared here except the device table is pure: no I/O, no Windows calls. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "input.h"

/* ---- input_ps.c: PlayStation controllers (protocol: notes/controls.md "DualSense / DualShock 4") -- */
#define PS_VID              0x054C
#define PS_PID_DS4_V1       0x05C4
#define PS_PID_DS4_V2       0x09CC
#define PS_PID_DS4_DONGLE   0x0BA0
#define PS_PID_DUALSENSE    0x0CE6
#define PS_PID_DUALSENSE_EDGE 0x0DF2
int ps_kind(uint16_t vid, uint16_t pid);         /* INPUT_DEV_DUALSENSE / _EDGE / DS4, or INPUT_DEV_NONE */

/* CRC-32 (IEEE 802.3, as zlib's crc32): crc = input_crc32(0, ...) then chain. */
uint32_t input_crc32(uint32_t crc, const void* data, size_t n);
/* Bluetooth CRC of a report: seed byte (0xA1 input, 0xA2 output, 0xA3 feature) + report[0..n-4). */
uint32_t ps_bt_crc(uint8_t seed, const uint8_t* report, size_t n);

typedef struct {
    input_state st;
    int battery, charging;                       /* percent (-1 unknown), 0/1 */
    int full;                                    /* a full report (USB 0x01, BT 0x31 / 0x11), not the BT simple one */
    int crc_ok;                                  /* BT full reports: the CRC matched (1), USB / simple: 1 */
} ps_report;
/* Parse one input report as ReadFile returned it (byte 0 = report id). bt: the device is connected
 * over Bluetooth (report 0x01 is then the short "simple" report). 1 if it was a state report. */
int ps_parse_dualsense(const uint8_t* r, size_t n, int bt, ps_report* out);
int ps_parse_ds4(const uint8_t* r, size_t n, int bt, ps_report* out);

typedef struct {
    uint8_t motor_small, motor_large;            /* high-frequency (right) / low-frequency (left) motor, 0..255 */
    int lightbar_on;
    uint8_t rgb[3];
    uint8_t player_leds;                         /* DualSense: 5 LED bits (0x04 = player 1, the middle one) */
    int release_leds;                            /* DualSense: hand the light bar over from the system's (first report) */
    int vibration_v2;                            /* DualSense firmware >= 2.24: "improved" rumble flag */
    uint8_t seq;                                 /* DualSense BT: 4-bit sequence number (caller increments) */
} ps_output;
#define PS_OUTPUT_MAX 78
/* Build an output report into buf (PS_OUTPUT_MAX bytes, zeroed first); returns its length.
 * DualSense: USB 0x02 (48 bytes) / BT 0x31 (78, CRC). DS4: USB 0x05 (32) / BT 0x11 (78, CRC). */
size_t ps_build_dualsense(const ps_output* o, int bt, uint8_t* buf);
size_t ps_build_ds4(const ps_output* o, int bt, uint8_t* buf);
/* DualSense firmware feature report 0x20: the "update version" (bytes 44-45), 0 if too short. */
unsigned ps_dualsense_fw_version(const uint8_t* feat, size_t n);
#define PS_DS_FW_VIBRATION_V2 0x0224

/* ---- input_map.c: mapping, options, names ------------------------------------------------------------- */
uint8_t input_axis_byte(int v);                  /* -32768..32767 -> 0..255 (0 -> 0x80) */
int16_t input_axis_from_byte(uint8_t b);         /* 0..255 (0x80 centre) -> -32768..32767 (0x80 -> 0) */
/* Radial dead zone (percent of full travel) with rescaling: drift reads centred, full tilt still
 * reaches the end. */
void input_stick(int x, int y, int deadzone_percent, uint8_t* ox, uint8_t* oy);
#define INPUT_TRIGGER_THRESHOLD 30               /* analog trigger -> digital L2/R2 (XInput's threshold) */
/* Merge a controller state into p (buttons OR'd, sticks and trigger pressures overwritten). */
void input_state_to_pad(const input_state* s, const input_config* c, pad_input* p);
/* Merge keys (down[vk] != 0 = held) into p: a stick axis driven by keys overrides; opposite keys cancel. */
void input_keys_to_pad(const uint8_t down[256], const input_config* c, pad_input* p);
void input_pad_clear(pad_input* p);
/* Invert the right stick (the camera) as the options say; 0x80 stays centred. */
void input_invert_camera(pad_input* p, const input_config* c);
/* XInput buttons/axes -> input_state (wButtons, triggers, thumbs as XINPUT_GAMEPAD has them). */
void input_xinput_to_state(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx,
                           int16_t ry, input_state* s);
/* Generic HID pad: buttons 1..32 (bit i = button i+1), hat 0..7 (N, NE, ... clockwise) or -1, and
 * axes already normalised to -32768..32767 (x, y, z, rx, ry, rz; INT16_MIN-1 = absent). */
#define INPUT_AXIS_ABSENT (-40000)
void input_generic_to_state(uint32_t buttons, int hat, const int axes[6], input_state* s);
/* Something is being pressed / pushed (button, stick past half, trigger past half). */
int input_state_active(const input_state* s);

/* sly2.ini [controls]: key (without the section) = value. Returns 0 ok, 1 bad value, -1 unknown key. */
int input_config_apply(input_config* c, const char* key, const char* value);
/* The i-th key of the section and its value for c (0 <= i < input_config_keys()). */
int input_config_keys(void);
void input_config_key(const input_config* c, int i, char* key, size_t kcap, char* value, size_t vcap);
int input_key_from_name(const char* name);       /* "Space", "LCtrl", "A", "Numpad2", "0x41" -> vk; 0 unknown */
const char* input_key_ini(int vk);               /* the ini name of a key ("Space", "LCtrl"); NULL if unbindable */
int input_button_from_name(const char* name);    /* "cross", "a", "options", "touchpad" -> INPUT_BTN_*; -1 unknown */
const char* input_button_ini(int btn);           /* "cross", ... */

/* Menu navigation: the nav bits (1 << INPUT_NAV_*) a controller / the keyboard asks for now. */
uint32_t input_state_nav_bits(const input_state* s);
/* Turn held nav bits into events with auto-repeat for directions. `pressed` are bits that went down
 * since the last step even if released again (short taps). Returns the number of events written. */
#define INPUT_NAV_DELAY_MS  400
#define INPUT_NAV_REPEAT_MS 110
typedef struct { uint32_t held; uint64_t next[16]; } input_nav_tracker;
int input_nav_step(input_nav_tracker* t, uint32_t now_bits, uint32_t pressed, uint64_t now_ms, int* navs, int* repeats,
                   int max);

/* Port assignment (notes/controls.md): the first controller showing activity takes port 0, the next
 * port 1; a port frees when its controller disconnects; a controller pressing something while the
 * port-0 controller has been idle for INPUT_TAKEOVER_MS takes port 0 over. */
#define INPUT_TAKEOVER_MS 2000
typedef struct { int id; int port; int active; uint64_t last_active; } input_port_dev;
/* Assign ports in place (devices in connection order; active = activity this step). Returns a bit
 * per device index whose port changed. */
uint32_t input_assign_ports(input_port_dev* d, int n, uint64_t now_ms);

/* ---- input.c: the device table (backends add, update and remove devices) ------------------------ */
typedef struct input_backend_ops {
    /* Send motors (0..255) and the light bar to the device; called on the input thread. */
    void (*output)(void* dev, uint8_t small_motor, uint8_t large_motor);
    /* XInput: read the current state now (the game asks); 0 = disconnected. NULL for HID devices. */
    int (*poll)(void* dev, input_state* st);
} input_backend_ops;
int  input_dev_add(int kind, int bus, uint16_t vid, uint16_t pid, const char* name, const input_backend_ops* ops,
                   void* backend, int can_rumble);          /* returns the device id */
void input_dev_remove(int id);
void input_dev_update(int id, const input_state* st, int battery, int charging, int enhanced, int crc_ok);
/* The motors each device should run now (game rumble for its port, a rumble test, or 0); called by
 * the input thread. Returns 1 if the device exists. */
int input_dev_motors(int id, uint8_t* small_motor, uint8_t* large_motor);
void input_dev_sent(int id, uint8_t small_motor, uint8_t large_motor);   /* what the backend sent (pad test) */
void input_service(void);                        /* input thread, every wake-up: XInput probing and rumble */
uint64_t input_now_ms(void);
/* Unit tests: replace XInputGetState / XInputSetState. */
typedef unsigned long (__stdcall* input_xi_get_fn)(unsigned long slot, void* state);
typedef unsigned long (__stdcall* input_xi_set_fn)(unsigned long slot, void* vibration);
void input_xinput_override(input_xi_get_fn get, input_xi_set_fn set);

/* input_hid.c: the HID backend and the input thread */
void input_hid_start(void);                      /* start the input thread (enumeration, hot-plug, reads) */
void input_hid_stop(void);                       /* motors off on every device, lights released */
void input_hid_kick(void);                       /* wake the thread (an output changed) */
