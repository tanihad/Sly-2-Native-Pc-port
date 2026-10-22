/* PlayStation controllers over HID: DualSense (PS5, also the Edge) and DualShock 4 (PS4), USB and
 * Bluetooth. Pure functions: report parsing and output report building (no I/O).
 *
 * The protocol is public: Linux's hid-playstation driver (written by Sony) and SDL's HIDAPI drivers
 * document it; notes/controls.md has the summary. Byte 0 of every report is its id, as Windows'
 * ReadFile/WriteFile on a HID handle have it.
 *
 *  DualSense input   USB 0x01 (64 bytes): state at byte 1. BT 0x31 (78): state at byte 2, CRC-32 of
 *                    0xA1 + bytes 0..73 at 74. BT 0x01 (10, before the full mode is on): the short
 *                    "simple" report, laid out like a DualShock 4's.
 *    state: 0 LX, 1 LY, 2 RX, 3 RY, 4 L2, 5 R2, 6 counter, 7 hat (low nibble: 0 N .. 7 NW, 8 none)
 *           + Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80; 8 L1 0x01, R1 0x02, L2 0x04,
 *           R2 0x08, Create 0x10, Options 0x20, L3 0x40, R3 0x80; 9 PS 0x01, touchpad 0x02, mute
 *           0x04; 52 status: battery 0..10 (low nibble), charging state (high nibble: 1 charging, 2 full).
 *  DualSense output  USB 0x02, the common block at byte 1. BT 0x31: byte 1 = sequence << 4, byte 2 =
 *                    0x10 (tag), the common block at byte 3, CRC-32 of 0xA2 + bytes 0..73 at 74.
 *    common: 0 flags0 (0x01 compatible vibration, 0x02 haptics select), 1 flags1 (0x04 light bar,
 *            0x08 release the LEDs, 0x10 player LEDs), 2 right (high-frequency) motor, 3 left
 *            (low-frequency) motor, 38 flags2 (0x02 light bar setup, 0x04 compatible vibration v2:
 *            firmware 2.24 and later), 41 light bar setup (0x02 = fade the system's light out),
 *            42 LED brightness, 43 player LEDs, 44..46 RGB.
 *  DualShock 4 input  USB 0x01 (64): state at byte 1. BT 0x11 (78): state at byte 3, CRC like the
 *                    DualSense's. BT 0x01: the simple report (state at byte 1, 9 bytes).
 *    state: 0..3 sticks, 4 hat + face buttons (as DualSense byte 7), 5 L1 R1 L2 R2 Share Options L3
 *           R3 (as DualSense byte 8), 6 PS 0x01, touchpad 0x02, 7 L2, 8 R2, 29 status: battery
 *           0..10 (low nibble), cable 0x10.
 *  DualShock 4 output USB 0x05 (32): 1 flags (0x01 motors, 0x02 light bar), 4 right (weak) motor,
 *                    5 left (strong) motor, 6..8 RGB. BT 0x11 (78): 1 = 0xC4 (HID + CRC, 4 ms
 *                    report interval), 2 = 0, the same block from byte 3, CRC at 74.
 */
#include "input_internal.h"

#include <string.h>

int ps_kind(uint16_t vid, uint16_t pid) {
    if (vid != PS_VID) return INPUT_DEV_NONE;
    switch (pid) {
    case PS_PID_DUALSENSE: return INPUT_DEV_DUALSENSE;
    case PS_PID_DUALSENSE_EDGE: return INPUT_DEV_DUALSENSE_EDGE;
    case PS_PID_DS4_V1: case PS_PID_DS4_V2: case PS_PID_DS4_DONGLE: return INPUT_DEV_DS4;
    default: return INPUT_DEV_NONE;
    }
}

