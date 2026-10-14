/* Tests for runtime/window.c.
 *
 * 1. Unit checks (no window): 4:3 letterbox maths, XInput -> DualShock 2 mapping (buttons,
 *    triggers with pressure, sticks with dead zone), keyboard mapping, and the triple-buffer frame
 *    hand-off (ordering, cropping, bad input, and a two-thread stress test for torn frames).
 * 2. Presenter checks (small test windows of their own, shown without activation): the GDI
 *    presenter paints every letterbox bar black, including one-pixel bars; the Direct3D 11
 *    presenter recovers from a failed ResizeBuffers when the window returns to its old size.
 * 3. Interactive run: opens the window, presents moving test patterns at 60 fps for ~3 s while the
 *    frame size changes, toggles borderless fullscreen (F11) on and off, checks the presented
 *    picture through PrintWindow (letterbox bars black, image colour exact), checks the title's
 *    FPS value (also the first one, with frames presented while the window was coming up),
 *    prints input state and timing, then exits.
 *
 * The implementation is #included so its internal helpers can be tested directly.
 *
 * Usage: test_window.exe [--unit-only] [--no-fullscreen] [--seconds N]
 *        (PS2_WINDOW_GDI=1 in the environment tests the GDI presenter)
 */
#include "window.c"

#include <stdlib.h>

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

static int is_cleared(const pad_input* p) {
    pad_input c;
    pad_clear(&c);
    return memcmp(&c, p, sizeof c) == 0;
}

/* ---- unit checks ---------------------------------------------------------------------------- */

static void test_letterbox(void) {
    wrect r = letterbox(1280, 960);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1280 && r.h == 960, "%d %d %d %d", r.x, r.y, r.w, r.h);
    r = letterbox(1920, 1080);
    CHECK(r.x == 240 && r.y == 0 && r.w == 1440 && r.h == 1080, "%d %d %d %d", r.x, r.y, r.w, r.h);
    r = letterbox(800, 1000);
    CHECK(r.x == 0 && r.y == 200 && r.w == 800 && r.h == 600, "%d %d %d %d", r.x, r.y, r.w, r.h);
    r = letterbox(0, 100);
    CHECK(r.w == 0 && r.h == 0, "%d %d", r.w, r.h);
    int bad = 0;
    for (int cw = 1; cw < 4000 && !bad; cw += 7)
        for (int ch = 1; ch < 3000 && !bad; ch += 11) {
            r = letterbox(cw, ch);
            int fits = r.x >= 0 && r.y >= 0 && r.x + r.w <= cw && r.y + r.h <= ch;
            int full = r.w == cw || r.h == ch;
            int centred = abs(r.x - (cw - r.x - r.w)) <= 1 && abs(r.y - (ch - r.y - r.h)) <= 1;
            int aspect = llabs((long long)r.w * 3 - (long long)r.h * 4) <= 4;
            if (!(fits && full && centred && aspect)) {
                bad = 1;
                CHECK(0, "letterbox(%d,%d) = %d %d %d %d", cw, ch, r.x, r.y, r.w, r.h);
            }
        }
}

