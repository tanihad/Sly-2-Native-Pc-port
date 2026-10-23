/* The Direct3D 11 GS backend in the game (notes/d3d11-renderer.md 11 M6): PS2_GS_BACKEND=hw or sly2.ini
 * [graphics] renderer = gpu. Used by ps2_runtime.c (and by scripts/gs_replay --game, headless).
 *
 * Threads: the GS thread decodes and hands primitives to the frontend thread (gpu_fthread.c), which
 * builds a frame per VBlank for the render thread (gpu_d3d11.c / gpu_exec.c). At a VBlank the GS thread
 * calls gpu_hw_vblank (gs_set_vblank_hook) with the display registers; the frame's last command composes
 * the picture on the GPU from the displayed buffers (shaders/display.hlsl, exactly gs_compose_from) and
 * presents it on the window's swap chain; the render thread then reports the picture (picture_done), so
 * the EE waits at its next VBlank just as with the software renderer. No picture is read back to the CPU
 * except for frame dumps (PS2_DUMP_FRAMES) and the in-game verifier.
 *
 * Environment (besides the settings): PS2_GS_HW_STATS=1 per-second report of per-thread and GPU times;
 * PS2_GS_VERIFY=N the software renderer also runs and every Nth VBlank both GS memories are compared
 * (10.2 tolerances; a mismatch dumps both pictures and records the next 60 VBlanks); PS2_GS_DEBUG_KEYS=1
 * F9 = switch GPU <-> software, Shift+F9 = simulate a device loss (also the PS2_INPUT tokens "renderer" and
 * "losedevice"); PS2_GPU_LOSE_AT=<vblank>[,...] simulated device losses and PS2_GPU_SWITCH_AT=<vblank>[,...]
 * backend switches (tests); PS2_GS_SAME_THREAD=1 the frontend on the GS thread. */
#pragma once
#include <stdint.h>

#include "gs_draw.h"

typedef struct {
    void* hwnd;                  /* the game window (window_gpu.h); NULL: headless (offscreen present target) */
    int adapter;                 /* GPU_ADAPTER_HW / GPU_ADAPTER_WARP */
    int pacing;                  /* SETTINGS_PACING_*: vsync, vrr (Present without waiting, tearing allowed), display */
    int verify;                  /* PS2_GS_VERIFY: every Nth VBlank compare with the software renderer (0: off) */
    int stats;                   /* PS2_GS_HW_STATS */
    int same_thread;             /* the frontend on the GS thread (verify forces it) */
    const char* out_dir;         /* where the verifier writes pictures and recordings ("." if NULL) */
    void (*client_size)(int* w, int* h);                 /* window_client_size (NULL: GetClientRect) */
    void (*presented)(void);                             /* a picture was presented (window title frame rate) */
    void (*aspect)(int content43, int* aspect_w, int* aspect_h);   /* the aspect a picture is shown at */
    int  (*dump_wanted)(uint32_t vblank);                /* PS2_DUMP_FRAMES: read this VBlank's picture back */
    /* on the dump thread, for every VBlank dump_wanted accepted (px NULL, w = h = 0: no picture - display
     * off or device lost) */
    void (*dump)(const uint32_t* px, int w, int h, uint32_t vblank, int content43);
    void (*window_external)(int on);                     /* window_set_external_presenter (fallback to sw) */
    void (*vblank_external)(int on);                     /* pacing display: the host timer stops raising VBlanks */
    void (*display_vblank)(void);                        /* pacing display: a VBlank of the display */
    int scale;                   /* (M7) internal scale N (1..8; 0: 1) - [graphics] internal_scale, PS2_GS_SCALE */
    uint32_t gfx;                /* (M7) GPU_GFX_* (gpu_front.h): progressive, thin outlines, flicker off, fields, scaler */
} gpu_hw_config;
/* (M7) graphics settings changed while the game runs: the scale and the target configuration apply from the
 * next frame (targets are rescaled, nothing is lost), picture options from the next VBlank. Any thread. */
void gpu_hw_set_graphics(int scale, uint32_t gfx);
/* (M11) internal_scale = auto: max > 0 lets the renderer lower the scale (to 1 at the least) while the GPU cannot keep
 * up and raise it again up to max (the owner's 3x) when it can (gpu_hw.c, hysteresis); 0 = the scale of
 * gpu_hw_set_graphics. gpu_hw_auto_scale: the scale auto runs at now (0: auto off). */
void gpu_hw_set_auto_scale(int max);
int  gpu_hw_auto_scale(void);
/* (pc-pacing) the internal scale as the GPU actually draws it: a fixed scale is the player's maximum - the renderer
 * draws a lower one while the GPU cannot draw it in real time, so the game never waits for the GPU (gpu_pace.c
 * pace_gov). For the Options menu ("4x requested, drawing 3x: GPU limit"). Any thread. */
typedef struct {
    int automatic;               /* internal_scale = auto */
    int requested;               /* the player's scale (auto: its maximum) */
    int drawn;                   /* the scale drawn now */
    int limited;                 /* a fixed scale drawn lower because the GPU cannot keep up */
    double load;                 /* GPU time per picture / the 16.68 ms frame time, last second (1.0 = the GPU's limit) */
    int reason;                  /* PACE_GOV_* of the last change (gpu_pace.h) */
    double stall_ms;             /* total time the game waited for the GPU since the start (0 when it never had to) */
} gpu_hw_scale_info;
void gpu_hw_scale_status(gpu_hw_scale_info* s);
/* (pc-pacing) graphics.pacing changed while the game runs (SETTINGS_PACING_*): the display clock for auto / display */
void gpu_hw_pacing_changed(int pacing);
/* (M11) why gpu_hw_start returned 0 (one line: no device, missing capabilities, swap chain, shaders) */
const char* gpu_hw_start_error(void);
/* (M7) inside the dump callback: the dumped picture's sample-grid shift in picture texels (x, y) - a picture at
 * scale is offset against one centred on the PS2's pixels (notes/d3d11-renderer.md 8.1) */
void gpu_hw_dump_shift(float* sx, float* sy);

/* Start the render thread (swap chain on cfg->hwnd) and the frontend; the GS must be idle (before
 * gs_thread_start, or after gs_sync). 0: no capable device (the caller stays with the software renderer). */
int  gpu_hw_start(const gpu_hw_config* cfg);
/* The GS thread's hooks: VBlank pictures (gs_set_vblank_hook) and memory read-back (gs_set_read_hook). */
void gpu_hw_attach_gs(void);
int  gpu_hw_active(void);                /* the GPU draws (0 after a switch / fall-back to the software renderer) */
/* GS thread, at each VBlank in stream order (the gs_set_vblank_hook hook). */
void gpu_hw_vblank(const gs_display_snap* snap, uint32_t vblank, int content43);
/* EE thread, at each VBlank before it is queued: backend switch requests, device loss recovery, the
 * frontend thread's PUNT fall-back, statistics. */
void gpu_hw_ee_vblank(uint32_t vblank);
void gpu_hw_request_switch(void);        /* GPU <-> software at the next VBlank (debug key) */
/* window_present_fn: a software picture shown through the render thread (after a switch to sw). */
void gpu_hw_present_sw(const uint32_t* bgrx, int w, int h, int stride, int aspect_w, int aspect_h);
void gpu_hw_sync_memory(void);           /* gs_vram <- every GPU-written word (TRXDIR=1, recordings, switch) */
/* GS memory as the GPU has it after everything queued (4 MB: vram_buf + the GPU words), GS side idle or
 * the caller; tests (gs_replay --game). */
int  gpu_hw_read_memory(uint8_t* out);
void gpu_hw_stop(void);
