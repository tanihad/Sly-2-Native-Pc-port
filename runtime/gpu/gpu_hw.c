/* The Direct3D 11 GS backend in the game (gpu_hw.h, notes/d3d11-renderer.md 11 M6). */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include "gpu_hw.h"
#include "gpu.h"
#include "gpu_front.h"
#include "gpu_verify.h"
#include "gpu_pace.h"
#include "perf.h"                        /* perf_state.gs_busy (the GS thread's busy time) */
#include "settings.h"

extern void frame_rate_note_display(double hz) __attribute__((weak));   /* (pc-fps) runtime/frame_rate.c */
extern int frame_rate_high(void) __attribute__((weak));
static int fr_high(void) { return frame_rate_high && frame_rate_high(); }
extern void frame_rate_set_lock(double hz) __attribute__((weak));      /* (pc-smooth) display-locked cadence */
extern double frame_rate_lock_hz(void) __attribute__((weak));
extern double frame_rate_base_hz(void) __attribute__((weak));
int gpu_present_display_fixed(void);

#include <dxgi1_2.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>                       /* (M9) K32GetProcessMemoryInfo (kernel32) */

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);   /* debug.c */
void gs_record_request(const char* path, uint32_t frames);                                  /* gs_record.c */

/* Frames queued for the render thread before the frontend waits. Normally the queue is empty (the
 * render thread needs 1-2 ms per frame); it fills only while the GPU is slow (clock changes): 6 frames
 * absorb ~0.1 s of GPU frames at twice their usual time before the EE has to wait (PS2_GS_INFLIGHT). */
#define GPU_INFLIGHT_GAME inflight_game()
static int inflight_game(void) {
    const char* e = getenv("PS2_GS_INFLIGHT");
    return e && atoi(e) > 0 ? atoi(e) : 6;
}
/* (M9) ... but only 2 normally: the full depth only for 2 s after a slowdown filled those while the queue had been short
 * (gpu_submit_set_adaptive). PS2_GS_INFLIGHT_BASE=N (0: always the full depth, the M6 behaviour) */
static int inflight_base_game(void) {
    const char* e = getenv("PS2_GS_INFLIGHT_BASE");
    return e ? atoi(e) : 2;
}

static gpu_hw_config C;
/* (M7) graphics settings: the requested ones (any thread) and those the frontend runs with (GS thread) */
static volatile LONG want_scale = 1, want_gfx;
static int run_scale = 1;
static uint32_t run_gfx;
static int run_kx = 1;                   /* (M10) the display family's kx the frontend runs with (anamorphic widescreen) */
static volatile LONG dump_kx = 1;        /* (M10) the largest kx a dumped picture may have (read-back buffer size) */
static void hlog(const char* fmt, ...);

/* (M10, 8.3) the display family's kx at scale n: round(n * W) for the game's display aspect (the aspect of the
 * game's pictures, not of 4:3 content shown pillarboxed - targets keep their factors through a movie), read at
 * every VBlank (F7 switches it while the game runs). PS2_GS_ASPECT=W:H overrides the aspect the targets are
 * widened for (4:3: the M7 targets, stretched like the software renderer's picture). */
static int display_kx(int n) {
    static int env = -1, ew, eh;
    if (env < 0) {
        const char* e = getenv("PS2_GS_ASPECT");
        env = e && sscanf(e, "%d:%d", &ew, &eh) == 2 && ew > 0 && eh > 0;
        if (env) hlog("PS2_GS_ASPECT=%d:%d: display targets widened for that aspect", ew, eh);
    }
    int aw = 4, ah = 3;
    if (env) { aw = ew; ah = eh; }
    else if (C.aspect) C.aspect(0, &aw, &ah);
    return gpu_display_kx(n, aw, ah);
}
static int render_on;                   /* the render thread runs (hw, or sw pictures after a switch) */
static volatile LONG active;             /* the GPU draws */
static volatile LONG switch_req;
static int losses;
static uint32_t seen_thread_punts, same_thread_until;
static volatile LONG last_punt_vblank;   /* GS thread (stats drain): the last VBlank with a PUNT */
static volatile LONG front_threaded;     /* mirror of gpu_front_threaded() for the GS thread */
static uint32_t lose_at[16];             /* PS2_GPU_LOSE_AT */
static int nlose_at;
static uint32_t switch_at[16];           /* PS2_GPU_SWITCH_AT: backend switches at these VBlanks (tests) */
static int nswitch_at;

static void hlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[gpu] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

/* ---- pictures done (render thread) ------------------------------------------------------------- */
static volatile LONG presents;           /* pictures reported (stats: flips per second) */

static void on_picture_done(uint32_t vblank, void* user) {
    (void)vblank; (void)user;
    InterlockedIncrement(&presents);
    gs_picture_presented();
}

/* ---- frame dumps: read-backs completed on a thread of their own ------------------------------------ */
#define NDUMP 4
static struct { gpu_readback rb; uint32_t* px; int cap; uint32_t vblank; int content43; volatile LONG busy; } dumps[NDUMP];
static volatile LONG dump_head, dump_tail;   /* queued: [head, tail) */
static HANDLE dump_thread, dump_event;

static float dump_shift[2];              /* (M7) of the picture being dumped (gpu_hw_dump_shift, dump thread) */
void gpu_hw_dump_shift(float* sx, float* sy) { *sx = dump_shift[0]; *sy = dump_shift[1]; }

static DWORD WINAPI dump_main(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(dump_event, INFINITE);
        while (dump_head != dump_tail) {
            int i = (int)(dump_head % NDUMP);
            gpu_wait_readback(&dumps[i].rb);
            dump_shift[0] = dumps[i].rb.shift[0];
            dump_shift[1] = dumps[i].rb.shift[1];
            int ok = dumps[i].rb.status == 0 && dumps[i].rb.w > 0;   /* (no picture: display off, or lost) */
            if (C.dump) C.dump(ok ? dumps[i].px : NULL, ok ? dumps[i].rb.w : 0, ok ? dumps[i].rb.h : 0, dumps[i].vblank, dumps[i].content43);
            InterlockedExchange(&dumps[i].busy, 0);
            InterlockedIncrement(&dump_head);
        }
    }
}