static void test_xinput_mapping(void) {
    XINPUT_GAMEPAD g;
    pad_input p;
    memset(&g, 0, sizeof g);
    pad_clear(&p);
    xi_to_pad(&g, &p);
    CHECK(is_cleared(&p), "idle pad not cleared");

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
        memset(&g, 0, sizeof g);
        g.wButtons = one[i].xi;
        pad_clear(&p);
        xi_to_pad(&g, &p);
        CHECK(p.buttons == one[i].pad, "xinput %04X -> %04X, want %04X", one[i].xi, p.buttons, one[i].pad);
        for (int k = 0; k < 12; k++)
            CHECK(p.pressure[k] == (k == one[i].pr ? 0xFF : 0), "xinput %04X pressure[%d] = %02X", one[i].xi, k, p.pressure[k]);
        CHECK(p.lx == 0x80 && p.ly == 0x80 && p.rx == 0x80 && p.ry == 0x80, "sticks moved");
    }

    memset(&g, 0, sizeof g);
    g.bLeftTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;          /* at the threshold: released */
    g.bRightTrigger = 200;
    pad_clear(&p);
    xi_to_pad(&g, &p);
    CHECK(p.buttons == PAD_R2 && p.pressure[10] == 0 && p.pressure[11] == 200, "%04X %02X %02X", p.buttons, p.pressure[10], p.pressure[11]);
    g.bLeftTrigger = 255;
    pad_clear(&p);
    xi_to_pad(&g, &p);
    CHECK(p.buttons == (PAD_L2 | PAD_R2) && p.pressure[10] == 255, "%04X %02X", p.buttons, p.pressure[10]);

    static const struct { SHORT lx, ly, rx, ry; uint8_t elx, ely, erx, ery; } sticks[] = {
        { 0, 0, 0, 0, 0x80, 0x80, 0x80, 0x80 },
        { 32767, 0, 0, 0, 0xFF, 0x80, 0x80, 0x80 },
        { -32768, 0, 0, 0, 0x00, 0x80, 0x80, 0x80 },
        { 0, 32767, 0, 0, 0x80, 0x00, 0x80, 0x80 },              /* up = smaller */
        { 0, -32768, 0, 0, 0x80, 0xFF, 0x80, 0x80 },             /* down = larger */
        { 0, 0, 32767, 0, 0x80, 0x80, 0xFF, 0x80 },
        { 0, 0, -32768, 0, 0x80, 0x80, 0x00, 0x80 },
        { 0, 0, 0, -32768, 0x80, 0x80, 0x80, 0xFF },
        { 0, 0, 0, 32767, 0x80, 0x80, 0x80, 0x00 },
        { 0, 0, 32767, -32768, 0x80, 0x80, 0xDA, 0xDA },         /* diagonal: on the unit circle */
        { 5000, -5000, 8000, 3000, 0x80, 0x80, 0x80, 0x80 },     /* inside the dead zones */
    };
    for (size_t i = 0; i < sizeof sticks / sizeof sticks[0]; i++) {
        memset(&g, 0, sizeof g);
        g.sThumbLX = sticks[i].lx; g.sThumbLY = sticks[i].ly; g.sThumbRX = sticks[i].rx; g.sThumbRY = sticks[i].ry;
        pad_clear(&p);
        xi_to_pad(&g, &p);
        CHECK(p.lx == sticks[i].elx && p.ly == sticks[i].ely && p.rx == sticks[i].erx && p.ry == sticks[i].ery,
              "stick case %zu: %02X %02X %02X %02X", i, p.lx, p.ly, p.rx, p.ry);
    }
    /* Diagonal full tilt: on the unit circle (about 0x80 +- 0x5A), up-right. */
    memset(&g, 0, sizeof g);
    g.sThumbLX = 23170; g.sThumbLY = 23170;
    pad_clear(&p);
    xi_to_pad(&g, &p);
    CHECK(p.lx >= 0xD6 && p.lx <= 0xDC && p.ly >= 0x24 && p.ly <= 0x2A, "diagonal %02X %02X", p.lx, p.ly);
    /* Monotonic in tilt, and just past the dead zone stays near the centre. */
    int prev = 0x80, mono = 1;
    for (int v = 0; v <= 32767; v += 97) {
        uint8_t x, y;
        xi_stick(v, 0, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &x, &y);
        if (x < prev || y != 0x80) mono = 0;
        prev = x;
    }
    CHECK(mono && prev == 0xFF, "stick ramp not monotonic (end %02X)", prev);
    uint8_t x, y;
    xi_stick(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 300, 0, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &x, &y);
    CHECK(x >= 0x80 && x <= 0x82, "just past dead zone: %02X", x);
}

static void test_keyboard_mapping(void) {
    uint8_t down[256];
    pad_input p;
    memset(down, 0, sizeof down);
    pad_clear(&p);
    kb_to_pad(down, &p);
    CHECK(is_cleared(&p), "no keys -> not cleared");

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
        pad_clear(&p);
        kb_to_pad(down, &p);
        all |= p.buttons;
        CHECK(p.buttons == one[i].pad, "key %02X -> %04X, want %04X", one[i].vk, p.buttons, one[i].pad);
        for (int k = 0; k < 12; k++)
            CHECK(p.pressure[k] == (k == one[i].pr ? 0xFF : 0), "key %02X pressure[%d] = %02X", one[i].vk, k, p.pressure[k]);
    }
    CHECK(all == 0xFFFF, "keyboard reaches buttons %04X, want FFFF", all);
    memset(down, 0, sizeof down);
    down[VK_CONTROL] = down[VK_RCONTROL] = 1;              /* right Ctrl is not mapped */
    pad_clear(&p);
    kb_to_pad(down, &p);
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
        pad_clear(&p);
        kb_to_pad(down, &p);
        CHECK(p.buttons == 0 && p.lx == st[i].lx && p.ly == st[i].ly && p.rx == st[i].rx && p.ry == st[i].ry,
              "keys %s: %04X %02X %02X %02X %02X", st[i].keys, p.buttons, p.lx, p.ly, p.rx, p.ry);
    }
    /* Keys merge with a controller: buttons OR'd, a key-driven axis overrides, others untouched. */
    pad_clear(&p);
    p.buttons = PAD_CIRCLE; p.lx = 0x30; p.ly = 0x40; p.rx = 0x50;
    memset(down, 0, sizeof down);
    down['D'] = 1; down[VK_SPACE] = 1;
    kb_to_pad(down, &p);
    CHECK(p.buttons == (PAD_CIRCLE | PAD_CROSS) && p.lx == 0xFF && p.ly == 0x40 && p.rx == 0x50,
          "merge: %04X %02X %02X %02X", p.buttons, p.lx, p.ly, p.rx);
}

static uint32_t src[FRAME_MAX * 2 * 16];

