# gpu-M11: hardening and default switch (branch gpu/M11, from next/gpu 4eaf32f)

**Goal**: GPU renderer by default (3x, 16:9, Modern, 60 fps) with a robust software fallback, a no-ROV path,
automatic internal scale for weaker GPUs, bounded memory over long play, flaky iop_kernel test fixed, docs.

**Status**: complete (see the end of Done; results in notes/d3d11-renderer.md appendix M).

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

- In game: device loss x3 (PS2_GPU_LOSE_AT=1500,2500,3500): recovered twice (one second at 52.9 flips/s), third ->
  software, game runs on (audio 0 underruns). No-ROV ep3 hub with the verifier (m8_sweep, 240 s): 180 comparisons, 0
  beyond. FL 11_0 in game: 59.9 flips/s, GPU 9.0 ms. WARP adapter in game: auto 3x -> 1x at v109, ~12 pictures/s,
  no error.
- Packaged GUI build (dist\Sly2 copy in build\guitest\Sly2 + portable.txt + sly2.ini with only [game] disc + a warp
  ep3 card; no PS2_* variables; Enter/Space by keybd_event): log "renderer gpu", "internal scale auto, now 3x",
  "presenting through the GPU backend's swap chain", "GPU renderer on (window ...)"; screenshot in the ep3 hub
  (build\screenshots\gpu_m11_gui_portable.png); exit 0 on close. (Title shows "(GPU)" only with show_fps.)
- Debug build (build\sly2, 29.1 MB) boots on the GPU. package.py -> dist\Sly2\sly2.exe 0.1.670.
- Replay gates (build\gates.txt, build\gate_*.txt; release gs_replay of 133a13f source): gpu_diff_all mixed S1 / S3 27/27
  (min PSNR 43.94), ref S1 / S3 27/27 (0 px); gpu_game_all S1 4136 pictures 0 differ, S3 27/27 (box gate min 33.25 dB
  without rv_prompt, rv_prompt waived 31.36, max |mse| 0.319); gpu_lockstep_all REF S1, mixed S1, mixed S3: 26402
  comparisons each, 0 differ; gpu_sample_all S3 0 failed; gpu_fthread_check and -Mixed 27/27 identical; m9_exact vs
  4eaf32f 27/27 identical S1+S3, and with PS2_GPU_NO_ROV=1 27/27 identical S1+S3 (the no-ROV path is bit-exact).

- First sweep (55 scenes, -Parts 2, -Scale 3, completed before the usage-limit stop): 50 scenes 0 beyond; beyond:
  m6_rc_battle 11/240, m6_heist_rc 2, m6_paraglide 1, m8_tntbarrel 1 (all "> 16" words, ~250) and m8_turret 1 (+-3..16
  only, > 16 0). Lockstep on the verifier recordings: ep6 ones diverge in REF too, and so do the 4eaf32f and pre-M9
  (gpu-M9\build\basert) builds -> pre-existing. Cause (PS2_GPU_PROBE 4,223 + a block trace): the T8H feedback pass of
  frame buffer 0 reads row 224 = frame buffer 0x38's row 0; target 1 xcopied that block once (frame 19) and kept it
  "current" because the serial only moves when the writer changes, while target 3 redrew it every other frame.
  Fix da2a721 (gpu_own.c blk_shared: an xcopied block's next write by its writer bumps the serial). After: lockstep REF
  and mixed 0 differ on all 13 ep6 / m8_tntbarrel verifier recordings. m8_turret = the dual-source layering class
  (owner question (a)): exact with PS2_GPU_DUAL_EXACT=1.
- Corpus: gpu-M11\build\gsrec\m6_rc_feedback.gsrec (copy of verify_003240) added to corpus.txt + baseline.json (28).
  gs-front test 10 (row past the frame buffer: xcopies again after the writer redraws).

- After the fix: gates 28/28 green (lockstep 28343 comparisons x3, game S1 4196 pictures), gs-hw 203, gs-front 25
  (test 10 fails with the fix reverted), hwdry hashes 28/28, no-ROV vs ROV current build m9_exact 28/28 identical S1+S3.
- Second sweep (final source, 55 scenes): 13260 comparisons, 3 beyond (m8_turret 1, m8_tntbarrel 2: dual-source
  layering, REF and DUAL_EXACT lockstep 0 differ), PUNT 0. Frames deleted.
- 200 s regression windowed default config: 59.9-60.0 flips/s (198 lines), memory 555-618 MB flat, 1 underrun at 0.04 s
  of audio (stream start; the software renderer does the same).
- package.py 0.1.678, debug build, run_tests 16/16. Appendix M complete.

**Status: complete** (owner questions (a) gas-cloud drift and (b) sharp distant scenery left as they are).

## Findings / traps
- Baseline gs_replay for exactness: build\base4eaf (git archive 4eaf32f runtime scripts + a copy of .git so the script
  finds the tools); m9_exact baseline hashes cached in build\exact\base_S*_E7.
- The game path needs no ConstantBufferOffsetting (only the M2 benchmark path uses VS/PSSetConstantBuffers1) and uses
  ClearView only when available; it does need doubles (gs.hlsl dmul_r) and R8G8B8A8_UINT typed UAV loads (mem.hlsl).
