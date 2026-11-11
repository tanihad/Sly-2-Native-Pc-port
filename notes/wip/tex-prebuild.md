# WIP: Reimagined AI textures ready before first sight (no pop-in)

Branch next/tex-prebuild from main e656907, worktree sly2-worktrees\ai. Do not push / merge.
Tools: the worktree has no tools\ - put `C:\Users\tanih\Projects\sly2-decomp\tools\{git\cmd,llvm-mingw\bin}` and its
.venv\Scripts on PATH. Build: `python scripts\build_game.py --release --out build\rel` (~2 min incremental).
Baseline exe (main e656907): build\prebuild\base_sly2.exe. Model: build\models\x4plus.bin (copy of light's, never commit).
Warm seed: build\prebuild\texcache_warm (copy of light\build\reimagine\texcache, 1764 files).

## Goal
No visible Enhanced -> AI texture swap after gameplay starts (warm: 0; cold: 0 or reported), no stalls.

## Done
- Instrumentation (PS2_AI_POPLOG=1 in gpu_tex_ai.h: level from widCurrent 0x3D4A68, first 3D frame, swaps visible /
  all, first-image kind + delay histogram; PS2_GPU_FT=2 per-picture "ftp:" lines in gpu_present.c).
- scripts/tex_pop_gate.ps1 (ep2 hub walk + camera turns, owner's settings, deterministic).

## In progress
- Baseline measurements (cold / warm) with the current code.

## Next
1. Design (see Findings): (1) cached AI file -> AI image directly at first resolve (no Enhanced first); loader threads.
   (2) per-level manifest (keys seen per world) -> preload into slots at the level change. (3) cold: AI images built
   in play are applied only while the texture is not drawn (no visible swap).
2. Measure after; gates; notes/reimagined.md section; report.

## Findings / traps
