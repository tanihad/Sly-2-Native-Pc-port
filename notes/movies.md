# Movies at the source's resolution (pc-small, 2026-10-24)

The owner: "Watch Movies ... are running at the original PS2 resolution regardless of settings". Every movie (the
Sony / legal intro, Watch Movies on the episode menu, episode intros / endings, chalk talks) goes through one player,
f_001B3BD0 (sceMpeg loop). It decodes 640x480 pictures (IPU CSC, BT.601 limited range -> full-range RGB: ipu_csc.c,
the PS2 IPU's own coefficients), uploads each as 1200 16x16 CT32 blocks into a texture at TBP 0x1800 (TBW 12) and shows
it with ONE sprite per field (dest 512x224, DECAL, texels 0.5..640.5 x 0.5..480.5): on the PS2 and in both renderers
the picture is squeezed through the 512x448 frame, then upscaled.

**Now** (runtime/movie_layer.c/.h): patches `movie_layer_begin/_end` (the player's entry and only return) send hints
MOVIE_BEGIN / MOVIE_END (gs_hint.h kinds 7 / 8) into the GS stream. On the GS thread (gs_draw.c `movie_prim`, every
primitive while a movie plays) the movie sprite - textured, opaque (ABE 0, FBMSK 0), CT32/24, DECAL or MODULATE,
covering the FRAME width and the scissor height - copies its source rectangle (whole macroblocks: 0..640 x 0..480)
from GS memory into a ring of 8 slots, only when an upload happened since (once per movie picture). gs_thread.c
`vblank_picture` records VBlank -> slot: the slot if the movie sprite was the last thing drawn in that VBlank, the
previous decision if nothing was drawn, none if anything was drawn after it (a fade, text): that picture is the GS
picture as before. The presenters look their picture's VBlank up:
- GPU renderer: gpu_present.c `present_resample` uploads the slot (B8G8R8A8, once per new picture) and resamples it
  separably (auto scaler: Catmull-Rom to enlarge, Lanczos-2 to shrink) straight into the 4:3 rectangle of the output
  (pillarboxed at 16:9); also for the Options menu's presents over a movie.
- Software renderer: ps2_runtime.c `present_frame` hands the 640x480 picture to the window presenter at 4:3 (and to
  PS2_DUMP_FRAMES dumps).
Timing is the GS picture's (the copy is taken at the sprite, in stream order): movie frame rate and audio sync are
unchanged. `PS2_MOVIE_LAYER=0`: no hints, the old path (the "before" pictures).

Measured (debug, headless, GPU renderer, 2560x1440): the legal intro 264/264 VBlanks and "Prelude" (Watch a Movie) all
VBlanks from the decoded picture, 0 fallbacks (the player draws nothing else over its movies). Evidence:
notes/movies/prelude_before_after.jpg (same VBlank v3239 of two runs: run timing differs by a few VBlanks, so the
frames are a moment apart). Chalk talks use the same player and sprite (d3d11-renderer.md appendix M) - not run here.
