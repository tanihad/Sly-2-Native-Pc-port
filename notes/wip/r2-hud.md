# r2-hud: R7 remainder, the gameplay HUD and text (prio 21, render class 5) natively

**Goal**: draw the gameplay HUD natively through R2 / r2gpu (widescreen anchors kept) so the exact path is idle in
gameplay frames; menus, loading, Binocucom RTT, ep2 photo, portraits stay exact. If profiling says the HUD is cheap,
do the smallest correct version and report the numbers.
Worktree C:\Users\tanih\Projects\sly2-worktrees\r2-hud, branch next/r2-hud (from main a3a5752).

## Done
- Release build: build\rel\sly2.exe; build\base\sly2_base.exe = main a3a5752 (copy, for A/B).
- Diagnostic: PS2_R2_DROPCLS follows the A phases of PS2_GPU_AB=<n> PS2_GPU_AB_EE=1 (HUD = render class 5, prio 21).

## In progress
- Step 1 profile (in-run A/B: class 5 dropped in A phases).

## Next
- decide: native HUD path or smallest correct version; implement; gates.

## Findings / traps
- Worktree: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python = main .venv; clang from main
  tools\llvm-mingw\bin. Build: main .venv python scripts\build_game.py --release --out build\rel (~80 s cold, 15 s warm).
- r2_capture.c / gpu_*.c are CRLF: use the Edit tool. Never name a PowerShell function R.
- Priority -> render class (0x2BDD20): prio 21 -> class 5 (2D), 15 -> 8, 18 -> 9, 22 -> 10, 7 -> 6, 8 -> 7.
- chaos_bench.ps1 -Scenes runs scenes in chaos.txt order (not the order given); PS2_GPU_PROF=1 slows runs (serialises).
