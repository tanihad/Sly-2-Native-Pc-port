# WIP: gpu-M6 (in-game integration, opt-in)

**Goal**: milestone M6 of notes/d3d11-renderer.md section 11 on branch gpu/M6 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M6): the hw backend in the game behind PS2_GS_BACKEND=hw /
sly2.ini `[graphics] renderer = gpu` (default software), GPU compose + present on the render thread's
swap chain, pacing, device loss, verify, stats, live switch, frame dumps, packaged build.

**Status**: in progress.

## Plan (design decisions)
- New `runtime/gpu/gpu_hw.c` (+ `gpu_hw.h`): the in-game integration used by ps2_runtime.c and by
  gs_replay (`--game`): starts the render thread (swap chain on the window, or headless offscreen), the
  frontend (mixed = 1, thread = 1, display_ky2 = 0, S = 1), the sink; the GS thread's VBlank hook
  (`gpu_hw_vblank`: display snapshot -> frontend VBlank with display info); picture done after Present;
  dumps via asynchronous picture read-back on a dump thread; PS2_GS_HW_STATS; PS2_GS_VERIFY; switch;
  device loss (restart of render thread + frontend; 3 losses -> sw).
- GPU compose (`GPU_FOP_PRESENT` extended): the frontend decides per circuit whether the displayed
  rect is current in a C32 target at the display base/width (sample the target) or not (own_mem_read:
  download GPU words into vram_buf, sample vram_buf through swz_tab). `cs_compose` reproduces
  gs_compose_from exactly (both circuits, PMODE MMOD/ALP/SLBG, BGCOLOR, MAGH/MAGV, field-mode DH fix),
  `cs_deint` the field deinterlacer (history per parity on the GPU, when/dims on the render thread).
  Gate: GPU picture == gs_compose_from(GPU memory) bit for bit at S=1 on the corpus.
- window.c: external presenter mode (window_gpu.h): no device, picture hand-off forwarded, HWND and
  client size exported, F9 (debug, PS2_GS_DEBUG_KEYS=1) backend switch request.
- gs_thread.c: VBlank hook, exported picture-done, read hook after gs_sync for TRXDIR=1.
- gs_draw.c: HUD-transformed vertex copies get serial 0 (the frontend thread's vertex cache is keyed by
  serial: a transformed copy must not alias the untransformed vertex).
- PUNT with the frontend thread: documented fallback - a PUNT on the frontend thread is counted, the
  primitive is skipped and the backend switches the frontend to the same thread at the next VBlank
  (exact PUNTs from then on); back to the frontend thread after 600 VBlanks without PUNT.

## Done
- (nothing yet)

## In progress
- reading code, baseline release build (build\rel)

## Next
1. gpu_cmd producer lock; gpu_hw.c skeleton; window_gpu.h; ps2_runtime wiring; game boots with hw.
2. GPU compose shaders + display info through the frontend (thread too) + present + picture done.
3. gs_replay --game: GPU picture vs reference compose (0 px) and vs sw (tolerance) on all 24.
4. dumps, stats, verify, switch, device loss, pacing modes, settings key.
5. Verification runs (fix-perf path, ep3/ep5/ep8 hubs), window tests, everything else; appendix H.

## Findings / traps
- (M5) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
