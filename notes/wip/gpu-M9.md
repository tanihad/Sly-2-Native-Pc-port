# WIP: gpu-M9 (performance)

**Goal**: milestone M9 of notes/d3d11-renderer.md section 11 on branch gpu/M9 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M9, from next/gpu 829070e): bring the GPU renderer at 3x 16:9 to
GS <= 6 ms, render <= 8 ms, GPU <= 8 ms, frontend <= 8 ms (medians) on ep8 boss / ep3 hub / ep5 street / ep1 hub /
m7 boss / m3 heist; GPU <= 12 ms at S=4/5; frames in flight 2-3 (adaptive), memory waste cut; every optimisation
bit-exact vs the un-optimised hw output (lockstep hw vs hw at S=1 and S=3).

**Status**: in progress.

## Plan
1. Builds (gs_replay release-ish via scripts, game release build\rel); seed build\hlslc_cache from gpu-M8.
2. Baseline: replay GPU ms per phase (PS2_GPU_PROF) at S=3 16:9 on ep8 boss, ep3 hub, ep5 street, ep1 hub, ep7 boss,
   load_m3; frontend / GS profiles (gs_replay --profile, -FrontThread); in-game m8_batch on the six scenes.
3. hw-vs-hw exactness tool: dump GPU memory / target samples per checkpoint of the un-optimised build and compare
   with the optimised one (S=1, S=3).
4. Optimisations in order of measured payoff (GPU, then frontend/GS, then latency, memory).
5. Gates; appendix L in the design doc; WIP complete.

## Done
(nothing yet)

## In progress
- step 1

## Next
- see Plan

## Findings / traps
- DayZ (the owner's game) may be running: check `nvidia-smi --query-gpu=utilization.gpu` before timing runs.
