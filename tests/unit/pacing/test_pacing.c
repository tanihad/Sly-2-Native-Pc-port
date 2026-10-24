/* Unit tests of the pacing / governor decisions (runtime/gpu/gpu_pace.c): present delay, present flags, VRR
 * detection from shown-present intervals, GPU load governor (internal scale). */
#include "gpu/gpu_pace.h"

#include <math.h>
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_delay(void) {
    pace_delay d;
    pace_delay_init(&d, PACE_PERIOD_MS);
    for (int i = 0; i < 200; i++) pace_delay_sample(&d, 10.0);
    CHECK(fabs(d.delay_ms - 11.5) < 0.01, "steady 10 ms -> D %.2f (11.5)", d.delay_ms);
    /* 10% of the pictures at 20 ms: the 95th percentile covers them */
    for (int i = 0; i < 200; i++) pace_delay_sample(&d, i % 10 == 0 ? 20.0 : 10.0);
    CHECK(fabs(d.delay_ms - 21.5) < 0.01, "10%% at 20 ms -> D %.2f (21.5)", d.delay_ms);
    /* faster again: D comes down slowly (0.05 ms per picture), never below the need */
    for (int i = 0; i < 120; i++) pace_delay_sample(&d, 5.0);
    double after = d.delay_ms;
    CHECK(after < 21.5 && after > 6.4, "after 120 fast pictures D %.2f (between 6.5 and 21.5)", after);
    for (int i = 0; i < 400; i++) pace_delay_sample(&d, 5.0);
    CHECK(fabs(d.delay_ms - 6.5) < 0.01, "steady 5 ms -> D %.2f (6.5)", d.delay_ms);
    /* a late picture raises D at once */
    for (int i = 0; i < 7; i++) pace_delay_sample(&d, 30.0);
    CHECK(d.delay_ms >= 31.4, "late pictures -> D %.2f (>= 31.5)", d.delay_ms);
    /* bounds: never above 3 periods, garbage ignored */
    pace_delay_init(&d, PACE_PERIOD_MS);
    for (int i = 0; i < 50; i++) pace_delay_sample(&d, 120.0);
    CHECK(d.delay_ms <= 3 * PACE_PERIOD_MS + 1e-9, "max D %.2f", d.delay_ms);
    uint32_t n = d.samples;
    pace_delay_sample(&d, NAN);
    pace_delay_sample(&d, -1.0);
    pace_delay_sample(&d, 1000.0);
    CHECK(d.samples == n, "NaN / negative / pause samples ignored");
    pace_delay_init(&d, PACE_PERIOD_MS);
    for (int i = 0; i < 50; i++) pace_delay_sample(&d, 0.1);
    CHECK(fabs(d.delay_ms - d.min_ms) < 1e-9 || d.delay_ms <= 0.5 * PACE_PERIOD_MS, "min D %.2f", d.delay_ms);
}

static void test_flags(void) {
    int tear;
    CHECK(pace_present_flags(PACE_VSYNC_ON, 1, 100, 90, &tear) == 1 && !tear, "vsync on: interval 1, no tearing");
    CHECK(pace_present_flags(PACE_VSYNC_OFF, 1, 100, 90, &tear) == 0 && tear, "vsync off: interval 0 + tearing");
    CHECK(pace_present_flags(PACE_VSYNC_OFF, 0, 100, 90, &tear) == 0 && !tear, "vsync off without tearing support");
    CHECK(pace_present_flags(PACE_VSYNC_ADAPTIVE, 1, 100.5, 100, &tear) == 1 && !tear, "adaptive on time: synced");
    CHECK(pace_present_flags(PACE_VSYNC_ADAPTIVE, 1, 103, 100, &tear) == 0 && tear, "adaptive late: immediate + tearing");
    CHECK(pace_present_flags(PACE_VSYNC_ADAPTIVE, 1, 103, 0, &tear) == 1 && !tear, "adaptive without a target: synced");
}

