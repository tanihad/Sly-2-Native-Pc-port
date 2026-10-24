# perf-p2: GPU pass costs (specialised shaders, shuffles, ROV, sample grid) - checkpoint

**Status: complete** (2026-10-24). Write-up: notes/d3d11-renderer.md appendix S; notes/perf-plan.md "P2 results".

**Goal**: cut GPU per-pass and per-fragment cost (notes/perf-plan.md P2), worst chaotic frames first, at 3x and 4x 16:9,
exact (REF bit-exact, FF within recorded tolerances), all gates green.

Branch next/perf-p2 (worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p2), from main 95e3147 (incl. P1).
Builds: build\gsr_base (main 95e3147 gs_replay; frozen runtime + scripts in build\base_src - rebuild the base with
build\base_src\scripts\gs_replay.ps1 -Runtime build\base_src\runtime, NOT the worktree's gs_replay.c (it prints new counters)),
build\gsr_p2a..g = the steps below, build\gsr_p2 = current.

## Done (each step m9_exact identical vs base; A and B on the whole corpus S=1/S=3, the others on 3-6 recordings)
- A (02adbc7) ALIAS16 copy runs: cs_a16_copy writes the shuffles straight into the target (no staging/ROV/scatter).
- B (fe5a331) specialised FF shaders SPEC_NOVIEW/SPEC_NOLOD, gpu_fop_draw.feat (GPU_DF_*).
- (1a038b4) CLUT ring 2 KB/slot (CT32 entry = one load); integer V in cs_a16_copy; ROV-tier early depth rejection
  (read-only DSV + GEQUAL, [earlydepthstencil], GPU_DF_ZNGE, ROV triangles get the proven P1 bound); p2_ab.ps1.
- (C) BL_NOCOLOR alpha-lane fills -> clears (alpha reset).
- FF/ROV rows only for used attributes. (D) SPEC_NOTEX FF + ROV permutations, ROV spec permutations (spec_level).
- (E) view-only 1:1 sprite permutations (SPEC_VIEWONLY SPEC_NOHIRES, GPU_DF_VRAM/HIRES).
- (F) cs_a16_copy thread per sample column, ky loop. (G 355c553) DC_VTX: FF spec tri shaders get draw consts + state
  index from the VS.
- Measured (min of 5 interleaved rounds, 3x): ep1 6.42 -> 4.74 (D), ep7 9.21 -> 6.66, ep3 6.89 -> 5.16, ep5 6.60 -> 4.83;
  4x (E): ep1 10.18 -> 7.37, ep7 15.39 -> 10.89.

## Final (2026-10-24)
- Also: thin lines keep the ubershaders (non-precise float planes fused differently: --thin replays moved); WARP keeps
  level 0 for level-1 draws (WARP anomaly, gs-hw t_random_states).
- m9_exact final: 28/28 S=1 and S=3, 6 at S=4, 4 with --thin S=3/S=4: identical. Gates: gpu_diff_all mixed/ref S=1/S=3 28/28,
  gpu_game_all S=1 4196 pictures 0 differ + S=3 box gate 0 failed, lockstep REF and mixed 28343 comparisons 0 differ,
  gpu_sample_all S=3 0 failed, gpu_fthread_check and -Mixed 28 identical, run_tests, every unit suite (gs-hw full hw +
  WARP, window/warp/menu -UnitOnly), pc_text_boot, debug + release builds, package.py.
- Results: perf-plan P2 results (3x quiet-run minima, paired ratios 3x/4x under perf-p3's GPU load).

## Findings / traps
- PowerShell piping `git archive` into tar corrupts it: use `git archive -o file.tar`.
- Shuffle runs (ep1): run 1 = 32 sprites 8x448 FBMSK 3FFF (A.alpha <- B.G), run 2 = 32 in-place sprites FBMSK 7FFFBFFF +
  32 sprites FBMSK 3FFF reading A; TEST 0x31001 = ATE NEVER + FB_ONLY (colour written as passing).
- "Wrong output" timing experiments that change texel alpha are invalid (alpha test / Z writes change the overdraw):
  use experiments with identical output (extra work masked by a runtime-zero constant).
- Under GPU contention gs_replay's prof drops frames > 50 ms: prof windows differ between runs; m9_bench medians double.
