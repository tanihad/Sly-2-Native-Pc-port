# r2-hud: R7 remainder, the gameplay HUD and text (prio 21, render class 5)

**Status: complete** (2026-10-30). Write-up: notes/modern-renderer.md "R7 part 3, the gameplay HUD".
Branch next/r2-hud (from main a3a5752), worktree C:\Users\tanih\Projects\sly2-worktrees\r2-hud. Not merged.

**Outcome**: profiled; the HUD is not drawn natively (its pixels are GS layering / mask / DATE rules, draws cost
0.06-0.26 ms; the fixed overhead is clears). Instead: lazy tile-aligned partial Z clears in mixed frames
(gpu_exec.c exec_clear, PS2_GPU_LAZYZ=0 off): in-run A/B GPU -0.18 / -0.27 / -0.28 ms (ep1 / m1 / m2), CPU unchanged.
Diagnostics: PS2_R2_DROPCLS follows PS2_GPU_AB_EE=1|2 phases, GPU profile "hud" column + [HUD] tag, opdump clear fields.

## Gates (final code)
run_tests 16/16; unit 25/26 (window fullscreen F11 / Alt+Enter: synthetic input, environmental); gpu_diff_all mixed S=1
28/28; m9_exact S1+S3 -Game identical; pc_text_boot rel + debug; debug + release builds; package.py; r2_sweep 22/22
(ep7 hc, m8_boss hc once failed in the full sweep, pass on re-run).

## Findings / traps
- Worktree: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python = main .venv; clang from main
  tools\llvm-mingw\bin; PATH = main .venv\Scripts;tools\git\cmd;tools\llvm-mingw\bin first. A failed build leaves the old exe.
- Base gs_replay: git archive -o x.tar a3a5752 runtime; tar -xf; gs_replay.ps1 -Runtime ... (pipe to tar corrupts in PS).
- Never Stop-Process by pattern; chaos_bench runs scenes in chaos.txt order; PS2_GPU_PROF inflates small-draw costs.
- Dropping a class's entries also skips its class begin / end functions: the A/B includes them.
- HUD Z target 4 is Z16S 512x256 rows, cleared 512x224 (not "full"): that is why the clear was the slow path.
- Single-run chaos comparisons are noise when the owner uses the GPU: use in-run A/B (PS2_GPU_AB + AB_EE=2).
