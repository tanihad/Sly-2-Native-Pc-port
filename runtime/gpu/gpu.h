/* Direct3D 11 GS backend: interface used by the GS side (notes/d3d11-renderer.md, section 2).
 *
 * A dedicated render thread owns the D3D11 device, the immediate context and the swap chain; the
 * GS side never calls D3D. It talks to the render thread only through command chunks
 * (gpu_cmd.h): every function below that takes effect on the GPU queues a command and returns.
 * The calls must come from one producer at a time (the GS thread, or whoever the caller orders
 * them with: gs_thread.c's picture callback runs on a pixel worker but never overlaps itself).
 *
 * Milestone M0 is the "all-fallback" backend: the software renderer draws everything and this
 * backend only uploads its picture, composes it into a GPU picture and presents it (swap chain on
 * a window, or an offscreen target when headless). Later milestones replace the picture upload by
 * real GPU drawing behind the same thread/command structure.
 *
 * Selection: PS2_GS_BACKEND=sw|hw (ps2_runtime.c), default sw. */
#pragma once
#include <stdint.h>

enum { GPU_ADAPTER_HW = 0, GPU_ADAPTER_WARP = 1 };

typedef struct {
    int adapter;                 /* GPU_ADAPTER_HW (default adapter) or GPU_ADAPTER_WARP */
    void* hwnd;                  /* window for the swap chain; NULL: headless (offscreen present target) */
    int present_w, present_h;    /* headless present target size (0: 1280x960) */
    int tearing;                 /* windowed: Present(0, ALLOW_TEARING) when supported, else Present(1, 0) */
    int timestamps;              /* GPU timestamp queries around each picture (gpu_frame_stats.gpu_ms) */
    /* (M6) the window's client size as the window thread last saw it (window_gpu.h); NULL: GetClientRect */
    void (*client_size)(int* w, int* h);
    /* (M11) the game: the device must have what the renderer needs (caps.full) and every shader the game can use is
     * created at start-up (a failure is a start-up failure: gpu_start_error) */
    int warmup;
} gpu_config;

typedef struct {
    int ok;                      /* device created */
    char adapter[128];           /* adapter description */
    uint32_t vendor_id, device_id;
    int warp;
    uint32_t feature_level;      /* D3D_FEATURE_LEVEL, 0xb100 = 11_1 */
    int rovs;                    /* D3D11_OPTIONS2.ROVsSupported */
    int typed_uav_load;          /* D3D11_OPTIONS2.TypedUAVLoadAdditionalFormats */
    int dual_source;             /* SRC1 blend state created (FL >= 10_0 guarantees it) */
    int cb_offsets;              /* D3D11_OPTIONS.ConstantBufferOffsetting (PSSetConstantBuffers1) */
    int cb_partial_update;       /* D3D11_OPTIONS.ConstantBufferPartialUpdate */
    int map_no_overwrite_cb;     /* D3D11_OPTIONS.MapNoOverwriteOnDynamicConstantBuffer */
    int doubles;                 /* D3D11_FEATURE_DOUBLES */
    int swapchain;               /* a swap chain exists (windowed) */
    int waitable;                /* frame-latency waitable object */
    int tearing;                 /* DXGI_FEATURE_PRESENT_ALLOW_TEARING */
    int full;                    /* (M11) everything the game's renderer needs: FL 11_0 or 11_1, typed UAV loads,
                                    dual source, doubles (ROVs are used when present: gpu_no_rov) */
} gpu_caps;

/* (M11) why gpu_start failed (one line), what a device lacks (caps.full = 0), and whether the ROV tier runs without
 * rasterizer-ordered views (none on the device, or PS2_GPU_NO_ROV=1: plain UAV draws of disjoint primitives).
 * Test switches: PS2_GPU_FAIL=device|caps|swapchain|shader, PS2_GPU_FL=11_0, PS2_GPU_NO_ROV=1. */
const char* gpu_start_error(void);
const char* gpu_missing_caps(const gpu_caps* c);
int gpu_no_rov(void);

/* Start the render thread and create the device (and the swap chain for cfg->hwnd). Returns 1 on
 * success; on failure everything is torn down and gpu_caps()->ok is 0. */
int  gpu_start(const gpu_config* cfg);
void gpu_stop(void);                      /* finish everything, release the device, end the thread */
int  gpu_running(void);
const gpu_caps* gpu_get_caps(void);
void gpu_caps_print(const gpu_caps* c);   /* one line per capability, to stderr */

/* ---- all-fallback backend (M0) ---------------------------------------------------------------------
 * The software picture of VBlank `vblank` (BGRX, `stride` pixels per row; ok = 0: display off,
 * shown black). Copied into the data ring before returning, then uploaded, composed into the GPU
 * picture and presented on the render thread. */
void gpu_picture(const uint32_t* bgrx, int ok, int w, int h, int stride, uint32_t vblank);
/* the same, presented fitted to aspect_w:aspect_h (M6: software pictures after a live backend switch) */
void gpu_picture_aspect(const uint32_t* bgrx, int ok, int w, int h, int stride, uint32_t vblank, int aspect_w, int aspect_h);

