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

## In progress
- ablation gates ZH=1.0, no disocclusion, blur sep (3 runs each, v2.exe, task chain).

## Next
1. Soft-edge crawl measured on the game's shadow alone: DBG=7 red channel (scratch redcheck = stability_check on R),
   X=256, every frame, pixel blocks.
2. Gates on final code.

## Findings / traps
- Baseline gate (ef7c53c, 1 run): world 1.256x, events only 326 (A) / 417 (B) per run, one triple up to 47: bursty.
  The bursts were Sly's silhouette (view 8 drew him flat 128) moving over shadowed ground (contrast 128 vs 87) - his
  legs' animation, not shadow flicker. Hence the mask.
