# P6 in-game CPU overhead (branch next/p6-pgo, worktree sly2-worktrees\p6-pgo)

**Goal**: build-level (ThinLTO, PGO, -march / multiversioning) and hot-path CPU optimisation of the recompiled game +
runtime; EE / frontend / worker ms per frame before -> after in ep1 hub, m1 brawl, m2 boardroom; all gates green.

## Done
- (nothing yet)

## In progress
- baseline release build in build\rel (6 jobs)

## Next
1. Baseline build + profile (PS2_PROFILE=1 / worker / front) in ep1 hub (chaos_bench scenes).
2. ThinLTO, PGO, -march / multiversioning, hot attributes, ps2_call dispatch, allocator / locks.
3. chaos_bench A/B (ep1_hub, m1_brawl, m2_boardroom) interleaved, medians.
4. Gates: run_tests, tests\unit, gpu_diff_all mixed S=1, m9_exact, pc_text_boot, debug + release, package.py,
   make_installer.py.

## Findings / traps
- build_game.py used os.cpu_count() compile jobs: added --jobs (shared machine: 6).
