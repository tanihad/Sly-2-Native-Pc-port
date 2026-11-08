/* Frame rate (pc-fps; frame_rate.h, notes/frame-rate.md). */
#include "frame_rate.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

volatile float ps2_frame_dt_min;
volatile float ps2_frame_dt_fixed;
static volatile LONG setting = 60;
static volatile LONG display_mhz;        /* the display's refresh, mHz (0 = unknown) */
static volatile LONG64 request_qpc;
static HANDLE request_event;
static volatile LONG movie;              /* a movie plays (patches frame_movie_*) */
static volatile LONG excluded;           /* the camera follows an object that keeps 60 fps (frame_vblank_request) */
static volatile LONG lock_mhz;           /* (pc-smooth) display-locked cadence: refresh / k, mHz (0 = none) */

static void update_dt_min(void) {
    /* the game's own 1/60 at 60; above, low enough for any rate the limiter allows (the clamp only guards against a
     * zero / negative step; real steps come from the COP0 Count) */
    ps2_frame_dt_min = frame_rate_high() ? (float)(1.0 / (FRAME_RATE_MAX_HZ * 1.25)) : 0.0f;
    static int fixed = -1;               /* (reimagine2, tests) PS2_FRAME_DT_FIXED=1: 1 / the target rate per game frame */
    if (fixed < 0) fixed = getenv("PS2_FRAME_DT_FIXED") && atoi(getenv("PS2_FRAME_DT_FIXED")) > 0;
    double hz = fixed ? frame_rate_target_hz() : 0.0;
    ps2_frame_dt_fixed = hz > 1.0 ? (float)(1.0 / hz) : 0.0f;
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

double frame_rate_base_hz(void) {
    int s = (int)setting;
    if (s == FRAME_RATE_UNLIMITED) return 0;
    if (s == FRAME_RATE_DISPLAY) {
        double d = frame_rate_display_hz();
        return d > FRAME_RATE_NTSC_HZ * 1.01 ? (d > FRAME_RATE_MAX_HZ ? FRAME_RATE_MAX_HZ : d) : FRAME_RATE_NTSC_HZ;
    }
    return s <= 60 ? FRAME_RATE_NTSC_HZ : (double)s;
}

void frame_rate_set_lock(double hz) {
    LONG v = hz > 0 ? (LONG)(hz * 1000.0 + 0.5) : 0;
    if (InterlockedExchange(&lock_mhz, v) != v) {
        update_dt_min();
        if (v) fprintf(stderr, "[fps] frame rate locked to the display: %.2f frames/s (every picture the same number of refreshes)\n", hz);
        else fprintf(stderr, "[fps] frame rate no longer locked to the display\n");
    }
}

double frame_rate_lock_hz(void) { return lock_mhz > 0 && !movie && !excluded ? (double)lock_mhz / 1000.0 : 0; }

double frame_rate_target_hz(void) {
    int s = (int)setting;
    if (movie || excluded) return FRAME_RATE_NTSC_HZ;   /* movies: the PS2's rate (their decode loop counts VBlanks); RC */
    if (lock_mhz > 0) return (double)lock_mhz / 1000.0; /* (pc-smooth) refresh / k of a fixed-refresh display */
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

/* Objects whose behaviour depends on the frame rate keep the PS2's 60 fps while the camera follows them:
 *  - 0x2D5898: Bentley's RC chopper (episodes 2, 4, 6: RC Bombing Run, Wall Bombing, Aerial Assault). With the stick released
 *    it never settles at 60 fps (it keeps swaying sideways ~+-1200 units); at 120 / 200 fps the sideways sway stops after a few
 *    seconds while the forward / back sway goes on, larger at 200: its controller is not frame-rate independent
 *    (notes/frame-rate.md). */
static int keeps_60(uint32_t vt) { return vt == 0x002D5898u; }

void frame_vblank_request(uint32_t target_vt) {
    int ex = keeps_60(target_vt);
    if (ex != (int)excluded) {
        InterlockedExchange(&excluded, ex);
        update_dt_min();
        if (setting != 60) fprintf(stderr, "[fps] %s\n", ex ? "RC chopper: 60 fps while it flies" : "back to the frame rate setting");
    }
    if (!frame_rate_high()) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    InterlockedExchange64(&request_qpc, t.QuadPart);
    SetEvent(frame_vblank_event());
}

void frame_rate_movie(int on) {
    if (InterlockedExchange(&movie, on != 0) != (on != 0)) update_dt_min();
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
