/* `sly2.exe --pad-test [--seconds N] [--print]`: a small window that lists the controllers the game
 * sees and shows their buttons, sticks, triggers, battery and the DualShock 2 state the game would
 * get, and tests rumble (keys 1 / 2 / 3). No disc needed. --print: no window, print the device list
 * and every change to the console / sly2.log (scripts/pad_test.ps1 uses the window by default). */
#include "input_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static double seconds_limit;
static ULONGLONG t_start;

static const char* kind_name(int k) {
    switch (k) {
    case INPUT_DEV_DUALSENSE: return "DualSense";
    case INPUT_DEV_DUALSENSE_EDGE: return "DualSense Edge";
    case INPUT_DEV_DS4: return "DualShock 4";
    case INPUT_DEV_XINPUT: return "XInput";
    case INPUT_DEV_GENERIC: return "generic HID";
    default: return "?";
    }
}

/* Text for one device (lines separated by \n). */
static int describe(const input_device_info* d, const input_config* c, char* out, size_t cap) {
    int xbox = d->kind == INPUT_DEV_XINPUT;
    int n = snprintf(out, cap, "#%d %s  [%s, %04X:%04X]  %s", d->id, d->name, kind_name(d->kind), d->vid, d->pid,
                     d->port >= 0 ? (d->port == 0 ? "-> PS2 port 0 (the game)" : "-> PS2 port 1") : "(press a button to use it)");
    if (d->battery >= 0) n += snprintf(out + n, cap - (size_t)n, "  battery %d%%%s", d->battery, d->charging ? " charging" : "");
    if (d->kind != INPUT_DEV_XINPUT)
        n += snprintf(out + n, cap - (size_t)n, "\n    reports %lu%s%s", (unsigned long)d->reports,
                      d->bus == INPUT_BUS_BT ? (d->enhanced ? ", full Bluetooth reports" : ", simple Bluetooth reports (no full mode yet)") : "",
                      d->crc_errors ? ", CRC errors!" : "");
    if (d->crc_errors) n += snprintf(out + n, cap - (size_t)n, " (%lu)", (unsigned long)d->crc_errors);
    n += snprintf(out + n, cap - (size_t)n, "\n    pressed:");
    int any = 0;
    for (int b = 0; b < INPUT_BTN_COUNT; b++)
        if (d->state.buttons >> b & 1) { n += snprintf(out + n, cap - (size_t)n, " [%s]", input_button_label(b, xbox)); any = 1; }
    if (!any) n += snprintf(out + n, cap - (size_t)n, " -");
    n += snprintf(out + n, cap - (size_t)n, "\n    left stick %6d %6d   right stick %6d %6d   L2 %3d  R2 %3d   rumble %s small %3d large %3d",
                  d->state.lx, d->state.ly, d->state.rx, d->state.ry, d->state.l2, d->state.r2,
                  d->can_rumble ? "yes" : "no", d->motor_small, d->motor_large);
    pad_input p;
    input_pad_clear(&p);
    input_state_to_pad(&d->state, c, &p);
    input_invert_camera(&p, c);
    n += snprintf(out + n, cap - (size_t)n, "\n    as a DualShock 2: buttons %04X  sticks L %02X %02X  R %02X %02X  pressure L2 %02X R2 %02X Cross %02X",
                  p.buttons, p.lx, p.ly, p.rx, p.ry, p.pressure[10], p.pressure[11], p.pressure[6]);
    return n;
}

