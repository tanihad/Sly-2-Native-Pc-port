# r2-m7b: modern renderer R7 part 2 (keep cutting GPU time in modern mode)

**Goal**: lower the GPU time of modern frames (every scene GPU-bound; ep1 hub 3x 4.04 ms, 4x 6.67 ms after R7 part 1).
Worktree C:\Users\tanih\Projects\sly2-worktrees\r2-m7b, branch next/r2-m7b (from main ba01fe2).

## Done
- Baseline in game (ep1_hub chaos, base exe = build\base\sly2_base.exe, PS2_R2=hybrid, PS2_GPU_PROF=1, prof_window):
  GPU 4.13 ms: ff-tri-fast-z 1.08, r2 1.03, sky (ff-tri-fast, 1 draw) 0.53, ref-tri (volumes) 0.35, alias16-draw
  (shuffles) 0.26, present 0.21, clear 0.15, ff-sprite 0.13, ff-line 0.12.
- Tools: `PS2_GPU_AB=<frames>` (gpu_exec.c: experimental features off in A phases, on in B, per-phase "prof-ab" lines;
  scratchpad ab_window.py differences them over the run's later part) - in-game A/B within one run (scene load varies
  a lot between runs). `PS2_GPU_OPDUMP=<frame>` logs every op of that frame; `PS2_GPU_PROF_SEQ=1` the per-op timings in
  order. Replays: build\r2prof\<ep1|ep5|m3> (bracket recordings from the r2-m7 worktree), exactness by
  `--game-save --vram-out` file hashes (TEXDEC/SHFUSE off vs on): identical on all three.
- Decoded texture cache (gpu_exec.c "texdec"; gs.hlsl TEXDEC, cs_texdec / cs_texdec_r2; perms ps_ff_t*d):
  FF triangle draws (specialised perms) per StateRec (pad2[0]) and R2 materials (word 15, r2_render.c), keyed by
  TEX0 + CLUT identity + page write serials (PATCH / DOWNLOAD). In game A/B (FF only): GPU 4.46 -> 4.07 ms.
  (A first measurement looked like a loss: a cbuffer layout bug decoded with the wrong CLUT - fixed.)
- Shadow pipeline fused (gpu_exec.c shfuse, post.hlsl cs_shfuse): shuffle 1 + G clear deferred past the volumes,
  counter resolve, shuffles 2 + 3, darken in one pass; feedback copy dropped. Tail-only A/B: 4.40 -> 4.27 ms.

- Chaos (base = main ba01fe2, tags r2m7b_s3 / r2m7b_s4): 3x ep1 221.7 -> 285.7, ep3 238.4 -> 320.5, ep5 261.1 -> 323.6, m1
  245.7 -> 284.1; 4x ep1 160.5 -> 194.6, ep3 152.9 -> 203.7, ep5 164.7 -> 213.2, m1 150.8 -> 191.2 fps.

**Status**: complete (2026-10-29).

## Gates (final)
- run_tests; all 26 tests/unit suites; gpu_diff_all mixed S=1 28/28; pc_text_boot; debug + release builds; package.py;
  r2_sweep ab + hc 22/22.

## Next (see notes/modern-renderer.md "R7 part 2 as built")
- R2 draw / state-switch overhead, exact triangles' record stepping, the sky (0.38 ms), the volumes (0.37 ms).

## Findings / traps
- PS2_R2_DBG=1 (no texture at run time) does not show the texture's cost: the code stays in the shader (registers).
- R2 with every triangle culled in the VS still costs ~0.19 ms (replay): draws + state/pipeline switches (~0.07).
- HLSL: an early `return` inside tex_level made fxc fail (X4000 uninitialized); keep one exit.
- gpu_staterec.pad2 is zero from the frontend (memset) but the patched copy sets pad2[0] for every StateRec anyway.
- Scene load in chaos runs varies (prims 9.5k vs 11.5k): compare GPU kinds only within one run (PS2_GPU_AB).
