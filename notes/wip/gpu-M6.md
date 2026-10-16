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

- Window tests (tests/unit/window/run.ps1): all pass, new --external test (14 checks: no own presenter,
  forwarding, client size / resize serial, F11 + Alt+Enter under a flip swap chain of another thread, debug
  keys gating, own presenter back); settings tests for renderer / pacing / env overrides.
- Title replays (gs_replay --game, build\t_*): switch hw->sw->hw (PS2_GPU_SWITCH_AT) all pictures within the
  gate vs sw; device loss x2 recovers (one picture lost each, ~10 pictures beyond the gate while the title's
  feedback effects rebuild from CPU memory), x3 falls back to sw; PS2_GPU_PUNT=5 on the frontend thread:
  switch to the same thread at the next VBlank, 2 pictures miss the PUNTed primitives, then within the gate.
- Pacing: picture reported when its frame is queued (GPU_FD_SIGNAL). Windowed hw: 0-1 missed VBlanks in 38 s
  (signal after Present: 6-9; sw: 0). display pacing declines on this 165 Hz monitor (logged); vrr runs.
- notes/settings.md, notes/install.md updated.

## In progress
- hub runs ep3/ep5/ep8 hw + sw (scratchpad hub_runs.ps1 -> build\warp\m6_ep*_{hw,sw})

## Next
1. compare hub dumps hw vs sw; 5-min PS2_GS_VERIFY=60 per hub (memory flat, audio underruns).
2. fix-perf scripted path hw vs sw dumps (150 s), screenshots build\screenshots\gpu_m6_*.png.
3. packaged build in hw mode (scripts\package.py; imports check), debug build.
4. run_tests, unit suites (gs-hw, gs-front, gs-draw, ...), gpu_diff_all, VRAM hashes, gpu_fthread_check,
   mixed lockstep (few), gpu_game_all again (after the pacing change).
5. design doc: M6 notes in 2.1/7/8.2 + appendix H; WIP final.

## Findings / traps
- (M5) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- PowerShell variables are case-insensitive: a script param $Exe and a local $exe are the same variable.
  `$x = if (...) { @('a') } else { ... }` unwraps one-element arrays: splatting @x then passes nothing useful.
- In-game steady-state downloads exist (not in the corpus): the intro cutscene (~VBlank 1520-1880) has a colour
  target writing main-Z pages (page 08F) that the Z target then tests: 8-16 download+load ops (24-64 blocks) per
  frame. Exact (verify clean), counted; an M8/M9 item (xcopy across kinds is not possible: data reinterpretation).
- NVIDIA power management: with ~3 ms of GPU work per 16.7 ms the RTX 4060 Ti drops from P0 (2565 MHz) to
  P3/P5 (~900 MHz core, low memory clock) about 2 s after a load spike; frames then take 15-40 ms until the
  driver raises the clocks again (nvidia-smi -lms 100 during a paced replay). Anything that makes the EE wait
  for the render thread (picture done after Present) turns that into missed VBlanks; dropping presents made
  it worse (lower load, longer at low clocks). Fix: report the picture when its frame is queued.
- GS-thread CPU per VBlank from QueryThreadCycleTime includes the spin-waits (SPINS pause loops before
  sleeping); PS2_PERF's "GS thread ms/vblank" (busy time in the slot loop) is the comparable number.