static void test_disp(void) {
    pace_disp p;
    const double r165 = 1000.0 / 165.0, r144 = 1000.0 / 144.0, r120 = 1000.0 / 120.0, r60 = 1000.0 / 60.0;
    /* VRR: shown 16.68 ms apart (+- 0.3 ms) on a 165 Hz display */
    pace_disp_init(&p, r165, PACE_PERIOD_MS);
    for (int i = 0; i < 100; i++) pace_disp_interval(&p, PACE_PERIOD_MS + ((i * 7) % 7 - 3) * 0.1);
    CHECK(pace_disp_classify(&p) == PACE_DISP_VRR, "165 Hz VRR -> %d", pace_disp_classify(&p));
    /* fixed 165 Hz: 3,3,3,2 refreshes per picture */
    pace_disp_init(&p, r165, PACE_PERIOD_MS);
    for (int i = 0; i < 100; i++) pace_disp_interval(&p, (i % 4 == 3 ? 2 : 3) * r165 + 0.02);
    CHECK(pace_disp_classify(&p) == PACE_DISP_FIXED, "165 Hz fixed -> %d", pace_disp_classify(&p));
    /* VRR with immediate presents: shown when ready, 16.68 +- 2.5 ms */
    pace_disp_init(&p, r165, PACE_PERIOD_MS);
    for (int i = 0; i < 100; i++) pace_disp_interval(&p, PACE_PERIOD_MS + ((i * 37) % 11 - 5) * 0.5);
    CHECK(pace_disp_classify(&p) == PACE_DISP_VRR, "165 Hz VRR, unpaced -> %d", pace_disp_classify(&p));
    /* fixed 144 Hz: 2 or 3 refreshes */
    pace_disp_init(&p, r144, PACE_PERIOD_MS);
    for (int i = 0; i < 100; i++) pace_disp_interval(&p, (i % 5 < 2 ? 2 : 3) * r144);
    CHECK(pace_disp_classify(&p) == PACE_DISP_FIXED, "144 Hz fixed -> %d", pace_disp_classify(&p));
    /* 144 Hz VRR */
    pace_disp_init(&p, r144, PACE_PERIOD_MS);
    for (int i = 0; i < 100; i++) pace_disp_interval(&p, PACE_PERIOD_MS);
    CHECK(pace_disp_classify(&p) == PACE_DISP_VRR, "144 Hz VRR -> %d", pace_disp_classify(&p));
    /* 120 Hz and 60 Hz: multiples of the content rate */
    pace_disp_init(&p, r120, PACE_PERIOD_MS);
    for (int i = 0; i < 100; i++) pace_disp_interval(&p, 2 * r120);
    CHECK(pace_disp_classify(&p) == PACE_DISP_MULTIPLE, "120 Hz -> %d", pace_disp_classify(&p));
    CHECK(pace_refresh_multiple(r120, PACE_PERIOD_MS) == 2, "120 Hz = 2 refreshes per VBlank");
    CHECK(pace_refresh_multiple(r60, PACE_PERIOD_MS) == 1, "60 Hz = 1");
    CHECK(pace_refresh_multiple(1000.0 / 59.94, PACE_PERIOD_MS) == 1, "59.94 Hz = 1");
    CHECK(pace_refresh_multiple(1000.0 / 240.0, PACE_PERIOD_MS) == 4, "240 Hz = 4");
    CHECK(pace_refresh_multiple(r165, PACE_PERIOD_MS) == 0, "165 Hz: no multiple");
    CHECK(pace_refresh_multiple(r144, PACE_PERIOD_MS) == 0, "144 Hz: no multiple");
    CHECK(pace_refresh_multiple(1000.0 / 75.0, PACE_PERIOD_MS) == 0, "75 Hz: no multiple");
    /* too few samples; intervals far from the cadence (repeats, late pictures) are not counted */
    pace_disp_init(&p, r165, PACE_PERIOD_MS);
    for (int i = 0; i < 59; i++) pace_disp_interval(&p, PACE_PERIOD_MS);
    CHECK(pace_disp_classify(&p) == PACE_DISP_UNKNOWN, "59 samples -> unknown");
    for (int i = 0; i < 50; i++) pace_disp_interval(&p, 2 * PACE_PERIOD_MS);
    CHECK(p.n == 59, "double intervals ignored (%d)", p.n);
    pace_disp_init(&p, 0, PACE_PERIOD_MS);
    for (int i = 0; i < 100; i++) pace_disp_interval(&p, PACE_PERIOD_MS);
    CHECK(pace_disp_classify(&p) == PACE_DISP_UNKNOWN, "refresh unknown -> unknown");
}

