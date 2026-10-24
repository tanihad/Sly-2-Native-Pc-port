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

- Chaos recordings (build\gsrec\chaos_*.gsrec, 8 x 240 VBlanks, 200-834 MB; frames in build\chaos\record): heavier
  than the corpus for the frontend (chaos_ep1_hub ~9-10 ms, chaos_m3_boss ~8-9 ms per game frame offline).
- Pool v2 (commit b62522a: producer-side segments, prefetched preparations): identical, no consistent gain, GS thread
  +15-30% -> NO-GO; reverted.
- Exact sequential work (commits 679a5c4, ab074d9; stream identical on the recordings checked so far): sb_prim integer
  row stepping (PS2_GPU_SB_CHECK=1: 5.5M rows, 0 differ) + empty-rectangle test, texel_rect without libm, inline vertex
  compare, own_draw shifts + per-target clean generations (gpu_own.c touch/page_tg; scan_clean now also requires
  !blk_shared - needed for ep2_photo / m6_rc_feedback identity), incremental CLUT hash (clut_note_pieces; CLUT slot
  cost 167 -> 90 Mcycles on chaos_ep1_hub), flat FF planes as constants, ring prefetch (PS2_GPU_RING_PREFETCH).
- In-game A/B run 1 (build\chaos\ab1.txt, P2 agent busy on the GPU/CPU): chaos scenes mostly GPU-bound at 3x.

- Pool v2 in game (build\chaos\pool.txt, 2 repeats): frontend stage not lower, GS higher -> no-go confirmed.
- Offline hold A/B (build\p3\bench_opt2.txt): -3..-8% frontend on every recording.
- Gates on the final source (build\p3\gates.log): gpu_stream_ab vs main 36/36 (corpus + chaos) identical in mixed S=3
  same+thread, mixed S=1 thread, plain S=1 same+thread; gpu_fthread_check and -Mixed 28/28; m9_exact -Every 7 S=1/S=3
  all identical (-Every 20 "fails" rv_intro only because its 16 VBlanks give no sample point on either side);
  gpu_diff_all mixed/ref S=1/S=3 28/28; gpu_game_all S=1 4196 pictures 0 differ; gpu_lockstep_all 28343 comparisons 0
  differ; run_tests 16/16; every unit suite (fps2_units.ps1) passes; pc_text_boot; debug + release builds; package.py.
- Notes: perf-plan.md "P3 results", d3d11-renderer.md appendix S.

## In progress
- Final in-game chaos A/B base vs p3 (2 repeats, build\chaos\final.txt) -> add the table to perf-plan.md, finish.

## Findings / traps
- NEVER use .NET file APIs ([IO.File]::...) with relative paths: their current directory is the MAIN checkout (an empty
  scripts\chaos_bench.ps1 landed there once; the coordinator deleted it). Absolute worktree paths only.
- Direct job starts: title intro ~50-60 s, then the job's intro cutscene (m1_boss ~3 min, START does not skip; m3_boss
  ~30 s, m5_boss ~50 s, m7_boss ~75 s). Scouting with 4 games in parallel + dumps: fps numbers meaningless.
- Offline thread-bench (hwsubmit) is noisy (+-10%) and GS-limited; use front_bench -Hold for frontend A/B.
- base_src from git archive has CRLF line endings.
