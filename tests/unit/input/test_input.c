/* Tests for the PC input code (runtime/input*.c), without any controller attached:
 *  - DualSense / DualShock 4 input reports (USB, Bluetooth full and simple) built from the documented
 *    layouts, CRC-32 checked against values computed independently (Python's zlib.crc32);
 *  - output reports (rumble, light bar, player LED, BT sequence and CRC);
 *  - controller -> DualShock 2 mapping (buttons, pressure, sticks, dead zones, camera inversion),
 *    XInput and keyboard mapping, generic HID pads;
 *  - [controls] keys (parse, format, round trip through sly2.ini via settings.c);
 *  - the device table with fake HID devices and a fake XInput: hot-plug, port assignment and
 *    takeover, menu suppression, hidden-until-released buttons, navigation events with repeat,
 *    binding capture, rumble scaling and timeout, connect/disconnect notices.
 * Built with runtime/input.c, input_map.c, input_ps.c, settings.c; input_hid.c is replaced by fakes. */
#include "input_internal.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xinput.h>

static int checks, failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                 \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

/* ---- fakes: the HID thread and XInput --------------------------------------------------------------- */
int kicks;
void input_hid_start(void) {}
void input_hid_stop(void) {}
void input_hid_kick(void) { kicks++; }

static XINPUT_STATE xi_state[4];
static int xi_present[4];
int xi_gets;
static XINPUT_VIBRATION xi_vib[4];
static int xi_sets[4];
static unsigned long __stdcall fake_xi_get(unsigned long slot, void* st) {
    xi_gets++;
    if (slot >= 4 || !xi_present[slot]) return ERROR_DEVICE_NOT_CONNECTED;
    memcpy(st, &xi_state[slot], sizeof xi_state[slot]);
    return ERROR_SUCCESS;
}
static unsigned long __stdcall fake_xi_set(unsigned long slot, void* v) {
    if (slot < 4) { memcpy(&xi_vib[slot], v, sizeof xi_vib[slot]); xi_sets[slot]++; }
    return ERROR_SUCCESS;
}

/* fake HID device output */
int out_calls;
uint8_t out_small, out_large;
static void fake_output(void* dev, uint8_t s, uint8_t l) { (void)dev; out_calls++; out_small = s; out_large = l; }
static const input_backend_ops fake_ops = { fake_output, NULL };