/* a read-back slot for this VBlank's picture, or NULL (dumps disabled, or all slots still busy) */
static gpu_readback* dump_slot(uint32_t vblank, int content43) {
    if (!C.dump || !C.dump_wanted || !C.dump_wanted(vblank)) return NULL;
    if (!dump_thread) {
        dump_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        dump_thread = CreateThread(NULL, 0, dump_main, NULL, 0, NULL);
        if (!dump_thread) return NULL;
        SetThreadPriority(dump_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    int i = (int)(dump_tail % NDUMP);
    if (dump_tail - dump_head >= NDUMP || dumps[i].busy) {
        static int logged;
        if (!logged++) hlog("frame dump of VBlank %u skipped: the dump thread is behind", vblank);
        return NULL;
    }
    /* (M7: the picture is up to 1024 x 1024 output pixels times the scale squared) */
    int s = (int)want_scale > run_scale ? (int)want_scale : run_scale;
    int kx = (int)dump_kx > s ? (int)dump_kx : s;   /* (M10: anamorphic pictures are kx columns per output pixel) */
    int need = 1024 * 1024 * s * kx;
    if (dumps[i].cap < need) {
        free(dumps[i].px);
        dumps[i].px = malloc((size_t)need * 4);
        dumps[i].cap = dumps[i].px ? need : 0;
        if (!dumps[i].px) return NULL;
    }
    dumps[i].busy = 1;
    dumps[i].vblank = vblank;
    dumps[i].content43 = content43;
    memset(&dumps[i].rb, 0, sizeof dumps[i].rb);
    dumps[i].rb.dst = dumps[i].px;
    dumps[i].rb.cap_pixels = dumps[i].cap;
    return &dumps[i].rb;
}

static void dump_queued(void) {
    InterlockedIncrement(&dump_tail);
    SetEvent(dump_event);
}

/* ---- PS2_GS_HW_STATS ----------------------------------------------------------------------------- */
#define SW 256                           /* samples per report */
static struct {
    double front[SW], rcpu[SW], rwall[SW], gpu[SW], pres[SW], lat[SW], photon[SW], queued[SW];
    int nfront, nr, ngpu, nlat, nphoton, nqueued;
    double pgpu[SW], ready[SW], target[SW], late[SW];   /* (pc-pacing) present GPU ms, ready / delay / late after the VBlank */
    int npg, nready, ntarget;
    uint32_t skipped;
    double stall0;
    uint64_t draws, ff, rov, rov_runs, punts, downloads, loads, display_mem, kicks;
    uint32_t frames, targets, dropped, presented, game_frames;
    uint32_t rescales, reads;            /* (M8) target rescales (sample-factor changes), TRXDIR=1 reads, per report */
    uint64_t gs_busy0;                  /* perf_state.gs_busy at the start of the report period (TSC ticks) */
    uint64_t stats_ticks0;               /* (M9) stats_ticks then */
    double phase[GPU_PHASES];            /* (M9) GPU ms per phase over nphase game frames (PS2_GPU_PROF) */
    int nphase;
    LONG presents0;
    LARGE_INTEGER t0;
    double tsc_per_ms;
} S;

static void mem_log(const char* when);
static uint64_t stats_ticks;             /* (M9) TSC ticks of stats_vblank on the GS thread (it counts as GS busy time) */

/* ---- internal scale governor (M11 auto, pc-pacing: every scale) ------------------------------------------------
 * The game must never wait for the GPU: a scale the GPU cannot draw in real time fills the render queue and the EE then
 * waits at its VBlank (the game slows down - the owner's "tick speed" at 6-8x). So the scale actually drawn is chosen
 * by gpu_pace.c's governor (pace_gov) from the GPU time of every picture (timestamps around the frame's drawing and
 * compose; the present is measured apart and added):
 *  - internal_scale = auto: the highest scale up to auto_max (3) that keeps the GPU comfortably inside real time
 *    (down above 85% load for 3 s, to <= 70%; up when the next scale is predicted at <= 70%, with back-off);
 *  - a fixed internal_scale N is the maximum: it is lowered only when the GPU cannot draw it in real time (overload:
 *    a few pictures over the frame period, or above 95% for 2 s) and raised back towards N when it fits again.
 *  Load = GPU ms per picture / 16.68 ms (one picture per VBlank). Strict fixed scale (no governor, tests): the
 *  PS2_GS_SCALE environment variable or PS2_GS_GOVERNOR=0.
 * GS thread (stats_vblank). Requests (gfx.c, any thread) are latched in req_* and applied there; the scale goes to
 * want_scale like an F2 change: the frontend rescales every target from the next frame. */
static volatile LONG auto_max;           /* 0: off (a fixed scale); else the highest scale auto picks */
static volatile LONG req_fixed = 1;      /* the player's fixed scale (gpu_hw_set_graphics) */
static volatile LONG req_serial;         /* bumped by every request */
static LONG req_seen = -1;
static pace_gov G;
static int gov_strict = -1;              /* PS2_GS_SCALE / PS2_GS_GOVERNOR=0: the fixed scale as is */
static double gov_slow = 1.0;            /* PS2_GS_AUTO_SLOW=<factor>[:<s>] */
static int gov_slow_secs;
static double gov_t0;
static volatile LONG gov_limited;        /* fixed scale: drawn below the player's (status for the menu) */
static volatile LONG gov_load_x1000;     /* the last second's load x 1000 (status) */
static volatile LONG gov_reason;

static double now_qpc_ms(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

void gpu_hw_set_auto_scale(int max) {
    if (max > 8) max = 8;
    if (max < 0) max = 0;
    if (InterlockedExchange(&auto_max, max) != max) InterlockedIncrement(&req_serial);
}
int gpu_hw_auto_scale(void) { return auto_max ? (int)want_scale : 0; }

void gpu_hw_scale_status(gpu_hw_scale_info* s) {
    memset(s, 0, sizeof *s);
    s->automatic = auto_max != 0;
    s->requested = auto_max ? (int)auto_max : (int)req_fixed;
    s->drawn = (int)want_scale;
    s->limited = !auto_max && (int)want_scale < (int)req_fixed;
    s->load = (double)gov_load_x1000 / 1000.0;
    s->reason = (int)gov_reason;
    s->stall_ms = gpu_submit_stall_ms();
}

static void gov_init(void) {
    if (gov_strict < 0) {
        const char* e = getenv("PS2_GS_GOVERNOR");
        const char* sc = getenv("PS2_GS_SCALE");
        gov_strict = (e && *e && atoi(e) == 0) || (sc && atoi(sc) >= 1);
        e = getenv("PS2_GS_AUTO_SLOW");
        gov_slow = e && atof(e) > 0 ? atof(e) : 1.0;
        gov_slow_secs = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
    }
    int fixed = auto_max == 0;
    pace_gov_init(&G, fixed ? (int)req_fixed : (int)auto_max, fixed, (int)want_scale);
    const char* r = getenv("PS2_GS_AUTO_RETRY");  /* (the first back-off, tests) */
    if (r && atoi(r) > 0) G.retry_secs = (uint32_t)atoi(r);
    G.slow = gov_slow;
    gov_t0 = now_qpc_ms();
    req_seen = req_serial;
}

static const char* gov_why(int reason) {
    switch (reason) {
    case PACE_GOV_OVERLOAD: return "the GPU could not draw it in real time";
    case PACE_GOV_HIGH: return "the GPU was too busy";
    case PACE_GOV_FITS: return "the GPU has room";
    default: return "requested";
    }
}

/* GS thread: a picture's GPU time (gpu_ms + present) at scale `scale` finished */
static void gov_frame(double gpu_ms, int scale) {
    if (!active) return;
    if (gov_slow_secs && now_qpc_ms() - gov_t0 > gov_slow_secs * 1000.0) G.slow = 1.0;
    if (scale != G.cur || scale != (int)want_scale || scale != run_scale) return;   /* (still pictures of the old scale) */
    /* (pc-smooth) PS2_GS_SCALE / PS2_GS_GOVERNOR=0 keep the scale exactly (tests, measurements): the overload rule lowered
     * a strict 3x to 1x seconds after the start of every 120 / 165 fps run */
    if (gov_strict && G.fixed && auto_max == 0) return;
    int was = G.cur;
    /* (pc-fps) the budget stays the PS2's 59.94 Hz frame at any frame rate: the resolution is lowered so the game keeps at
     * least 60 frames per second, never for a higher frame rate (above 60 the frame rate is what the GPU and the EE give,
     * the game's speed does not depend on it: notes/frame-rate.md) */
    G.period_ms = PACE_PERIOD_MS;
    G.no_overload = fr_high();           /* (pc-smooth) above 60 the game's speed does not depend on the GPU: per-second rules */
    int to = pace_gov_frame(&G, gpu_ms, now_qpc_ms());
    InterlockedExchange(&gov_load_x1000, (LONG)(G.load * 1000.0));
    static int log_secs = -1;            /* PS2_GS_AUTO_LOG=1: every second's measurement and state */
    if (log_secs < 0) { const char* e = getenv("PS2_GS_AUTO_LOG"); log_secs = e && atoi(e) > 0; }
    static uint32_t logged_secs;
    if (log_secs && G.secs != logged_secs) {
        logged_secs = G.secs;
        hlog("internal scale %s: load %.1f%% at %dx (max %d), over %d, seconds here %d, settle %d, judged %u s", G.fixed ? "governor" : "auto",
             G.load * 100.0, G.cur, G.max, G.over, G.nhist, G.settle, G.secs);
    }
    /* even 1x is more than this GPU can draw in real time (10 s over 120%): the software renderer takes over, so the
     * game keeps its speed (PS2_GS_SW_FALLBACK=0: stay on the GPU) */
    static uint32_t sw_secs;
    static int sw_over;
    if (G.secs != sw_secs) {
        sw_secs = G.secs;
        /* (pc-fps: judged against the PS2's 59.94 Hz - a frame rate above it is not a reason for the software renderer) */
        sw_over = G.cur == 1 && G.load * G.period_ms / PACE_PERIOD_MS > 1.2 ? sw_over + 1 : 0;
        static int fb = -1;
        if (fb < 0) { const char* e = getenv("PS2_GS_SW_FALLBACK"); fb = !(e && *e && atoi(e) == 0); }
        if (sw_over >= 10 && fb && !gov_strict && !C.verify) {
            sw_over = 0;
            hlog("the GPU cannot draw even the PS2's resolution in real time (%.0f%% of the frame time at 1x): switching to the "
                 "software renderer so the game keeps its speed", G.load * 100.0);
            gpu_hw_request_switch();
        }
    }
    if (!to) return;
    InterlockedExchange(&gov_reason, G.reason);
    if (G.fixed)
        hlog("internal scale %dx: %s (%.0f%% of the frame time at %dx) -> drawing at %dx%s (change %u)", G.max, gov_why(G.reason),
             G.reason_load * 100.0, was, to, to < G.max ? ", the game keeps its speed" : "", G.changes);
    else
        hlog("internal scale auto: %s (%.0f%% of the frame time at %dx) -> %dx (change %u)", gov_why(G.reason), G.reason_load * 100.0,
             was, to, G.changes);
    InterlockedExchange(&gov_limited, G.fixed && to < G.max);
    InterlockedExchange(&want_scale, to);
}

/* GS thread, every VBlank: requests from gfx.c */
static void gov_requests(void) {

    if (req_seen == req_serial) return;
    req_seen = req_serial;
    int fixed = auto_max == 0;
    int max = fixed ? (int)req_fixed : (int)auto_max;
    if (fixed && gov_strict) {           /* (tests: exactly the scale asked for) */
        G.max = G.cur = max;
        G.fixed = 1;
        InterlockedExchange(&want_scale, max);
        return;
    }
    int was = G.cur;
    pace_gov_set(&G, max, fixed, now_qpc_ms());
    if (G.cur != max) hlog("internal scale %s%d: %dx failed moments ago, drawing at %dx for now", fixed ? "" : "auto, up to ", max, max, G.cur);
    (void)was;
    InterlockedExchange(&gov_limited, fixed && G.cur < max);
    InterlockedExchange(&want_scale, G.cur);
}

/* (M9) the once-a-second report lines are formatted on the GS thread and written by a logger thread: a
 * synchronous write to the redirected stderr took ~0.5 ms per game frame of the GS thread's measured time
 * (PS2_PROFILE=gs, ep1 hub). PS2_GS_STATS_SYNC=1 writes them at once as before. */
#define LOG_LINES 16
static char log_q[LOG_LINES][2048];
static volatile LONG log_w, log_r;
static HANDLE log_ev, log_thread;
static int log_sync = -1;

static DWORD WINAPI log_main(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(log_ev, INFINITE);
        while (log_r != log_w) {
            fputs(log_q[log_r % LOG_LINES], stderr);
            fflush(stderr);
            InterlockedIncrement(&log_r);
        }
    }
}

static void slog(const char* fmt, ...) {
    if (log_sync < 0) {
        const char* e = getenv("PS2_GS_STATS_SYNC");
        log_sync = e && atoi(e) > 0;
        if (!log_sync && (log_ev = CreateEventA(NULL, FALSE, FALSE, NULL)))
            log_thread = CreateThread(NULL, 0, log_main, NULL, 0, NULL);
        if (!log_thread) log_sync = 1;
    }
    char* b;
    char tmp[2048];
    int full = !log_sync && log_w - log_r >= LOG_LINES;
    b = log_sync || full ? tmp : log_q[log_w % LOG_LINES];
    int o = snprintf(b, 2048, "[gpu] ");
    va_list ap;
    va_start(ap, fmt);
    o += vsnprintf(b + o, 2048 - (size_t)o, fmt, ap);
    va_end(ap);
    if (o > 2046) o = 2046;
    b[o] = '\n';
    b[o + 1] = 0;
    if (log_sync || full) { fputs(b, stderr); fflush(stderr); return; }
    InterlockedIncrement(&log_w);
    SetEvent(log_ev);
}

static int dcmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static double med(double* v, int n) { if (!n) return 0; qsort(v, (size_t)n, sizeof *v, dcmp); return v[n / 2]; }
static double vmax(const double* v, int n) { double m = 0; for (int i = 0; i < n; i++) if (v[i] > m) m = v[i]; return m; }

static double tsc_ms(void) {
    if (!S.tsc_per_ms) {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&b); while (b.QuadPart - a.QuadPart < f.QuadPart / 50);
        S.tsc_per_ms = (double)(__rdtsc() - t0) / ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    }
    return S.tsc_per_ms;
}

/* GS thread, every VBlank: the frontend's and the render thread's per-frame records are taken (they
 * pile up otherwise) and, with PS2_GS_HW_STATS, summarised once a second */
static void stats_vblank(uint32_t vblank) {
    static gpu_front_frame ff[16];
    static gpu_frame_stats fs[64];
    int n;
    while ((n = gpu_front_take_frames(ff, 16)) > 0)
        for (int i = 0; i < n; i++) {
            const gpu_front_frame* f = &ff[i];
            uint32_t punts = f->tier[GPU_TIER_PUNT];
            if (punts) InterlockedExchange(&last_punt_vblank, (LONG)f->vblank);
            if (C.stats > 1 && (f->downloads || f->loads || f->xcopies || punts))   /* PS2_GS_HW_STATS=2: per frame */
                hlog("frame v%u: downloads %u (%u blocks), loads %u (%u blocks), cross copies %u, PUNT %u, targets created %u",
                     f->vblank, f->downloads, f->download_blocks, f->loads, f->load_blocks, f->xcopies, punts, f->targets_created);
            if (!C.stats) continue;
            /* per game frame (>= 1000 primitives; Sly draws every second VBlank in most scenes): the time
             * spent in the frontend's hooks (its thread's CPU time would include its spin-waits) */
            if (f->kicks >= 1000 && S.nfront < SW) S.front[S.nfront++] = f->front_ms;
            if (f->kicks >= 1000) S.game_frames++;
            S.draws += f->d3d_draws; S.ff += f->ff_draws; S.rov += f->rov_draws; S.rov_runs += f->rov_runs;
            S.punts += punts; S.downloads += f->downloads; S.loads += f->loads; S.display_mem += f->display_mem;
            S.kicks += f->kicks;
            S.rescales += f->rescales; S.reads += f->xfer_read;   /* (M8) */
            S.targets = f->targets_live;
            S.frames++;
        }
    while ((n = gpu_take_frame_stats(fs, 64)) > 0) {
        for (int i = 0; i < n; i++)      /* the internal scale governor: GPU time per picture (drawing + compose + present) */
            if (!fs[i].partial && fs[i].skipped && !fr_high()) pace_gov_evidence(&G, 1, 0);   /* (behind: evidence of overload) */
        for (int i = 0; i < n; i++)
            if (!fs[i].partial && fs[i].gpu_ms >= 0 && fs[i].scale > 0)
                gov_frame(fs[i].gpu_ms + (fs[i].present_gpu_ms > 0 ? fs[i].present_gpu_ms : 0), fs[i].scale);
        for (int i = 0; i < n && C.stats; i++) {
            if (fs[i].partial) continue;
            if (C.stats > 1 && (fs[i].render_ms > 12.0 || fs[i].gpu_ms > 12.0))   /* PS2_GS_HW_STATS=2: slow frames */
                hlog("slow frame v%u: render wall %.2f ms (CPU %.2f, upload %.2f, present %.2f), GPU %.2f ms, %u draws, %u dispatches, "
                     "%u copies, %u state calls", fs[i].vblank, fs[i].render_ms, fs[i].render_cpu_ms, fs[i].upload_ms, fs[i].present_ms,
                     fs[i].gpu_ms, fs[i].draws, fs[i].dispatches, fs[i].copies, fs[i].state_calls);
            S.dropped += fs[i].dropped;
            S.presented += fs[i].presented;
            if (fs[i].latency_ms >= 0 && S.nlat < SW) S.lat[S.nlat++] = fs[i].latency_ms;
            if (fs[i].photon_ms >= 0 && S.nphoton < SW) S.photon[S.nphoton++] = fs[i].photon_ms;   /* (M9) */
            if (S.nqueued < SW) S.queued[S.nqueued++] = fs[i].queued;
            if (fs[i].present_gpu_ms >= 0 && S.npg < SW) S.pgpu[S.npg++] = fs[i].present_gpu_ms;   /* (pc-pacing) */
            if (fs[i].ready_ms >= 0 && S.nready < SW) S.ready[S.nready++] = fs[i].ready_ms;
            if (fs[i].target_ms >= 0 && S.ntarget < SW) { S.target[S.ntarget] = fs[i].target_ms; S.late[S.ntarget++] = fs[i].present_late_ms; }
            S.skipped += fs[i].skipped;
            if (fs[i].draws < 200) continue;     /* (per game frame: the VBlanks in between draw nothing) */
            if (S.nr < SW) { S.rcpu[S.nr] = fs[i].render_cpu_ms; S.rwall[S.nr] = fs[i].render_ms; S.pres[S.nr] = fs[i].present_ms; S.nr++; }
            if (fs[i].gpu_ms >= 0 && S.ngpu < SW) S.gpu[S.ngpu++] = fs[i].gpu_ms;
            if (fs[i].phases) {          /* (M9) PS2_GPU_PROF: GPU ms per phase, summed over the game frames */
                for (int k = 0; k < GPU_PHASES; k++) S.phase[k] += fs[i].phase_ms[k];
                S.nphase++;
            }
        }
    }
    {                                    /* the time the game waited for the render queue: evidence of overload */
        static double last_stall;
        double s = gpu_submit_stall_ms();
        /* (pc-fps: above 60 fps the game waits for the GPU by design - the frame rate is what it can draw - and its speed does
         * not depend on it: the GPU time per picture against the 59.94 Hz frame decides alone) */
        if (s > last_stall && !fr_high()) pace_gov_evidence(&G, 0, s - last_stall);
        last_stall = s;
    }
    gov_requests();
    if (!C.stats) return;
    if (vblank % 600 == 0) mem_log("now");
    if (vblank % 60) return;
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    LONG p = presents;
    uint64_t busy = perf_state.gs_busy;  /* (gs_thread.c: TSC ticks executing the GS stream, as PS2_PERF reports) */
    double secs = S.t0.QuadPart ? (double)(t.QuadPart - S.t0.QuadPart) / (double)f.QuadPart : 0;
    if (secs > 0) {
        double pics = (double)(p - S.presents0) / secs, k = S.frames ? 1.0 / S.frames : 0;
        double flips = C.hwnd ? (double)S.presented / secs : pics;   /* (headless: nothing is flipped; pictures) */
        double gs = S.game_frames ? (double)(busy - S.gs_busy0) / tsc_ms() / S.game_frames : 0;
        double frm = vmax(S.front, S.nfront), rcm = vmax(S.rcpu, S.nr), rwm = vmax(S.rwall, S.nr), gpm = vmax(S.gpu, S.ngpu);
        double scost = S.game_frames ? (double)(stats_ticks - S.stats_ticks0) / tsc_ms() / S.game_frames : 0;
        slog("stats v%u: %.1f flips/s (%.1f pictures/s, %u dropped) | per game frame (%u, median / max ms): GS thread %.2f (mean) | frontend %s %.2f / %.2f | "
             "render CPU %.2f / %.2f, wall %.2f / %.2f, present %.2f | GPU %.2f / %.2f | latency %.2f / %.2f | per VBlank: %.0f prims, "
             "%.0f draws (FF %.0f, ROV %.0f in %.1f runs), PUNT %llu, downloads %.1f, loads %.1f, display from memory %.2f, targets %u | "
             "this second: target rescales %u, TRXDIR=1 reads %u | photon %.2f / %.2f ms, queued frames %.0f / %.0f, absorbed slowdowns %u | "
             "stats on the GS thread %.3f ms",
             vblank, flips, pics, S.dropped, S.game_frames, gs, front_threaded ? "thread" : "(GS thread)", med(S.front, S.nfront), frm,
             med(S.rcpu, S.nr), rcm, med(S.rwall, S.nr), rwm, med(S.pres, S.nr), med(S.gpu, S.ngpu), gpm, med(S.lat, S.nlat),
             vmax(S.lat, S.nlat), (double)S.kicks * k,
             (double)S.draws * k, (double)S.ff * k, (double)S.rov * k, (double)S.rov_runs * k, (unsigned long long)S.punts,
             (double)S.downloads * k, (double)S.loads * k, (double)S.display_mem * k, S.targets, S.rescales, S.reads,
             med(S.photon, S.nphoton), vmax(S.photon, S.nphoton), med(S.queued, S.nqueued), vmax(S.queued, S.nqueued),
             gpu_submit_absorbs(), scost);
        double stall = gpu_submit_stall_ms();   /* (pc-pacing) */
        slog("stats v%u: pacing | scale drawn %dx (%s %d), load %.0f%% | GPU present %.2f ms | ready %.2f / %.2f ms after the VBlank | "
             "present delay %.2f / %.2f, late %.2f / %.2f ms | skipped presents %u | game waited for the GPU %.1f ms this second",
             vblank, (int)want_scale, auto_max ? "auto up to" : "player", auto_max ? (int)auto_max : (int)req_fixed, G.load * 100.0,
             med(S.pgpu, S.npg), med(S.ready, S.nready), vmax(S.ready, S.nready), med(S.target, S.ntarget), vmax(S.target, S.ntarget),
             med(S.late, S.ntarget), vmax(S.late, S.ntarget), S.skipped, stall - S.stall0);
        S.stall0 = stall;
        if (S.nphase) {                  /* (M9) PS2_GPU_PROF: GPU timestamps per phase */
            char ph[256];
            int o = 0;
            for (int i = 0; i < GPU_PHASES; i++)
                o += snprintf(ph + o, sizeof ph - (size_t)o, " %s %.2f", gpu_phase_names[i], S.phase[i] / S.nphase);
            slog("stats v%u: GPU phases (mean ms per game frame, %d frames):%s", vblank, S.nphase, ph);
        }
    }
    S.nphase = 0;
    memset(S.phase, 0, sizeof S.phase);
    S.stats_ticks0 = stats_ticks;
    S.nfront = S.nr = S.ngpu = S.nlat = S.nphoton = S.nqueued = 0;
    S.npg = S.nready = S.ntarget = 0;
    S.skipped = 0;
    S.draws = S.ff = S.rov = S.rov_runs = S.punts = S.downloads = S.loads = S.display_mem = S.kicks = 0;
    S.rescales = S.reads = 0;
    S.frames = S.dropped = S.presented = S.game_frames = 0;
    S.t0 = t;
    S.presents0 = p;
    S.gs_busy0 = busy;
}

/* ---- render thread and frontend ---------------------------------------------------------------- */
static char start_err[256];              /* (M11) why the last start failed */

static int render_start(void) {
    gpu_config gc;
    memset(&gc, 0, sizeof gc);
    gc.adapter = C.adapter;
    gc.hwnd = C.hwnd;
    gc.present_w = 1280;
    gc.present_h = 960;
    {                                    /* (pc-menu, pc-pacing) PS2_GS_PRESENT_SIZE=WxH: the headless present target (screenshots,
                                            pacing measurements at 1440p) */
        const char* e = getenv("PS2_GS_PRESENT_SIZE");
        int w = 0, h = 0;
        if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 && h <= 8192) { gc.present_w = w; gc.present_h = h; }
    }
    gc.tearing = C.pacing == SETTINGS_PACING_VRR;
    gc.timestamps = 1;                   /* (M11: GPU time per picture also for internal_scale = auto) */
    gc.client_size = C.client_size;
    gc.warmup = 1;                       /* (M11) caps checked and every shader created now: failures are start-up failures */
    if (!gpu_start(&gc)) {
        snprintf(start_err, sizeof start_err, "%s", gpu_start_error());
        return 0;
    }
    return 1;
}

