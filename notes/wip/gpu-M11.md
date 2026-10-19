# gpu-M11: hardening and default switch (branch gpu/M11, from next/gpu 4eaf32f)

**Goal**: GPU renderer by default (3x, 16:9, Modern, 60 fps) with a robust software fallback, a no-ROV path,
automatic internal scale for weaker GPUs, bounded memory over long play, flaky iop_kernel test fixed, docs.

**Status**: in progress.

## Done
- e6e1653 iop_kernel test hardened (timestamps around the calls; run_all = iop_run until iop_run_unfinished() is 0;
  IOP-thread checks timed on the IOP thread / in IOP passes; printf redirect via _dup2 + per-process file, run before
  the IOP thread). Old test: 6/32 runs failed under 56 spinning threads + 8 parallel instances; new: 0/160. Mutants
  (hold no-op, timer without catch-up) still caught. runtime: iop_run_unfinished(), iop_pass_count().
- Default switch code (unbuilt-tested only partly): settings renderer default gpu, internal_scale default auto (0);
  gpu_d3d11.c: caps.full = FL>=11_0 + typed UAV loads + dual source + doubles (ROV optional); start-up errors
  (gpu_start_error), PS2_GPU_FAIL=device|caps|swapchain|shader, PS2_GPU_FL=11_0, PS2_GPU_NO_ROV=1; shader warm-up
  (cfg.warmup, game only: every game shader created at start); ps2_runtime prints one line
  "[ps2] GPU renderer unavailable (<reason>): using the software renderer".
- No-ROV path: gs.hlsl NOROV (RWTexture2D), perms *_nr, exec picks them (gpu_no_rov()), frontend gf_cfg.no_rov splits
  ROV/ALIAS16 draws so primitives are disjoint (rect list, triangles/thin grown by 1). m9_exact with PS2_GPU_NO_ROV=1 vs
  the 4eaf32f baseline (build\base4eaf\out\gs_replay.exe): 7 recordings S1+S3 bit-identical. GPU 3x 16:9 game-frame
  median ROV -> no-ROV: ep7 boss 11.4 -> 19.6, ep1 9.5 -> 13.3, ep3 8.9 -> 12.2, ep5 9.2 -> 12.8, ep8 boss 7.0 -> 8.9
  (draws x100: strip triangles' boxes always overlap).
- internal_scale = auto: gpu_hw.c auto_scale_tick (GS thread, per second GPU busy fraction from per-picture timestamps,
  now always on); gfx.c auto handling; Ctrl+F2 = auto, F2 steps from the scale in use; script token scaleauto.

## In progress
- Build + test start-up / fallback in game.

## Next
1. Game tests: default boots GPU 3x; PS2_GPU_FAIL each -> software, one line; PS2_GPU_FL=11_0 run; no-ROV in game.
2. Auto scale: verify lands on 3x here; force a slow GPU (env to inflate? e.g. PS2_GPU_AUTO_TEST) to see it drop/raise.
3. Unit tests: window settings tests (defaults gpu/auto, parse auto), gs-hw fallback test.
4. Long-session memory (warp hub -> job -> hub -> next episode, 20-30 min).
5. Docs (settings, install, controls, dist README).
6. Gates + in-game sweep + package + 200 s regression; appendix M.

## Findings / traps
- Baseline gs_replay for exactness: build\base4eaf (git archive 4eaf32f runtime scripts + a copy of .git so the script
  finds the tools); m9_exact baseline hashes cached in build\exact\base_S*_E7.
- The game path needs no ConstantBufferOffsetting (only the M2 benchmark path uses VS/PSSetConstantBuffers1) and uses
  ClearView only when available; it does need doubles (gs.hlsl dmul_r) and R8G8B8A8_UINT typed UAV loads (mem.hlsl).
