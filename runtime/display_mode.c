/* Fullscreen refresh rate (pc-pacing; display_mode.h). */
#include "display_mode.h"
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define CONTENT_HZ (60000.0 / 1001.0)    /* the PS2's VBlank rate */

int display_mode_pick(const int* rates, int n, int desktop_hz, int want) {
    if (want == 0 || n <= 0) return 0;
    if (want > 0) {                      /* a rate the player chose: if the display has it */
        for (int i = 0; i < n; i++) if (rates[i] == want) return want == desktop_hz ? 0 : want;
        return 0;
    }
    /* auto: the desktop's rate if it is a multiple of 59.94 already; else the highest multiple (k = 1..4) the display
     * offers at this resolution, at most the desktop's rate (integer rates: 59 / 60, 119 / 120, 239 / 240 within 1%) */
    for (int k = 1; k <= 4; k++)
        if (fabs(desktop_hz - k * CONTENT_HZ) / (k * CONTENT_HZ) < 0.01) return 0;
    int best = 0;
    for (int i = 0; i < n; i++) {
        int r = rates[i];
        if (r > desktop_hz) continue;
        for (int k = 1; k <= 4; k++)
            if (fabs(r - k * CONTENT_HZ) / (k * CONTENT_HZ) < 0.01 && r > best) best = r;
    }
    return best;
}

static int changed;                      /* the mode was changed (and must be restored) */
static volatile int exclusive;           /* exclusive fullscreen (the Options menu): the refresh is auto unless set */

int display_mode_exclusive(void* hwnd, int on) {
    int was = exclusive;
    exclusive = on != 0;
    if (was != exclusive && hwnd) {          /* (the window is fullscreen already: the mode for the new choice) */
        display_mode_fullscreen(hwnd, 0);
        display_mode_fullscreen(hwnd, 1);
    }
    return 1;
}
static WCHAR changed_dev[32];

static int monitor_device(HWND hwnd, WCHAR* dev) {
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO*)&mi)) return 0;
    memcpy(dev, mi.szDevice, sizeof mi.szDevice);
    return 1;
}

int display_mode_fullscreen(void* hwnd_, int on) {
    HWND hwnd = (HWND)hwnd_;
    if (!on) {
        if (!changed) return 0;
        ChangeDisplaySettingsExW(changed_dev, NULL, NULL, 0, NULL);   /* back to the desktop's mode (the registry's) */
        changed = 0;
        fprintf(stderr, "[display] desktop refresh rate restored\n");
        return 1;
    }
    int want = ps2_settings.fullscreen_refresh;
    if (exclusive && !want) want = -1;       /* exclusive: the game owns the display - a multiple of 59.94 Hz when it has one */
    if (!want || changed) return 0;
    WCHAR dev[32];
    if (!monitor_device(hwnd, dev)) return 0;
    DEVMODEW cur;
    memset(&cur, 0, sizeof cur);
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &cur)) return 0;
    int rates[64], n = 0;
    DEVMODEW m;
    for (DWORD i = 0; n < 64; i++) {
        memset(&m, 0, sizeof m);
        m.dmSize = sizeof m;
        if (!EnumDisplaySettingsW(dev, i, &m)) break;
        if (m.dmPelsWidth != cur.dmPelsWidth || m.dmPelsHeight != cur.dmPelsHeight || m.dmBitsPerPel != cur.dmBitsPerPel) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= rates[k] == (int)m.dmDisplayFrequency;
        if (!dup) rates[n++] = (int)m.dmDisplayFrequency;
    }
    int hz = display_mode_pick(rates, n, (int)cur.dmDisplayFrequency, want);
    if (!hz) {
        if (want > 0) fprintf(stderr, "[display] fullscreen refresh %d Hz: the display has no such mode at %lux%lu (or it is the desktop's)\n",
                              want, (unsigned long)cur.dmPelsWidth, (unsigned long)cur.dmPelsHeight);
        return 0;
    }
    DEVMODEW dm = cur;
    dm.dmDisplayFrequency = (DWORD)hz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    if (ChangeDisplaySettingsExW(dev, &dm, NULL, CDS_TEST, NULL) != DISP_CHANGE_SUCCESSFUL) {
        fprintf(stderr, "[display] the display refuses %d Hz at %lux%lu\n", hz, (unsigned long)cur.dmPelsWidth, (unsigned long)cur.dmPelsHeight);
        return 0;
    }
    /* CDS_FULLSCREEN: a temporary mode (not written to the registry); Windows restores the desktop's when the game ends */
    LONG r = ChangeDisplaySettingsExW(dev, &dm, NULL, CDS_FULLSCREEN, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        fprintf(stderr, "[display] switching to %d Hz failed (%ld)\n", hz, (long)r);
        return 0;
    }
    changed = 1;
    memcpy(changed_dev, dev, sizeof dev);
    fprintf(stderr, "[display] fullscreen at %d Hz (desktop %lu Hz): %s\n", hz, (unsigned long)cur.dmDisplayFrequency,
            "every 59.94 Hz picture lasts the same number of refreshes");
    return 1;
}