const char* gpu_hw_start_error(void) { return start_err[0] ? start_err : "unknown"; }

static int punt_reader(uint8_t* out, void* user) { (void)user; return gpu_exec_read_memory(NULL, NULL, 0, out); }

#define GFX_FRONT (GPU_GFX_PROGRESSIVE | GPU_GFX_THIN)   /* the bits the frontend's configuration depends on */

void gpu_hw_set_graphics(int scale, uint32_t gfx) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    InterlockedExchange(&want_gfx, (LONG)gfx);
    if (!active) InterlockedExchange(&want_scale, scale);   /* (before the start: the scale to start at) */
    if (!auto_max && InterlockedExchange(&req_fixed, scale) != scale) InterlockedIncrement(&req_serial);
}

static void front_start(void) {
    gpu_front_config fc;
    memset(&fc, 0, sizeof fc);
    fc.mixed = 1;                        /* FF + ROV frames, the M5 default mode */
    run_scale = (int)want_scale;
    run_gfx = (uint32_t)want_gfx;
    fc.scale = run_scale;                /* (M7) */
    run_kx = display_kx(run_scale);      /* (M10) */
    fc.display_kx = run_kx;
    fc.display_ky2 = (run_gfx & GPU_GFX_PROGRESSIVE) != 0;
    fc.thin = (run_gfx & GPU_GFX_THIN) != 0;
    fc.sw = C.verify > 0;                /* verify: the software renderer draws too (same thread) */
    fc.thread = !fc.sw && !C.same_thread;
    gpu_exec_reset();                    /* (a restart: the render thread forgets the old targets) */
    gpu_front_start(&fc);
    gpu_front_set_sink(gpu_submit_sink, NULL);
    gpu_submit_set_depth(GPU_INFLIGHT_GAME);
    gpu_submit_set_adaptive(inflight_base_game());
    gpu_front_set_mem_reader(punt_reader, NULL);
    InterlockedExchange(&front_threaded, fc.thread);
    seen_thread_punts = gpu_front_thread_punts();
}