static int is_cleared(const pad_input* p) {
    pad_input c;
    input_pad_clear(&c);
    return memcmp(&c, p, sizeof c) == 0;
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
#define B(x) (1u << INPUT_BTN_##x)

/* ---- CRC and reports --------------------------------------------------------------------------------- */

static void test_crc(void) {
    CHECK(input_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 check value %08X", input_crc32(0, "123456789", 9));
    uint32_t a = input_crc32(0, "1234", 4);
    CHECK(input_crc32(a, "56789", 5) == 0xCBF43926u, "chained crc");
}

static void test_dualsense_input(void) {
    uint8_t r[78];
    ps_report p;
    /* USB 0x01 */
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    const uint8_t s[10] = { 0x00, 0xFF, 0x80, 0x40, 0x10, 0xF0, 0x05, 0x22, 0x41, 0x02 };
    memcpy(r + 1, s, 10);
    r[1 + 52] = 0x17;                                         /* charging, level 7 */
    CHECK(ps_parse_dualsense(r, 64, 0, &p) == 1, "usb parse");
    CHECK(p.full && p.crc_ok, "usb full");
    CHECK(p.st.lx == -32768 && p.st.ly == 32766 && p.st.rx == 0 && p.st.ry == -16512, "sticks %d %d %d %d", p.st.lx, p.st.ly, p.st.rx, p.st.ry);
    CHECK(p.st.l2 == 0x10 && p.st.r2 == 0xF0, "triggers");
    CHECK(p.st.buttons == (B(RIGHT) | B(CROSS) | B(L1) | B(L3) | B(TOUCHPAD)), "buttons %08X", p.st.buttons);
    CHECK(p.battery == 75 && p.charging == 1, "battery %d charging %d", p.battery, p.charging);
    CHECK(ps_parse_dualsense(r, 63, 0, &p) == 0, "short usb report accepted");
    /* every hat position */
    static const uint32_t hat[9] = { B(UP), B(UP) | B(RIGHT), B(RIGHT), B(RIGHT) | B(DOWN), B(DOWN), B(DOWN) | B(LEFT),
                                     B(LEFT), B(LEFT) | B(UP), 0 };
    for (int h = 0; h < 9; h++) {
        r[8] = (uint8_t)h; r[9] = 0; r[10] = 0;
        ps_parse_dualsense(r, 64, 0, &p);
        CHECK(p.st.buttons == hat[h], "hat %d -> %08X", h, p.st.buttons);
    }
    /* each button bit */
    static const struct { int byte; uint8_t bit; uint32_t want; } bits[] = {
        { 8, 0x18, B(SQUARE) }, { 8, 0x28, B(CROSS) }, { 8, 0x48, B(CIRCLE) }, { 8, 0x88, B(TRIANGLE) },
        { 9, 0x01, B(L1) }, { 9, 0x02, B(R1) }, { 9, 0x04, B(L2) }, { 9, 0x08, B(R2) }, { 9, 0x10, B(SELECT) },
        { 9, 0x20, B(START) }, { 9, 0x40, B(L3) }, { 9, 0x80, B(R3) }, { 10, 0x01, B(HOME) }, { 10, 0x02, B(TOUCHPAD) },
        { 10, 0x04, B(MUTE) },
    };
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        r[8] = 0x08; r[9] = 0; r[10] = 0;
        r[bits[i].byte] = bits[i].bit;
        ps_parse_dualsense(r, 64, 0, &p);
        CHECK(p.st.buttons == bits[i].want, "bit %zu -> %08X", i, p.st.buttons);
    }
    /* Bluetooth 0x31 with its CRC */
    memset(r, 0, sizeof r);
    r[0] = 0x31; r[1] = 0x01;
    memcpy(r + 2, s, 10);
    r[2 + 52] = 0x17;
    put32(r + 74, 0x9B4342C9u);                               /* zlib.crc32(0xA1 + r[0:74]) */
    CHECK(ps_parse_dualsense(r, 78, 1, &p) == 1 && p.full && p.crc_ok, "bt parse / crc %d", p.crc_ok);
    CHECK(ps_bt_crc(0xA1, r, 78) == 0x9B4342C9u, "bt input crc %08X", ps_bt_crc(0xA1, r, 78));
    CHECK(p.st.buttons == (B(RIGHT) | B(CROSS) | B(L1) | B(L3) | B(TOUCHPAD)) && p.st.r2 == 0xF0, "bt state %08X", p.st.buttons);
    r[20] ^= 1;
    ps_parse_dualsense(r, 78, 1, &p);
    CHECK(!p.crc_ok, "corrupt report passes the crc");
    /* Bluetooth simple 0x01 (DS4 layout): sticks, buttons, then L2/R2 */
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    const uint8_t simple[9] = { 0x80, 0x80, 0xFF, 0x00, 0x48, 0x20, 0x05, 0x33, 0x44 };
    memcpy(r + 1, simple, 9);
    CHECK(ps_parse_dualsense(r, 78, 1, &p) == 1 && !p.full, "simple parse");
    CHECK(p.st.buttons == (B(CIRCLE) | B(START) | B(HOME)) && p.st.l2 == 0x33 && p.st.r2 == 0x44 && p.st.rx == 32766 &&
          p.st.ry == -32768 && p.battery == -1, "simple state %08X %d %d", p.st.buttons, p.st.rx, p.st.ry);
}

static void test_ds4_input(void) {
    uint8_t r[78];
    ps_report p;
    memset(r, 0, sizeof r);
    const uint8_t s[9] = { 0xFF, 0x00, 0x40, 0xC0, 0x1F, 0x82, 0x01, 0x00, 0xFF };
    r[0] = 0x01;
    memcpy(r + 1, s, 9);
    r[1 + 29] = 0x1B;
    CHECK(ps_parse_ds4(r, 64, 0, &p) == 1 && p.full, "ds4 usb");
    CHECK(p.st.buttons == (B(SQUARE) | B(R1) | B(R3) | B(HOME)) && p.st.l2 == 0 && p.st.r2 == 0xFF, "ds4 usb %08X", p.st.buttons);
    CHECK(p.st.lx == 32766 && p.st.ly == -32768 && p.battery == 100 && !p.charging, "ds4 usb sticks/battery %d", p.battery);
    r[7] = 0xFF;                                              /* counter bits in byte 7 are not buttons */
    ps_parse_ds4(r, 64, 0, &p);
    CHECK(p.st.buttons == (B(SQUARE) | B(R1) | B(R3) | B(HOME) | B(TOUCHPAD)), "ds4 counter bits %08X", p.st.buttons);
    memset(r, 0, sizeof r);
    r[0] = 0x11; r[1] = 0xC0;
    memcpy(r + 3, s, 9);
    r[3 + 29] = 0x1B;
    put32(r + 74, 0x3A24C9F4u);
    CHECK(ps_parse_ds4(r, 78, 1, &p) == 1 && p.full && p.crc_ok, "ds4 bt crc %08X", ps_bt_crc(0xA1, r, 78));
    CHECK(p.st.buttons == (B(SQUARE) | B(R1) | B(R3) | B(HOME)), "ds4 bt %08X", p.st.buttons);
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    memcpy(r + 1, s, 9);
    CHECK(ps_parse_ds4(r, 547, 1, &p) == 1 && !p.full && p.battery == -1, "ds4 bt simple");
    CHECK(ps_parse_ds4(r, 9, 1, &p) == 0, "too short");
    r[0] = 0x05;
    CHECK(ps_parse_ds4(r, 64, 0, &p) == 0, "other report id");
}

static void test_outputs(void) {
    uint8_t b[PS_OUTPUT_MAX];
    ps_output o;
    memset(&o, 0, sizeof o);
    o.motor_small = 0x80; o.motor_large = 0xFF; o.lightbar_on = 1; o.rgb[0] = 0x28; o.rgb[1] = 0x64; o.rgb[2] = 0xFF;
    o.player_leds = 0x04; o.vibration_v2 = 1; o.seq = 3;
    size_t n = ps_build_dualsense(&o, 1, b);
    CHECK(n == 78 && b[0] == 0x31 && b[1] == 0x30 && b[2] == 0x10, "ds bt header %02X %02X %02X", b[0], b[1], b[2]);
    CHECK(b[3] == 0x02 && b[4] == 0x14 && b[5] == 0x80 && b[6] == 0xFF && b[41] == 0x04 && b[46] == 0x04 &&
          b[47] == 0x28 && b[48] == 0x64 && b[49] == 0xFF, "ds bt common");
    CHECK(le32(b + 74) == 0xE262EA8Eu, "ds bt crc %08X", le32(b + 74));
    o.seq = 0x13;                                             /* 4-bit sequence */
    ps_build_dualsense(&o, 1, b);
    CHECK(b[1] == 0x30, "sequence wraps: %02X", b[1]);
    o.bt_alt_header = 1;                                      /* SDL's header (PS2_DS_BT_HEADER=sdl) */
    ps_build_dualsense(&o, 1, b);
    CHECK(b[0] == 0x31 && b[1] == 0x02 && b[2] == 0x02 && b[3] == 0x14 && b[4] == 0x80 && b[46] == 0x28 &&
          le32(b + 74) == ps_bt_crc(0xA2, b, 78), "sdl header");
    o.bt_alt_header = 0;
    n = ps_build_dualsense(&o, 0, b);
    CHECK(n == 63 && b[0] == 0x02 && b[1] == 0x02 && b[2] == 0x14 && b[3] == 0x80 && b[4] == 0xFF && b[39] == 0x04 &&
          b[44] == 0x04 && b[45] == 0x28 && b[47] == 0xFF, "ds usb");
    o.vibration_v2 = 0;                                       /* older firmware: classic flag, half values */
    ps_build_dualsense(&o, 0, b);
    CHECK(b[1] == 0x03 && b[3] == 0x40 && b[4] == 0x7F && b[39] == 0x00, "ds usb classic rumble");
    o.release_leds = 1;
    ps_build_dualsense(&o, 0, b);
    CHECK((b[2] & 0x08) && !(b[2] & 0x04) && (b[39] & 0x02) && b[42] == 0x02 && b[45] == 0, "release leds");
    o.release_leds = 0; o.lightbar_on = 0;
    ps_build_dualsense(&o, 0, b);
    CHECK((b[2] & 0x04) && b[45] == 0 && b[46] == 0 && b[47] == 0, "lightbar off");

    memset(&o, 0, sizeof o);
    o.motor_small = 0x80; o.motor_large = 0xFF; o.lightbar_on = 1; o.rgb[0] = 0x28; o.rgb[1] = 0x64; o.rgb[2] = 0xFF;
    n = ps_build_ds4(&o, 1, b);
    CHECK(n == 78 && b[0] == 0x11 && b[1] == 0xC4 && b[2] == 0 && b[3] == 0x03 && b[6] == 0x80 && b[7] == 0xFF &&
          b[8] == 0x28 && b[9] == 0x64 && b[10] == 0xFF, "ds4 bt");
    CHECK(le32(b + 74) == 0xFCFA6253u, "ds4 bt crc %08X", le32(b + 74));
    n = ps_build_ds4(&o, 0, b);
    CHECK(n == 32 && b[0] == 0x05 && b[1] == 0x03 && b[4] == 0x80 && b[5] == 0xFF && b[6] == 0x28 && b[8] == 0xFF, "ds4 usb");

    uint8_t f[64];
    memset(f, 0, sizeof f);
    f[0] = 0x20; f[44] = 0x24; f[45] = 0x02;
    CHECK(ps_dualsense_fw_version(f, 64) == 0x0224, "fw version");
    CHECK(ps_dualsense_fw_version(f, 40) == 0, "fw short");
    CHECK(ps_kind(0x054C, 0x0CE6) == INPUT_DEV_DUALSENSE && ps_kind(0x054C, 0x0DF2) == INPUT_DEV_DUALSENSE_EDGE &&
          ps_kind(0x054C, 0x09CC) == INPUT_DEV_DS4 && ps_kind(0x054C, 0x05C4) == INPUT_DEV_DS4 &&
          ps_kind(0x045E, 0x0CE6) == INPUT_DEV_NONE && ps_kind(0x054C, 0x0268) == INPUT_DEV_NONE, "kinds");
}

/* ---- mapping ------------------------------------------------------------------------------------------- */

static void test_state_mapping(void) {
    input_config c;
    input_config_defaults(&c);
    input_state s;
    pad_input p;
    memset(&s, 0, sizeof s);
    input_pad_clear(&p);
    input_state_to_pad(&s, &c, &p);
    CHECK(is_cleared(&p), "idle state");
    static const struct { int btn; uint16_t pad; int pr; } one[] = {
        { INPUT_BTN_CROSS, PAD_CROSS, 6 }, { INPUT_BTN_CIRCLE, PAD_CIRCLE, 5 }, { INPUT_BTN_SQUARE, PAD_SQUARE, 7 },
        { INPUT_BTN_TRIANGLE, PAD_TRIANGLE, 4 }, { INPUT_BTN_L1, PAD_L1, 8 }, { INPUT_BTN_R1, PAD_R1, 9 },
        { INPUT_BTN_L2, PAD_L2, 10 }, { INPUT_BTN_R2, PAD_R2, 11 }, { INPUT_BTN_L3, PAD_L3, -1 }, { INPUT_BTN_R3, PAD_R3, -1 },
        { INPUT_BTN_SELECT, PAD_SELECT, -1 }, { INPUT_BTN_TOUCHPAD, PAD_SELECT, -1 }, { INPUT_BTN_START, PAD_START, -1 },
        { INPUT_BTN_UP, PAD_UP, 2 }, { INPUT_BTN_DOWN, PAD_DOWN, 3 }, { INPUT_BTN_LEFT, PAD_LEFT, 1 },
        { INPUT_BTN_RIGHT, PAD_RIGHT, 0 }, { INPUT_BTN_HOME, 0, -1 }, { INPUT_BTN_MUTE, 0, -1 },
    };
    for (size_t i = 0; i < sizeof one / sizeof one[0]; i++) {
        memset(&s, 0, sizeof s);
        s.buttons = 1u << one[i].btn;
        input_pad_clear(&p);
        input_state_to_pad(&s, &c, &p);
        CHECK(p.buttons == one[i].pad, "btn %d -> %04X", one[i].btn, p.buttons);
        for (int k = 0; k < 12; k++) CHECK(p.pressure[k] == (k == one[i].pr ? 0xFF : 0), "btn %d pressure %d", one[i].btn, k);
    }
    /* analog triggers: pressure = travel; the digital bit alone = full press */
    memset(&s, 0, sizeof s);
    s.l2 = 30; s.r2 = 200;
    input_pad_clear(&p);
    input_state_to_pad(&s, &c, &p);
    CHECK(p.buttons == PAD_R2 && p.pressure[10] == 0 && p.pressure[11] == 200, "triggers %04X", p.buttons);
    s.buttons = B(L2); s.l2 = 0;
    input_pad_clear(&p);
    input_state_to_pad(&s, &c, &p);
    CHECK((p.buttons & PAD_L2) && p.pressure[10] == 0xFF, "digital L2");
    /* remapped: Cross on R1, Circle on two buttons, Start unbound */
    input_config r = c;
    r.pad[INPUT_ACT_CROSS][0] = INPUT_BTN_R1;
    r.pad[INPUT_ACT_R1][0] = INPUT_BTN_CROSS;
    r.pad[INPUT_ACT_CIRCLE][1] = INPUT_BTN_TRIANGLE;
    r.pad[INPUT_ACT_START][0] = INPUT_BTN_NONE;
    memset(&s, 0, sizeof s);
    s.buttons = B(R1) | B(TRIANGLE) | B(START);
    input_pad_clear(&p);
    input_state_to_pad(&s, &r, &p);
    CHECK(p.buttons == (PAD_CROSS | PAD_CIRCLE | PAD_TRIANGLE) && p.pressure[6] == 0xFF && p.pressure[9] == 0, "remap %04X", p.buttons);
    /* camera inversion */
    input_pad_clear(&p);
    p.rx = 0x00; p.ry = 0xFF; p.lx = 0x00;
    c.invert_camera_x = 1; c.invert_camera_y = 1;
    input_invert_camera(&p, &c);
    CHECK(p.rx == 0xFF && p.ry == 0x01 && p.lx == 0x00, "invert %02X %02X", p.rx, p.ry);
    p.rx = p.ry = 0x80;
    input_invert_camera(&p, &c);
    CHECK(p.rx == 0x80 && p.ry == 0x80, "invert keeps centre");
    /* dead zone setting */
    uint8_t x, y;
    input_stick(5000, 0, 20, &x, &y);
    CHECK(x == 0x80 && y == 0x80, "inside 20%%: %02X", x);
    input_stick(5000, 0, 10, &x, &y);
    CHECK(x > 0x80 && x < 0x90, "past 10%%: %02X", x);
    input_stick(32767, 0, 0, &x, &y);
    CHECK(x == 0xFF, "full: %02X", x);
    input_stick(-32768, 0, 50, &x, &y);
    CHECK(x == 0x00, "full left with 50%%: %02X", x);
    CHECK(input_axis_byte(0) == 0x80 && input_axis_byte(32767) == 0xFF && input_axis_byte(-32768) == 0, "axis bytes");
    CHECK(input_axis_from_byte(0x80) == 0 && input_axis_from_byte(0) == -32768 && input_axis_from_byte(0xFF) == 32766, "from byte");
}

static void test_xinput_mapping(void) {
    input_config c;
    input_config_defaults(&c);
    input_state s;
    pad_input p;
    static const struct { WORD xi; uint16_t pad; int pr; } one[] = {
        { XINPUT_GAMEPAD_A, PAD_CROSS, 6 }, { XINPUT_GAMEPAD_B, PAD_CIRCLE, 5 },
        { XINPUT_GAMEPAD_X, PAD_SQUARE, 7 }, { XINPUT_GAMEPAD_Y, PAD_TRIANGLE, 4 },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, PAD_L1, 8 }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, PAD_R1, 9 },
        { XINPUT_GAMEPAD_DPAD_RIGHT, PAD_RIGHT, 0 }, { XINPUT_GAMEPAD_DPAD_LEFT, PAD_LEFT, 1 },
        { XINPUT_GAMEPAD_DPAD_UP, PAD_UP, 2 }, { XINPUT_GAMEPAD_DPAD_DOWN, PAD_DOWN, 3 },
        { XINPUT_GAMEPAD_LEFT_THUMB, PAD_L3, -1 }, { XINPUT_GAMEPAD_RIGHT_THUMB, PAD_R3, -1 },
        { XINPUT_GAMEPAD_BACK, PAD_SELECT, -1 }, { XINPUT_GAMEPAD_START, PAD_START, -1 },
    };
    for (size_t i = 0; i < sizeof one / sizeof one[0]; i++) {
        input_xinput_to_state(one[i].xi, 0, 0, 0, 0, 0, 0, &s);
        input_pad_clear(&p);
        input_state_to_pad(&s, &c, &p);
        CHECK(p.buttons == one[i].pad, "xinput %04X -> %04X", one[i].xi, p.buttons);
        for (int k = 0; k < 12; k++) CHECK(p.pressure[k] == (k == one[i].pr ? 0xFF : 0), "xinput %04X pressure %d", one[i].xi, k);
        CHECK(p.lx == 0x80 && p.ly == 0x80 && p.rx == 0x80 && p.ry == 0x80, "sticks moved");
    }
    input_xinput_to_state(0, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 200, 0, 0, 0, 0, &s);
    input_pad_clear(&p);
    input_state_to_pad(&s, &c, &p);
    CHECK(p.buttons == PAD_R2 && p.pressure[10] == 0 && p.pressure[11] == 200, "xi triggers %04X", p.buttons);
    static const struct { SHORT lx, ly, rx, ry; uint8_t elx, ely, erx, ery; } sticks[] = {
        { 0, 0, 0, 0, 0x80, 0x80, 0x80, 0x80 }, { 32767, 0, 0, 0, 0xFF, 0x80, 0x80, 0x80 },
        { -32768, 0, 0, 0, 0x00, 0x80, 0x80, 0x80 }, { 0, 32767, 0, 0, 0x80, 0x00, 0x80, 0x80 },
        { 0, -32768, 0, 0, 0x80, 0xFF, 0x80, 0x80 }, { 0, 0, 32767, 0, 0x80, 0x80, 0xFF, 0x80 },
        { 0, 0, -32768, 0, 0x80, 0x80, 0x00, 0x80 }, { 0, 0, 0, -32768, 0x80, 0x80, 0x80, 0xFF },
        { 0, 0, 0, 32767, 0x80, 0x80, 0x80, 0x00 }, { 0, 0, 32767, -32768, 0x80, 0x80, 0xDA, 0xDA },
        { 2000, -2000, 3000, 1000, 0x80, 0x80, 0x80, 0x80 },     /* inside the 12% dead zones */
    };
    for (size_t i = 0; i < sizeof sticks / sizeof sticks[0]; i++) {
        input_xinput_to_state(0, 0, 0, sticks[i].lx, sticks[i].ly, sticks[i].rx, sticks[i].ry, &s);
        input_pad_clear(&p);
        input_state_to_pad(&s, &c, &p);
        CHECK(p.lx == sticks[i].elx && p.ly == sticks[i].ely && p.rx == sticks[i].erx && p.ry == sticks[i].ery,
              "stick case %zu: %02X %02X %02X %02X", i, p.lx, p.ly, p.rx, p.ry);
    }
    input_xinput_to_state(0, 0, 0, 23170, 23170, 0, 0, &s);
    input_pad_clear(&p);
    input_state_to_pad(&s, &c, &p);
    CHECK(p.lx >= 0xD6 && p.lx <= 0xDC && p.ly >= 0x24 && p.ly <= 0x2A, "diagonal %02X %02X", p.lx, p.ly);
    int prev = 0x80, mono = 1;
    for (int v = 0; v <= 32767; v += 97) {
        uint8_t x, y;
        input_stick(v, 0, 12, &x, &y);
        if (x < prev || y != 0x80) mono = 0;
        prev = x;
    }
    CHECK(mono && prev == 0xFF, "ramp not monotonic (end %02X)", prev);
    uint8_t x, y;
    input_stick(32767 * 12 / 100 + 300, 0, 12, &x, &y);
    CHECK(x >= 0x80 && x <= 0x82, "just past dead zone: %02X", x);
}