uint32_t input_crc32(uint32_t crc, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t ps_bt_crc(uint8_t seed, const uint8_t* report, size_t n) {
    if (n < 4) return 0;
    return input_crc32(input_crc32(0, &seed, 1), report, n - 4);
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Hat (0 N, 1 NE, ... 7 NW, else none) and the PlayStation button bytes shared by both controllers. */
static uint32_t ps_buttons(uint8_t b0, uint8_t b1, uint8_t b2) {
    static const uint8_t hat[8] = { 1, 1 | 8, 8, 8 | 2, 2, 2 | 4, 4, 4 | 1 };   /* up 1, down 2, left 4, right 8 */
    uint32_t b = 0;
    uint8_t h = (b0 & 0x0F) < 8 ? hat[b0 & 0x0F] : 0;
    if (h & 1) b |= 1u << INPUT_BTN_UP;
    if (h & 2) b |= 1u << INPUT_BTN_DOWN;
    if (h & 4) b |= 1u << INPUT_BTN_LEFT;
    if (h & 8) b |= 1u << INPUT_BTN_RIGHT;
    if (b0 & 0x10) b |= 1u << INPUT_BTN_SQUARE;
    if (b0 & 0x20) b |= 1u << INPUT_BTN_CROSS;
    if (b0 & 0x40) b |= 1u << INPUT_BTN_CIRCLE;
    if (b0 & 0x80) b |= 1u << INPUT_BTN_TRIANGLE;
    if (b1 & 0x01) b |= 1u << INPUT_BTN_L1;
    if (b1 & 0x02) b |= 1u << INPUT_BTN_R1;
    if (b1 & 0x04) b |= 1u << INPUT_BTN_L2;
    if (b1 & 0x08) b |= 1u << INPUT_BTN_R2;
    if (b1 & 0x10) b |= 1u << INPUT_BTN_SELECT;               /* Create / Share */
    if (b1 & 0x20) b |= 1u << INPUT_BTN_START;                /* Options */
    if (b1 & 0x40) b |= 1u << INPUT_BTN_L3;
    if (b1 & 0x80) b |= 1u << INPUT_BTN_R3;
    if (b2 & 0x01) b |= 1u << INPUT_BTN_HOME;
    if (b2 & 0x02) b |= 1u << INPUT_BTN_TOUCHPAD;
    if (b2 & 0x04) b |= 1u << INPUT_BTN_MUTE;                 /* (DualSense only; DS4: counter bits) */
    return b;
}

static void ps_sticks(const uint8_t* d, ps_report* out) {
    out->st.lx = input_axis_from_byte(d[0]);
    out->st.ly = input_axis_from_byte(d[1]);
    out->st.rx = input_axis_from_byte(d[2]);
    out->st.ry = input_axis_from_byte(d[3]);
}

static int battery_percent(int level) { int p = level * 10 + 5; return p > 100 ? 100 : p; }

int ps_parse_dualsense(const uint8_t* r, size_t n, int bt, ps_report* out) {
    memset(out, 0, sizeof *out);
    out->battery = -1;
    out->crc_ok = 1;
    if (!r || n < 10) return 0;
    if (r[0] == 0x01 && bt) {                                 /* BT simple report */
        const uint8_t* d = r + 1;
        ps_sticks(d, out);
        out->st.buttons = ps_buttons(d[4], d[5], (uint8_t)(d[6] & 0x03));
        out->st.l2 = d[7];
        out->st.r2 = d[8];
        return 1;
    }
    const uint8_t* d;
    if (r[0] == 0x01 && !bt) {
        if (n < 64) return 0;
        d = r + 1;
    } else if (r[0] == 0x31) {
        if (n < 78) return 0;
        d = r + 2;
        out->crc_ok = ps_bt_crc(0xA1, r, 78) == le32(r + 74);
    } else {
        return 0;
    }
    out->full = 1;
    ps_sticks(d, out);
    out->st.l2 = d[4];
    out->st.r2 = d[5];
    out->st.buttons = ps_buttons(d[7], d[8], d[9]);
    uint8_t status = d[52];
    int state = status >> 4;
    out->charging = state == 1;
    out->battery = state == 2 ? 100 : battery_percent(status & 0x0F);
    if (state > 2) out->battery = -1;                         /* error states (temperature, voltage) */
    return 1;
}

int ps_parse_ds4(const uint8_t* r, size_t n, int bt, ps_report* out) {
    memset(out, 0, sizeof *out);
    out->battery = -1;
    out->crc_ok = 1;
    if (!r || n < 10) return 0;
    const uint8_t* d;
    if (r[0] == 0x01) {
        d = r + 1;
        out->full = !bt && n >= 64;
    } else if (r[0] == 0x11) {
        if (n < 78) return 0;
        d = r + 3;
        out->full = 1;
        out->crc_ok = ps_bt_crc(0xA1, r, 78) == le32(r + 74);
    } else {
        return 0;
    }
    ps_sticks(d, out);
    out->st.buttons = ps_buttons(d[4], d[5], (uint8_t)(d[6] & 0x03));
    out->st.l2 = d[7];
    out->st.r2 = d[8];
    if (out->full) {
        uint8_t status = d[29];
        int level = status & 0x0F, cable = (status & 0x10) != 0;
        out->charging = cable && level < 11;
        out->battery = level >= 10 ? 100 : battery_percent(level);
    }
    return 1;
}

size_t ps_build_dualsense(const ps_output* o, int bt, uint8_t* buf) {
    memset(buf, 0, PS_OUTPUT_MAX);
    uint8_t* c;
    if (bt) {
        buf[0] = 0x31;
        buf[1] = (uint8_t)((o->seq & 0x0F) << 4);
        buf[2] = 0x10;
        c = buf + 3;
    } else {
        buf[0] = 0x02;
        c = buf + 1;
    }
    c[0] = 0x02;                                              /* haptics select: classic rumble */
    if (o->vibration_v2) {
        c[38] |= 0x04;
        c[2] = o->motor_small;
        c[3] = o->motor_large;
    } else {                                                  /* older firmware: half values (as SDL) */
        c[0] |= 0x01;
        c[2] = o->motor_small >> 1;
        c[3] = o->motor_large >> 1;
    }
    if (o->release_leds) {                                    /* fade out the light the system set */
        c[1] |= 0x08;
        c[38] |= 0x02;
        c[41] = 0x02;
    } else {
        c[1] |= 0x04 | 0x10;                                  /* light bar + player LEDs */
        c[43] = o->player_leds;
        if (o->lightbar_on) { c[44] = o->rgb[0]; c[45] = o->rgb[1]; c[46] = o->rgb[2]; }
    }
    if (!bt) return 63;
    uint32_t crc = ps_bt_crc(0xA2, buf, 78);
    buf[74] = (uint8_t)crc; buf[75] = (uint8_t)(crc >> 8); buf[76] = (uint8_t)(crc >> 16); buf[77] = (uint8_t)(crc >> 24);
    return 78;
}

size_t ps_build_ds4(const ps_output* o, int bt, uint8_t* buf) {
    memset(buf, 0, PS_OUTPUT_MAX);
    uint8_t* c;
    if (bt) {
        buf[0] = 0x11;
        buf[1] = 0xC4;                                        /* HID + CRC, 4 ms input report interval */
        buf[2] = 0x00;
        c = buf + 3;
    } else {
        buf[0] = 0x05;
        c = buf + 1;
    }
    c[0] = 0x01 | 0x02;                                       /* motors + light bar */
    c[3] = o->motor_small;
    c[4] = o->motor_large;
    if (o->lightbar_on) { c[5] = o->rgb[0]; c[6] = o->rgb[1]; c[7] = o->rgb[2]; }
    if (!bt) return 32;
    uint32_t crc = ps_bt_crc(0xA2, buf, 78);
    buf[74] = (uint8_t)crc; buf[75] = (uint8_t)(crc >> 8); buf[76] = (uint8_t)(crc >> 16); buf[77] = (uint8_t)(crc >> 24);
    return 78;
}

unsigned ps_dualsense_fw_version(const uint8_t* feat, size_t n) {
    if (!feat || n < 46) return 0;
    return feat[44] | feat[45] << 8;
}