/* ---- display-driven VBlanks (pacing = display) ----------------------------------------------------- */
static HANDLE clock_thread;

static IDXGIOutput* find_output(void) {
    static HMODULE m;
    if (!m) m = LoadLibraryW(L"dxgi.dll");
    typedef HRESULT (WINAPI * create_fn)(REFIID, void**);
    create_fn create = m ? (create_fn)(void*)GetProcAddress(m, "CreateDXGIFactory1") : NULL;
    static const IID iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    IDXGIFactory1* f = NULL;
    if (!create || FAILED(create(&iid_factory1, (void**)&f))) return NULL;
    HMONITOR mon = MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY);
    IDXGIOutput* found = NULL;
    IDXGIAdapter1* ad = NULL;
    for (UINT a = 0; !found && SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, a, &ad)); a++) {
        IDXGIOutput* o = NULL;
        for (UINT k = 0; !found && SUCCEEDED(IDXGIAdapter1_EnumOutputs(ad, k, &o)); k++) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(IDXGIOutput_GetDesc(o, &d)) && d.Monitor == mon) found = o;
            else IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(f);
    return found;
}

/* The display's refresh rate, measured over 30 of its VBlanks (Hz; 0: no output) */
static double measure_refresh(IDXGIOutput* o) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    if (FAILED(IDXGIOutput_WaitForVBlank(o))) return 0;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 30; i++) IDXGIOutput_WaitForVBlank(o);
    QueryPerformanceCounter(&b);
    return b.QuadPart > a.QuadPart ? 30.0 * (double)f.QuadPart / (double)(b.QuadPart - a.QuadPart) : 0;
}

