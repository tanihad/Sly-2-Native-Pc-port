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
/* (pc-smooth) progressive output (interlacing off): the game draws every frame for the same field - no half-line
 * XYOFFSET on odd fields (patches "field_offset_*") and the motion blur blends the previous picture in place (patch
 * "motion_blur_progressive"); 0 = the game's field rendering, as on a PS2 on a TV (PS2_PROGRESSIVE_FIX=0 forces it) */
extern volatile int ps2_progressive;

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
/* (pc-menu) internal_scale = native: the smallest scale whose 448 * N rows cover out_h output rows (1..8) */
int  gfx_native_scale(int out_h);
/* the output's size for internal_scale = native (window.c sets it to window_client_size; NULL: 3x) */
extern void (*gfx_output_size)(int* w, int* h);
/* the window's client size changed (window.c, when a resize ends): internal_scale = native follows it */
void gfx_output_resized(void);
/* (pc-pacing) what is drawn now (the Options menu, the log): the player's scale, the scale the GPU draws (every scale is a
 * maximum: limited = drawn lower because the GPU cannot draw it in real time - the game never slows down), the GPU load
 * (GPU ms per picture / the 16.68 ms frame time), the time the game had to wait for the GPU, the picture's size */
typedef struct {
    int automatic, requested, drawn, limited;
    double load, stall_ms;
    int w, h;
} gfx_scale_info;
void gfx_scale_status(gfx_scale_info* s);
/* (pc-pacing) graphics.pacing and graphics.vsync -> the GPU renderer's presenter, live (gfx_apply calls it too); 1 if done */
int  gfx_apply_pacing(void);