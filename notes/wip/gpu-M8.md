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

## In progress
- release build (build_rel.log)

## Next
(see Plan)

## Findings / traps
- gpu_diff_all.ps1 as inherited runs `-Backend both` without --ref/--mixed = the M0 all-fallback verify path
  (sw underneath), and only on the 13 original recordings (baseline.json).
- Disk: delete every dump dir after use; 3x PNG ~6 MB.
