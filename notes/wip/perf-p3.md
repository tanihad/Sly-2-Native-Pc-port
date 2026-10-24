# perf-p3: frontend split across cores (P3 of notes/perf-plan.md) - checkpoint

**Goal**: chaos benchmark (scripts/chaos_bench.ps1, tests/gpu/chaos.txt) + parallel prepare / sequential commit in the
GPU frontend (runtime/gpu/gpu_front*.c, gpu_fthread.c), output bit-identical; better 1% / 0.1% lows in chaotic scenes.

Branch next/perf-p3, worktree C:\Users\tanih\Projects\sly2-worktrees\perf-p3 (from main 95e3147).

## Done
- Chaos bench scripts: scripts/chaos_bench.ps1 (-Scout / -Record / bench), chaos_sum.py, chaos_sheet.ps1; scene list
  tests/gpu/chaos.txt (8 scenes, windows from scouting). Scout runs in build\chaos\scout (sheets).
- Tools: scripts/gpu_stream_ab.ps1 (stream hash vs a baseline exe, modes same/thread/poolN), scripts/front_bench.ps1
  (interleaved offline frontend bench; -Hold = PS2_GPU_FTHREAD_HOLD + 2 GB ring: the frontend thread runs only after
  the whole stream is decoded -> its CPU alone, low noise), gs_replay --front-pool, GS_REPLAY_CFLAGS.
- Baseline gs_replay build\p3\gsr_base (main runtime + the hold/ring-size patch in build\base_src\runtime).
- Prototype (commits 2450b73, 25e6f78): fr_prim -> gf_prepare (pure) + gf_prim_commit; pool of helpers preparing
  128-primitive segments ahead of the commit. Stream hashes identical (ep8_boss, m1_brawl, ws_ep3_hud; same/thread/
  pool1/pool3). **NO-GO**: hold-mode medians (ms per game frame, frontend thread): ep8 boss base 6.87 / split 7.37 /
  pool3 7.44; ep1 hub 6.06 / 6.38 / 6.37; m1 brawl 3.51 / 3.85 / 4.04. Why (GPU_FRONT_PROF + sampling): prepare is
  ~23-27% of fr_prim, but cutting segments costs ~15% (ring scan, state snapshots), reading the prepared results from
  other cores ~8-13% (prep data as big as its inputs), and helpers slow the GS thread (clocks / L3); SMT-sibling helper
  worse. The split alone costs 5-7%.

## In progress
- GS recordings of the chaos scenes (chaos_bench -Record, background).
- Next: revert the split/pool (keep tools), optimise the sequential frontend hot spots exactly.

## Next
1. Revert gpu_front.c / gpu_front_int.h / pool parts of gpu_fthread.c to main; keep ring size / hold / prefetch.
2. Hot spots (hold-mode profile ep8 boss, no-inline): fr_prim body 24%, sb_prim+sb_span 12.7%, own_draw 8%,
   emit_vertex 6%, texel_rect 6% (libm floor/ceil calls), gs_tri_setup 5.4%, ring/unpack ~7%.
3. Baseline in-game chaos bench (base exe), then after; gates; notes/perf-plan.md P3 results.

## Findings / traps
- NEVER use .NET file APIs ([IO.File]::...) with relative paths: their current directory is the MAIN checkout (an empty
  scripts\chaos_bench.ps1 landed there once; the coordinator deleted it). Absolute worktree paths only.
- Direct job starts: title intro ~50-60 s, then the job's intro cutscene (m1_boss ~3 min, START does not skip; m3_boss
  ~30 s, m5_boss ~50 s, m7_boss ~75 s). Scouting with 4 games in parallel + dumps: fps numbers meaningless.
- Offline thread-bench (hwsubmit) is noisy (+-10%) and GS-limited; use front_bench -Hold for frontend A/B.
- base_src from git archive has CRLF line endings.