static void test_frame_handoff_basic(void) {
    CHECK(fb_setup(), "fb_setup");
    InterlockedExchange(&win_accepting, 1);
    while (fb_take()) {}
    CHECK(!fb_take(), "spurious frame");

    for (int i = 0; i < 8 * 3; i++) src[i] = 0xA0000000u | (uint32_t)i;
    window_present(src, 4, 3, 8);                        /* stride > width */
    CHECK(fb_take(), "frame A not delivered");
    CHECK(fb_w[fb_read] == 4 && fb_h[fb_read] == 3, "A size %dx%d", fb_w[fb_read], fb_h[fb_read]);
    int ok = 1;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++) ok &= fb_pix[fb_read][y * 4 + x] == (0xA0000000u | (uint32_t)(y * 8 + x));
    CHECK(ok, "frame A rows copied wrongly");
    CHECK(!fb_take(), "frame A delivered twice");

    for (int i = 0; i < 16; i++) src[i] = 0xB0000000u | (uint32_t)i;
    window_present(src, 5, 2, 5);
    for (int i = 0; i < 16; i++) src[i] = 0xC0000000u | (uint32_t)i;
    window_present(src, 6, 1, 6);
    CHECK(fb_take(), "frame C not delivered");
    CHECK(fb_w[fb_read] == 6 && fb_h[fb_read] == 1 && fb_pix[fb_read][5] == 0xC0000005u, "not the newest frame");
    CHECK(!fb_take(), "stale frame B delivered after C");

    printf("(two 'bad frame' / 'cropped' log lines are expected next)\n");
    fflush(stdout);
    window_present(NULL, 4, 4, 4);
    window_present(src, 8, 1, 4);                        /* stride < width */
    window_present(src, 0, 1, 4);
    CHECK(!fb_take(), "invalid frame delivered");

    for (int i = 0; i < FRAME_MAX * 2 * 2; i++) src[i] = (uint32_t)i;
    window_present(src, FRAME_MAX * 2, 2, FRAME_MAX * 2);  /* too wide: cropped */
    CHECK(fb_take() && fb_w[fb_read] == FRAME_MAX && fb_h[fb_read] == 2 &&
          fb_pix[fb_read][FRAME_MAX] == (uint32_t)(FRAME_MAX * 2), "crop: %dx%d", fb_w[fb_read], fb_h[fb_read]);
    InterlockedExchange(&win_accepting, 0);
    window_present(src, 4, 4, 4);
    CHECK(!fb_take(), "frame accepted while not initialised");
}

/* Producer thread presents frames whose every pixel holds the frame number; the consumer checks
 * that no taken frame is torn and that frame numbers never go backwards. */
#define STRESS_W 64
#define STRESS_H 48
static volatile LONG stress_stop;
static DWORD WINAPI stress_producer(LPVOID arg) {
    (void)arg;
    static uint32_t frame[STRESS_W * STRESS_H];
    for (uint32_t n = 1; !stress_stop; n++) {
        for (int i = 0; i < STRESS_W * STRESS_H; i++) frame[i] = n;
        window_present(frame, STRESS_W, STRESS_H, STRESS_W);
    }
    return 0;
}

static void test_frame_handoff_stress(void) {
    InterlockedExchange(&win_accepting, 1);
    while (fb_take()) {}
    stress_stop = 0;
    HANDLE t = CreateThread(NULL, 0, stress_producer, NULL, 0, NULL);
    uint32_t last = 0;
    int taken = 0, torn = 0, backwards = 0;
    ULONGLONG end = GetTickCount64() + 300;
    while (GetTickCount64() < end) {
        if (!fb_take()) { YieldProcessor(); continue; }
        const uint32_t* px = fb_pix[fb_read];
        uint32_t n = px[0];
        for (int i = 1; i < STRESS_W * STRESS_H; i++) if (px[i] != n) { torn++; break; }
        if (n <= last) backwards++;
        last = n;
        taken++;
    }
    stress_stop = 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    while (fb_take()) {}
    InterlockedExchange(&win_accepting, 0);
    printf("hand-off stress: %d frames taken, %u presented\n", taken, last);
    CHECK(taken > 100 && torn == 0 && backwards == 0, "taken %d torn %d backwards %d", taken, torn, backwards);
}

static void test_headless_input(void) {
    pad_input p;
    window_get_input(2, &p);
    CHECK(is_cleared(&p), "port 2 not cleared");
    window_get_input(-1, &p);
    CHECK(is_cleared(&p), "port -1 not cleared");
    window_get_input(1, &p);                             /* no window yet: keyboard ignored */
    int slots = 0;
    for (int s = 0; s < XUSER_MAX_COUNT; s++) slots += xi_connected[s] != 0;
    printf("XInput: %s, %d controller(s) connected\n", xi_get_state ? "loaded" : "unavailable", slots);
    if (slots < 2) CHECK(is_cleared(&p), "absent port 1 not cleared");
}

/* ---- PS2_INPUT scripts: absolute, relative (+n) and function-anchored (@addr+n) tokens --------- */

static uint32_t fake_anchor_calls;
uint32_t ps2_func_calls(uint32_t addr, uint32_t ra) {
    return addr == 0x187D80 && ra == 0x1DD96C ? fake_anchor_calls : 0;
}
uint32_t ps2_vblank_count(void) { return 0; }
static int ram_dumps;
void ps2_request_ram_dump(void) { ram_dumps++; }

