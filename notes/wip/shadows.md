# WIP: Reimagined shadows (branch next/shadows, worktree sly2-worktrees\shadow, from main 336e78e)

**Status: in progress**

**Goal**: owner reports (Look = Reimagined) - (1) overlapping shadows flick (Sly under a tree's shadow), (2) title
museum rope shot: Sly brown instead of hidden in shadow. Fix the causes (re-architecture allowed, PS2_REIM3=0 = old path);
Faithful / Retro byte-identical.

## Done
- (start) worktree has no tools\ / .venv: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; scripts use
  project.ps1 ($MainRoot) for clang / python.

- Baseline exe (336e78e) at build\shadows\base\sly2.exe (+ its build\gen\gpu_shaders.h for the block compare).
- Helper scripts (local, build\shadows\tools): title.ps1 (title / intro run, dumps), batch.ps1 (cases 2 at a time),
  sheet.ps1 (contact sheet, crops), maskstat.ps1 (mean RGB under a Faithful dark mask), trees.ps1 (ep2 path, owner
  settings, 165 fps, BMP dumps).
- Report 2 reproduced + root cause (see Findings).

## In progress
- Report 1: ep2 trees path at owner settings (build\light\shots\ep2_hub__base_reim).

## Next
1. Report 1: look at the frames / video, find the causes, build the overlap measure.
2. Fix report 2: game-lit pixels (lit R2 entries) get no local light (mask pass of lit entries from the camera).
3. Fix report 1; PS2_REIM3=0 = old path.
4. Gates, videos, notes.

## Findings / traps
- Report 2 (intro, not the title menu: the museum rope shot is VBlank ~2700-3300 of a boot with no input; deterministic
  with PS2_FRAME_DT_FIXED=1, dump VBlank 3100 / 3200). Sly's silhouette mean RGB (Faithful dark mask, rect 800,250,700,800
  of the 2389x1344 dump): Faithful 6.3/5.5/6.0, Reimagined 17.2/11.5/7.8 (brown). Ablations: rim+spec off 17.1/11.5/7.8,
  bloom/fog/grade/shafts/CAS/relight/fire off: unchanged; **PS2_REIM_LIGHTS=0: 5.5/4.5/5.0** -> the local lights cause it:
  lin += (lin + floor 0.025) x E lights the black silhouette from the museum's lamps. But the game already lights every lit
  entry (characters, lit props) with its own best four point lights in the VU (0x1A4478) - the post pass double-counts them
  on characters, and overrides the game where it deliberately leaves a character dark.
- Parallel game runs: at most 2 at once (7 at once: VRAM full, the game crawled - no dumps).