/* pacing = display, and (pc-pacing) pacing = auto: the display's VBlanks drive the game when it refreshes at a multiple
 * k (1..4: 60 / 120 / 240 Hz, within 0.5%) of 59.94 Hz - every k-th display VBlank is a PS2 VBlank, so every picture
 * stays on screen for exactly k refreshes (no repeated picture every 16 s); otherwise the 59.94 Hz timer stays and
 * the paced present does what it can (VRR: even; fixed refresh: the gpu_present.c log says why it judders). */
static volatile LONG clock_pacing;       /* the pacing setting now (gpu_hw_pacing_changed): the clock runs for auto / display */
static int clock_wanted(void) { return clock_pacing == SETTINGS_PACING_AUTO || clock_pacing == SETTINGS_PACING_DISPLAY; }

/* (pc-smooth) the display lock: refreshes per game frame k (0 = no lock) for a display of hz Hz - pacing = display, or auto on a
 * display seen refreshing at a fixed rate (no VRR); only when the refresh is not already a multiple of the frame rate (60 / 120
 * Hz: the existing display VBlanks) and the rate is not unlimited. k makes hz / k the highest rate at or below the setting: 165 Hz
 * at 60 -> 55 frames/s, every picture 3 refreshes */
#define FRAME_RATE_LOCK_MAX_HZ (60000.0 / 1001.0 * 1.01)
static int clock_lock_k(double hz) {
    static int env = -1;                 /* PS2_DISPLAY_LOCK=0: never (A/B measurements) */
    if (env < 0) { const char* e = getenv("PS2_DISPLAY_LOCK"); env = !(e && *e && atoi(e) == 0); }
    if (!env || !(hz > 0) || !frame_rate_base_hz || !frame_rate_set_lock) return 0;
    /* (pc-menu2) only when the player chose it (Frame pacing: Match display): the default (auto) is a plain 60 on every
     * display - 55 frames/s on a 165 Hz monitor is the player's choice, never the default */
    if (clock_pacing != SETTINGS_PACING_DISPLAY) return 0;
    double base = frame_rate_base_hz();
    if (!(base > 0)) return 0;
    if (pace_refresh_multiple(1000.0 / hz, 1000.0 / base)) return 0;
    int k = (int)ceil(hz / base - 0.01);
    if (k < 1) k = 1;
    if (fabs(hz / k - base) / base < 0.005) return 0;
    /* only rates the display's VBlanks can drive (at most the PS2's 60: above, the frame limiter makes one picture per game
     * frame and display VBlanks would present repeated pictures - measured: 120 on 165 Hz locked to 82.5 queued 90 ms of
     * pictures), and not far below the setting (144 Hz: 48 for 60 - not locked) */
    if (hz / k > FRAME_RATE_LOCK_MAX_HZ || hz / k < 0.85 * base) return 0;
    return k;
}

