# r2-m7b: modern renderer R7 part 2 (keep cutting GPU time in modern mode)

**Goal**: lower the GPU time of modern frames (every scene GPU-bound; ep1 hub 3x 4.04 ms, 4x 6.67 ms after R7 part 1).
Worktree C:\Users\tanih\Projects\sly2-worktrees\r2-m7b, branch next/r2-m7b (from main ba01fe2).

## Done
- Baseline in game (ep1_hub chaos, base exe = build\base\sly2_base.exe, PS2_R2=hybrid, PS2_GPU_PROF=1, prof_window):
  GPU 4.13 ms: ff-tri-fast-z 1.08, r2 1.03, sky (ff-tri-fast, 1 draw) 0.53, ref-tri (volumes) 0.35, alias16-draw
  (shuffles) 0.26, present 0.21, clear 0.15, ff-sprite 0.13, ff-line 0.12.
- Fixed-stream replay tools (scratchpad prof.ps1 / exp.ps1: gs_replay of build\r2prof\<scene>\frame.gsrec, bracket
  recordings copied from the r2-m7 worktree, `--r2-ab 1 --scale 3 --modern --aspect 16:9`, PS2_GPU_PROF=1).
- Experiments (replay ep1, per-kind ms: r2 0.455, fast-z 0.40, sky 0.134):
  - texel() returning a constant (compile-time, FF + R2): r2 -0.07, fast-z -0.07, sky -0.04 (the texture fetch's
    upper bound; in game ~0.5 ms).
  - R2 with every triangle culled in the VS (PS2_R2_DBG=64): 0.19 ms (draws + VS); a fixed blend/depth state 0.165,
    plus one pipeline 0.13: state + pipeline switches between R2 draws ~0.07 ms; constant PS (DBG=8) 0.28.

## In progress
- Decoded texture cache (gpu_exec.c): vram textures (T4/T8/CT16/...) of FF draws decoded to RGBA words once per
  content version (page generations from PATCH / DOWNLOAD ops), per-state base in StateRec pad2[0]; then R2 materials.

## Next
- Fuse shuffles 2 + 3 + darken (+ svol resolve) into one pass.
- R2 state/pipeline switches.
- Chaos before/after at 3x and 4x (ep1, ep3, ep5, m1), gates, notes.

## Findings / traps
- PS2_R2_DBG=1 (no texture at run time) does not show the texture's cost: the code stays in the shader (registers).
