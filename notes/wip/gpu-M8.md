# WIP: gpu-M8 (episode sweep and corpus gate)

**Goal**: milestone M8 of notes/d3d11-renderer.md section 11 on branch gpu/M8 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M8): full corpus gate (24 recordings) at S=1 and S=3 with 0 PUNT, and an
in-game warp sweep with the GPU renderer at 3x over every episode (hub + one job of each kind, menus, ep2 photo),
PS2_GS_VERIFY sampling, hw vs sw screenshots + contact sheets (notes/m8/), performance table for M9; fix every
divergence / PUNT (generic fixes only).

**Status**: in progress.

## Plan
1. Builds: release game (build\rel), gs_replay via the gate scripts.
2. Corpus gate: extend gpu_diff_all (24 recordings, -Mixed/-Ref, -Scale, PUNT gate); run S=1 and S=3;
   gpu_game_all S=1 and -Scale 3; gpu_lockstep_all -Scale 3 -Mixed; FF vs REF --sample-compare at S=3.
3. In-game sweep (release, renderer gpu, 3x, PS2_GS_VERIFY sampling, PS2_GS_HW_STATS): per episode hub + jobs,
   menus, photo mission; verify dumps for hw vs sw pairs; contact sheets per episode under notes/m8/.
4. Fix divergences; record root causes.
5. Final gates: run_tests, unit suites, VRAM hashes, fthread, lockstep, debug + release builds, package.py.
6. Design appendix J; WIP complete.

## Done
- e5564c7 gpu_diff_all.ps1: -Mode mixed (default) | ref | fallback (old M0 path), -Scale N, PUNT gate (gs_replay metrics
  summary `punt`, `beyond_gate`, `scale`), baseline.json has all 24 recordings (gpu-C entries: frames/sw_hash to fill
  from the first run); gs_replay prints PUNT in the `both ref:` / `mixed:` lines.
- runtime/gpu/gpu_hw.c: PS2_GS_VERIFY_DUMP=K (> 1) writes verify picture pairs only at VBlanks % K == 0.
- scripts/m8_sweep.ps1 (one scene: warp_run -Release + hw 3x + verify + stats -> summary.txt, stats.csv),
  scripts/m8_batch.ps1 + scripts/m8_scenes.txt (the scene list), scripts/m8_sheet.py (contact sheet: sw | hw | crops).
- Release build build\rel (1220 s cold; shader cache seeded from gpu-M7\build\hlslc_cache: hlslc is the slow part).
- gpu_diff_all mixed S=1: the 13 original recordings ok with the baseline sw hashes, 0 beyond, PUNT 0.

## In progress
- gpu_diff_all mixed S=1 (build\diff_mixed_s1.txt) - gpu-C entries fail only on the empty baseline frames/hash.
- sweep batches part 0/1 of 2 (background; build\warp\m8_batch_{0,1}.txt, build\warp\m8_<scene>\summary.txt).

## Next
(see Plan)

## Findings / traps
- gpu_diff_all.ps1 as inherited runs `-Backend both` without --ref/--mixed = the M0 all-fallback verify path
  (sw underneath), and only on the 13 original recordings (baseline.json).
- Disk: delete every dump dir after use; 3x PNG ~6 MB.