/* feed `n` pictures of gpu_ms each, one per period, from *t; returns the last change (0: none) */
static int feed(pace_gov* g, int n, double gpu_ms, double* t) {
    int last = 0;
    for (int i = 0; i < n; i++) {
        *t += PACE_PERIOD_MS;
        int to = pace_gov_frame(g, gpu_ms, *t);
        if (to) last = to;
    }
    return last;
}

static void test_gov(void) {
    pace_gov g;
    double t = 0;
    const double P = PACE_PERIOD_MS;
    /* the first pictures after the start (or a light second) at low GPU clocks: not judged as overload */
    pace_gov_init(&g, 3, 0, 3);
    CHECK(feed(&g, 40, 3.0 * P, &t) == 0 && g.cur == 3, "warm-up: 40 slow pictures at the start do not lower it (%d)", g.cur);
    /* auto at 3x, 50% load: stays */
    pace_gov_init(&g, 3, 0, 3);
    CHECK(feed(&g, 60 * 60, 0.5 * P, &t) == 0 && g.cur == 3, "auto 3x at 50%%: stays (%d)", g.cur);
    /* single slow pictures (GPU clock changes) are absorbed */
    for (int i = 0; i < 20; i++) { feed(&g, 1, 2.5 * P, &t); feed(&g, 59, 0.5 * P, &t); }
    CHECK(g.cur == 3, "isolated 2.5-period pictures: stays at 3x (%d)", g.cur);
    /* a loading burst (5 pictures at 3 periods, 6 at 1.5) between ordinary seconds: absorbed */
    for (int i = 0; i < 10; i++) { feed(&g, 5, 3.0 * P, &t); feed(&g, 6, 1.5 * P, &t); feed(&g, 120, 0.5 * P, &t); }
    CHECK(g.cur == 3, "loading bursts: stays at 3x (%d)", g.cur);
    /* auto at 3x, 95% for 3 s: down to the scale predicted at <= 80% */
    int to = feed(&g, 60 * 5, 0.95 * P, &t);
    CHECK(to == 2 && g.cur == 2 && g.reason == PACE_GOV_HIGH, "auto 95%% -> 2x (%d, reason %d)", to, g.reason);
    /* and not back to 3x within its back-off even when 2x is light */
    feed(&g, 60 * 60, 0.2 * P, &t);
    CHECK(g.cur == 2, "back-off: stays at 2x for 120 s (%d)", g.cur);
    feed(&g, 60 * 90, 0.2 * P, &t);
    CHECK(g.cur == 3, "after the back-off: 3x again (%d)", g.cur);

    /* the player picks 8x on a GPU that needs 70 ms per picture: lowered within a handful of pictures */
    pace_gov_init(&g, 8, 1, 8);
    g.warm = 0;                          /* (a player switching while playing: the GPU's clocks are up) */
    t = 0;
    int k = 0;
    for (; k < 30 && g.cur == 8; k++) { t += 70; pace_gov_frame(&g, 70, t); }
    CHECK(g.cur == 3 && k <= 12 && g.reason == PACE_GOV_OVERLOAD, "fixed 8x at 70 ms -> %dx after %d pictures (3x, <= 12: < 1 s)", g.cur, k);
    {   /* (pc-smooth) above 60 fps: no immediate overload rule (the game's speed does not depend on the GPU there) */
        pace_gov g2;
        pace_gov_init(&g2, 8, 1, 8);
        g2.warm = 0;
        g2.no_overload = 1;
        double t2 = 0;
        for (int i = 0; i < 30; i++) { t2 += 70; pace_gov_frame(&g2, 70, t2); }
        CHECK(g2.cur == 8, "no_overload: 30 slow pictures leave 8x to the per-second rules (%d)", g2.cur);
        /* the warm-up lasts 1 s at 165 fps too (60 pictures are 0.36 s there) */
        pace_gov_init(&g2, 3, 1, 3);
        t2 = 0;
        int n = 0;
        for (; n < 200 && g2.cur == 3; n++) { t2 += 6.06; pace_gov_frame(&g2, 60, t2); }
        CHECK(g2.cur < 3 && t2 >= 1000.0, "warm-up at 165 fps: overload judged after >= 1 s (%.0f ms, %d pictures)", t2, n);
    }
    /* at 3x 40%: a fixed scale climbs back towards the player's while the prediction fits 80% */
    feed(&g, 60 * 20, 0.4 * P, &t);    /* (one step: 3 s settle + 15 s judged)*/
    CHECK(g.cur == 4, "fixed: back up to 4x where 3x at 40%% predicts 72%% (%d)", g.cur);
    feed(&g, 60 * 40, 0.78 * P, &t);
    CHECK(g.cur == 4, "fixed 4x at 78%%: stays (5x predicted 127%%) (%d)", g.cur);
    /* a fixed scale under 95% stays (no headroom rule) */
    pace_gov_init(&g, 4, 1, 4);
    t = 0;
    CHECK(feed(&g, 60 * 30, 0.92 * P, &t) == 0 && g.cur == 4, "fixed 4x at 92%%: stays (%d)", g.cur);
    /* sustained 1.05: the 6-picture mean is under 1.1, the per-second rule lowers it after 2 s */
    to = feed(&g, 60 * 4, 1.05 * P, &t);
    CHECK(to == 3 && g.cur == 3, "fixed 4x at 105%% -> 3x (%d)", g.cur);
    /* the player asks for 4x again right away: not retried within 10 s; later it is */
    pace_gov_set(&g, 4, 1, t);
    CHECK(g.cur == 3, "4x just failed: the request stays at 3x (%d)", g.cur);
    feed(&g, 60 * 12, 0.5 * P, &t);
    pace_gov_set(&g, 4, 1, t);
    CHECK(g.cur == 4, "4x asked again after 10 s: tried (%d)", g.cur);
    /* auto maximum lowered below the current scale */
    pace_gov_init(&g, 3, 0, 3);
    pace_gov_set(&g, 2, 0, 0);
    CHECK(g.cur == 2 && g.max == 2, "max lowered: cur %d", g.cur);
    pace_gov_set(&g, 3, 0, 0);
    CHECK(g.cur == 3, "max raised: cur %d", g.cur);
    /* never below 1x, 1x overloaded stays 1x */
    pace_gov_init(&g, 1, 1, 1);
    t = 0;
    CHECK(feed(&g, 600, 3 * P, &t) == 0 && g.cur == 1, "1x overloaded: stays 1x");
    /* evidence: the GPU times look fine (60%) but the renderer is behind (20% of the pictures skipped) - another program
     * shares the GPU: auto lowers the scale after its 3 s; waits of the game (50+ ms a second) likewise */
    pace_gov_init(&g, 3, 0, 3);
    t = 0;
    to = 0;
    for (int s = 0; s < 8 && !to; s++) {
        for (int i = 0; i < 60 && !to; i++) {
            if (i % 5 == 0) pace_gov_evidence(&g, 1, 0);
            t += P;
            to = pace_gov_frame(&g, 0.75 * P, t);
        }
    }
    CHECK(to == 2 && g.reason == PACE_GOV_HIGH, "behind 20%% at 75%% load: auto 3x -> 2x (%d)", to);
    pace_gov_init(&g, 4, 1, 4);
    t = 0;
    to = 0;
    for (int s = 0; s < 8 && !to; s++) {
        pace_gov_evidence(&g, 0, 80.0);
        for (int i = 0; i < 60 && !to; i++) { t += P; to = pace_gov_frame(&g, 0.75 * P, t); }
    }
    CHECK(to == 3, "the game waited 80 ms a second at 75%%: fixed 4x -> 3x (%d)", to);
    pace_gov_init(&g, 3, 0, 3);
    t = 0;
    for (int s = 0; s < 10; s++) {
        pace_gov_evidence(&g, 2, 20.0);  /* (a little: under the thresholds) */
        feed(&g, 60, 0.5 * P, &t);
    }
    CHECK(g.cur == 3, "a few skips / 20 ms waits a second: stays (%d)", g.cur);
    /* the game waits (200 ms a second) with the GPU at 50%: a present held up elsewhere - not the GPU: stays */
    pace_gov_init(&g, 3, 0, 3);
    t = 0;
    for (int s = 0; s < 10; s++) { pace_gov_evidence(&g, 20, 200.0); feed(&g, 60, 0.5 * P, &t); }
    CHECK(g.cur == 3, "stalls with an idle GPU: stays (%d)", g.cur);
    /* the game waits with the GPU at 95%: lowered within the second */
    pace_gov_init(&g, 4, 1, 4);
    t = 0;
    feed(&g, 60 * 4, 0.6 * P, &t);
    to = 0;
    for (int s = 0; s < 2 && !to; s++) { pace_gov_evidence(&g, 0, 150.0); to = feed(&g, 61, 0.95 * P, &t); }
    CHECK(to == 3 && g.reason == PACE_GOV_OVERLOAD, "stall at 95%%: fixed 4x -> 3x in the first full second (%d)", to);
    /* the cost table: fit helper */
    CHECK(pace_gov_fit(3, 0.9, 0.7) == 2, "fit 3x 90%% -> 2");
    CHECK(pace_gov_fit(3, 2.0, 0.7) == 1, "fit 3x 200%% -> 1");
    CHECK(pace_gov_fit(8, 4.2, 0.85) == 3, "fit 8x 420%% -> 3 (%d)", pace_gov_fit(8, 4.2, 0.85));
    CHECK(pace_gov_fit(2, 0.1, 0.7) == 1, "fit: at least one step");
    /* the slow factor (tests of a slower GPU) */
    pace_gov_init(&g, 3, 0, 3);
    g.slow = 2.5;
    t = 0;
    feed(&g, 60 * 5, 0.6 * P, &t);
    CHECK(g.cur < 3, "slow x2.5: 60%% measures 150%% -> lowered (%d)", g.cur);
}