static void test_keyboard_mapping(void) {
    input_config c;
    input_config_defaults(&c);
    uint8_t down[256];
    pad_input p;
    memset(down, 0, sizeof down);
    input_pad_clear(&p);
    input_keys_to_pad(down, &c, &p);
    CHECK(is_cleared(&p), "no keys");
    static const struct { uint8_t vk; uint16_t pad; int pr; } one[] = {
        { VK_SPACE, PAD_CROSS, 6 }, { VK_LCONTROL, PAD_CIRCLE, 5 }, { 'F', PAD_SQUARE, 7 }, { 'R', PAD_TRIANGLE, 4 },
        { VK_NUMPAD2, PAD_CROSS, 6 }, { VK_NUMPAD6, PAD_CIRCLE, 5 }, { VK_NUMPAD4, PAD_SQUARE, 7 }, { VK_NUMPAD8, PAD_TRIANGLE, 4 },
        { 'Q', PAD_L1, 8 }, { 'E', PAD_R1, 9 }, { '1', PAD_L2, 10 }, { '3', PAD_R2, 11 },
        { VK_UP, PAD_UP, 2 }, { VK_DOWN, PAD_DOWN, 3 }, { VK_LEFT, PAD_LEFT, 1 }, { VK_RIGHT, PAD_RIGHT, 0 },
        { VK_RETURN, PAD_START, -1 }, { VK_BACK, PAD_SELECT, -1 }, { 'C', PAD_L3, -1 }, { 'U', PAD_R3, -1 },
    };
    uint16_t all = 0;
    for (size_t i = 0; i < sizeof one / sizeof one[0]; i++) {
        memset(down, 0, sizeof down);
        down[one[i].vk] = 1;
        input_pad_clear(&p);
        input_keys_to_pad(down, &c, &p);
        all |= p.buttons;
        CHECK(p.buttons == one[i].pad, "key %02X -> %04X", one[i].vk, p.buttons);
        for (int k = 0; k < 12; k++) CHECK(p.pressure[k] == (k == one[i].pr ? 0xFF : 0), "key %02X pressure %d", one[i].vk, k);
    }
    CHECK(all == 0xFFFF, "keyboard reaches %04X", all);
    memset(down, 0, sizeof down);
    down[VK_CONTROL] = down[VK_RCONTROL] = 1;
    input_pad_clear(&p);
    input_keys_to_pad(down, &c, &p);
    CHECK(is_cleared(&p), "right ctrl mapped");
    static const struct { const char* keys; uint8_t lx, ly, rx, ry; } st[] = {
        { "W", 0x80, 0x00, 0x80, 0x80 }, { "S", 0x80, 0xFF, 0x80, 0x80 }, { "A", 0x00, 0x80, 0x80, 0x80 },
        { "D", 0xFF, 0x80, 0x80, 0x80 }, { "WD", 0xFF, 0x00, 0x80, 0x80 }, { "WS", 0x80, 0x80, 0x80, 0x80 },
        { "I", 0x80, 0x80, 0x80, 0x00 }, { "K", 0x80, 0x80, 0x80, 0xFF }, { "J", 0x80, 0x80, 0x00, 0x80 },
        { "L", 0x80, 0x80, 0xFF, 0x80 }, { "AJKS", 0x00, 0xFF, 0x00, 0xFF }, { "JL", 0x80, 0x80, 0x80, 0x80 },
    };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) {
        memset(down, 0, sizeof down);
        for (const char* k = st[i].keys; *k; k++) down[(uint8_t)*k] = 1;
        input_pad_clear(&p);
        input_keys_to_pad(down, &c, &p);
        CHECK(p.buttons == 0 && p.lx == st[i].lx && p.ly == st[i].ly && p.rx == st[i].rx && p.ry == st[i].ry,
              "keys %s: %02X %02X %02X %02X", st[i].keys, p.lx, p.ly, p.rx, p.ry);
    }
    input_pad_clear(&p);
    p.buttons = PAD_CIRCLE; p.lx = 0x30; p.ly = 0x40; p.rx = 0x50;
    memset(down, 0, sizeof down);
    down['D'] = 1; down[VK_SPACE] = 1;
    input_keys_to_pad(down, &c, &p);
    CHECK(p.buttons == (PAD_CIRCLE | PAD_CROSS) && p.lx == 0xFF && p.ly == 0x40 && p.rx == 0x50, "merge");
    /* rebinding: Cross on Mouse1 + 'X', stick up on Up arrow */
    c.key[INPUT_ACT_CROSS][0] = VK_LBUTTON; c.key[INPUT_ACT_CROSS][1] = 'X';
    c.key[INPUT_ACT_LSTICK_UP][0] = VK_UP;
    memset(down, 0, sizeof down);
    down[VK_LBUTTON] = 1; down[VK_UP] = 1; down[VK_SPACE] = 1;
    input_pad_clear(&p);
    input_keys_to_pad(down, &c, &p);
    CHECK(p.buttons == (PAD_CROSS | PAD_UP) && p.ly == 0x00, "rebound keys %04X %02X", p.buttons, p.ly);
}

