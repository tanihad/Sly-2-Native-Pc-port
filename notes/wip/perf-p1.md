# perf-p1: exact depth with early rejection (product) - checkpoint

**Goal**: productise the pc-research early-depth prototype (z / 2^20 encoding, conservative SV_DepthLessEqual with a
vertex upper bound) everywhere depth matters, prove exactness per Z format with per-target / per-draw fallbacks, rewrite
the depth conformance kernels, measure 3x / 4x before/after, all gates green. Plan: notes/perf-plan.md P1.

Branch next/perf-p1 (worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p1), from main 4e2158e + next/pc-research.

## Design (decided)
- Encoding enc(z) = min(z, L) / 2^20, L = 2^20 (enc(L) = 1.0, exact). decode(d) = d * 2^20.
- FF fragments always have z < L: a Z24 / Z32 primitive with a vertex Z > T (= L - 512) sets z_high (ROV, zpatch as
  depth, sticky per target until a full clear: the old path). Z16 / Z16S never exceed 0xFFFF < L.
- Stored values >= L (clears, uploads, ROV writes) are held SATURATED in the DSV (= L = 1.0): exact for GREATER /
  GEQUAL against FF fragments (< L). cs_zsync keeps zpatch's word where the DSV holds 1.0 (sentinel: FF never writes L).
- Triangle bound: per vertex enc(min(z, L - M) + M), M = 128, interpolated by the rasterizer. Proven (appendix P) for
  triangles whose vertices are all <= min(T, zmax of the format) and whose Z slope |cx| + |cy| <= 8192 per native pixel;
  any other FF triangle gets saturated vertex bounds (pad = 0xFFFFFFFF -> 1.0: no rejection, still exact). vcache keyed
  on that flag.
- Sprites / line / point pixels: enc(z + 1); thin lines: the VS bounds the PS's float plane over the line's t range.
- PS2_GPU_EARLYZ=0: every bound 1.0 (A/B switch, draw flag).

## Done
- (nothing yet)

## In progress
- Implementing shaders + frontend + executor.

## Next
1. Shaders: int64.hlsli (encode/decode/bounds), zsync.hlsl (sentinel), gs.hlsl (bounds all VS paths, PRIM 3
   SV_DepthLessEqual), conformance.hlsl (new kernels).
2. gpu_front.c: z_high for Z24+Z32 > T, vertex saturation (Z16 vertex > 0xFFFF, slope), vcache flag, counters.
3. gpu_exec.c: clear encode helper, PS2_GPU_EARLYZ draw flag.
4. gpu_conf.c: rewrite depth conformance (round trip every format, ties GREATER/GEQUAL with conservative output, zsync
   sentinel round trip incl. Z24 top byte and values >= L, bound eta measurement on random / steep triangles).
5. gs-draw / gs-hw scene with high Z values (fallbacks).
6. Measure (before = main 4e2158e runtime, after) 3x / 4x on 5 recordings; gates; docs.

## Findings / traps
- (pc-research) PowerShell Set-Content utf8 writes a BOM; fxc rejects it.
- (pc-research) SV_DepthLessEqual needs the PS SV_Position `noperspective centroid`.
