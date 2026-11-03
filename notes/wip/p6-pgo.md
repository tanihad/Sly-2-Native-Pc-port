# P6 in-game CPU overhead (branch next/p6-pgo, worktree sly2-worktrees\p6-pgo)

**Goal**: build-level (ThinLTO, PGO, -march / multiversioning) and hot-path CPU optimisation of the recompiled game +
runtime; EE / frontend / worker ms per frame before -> after in ep1 hub, m1 brawl, m2 boardroom; all gates green.

Results and design: notes/perf-plan.md "P6 results".

## Done
- build_game.py: --jobs (SLY2_BUILD_JOBS); release = ThinLTO + profile (runtime/pgo/sly2.proftext) for our code only
  (translated recomp/iop/vu compiled as before); --lto off / --pgo-use none; --pgo-gen; --no-vu-v3; biggest files first.
- VU x86-64-v3 copies for ks15/ks11 (recomp/vu_recomp.py V3_STICKY, runtime/vu_compiled.h, vu.c vuc_use_v3).
- ps2_call direct table; vu0_macro_flags SSE; pgo_flush (launch.h); pgo_train.ps1 (-Ship); chaos_sum EE column.
- Gates: run_tests 16/16; tests\unit all suites pass (window: only the known F11/Alt+Enter flake); m9_exact 28/28
  identical S=1/3 (rv_intro needs -Every 7: 16 VBlanks, no sample at E20, both empty) and -Game all identical;
  gpu_diff_all mixed S=1 28/28; pc_text_boot release + debug; debug + release builds; package.py; make_installer.py;
  payload sources built in a scratch "builder" folder with absolute paths (as the installer does) incl. --package.
- Offline frontend with the shipped profile: -10..-15%; GS decode -4..-9%; VU v3 -9%.
- In game ab2 (Rate 120, Scale 1, 3 repeats): differences inside the +-20% run-to-run noise.

## In progress
- ab2 3 more repeats (appends to build\chaos\ab2.jsonl), then fill IN_GAME_TABLE in perf-plan.md.

## Next
- Fill the table, final commit, clean build dirs, Status: complete.

## Findings / traps
- Profiles: EE thread is now the largest CPU stage (r2 capture ~1/3 incl. r2_src_same memcmp ~10%); no lock/allocator
  hot spot. Since r2 the exact frontend sees 5-8k prims per frame in the ep1 hub.
- IR PGO counter promotion: 10+ min per translated jump-table function -> -do-counter-promotion=false.
- Static function profile names carry the source path as given to clang -> -static-func-full-module-prefix=false.
- PowerShell: a [switch] $Ship and a variable $ship are the same variable (assigning a path made it True).
- Whole-program ThinLTO (game code too): 101 s build, 0.84 GB link, no measurable gain -> runtime only (67 s).
- build_game --package with a relative --out fails in windres (pre-existing; package.py / the installer pass absolute).