static void test_generic(void) {
    int axes[6] = { 0, 0, 32767, -32768, 32767, -32768 };   /* x y z rx ry rz */
    input_state s;
    input_generic_to_state(0x3FFF, 2, axes, &s);
    CHECK(s.buttons == (B(SQUARE) | B(CROSS) | B(CIRCLE) | B(TRIANGLE) | B(L1) | B(R1) | B(L2) | B(R2) | B(SELECT) |
                        B(START) | B(L3) | B(R3) | B(HOME) | B(TOUCHPAD) | B(RIGHT)), "generic buttons %08X", s.buttons);
    CHECK(s.rx == 32767 && s.ry == -32768 && s.l2 == 0 && s.r2 == 0xFF, "generic z/rz right stick, rx/ry triggers");
    int axes2[6] = { 100, -100, INPUT_AXIS_ABSENT, 5000, -5000, INPUT_AXIS_ABSENT };
    input_generic_to_state(0, -1, axes2, &s);
    CHECK(s.buttons == 0 && s.lx == 100 && s.ly == -100 && s.rx == 5000 && s.ry == -5000, "generic rx/ry right stick");
    input_generic_to_state(0, 7, axes2, &s);
    CHECK(s.buttons == (B(UP) | B(LEFT)), "hat NW %08X", s.buttons);
}