static void test_input_script(void) {
    script_n = 0;
    script_frame = 0;
    script_parse("10:cross:2,+5:square:1,@187d80/1dd96c+3:start:2,+1:lup:1,7:bogus");
    CHECK(script_n == 4, "parsed %d tokens", script_n);
    uint16_t buttons[40];
    uint8_t ly[40];
    for (int f = 0; f < 40; f++) {
        if (f == 25) fake_anchor_calls++;                 /* the anchor function runs before frame 25 */
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        buttons[f] = p.buttons;
        ly[f] = p.ly;
    }
    for (int f = 0; f < 40; f++) {
        CHECK(!!(buttons[f] & PAD_CROSS) == (f == 10 || f == 11), "cross at frame %d", f);
        CHECK(!!(buttons[f] & PAD_SQUARE) == (f == 15), "square at frame %d", f);
        CHECK(!!(buttons[f] & PAD_START) == (f == 28 || f == 29), "start at frame %d", f);
        CHECK((ly[f] == 0) == (f == 29), "left stick up at frame %d (ly %02X)", f, ly[f]);
    }
    /* quiet anchor: fires once the function was called and then stayed silent for 5 frames */
    script_n = 0;
    script_frame = 0;
    fake_anchor_calls = 0;
    script_parse("@!187d80/1dd96c+5:none,+2:circle:1");
    CHECK(script_n == 2, "parsed %d tokens", script_n);
    for (int f = 0; f < 40; f++) {
        if (f >= 10 && f < 20) fake_anchor_calls++;       /* called on frames 10..19, silent after */
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        CHECK(!!(p.buttons & PAD_CIRCLE) == (f == 26), "circle at frame %d", f);
    }
    /* partial stick deflection and the RAM dump request */
    script_n = 0;
    script_frame = 0;
    ram_dumps = 0;
    script_parse("2:lx40:3,+1:ryC0:1,+0:RAM");          /* upper case works too */
    CHECK(script_n == 3, "parsed %d tokens", script_n);
    for (int f = 0; f < 8; f++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        CHECK((p.lx == 0x40) == (f >= 2 && f < 5), "lx at frame %d: %02X", f, p.lx);
        CHECK((p.ry == 0xC0) == (f == 3), "ry at frame %d: %02X", f, p.ry);
    }
    CHECK(ram_dumps == 1, "%d RAM dumps", ram_dumps);
    /* "release" ends the holds in progress (r1, lx) but not a press that starts with it */
    script_n = 0;
    script_frame = 0;
    script_parse("1:r1:1000,1:lx20:1000,6:release,6:cross:2,+3:r1:2");
    CHECK(script_n == 5, "parsed %d tokens", script_n);
    for (int f = 0; f < 14; f++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        CHECK(!!(p.buttons & PAD_R1) == ((f >= 1 && f < 6) || f == 9 || f == 10), "r1 at frame %d", f);
        CHECK((p.lx == 0x20) == (f >= 1 && f < 6), "lx at frame %d: %02X", f, p.lx);
        CHECK(!!(p.buttons & PAD_CROSS) == (f == 6 || f == 7), "cross at frame %d", f);
    }
    script_n = 0;                                         /* leave scripting off for the other tests */
}

/* PS2_INPUT_FILE live input: a line longer than any fixed buffer (24 KB), two lines appended at once
 * (each starts at the moment it is read), and more tokens over a session than the script holds at a
 * time (finished ones are dropped when a line arrives). */
static void append_line(const char* path, const char* pre, int fillers, const char* post) {
    FILE* f = fopen(path, "ab");
    if (!f) return;
    fputs(pre, f);
    for (int i = 0; i < fillers; i++) fputs("+0:none,", f);
    fprintf(f, "%s\r\n", post);
    fclose(f);
}

static void test_live_input(void) {
    char path[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, path);
    snprintf(path + n, sizeof path - n, "sly2_live_input_%lu.txt", GetCurrentProcessId());
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL, "cannot create %s", path);
    if (!f) return;
    fclose(f);
    script_path = path;
    script_offset = 0;
    script_n = 0;
    script_frame = 0;
    append_line(path, "", 3000, "+5:circle:1");           /* read at frame 0 (lines are read every 8) */
    int circle_at = -1, pad_frames = 0;
    for (int f = 0; f < 16; f++, pad_frames++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        if (p.buttons & PAD_CIRCLE) { CHECK(circle_at < 0, "circle twice"); circle_at = f; }
    }
    CHECK(circle_at == 5, "long line: circle at frame %d (want 5)", circle_at);
    append_line(path, "+2:cross:1,", 0, "+0:none");       /* two lines at once: read at frame 16 */
    append_line(path, "+3:square:1,", 0, "+0:none");
    int cross_at = -1, square_at = -1;
    for (int f = 16; f < 24; f++, pad_frames++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        if (p.buttons & PAD_CROSS) cross_at = f;
        if (p.buttons & PAD_SQUARE) square_at = f;
    }
    CHECK(cross_at == 18 && square_at == 19, "two lines: cross at %d (want 18), square at %d (want 19)",
          cross_at, square_at);
    int sent = 0, seen = 0;                               /* 10 lines of 3000 tokens: > SCRIPT_MAX in all */
    for (int k = 0; k < 10; k++) {
        append_line(path, "", 3000, "+1:triangle:1");
        sent += 3002;
        for (int f = 0; f < 8; f++, pad_frames++) {
            pad_input p;
            pad_clear(&p);
            script_apply(0, &p);
            if (p.buttons & PAD_TRIANGLE) seen++;
        }
    }
    CHECK(sent > SCRIPT_MAX && seen == 10, "%d tokens sent, triangle seen %d times (want 10), %d pending",
          sent, seen, script_n);
    CHECK(script_n < 3100, "%d tokens still held", script_n);
    script_path = NULL;                                   /* leave scripting off for the other tests */
    script_n = 0;
    remove(path);
}

/* "pause": the pad read after the pause token blocks until the next line arrives, and that line's
 * "+0" tokens apply in the same pad frame. */
