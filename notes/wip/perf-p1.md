# perf-p1: exact depth with early rejection (product) - checkpoint

**Status: complete** (2026-10-23). Write-up: notes/d3d11-renderer.md appendix P (design, exactness proof per Z format,
tests, gates, numbers); notes/perf-plan.md "P1 results".

**Goal**: productise the pc-research early-depth prototype (z / 2^20 encoding, conservative SV_DepthLessEqual with a
vertex upper bound) everywhere depth matters, prove exactness per Z format with fallbacks, rewrite the depth conformance
kernels, measure 3x / 4x before/after, all gates green.

Branch next/perf-p1 (worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p1), from main 4e2158e + next/pc-research.
Builds: build\gsr_base (main 4e2158e runtime, git archive in build\base_src), build\gsr_proto (prototype), build\gsr_p1.
(This branch's scripts/gs_replay.c prints zb_sat / zb_rov: rebuild the base with build\base_src's gs_replay.c if needed.)

## Done
- Encoding min(z, 2^20) / 2^20 everywhere (int64.hlsli, gpu_front.h gpu_z_encode, clears, ps_zresolve), saturation +
  cs_zsync sentinel (zsync.hlsl), Z24/Z32 above GPU_Z_FF_MAX -> ROV (sticky z_high), gpu_front_zbound (vertex bound /
  no bound / ROV), vcache_nb, bounds in every FF VS (tri, sprite, line/point, thin), PRIM 3 SV_DepthLessEqual,
  PS2_GPU_EARLYZ=0, counters zb_sat / zb_rov.
- gpu_conf.c + conformance.hlsl rewritten (enc/dec/bounds bits, D32 round trip + ties vs true stored values, zsync
  Z16/Z24/Z32, triangle bound incl. degenerate / steep / ROV decision); gs-draw t_zlimits (4 formats).
- m9_exact identical S1/S3, --thin S3/S4, EARLYZ=0; all gates green (list in appendix P).
- Measurements in appendix P / perf-plan (3x ep1 9.48 -> 6.41, 4x 15.03 -> 9.87 ...).

## Findings / traps
- (pc-research) PowerShell Set-Content utf8 writes a BOM; fxc rejects it.
- (pc-research) SV_DepthLessEqual needs the PS SV_Position `noperspective centroid`.
- A PSMZ24 host->local transfer packs 3 bytes per pixel; to put whole words (top byte) into a Z24 buffer upload PSMZ32.
- RWByteAddressBuffer.InterlockedMin is signed only with an int operand (asuint(...) makes it unsigned).
- gpu_prof_ab's ms (PS2_GPU_PROF=2) are inflated (it serialises CPU and GPU): use m9_bench for ms, prof for PS counts.
- The GS integer Z of a degenerate sliver can wrap / overshoot far beyond its vertices (int64 dx * px): "FF fragments <
  2^20" needs the per-triangle proof, not just the vertex Z rule. A bounding-box bound was too loose (false ROV
  triangles in every recording); the affine bound at the three vertices is exact and gave 0.
- Measured hardware depth interpolation error < 1 unit of 2^-20 (min slack 127 of the 128 margin) on the RTX 4060 Ti and
  WARP; without the slope limit violations start at ~4M per pixel.
- gpu_game_all S=3 failed once (ep2_photo, absurd pixel count) while other GPU tests ran beside it; not reproducible alone.
