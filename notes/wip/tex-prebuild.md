# WIP: Reimagined AI textures ready before first sight (no pop-in)

Branch next/tex-prebuild from main e656907, worktree sly2-worktrees\ai. Do not push / merge.
Tools: the worktree has no tools\ - put `C:\Users\tanih\Projects\sly2-decomp\tools\{git\cmd,llvm-mingw\bin}` and its
.venv\Scripts on PATH. Build: `python scripts\build_game.py --release --out build\rel` (~2 min incremental).
Baseline exe (main e656907): build\prebuild\base_sly2.exe. Model: build\models\x4plus.bin (copy of light's, never commit).
Seeds: build\prebuild\texcache_warm (1764 files, no lists), texcache_lists (+ w0, w8, last.bin), texcache_lists2 (+ w2);
build\reimagine\texcache = copy of texcache_lists (the gates' warm seed).

## Goal
No visible Enhanced -> AI texture swap after gameplay starts (warm: 0; cold: 0 or reported), no stalls.

## Done
- Instrumentation (PS2_AI_POPLOG=1, PS2_GPU_FT=2) + scripts/tex_pop_gate.ps1.
- Feature in gpu_tex_ai.h "Ready before first sight" (PS2_AI_READY=0 = old flow): direct AI images, loaders, level lists
  + preload (title: also last.bin's level), class creation / growth for preloads while no 3D, own build rate, deferral.
- Measurements: notes/reimagined.md "Ready before first sight" (table, videos). Videos: build\prebuild\videos.
- Gates so far: run_tests 16/16; units 0 failed; m9_exact S1+S3 -Game all identical (base e656907 runtime,
  build\basesrc -> build\basert); gpu_diff_all mixed S=1 28/28.

## In progress
- r2_sweep Faithful (ep1, ep3, m2_boardroom x tfo, tfb, mf) + Reimagined (PS2_TEX_AI=1 ep1, m2_boardroom x tfo, mf).

## Next
1. reim_stability_gate (warm), gfx_stress_gate -Cold and warm.
2. Gate results into notes/reimagined.md; status complete; final commit; report.

## Findings / traps
- widCurrent (0x3D4A68) changes at START (the load's start), the 3D of the new level follows ~22 frames (0.2 s) later:
  a loading screen far too short to build a level's images in -> preload the title's list during the boot (~5 s of
  logos) and the last-played level's list during the title; other levels: lists at the change, direct images, deferral.
- Image classes do not exist before the first et_alloc: preloads at the boot must create them (first version: 608 of
  608 preloads "no slot").
- The statistics lines (every 1200 frames) cost the render thread up to ~40 ms: tex_pop_gate leaves those pictures out.
- The name RR is taken (gpu_reim.h): the new state is RQ. The worktree's files are CRLF: Python edits must normalize.
- Another agent's game runs (and once the owner's Setup + game) share the GPU: tex_pop_gate waits for no sly2* process
  before starting and reports any seen during the run.