static volatile LONG line_appended, held_while_waiting;
static volatile LONG timer_held, timer_holds;
void ps2_debug_hold(int on) { InterlockedExchange(&timer_held, on); if (on) InterlockedIncrement(&timer_holds); }
static DWORD WINAPI append_later(LPVOID path) {
    Sleep(150);
    InterlockedExchange(&held_while_waiting, timer_held);  /* EE time stands still during the wait */
    append_line((const char*)path, "+0:cross:1,", 0, "+0:none");
    InterlockedExchange(&line_appended, 1);
    return 0;
}

static void test_live_pause(void) {
    char path[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, path);
    snprintf(path + n, sizeof path - n, "sly2_live_pause_%lu.txt", GetCurrentProcessId());
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL, "cannot create %s", path);
    if (!f) return;
    fclose(f);
    script_path = path;
    script_offset = 0;
    script_n = 0;
    script_frame = 0;
    append_line(path, "+2:pause,", 0, "+0:none");       /* read at frame 0, pause from frame 3 */
    int blocked_at = -1, cross_at = -1;
    HANDLE t = NULL;
    line_appended = 0;
    for (int fr = 0; fr < 8; fr++) {
        if (fr == 3) t = CreateThread(NULL, 0, append_later, path, 0, NULL);
        LONG before = line_appended;                      /* a call that waited for the line */
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        if (!before && line_appended) blocked_at = fr;
        if (p.buttons & PAD_CROSS) cross_at = fr;
    }
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
    CHECK(blocked_at == 3 && cross_at == 3, "pause: blocked at frame %d, cross at %d (want 3, 3)",
          blocked_at, cross_at);
    CHECK(held_while_waiting == 1 && timer_held == 0 && timer_holds == 1,
          "pause: PS2 time held while waiting %ld, held after %ld, holds %ld (want 1, 0, 1)",
          (long)held_while_waiting, (long)timer_held, (long)timer_holds);
    script_path = NULL;
    script_n = 0;
    remove(path);
}

/* ---- presenter checks: the GDI and Direct3D 11 presenters drawing into a test window of their own
 * (shown without activation, before window_init, so the window thread is not involved) ---------- */

static LRESULT CALLBACK test_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND test_window(int cw, int ch) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = test_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"Sly2WindowTest";
    RegisterClassW(&wc);
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Sly2WindowTest", L"window test",
                             WS_OVERLAPPEDWINDOW, 40, 40, r.right - r.left, r.bottom - r.top, NULL, NULL,
                             wc.hInstance, NULL);
    if (h) ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

static void set_client_size(HWND h, int cw, int ch);
static int near_rgb(uint32_t a, uint32_t b, int tol);

/* Copy the client area into a top-down 32-bit buffer. GDI content is read through the window DC;
 * flip-model Direct3D content only through PrintWindow(PW_RENDERFULLCONTENT). */
static int read_client(HWND h, uint32_t* buf, int cw, int ch, int print) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC wdc = GetDC(h), mem = CreateCompatibleDC(wdc);
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int ok = 0;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        memset(bits, 0x5A, (size_t)cw * (size_t)ch * 4);
        ok = print ? PrintWindow(h, mem, 1 | 2) : BitBlt(mem, 0, 0, cw, ch, wdc, 0, 0, SRCCOPY);
        GdiFlush();
        for (size_t i = 0; i < (size_t)cw * (size_t)ch; i++) buf[i] = ((const uint32_t*)bits)[i] & 0xFFFFFFu;
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(h, wdc);
    return ok;
}

static uint32_t shot[1200 * 1000];
static uint32_t solid[640 * 480];

/* GDI: every client pixel outside the 4:3 picture must be painted black, including a bar only one
 * pixel wide (the client is first filled white so an unpainted bar shows). */
static void test_gdi_bars(void) {
    static const int sizes[][2] = { { 700, 480 }, { 640, 600 }, { 641, 480 }, { 642, 481 }, { 640, 481 }, { 801, 600 } };
    HWND h = test_window(700, 600);
    CHECK(h != NULL, "test window");
    if (!h) return;
    for (int i = 0; i < 320 * 224; i++) solid[i] = 0x7F2060C0u;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        set_client_size(h, sizes[k][0], sizes[k][1]);
        RECT rc;
        GetClientRect(h, &rc);
        int cw = rc.right, ch = rc.bottom;
        HDC dc = GetDC(h);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        ReleaseDC(h, dc);
        gdi_render(h, solid, 320, 224);
        if (!read_client(h, shot, cw, ch, 0)) { CHECK(0, "cannot read the client area"); break; }
        wrect r = letterbox(cw, ch);
        int unpainted = 0, fx = -1, fy = -1;
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                int in = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
                if (!in && shot[(size_t)y * cw + x] != 0 && unpainted++ == 0) { fx = x; fy = y; }
            }
        uint32_t mid = shot[(size_t)(r.y + r.h / 2) * cw + r.x + r.w / 2];
        CHECK(unpainted == 0, "GDI client %dx%d (picture %d,%d %dx%d): %d bar pixels not black, first at %d,%d = %06X",
              cw, ch, r.x, r.y, r.w, r.h, unpainted, fx, fy, unpainted ? shot[(size_t)fy * cw + fx] : 0);
        CHECK(near_rgb(mid, 0x2060C0, 2), "GDI client %dx%d: picture centre %06X", cw, ch, mid);
    }
    DestroyWindow(h);
}

/* Direct3D 11: a failed ResizeBuffers (forced here by holding a back-buffer reference, as the
 * runtime refuses to resize then) must not leave the presenter without a render target. When the
 * window goes back to the old size, the next frame must be drawn and shown. */
