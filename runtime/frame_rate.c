/* Frame rate (pc-fps; frame_rate.h, notes/frame-rate.md). */
#include "frame_rate.h"
#include "settings.h"

#include <stdio.h>
#include <windows.h>

volatile float ps2_frame_dt_min;
static volatile LONG setting = 60;
static volatile LONG display_mhz;        /* the display's refresh, mHz (0 = unknown) */
static volatile LONG64 request_qpc;
static HANDLE request_event;

static void update_dt_min(void) {
    /* the game's own 1/60 at 60; above, low enough for any rate the limiter allows (the clamp only guards against a
     * zero / negative step; real steps come from the COP0 Count) */
    ps2_frame_dt_min = frame_rate_high() ? (float)(1.0 / (FRAME_RATE_MAX_HZ * 1.25)) : 0.0f;
}

void frame_rate_set(int s) {
    if (s != FRAME_RATE_UNLIMITED && s != FRAME_RATE_DISPLAY && (s < 30 || s > 1000)) s = 60;
    LONG was = InterlockedExchange(&setting, s);
    update_dt_min();
    if (was != s) {
        char b[64];
        fprintf(stderr, "[fps] frame rate: %s\n", frame_rate_describe(b, sizeof b));
    }
}

int frame_rate_setting(void) { return (int)setting; }

static void applier(int option) {
    (void)option;
    frame_rate_set(ps2_settings.frame_rate);
}

void frame_rate_init(void) {
    settings_on_apply(SET_APPLY_FRAME_RATE, applier);
    frame_rate_set(ps2_settings.frame_rate);
}

void frame_rate_note_display(double hz) {
    LONG v = hz > 0 ? (LONG)(hz * 1000.0 + 0.5) : 0;
    if (InterlockedExchange(&display_mhz, v) != v) update_dt_min();
}

double frame_rate_display_hz(void) { return display_mhz > 0 ? (double)display_mhz / 1000.0 : 0; }

double frame_rate_target_hz(void) {
    int s = (int)setting;
    if (s == FRAME_RATE_UNLIMITED) return 0;
    if (s == FRAME_RATE_DISPLAY) {
        double d = frame_rate_display_hz();
        return d > FRAME_RATE_NTSC_HZ * 1.01 ? (d > FRAME_RATE_MAX_HZ ? FRAME_RATE_MAX_HZ : d) : FRAME_RATE_NTSC_HZ;
    }
    return s <= 60 ? FRAME_RATE_NTSC_HZ : (double)s;
}

int frame_rate_high(void) {
    double t = frame_rate_target_hz();
    return t == 0 || t > FRAME_RATE_NTSC_HZ * 1.01;
}

double frame_rate_content_ms(void) {
    double t = frame_rate_target_hz();
    if (t > 0) return 1000.0 / t;
    double d = frame_rate_display_hz();               /* unlimited: what the display can show */
    if (d > FRAME_RATE_MAX_HZ) d = FRAME_RATE_MAX_HZ;
    return d > FRAME_RATE_NTSC_HZ ? 1000.0 / d : 1000.0 / FRAME_RATE_NTSC_HZ;
}

int64_t frame_rate_min_gap(int64_t qpf) {
    if (!frame_rate_high()) return 0;
    double t = frame_rate_target_hz();
    if (t <= 0) t = FRAME_RATE_MAX_HZ;
    return (int64_t)((double)qpf / t);
}

void* frame_vblank_event(void) {
    if (!request_event) {
        HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (InterlockedCompareExchangePointer(&request_event, e, NULL) != NULL) CloseHandle(e);
    }
    return request_event;
}

void frame_vblank_request(void) {
    if (!frame_rate_high()) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    InterlockedExchange64(&request_qpc, t.QuadPart);
    SetEvent(frame_vblank_event());
}

int64_t frame_vblank_take_request(void) { return InterlockedExchange64(&request_qpc, 0); }

const char* frame_rate_describe(char* buf, int cap) {
    int s = (int)setting;
    if (s == FRAME_RATE_UNLIMITED) snprintf(buf, (size_t)cap, "unlimited (at most %.0f fps)", FRAME_RATE_MAX_HZ);
    else if (s == FRAME_RATE_DISPLAY) snprintf(buf, (size_t)cap, "the display's (%.2f fps)", frame_rate_target_hz());
    else if (s <= 60) snprintf(buf, (size_t)cap, "60 (the PS2's 59.94 fps)");
    else snprintf(buf, (size_t)cap, "%d fps", s);
    return buf;
}
