# pc-small (checkpoint)

**Goal**: (1) movies shown from the decoded 640x480 picture; (2) pause "Controls..." opens our Controls page.

## Done
- runtime/movie_layer.c/.h, hints 7/8, gs_draw.c movie_prim, gs_thread.c vblank, gpu_present.c present_resample,
  ps2_runtime.c present_frame, patches movie_layer_begin/_end; test lists (gs-draw/front/hw, gs_replay, gs_post_probe).
- Verified headless: legal intro + Prelude 0 fallbacks; crops notes/movies/prelude_before_after.jpg.
- Controls: menu_game.c + menu_open_page; verified (notes/pc-menu2/controls_before_after.jpg).
- Docs: notes/movies.md, pc-menu.md.

## Next
- gates (build\small\gates.ps1), commit, report.

## Findings
- Movie sprite UVs 0.5..640.5 x 0.5..480.5 (texel centres); rect snapped to macroblocks.
- Start-Job inherits the parent's env: M9_GAME_EXE leaked into the "after" run once (ran main's exe).