static void test_d3d_resize_failure(void) {
    HWND h = test_window(640, 480);
    CHECK(h != NULL, "test window");
    if (!h) return;
    if (!d3d_init(h)) {
        printf("Direct3D resize check: SKIPPED (no Direct3D 11)\n");
        DestroyWindow(h);
        return;
    }
    for (int i = 0; i < 640 * 480; i++) solid[i] = 0x7FFF0000u;             /* red */
    HRESULT hr = d3d_render(h, solid, 640, 480, 1);
    CHECK(SUCCEEDED(hr) && d3d.bb_w == 640 && d3d.bb_h == 480, "first frame: hr %08lX, back buffer %dx%d",
          (unsigned long)hr, d3d.bb_w, d3d.bb_h);
    ID3D11Texture2D* held = NULL;
    IDXGISwapChain_GetBuffer(d3d.sc, 0, &IID_ID3D11Texture2D_, (void**)&held);
    set_client_size(h, 700, 500);
    hr = d3d_render(h, solid, 640, 480, 1);
    CHECK(FAILED(hr), "resize with a back buffer still referenced succeeded (hr %08lX)", (unsigned long)hr);
    RELEASE(held);
    set_client_size(h, 640, 480);                                           /* back to the old size */
    for (int i = 0; i < 640 * 480; i++) solid[i] = 0x7F00FF00u;             /* green */
    hr = d3d_render(h, solid, 640, 480, 2);
    CHECK(SUCCEEDED(hr) && d3d.rtv != NULL && d3d.bb_w == 640 && d3d.bb_h == 480,
          "frame after a failed resize: hr %08lX, render target %p, back buffer %dx%d", (unsigned long)hr,
          (void*)d3d.rtv, d3d.bb_w, d3d.bb_h);
    hr = d3d_render(h, solid, 640, 480, 2);                                 /* flip model: show it twice */
    Sleep(100);
    if (read_client(h, shot, 640, 480, 1) && shot[240 * 640 + 320] != 0x5A5A5A) {
        uint32_t c = shot[240 * 640 + 320];
        printf("Direct3D resize check: centre after recovery %06X (want 00FF00)\n", c);
        CHECK(near_rgb(c, 0x00FF00, 2), "frame after a failed resize not shown: centre %06X", c);
    } else {
        printf("Direct3D resize check: picture capture unavailable\n");
    }
    d3d_shutdown();
    DestroyWindow(h);
}

/* ---- interactive run -------------------------------------------------------------------------- */

static uint32_t frame[FRAME_MAX * FRAME_MAX];

/* Gradient + checkerboard, a white 1-pixel border, a moving white column and yellow row, and a ring
 * that looks circular when the picture is shown at 4:3. The X byte holds junk on purpose. */
static void pattern(uint32_t* px, int stride, int w, int h, int t) {
    int bx = (t * 6) % w, by = (t * 3) % h;
    float sx = (4.0f / 3.0f) / (float)w, sy = 1.0f / (float)h;   /* pixel -> 4:3 display units */
    for (int y = 0; y < h; y++) {
        uint32_t* row = px + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint32_t c = (uint32_t)(x * 255 / w) << 16 | (uint32_t)(y * 255 / h) << 8 | (uint32_t)((t * 4) & 255);
            if (((x >> 4) ^ (y >> 4)) & 1) c = (c >> 1) & 0x7F7F7Fu;
            float dx = (x - w / 2) * sx, dy = (y - h / 2) * sy, d = dx * dx + dy * dy;
            if (d > 0.30f * 0.30f && d < 0.32f * 0.32f) c = 0x00FF40u;
            if (x >= bx && x < bx + 8) c = 0xFFFFFFu;
            if (y >= by && y < by + 4) c = 0xFFFF00u;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) c = 0xFFFFFFu;
            row[x] = c | 0xAB000000u;
        }
    }
}

static void print_input(int n) {
    pad_input a, b;
    window_get_input(0, &a);
    window_get_input(1, &b);
    printf("[input] frame %3d focus=%d | port0 buttons=%04X L=(%02X,%02X) R=(%02X,%02X) L2p=%02X R2p=%02X"
           " | port1 buttons=%04X L=(%02X,%02X) R=(%02X,%02X)\n",
           n, GetForegroundWindow() == g_hwnd, a.buttons, a.lx, a.ly, a.rx, a.ry, a.pressure[10], a.pressure[11],
           b.buttons, b.lx, b.ly, b.rx, b.ry);
}

static int near_rgb(uint32_t a, uint32_t b, int tol) {
    for (int s = 0; s < 24; s += 8)
        if (abs((int)((a >> s) & 255) - (int)((b >> s) & 255)) > tol) return 0;
    return 1;
}

/* Capture the client area (PrintWindow with PW_RENDERFULLCONTENT also captures Direct3D content)
 * and compare the four quadrants of the letterboxed picture (top-left, top-right, bottom-left,
 * bottom-right: catches channel swaps and flips), a pixel just inside the top-left corner, and a
 * letterbox bar (must be black). Returns 1 = pass, 0 = fail, -1 = capture unavailable. */