/* Asynchronous read-back of the GPU picture composed last (sample 0 / native resolution), BGRX
 * with alpha forced to 0 like gs_compose's X byte is ignored. The render thread sets `done` to 1
 * (status 0 ok, <0 error) when dst holds w x h pixels with stride w. */
typedef struct {
    uint32_t* dst;
    int cap_pixels;                       /* capacity of dst */
    int w, h, status;
    uint32_t vblank;
    volatile long done;
    float shift[2];                       /* (M7) the picture's sample-grid shift, texels (gpu_present.c cur_shift) */
} gpu_readback;
void gpu_read_picture(gpu_readback* rb);
void gpu_wait_readback(gpu_readback* rb);

/* Run fn(arg) on the render thread (with the device) and wait for it. */
void gpu_run_sync(void (*fn)(void* arg), void* arg);

/* ---- M6: in-game pictures (gpu_hw.c) --------------------------------------------------------------
 * Frames whose VBlank carries a display info (gpu_front_vblank_display) are composed on the GPU from the
 * displayed buffers (shaders/display.hlsl, exact gs_compose_from), presented (fitted to the display
 * aspect, flip-model swap chain) and then reported here, on the render thread, in VBlank order - also
 * when the device is lost and nothing was drawn. */
void gpu_set_picture_done(void (*fn)(uint32_t vblank, void* user), void* user);
/* called on the render thread after every Present (the window title's frame rate) */
void gpu_set_flip_callback(void (*fn)(void));
/* (pc-pacing) presentation (any thread, applies from the next picture): mode PACE_MODE_* (auto / paced: picture v
 * presented at its VBlank's time + the present delay; immediate: as soon as it is drawn), vsync PACE_VSYNC_* (on /
 * off: tearing allowed / adaptive: synced when on time, at once when late); -1 keeps a value (gpu_pace.h) */
void gpu_present_set_mode(int mode, int vsync);
void gpu_present_get_mode(int* mode, int* vsync);
void gpu_present_set_refresh(double hz);  /* the display's refresh rate when known elsewhere (render thread measures it too) */
int  gpu_present_display_state(void);     /* PACE_DISP_*: what the display does with the presents (VRR, fixed refresh, ...) */
double gpu_present_delay_ms(void);        /* the present delay D now (render thread's value, statistics) */
int  gpu_device_lost(void);               /* the device was removed (or a loss simulated): restart or give up */
void gpu_simulate_device_lost(void);      /* tests: the next frame finds the device lost */
/* Forget every target (render thread state of a frontend that is restarted). */
void gpu_exec_reset(void);
/* Wait until the render thread has executed everything queued so far and the GPU is idle. */
void gpu_finish(void);
/* One line describing the render thread and the command rings (hang diagnostics; any thread). */
void gpu_debug_state(char* buf, size_t n);

/* (M9) GPU phases of a frame (PS2_GPU_PROF timestamps, grouped): FF triangles, FF sprites and lines, ROV / REF draws,
 * ALIAS16 runs (gather, draws, scatter), Z resolve / sync, the rest (memory ops, clears, shadow copies, compose) */
#define GPU_PHASES 6
static const char* const gpu_phase_names[GPU_PHASES] = { "ff-tri", "ff-sprite/line", "rov", "alias16", "zsync", "other" };

/* Per-picture statistics, completed in order (GPU time needs the timestamp queries to resolve). */
typedef struct {
    uint32_t vblank;
    uint32_t draws, dispatches, copies, uploads;   /* D3D calls made for this picture */
    uint32_t upload_bytes;
    double front_ms;                      /* producer time in gpu_* calls for this picture */
    double render_ms;                     /* render-thread wall time for this picture */
    double gpu_ms;                        /* GPU time (timestamps; -1: not measured) */
    /* frontend frames (gpu_exec.c, M2): */
    double render_cpu_ms;                 /* render-thread CPU time (QueryThreadCycleTime) */
    double upload_ms;                     /* of render_ms: Map + copy of vertices, indices, records, constants */
    uint32_t state_calls;                 /* state-setting D3D calls (after redundancy filtering) */
    int partial;                          /* a checkpoint frame (lockstep), not a VBlank */
    /* (M6) the picture: */
    double present_ms;                    /* of render_ms: the frame-latency wait + Present */
    int presented;                        /* 1: Present was called (0: headless, minimised, lost) */
    int dropped;                          /* not shown: the render thread was behind and the swap chain busy */
    uint32_t ff_draws, rov_draws;         /* D3D draws on the FF / ROV tier in this frame */
    double latency_ms;                    /* (M7) frame queued (picture reported to the EE) -> Present returned; -1: not shown */
    double photon_ms;                     /* (M9) frame queued -> the display's vsync that showed it (DXGI frame statistics),
                                             for the most recent picture the display has shown; -1: not known */
    int queued;                           /* (M9) frames queued for the render thread when this one was taken */
    int phases;                           /* (M9) phase_ms is set (PS2_GPU_PROF: GPU timestamps per phase) */
    float phase_ms[GPU_PHASES];           /* (M9) GPU ms per phase (gpu_phase_names) */
    /* (pc-pacing) */
    int scale;                            /* the internal scale the picture was composed at (0: no picture) */
    double present_gpu_ms;                /* GPU time of the present (resample into the back buffer; -1: none / not measured);
                                             gpu_ms is the frame's drawing + compose without it and without the wait before it */
    double ready_ms;                      /* the frame's GPU work finished this long after its VBlank (-1: not known) */
    double target_ms;                     /* paced present: planned present time - VBlank time (the delay D; -1: not paced) */
    double present_late_ms;               /* Present called this long after the planned time (only when paced) */
    int skipped;                          /* the present was skipped: the render thread was behind (a newer picture queued) */
} gpu_frame_stats;
int gpu_take_frame_stats(gpu_frame_stats* out, int max);   /* returns how many were taken */