static DWORD WINAPI clock_main(LPVOID arg) {
    int forced = arg != NULL;                /* pacing = display (logged when it cannot); auto (quiet) */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    IDXGIOutput* o = find_output();
    if (!o) { if (forced) hlog("display pacing: no DXGI output for the window: the 59.94 Hz timer stays"); return 0; }
    for (;;) {
        while (!clock_wanted()) { if (frame_rate_set_lock) frame_rate_set_lock(0); Sleep(500); }   /* (pacing changed to paced / immediate) */
        double hz = measure_refresh(o);
        if (hz > 0) gpu_present_set_refresh(hz);
        if (hz > 0 && frame_rate_note_display) frame_rate_note_display(hz);   /* (pc-fps: frame_rate = display follows it) */
        /* (pc-smooth) a fixed-refresh display that is not a multiple of the frame rate (165 Hz without VRR): the game's cadence is
         * locked to every k-th refresh (frame_rate.h frame_rate_set_lock) */
        int lock_k = clock_lock_k(hz);
        if (frame_rate_set_lock) frame_rate_set_lock(lock_k ? hz / lock_k : 0);
        int locked = lock_k && frame_rate_lock_hz && frame_rate_lock_hz() > 0;   /* (not during movies / the RC chopper) */
        int fixed_was = gpu_present_display_fixed();
        double content = pace_content_ms();  /* (pc-fps) the frame rate's period: 59.94 Hz, or e.g. 165 Hz = the display's own */
        /* (pc-fps) above 60 fps the frame limiter makes one picture per game frame (ps2_runtime.c): display VBlanks would compose and
         * present a picture at every refresh, also when the game has not drawn a new frame (GPU time for nothing) */
        int high = fr_high();
        int k = locked ? lock_k : hz > 0 && !high ? pace_refresh_multiple(1000.0 / hz, content) : 0;
        HMONITOR mon = MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY);
        if (!k) {
            if (high) hlog("display pacing: frame rate above 60: one picture per game frame (frame limiter), the display at %.2f Hz", hz);
            else hlog("display pacing: the display runs at %.2f Hz, not a multiple of the game's %.2f Hz: the game's timer stays", hz, 1000.0 / content);
            /* (the window may go to another display: look again now and then) */
            for (;;) {                   /* (also when the display's mode changes: fullscreen_refresh, display_mode.c) */
                Sleep(2000);
                double now_hz = gpu_window_refresh_hz(C.hwnd);
                if (MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY) == mon && !(now_hz > 0 && fabs(now_hz - hz) > 1.0) &&
                    fabs(pace_content_ms() - content) <= 0.01 && gpu_present_display_fixed() == fixed_was &&
                    clock_lock_k(hz) == lock_k) continue;
                IDXGIOutput_Release(o);
                if (!(o = find_output())) return 0;
                break;
            }
            continue;
        }
        hlog("display pacing: VBlanks from the display (%.3f Hz, %d refresh%s per VBlank, %.2f frames/s%s)", hz, k, k > 1 ? "es" : "", hz / k,
             locked ? ", locked: the display refreshes at a fixed rate that is not a multiple of the frame rate" : "");
        C.vblank_external(1);
        int moved = 0;
        for (uint32_t n = 0; !moved; n++) {
            if (FAILED(IDXGIOutput_WaitForVBlank(o))) { Sleep(16); }
            else if (n % (uint32_t)k == 0) C.display_vblank();
            if ((n & 127) == 127 && (MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY) != mon ||   /* the window moved, */
                                     fabs(gpu_window_refresh_hz(C.hwnd) - hz) > 1.0 || !clock_wanted() ||
                                     fabs(pace_content_ms() - content) > 0.01 ||   /* the mode, pacing or frame rate changed */
                                     clock_lock_k(hz) != lock_k)) moved = 1;       /* (pc-smooth) VRR seen, setting changed */
            /* (pc-smooth) a movie / the RC chopper while locked: their 59.94 Hz comes from the timer (content changed above) */
        }
        C.vblank_external(0);
        IDXGIOutput_Release(o);
        if (!(o = find_output())) { hlog("display pacing: output lost: the 59.94 Hz timer again"); return 0; }
    }
}

/* (M9) the process's committed memory (private bytes) and working set, and the frontend's frame arenas */
static void mem_log(const char* when) {
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
    {
        uint64_t vl = 0, vn = 0;
        gpu_video_memory(&vl, &vn);
        slog("memory %s: committed %.0f MB (peak %.0f), working set %.0f MB, frame arenas %.1f MB, video memory %.0f MB "
             "(+ %.0f MB non-local)", when, pm.PrivateUsage / 1048576.0, pm.PeakPagefileUsage / 1048576.0,
             pm.WorkingSetSize / 1048576.0, gpu_front_arena_bytes() / 1048576.0, vl / 1048576.0, vn / 1048576.0);
    }
    static int map = -1;                 /* PS2_GS_MEM_MAP=1: committed private memory by allocation size (largest totals) */
    if (map < 0) { const char* e = getenv("PS2_GS_MEM_MAP"); map = e && atoi(e) > 0; }
    if (!map) return;
    struct { size_t size; int n; } sz[64];
    int nsz = 0;
    size_t other = 0, cur = 0;
    uint8_t* p = NULL;
    void* cur_base = NULL;
    MEMORY_BASIC_INFORMATION mi;
    for (;;) {
        int end = !VirtualQuery(p, &mi, sizeof mi);
        if (end || mi.AllocationBase != cur_base) {
            if (cur) {
                int k = 0;
                while (k < nsz && sz[k].size != cur) k++;
                if (k < nsz) sz[k].n++;
                else if (nsz < 64) { sz[nsz].size = cur; sz[nsz].n = 1; nsz++; }
                else other += cur;
            }
            if (end) break;
            cur_base = mi.AllocationBase;
            cur = 0;
        }
        if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE) cur += mi.RegionSize;
        uint8_t* q = (uint8_t*)mi.BaseAddress + mi.RegionSize;
        if (q <= p) break;
        p = q;
    }
    for (int i = 0; i < nsz; i++)
        for (int j = i + 1; j < nsz; j++)
            if ((double)sz[j].size * sz[j].n > (double)sz[i].size * sz[i].n) { size_t s = sz[i].size; int n = sz[i].n; sz[i] = sz[j]; sz[j].size = s; sz[j].n = n; }
    char line[1024];
    int o = snprintf(line, sizeof line, "memory map %s:", when);
    for (int i = 0; i < nsz && i < 20 && o < (int)sizeof line - 40; i++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %.2fMBx%d", sz[i].size / 1048576.0, sz[i].n);
    slog("%s, other %.1f MB", line, other / 1048576.0);
}

/* ---- start / stop -------------------------------------------------------------------------------- */
int gpu_hw_start(const gpu_hw_config* cfg) {
    C = *cfg;
    if (C.verify) C.same_thread = 1;
    gpu_hw_set_graphics(C.scale > 0 ? C.scale : 1, C.gfx);
    const char* e = getenv("PS2_GPU_LOSE_AT");
    for (const char* s = e; s && *s && nlose_at < 16; s = strchr(s, ',') ? strchr(s, ',') + 1 : "")
        lose_at[nlose_at++] = (uint32_t)strtoul(s, NULL, 10);
    e = getenv("PS2_GPU_SWITCH_AT");
    for (const char* s = e; s && *s && nswitch_at < 16; s = strchr(s, ',') ? strchr(s, ',') + 1 : "")
        switch_at[nswitch_at++] = (uint32_t)strtoul(s, NULL, 10);
    gpu_set_picture_done(on_picture_done, NULL);
    gpu_set_flip_callback(C.presented);
    if (C.stats) mem_log("before the GPU renderer");
    if (!render_start()) return 0;
    render_on = 1;
    gpu_caps_print(gpu_get_caps());
    if (C.stats) mem_log("render thread and device up");
    front_start();
    gov_init();                          /* (pc-pacing: the internal scale governor, from the scale the frontend starts at) */
    InterlockedExchange(&active, 1);
    int pm, pv;
    gpu_present_get_mode(&pm, &pv);
    static const char* const pmn[PACE_MODE_N] = { "auto", "paced", "immediate", "display" }, *const pvn[PACE_VSYNC_N] = { "on", "off", "adaptive" };
    hlog("GPU renderer on (%s, frontend on %s%s, pacing %s, vsync %s)", C.hwnd ? "window" : "headless",
         front_threaded ? "its own thread" : "the GS thread", C.verify ? ", verify" : "", pmn[pm], pvn[pv]);
    /* (pc-pacing) the display's VBlanks drive the game on 60 / 120 / 240 Hz displays: pacing = display, and auto */
    gpu_hw_pacing_changed(C.pacing);
    return 1;
}

