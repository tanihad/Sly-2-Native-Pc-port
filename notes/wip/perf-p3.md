# perf-p3: frontend split across cores (P3 of notes/perf-plan.md) - checkpoint

**Goal**: chaos benchmark (scripts/chaos_bench.ps1, tests/gpu/chaos.txt) + parallel prepare / sequential commit in the
GPU frontend (runtime/gpu/gpu_front*.c, gpu_fthread.c), output bit-identical; better 1% / 0.1% lows in chaotic scenes.

Branch next/perf-p3, worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p3 (from main 95e3147).

## Done
- Read perf-plan.md, pc-research.md, d3d11-renderer.md C/D/L/M, frame-rate.md, gpu_fthread.c, fr_prim.

## In progress
- Step 0: release build (build\rel), chaos recordings + in-game benchmark.

## Next
1. Chaos benchmark: per-frame frame times in game (1% / 0.1% lows), scenes list, baseline on main.
2. GS recordings of chaos scenes (build\gsrec\chaos_*.gsrec), offline frontend ms per frame (gs_replay hwsubmit
   --front-thread), worst frames.
3. Prototype: parallel prepare + sequential commit; stream hash identity.
4. Gates, notes (perf-plan.md P3 results), final commit.

## Findings / traps
- NEVER use .NET file APIs ([IO.File]::...) with relative paths: their current directory is the MAIN checkout (an empty
  scripts\chaos_bench.ps1 landed there once; the coordinator deleted it). Absolute worktree paths only.
- Direct job starts: title intro ~60 s, then the job's intro cutscene (m1_boss ~3 min, START does not skip; m3_boss
  ~30 s, m5_boss ~50 s, m7_boss ~75 s). Scouting with 4 games in parallel + dumps: fps numbers meaningless.
