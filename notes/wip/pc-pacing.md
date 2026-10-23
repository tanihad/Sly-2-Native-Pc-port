# WIP: pc-pacing (branch next/pc-pacing, worktree sly2-worktrees\pc-pacing)

**Goal**: game speed independent of rendering (59.94 VBlanks/s at any internal scale), smooth presentation on
high-refresh / VRR displays (owner: 2560x1440 @ 165 Hz Dell S2722DGM, RTX 4060 Ti), GPU cost at 1440p per preset,
defaults that adapt to other PCs (weaker GPUs, AMD/Intel, 60 Hz/high refresh, VRR or not), runtime interfaces for the
Options menu. Brief: notes/pc-experience.md.

**Coordinator rules**: run headless/offscreen for everything except a few batched real-monitor pacing runs
(< 60 s each, write a line here before each, close the game right after). Other agents' games share the GPU
(check `Get-Process sly2*` before measuring GPU cost).

## Done (all committed)
- Root cause 1 confirmed (baseline, release, headless, ep1 hub): 3x EE 59.92 VBlanks/s; 6x EE 42.8 (min 29.9), game
  flips 29.3/s. gpu_submit_sink blocks while the render queue is full -> frontend -> GS thread -> EE waits in
  gs_queue_vsync. build\pace\base_ep1_{3,6}x\run.log.
- runtime/gpu/gpu_pace.c/.h (pure, tests/unit/pacing 68 checks): present delay D (p95 of ready latency + 1.5 ms), present
  flags (vsync on/off/adaptive), VRR classifier (DXGI shown intervals vs refresh grid), governor (auto: 85%/3 s -> 70%,
  up <= 70% lower quartile 15 s, back-off 120 s doubling; fixed = maximum: > 100% 2 s -> 85%; overload: mean of 12
  pictures > 2.5 periods or 24 > 1.4; evidence: >= 10% skipped presents or >= 50 ms game wait per second counts as 105%
  (one step down); warm-up 60 pictures after start / light seconds), picture sizes and native scale.
- gpu_hw.c: governor replaces auto_scale_tick (fixed scales too; PS2_GS_SCALE / PS2_GS_GOVERNOR=0 strict), native
  scale following the window, software fallback when 1x > 120% for 10 s (PS2_GS_SW_FALLBACK=0 off), stats "pacing" line,
  display clock for 60/120/240 Hz (pacing auto/display), PS2_GS_PRESENT_SIZE.
- gpu_present.c: t1 before the present (gpu_ms = drawing + compose), present_gpu_ms, ready event per picture, paced
  present at VBlank due time + D (headless with PS2_PACE_HEADLESS=1), skip when behind, vsync modes, frame statistics ->
  VRR detection + PS2_PACE_LOG lines, refresh from QueryDisplayConfig; auto = immediate once the display is FIXED or a
  multiple (pacing cannot help on a fixed grid; latency).
- VBlank due time plumbing (ps2_runtime host timer -> gs_set_vsync_time -> K_VSYNC slot -> gs_vsync_qpc ->
  gpu_display_info.vblank_ms). Queue depth + ceil(D/period) while paced (gpu_present_pace_frames).
- Settings (settings.c minimal): graphics.pacing auto|paced|immediate|display|vrr (vsync = auto), graphics.vsync,
  graphics.internal_scale native, graphics.internal_scale_max, display.fullscreen_refresh desktop|auto|Hz.
- gfx.c/.h menu interfaces: gfx_set_internal_res, gfx_set_pacing, gfx_res_options (pixel sizes), gfx_native_scale,
  gfx_scale_status, gfx_preset_apply. display_mode.c (fullscreen refresh switch, CDS_FULLSCREEN) + window.c hooks.
- Tests: tests/unit/pacing, window unit 1023 checks. Scripts: pace_run.ps1, pace_report.ps1, pace_plot.py, pace_cost.ps1.

## Real-monitor runs (logged before each, per the coordinator)
- 2026-10-23 R1-R4 (done): borderless auto / borderless immediate / windowed maximised / borderless 6x; 50 s each.
  Result: the owner's display is FIXED 165 Hz (VRR not active: shown intervals on the 6.06 ms grid; G-Sync/FreeSync off
  in the driver or monitor). Paced vs immediate: same 2/3-refresh judder (std 6.4 ms), paced adds ~40 ms latency ->
  auto now presents immediately on fixed displays. 6x borderless: governor 6x -> 4x -> 3x, game 59.9 VBlanks/s.