/* (pc-pacing) graphics.pacing while the game runs: the display clock thread is started for auto / display and stands by
 * otherwise (the presenter's part: gpu_present_set_mode) */
void gpu_hw_pacing_changed(int pacing) {
    InterlockedExchange(&clock_pacing, pacing);
    C.pacing = pacing;
    if ((pacing == SETTINGS_PACING_DISPLAY || pacing == SETTINGS_PACING_AUTO) && render_on && C.hwnd && C.vblank_external && C.display_vblank)
        if (!clock_thread && !(clock_thread = CreateThread(NULL, 0, clock_main, (void*)(intptr_t)(pacing == SETTINGS_PACING_DISPLAY), 0, NULL)))
            hlog("display pacing: no clock thread");
}

void gpu_hw_attach_gs(void) {
    gs_set_vblank_hook(gpu_hw_vblank);
    gs_set_read_hook(gpu_hw_sync_memory);
}

int gpu_hw_active(void) { return active != 0; }

void gpu_hw_stop(void) {
    if (active) gpu_front_stop();
    InterlockedExchange(&active, 0);
    if (render_on) gpu_stop();
    render_on = 0;
}

/* ---- GS memory read-back ------------------------------------------------------------------------- */
static gpu_dl_entry* rb_e;
static uint16_t* rb_w;
static uint8_t* rb_mem;

static int read_gpu_memory(int* n_out) {   /* (frontend thread, or the GS side with the frontend idle) */
    if (!rb_mem) {
        rb_mem = malloc(4u << 20);
        rb_e = malloc(16384 * sizeof *rb_e);
        rb_w = malloc(16384 * sizeof *rb_w);
        if (!rb_mem || !rb_e || !rb_w) { hlog("out of memory"); exit(1); }
    }
    gpu_front_checkpoint();              /* everything so far to the render thread */
    int n = gpu_front_gpu_blocks(rb_e, rb_w, 16384);
    *n_out = n;
    return gpu_exec_read_memory(rb_e, rb_w, n, rb_mem);
}

static void sync_fn(void* arg) {
    int* ok = arg;
    int n = 0;
    *ok = read_gpu_memory(&n);
    if (!*ok) return;
    for (int i = 0; i < n; i++) {        /* the GPU words of each block into gs_vram */
        uint32_t b = rb_e[i].abs;
        uint32_t* d = (uint32_t*)(gs_vram + (size_t)b * 256);
        const uint32_t* s = (const uint32_t*)(rb_mem + (size_t)b * 256);
        for (int k = 0; k < 64; k++) if (rb_e[i].words >> k & 1) d[k] = s[k];
    }
}

void gpu_hw_sync_memory(void) {
    if (!active) return;
    int ok = 0;
    gpu_front_call(sync_fn, &ok);
    if (!ok) hlog("GS memory read-back failed (device lost?): gs_vram keeps what it has");
}

typedef struct { uint8_t* out; int ok; } readmem_arg;
static void readmem_fn(void* arg) {
    readmem_arg* a = arg;
    int n = 0;
    a->ok = read_gpu_memory(&n);
    if (a->ok) memcpy(a->out, rb_mem, 4u << 20);
}

int gpu_hw_read_memory(uint8_t* out) {
    if (!active) return 0;
    readmem_arg a = { out, 0 };
    gpu_front_call(readmem_fn, &a);
    return a.ok;
}

/* ---- the VBlank (GS thread) ---------------------------------------------------------------------- */
static void verify_vblank(const gs_display_snap* snap, uint32_t vblank);

void gpu_hw_vblank(const gs_display_snap* snap, uint32_t vblank, int content43) {
    if (!active) return;                 /* (the software renderer draws: its own pictures) */
    gpu_display_info di;
    memset(&di, 0, sizeof di);
    memcpy(di.priv, snap->priv, sizeof di.priv);
    di.vblank = vblank;
    int64_t vq = gs_vsync_qpc();         /* (pc-pacing) when the VBlank was due: the paced present */
    if (vq > 0) {
        static LARGE_INTEGER qf;
        if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
        di.vblank_ms = (double)vq * 1000.0 / (double)qf.QuadPart;
    }
    gs_display_layout L;
    di.field_code = (uint8_t)(gs_display_layout_of(snap->priv, &L) && L.field_mode ? gs_display_field_code(snap, &L) : 0xFF);
    di.content43 = (uint8_t)content43;
    int aw = 4, ah = 3;
    if (C.aspect) C.aspect(content43, &aw, &ah);
    di.aspect_w = (uint16_t)aw;
    di.aspect_h = (uint16_t)ah;
    di.flags = GPU_DI_SHOW | GPU_DI_SIGNAL;
    /* (M7) graphics settings: picture options now, the frontend's configuration from the next frame */
    int ws = (int)want_scale;
    uint32_t wg = (uint32_t)want_gfx;
    di.scale = (uint8_t)ws;
    di.gfx = wg;
    int wk = display_kx(ws);             /* (M10: the game's aspect, per picture: F7 switches it) */
    di.scale_x = (uint16_t)wk;
    if (wk > (int)dump_kx) InterlockedExchange(&dump_kx, wk);
    if (ws != run_scale || ((wg ^ run_gfx) & GFX_FRONT) || wk != run_kx) {
        di.flags |= GPU_DI_CONFIG;
        hlog("graphics: internal scale %d, display targets %dx%d samples per pixel (%s), outlines %s (from VBlank %u)", ws, wk,
             (wg & GPU_GFX_PROGRESSIVE) ? 2 * ws : ws, (wg & GPU_GFX_PROGRESSIVE) ? "progressive" : "field-resolution",
             (wg & GPU_GFX_THIN) ? "thin" : "ps2", vblank + 1);
    }
    run_scale = ws;
    run_gfx = wg;
    run_kx = wk;
    gpu_readback* rb = dump_slot(vblank, content43);
    di.rb = rb;
    gpu_front_vblank_display(&di);
    if (rb) dump_queued();
    if (C.verify > 0 && vblank % (uint32_t)C.verify == 0) verify_vblank(snap, vblank);
    uint64_t t0 = __rdtsc();
    stats_vblank(vblank);
    stats_ticks += __rdtsc() - t0;
}

/* ---- PS2_GS_VERIFY (GS thread; the frontend runs on it, the software renderer draws too) ----------- */
/* (ids >= 0x100: downloaded data of a target of kind id - 0x100, gpu_front_block_origin) */
static int verify_kind(uint32_t id) { return id >= 0x100 ? (int)(id - 0x100) : gpu_front_target_kind(id); }

