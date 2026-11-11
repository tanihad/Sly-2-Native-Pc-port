# WIP: Reimagined AI textures ready before first sight (no pop-in)

Branch next/tex-prebuild from main e656907, worktree sly2-worktrees\ai. Do not push / merge.
Tools: the worktree has no tools\ - put `C:\Users\tanih\Projects\sly2-decomp\tools\{git\cmd,llvm-mingw\bin}` and its
.venv\Scripts on PATH. Build: `python scripts\build_game.py --release --out build\rel` (~2 min incremental).
Baseline exe (main e656907): build\prebuild\base_sly2.exe. Model: build\models\x4plus.bin (copy of light's, never commit).
Warm seed: build\prebuild\texcache_warm (copy of light\build\reimagine\texcache, 1764 files, no level lists).

## Goal
No visible Enhanced -> AI texture swap after gameplay starts (warm: 0; cold: 0 or reported), no stalls.

## Done
- Instrumentation (PS2_AI_POPLOG=1 in gpu_tex_ai.h: level from widCurrent 0x3D4A68, first 3D frame after a no-3D gap,
  swaps visible / all, first-image kind + delay histogram; PS2_GPU_FT=2 per-picture "ftp:" lines in gpu_present.c).
- scripts/tex_pop_gate.ps1 (ep2 hub walk + camera turns, owner's settings, deterministic; -Cold, -Seed, -SeedOut, -Video).
- Feature (gpu_tex_ai.h "Ready before first sight", PS2_AI_READY=0 = old flow): direct AI images (ES_AILOAD), loader
  threads, level lists (<cache>\levels\w<world>.bin + last.bin) and preloads (title: also the last level's list),
  deferred swaps while shown (RI_DEFER), in-play upload budget = Enhanced's while textures wait.
- Before (PS2_AI_READY=0) warm: 354 swaps after the first 3D frame (349 visible); first images AI 381 / Enhanced 507;
  GPU p99 8.64 max 10.81, render thread p99 4.51 max 26.68 ms; load 0.2 s (22 frames from START to the safehouse).

## In progress
- After runs: warm without lists (-SeedOut build\prebuild\texcache_lists), then warm with lists, cold.

## Next
1. After measurements (warm no list / warm list / cold), before cold.
2. Videos (-Video 1200) before/after warm list -> mp4 (scripts/frames_to_mp4.c) into build\prebuild\videos; look at them.
3. Gates: run_tests, units, m9_exact S1+S3 -Game, gpu_diff_all, r2_sweep Faithful+Reimagined, reim_stability_gate,
   gfx_stress_gate cold+warm (needs build\models\x4plus.bin - present).
4. notes/reimagined.md section "Ready before first sight"; progress.md entry; final commit; report.

## Findings / traps
- widCurrent (0x3D4A68) changes at START (the load's start), the 3D of the new level follows ~22 frames (0.2 s) later:
  a loading screen far too short to build a level's images in -> preload the title's list during the boot (~5 s of
  logos) and the last-played level's list during the title; other levels: lists at the change, direct images, deferral.
- The name RR is taken (gpu_reim.h): the new state is RQ.
- The worktree's files are CRLF: Python edits must normalize line endings.
- Another agent's game runs share the GPU: tex_pop_gate waits for no sly2* process before starting.