static int build_text(char* out, size_t cap) {
    input_device_info d[INPUT_MAX_DEVICES];
    input_config c;
    input_config_get(&c);
    int nd = input_devices(d, INPUT_MAX_DEVICES);
    int n = snprintf(out, cap,
                     "Sly 2 controller test.  Keys: 1 = small motor, 2 = large motor, 3 = both (1 s, every controller), Esc = quit.\n"
                     "Vibration %s %d%%, dead zones %d%% / %d%%, camera inversion X %s Y %s (sly2.ini [controls]).\n\n",
                     c.vibration ? "on" : "off", c.vibration_strength, c.deadzone_left, c.deadzone_right,
                     c.invert_camera_x ? "on" : "off", c.invert_camera_y ? "on" : "off");
    if (!nd) n += snprintf(out + n, cap - (size_t)n, "No controller found. Plug one in (USB) or connect it over Bluetooth; it appears here within a second or two.\n");
    for (int i = 0; i < nd && (size_t)n < cap - 64; i++) {
        n += describe(&d[i], &c, out + n, cap - (size_t)n);
        n += snprintf(out + n, cap - (size_t)n, "\n\n");
    }
    pad_input g;
    input_get_pad(0, &g);
    n += snprintf(out + n, cap - (size_t)n, "The game's port 0 now (controller + keyboard): buttons %04X  sticks L %02X %02X  R %02X %02X\n",
                  g.buttons, g.lx, g.ly, g.rx, g.ry);
    return n;
}

static void rumble_all(uint8_t s, uint8_t l) {
    input_device_info d[INPUT_MAX_DEVICES];
    int nd = input_devices(d, INPUT_MAX_DEVICES);
    for (int i = 0; i < nd; i++) input_test_rumble(d[i].id, s, l, 1000);
    fprintf(stderr, "[padtest] rumble test small %d large %d on %d controller(s)\n", s, l, nd);
}

static void log_notices(void) {
    input_notice n;
    while (input_poll_notice(&n))
        fprintf(stderr, "[padtest] %s: %s\n", n.connected ? "connected" : "disconnected", n.name);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static HFONT font;
    switch (msg) {
    case WM_CREATE:
        font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        SetTimer(h, 1, 33, NULL);
        return 0;
    case WM_TIMER:
        log_notices();
        if (seconds_limit > 0 && (double)(GetTickCount64() - t_start) / 1000.0 >= seconds_limit) DestroyWindow(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        else if (wp == '1') rumble_all(255, 0);
        else if (wp == '2') rumble_all(0, 255);
        else if (wp == '3') rumble_all(255, 255);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(16, 24, 40));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(220, 230, 255));
        static char text[16384];
        build_text(text, sizeof text);
        RECT tr = { 12, 10, rc.right - 12, rc.bottom - 10 };
        DrawTextA(mem, text, -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    if (input_window_message(h, msg, wp, lp)) return 0;
    return DefWindowProcW(h, msg, wp, lp);
}

static int run_print(void) {
    static char last[16384], text[16384];
    while (seconds_limit <= 0 || (double)(GetTickCount64() - t_start) / 1000.0 < seconds_limit) {
        log_notices();
        build_text(text, sizeof text);
        if (strcmp(text, last)) {
            printf("---- %.1f s\n%s", (double)(GetTickCount64() - t_start) / 1000.0, text);
            fflush(stdout);
            strcpy(last, text);
        }
        Sleep(250);
    }
    return 0;
}

int input_pad_test(int argc, char** argv) {
    int print = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds_limit = atof(argv[++i]);
        else if (!strcmp(argv[i], "--print")) print = 1;
    }
    t_start = GetTickCount64();
    fprintf(stderr, "[padtest] controller test%s\n", print ? " (console)" : "");
    input_init();
    int rc = 0;
    if (print) rc = run_print();
    else {
        HINSTANCE inst = GetModuleHandleW(NULL);
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"Sly2PadTest";
        RegisterClassExW(&wc);
        HWND h = CreateWindowExW(0, wc.lpszClassName, L"Sly 2 - controller test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, NULL, NULL, inst, NULL);
        if (!h) { fprintf(stderr, "[padtest] cannot open a window\n"); rc = run_print(); }
        else {
            input_set_window(h);
            MSG m;
            while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
    }
    static char text[16384];
    build_text(text, sizeof text);
    fprintf(stderr, "[padtest] final state:\n%s", text);
    input_shutdown();
    return rc;
}
