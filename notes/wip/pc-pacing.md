# WIP: pc-pacing (branch next/pc-pacing, worktree sly2-worktrees\pc-pacing)

**Goal**: game speed independent of rendering (59.94 VBlanks/s at any internal scale), smooth presentation on
high-refresh / VRR displays (owner: 2560x1440 @ 165 Hz Dell S2722DGM, RTX 4060 Ti), GPU cost at 1440p per preset,
defaults that adapt to other PCs (weaker GPUs, AMD/Intel, 60 Hz/high refresh, VRR or not), runtime interfaces for the
Options menu. Brief: notes/pc-experience.md.

**Coordinator rules**: run headless/offscreen for everything except a few batched real-monitor pacing runs
(< 60 s each, write a line here before each, close the game right after). Other agents' games share the GPU
(check `Get-Process sly2*` before measuring GPU cost).

## Done
- Root cause 1 confirmed (baseline, release build, headless, ep1 hub, pc-text agent's game also running):
  3x: EE 59.92 VBlanks/s, game flips 60.9/s, GPU 13.9 ms/picture; 6x: EE 42.8 VBlanks/s (min 29.9), game flips 29.3/s,
  GPU 36-44 ms/picture. build\pace\base_ep1_{3,6}x\run.log. Mechanism: gpu_submit_sink blocks while the render queue
  is full -> frontend -> GS thread -> EE waits in gs_queue_vsync for picture_done.
- runtime/gpu/gpu_pace.c/.h: pure logic (present delay D controller, present flags vsync on/off/adaptive, VRR
  classifier from shown-present intervals, GPU load governor for auto AND fixed scales) + tests/unit/pacing (49 checks).
- scripts/pace_run.ps1: warp + run + summary (EE VBlanks/s, game flips/s, presents/s, GPU ms).

## In progress
- Integrating the governor into gpu_hw.c (replaces auto_scale_tick; fixed scales become "maximum"; PS2_GS_SCALE env
  and PS2_GS_GOVERNOR=0 = strict fixed scale for tests); gpu_frame_stats gets the picture's scale.

## Next
1. gpu_hw.c governor + sink stall time (gpu_submit_stall_ms) in the stats line; scale status API for the menu.
2. gpu_present.c: GPU timestamps exclude the present wait (gpu_ms = draws+compose, present_gpu_ms separate);
   paced present (target = VBlank QPC + D, sleep, then Present), event query for ready time, skip present when behind,
   vsync modes, frame statistics -> pace_disp (VRR detection), PS2_PACE_LOG per-present log.
3. VBlank QPC plumbing: ps2_runtime host timer due time -> gs_queue_vsync slot -> gpu_display_info.vblank_qpc.
4. Settings: graphics.pacing auto|paced|immediate|display (vrr alias), graphics.vsync on|off|adaptive (minimal in
   settings.c); gfx.c: gfx_set_internal_res(), resolution list with pixel sizes, present_set_mode().
5. Display-driven VBlanks generalised to 60/120/240 Hz when no VRR (auto).
6. Measurements (headless first; then ONE batched real-monitor run set), presets cost at 1440p, docs, gates.

## Findings / traps
- Frames cannot be dropped: each frame's GS draws build GS memory later frames read. Only presents are optional.
- Sly 2 draws a new game frame every VBlank in the ep1 hub (60.9 flips/s), so the GPU budget is 16.7 ms.
- F2 wraps 8 -> 1 (owner cycled through 1..8 repeatedly).
- Owner's sly2.ini now: internal_scale = 8 (fixed), filters faithful, aspect 4:3.
