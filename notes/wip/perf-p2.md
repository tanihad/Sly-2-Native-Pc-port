# perf-p2: GPU pass costs (specialised shaders, shuffles, ROV, sample grid) - checkpoint

**Goal**: cut GPU per-pass and per-fragment cost (notes/perf-plan.md P2), worst chaotic frames first, at 3x and 4x 16:9,
exact (REF bit-exact, FF within recorded tolerances), all gates green.

Branch next/perf-p2 (worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p2), from main 95e3147 (incl. P1).
Builds: build\gsr_base (main 95e3147 gs_replay; frozen copy of the runtime in build\base_src).

## Done
- Read perf-plan, renderer appendices F/G/I/L/P, perf-p1, pc-research.

## In progress
- Baseline gs_replay build + GPU profile (PS2_GPU_PROF=1/2) of chaos scenes and hubs at 3x / 4x.

## Next
1. Profile -> decide optimisations by measured share.
2. Implement (each step: m9_exact vs base S=1/S=3, commit).
3. Gates, notes (perf-plan P2 results, renderer appendix S).

## Findings / traps
