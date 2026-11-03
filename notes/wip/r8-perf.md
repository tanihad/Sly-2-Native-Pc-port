# r8-perf: GPU time of the Modern renderer at the Balanced preset (MFT 100%, MSAA 4x, 2560x1440)

**Goal**: >= 300 fps in hubs at Balanced (ep1 hub ~258 fps / 3.79 ms GPU, m2 boardroom ~172 at the start). Exact
faithfulness gates unchanged (tests/r2/gates.json via r2_sweep ab+hc+tf; Reference identical: m9_exact).
Worktree C:\Users\tanih\Projects\sly2-worktrees\r8-perf, branch next/r8-perf (from main 38a38df).

## Done
- Tools (gpu_exec.c): `PS2_GPU_PROF_OPS_N=<n>` + `PS2_GPU_PROF_OPS_MFT=1` (op signatures averaged over n MFT frames);
  `PS2_R8P_SKIP=<m1>,<m2>,..` with `PS2_GPU_AB=<frames>`: phases cycle "nothing skipped" / each R8S_* mask left out
  (prof-skip line: the cost of each item, one run); `PS2_GPU_AB_R8=<mask>`: only R8P_* features follow the A/B phases
  (MFT stays on in both); `PS2_R8P=<mask>`: features on; per-phase picture GPU time without PS2_GPU_PROF ("ab-gpu"
  line, gpu_present.c ts_complete; gpu_frame_stats.ab_phase).
- Profile ep1 hub Balanced (machine shared, GPU ~4x inflated; shares): R2 ~31%, shadow (svol + fuse) ~21%, exact FF
  triangles (sky, characters) ~22%, outline line pixels ~6%, blur ~4%. R2: ~5500 draws/frame of 1-6 triangles
  (grass/leaves: per sub-batch pass A pass-half 5F74, fail-half 5F64, pass B additive 1F42); skipping the raster
  (empty scissor) saves only ~40% of R2, 3-index draws cost as much as full ones: **draw count is R2's cost**.
- R2 plan (gpu_r2front.c r2_plan, gpu_front.h gpu_r2_chunk, r2.hlsl r2_vs_pull / vs_r2m_pull, r2_render.c plan
  loop, mesh table 16 B/mesh with vbase/ibase, arena VB/IB raw SRVs t20/t21): exact reordering of draws with disjoint
  screen boxes (tile levels), runs of one state merged into instanced pulled draws. ep1: 5444 -> 3043 GPU draws
  (TS 16), 2614 (TS 8), 2945 (TS 4): overlap-limited. A/B (prof): R2 1.67 -> 1.28 ms; ab-gpu -0.78 of 19.6 (loaded).

- `PS2_R8P_VERIFY=<n>` (gpu_exec_mft.h vf_*, mft.hlsl cs_mft_cmp): every n-th MFT frame each optimised step runs twice
  (optimised, then original from the same surfaces), compared per sample; R2 plan, shadow sequence, line-pixel runs all
  identical in ep1 (thousands of checks); negative test PS2_R2_PLAN_REVERSE=1 is caught.
- Shadow volumes (R8P_SHADOW=4): SVOL_LITE perms (flat colour / TEST / ALPHA from record + state when the G / A planes
  have no gradients), counters / DSV / colour SRV kept bound across the volume draws (X.mode 10). svol 0.79 -> 0.64 (prof).
- Line-pixel runs (R8P_LINEPIX=2, lm_plan / lm_exec, vs_mft_linepix_m + table t26): 2.5 M -> 0.8 M draws, ~5% of
  ff-line time only. Bugs fixed: m_cfg.z left at the table offset for plain draws; the iota (DRAWID) buffer must cover
  slot + instances (R2 too: data / 32 + ndraw).
- R2 plan boxes from the draw's vertices when the AABB reaches behind the camera: ep1 5167 -> 2542 GPU draws.
- mft_tri_frag rows / steps in wide loads (same words).
- All features A/B ep1 (no prof, loaded GPU): 12.12 -> 11.50 ms (-5.2%).

## In progress
- Headline A/B m1 / m2 (PS2_GPU_AB=24 PS2_GPU_AB_R8=0xFF, -Secs 330, ab-gpu line).

## Next
1. Gates: run_tests, tests\unit, gpu_diff_all mixed S=1, m9_exact, pc_text_boot, debug + release, package.py,
   r2_sweep ab+hc+tf with -Parallel 1.
2. notes/modern-renderer.md "as built", final report, cleanup (build\chaos, build\r2trace, build\mftcheck).

## Findings / traps
- Build: `$env:PYTHON_CPU_COUNT='6'` limits build_game.py's thread pool (Python 3.13) to 6 jobs.
- PS2_GPU_AB alone makes the A phases grid frames (ab_off() turns MFT, texdec, lazy Z, npost, shfuse off).
- PS2_GPU_PROF=1 serialises CPU/GPU per frame (spin in prof_end: render CPU looks ~4 ms); without it render ~0.9 ms.
- Other agents run 2-3 games at once: GPU time 3-5x inflated; only in-run A/B, long runs (-Secs 400).
- PowerShell Set-Content -Encoding utf8 adds a BOM: strip it (files are CRLF in the worktree, autocrlf).