static void verify_vblank(const gs_display_snap* snap, uint32_t vblank) {
    static uint16_t writer[16384];
    static uint32_t checks, bad, tolerated;
    gs_draw_sync();                      /* the software renderer has drawn everything up to this VBlank */
    int n = 0;
    if (!read_gpu_memory(&n)) { hlog("verify v%u: GPU memory read-back failed", vblank); return; }
    memset(writer, 0, sizeof writer);
    /* GPU data downloaded into vram_buf (a memory read of a target's pages, e.g. the hubs' colour target
     * drawing into main-Z pages) keeps the tolerance class of the target that drew it */
    for (uint32_t b = 0; b < 16384; b++) writer[b] = (uint16_t)gpu_front_block_origin(b);
    for (int i = 0; i < n; i++) writer[rb_e[i].abs] = rb_w[i];
    gpu_mem_diff d;
    gpu_mem_compare(rb_mem, gs_vram, writer, verify_kind, 0, &d);
    checks++;
    if (d.blocks && !d.over) tolerated++;
    /* PS2_GS_VERIFY_DUMP=1: both pictures of every comparison (paired checks); =K > 1: of the comparisons at VBlanks
     * that are multiples of K (M8 sweeps: K = PS2_DUMP_EVERY pairs them with the frame dumps at scale) */
    static int pair_dump = -1;
    if (pair_dump < 0) { const char* e = getenv("PS2_GS_VERIFY_DUMP"); pair_dump = e && atoi(e) > 0 ? atoi(e) : 0; }
    if (pair_dump && !d.over && vblank % (uint32_t)pair_dump == 0) {
        static gs_compose_hist *ps, *ph;
        static uint32_t a[1024 * 1024], b[1024 * 1024];
        if (!ps) { ps = gs_compose_hist_new(); ph = gs_compose_hist_new(); }
        const char* dir = C.out_dir && *C.out_dir ? C.out_dir : ".";
        char path[600];
        int w = 0, h = 0, w2 = 0, h2 = 0;
        if (ps && ph && gs_compose_from(snap, gs_vram, ps, a, 1024, &w, &h) && gs_compose_from(snap, rb_mem, ph, b, 1024, &w2, &h2)) {
            snprintf(path, sizeof path, "%s/pair_%06u_sw.png", dir, vblank);
            debug_write_png(path, a, w, h, 1024);
            snprintf(path, sizeof path, "%s/pair_%06u_hw.png", dir, vblank);
            debug_write_png(path, b, w2, h2, 1024);
        }
    }
    if (d.over) {
        bad++;
        hlog("verify v%u: GPU memory differs beyond the tolerance: %u blocks (first %04X, writer %u); colour words +-1 %llu, "
             "+-2 %llu, 3..16 %llu, > 16 %llu of %llu; Z %llu of %llu; CPU %llu", vblank, d.blocks, d.first_block,
             d.first_block >= 0 ? writer[d.first_block] : 0, (unsigned long long)d.c1, (unsigned long long)d.c2,
             (unsigned long long)d.c16, (unsigned long long)d.cbig, (unsigned long long)d.nc, (unsigned long long)d.z,
             (unsigned long long)d.nz, (unsigned long long)d.cpu);
        if (d.cpu && bad <= 8) {         /* CPU data differs: where (it is exact otherwise) */
            int shown = 0;
            for (uint32_t b = 0; b < 16384 && shown < 6; b++) {
                if (writer[b]) continue;
                const uint32_t *x = (const uint32_t*)(rb_mem + (size_t)b * 256), *y = (const uint32_t*)(gs_vram + (size_t)b * 256);
                int k = 0;
                while (k < 64 && x[k] == y[k]) k++;
                if (k == 64) continue;
                int nw = 0;
                for (int j = 0; j < 64; j++) nw += x[j] != y[j];
                hlog("verify v%u: CPU block %04X (page %03X): %d words differ, first word %d: GPU %08X software %08X", vblank, b, b >> 5,
                     nw, k, x[k], y[k]);
                gpu_front_debug_block(b, stderr);
                shown++;
            }
        }
        if (bad <= 8) {                  /* both pictures, and the stream from here for offline bisection */
            static gs_compose_hist *hs, *hh;
            static uint32_t sw[1024 * 1024], hw[1024 * 1024];
            if (!hs) { hs = gs_compose_hist_new(); hh = gs_compose_hist_new(); }
            const char* dir = C.out_dir && *C.out_dir ? C.out_dir : ".";
            char path[600];
            int w = 0, h = 0;
            if (hs && gs_compose_from(snap, gs_vram, hs, sw, 1024, &w, &h)) {
                snprintf(path, sizeof path, "%s/verify_%06u_sw.png", dir, vblank);
                debug_write_png(path, sw, w, h, 1024);
            }
            if (hh && gs_compose_from(snap, rb_mem, hh, hw, 1024, &w, &h)) {
                snprintf(path, sizeof path, "%s/verify_%06u_hw.png", dir, vblank);
                debug_write_png(path, hw, w, h, 1024);
            }
            snprintf(path, sizeof path, "%s/verify_%06u.gsrec", dir, vblank);
            gs_record_request(path, 60);
            hlog("verify v%u: pictures and the next 60 VBlanks' GS stream in %s/verify_%06u*", vblank, dir, vblank);
        }
        gpu_front_resync();              /* compare the next frames from equal memory */
    }
    if (checks % 60 == 0 || d.over)
        hlog("verify v%u: %u comparisons, %u beyond the tolerance, %u with tolerated differences", vblank, checks, bad, tolerated);
}

/* ---- EE thread: switches, device loss, PUNT fall-back --------------------------------------------- */
void gpu_hw_request_switch(void) { InterlockedExchange(&switch_req, 1); }

void gpu_hw_present_sw(const uint32_t* bgrx, int w, int h, int stride, int aspect_w, int aspect_h) {
    static uint32_t n;
    if (render_on) gpu_picture_aspect(bgrx, 1, w, h, stride, ++n, aspect_w, aspect_h);
}

static void to_sw(int lost) {
    if (!lost) gpu_hw_sync_memory();     /* the GPU's drawing becomes the software renderer's memory */
    gs_set_vblank_hook(NULL);
    gs_set_read_hook(NULL);
    gpu_front_stop();
    gs_draw_set_sw(1);
    InterlockedExchange(&active, 0);
}

static void to_hw(void) {
    front_start();                       /* gs_vram (the software renderer's drawing) is the frontend's memory */
    gpu_hw_attach_gs();
    InterlockedExchange(&active, 1);
}

void gpu_hw_ee_vblank(uint32_t vblank) {
    if (!render_on) return;
    for (int i = 0; i < nlose_at; i++)
        if (lose_at[i] == vblank) { hlog("simulating a device loss at VBlank %u (PS2_GPU_LOSE_AT)", vblank); gpu_simulate_device_lost(); }
    for (int i = 0; i < nswitch_at; i++)
        if (switch_at[i] == vblank) InterlockedExchange(&switch_req, 1);
    if (gpu_device_lost()) {
        gs_sync();                       /* (pictures still queued are reported by the render thread as it skips them) */
        losses++;
        int was = active != 0;
        if (was) to_sw(1);
        gpu_stop();
        render_on = 0;
        if (losses < 3 && render_start()) {
            render_on = 1;
            if (was) to_hw();
            hlog("device lost (%d of 3): device and swap chain recreated, GS memory reloaded from the CPU side", losses);
        } else {
            hlog("device lost %d times: the software renderer takes over", losses);
            if (C.window_external) C.window_external(0);
        }
        return;
    }
    if (switch_req) {
        InterlockedExchange(&switch_req, 0);
        gs_sync();
        if (active) { to_sw(0); hlog("switched to the software renderer (VBlank %u)", vblank); }
        else { to_hw(); hlog("switched to the GPU renderer (VBlank %u)", vblank); }
        return;
    }
    if (!active) return;
    /* PUNT on the frontend thread (5.4): it cannot make the GS thread draw the primitive; the frontend
     * moves to the GS thread, where PUNTs are exact, and back after 600 VBlanks without one */
    uint32_t tp = gpu_front_thread_punts();
    if (front_threaded && tp != seen_thread_punts) {
        gs_sync();
        gpu_front_set_thread(0);
        InterlockedExchange(&front_threaded, 0);
        seen_thread_punts = gpu_front_thread_punts();
        same_thread_until = vblank + 600;
        hlog("PUNT on the frontend thread (%u so far, not drawn): the frontend runs on the GS thread for now", tp);
    } else if (!front_threaded && !C.same_thread && vblank >= same_thread_until &&
               vblank - (uint32_t)last_punt_vblank >= 600) {
        gs_sync();
        gpu_front_set_thread(1);
        InterlockedExchange(&front_threaded, 1);
        seen_thread_punts = gpu_front_thread_punts();
        hlog("no PUNT for 600 VBlanks: the frontend is on its own thread again");
    }
}