static int check_picture(HWND h, const uint32_t quad[4]) {
    RECT rc;
    GetClientRect(h, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return -1;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int result = -1;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        memset(bits, 0x5A, (size_t)cw * (size_t)ch * 4);
        if (PrintWindow(h, mem, 1 /* PW_CLIENTONLY */ | 2 /* PW_RENDERFULLCONTENT */)) {
            GdiFlush();
            const uint32_t* px = bits;
            wrect r = letterbox(cw, ch);
#define AT(x, y) (px[(size_t)(y) * cw + (x)] & 0xFFFFFFu)
            uint32_t got[4] = { AT(r.x + r.w / 4, r.y + r.h / 4), AT(r.x + r.w * 3 / 4, r.y + r.h / 4),
                                AT(r.x + r.w / 4, r.y + r.h * 3 / 4), AT(r.x + r.w * 3 / 4, r.y + r.h * 3 / 4) };
            uint32_t inner = AT(r.x + 1, r.y + 1);
            uint32_t bar = r.x > 0 ? AT(r.x / 2, ch / 2) : AT(cw / 2, r.y / 2);
#undef AT
            printf("picture check: client %dx%d, picture %d,%d %dx%d: quadrants %06X %06X %06X %06X"
                   " (want %06X %06X %06X %06X), corner %06X, bar %06X\n", cw, ch, r.x, r.y, r.w, r.h,
                   got[0], got[1], got[2], got[3], quad[0], quad[1], quad[2], quad[3], inner, bar);
            if (got[0] == 0x5A5A5A && got[3] == 0x5A5A5A) result = -1;   /* nothing captured */
            else {
                result = near_rgb(inner, quad[0], 2) && near_rgb(bar, 0, 2);
                for (int q = 0; q < 4; q++) result &= near_rgb(got[q], quad[q], 2);
            }
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return result;
}

static LARGE_INTEGER qf;
static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qf.QuadPart;
}

static void set_client_size(HWND h, int cw, int ch) {
    RECT wr, cr;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    SetWindowPos(h, NULL, 0, 0, cw + (wr.right - wr.left) - cr.right, ch + (wr.bottom - wr.top) - cr.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void interactive(double seconds, int do_fullscreen) {
    /* Same DPI context as the window thread, so sizes and captures are in real pixels. */
    typedef HANDLE (WINAPI* set_dpi_fn)(HANDLE);
    set_dpi_fn set_dpi = (set_dpi_fn)dll_proc(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (set_dpi && !set_dpi((HANDLE)(LONG_PTR)-4)) set_dpi((HANDLE)(LONG_PTR)-3);

    /* Frames handed over while the window and renderer are still coming up (0.35-0.9 s on a cold
     * start, i.e. 20-55 frames from the emulator) must not inflate the first FPS reading in the
     * title. Present 60 at once right away (the window takes far longer than that to come up), then
     * 60 per second. Calls made by the unit checks above don't count either. */
    InterlockedExchange(&fb_presented, 0);
    double t0 = now_ms(), early_due = t0;
    window_init();
    pattern(frame, FRAME_MAX, 640, 448, 0);
    for (int n = 0; n < 60; n++) window_present(frame, 640, 448, FRAME_MAX);
    int early_burst_done = !g_hwnd;
    for (int n = 0; !g_hwnd && now_ms() - t0 < 5000; n++) {
        pattern(frame, FRAME_MAX, 640, 448, n);
        window_present(frame, 640, 448, FRAME_MAX);
        early_due += 1000.0 / 60.0;
        while (now_ms() < early_due) Sleep(1);
    }
    if (!early_burst_done) printf("note: the window came up before the 60-frame burst ended\n");
    HWND h = g_hwnd;
    CHECK(h != NULL, "window did not come up");
    if (!h) return;
    printf("window up after %.0f ms, presenter: %s\n", now_ms() - t0, use_d3d ? "Direct3D 11" : "GDI");
    RECT cr;
    GetClientRect(h, &cr);
    printf("client area %ldx%ld\n", cr.right, cr.bottom);
    CHECK(labs(cr.right * 3 - cr.bottom * 4) <= 4, "default client area is not 4:3");

    static const int sizes[][2] = { { 640, 448 }, { 512, 448 }, { 640, 480 }, { 320, 240 }, { 720, 480 }, { 256, 224 } };
    int frames = (int)(seconds * 60.0), shown0 = frames_shown;
    double present_total = 0, present_max = 0, input_total = 0;
    int input_calls = 0, fs_seen = 0, fs_restored = 0;
    double first_fps = -1;
    RECT before_fs;
    GetWindowRect(h, &before_fs);
    double start = now_ms();
    for (int i = 0; i < frames; i++) {
        const int* s = sizes[(i / 30) % (sizeof sizes / sizeof sizes[0])];
        pattern(frame, FRAME_MAX, s[0], s[1], i);
        double a = now_ms();
        window_present(frame, s[0], s[1], FRAME_MAX);
        double d = now_ms() - a;
        present_total += d;
        if (d > present_max) present_max = d;
        if (i % 30 == 0) print_input(i);
        if (first_fps < 0 && i % 6 == 3) {               /* the first FPS value the title shows */
            char t[128];
            GetWindowTextA(h, t, sizeof t);
            if (!strncmp(t, "Sly 2 - ", 8) && strncmp(t + 8, "0.0 ", 4)) {
                first_fps = atof(t + 8);
                printf("first title FPS reading: \"%s\" (frame %d)\n", t, i);
            }
        }
        if (i % 30 == 15) {                              /* time the emulation-thread call alone */
            pad_input p;
            a = now_ms();
            for (int k = 0; k < 100; k++) window_get_input(k & 1, &p);
            input_total += now_ms() - a;
            input_calls += 100;
        }
        if (do_fullscreen) {
            if (i == 60) PostMessageW(h, WM_KEYDOWN, VK_F11, 0);
            if (i == 80) {
                MONITORINFO mi;
                mi.cbSize = sizeof mi;
                GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
                RECT wr;
                GetWindowRect(h, &wr);
                fs_seen = (GetWindowLongPtrW(h, GWL_STYLE) & WS_POPUP) && EqualRect(&wr, &mi.rcMonitor);
                printf("fullscreen: %s (window %ld,%ld-%ld,%ld)\n", fs_seen ? "on" : "NOT ON", wr.left, wr.top, wr.right, wr.bottom);
            }
            if (i == 90) PostMessageW(h, WM_SYSKEYDOWN, VK_RETURN, (LPARAM)1 << 29);   /* Alt+Enter */
            if (i == 110) {
                RECT wr;
                GetWindowRect(h, &wr);
                fs_restored = (GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION) == WS_CAPTION && EqualRect(&wr, &before_fs);
                printf("fullscreen off: %s\n", fs_restored ? "restored" : "NOT RESTORED");
            }
        }
        double due = start + (i + 1) * (1000.0 / 60.0);
        while (now_ms() < due - 1.5) Sleep(1);
        while (now_ms() < due) YieldProcessor();
    }
    double elapsed = now_ms() - start;
    int shown = frames_shown - shown0;
    if (do_fullscreen) {
        CHECK(fs_seen, "F11 did not enter borderless fullscreen");
        CHECK(fs_restored, "Alt+Enter did not restore the window");
    }

    char title[128];
    GetWindowTextA(h, title, sizeof title);
    printf("title: \"%s\"\n", title);
    CHECK(strncmp(title, "Sly 2 - ", 8) == 0 && strstr(title, " fps"), "title has no FPS counter");
    double fps = atof(title + 8);
    CHECK(fps > 40 && fps < 75, "title FPS %.1f while presenting at 60", fps);
    CHECK(first_fps > 54 && first_fps < 66, "first title FPS %.1f while presenting at 60 from the start", first_fps);

    /* Picture check at a wide client size: letterbox bars must be black, the image exact. */
    set_client_size(h, 900, 450);
    static const uint32_t quad[4] = { 0x20C060, 0xE04010, 0x1030F0, 0xF0F0F0 };
    for (int i = 0; i < 15; i++) {
        for (int y = 0; y < 448; y++)
            for (int x = 0; x < 640; x++) frame[y * 640 + x] = quad[(y >= 224) * 2 + (x >= 320)] | 0x7F000000u;
        window_present(frame, 640, 448, 640);
        Sleep(16);
    }
    Sleep(50);
    int pic = check_picture(h, quad);
    if (pic < 0) printf("picture check: SKIPPED (capture unavailable)\n");
    else CHECK(pic == 1, "presented picture wrong (see 'picture check' line)");
    set_client_size(h, DEFAULT_CLIENT_W, DEFAULT_CLIENT_H);

    printf("presented %d frames in %.0f ms (%.1f fps); window thread showed %d new frames (%.0f%%)\n", frames,
           elapsed, frames * 1000.0 / elapsed, shown, 100.0 * shown / frames);
    printf("window_present: avg %.3f ms, max %.3f ms; window_get_input: avg %.1f us\n", present_total / frames,
           present_max, input_total * 1000.0 / input_calls);
    CHECK(shown > frames / 4, "window thread showed only %d of %d frames", shown, frames);
    CHECK(present_total / frames < 2.0, "window_present too slow");
}

/* --close-test: closing the window must end the process with exit code 0 (run.ps1 checks it). */
static int close_test(void) {
    window_init();
    for (int i = 0; i < 500 && !g_hwnd; i++) Sleep(10);
    if (!g_hwnd) return 2;
    pattern(frame, FRAME_MAX, 640, 448, 0);
    window_present(frame, 640, 448, FRAME_MAX);
    Sleep(100);
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    Sleep(3000);
    printf("window still open 3 s after WM_CLOSE\n");
    return 3;
}

int main(int argc, char** argv) {
    int unit_only = 0, fullscreen = 1;
    double seconds = 3.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--close-test")) return close_test();
        if (!strcmp(argv[i], "--unit-only")) unit_only = 1;
        else if (!strcmp(argv[i], "--no-fullscreen")) fullscreen = 0;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&qf);
    timeBeginPeriod(1);

    test_letterbox();
    test_xinput_mapping();
    test_keyboard_mapping();
    test_frame_handoff_basic();
    test_frame_handoff_stress();
    test_headless_input();
    test_input_script();
    test_live_input();
    test_live_pause();
    printf("unit checks: %d, failures: %d\n", checks, failures);
    if (!unit_only) {
        typedef HANDLE (WINAPI* set_dpi_fn)(HANDLE);      /* real pixels, like the window thread */
        set_dpi_fn set_dpi = (set_dpi_fn)dll_proc(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
        if (set_dpi && !set_dpi((HANDLE)(LONG_PTR)-4)) set_dpi((HANDLE)(LONG_PTR)-3);
        int before = failures;
        test_gdi_bars();
        test_d3d_resize_failure();
        printf("presenter checks: %s\n", failures == before ? "passed" : "FAILED");
        interactive(seconds < 2.5 ? 2.5 : seconds, fullscreen);
    }

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
