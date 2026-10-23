/* Graphics settings applied while the game runs (notes/settings.md "graphics", notes/d3d11-renderer.md
 * section 13): sly2.ini [graphics] internal_scale, outlines, filters + motion_blur / flicker_filter /
 * interlacing, scaler -> the GPU renderer (runtime/gpu/gpu_hw.c gpu_hw_set_graphics: from the next frame,
 * targets rescaled, nothing restarted) and the game's own motion blur switch (an instruction patch,
 * recomp/patches.py "motion_blur_off": the flag at 0x2DEB24 that f_00160F98 tests before drawing the effect).
 *
 * Keys (window.c): F2 / Shift+F2 internal scale up / down, F3 outlines ps2 <-> thin, F4 filters preset
 * modern <-> faithful (the three toggles back to the preset), F5 motion blur, F6 flicker filter, Shift+F6
 * interlacing; each change is written to sly2.ini (not PS2_GS_SCALE's scale). An edit of [graphics] in sly2.ini
 * while the game runs applies when the window is activated again (gfx_reread). */
#pragma once
#include <stdint.h>

/* motion blur off (read by the recompiled game code at 0x160FA0; 0 = the game decides, as on a PS2) */
extern volatile int ps2_motion_blur_off;

int  gfx_scale(void);                    /* the internal scale in use: PS2_GS_SCALE, else internal_scale (auto: the scale
                                            the GPU renderer's auto choice runs at, 3 before it starts) */
uint32_t gfx_bits(void);                 /* GPU_GFX_* (runtime/gpu/gpu_front.h) of ps2_settings */
/* ps2_settings -> the renderer and the motion blur flag (logged when something changed) */
void gfx_apply(void);
/* a graphics key (VK_F2 .. VK_F6; shift: 1 Shift held, 2 Ctrl held): 1 if it was one; persist: write the change to
 * sly2.ini. (M11) F2 / Shift+F2 step from the scale in use (also when auto picked it) and make it fixed; Ctrl+F2 sets
 * internal_scale = auto. */
int  gfx_key(unsigned vk, int shift, int persist);
/* sly2.ini's [graphics] read again and applied (the window was activated: the file may have been edited) */
void gfx_reread(void);
/* one line describing the settings in use (the log, the window title's tooltip) */
void gfx_describe(char* out, int cap);

/* ---- (pc-pacing) for the Options menu (notes/pc-experience.md "Interfaces: pacing and resolution") ----------------
 * All apply live (from the next frame / picture) and log; persist = write to sly2.ini. */
/* internal resolution: setting = SETTINGS_SCALE_AUTO (0), 1..8, or SETTINGS_SCALE_NATIVE (settings.h); max = auto's highest
 * scale (1..8, SETTINGS_SCALE_NATIVE; other values keep it). A fixed or native scale is the most the renderer draws: while
 * the GPU cannot draw it in real time it draws a lower one (gfx_scale_status says so), the game never slows down. */
void gfx_set_internal_res(int setting, int max, int persist);
/* presentation: pacing SETTINGS_PACING_* (auto / paced / immediate / display), vsync SETTINGS_VSYNC_* (on / off /
 * adaptive); -1 keeps a value. auto: even 59.94 Hz cadence (VRR displays show it as such), display-driven game clock on
 * 60 / 120 / 240 Hz displays. */
void gfx_set_pacing(int pacing, int vsync, int persist);
/* the choices for the menu's resolution list, with real pixel sizes for the window and aspect now: "Auto (up to 2048 x
 * 1344)", "Native: 2560 x 1792 (4x)", "512 x 448 (1x, PS2)", "1536 x 896 (2x)", ... "5632 x 3584 (8x)". Returns the count. */
typedef struct {
    int setting;                 /* the internal_scale value (SETTINGS_SCALE_AUTO, SETTINGS_SCALE_NATIVE, 1..8) */
    int scale;                   /* the scale it stands for now (auto: its maximum) */
    int w, h;                    /* the picture's size in pixels at that scale */
    char label[48];
} gfx_res_option;
int  gfx_res_options(gfx_res_option* out, int max);
int  gfx_native_scale(void);             /* the native scale for the window now (2560 x 1440 16:9: 4) */
/* what is drawn now: the player's scale and the one the GPU draws (limited: lowered because the GPU cannot keep up),
 * the GPU load (GPU ms per picture / 16.68 ms) and the picture size */
typedef struct {
    int automatic, requested, drawn, limited;
    double load, stall_ms;
    int w, h;
} gfx_scale_info;
void gfx_scale_status(gfx_scale_info* s);
/* graphics presets - resolution part (the menu sets filters / outlines / aspect itself): Original = 1x; Performance =
 * auto up to 2x; Balanced = auto up to 3x (the default); Quality = native (4x at 1440p), drawn lower where the GPU cannot
 * draw it in real time. Auto measures the GPU and keeps headroom (up to 75-90% of the frame time); every scale is lowered
 * while the GPU cannot keep up, so every preset adapts to the PC it runs on and none slows the game. */
enum { GFX_PRESET_ORIGINAL, GFX_PRESET_PERFORMANCE, GFX_PRESET_BALANCED, GFX_PRESET_QUALITY, GFX_PRESET_N };
void gfx_preset_apply(int preset, int persist);