/* ---- [controls] ------------------------------------------------------------------------------------------ */

static void test_config(void) {
    input_config c, d;
    input_config_defaults(&c);
    CHECK(c.vibration == 1 && c.vibration_strength == 100 && c.deadzone_left == 12 && c.lightbar_on == 1 &&
          c.lightbar[2] == 0xFF && c.pad[INPUT_ACT_SELECT][1] == INPUT_BTN_TOUCHPAD, "defaults");
    CHECK(input_config_apply(&c, "vibration", "off") == 0 && !c.vibration, "vibration off");
    CHECK(input_config_apply(&c, "vibration_strength", "40%") == 0 && c.vibration_strength == 40, "strength");
    CHECK(input_config_apply(&c, "vibration_strength", "140") == 1 && c.vibration_strength == 40, "strength range");
    CHECK(input_config_apply(&c, "invert_camera_y", "on") == 0 && c.invert_camera_y, "invert");
    CHECK(input_config_apply(&c, "deadzone_right", "20") == 0 && c.deadzone_right == 20, "deadzone");
    CHECK(input_config_apply(&c, "deadzone_right", "60") == 1, "deadzone range");
    CHECK(input_config_apply(&c, "lightbar_color", "FF8000") == 0 && c.lightbar[0] == 0xFF && c.lightbar[1] == 0x80 &&
          c.lightbar[2] == 0, "colour");
    CHECK(input_config_apply(&c, "lightbar_color", "FF80") == 1, "short colour");
    CHECK(input_config_apply(&c, "key_cross", "X, Numpad5") == 0 && c.key[INPUT_ACT_CROSS][0] == 'X' &&
          c.key[INPUT_ACT_CROSS][1] == VK_NUMPAD5, "key list");
    CHECK(input_config_apply(&c, "key_circle", "none") == 0 && !c.key[INPUT_ACT_CIRCLE][0] && !c.key[INPUT_ACT_CIRCLE][1], "key none");
    CHECK(input_config_apply(&c, "key_square", "F5") == 1, "reserved F5 accepted");
    CHECK(input_config_apply(&c, "key_square", "A, B, C") == 1, "three keys accepted");
    CHECK(input_config_apply(&c, "key_rstick_up", "mouse4") == 0 && c.key[INPUT_ACT_RSTICK_UP][0] == VK_XBUTTON1, "mouse key");
    CHECK(input_config_apply(&c, "pad_cross", "a") == 0 && c.pad[INPUT_ACT_CROSS][0] == INPUT_BTN_CROSS, "xbox alias");
    CHECK(input_config_apply(&c, "pad_select", "share, touchpad") == 0 && c.pad[INPUT_ACT_SELECT][0] == INPUT_BTN_SELECT &&
          c.pad[INPUT_ACT_SELECT][1] == INPUT_BTN_TOUCHPAD, "pad list");
    CHECK(input_config_apply(&c, "pad_start", "ps") == 0 && c.pad[INPUT_ACT_START][0] == INPUT_BTN_HOME &&
          c.pad[INPUT_ACT_START][1] == INPUT_BTN_NONE, "pad ps");
    CHECK(input_config_apply(&c, "pad_l1", "cross") == 0 && c.pad[INPUT_ACT_L1][0] == INPUT_BTN_CROSS, "pad cross value 0");
    CHECK(input_config_apply(&c, "pad_lstick_up", "cross") == -1, "no stick pad bindings");
    CHECK(input_config_apply(&c, "frobnicate", "1") == -1, "unknown key");
    /* format every key and read it back */
    d = c;
    input_config e;
    input_config_defaults(&e);
    for (int i = 0; i < input_config_keys(); i++) {
        char k[40], v[128];
        input_config_key(&d, i, k, sizeof k, v, sizeof v);
        CHECK(input_config_apply(&e, k, v) == 0, "round trip %s = %s", k, v);
    }
    CHECK(!memcmp(&d, &e, sizeof d), "round trip differs");
    /* names */
    CHECK(input_key_from_name("space") == VK_SPACE && input_key_from_name("LCtrl") == VK_LCONTROL &&
          input_key_from_name("q") == 'Q' && input_key_from_name("0x41") == 'A' && input_key_from_name("Escape") == 0 &&
          input_key_from_name("0x1B") == 0, "key names");
    CHECK(!strcmp(input_key_label(VK_NUMPAD2), "Numpad 2") && !strcmp(input_key_label('W'), "W") && !input_key_label(VK_ESCAPE) &&
          !input_key_label(VK_F7) && !input_key_label(VK_MENU), "key labels");
    CHECK(!strcmp(input_button_label(INPUT_BTN_SELECT, 0), "Create") && !strcmp(input_button_label(INPUT_BTN_SELECT, 1), "View") &&
          !strcmp(input_button_label(INPUT_BTN_CROSS, 1), "A"), "button labels");
    CHECK(!strcmp(input_action_ini(INPUT_ACT_RSTICK_LEFT), "rstick_left") && !strcmp(input_action_label(INPUT_ACT_L2), "L2"), "action names");
}