static void test_res(void) {
    int w, h;
    pace_picture_size(1, 16, 9, &w, &h);
    CHECK(w == 512 && h == 448, "1x 16:9 = PS2 512 x 448 (%d x %d)", w, h);
    pace_picture_size(3, 16, 9, &w, &h);
    CHECK(w == 2048 && h == 1344, "3x 16:9 = 2048 x 1344 (%d x %d)", w, h);
    pace_picture_size(3, 4, 3, &w, &h);
    CHECK(w == 1536 && h == 1344, "3x 4:3 = 1536 x 1344 (%d x %d)", w, h);
    pace_picture_size(4, 16, 9, &w, &h);
    CHECK(w == 2560 && h == 1792, "4x 16:9 = 2560 x 1792 (%d x %d)", w, h);
    pace_picture_size(2, 16, 9, &w, &h);
    CHECK(w == 1536 && h == 896, "2x 16:9 = 1536 x 896 (%d x %d)", w, h);
    CHECK(pace_native_scale(2560, 1440, 16, 9) == 4, "native 1440p 16:9 = 4x");
    CHECK(pace_native_scale(2560, 1440, 4, 3) == 4, "native 1440p 4:3 (1920 x 1440) = 4x");
    CHECK(pace_native_scale(1920, 1080, 16, 9) == 3, "native 1080p = 3x");
    CHECK(pace_native_scale(1280, 720, 16, 9) == 2, "native 720p = 2x");
    CHECK(pace_native_scale(3840, 2160, 16, 9) == 6, "native 2160p = 6x (%d)", pace_native_scale(3840, 2160, 16, 9));
    CHECK(pace_native_scale(1723, 999, 16, 9) == 3, "native 1723 x 999 window (1723 x 969 picture) = 3x");
    CHECK(pace_native_scale(640, 480, 4, 3) == 2, "native 640 x 480 4:3 = 2x (1x is 512 wide)");
    CHECK(pace_native_scale(0, 0, 16, 9) == 3, "no window: 3x");
    CHECK(pace_display_kx(3, 21, 9) == 5 && pace_display_kx(8, 16, 9) == 11, "kx 21:9 3x, 16:9 8x");
}

int main(void) {
    test_res();
    test_delay();
    test_flags();
    test_disp();
    test_gov();
    printf("pacing: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
