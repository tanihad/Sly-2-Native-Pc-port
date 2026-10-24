# perf-p2: GPU pass costs (specialised shaders, shuffles, ROV, sample grid) - checkpoint

**Goal**: cut GPU per-pass and per-fragment cost (notes/perf-plan.md P2), worst chaotic frames first, at 3x and 4x 16:9,
exact (REF bit-exact, FF within recorded tolerances), all gates green.

Branch next/perf-p2 (worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p2), from main 95e3147 (incl. P1).
Builds: build\gsr_base (main 95e3147 gs_replay; frozen runtime + scripts in build\base_src - rebuild the base with
build\base_src\scripts\gs_replay.ps1 -Runtime build\base_src\runtime, NOT the worktree's gs_replay.c (it prints new counters)),
build\gsr_p2a (step A), build\gsr_p2b (A + B), build\gsr_p2 (current).

## Done
- Read perf-plan, renderer appendices F/G/I/L/P, perf-p1, pc-research.
- Baseline profile (PS2_GPU_PROF=2, 3x, per VBlank, GPU shared): ep1 hub ff-tri-fast-z 1.43 / alias16-draw 0.72 + alias16
  0.21 / ff-sprite 0.50 / ref-tri 0.19 of 3.62 ms; ep7 boss ref-tri 1.41 of 5.33; ep8 boss alias16 0.91 of 3.91.
- scripts/m9_bench.ps1: chaos recordings (m5_battle, m1_brawl, ep6_rc, ep2_hub) + worst-frame column.
- **A. ALIAS16 copy runs** (commit 02adbc7): gpu_front.c a16_copy_prim / alias_prim batches (CB hazard rects, <= 40
  sprites per draw), gpu_fop_alias16.mode, gpu_exec.c exec_a16_copy, alias16.hlsl cs_a16_copy (atomic AND/OR of the
  written bits straight into the target, no staging/gather/ROV/scatter). PS2_GPU_A16_COPY=0 = old path.
  m9_exact 28 recordings S=1 and S=3 every 7: all identical.
- **B. specialised FF shaders**: gs.hlsl SPEC_NOVIEW / SPEC_NOLOD; perms ps_ff_{t001,t011,t101,t111,s..,l..,n..}s;
  gpu_fop_draw.feat (GPU_DF_VIEW / GPU_DF_LOD, OR of the draw's primitives, gpu_front.c), exec_draw_ff picks them;
  PS2_GPU_SPEC=0 = ubershaders. DXBC: ps_ff_t011 1584 instr / 32 temps -> t011s 960 / 15.

## In progress
- m9_exact for B (build\exact_b.txt).
- GPU timing impossible right now: perf-p3 runs 4 games + a `sly2` process (owner?) at 100% GPU since ~02:30.

## Next
1. When the GPU is quiet: m9_bench base vs A vs B (3x, 4x), interleaved; PS2_GPU_PROF split.
2. ROV-tier early rejection (P2 c): DSV read-only + conservative bound for ROV draws whose run writes Z only through
   GEQUAL/GREATER (monotone) - measure ROVSKIP=3 upper bound first (ep7 boss).
3. Sprite paths / CLUT expanded table / draw constants via interpolants - only if the profile says so.
4. (d) native sample grid at render scale 100% (5x6 at 1440p): display family ky decoupled from 2N.
5. Gates, notes (perf-plan P2 results, renderer appendix S).

## Findings / traps
- PowerShell piping `git archive` into tar corrupts it: use `git archive -o file.tar`.
- Shuffle runs (ep1): run 1 = 32 sprites 8x448 FBMSK 3FFF (A.alpha <- B.G), run 2 = 32 in-place sprites FBMSK 7FFFBFFF +
  32 sprites FBMSK 3FFF reading A; TEST 0x31001 = ATE NEVER + FB_ONLY (colour written as passing).