static int section_keys;
static void count_key(void* user, const char* key, const char* value, int line) {
    (void)line;
    input_config_apply((input_config*)user, key, value);
    section_keys++;
}

static void test_section_parse(void) {
    const char* text = "[display]\naspect = 16:9\n[controls]\nvibration = off ; quiet\nkey_cross = \"Space, X\"\n"
                       "[graphics]\nvibration = on\n";
    input_config c;
    input_config_defaults(&c);
    section_keys = 0;
    CHECK(settings_parse_section(text, "controls", count_key, &c) == 2 && section_keys == 2, "section keys %d", section_keys);
    CHECK(!c.vibration && c.key[INPUT_ACT_CROSS][1] == 'X', "section values");
    ps2_settings_t s;
    settings_defaults(&s);
    CHECK(settings_parse(text, &s, NULL, NULL) == 1, "settings_parse: [controls] keys are not problems (only graphics.vibration is)");
}

/* ---- the device table ----------------------------------------------------------------------------------- */

static input_state st_buttons(uint32_t b) { input_state s; memset(&s, 0, sizeof s); s.buttons = b; return s; }

static void test_devices(void) {
    pad_input p;
    input_state s;
    input_notice n;
    while (input_poll_notice(&n)) {}
    int a = input_dev_add(INPUT_DEV_DUALSENSE, INPUT_BUS_USB, 0x054C, 0x0CE6, "DualSense (USB)", &fake_ops, NULL, 1);
    CHECK(a > 0, "add");
    CHECK(input_poll_notice(&n) && n.connected && n.device_id == a && !strcmp(n.name, "DualSense (USB)"), "connect notice");
    input_get_pad(0, &p);
    CHECK(is_cleared(&p), "idle controller drives nothing");
    CHECK(input_dev_port(a) == -1, "no port before activity");
    s = st_buttons(B(CROSS));
    input_dev_update(a, &s, 80, 0, 1, 1);
    CHECK(input_dev_port(a) == 0, "first active -> port 0");
    input_get_pad(0, &p);
    CHECK(p.buttons == PAD_CROSS && p.pressure[6] == 0xFF, "port 0 cross %04X", p.buttons);
    int b = input_dev_add(INPUT_DEV_DS4, INPUT_BUS_BT, 0x054C, 0x09CC, "DualShock 4 (Bluetooth)", &fake_ops, NULL, 1);
    s = st_buttons(B(TRIANGLE));
    input_dev_update(b, &s, -1, 0, 1, 0);
    CHECK(input_dev_port(b) == 1, "second active -> port 1 (port 0 busy)");
    input_get_pad(1, &p);
    CHECK(p.buttons == PAD_TRIANGLE, "port 1 %04X", p.buttons);
    input_device_info info[INPUT_MAX_DEVICES];
    int nd = input_devices(info, INPUT_MAX_DEVICES);
    CHECK(nd == 2 && info[0].id == a && info[0].battery == 80 && info[1].crc_errors == 1 && info[1].bus == INPUT_BUS_BT,
          "device list %d", nd);
    /* unplug the port-0 controller in the middle of play: the game keeps running, port 0 neutral */
    input_dev_remove(a);
    input_get_pad(0, &p);
    CHECK(is_cleared(&p), "removed controller still drives port 0");
    input_poll_notice(&n);
    CHECK(input_poll_notice(&n) && !n.connected && n.device_id == a && n.port == 0, "disconnect notice");
    s = st_buttons(B(SQUARE));
    input_dev_update(b, &s, -1, 0, 1, 1);
    CHECK(input_dev_port(b) == 0, "next active controller takes port 0");
    input_get_pad(0, &p);
    CHECK(p.buttons == PAD_SQUARE, "port 0 now DS4 %04X", p.buttons);
    input_get_pad(1, &p);
    CHECK(is_cleared(&p), "port 1 freed");
    /* reconnect: a new id, joins as port 1 once used */
    a = input_dev_add(INPUT_DEV_DUALSENSE, INPUT_BUS_BT, 0x054C, 0x0CE6, "DualSense (Bluetooth)", &fake_ops, NULL, 1);
    s = st_buttons(B(CIRCLE));
    input_dev_update(a, &s, -1, 0, 1, 1);
    CHECK(input_dev_port(a) == 1, "reconnected controller -> port 1, got %d", input_dev_port(a));

    /* rumble: the port's controller, scaled by the options */
    uint8_t ms, ml;
    input_set_rumble(0, 1, 0x80);
    CHECK(input_dev_motors(b, &ms, &ml) && ms == 255 && ml == 0x80, "rumble %d %d", ms, ml);
    CHECK(input_dev_motors(a, &ms, &ml) && ms == 0 && ml == 0, "rumble on the other port's controller");
    input_config c;
    input_config_get(&c);
    c.vibration_strength = 50;
    input_config_set(&c, 0);
    input_dev_motors(b, &ms, &ml);
    CHECK(ms == 127 && ml == 64, "strength 50%%: %d %d", ms, ml);
    c.vibration = 0;
    input_config_set(&c, 0);
    input_dev_motors(b, &ms, &ml);
    CHECK(ms == 0 && ml == 0, "vibration off");
    c.vibration = 1; c.vibration_strength = 100;
    input_config_set(&c, 0);
    input_test_rumble(a, 0, 200, 1000);
    input_dev_motors(a, &ms, &ml);
    CHECK(ms == 0 && ml == 200, "rumble test %d %d", ms, ml);
    input_test_rumble(a, 0, 0, 0);
    input_set_rumble(0, 0, 0xFF);
    Sleep(350);
    input_dev_motors(b, &ms, &ml);
    CHECK(ms == 0 && ml == 0, "rumble keeps going after the game stopped sending");

    /* the menu: neutral game input, navigation, hidden until released */
    s = st_buttons(0);
    input_dev_update(a, &s, -1, 0, 1, 1);
    input_dev_update(b, &s, -1, 0, 1, 1);
    input_set_menu_active(1);
    input_nav_flush();
    input_nav_event ev;
    s = st_buttons(B(CROSS));
    input_dev_update(b, &s, -1, 0, 1, 1);
    input_get_pad(0, &p);
    CHECK(is_cleared(&p), "menu open: game sees input");
    input_set_rumble(0, 1, 0xFF);
    input_dev_motors(b, &ms, &ml);
    CHECK(ms == 0 && ml == 0, "menu open: rumble");
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_CONFIRM && ev.source == INPUT_SRC_CONTROLLER && ev.device_id == b, "confirm event");
    CHECK(!input_nav_poll(&ev), "one event per press");
    s = st_buttons(0);
    input_dev_update(b, &s, -1, 0, 1, 1);
    s = st_buttons(B(CIRCLE));                                /* a tap between two polls still counts */
    input_dev_update(a, &s, -1, 0, 1, 1);
    s = st_buttons(0);
    input_dev_update(a, &s, -1, 0, 1, 1);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_BACK && ev.device_id == a, "tap -> back");
    s = st_buttons(B(DOWN));
    input_dev_update(b, &s, -1, 0, 1, 1);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_DOWN && !ev.repeat, "down");
    Sleep(INPUT_NAV_DELAY_MS + 30);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_DOWN && ev.repeat, "down repeats");
    s = st_buttons(B(START));
    input_dev_update(b, &s, -1, 0, 1, 1);
    while (input_nav_poll(&ev) && ev.nav != INPUT_NAV_MENU) {}
    CHECK(ev.nav == INPUT_NAV_MENU, "start in the menu -> MENU");
    /* close the menu while Start is still held: the game must not see it until it is released */
    input_set_menu_active(0);
    input_get_pad(0, &p);
    CHECK(p.buttons == 0, "held start leaked into the game %04X", p.buttons);
    s = st_buttons(0);
    input_dev_update(b, &s, -1, 0, 1, 1);
    s = st_buttons(B(START));
    input_dev_update(b, &s, -1, 0, 1, 1);
    input_get_pad(0, &p);
    CHECK(p.buttons == PAD_START, "start after release %04X", p.buttons);
    input_nav_flush();
    s = st_buttons(0);
    input_dev_update(b, &s, -1, 0, 1, 1);
    s = st_buttons(B(START));
    input_dev_update(b, &s, -1, 0, 1, 1);
    CHECK(!input_nav_poll(&ev), "start with the menu closed is the game's (event %d)", ev.nav);
    s = st_buttons(B(HOME));
    input_dev_update(b, &s, -1, 0, 1, 1);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_MENU, "PS button opens the menu");
    s = st_buttons(0);
    input_dev_update(b, &s, -1, 0, 1, 1);

    /* capture the next controller button */
    input_set_menu_active(1);
    input_nav_flush();
    int code = -1;
    input_capture_begin(INPUT_CAPTURE_CONTROLLER);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_PENDING, "pending");
    s = st_buttons(B(R2));
    input_dev_update(a, &s, -1, 0, 1, 1);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_BUTTON && code == INPUT_BTN_R2, "captured %d", code);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_IDLE, "idle after");
    input_capture_begin(INPUT_CAPTURE_CONTROLLER);
    s = st_buttons(B(R2) | B(HOME));
    input_dev_update(a, &s, -1, 0, 1, 1);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_CANCELLED, "PS cancels");
    s = st_buttons(B(CROSS));
    input_capture_begin(INPUT_CAPTURE_CONTROLLER);
    input_dev_update(a, &s, -1, 0, 1, 1);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_BUTTON && code == INPUT_BTN_CROSS, "capture cross");
    CHECK(!input_nav_poll(&ev), "the captured Cross also confirmed (event %d)", ev.nav);

    /* keyboard and mouse through the window procedure */
    input_window_message(NULL, WM_KEYDOWN, VK_DOWN, 0);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_DOWN && ev.source == INPUT_SRC_KEYBOARD, "key down");
    input_window_message(NULL, WM_KEYDOWN, VK_DOWN, 1 << 30);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_DOWN && ev.repeat, "key auto-repeat");
    CHECK(input_window_message(NULL, WM_KEYDOWN, VK_ESCAPE, 0) == 1 && input_nav_poll(&ev) && ev.nav == INPUT_NAV_BACK, "esc back");
    CHECK(input_window_message(NULL, WM_KEYDOWN, VK_F11, 0) == 0, "F11 must reach the window");
    input_window_message(NULL, WM_MOUSEWHEEL, (uintptr_t)(WHEEL_DELTA << 16), 0);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_WHEEL && ev.delta == 1 && ev.source == INPUT_SRC_MOUSE, "wheel up (pc-menu2: scrolls)");
    input_window_message(NULL, WM_MOUSEWHEEL, (uintptr_t)(uint16_t)(-WHEEL_DELTA / 2) << 16, 0);   /* (half notches add up) */
    CHECK(!input_nav_poll(&ev), "half a notch: nothing yet");
    input_window_message(NULL, WM_MOUSEWHEEL, (uintptr_t)(uint16_t)(-WHEEL_DELTA / 2) << 16, 0);
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_WHEEL && ev.delta == -1, "two halves: one notch down");
    input_window_message(NULL, WM_LBUTTONUP, 0, MAKELPARAM(7, 8));
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_RELEASE && ev.x == 7 && ev.y == 8, "button up (the end of a drag)");
    input_window_message(NULL, WM_LBUTTONDOWN, 0, MAKELPARAM(120, 45));
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_CLICK && ev.x == 120 && ev.y == 45, "click");
    input_window_message(NULL, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    input_window_message(NULL, WM_MOUSEMOVE, 0, MAKELPARAM(11, 12));
    CHECK(input_nav_poll(&ev) && ev.nav == INPUT_NAV_POINTER && ev.x == 11 && ev.y == 12 && !input_nav_poll(&ev), "pointer coalesced");
    input_capture_begin(INPUT_CAPTURE_KEYBOARD);
    CHECK(input_window_message(NULL, WM_KEYDOWN, VK_F5, 0) == 1 && input_capture_poll(&code) == INPUT_CAPTURE_PENDING, "F5 not bindable");
    input_window_message(NULL, WM_KEYDOWN, 'G', 0);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_KEY && code == 'G', "captured key %d", code);
    input_window_message(NULL, WM_KEYDOWN, 'G', 1 << 30);    /* still held: no nav from it */
    CHECK(!input_nav_poll(&ev), "captured key navigates");
    input_window_message(NULL, WM_KEYUP, 'G', 0);
    input_capture_begin(INPUT_CAPTURE_KEYBOARD);
    input_window_message(NULL, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_CANCELLED, "esc cancels capture");
    input_capture_begin(INPUT_CAPTURE_KEYBOARD);
    input_window_message(NULL, WM_RBUTTONDOWN, 0, 0);
    CHECK(input_capture_poll(&code) == INPUT_CAPTURE_KEY && code == VK_RBUTTON, "mouse button capture");
    input_set_menu_active(0);
    input_window_message(NULL, WM_KEYDOWN, VK_DOWN, 0);
    CHECK(!input_nav_poll(&ev), "menu closed: arrows are the game's");
    CHECK(input_window_message(NULL, WM_KEYDOWN, VK_ESCAPE, 0) == 0 && input_nav_poll(&ev) && ev.nav == INPUT_NAV_MENU,
          "esc with the menu closed -> MENU");

    input_dev_remove(a);
    input_dev_remove(b);
    CHECK(input_devices(info, INPUT_MAX_DEVICES) == 0, "table empty");
}

