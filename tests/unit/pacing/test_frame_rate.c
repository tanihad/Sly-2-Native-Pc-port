/* Unit tests of the frame rate setting (runtime/frame_rate.c, pc-fps): targets, the limiter's gap, the dt clamp the patch
 * uses, movies and the RC chopper at 59.94, the pacing cadence (gpu_pace.h pace_content_ms) and the governor's budget. */
#include "frame_rate.h"
#include "settings.h"
#include "gpu/gpu_pace.h"

#include <math.h>
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const int64_t qpf = 10000000;
    frame_rate_set(60);
    CHECK(!frame_rate_high() && fabs(frame_rate_target_hz() - FRAME_RATE_NTSC_HZ) < 1e-9, "60: the PS2's 59.94, not high");
    CHECK(ps2_frame_dt_min == 0.0f && frame_rate_min_gap(qpf) == 0, "60: the game's own clamp, no limiter");
    CHECK(fabs(pace_content_ms() - PACE_PERIOD_MS) < 1e-9, "60: pacing cadence 16.68 ms (%.3f)", pace_content_ms());

    frame_rate_set(165);
    CHECK(frame_rate_high() && frame_rate_min_gap(qpf) == qpf / 165, "165: limiter gap 1/165 s");
    CHECK(fabs(pace_content_ms() - 1000.0 / 165) < 1e-9, "165: cadence 6.06 ms");
    CHECK(ps2_frame_dt_min > 0.0f && ps2_frame_dt_min < 1.0f / 500, "165: dt clamp below 1/500 s (%g)", ps2_frame_dt_min);

    frame_rate_movie(1);
    CHECK(!frame_rate_high() && ps2_frame_dt_min == 0.0f && fabs(pace_content_ms() - PACE_PERIOD_MS) < 1e-9, "a movie: 59.94");
    frame_rate_movie(0);
    CHECK(frame_rate_high() && ps2_frame_dt_min > 0.0f, "after the movie: 165 again");

    frame_vblank_request(0x002D5898u);           /* the camera follows an RC chopper */
    CHECK(!frame_rate_high() && ps2_frame_dt_min == 0.0f, "RC chopper: 59.94");
    frame_vblank_request(0x002CB230u);           /* Bentley */
    CHECK(frame_rate_high(), "RC chopper gone: 165 again");

    frame_rate_set(FRAME_RATE_DISPLAY);
    frame_rate_note_display(0);
    CHECK(!frame_rate_high(), "display, refresh unknown: 59.94");
    frame_rate_note_display(165.08);
    CHECK(frame_rate_high() && fabs(frame_rate_target_hz() - 165.08) < 1e-3, "display 165.08 Hz (%.3f)", frame_rate_target_hz());
    frame_rate_note_display(60.0);
    CHECK(!frame_rate_high(), "display 60 Hz: 59.94 (the 60 mode)");

    frame_rate_set(FRAME_RATE_UNLIMITED);
    frame_rate_note_display(144.0);
    CHECK(frame_rate_high() && frame_rate_target_hz() == 0 && frame_rate_min_gap(qpf) == (int64_t)(qpf / FRAME_RATE_MAX_HZ),
          "unlimited: at most 500 per second");
    CHECK(fabs(pace_content_ms() - 1000.0 / 144) < 1e-9, "unlimited: pacing cadence = the display's (%.3f)", pace_content_ms());

    frame_rate_set(20);
    CHECK(frame_rate_setting() == 60 && !frame_rate_high(), "20: not a rate -> 60");

    /* the setting through the settings model (live applier) */
    frame_rate_init();
    int fi = settings_find("display", "frame_rate");
    CHECK(fi >= 0 && settings_opt_at(fi)->def == 60, "display.frame_rate, default 60");
    settings_change(fi, 120, 0);
    CHECK(frame_rate_setting() == 120 && frame_rate_high(), "settings_change 120 applies live");
    settings_change(fi, 60, 0);

    /* the governor's budget stays the 59.94 Hz frame (gpu_hw.c) */
    pace_gov g;
    pace_gov_init(&g, 3, 1, 3);
    CHECK(fabs(g.period_ms - PACE_PERIOD_MS) < 1e-9, "governor period 16.68 ms");

    printf("frame rate: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
