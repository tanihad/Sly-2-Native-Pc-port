/* Frame pacing and GPU load governor: the pure decision logic (no D3D, no clocks of its own: every time is passed in,
 * in milliseconds), used by gpu_present.c (render thread) and gpu_hw.c (GS thread), unit-tested by tests/unit/pacing.
 * notes/pc-experience.md "Interfaces (pacing)" and notes/d3d11-renderer.md appendix N.
 *
 * The game's clock is the PS2's 59.94 Hz VBlank (ps2_runtime.c host timer); rendering never sets it:
 *  - pace_delay: picture v is presented at its VBlank's time + D, D adapted to how long the pictures take to be ready
 *    (95th percentile of the last 2 s + a margin): pictures leave at an even 16.68 ms cadence, which a VRR display
 *    (G-Sync / FreeSync) shows as such, and the render thread never submits picture v+1 before v is presented.
 *  - pace_disp: from the display's own record of when it showed each present (DXGI frame statistics) - is the
 *    display following the presents (VRR active) or showing them on its fixed refresh grid?
 *  - pace_gov: the internal scale actually drawn. GPU load = GPU time per VBlank picture / 16.68 ms; a GPU that
 *    cannot draw a scale in real time would make the whole game wait (render queue full -> EE waits at the VBlank),
 *    so the scale is lowered as soon as that is measured (fixed scales too: the player's scale is the maximum) and
 *    raised again with hysteresis when the predicted load fits. */
#pragma once
#include <stdint.h>

#define PACE_PERIOD_MS (1000.0 * 1001.0 / 60000.0)   /* one NTSC VBlank: 16.683 ms */

/* (pc-fps) the pictures' cadence now: PACE_PERIOD_MS at frame_rate = 60, the chosen rate's period above (runtime/frame_rate.c;
 * programs without it - unit tests, gs_replay - get PACE_PERIOD_MS) */
extern double frame_rate_content_ms(void) __attribute__((weak));
static inline double pace_content_ms(void) { return frame_rate_content_ms ? frame_rate_content_ms() : PACE_PERIOD_MS; }

/* ---- present delay ---------------------------------------------------------------------------------------------- */
#define PACE_NLAT 120
typedef struct {
    double period_ms;                    /* the content's period (PACE_PERIOD_MS) */
    double delay_ms;                     /* D: present at VBlank time + D */
    double min_ms, max_ms, margin_ms;
    float lat[PACE_NLAT];                /* ready latencies (picture ready - its VBlank), ring */
    int nlat, lpos;
    uint32_t samples;
} pace_delay;
void pace_delay_init(pace_delay* d, double period_ms);
/* a picture was ready (its GPU work finished) ready_ms after its VBlank: D follows the 95th percentile + margin,
 * up at once, down by at most 0.05 ms per picture */
void pace_delay_sample(pace_delay* d, double ready_ms);
double pace_delay_quantile(const pace_delay* d, double q);   /* of the stored samples (0 if none) */

/* ---- present flags ---------------------------------------------------------------------------------------------- */
enum { PACE_VSYNC_ON, PACE_VSYNC_OFF, PACE_VSYNC_ADAPTIVE, PACE_VSYNC_N };
enum { PACE_MODE_AUTO, PACE_MODE_PACED, PACE_MODE_IMMEDIATE, PACE_MODE_DISPLAY, PACE_MODE_N };
/* Sync interval and tearing for a present at now_ms whose target was target_ms (0: none): vsync on -> 1; off -> 0
 * with tearing when the swap chain allows it; adaptive -> 1 when on time, 0 + tearing when more than 2 ms late.
 * Returns the sync interval; *tear = use DXGI_PRESENT_ALLOW_TEARING. */
int pace_present_flags(int vsync, int tearing_ok, double now_ms, double target_ms, int* tear);

/* ---- display: VRR or fixed refresh ------------------------------------------------------------------------------ */
#define PACE_NIV 128
enum { PACE_DISP_UNKNOWN, PACE_DISP_VRR, PACE_DISP_FIXED, PACE_DISP_MULTIPLE };
typedef struct {
    double refresh_ms;                   /* the display's refresh period (0: unknown) */
    double content_ms;                   /* the presents' cadence (PACE_PERIOD_MS) */
    float iv[PACE_NIV];                  /* intervals between consecutive presents as shown (ms), ring */
    int n, pos;
} pace_disp;
void pace_disp_init(pace_disp* p, double refresh_ms, double content_ms);
/* two consecutive presents (present counts n and n + 1) were shown interval_ms apart */
void pace_disp_interval(pace_disp* p, double interval_ms);
/* PACE_DISP_VRR: the shown intervals follow the content cadence off the refresh grid; FIXED: they sit on the refresh
 * grid (59.94 fps content judders on it unless the refresh is a multiple); MULTIPLE: the refresh is a multiple of the
 * content rate (60, 120, 240 Hz: even either way, cannot tell and need not); UNKNOWN: too few samples */
int pace_disp_classify(const pace_disp* p);
/* refresh period a multiple k (1..4) of the content period within 0.5% -> k, else 0 */
int pace_refresh_multiple(double refresh_ms, double content_ms);

