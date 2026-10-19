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

- In game (release, headless): default -> "[settings] renderer gpu", "[gfx] internal scale auto, now 3x", 47 shaders
  created in 7 ms, GPU 8.6 ms per game frame in the intro (52% busy) -> stays 3x. PS2_GPU_FAIL=device/caps/swapchain/
  shader -> one line "[ps2] GPU renderer unavailable (<reason>): using the software renderer", game runs on (audio,
  IOP fine). m9_exact with PS2_GPU_FL=11_0: 5 recordings S1+S3 identical.
- Auto scale: PS2_GS_AUTO_SLOW=2.5:20 (+ PS2_GS_AUTO_RETRY=30): 3x -> 1x (133% busy), after the factor ends 1x -> 2x ->
  3x. Found: at light load NVIDIA lowers clocks and busy alternates 8.7% / 25% per second (1x): up rule uses the lower
  quartile of 15 s; costs relative per scale from measurements (1x 8.7, 2x 38.2, 3x 52 % busy on the title): table
  {1, 4.4, 6.0, 9.4, 15.5, 22, 30, 40}; AUTO_LOW 0.70; back-off 120 s doubled per repeat. PS2_GS_AUTO_LOG=1 logs each s.
- Unit tests: window 989 checks (defaults gpu/auto, parse auto, Ctrl+F2), gs-hw 203 checks incl. startup_tests (every
  PS2_GPU_FAIL, restart after failure, FL 11_0 + no-ROV start) on hw and WARP; gs-draw scenes ref + mixed also with
  PS2_GPU_NO_ROV=1 on hw and WARP: 195/195 each. run_tests 16/16, all CPU suites pass, warp -UnitOnly 113.
- Level travel (warp.c: PS2_TASK_SET items j<task>:<world> / w<world>) + scripts/m11_long.ps1. Long session (headless,
  defaults, 1320 s, ep1 hub -> 18 hubs/jobs of all 8 episodes): private bytes 787 at the title -> 1138 after the first
  ep1 loads (t=130 s) -> 1138-1144 for the remaining 1190 s (includes ~190 MB of PS2_DUMP_FRAMES read-back buffers at
  3x); video memory local 198-238 MB, non-local 168-321 MB; targets live 2-7; 1317 stats lines all 59.9-60.0 flips/s;
  audio 1310 s 0 underruns; auto stayed 3x. Sheet build\screenshots\gpu_m11_long_route.png.
- gs_vram_hashes: sw 27/27 and hwdry identical to sw 27/27; matches tests/gpu/vram_hashes.txt.

## In progress
- In-game checks: device loss x3, no-ROV in game with verify, FL 11_0 in game, WARP game run, GUI package portable.

## Next
1. Replay gates: gpu_diff_all mixed/ref S1/S3, gpu_game_all S1 + S3 (waiver), gpu_lockstep_all, gpu_sample_all S3,
   gpu_fthread_check (+ -Mixed), m9_exact full corpus S1/S3 (+ PS2_GPU_NO_ROV=1) vs build\base4eaf.
2. m8_sweep: backend from the default (only -Sw sets it), -Scale 0 = auto; sweep 54 scenes (-Parts 2).
3. 200 s boot-to-museum regression windowed default config; debug build; package.py; GUI portable boot.
4. Appendix M; WIP complete.

## Findings / traps
- Baseline gs_replay for exactness: build\base4eaf (git archive 4eaf32f runtime scripts + a copy of .git so the script
  finds the tools); m9_exact baseline hashes cached in build\exact\base_S*_E7.
- The game path needs no ConstantBufferOffsetting (only the M2 benchmark path uses VS/PSSetConstantBuffers1) and uses
  ClearView only when available; it does need doubles (gs.hlsl dmul_r) and R8G8B8A8_UINT typed UAV loads (mem.hlsl).
