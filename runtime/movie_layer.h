/* (pc-small) Movies at the source's resolution (notes/movies.md).
 *
 * The game's movie player (f_001B3BD0) uploads each decoded 640x480 picture into GS memory as 16x16
 * macroblocks and shows it with one 512x224 sprite per field: on the PS2 (and in the renderers) the
 * picture reaches the screen squeezed through the 512x448 frame. While a movie plays (hints
 * MOVIE_BEGIN / MOVIE_END from patches at the player's entry and return), the GS thread watches the
 * primitives: the movie sprite (a textured, opaque CT32 sprite covering the frame) copies the
 * texture's source rectangle - the decoded picture as the IPU produced it - into a ring of slots; a
 * VBlank whose picture is exactly that sprite gets the slot, and the presenters (gpu_present.c, the
 * software renderer's present_frame) show the slot's picture upscaled straight to the output, at 4:3.
 * Anything else drawn after the sprite (a fade, text) -> no slot for that VBlank: the GS picture
 * is shown as before. PS2_MOVIE_LAYER=0 turns it off (no hints are sent). */
#pragma once
#include <stdint.h>

/* EE (patches movie_layer_begin / _end): the hints into the GS stream */
void movie_layer_ee(int on);

/* GS thread (gs_draw.c / gs_thread.c) */
int  movie_gs_active(void);              /* between MOVIE_BEGIN and MOVIE_END */
void movie_gs_hint(int on);
/* the movie sprite: texture tbp / tbw (CT32), source rectangle [u0, u1) x [v0, v1) in texels; mod = 0 (DECAL) or the
 * MODULATE vertex colour 0x00BBGGRR (0x80 = 1.0); uploads = a counter that changes with every host->local transfer */
void movie_gs_sprite(uint32_t tbp, uint32_t tbw, int u0, int v0, int u1, int v1, uint32_t mod, uint64_t uploads);
void movie_gs_other(void);               /* any other primitive */
void movie_gs_vblank(uint32_t vblank);   /* the picture of this VBlank: decide */

/* presenters: the movie picture of VBlank `vblank` (0xAARRGGBB rows, stride w); 0 = none (show the GS picture) */
int  movie_layer_get(uint32_t vblank, const uint32_t** px, int* w, int* h, uint32_t* serial);