/* ---- GPU load governor (internal scale) ------------------------------------------------------------------------- */
#define PACE_GOV_UP_SECS 15
#define PACE_GOV_NFR 24
enum { PACE_GOV_NONE, PACE_GOV_OVERLOAD, PACE_GOV_HIGH, PACE_GOV_FITS, PACE_GOV_MAX };
typedef struct {
    int max;                             /* the highest scale (fixed: the player's; auto: the auto maximum) */
    int fixed;                           /* 1: the player chose the scale (lowered only when the GPU cannot keep up) */
    int cur;                             /* the scale drawn now */
    float fr[PACE_GOV_NFR];              /* GPU ms of the last pictures at cur (ring) */
    int nfr, fpos;
    int skip;                            /* pictures not judged after a change (rescale, clocks) */
    int warm;                            /* pictures the overload rule waits after a light second (GPU clocks rising) */
    double sec_t0, sec_gpu;              /* the current second: start, GPU ms, pictures */
    int sec_frames;
    int over, settle, nhist;
    double hist[PACE_GOV_UP_SECS];       /* load of the judged seconds at cur */
    uint32_t secs, left_at[9], backoff[9];
    uint32_t changes;
    uint32_t retry_secs;                 /* first back-off (120 s, doubled each time a scale is left again) */
    double load;                         /* the last second's load (GPU ms per picture / period) */
    double slow;                         /* tests: measured GPU time x slow (PS2_GS_AUTO_SLOW) */
    int reason;                          /* PACE_GOV_* of the last change */
    double reason_load;                  /* the load that caused it */
    int sec_skipped;                     /* this second: pictures skipped because the renderer was behind */
    double sec_stall_ms;                 /* this second: ms the game waited for the render queue */
    double last_behind, last_stall_ms;   /* the last second's (statistics) */
    double period_ms;                    /* (pc-fps) the frame budget: one picture per period (PACE_PERIOD_MS at 60 fps; the
                                            frame rate's period above, frame_rate.h) */
} pace_gov;
void pace_gov_init(pace_gov* g, int max, int fixed, int start);
/* a new maximum / mode (player's choice): cur becomes the highest scale <= max not left for being too slow within
 * its back-off (the player asked for it again: a fixed scale is tried once more unless it failed in the last 10 s) */
void pace_gov_set(pace_gov* g, int max, int fixed, double now_ms);
/* evidence of the current second: pictures skipped because the renderer was behind, ms the game waited for the GPU; a second
 * with >= 10% skipped or >= 50 ms waited counts as overloaded (105%) whatever the GPU times say (another program on the GPU) */
void pace_gov_evidence(pace_gov* g, int skipped, double stall_ms);
/* one picture's GPU time (ms) at the scale drawn now, completed at now_ms; returns the new scale (0: no change) */
int pace_gov_frame(pace_gov* g, double gpu_ms, double now_ms);
/* relative GPU time of scale n against scale s (measured table, 16:9) */
double pace_gov_cost(int n, int s);
/* the highest scale below cur predicted at <= fit for a load measured at cur (at least one step, at least 1) */
int pace_gov_fit(int cur, double load, double fit);

/* ---- internal resolution: picture sizes ------------------------------------------------------------------------- */
/* The picture the GPU renderer composes at internal scale n for a display aspect aw:ah (progressive, the game's 512 x 448
 * display): 512 * kx x 448 * n, kx = round(n * (aw/ah) / (4/3)) (gpu_front.h gpu_display_kx: anamorphic widescreen), at
 * 1x the PS2's own 512 x 448. Native: the smallest n whose picture covers the window's picture area (the largest aw:ah
 * rectangle in out_w x out_h) in both directions - 2560 x 1440 at 16:9: 4x (2560 x 1792); 1920 x 1080: 3x. */
/* (inline: gfx.c uses them in programs without the GPU renderer) */
static inline int pace_display_kx(int n, int aw, int ah) {     /* (= gpu_front.h gpu_display_kx) */
    if (n < 1) n = 1;
    if (n == 1 || aw <= 0 || ah <= 0) return n;
    int k = (int)((double)n * (double)aw * 3.0 / ((double)ah * 4.0) + 0.5);
    return k < n ? n : k > 16 ? 16 : k;
}

static inline void pace_picture_size(int n, int aw, int ah, int* w, int* h) {
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    *w = 512 * pace_display_kx(n, aw, ah);
    *h = 448 * n;
}

static inline int pace_native_scale(int out_w, int out_h, int aw, int ah) {
    if (aw <= 0 || ah <= 0) { aw = 4; ah = 3; }
    if (out_w <= 0 || out_h <= 0) return 3;
    int lw = out_w, lh = out_h;                  /* the picture area: the largest aw:ah rectangle in the window */
    if ((long long)out_w * ah > (long long)out_h * aw) lw = (int)((long long)out_h * aw / ah);
    else lh = (int)((long long)out_w * ah / aw);
    for (int n = 1; n <= 8; n++) {
        int w, h;
        pace_picture_size(n, aw, ah, &w, &h);
        if (w >= lw && h >= lh) return n;
    }
    return 8;
}