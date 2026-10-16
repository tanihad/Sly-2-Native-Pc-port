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
- 25e5b11 core: gpu_hw.c/.h, display.hlsl (cs_compose, cs_deint), gpu_present.c compose/show/fit_aspect/picture
  done, gpu_exec.c PRESENT op, frontend display info (FR_VBLANK carries it; FR_CALL), gs.c gs_display_layout_of
  (gs_compose_from refactored onto it), gs_thread hooks (vblank / read), gs_record_request, window external
  presenter (window_gpu.h), settings renderer/pacing, ps2_runtime wiring, gs_replay --game / --game-verify.
- next commit: display rect from memory through a scratch copy (no ownership change), gpu_game_all.ps1.
- Corpus (build\game_all.txt, game_allv.txt): --game 24/24, 3816 pictures = reference compose (0 px);
  --game-verify 1: 24/24, gpu_hw verifier 0 beyond tolerance, pictures vs sw 0 beyond the gate, min PSNR as M5.
- Game (release, headless, hw): boots to the title, 60 flips/s, dumps OK (build\m6_run1).

## In progress
- tests for switch / device loss / PUNT fallback (gs_replay --game calls gpu_hw_ee_vblank)

## Next
1. window unit test for the external presenter (window_gpu.h, resize, fullscreen with a flip swap chain).
2. script tokens for the switch / device loss (headless tests); PUNT fallback test (gs_replay --game PS2_GPU_PUNT).
3. pacing modes check (windowed), packaged build in hw mode.
4. Verification runs (fix-perf path to gameplay, ep3/ep5/ep8 hubs hw vs sw dumps, 5-min verify per hub, perf).
5. Everything else green; notes (settings.md, install.md, design appendix H).

## Findings / traps
- (M5) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- PowerShell variables are case-insensitive: a script param $Exe and a local $exe are the same variable.
  `$x = if (...) { @('a') } else { ... }` unwraps one-element arrays: splatting @x then passes nothing useful.
- In-game steady-state downloads exist (not in the corpus): the intro cutscene (~VBlank 1520-1880) has a colour
  target writing main-Z pages (page 08F) that the Z target then tests: 8-16 download+load ops (24-64 blocks) per
  frame. Exact (verify clean), counted; an M8/M9 item (xcopy across kinds is not possible: data reinterpretation).
- GS-thread CPU per VBlank from QueryThreadCycleTime includes the spin-waits (SPINS pause loops before
  sleeping); PS2_PERF's "GS thread ms/vblank" (busy time in the slot loop) is the comparable number.
