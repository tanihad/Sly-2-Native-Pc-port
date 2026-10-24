# perf-p1: exact depth with early rejection (product) - checkpoint

**Goal**: productise the pc-research early-depth prototype (z / 2^20 encoding, conservative SV_DepthLessEqual with a
vertex upper bound) everywhere depth matters, prove exactness per Z format with per-target / per-draw fallbacks, rewrite
the depth conformance kernels, measure 3x / 4x before/after, all gates green. Plan: notes/perf-plan.md P1.

Branch next/perf-p1 (worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p1), from main 4e2158e + next/pc-research.
Builds: build\gsr_base (main 4e2158e runtime, git archive in build\base_src), build\gsr_proto (pc-research prototype),
build\gsr_p1 (this branch). (gs_replay.c of this branch has the new zb_sat field: rebuild base with build\base_src's
scripts/gs_replay.c if ever needed.)

## Design (as built)
- Encoding enc(z) = min(z, L) / 2^20, L = 2^20 (enc(L) = 1.0, exact). decode(d) = d * 2^20 (gpu_front.h GPU_ZE_ONE,
  int64.hlsli ZE_ONE).
- FF fragments always have z < L: a Z24 / Z32 primitive with a vertex Z > T (= GPU_Z_FF_MAX = L - 512) sets z_high
  (ROV, zpatch as depth, sticky per target until a full clear: the old path, now also for Z24). Z16 / Z16S never exceed
  0xFFFF < L.
- Stored values >= L (clears, uploads, ROV writes) are held SATURATED in the DSV (1.0): exact for GREATER / GEQUAL against
  FF fragments (< L). cs_zsync keeps zpatch's word where the DSV holds 1.0 (FF draws never write L).
- Triangle bound: per vertex enc(min(z, L - M) + M), M = 128 (GPU_ZB_MARGIN), interpolated by the rasterizer. Used when all
  vertex Z <= min(T, format max) and |cx| + |cy| <= 8192 (GPU_ZB_SLOPE) per native pixel; else the vertex word is
  0xFFFFFFFF (bound 1.0: exact, no early rejection; counted gf.zb_sat). vcache keyed on that flag.
- Sprites / line / point pixels: enc(z + 1); thin lines: the VS bounds the PS's float plane over the line's t range.
- PS2_GPU_EARLYZ=0: draw flag 4, every bound 1.0 (A/B switch).

## Done
- Shaders: int64.hlsli (encode/decode/bounds), zsync.hlsl (sentinel), gs.hlsl (bounds in vs_ff_tri, vs_ref_sprite,
  vs_ref_linepix, vs_ref_thin; PRIM 3 SV_DepthLessEqual too), conformance.hlsl (new kernels), perms.txt.
- gpu_front.c/h: z_high for Z24+Z32 > T, nb vertices (Z16 > 0xFFFF, slope), vcache_nb, zb_sat counter; gpu_exec.c:
  clear encode (gpu_z_encode), gpu_earlyz(); gs_replay.c prints zb_sat.
- gpu_conf.c rewritten: cs_conformance (enc / dec / bounds bits), D32 round trip with SV_DepthLessEqual (raw, SRV,
  GREATER / GEQUAL / GEQUAL+write vs TRUE stored values incl. >= 2^20), zsync round trip Z16/Z24/Z32 (top byte, >= L,
  constant resolves), triangle bound (28672 triangles at 7 factors on hw, 2560 on WARP): all 0 mismatches, min slack
  127 of 128 (hw interpolation error < 1 unit); WITHOUT the slope limit violations appear only from 4.1M/pixel slopes.
- gs-draw scene t_zlimits(Z32/Z24/Z16/Z16S): sw 247/247; gs-hw full both adapters passed (before the scene was added;
  rerun pending in the gate list).
- m9_exact vs main (gsr_base): S1+S3 Every 7 all 28 identical; --thin S3+S4 identical; PS2_GPU_EARLYZ=0 S3 identical.
- hwdry: zb_sat ep1 8236 of 1.93M FF prims (0.4%), ep8 5088 (0.2%), ep7 8084 (1%); z_high 0 everywhere (Sly: Z32 <
  0x7F5C3, Z16S < 0x2D3C; no Z24).
- Measured (quiet GPU, 12% from another agent's idle game, interleaved 3 rounds, m9_bench medians, 16:9, GPU ms per game
  frame) base -> proto -> p1:
  S3: ep1 9.48 -> 6.39 -> 6.41; ep3 8.84 -> 6.88 -> 6.90; ep5 8.98 -> 6.55 -> 6.57; ep8 7.00 -> 6.90 -> 6.93; ep7 11.02 -> 9.06 -> 9.04
  S4: ep1 15.03 -> 10.02 -> 9.87; ep3 13.94 -> 10.69 -> 10.80; ep5 14.27 -> 10.25 -> 10.30; ep8 10.72 -> 10.74 -> 10.58; ep7 17.99 -> 14.51 -> 14.54
  PS invocations per game frame (gpu_prof_ab, PS2_GPU_PROF=2), total / FF depth triangles (passing) base -> p1:
  S3: ep1 47.15 -> 30.10 M (tri-z 30.04 -> 13.06, pass 11.75); ep3 42.27 -> 30.29 (24.91 -> 12.99, 11.01); ep5 46.16 -> 32.89
  (25.05 -> 11.85, 10.57); ep8 31.27 -> 30.56 (13.54 -> 12.88, 12.49); ep7 73.41 -> 61.46 (31.13 -> 19.32, 16.47)
  S4: ep1 78.57 -> 50.05 (50.05 -> 21.62, 19.60); ep3 70.51 -> 50.35 (41.53 -> 21.47, 18.37); ep5 76.95 -> 54.64 (41.77 ->
  19.63, 17.59); ep8 52.09 -> 50.91 (22.56 -> 21.45, 20.79); ep7 122.38 -> 102.12 (51.88 -> 31.86, 27.44)

## In progress
- Corpus gates (scratchpad gates.ps1 -> build\gates\summary.txt): diff mixed/ref S1/S3, game S1/S3, lockstep (REF,
  mixed), sample S3, fthread (+mixed).

## Next
1. Remaining gates: run_tests.py; every tests\unit\*\run.ps1 (window/warp/menu -UnitOnly; gs-hw full both adapters
   -NoWindow - with t_zlimits); tests\pc_text_boot.ps1; debug + --release builds (scripts\build_game.py); package.py.
2. Docs: notes/perf-plan.md "P1 results"; d3d11-renderer.md appendix P (proof per format, fallbacks, tests, numbers).
3. Final commit, Status complete.

## Findings / traps
- (pc-research) PowerShell Set-Content utf8 writes a BOM; fxc rejects it.
- (pc-research) SV_DepthLessEqual needs the PS SV_Position `noperspective centroid`.
- A PSMZ24 host->local transfer packs 3 bytes per pixel; to put whole words (top byte) into a Z24 buffer upload PSMZ32.
- RWByteAddressBuffer.InterlockedMin is signed only with an int operand (asuint(...) makes it unsigned).
- gpu_prof_ab's ms (PS2_GPU_PROF=2) are inflated (it serialises CPU and GPU): use m9_bench for ms, prof for PS counts.
