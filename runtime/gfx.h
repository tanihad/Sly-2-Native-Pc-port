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

int  gfx_scale(void);                    /* the internal scale in use: PS2_GS_SCALE, else internal_scale */
uint32_t gfx_bits(void);                 /* GPU_GFX_* (runtime/gpu/gpu_front.h) of ps2_settings */
/* ps2_settings -> the renderer and the motion blur flag (logged when something changed) */
void gfx_apply(void);
/* a graphics key (VK_F2 .. VK_F6, shift held): 1 if it was one; persist: write the change to sly2.ini */
int  gfx_key(unsigned vk, int shift, int persist);
/* sly2.ini's [graphics] read again and applied (the window was activated: the file may have been edited) */
void gfx_reread(void);
/* one line describing the settings in use (the log, the window title's tooltip) */
void gfx_describe(char* out, int cap);
