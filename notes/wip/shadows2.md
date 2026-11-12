# WIP: Reimagined shadows, part 2 (branch next/shadows2, worktree sly2-worktrees\shadow2, from main ef7c53c)

**Status: in progress**

**Goal**: world (ground) shadows flicker less than main's old path under motion (reim_shadow_gate view 8, median <= 0.8x),
stacked / character-light gains kept; the game's soft shadow edges (Enhanced lighting) stable under motion; the gate
judged on the median of >= 3 runs.

## Done
- (start) git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe (env.ps1); build: `& $ProjectPython
  scripts\build_game.py --release --out <wt>\build\rel` (1.6 min incremental). Baseline exe (ef7c53c) at
  build\shadows2\base\sly2.exe (+ gpu_shaders.h). build\models\x4plus.bin, build\reimagine\texcache copied from main.
- scripts/reim_shadow_gate.ps1: -Runs N (default 3), median verdict + range, -Limit8 (world-shadow, 0.8), -Heat
  (reversal maps per phase, build\light\shadow_gate_heat), -Tag.

- Soft edges: mft.hlsl mft_soft_cov_fixed (16 samples, one fixed spiral), flag sf_ep.w (gpu_exec_mft.h ep[3] =
  gpu_reim.h reim_soft_stable(): Reimagined + v3 by default, PS2_REIM_X=256 alone, PS2_LIGHT_SOFT_STABLE=0/1 forces).
- Knobs: PS2_REIM_ZH (z history share, 0.65), PS2_REIM_BLSEP=1 (blur2 keeps game-lit / world apart) - r_ptaa.z / .w.
- View 8 paints the game-lit pure red; stability_check arg 7 "marks" (red read as 128 = old metric) / "mask" (blocks at
  and around red left out); the gate's world number is the masked one, the old one printed as "with the silhouettes".
- build\shadows2\v1.exe = the above.

- G1 (v1, soft stable off in both: the rework vs old, new metric): world masked 0.696x (0.441..0.722), silhouettes
  0.667x, char 0.738x, stacked 2.35 -> 0.36 %. G2 (v1 default, soft stable in B): world 0.992x (0.767..1.333) - but
  cross-run noise: the in-run soft A/B (PS2_REIM_X=256, 1 run) gave world 0.552x; pixel-scale metrics (block 1/2,
  thr 3..12) A vs B within 3 %.
- stability_check arg 8 = block size. v2.exe: ablation bits PS2_REIM_X=512 (A z history 0.65 / B PS2_REIM_ZH),
  1024 (B blur keeps game-lit apart), 2048 (B no disocclusion).
- DBG=7 on the trees path: nearly every world shadow is OURS (the key light's per-frame map: trees, posts, planks are
  per-frame casters); the game's volumes are only Sly's shadow and a small patch -> the soft-edge change cannot show in
  this gate. Masked bursts = our map's shadows popping for 1 frame / whole post faces flipping lit <-> shadowed.

- ZH=1.0 vs 0.65 in-run (X=512): 0.70 / 1.06 / 2.03 (uncapped 12/255 metric: useless noise). Disocclusion / blur-sep
  ablations stopped (metric too noisy to tell).
- Metric study on 3 kept runs (v2, X=64, old vs rework): uncapped 12/255 masked 1.08 / 0.57 / 1.26; capped (<= 5 events
  a triple) at 6/255 masked 0.977 / 0.968 / 0.950 -> the gate's world number now = capped 6/255 masked (stability_check
  arg 9 = cap). The rework's world shadows are ~0.97x old: equal. The bursts (A and B alike, same moments): our map's
  shadows popping in for ONE frame (a blob of a far caster), thin post / plank faces flipping.
- v3.exe: ps_reim_shtaa median(this frame, last frame's raw term, history) per channel (RT_RAW copy, t12, r_v3b.y;
  PS2_REIM_POP=0 off, PS2_REIM_X=4096 A off / B on).

- pop filter alone (X=4096, 1 run): capped 6/255 0.975x - the small reversals are content.
- v4: view 8 also marks "fresh" surfaces (last frame's depth at the reprojected point differs: posts / planks sweeping
  across the ground); RT_PW copy moved after the composite (bound at t30 for view 8). v4 rework-as-main vs old, 1 run:
  12/255 masked 1.435x.
- v5: wider tent PCF (9 gathers, 6x6 texels) + slope bias 3.5 (r_v3b.w, PS2_REIM_PCF=0, X=8192). v5 + ZH 1.0 vs old,
  3 runs: world 12/255 on persistent surfaces 0.39x (0.37..0.565); 6/255 capped 1.009x (0.913..1.033: content, plants);
  char 0.747x; stacked 2.32 -> 0.33 %.
- v6 = v5 + ZH default 1.0; gate world number = 12/255 masked (characters + fresh).
- Soft edges: trees path has almost no game volumes (DBG 7: only Sly's shadow + a patch) -> measured in the museum intro
  (ep1_boot, VBlank 2750+500, DBG=7 red channel = the game's shadow alone, X=256): stable pattern B vs noise A: pixel
  reversals thr 4: 2.97 -> 2.54 per 1000, 2x2 thr 6: 1.41 -> 1.08, mean pixel change (motion) 0.534 -> 0.458 (-14 %).
- Cost (v6, X=13056 = all new parts A off / B on, ZH via 512; parallel with another game, so absolute ms high): B - A
  +0.10 ms (reim-shadow +0.04, mft-shfuse +0.02).

- Clean cost run (v6 alone, X=13056): prof-ab 6.443 -> 6.464 ms; per frame mean 6.489 / 6.465, p99 9.50 / 9.60.
- Final gate (final.exe, 3 runs): PASS - world 0.682x (0.583..0.979), stacked 2.33 -> 0.33 %, char 0.738x.
  main-equivalent (POP=0 PCF=0 ZH=0.65 SOFT_STABLE=0) same gate: world 1.786x (1.221..3.968), char 0.848x.
- Stable soft edges moved to SOFT=2 permutations (ps_mft_shfuse*_s2) for the non-GSM path: Faithful's 341 shaders
  byte-identical to ef7c53c (block compare: 8 Reimagined-only differ, 3 new). final2.exe.
- Gates: run_tests 16/16; 28 unit suites exit 0; gpu_diff_all 28/28; m9_exact S1+S3 -Game all identical (base = main's
  build\gpu_diff_mixed_s1\gs_replay.exe copy). Notes: reimagined-lighting.md "Shadows rework, part 2".

## In progress
- chain: r2_sweep Faithful + Reimagined (ep1, ep3, m2_boardroom x tfo, tfb, mf), reim_stability_gate warm + cold,
  gfx_stress_gate -Cold.

## Next
1. Soft-edge crawl measured on the game's shadow alone: DBG=7 red channel (scratch redcheck = stability_check on R),
   X=256, every frame, pixel blocks.
2. Gates on final code.

## Findings / traps
- Baseline gate (ef7c53c, 1 run): world 1.256x, events only 326 (A) / 417 (B) per run, one triple up to 47: bursty.
  The bursts were Sly's silhouette (view 8 drew him flat 128) moving over shadowed ground (contrast 128 vs 87) - his
  legs' animation, not shadow flicker. Hence the mask.