- 2026-10-23 F1-F4 (final build): borderless auto (3x auto) / windowed maximised auto / borderless native (Quality) /
  borderless 7x fixed; 50 s each from boot, PS2_PACE_LOG=1, game closes by itself (watchdog). No display mode change
  (fullscreen_refresh stays desktop: CDS_TEST only, 120 Hz accepted).

- 2026-10-23 F5-F6 (after the merge): console build borderless 7x 50 s; packaged build (portable copy, own sly2.ini,
  default settings + fullscreen) 45 s. PS2_PACE_LOG=1, game closes itself (watchdog).

## Merged main 4c3cf6c (pc-menu etc.) - commit d871088
- Settings now table-driven (main): my options added as rows (graphics.pacing 5 values, graphics.vsync,
  display.fullscreen_refresh); main's auto_max / native (-1, height-based) / presets kept; my scale_max, gfx_set_* menu
  functions and gpu_hw native code dropped (the menu owns them). runtime/pacing_menu.c = menu_ext.h's
  pacing_apply_mode / pacing_exclusive_fullscreen (display_mode_exclusive). menu.c: Frame pacing choices + VSync row.
- gs_replay --game sets PS2_GS_GOVERNOR=0 (replays are not real time).

## Status: complete (2026-10-23)
- Gates after the merge: run_tests 16/16; every tests\unit suite passes (window / warp -UnitOnly; pacing 71, menu 220,
  window 524, gs-hw); gpu_diff_all mixed S=1 28/28; gpu_game_all S=1 4196 pictures 0 differ; debug + release builds;
  package.py (dist\Sly2\sly2.exe 0.1.757). gs-hw, debug build and package re-run after the last present change.
- F5 (console, borderless, 7x fixed): 7x -> 3x within ~1.5 s of the first 3D frames (43-55 VBlanks/s for 2 s, the
  warm-up), then 59.9 throughout. F6 (packaged portable build, defaults, fullscreen): 59.9 VBlanks/s after boot; the
  frame statistics stopped after 2 s (window probably covered by the owner's windows) -> auto now presents immediately
  when no statistics arrive for 5 s.

## Left / owner decisions
- The owner's display runs a fixed 165 Hz for the game (VRR not active). Smooth needs either G-Sync Compatible / FreeSync
  on (NVIDIA Control Panel "Set up G-SYNC" + the monitor's OSD), or Exclusive fullscreen / fullscreen_refresh = auto
  (120 Hz; implemented, validated with CDS_TEST only - not switched on the owner's monitor while they used it).
- Starting at an unsustainable fixed scale costs ~1.5 s of slowdown at the first 3D frames (the warm-up that protects
  against NVIDIA idle clocks); the last good scale is not remembered across starts.
- Flicker-filter variant with equal DBY and DY 50/51 (both circuits) is not stripped by flicker_filter = off (blend, no
  shake) - a renderer item.
## Findings / traps
- Frames cannot be dropped: each frame's GS draws build GS memory later frames read. Only presents are optional.
- Sly 2 draws a new game frame every VBlank in the ep1 hub (60.9 flips/s), so the GPU budget is 16.7 ms.
- Ready latency (VBlank -> picture's GPU work done) is ~2 periods at 3x (frame closes at the VBlank, then render + GPU).
- Paced present on the render thread delays the next frame's submission: needs exec < period; the queue must be deeper
  by ceil(D/P), and "behind" = more queued than that. The ready sample must exclude the time the frame sat in the queue.
- NVIDIA idle clocks: the first 3D frames after the logos take 50-70 ms at 3x -> the overload rule needs a warm-up.
- GPU timestamps include other processes' time slices: another agent's game on the GPU inflates "load" and makes the
  renderer fall behind -> governor uses skipped presents / game waits as evidence.
- PowerShell: [IO.File] relative paths resolve against the MAIN checkout (lost edits once) - always absolute paths.
- Owner's sly2.ini now: internal_scale = 8 (fixed), filters faithful, aspect 4:3. Display modes at 2560x1440: 165, 144,
  119, 59 Hz (119.88 = 2 x 59.94: fullscreen_refresh = auto would pick it).