/* ---- frontend frames (M2 submission benchmark, gpu_exec.c) -------------------------------------------
 * gpu_submit_sink is a gpu_front_set_sink callback: it takes the closed frame's buffers
 * (gpu_front_detach) and queues them; the render thread executes the GPU_FOP stream with real D3D11
 * objects and trivial shaders and records gpu_frame_stats per frame. At most 3 frames are in flight
 * (the sink waits). gpu_submit_wait_idle: every queued frame finished. */
#include "gpu_front.h"
void gpu_submit_sink(const gpu_front_frame_data* fd, void* user);
void gpu_submit_wait_idle(void);
/* frames the sink lets queue for the render thread before the producer waits (default 3) */
void gpu_submit_set_depth(int frames);
/* (M9) adaptive: `base` frames normally (0: the fixed depth above), the full depth for 2 s after a slowdown that filled
 * the base queue while it had been short; gpu_submit_absorbs: how often that happened */
void gpu_submit_set_adaptive(int base);
uint32_t gpu_submit_absorbs(void);
/* (pc-pacing) total ms the producer (and so the GS thread and, behind it, the game) waited for room in the render
 * queue - time the game lost to a GPU that could not keep up; frames queued now */
double gpu_submit_stall_ms(void);
int gpu_submit_queued(void);
/* M3 REF tier: frames flagged GPU_FD_REF are drawn exactly (every draw through shaders/gs.hlsl on
 * the ROV path, depth in zpatch). The GS memory image as the GPU sees it after everything queued so
 * far: vram_buf with the GPU words of `n` blocks (gpu_front_gpu_blocks: entries and writer targets)
 * from their writers (sample 0); vram_buf itself is not changed. out: 4 MB. Returns 1 on success. */
int gpu_exec_read_memory(const gpu_dl_entry* entries, const uint16_t* writers, int n, uint8_t* out);
/* statistics of the frames executed so far: draws skipped because the REF tier cannot render them
 * yet (ALIAS16 batches), checkpoint frames */
typedef struct {
    uint64_t frames, partial_frames, ref_draws, alias16_runs, downloads, loads, patches, clears, xcopies;
    uint64_t ff_draws, rov_draws;        /* M5 mixed frames: D3D draws on the FF tier (split halves counted), on the ROV tier */
    uint64_t zresolves, zsyncs;          /* zpatch -> DSV quads (ps_zresolve), DSV -> zpatch dispatches (cs_zsync) */
    uint64_t zresolve_px, zsync_px;      /* their pixels (samples) */
    uint64_t rescales;                   /* (M7) live targets whose contents moved to new sample factors */
    uint64_t dyn_shrinks;                /* (M9) per-frame upload buffers recreated smaller after a peak */
} gpu_exec_counters;
void gpu_exec_get_counters(gpu_exec_counters* out);
/* (M9) the process's video memory use (bytes): local (on the GPU), non-local (system memory); 0: unknown */
int gpu_video_memory(uint64_t* local, uint64_t* nonlocal);
/* (M7, tests) every sample of target id after everything submitted: w x h words (colour R8G8B8A8, or the zpatch
 * Z words of a depth target), its factors; 0: no such target or out too small (cap_words) */
int gpu_exec_read_target(uint32_t id, uint32_t* out, size_t cap_words, int* w, int* h, int* kx, int* ky, int* depth);
uint64_t gpu_render_thread_cycles(void);  /* QueryThreadCycleTime of the render thread (TSC ticks) */
void gpu_render_thread_affinity(uint64_t mask);   /* benchmarks: pin the render thread */

/* ---- conformance (gpu_conf.c) ----------------------------------------------------------------------
 * cs_conformance (int64 helpers, exact integer Z formula, depth encoding) against the C reference,
 * plus an SV_Depth round trip and GREATER/GEQUAL tie test through a D32S8 depth buffer. Runs on the
 * render thread. Returns the number of mismatching values (0 = bit identical), < 0 on error. */
int gpu_conformance(int vectors, uint32_t seed, int verbose);
