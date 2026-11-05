# WIP: Enhanced textures (branch next/tex, worktree C:\Users\tanih\Projects\sly2-worktrees\light)

Goal: textures clearly sharper / cleaner at 1440p (HD-remaster look), Original bit-identical and free. Design:
notes/enhanced-textures.md. Setting graphics.textures = enhanced | original (live), env PS2_TEX=0/1 + per feature.

## Status
- [x] 1 design note (notes/enhanced-textures.md) + texture statistics (PS2_TEX_STATS=1)
- [x] 2 ET cache (runtime/gpu/gpu_tex.h, shaders/tex.hlsl): GPU hash + staging read-back, class arrays, et_tab lookup,
      one-pass edge-directed 4x upscale + range-limited unsharp + deband, cubic mips with coverage, aniso (dece937)
- [x] 3 R2 permutations (R2_ETEX, r2_render.c tp[][][]) + MFT FF triangle permutations (phase A, depth) (dece937)
- [x] 4 setting / menu / presets / tests (f66235b; menu test 360 checks)
- [x] 5 HD-pack folder hook (WIC worker) + PS2_TEX_DUMP=1/2 (c2944c1; tested with a tinted pack: works)
- [ ] 6 tuning with pictures (build\tex\shots), perf A/B (avg + worst)
- [ ] 7 gates

## Tuning state
- defaults: scale 4, sharp 1.6, sigma across 0.40 / along 1.2, edge 0.03, flat 6 / detail 18 (deband), bias -0.75, aniso 16
- PS2_TEX_DBG=1 shows enhanced texels red (coverage: ~all 3D in ep1, HUD untouched); PS2_TEX_DUMP=2 + scratch texcmp.py
  (original bilinear | enhanced) to judge single textures.
- ep1 hub: ~690 images built, 790 MB allocated (doubling growth), 0 re-decodes / rejects.

## How to run (worktree has no .venv)
- `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1; cd C:\Users\tanih\Projects\sly2-worktrees\light`
- python: C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe
- set PS2_USER_DIR to build\tex\user for every game run (HD pack folder lives there)

## Findings / traps
- (see notes/wip/light.md: never relative paths with [IO.File]; motion_blur=off shrinks pictures)

## Log
- branch next/tex from main 7601e3f
