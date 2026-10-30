# r2-hud: R7 remainder, the gameplay HUD and text (prio 21, render class 5) natively

**Goal**: draw the gameplay HUD natively through R2 / r2gpu (widescreen anchors kept) so the exact path is idle in
gameplay frames; menus, loading, Binocucom RTT, ep2 photo, portraits stay exact. If profiling says the HUD is cheap,
do the smallest correct version and report the numbers.
Worktree C:\Users\tanih\Projects\sly2-worktrees\r2-hud, branch next/r2-hud (from main a3a5752).

## Done
- Release build: build\rel\sly2.exe; build\base\sly2_base.exe = main a3a5752 (copy, for A/B).
- Diagnostics: PS2_R2_DROPCLS follows the A phases of PS2_GPU_AB=<n> PS2_GPU_AB_EE=1|2 (2 = only this follows; HUD =
  render class 5 = prio 21). GPU profile "hud" column (draws kicked inside HUD_BEGIN/END = GPU_DF_HUD, clears flagged
  gpu_fop_clear.hud), "[HUD]" tag in prof-seq lines; opdump shows clear fields.
- Step 1 profile (chaos, modern, 3x, release; in-run A/B of dropping class 5, scripts in scratchpad ph.py):
  | | GPU B-A (no PROF) | frontend | GS thread | worker |
  | ep1 hub | +0.49 ms | +0.14 | +0.04 | -0.04 |
  | m1 brawl | +0.82 | +0.39 | +0.17 | +0.09 |
  | m2 boardroom | +0.55 | +0.29 | +0.11 | -0.01 |
  (PROF=1 runs agree: +0.59 / +0.68 / +0.61 GPU). Direct "hud" column (its draws + flagged clears only): ep1 0.14,
  m1 0.26, m2 0.11 ms; the draws are tiny (ep1 frame: 11 draws, 0.06 ms). The rest of the A/B delta is the class's begin
  / end work outside HUD_BEGIN: a full colour alpha clear (mask 8, value 8F056E7D), a full Z clear of target 4 (x2 per
  frame: one per 2D scene), ~0.04-0.05 ms each (4 MB at 3x: latency bound), plus the Z resolve (0.05) after each.
  HUD = ~1000-2000 vector-art prims per frame in m1 (tessellated Flash shapes), few draws.
- The HUD's pixels come from GS semantics: Z layering (GEQUAL, own Z target 4), alpha-plane masks (FBMSK FF000000 / alpha-only
  writes), DATE (ROV tier draws of 6 vertices), AA lines. A native re-implementation = re-implementing those rules.

## In progress
- EXPERIMENT (wrong pictures, to be removed): PS2_HUDCLR_TEST=1 limits the HUD's full clears to 120x60 in B phases;
  tag hudclrab. If the GPU gain is < 0.05 ms: area is not the cost, drop the idea.

## Next
- decide (likely: no native HUD; keep diagnostics; notes with numbers), gates, merge-ready commit.

## Findings / traps
- Worktree: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python = main .venv; clang from main
  tools\llvm-mingw\bin. Build: main .venv python scripts\build_game.py --release --out build\rel (~80 s cold, 15 s warm).
  Put PATH = main .venv\Scripts;tools\git\cmd;tools\llvm-mingw\bin first. A failed build leaves the old exe: check the exit code
  before starting runs.
- r2_capture.c / gpu_*.c are CRLF: use the Edit tool. Never name a PowerShell function R.
- Never Stop-Process by pattern (other agents' / the owner's runs): only your own PIDs.
- Priority -> render class (0x2BDD20): prio 21 -> class 5 (2D), 15 -> 8, 18 -> 9, 22 -> 10, 7 -> 6, 8 -> 7.
- chaos_bench.ps1 -Scenes runs scenes in chaos.txt order (not the order given); PS2_GPU_PROF=1 slows runs (serialises).
- Per-op timestamps (PROF_OPS) inflate many-small-draw costs (~1.5 us per op): do not read 515 x ff-line as 1.4 ms.
- Dropping a class's entries also skips its class begin / end functions (no class switch): the A/B includes them.
