# pc-small (checkpoint)

**Goal**: (1) movies shown from the decoded 640x480 picture (high-quality upscale, 4:3) instead of the 512x224-per-field
GS path; (2) the pause menu's "Controls..." opens our Options on the Controls page.

## Design (decided)
- Movie layer, renderer-agnostic, on the GS thread: patches at the movie player's entry/return emit hints
  MOVIE_BEGIN/END (gs_hint.h); gs_draw.c draw_prim tells runtime/movie_layer.c about each primitive while a movie plays;
  the movie sprite (textured CT32 sprite) snapshots its texture from gs_vram (640x480) into a ring slot; anything else
  drawn into the picture -> no layer for that VBlank (fallback = the GS picture). VBlank -> slot table; presenters
  (gpu_present.c present_fit, sw presenter) look the picture's vblank up and draw the slot with the resampler.
- Controls: pcmenu_pause_list replaces entry 0x43 with a callback-free copy; pcmenu_select opens menu on Controls page.

## Done
- debug build works in worktree (build\sly2, 47 s).

## Next
- implement movie_layer.c + hooks; controls entry; headless verify; gates.