static void test_ports_pure(void) {
    input_port_dev d[3] = { { 1, -1, 0, 0 }, { 2, -1, 0, 0 }, { 3, -1, 0, 0 } };
    input_assign_ports(d, 3, 1000);
    CHECK(d[0].port == -1 && d[1].port == -1 && d[2].port == -1, "nobody active");
    d[1].active = 1;
    input_assign_ports(d, 3, 1000);
    CHECK(d[1].port == 0, "first active");
    d[1].active = 0; d[0].active = 1;
    input_assign_ports(d, 3, 1500);
    CHECK(d[0].port == 1 && d[1].port == 0, "second active -> 1");
    d[0].active = 0; d[2].active = 1;
    input_assign_ports(d, 3, 2000);
    CHECK(d[2].port == -1, "third: no port while the owner is recent");
    input_assign_ports(d, 3, 1000 + INPUT_TAKEOVER_MS);
    CHECK(d[2].port == 0 && d[1].port == -1, "takeover after %d ms idle: %d %d", INPUT_TAKEOVER_MS, d[2].port, d[1].port);
    d[2].active = 0; d[0].active = 1;
    input_assign_ports(d, 3, 1000 + INPUT_TAKEOVER_MS + 10);
    CHECK(d[0].port == 1, "port 1 owner stays while the port-0 owner is recent");
    d[0].active = 0;
    input_assign_ports(d, 3, 1000 + 3 * INPUT_TAKEOVER_MS);
    d[0].active = 1;
    input_assign_ports(d, 3, 1000 + 3 * INPUT_TAKEOVER_MS + 1);
    CHECK(d[0].port == 0 && d[2].port == 1, "swap: idle owner moves to the freed port 1 (%d %d)", d[0].port, d[2].port);
}

