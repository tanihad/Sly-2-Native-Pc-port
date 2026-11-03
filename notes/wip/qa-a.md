# qa-a: visual QA of the Modern renderer, episodes 1-4 (+ menus / transitions)

Branch next/qa-a (main 38a38df), worktree C:\Users\tanih\Projects\sly2-worktrees\qa-a. Part B (ep 5-8) is qa-b.
Harness only; no renderer fixes. Evidence under build\qa\evidence (not committed): batch1..3.txt (run summaries), PNG pairs.

## Harness
- scripts\qa_run.ps1 (cell: scene x cfg x rate; sampled mode by default, -Tf = every VBlank + flicker_check, -Seq = live
  toggles via PS2_INPUT tokens, segments.txt), scripts\qa_cmp.c (--diff clusters, --pair, --crop, --sheet), scripts\qa_scenes_a.txt.
- runtime\gpu\gpu_exec_mft.h: PS2_MFT_LOG_MAX (diagnostics only). flicker_run.ps1: -DumpEvery, -Seconds.
- Traps: (1) governor lowers fixed scales on a loaded GPU -> PS2_GS_GOVERNOR=0 in qa_run; (2) frame dumps perturb the game:
  ~6-8 "not modern" grid VBlanks follow every dump at 60 fps, so sample (every 20-25th) instead of dumping each VBlank;
  (3) loading is real-time: hub X presses at +1500 are too early on a loaded machine, scenes need extra presses and a later At;
  (4) 6 agents: 120 fps scenes run slower than real time, give -Seconds; (5) `menu` token in headless closed the pause menu
  instead of opening Options (Options not captured); (6) different runs of one scene are not frame-aligned (idle animations
  differ), so Modern-vs-Exact pixel diffs only work on static screens (pause screen: matched, 78 blocks of 14400).

## Results (all runs: preset or custom combos, ep1-4 hubs, ep1 boot sequence, pause menu)
- ep1-4 hubs Balanced 60 fps with live toggles (outlines thin / ps2, filters faithful / modern): tf PASS, no flicker, no seams.
- ep1 hub thin outlines at 120 fps, 150 consecutive pictures: tf PASS (spike 0.0012, blobs 3). Quality / Performance (ep3): PASS.
- Finding 1: filters = faithful in Modern -> every frame on the integer grid (confirmed ep1, ep2, ep3, ep4).
- Observation 2: isolated / burst "not modern" grid VBlanks in steady hubs at 120 fps without dumps (ep1: 11651, 11702-04,
  11771, 12303, 12312, 12964, 13932, 13984; with dumps a 20-frame burst 7324-7343); cause = VBlank with no R2 ops; load dependent.
- Pause screen (MFT picture vs Reference): same placement and art. Boot sequence (movies, title, loading, safehouse) looked normal.
