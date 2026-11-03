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

## In progress
- instrumented build build\pgogen (very slow: funcs_*.c take 7+ min each instrumented)

## Next
1. pgo_train.ps1 on ep3_hub,ep5_hub,m3_boss,m5_boss,m8_boss; merge; check profdata size.
2. Builds: code+O2, code+LTO, code+LTO+PGO; chaos_bench interleaved on ep1_hub,m1_brawl,m2_boardroom (2-3 repeats).
3. Decide what ships (PGO legal/size/build-time), VU x86-64-v3 multiversion (vu_replay verify) if time.
4. Gates.

## Findings / traps
- Profiles (ep1 hub, release, 3x, other agents loading the GPU: 50-118 fps, GPU-bound): EE thread busy ~2.2 ms/frame,
  half recompiled game code, rest runtime spread thin (vu0_macro_flags ~8% of busy, r2 capture ~5%, ps2_call ~2.5%);
  worker: VU1 microcode ~60%, VIF unpack ~15%; frontend: fr_prim 31% self, then sb_prim, own_draw, pass_items,
  gf_r2_flush, ring (fthread_main + done_rec ~9%); GS thread: decode spread (px_prim, send_state, gs_px_write...).
  No lock or allocator hot spot: ntdll time is waiting (GPU-bound). Since r2 (modern renderer) the exact frontend sees
  5-8k prims per frame in the ep1 hub (perf-plan's 35k was before), frontend ~1.3-2.9 ms.
- Per-frame CPU ms vary 2x between runs of the same build (other agents' load, scripted play content): A/B must be
  interleaved, repeated, medians.