static void test_xinput_hotplug(void) {
    memset(xi_present, 0, sizeof xi_present);
    xi_present[2] = 1;
    xi_state[2].Gamepad.wButtons = XINPUT_GAMEPAD_A;
    int found = 0;
    for (int t = 0; t < 12 && !found; t++) {                  /* one empty slot probed per 500 ms */
        input_service();
        input_device_info info[INPUT_MAX_DEVICES];
        found = input_devices(info, INPUT_MAX_DEVICES) == 1 && info[0].kind == INPUT_DEV_XINPUT;
        if (!found) Sleep(260);
    }
    CHECK(found, "XInput controller not found by probing");
    pad_input p;
    input_get_pad(0, &p);
    CHECK(p.buttons == PAD_CROSS, "xinput drives port 0: %04X", p.buttons);
    input_set_rumble(0, 1, 0x40);
    input_service();
    CHECK(xi_sets[2] >= 1 && xi_vib[2].wRightMotorSpeed == 0xFFFF && xi_vib[2].wLeftMotorSpeed == 0x40 * 257,
          "xinput rumble %u %u", xi_vib[2].wLeftMotorSpeed, xi_vib[2].wRightMotorSpeed);
    int sets = xi_sets[2];
    input_set_rumble(0, 1, 0x40);
    input_service();
    CHECK(xi_sets[2] == sets, "unchanged rumble sent again");
    xi_present[2] = 0;                                        /* unplugged */
    input_get_pad(0, &p);
    CHECK(is_cleared(&p), "unplugged xinput still drives");
    input_device_info info[INPUT_MAX_DEVICES];
    CHECK(input_devices(info, INPUT_MAX_DEVICES) == 0, "xinput not removed");
}

static void test_config_file(const char* dir) {
    input_config c;
    input_config_defaults(&c);
    c.vibration_strength = 70;
    c.invert_camera_x = 1;
    c.key[INPUT_ACT_CROSS][0] = 'J'; c.key[INPUT_ACT_CROSS][1] = 0;
    c.pad[INPUT_ACT_CROSS][0] = INPUT_BTN_SQUARE;
    CHECK(input_config_set(&c, 1), "save");
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\sly2.ini", dir);
    FILE* f = fopen(path, "rb");
    static char text[65536];
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    CHECK(strstr(text, "[controls]") && strstr(text, "vibration_strength = 70") && strstr(text, "key_cross = J") &&
          strstr(text, "pad_cross = square") && strstr(text, "invert_camera_x = on"), "sly2.ini:\n%s", text);
    input_config_defaults(&c);
    input_config_set(&c, 0);
    input_config_reload();
    input_config_get(&c);
    CHECK(c.vibration_strength == 70 && c.invert_camera_x && c.key[INPUT_ACT_CROSS][0] == 'J' &&
          c.pad[INPUT_ACT_CROSS][0] == INPUT_BTN_SQUARE, "reload from sly2.ini");
    ps2_settings_t s;
    settings_defaults(&s);
    CHECK(settings_parse(text, &s, NULL, NULL) == 0, "settings.c complains about [controls]");
    input_config_defaults(&c);
    input_config_set(&c, 1);
}

int main(void) {
    char dir[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    char sub[MAX_PATH];
    snprintf(sub, sizeof sub, "%ssly2_input_test_%lu", dir, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(sub, NULL);
    SetEnvironmentVariableA("PS2_USER_DIR", sub);
    _putenv_s("PS2_USER_DIR", sub);
    input_xinput_override(fake_xi_get, fake_xi_set);

    test_crc();
    test_dualsense_input();
    test_ds4_input();
    test_outputs();
    test_state_mapping();
    test_xinput_mapping();
    test_keyboard_mapping();
    test_generic();
    test_config();
    test_section_parse();
    test_ports_pure();
    input_init();
    test_devices();
    test_xinput_hotplug();
    test_config_file(sub);

    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%s\\sly2.ini", sub);
    DeleteFileA(ini);
    RemoveDirectoryA(sub);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
