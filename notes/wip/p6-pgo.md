# P6 in-game CPU overhead (branch next/p6-pgo, worktree sly2-worktrees\p6-pgo)

**Goal**: build-level (ThinLTO, PGO, -march / multiversioning) and hot-path CPU optimisation of the recompiled game +
runtime; EE / frontend / worker ms per frame before -> after in ep1 hub, m1 brawl, m2 boardroom; all gates green.

## Done
- build_game.py: --jobs (SLY2_BUILD_JOBS), --lto thin (VU files kept out: -fno-slp-vectorize is a pass option),
  --cflags/--ldflags (SLY2_CFLAGS/LDFLAGS), --pgo-gen (-fprofile-generate -DPS2_PGO_GEN), --pgo-use FILE (stamp includes
  the profile hash). runtime/launch.h pgo_flush() before ExitProcess (watchdog, launch_exit).
- scripts/pgo_train.ps1 (chaos_bench with LLVM_PROFILE_FILE, llvm-profdata merge).
- ps2_call: direct table (addr - first)/4 -> fn instead of binary search (ps2_runtime.c).
- vu0_macro_flags: SSE, branchless (vu.c); tests\unit\vu passes.
- Baseline build build\rel (main code) copied to build\base\sly2_base.exe; 106 s with 6 jobs (machine busy).
- ThinLTO build build\lto: 98 s (compile 51 + link 46).

- pgo-gen: -mllvm -do-counter-promotion=false (counter promotion took 10+ min per jump-table function); instrumented
  build 104 s. First training (5 scenes, full-path static names): build\pgo\sly2.profdata 3.8 MB (5660 functions).
- VU x86-64-v3 copies: build_game compiles *_ks*.c twice (VUC_V3 -march=x86-64-v3 -ffp-contract=off), dispatchers
  pick them when the CPU has x86-64-v3 (PS2_VU_V3=0 off). vu_replay museum / rv_ep2hub: 1.81 -> 1.64 ns/pair median
  (-9%), 0 programs differ.
- Static function names in profiles: -mllvm -static-func-full-module-prefix=false (file name only; runtime basenames
  are unique) so a committed profile fits any checkout. build_game --pgo-use auto (release): runtime/pgo/sly2.proftext
  -> <out>/pgo/sly2.profdata via llvm-profdata (kept in the installer's trimmed toolchain); never fails the build.
- pgo_train.ps1 -Ship writes runtime/pgo/sly2.proftext with translated code filtered out (f_*, iop_*, vuc_*,
  generated files' statics): 0.19 MB text, ~800 runtime functions.
- Unit tests: all 25 suites except window/warp pass (gs-hw 569 s). run_tests 16/16.
- gs_replay builds for offline A/B: build\gsr_base, gsr_lto, gsr_pgo (GS_REPLAY_CFLAGS).

## In progress
- chaos_bench -Tag ab1: base / code / lto / pgo (first profile), ep1_hub m1_brawl m2_boardroom, 2 repeats.
  Very noisy (machine loaded; my unit tests ran during the first scenes).

## Next
1. After ab1: retrain with portable names (build\pgogen already rebuilt with --lto thin), pgo_train -Ship.
2. Offline: front_bench -Hold with gsr_base/lto/pgo on corpus ep1_hub/m1_brawl; vu_replay done.
3. Final build (release default: LTO? + shipped profile + VU v3); A/B base vs final, more repeats.
4. Decide LTO default; build time + LTO link RAM; notes/perf-plan.md P6 results; install.md CPU note.
5. Gates: window, warp, gpu_diff_all mixed S=1, m9_exact, pc_text_boot, debug build, package.py, make_installer.py.

## Findings / traps
- Profiles (ep1 hub, release, 3x, other agents loading the GPU: 50-118 fps, GPU-bound): EE thread busy ~2.2 ms/frame,
  half recompiled game code, rest runtime spread thin (vu0_macro_flags ~8% of busy, r2 capture ~5%, ps2_call ~2.5%);
  worker: VU1 microcode ~60%, VIF unpack ~15%; frontend: fr_prim 31% self, then sb_prim, own_draw, pass_items,
  gf_r2_flush, ring (fthread_main + done_rec ~9%); GS thread: decode spread (px_prim, send_state, gs_px_write...).
  No lock or allocator hot spot: ntdll time is waiting (GPU-bound). Since r2 (modern renderer) the exact frontend sees
  5-8k prims per frame in the ep1 hub (perf-plan's 35k was before), frontend ~1.3-2.9 ms.
- Per-frame CPU ms vary 2x between runs of the same build (other agents' load, scripted play content): A/B must be
  interleaved, repeated, medians.
