# WIP: pc-pacing (branch next/pc-pacing, worktree sly2-worktrees\pc-pacing)

**Goal**: game speed independent of rendering (59.94 VBlanks/s at any internal scale), smooth presentation on
high-refresh / VRR displays (owner: 2560x1440 @ 165 Hz, RTX 4060 Ti), GPU cost at 1440p per preset, runtime
interfaces for the Options menu (gfx_set_internal_res, present_set_mode ...). Brief: notes/pc-experience.md.

## Done
- Read the notes and code (gs_thread.c gs_queue_vsync, gpu_exec.c gpu_submit_sink, gpu_present.c present_fit,
  gpu_hw.c auto scale, ps2_runtime.c host_timer, window.c fullscreen).

## Findings so far (code reading; to be confirmed by measurement)
- Root cause 1 (game slows at high scale): EE waits in gs_queue_vsync for the previous VBlank's picture_done;
  with the GPU renderer picture_done = frame queued for the render thread (gpu_submit_sink), and the sink BLOCKS
  while `inflight >= limit` (2, 6 for 2 s after a slowdown). A GPU slower than real time therefore back-pressures
  render thread -> sink -> frontend -> GS thread -> EE: the game runs at the GPU's speed (documented in the sink:
  "the EE then waits: the game slows down"). Owner log: F2 up to 8x (11x16 samples per pixel at 16:9).
- Frames can't be "dropped" freely: each frame's GS draws build GS memory state later frames read (feedback,
  RTTs, EE read-backs). Only the present step (compose + resample + Present) is optional.
- Present today: inside each frame's execution, waitable object (latency 1, waits up to 100 ms), Present(1) or
  Present(0, tearing) for pacing=vrr. No pacing to a cadence: present time = when the frame happened to finish.
- Owner's sly2.ini now: internal_scale = 8, filters faithful, aspect 4:3 (left at 8x: next start unplayable).

## Next
1. Build release in the worktree (build\rel), measure baseline: VBlanks/s (PS2_PERF=1) and GPU ms
   (PS2_GS_HW_STATS=1) at 1x/3x/4x/6x in a heavy scene (warp ep1/ep3 hub), windowed 2560x1440.
2. Design + implement (see plan below), unit tests, measurements, docs (pc-experience.md Interfaces).

## Plan
- A. Never stall the game on the GPU: governor that lowers the effective internal scale quickly when the
  render queue backs up (fixed scale = maximum), present skipping when behind, measure.
- B. Paced presentation: present picture v at t_vblank(v) + D (adaptive), Present(1) (VRR displays then show
  even 16.68 ms frames), VRR detection from DXGI frame statistics, fixed-refresh fallback, vsync on/off/adaptive.
- C. Interfaces for the menu: internal resolution list with pixel sizes, gfx_set_internal_res, present_set_mode.
- D. Optional: exclusive fullscreen at a 60-multiple refresh (120 Hz) for non-VRR high-refresh displays.
