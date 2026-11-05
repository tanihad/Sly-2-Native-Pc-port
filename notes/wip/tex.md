# WIP: Enhanced textures (branch next/tex, worktree C:\Users\tanih\Projects\sly2-worktrees\light)

Goal: textures clearly sharper / cleaner at 1440p (HD-remaster look), Original bit-identical and free. Design and
results: notes/enhanced-textures.md. Setting graphics.textures = enhanced | original (live), env PS2_TEX=0/1 + per feature.

STATUS: COMPLETE (2026-11-05)

## Status
- [x] 1 design note (notes/enhanced-textures.md) + texture statistics (PS2_TEX_STATS=1)
- [x] 2 ET cache (runtime/gpu/gpu_tex.h, shaders/tex.hlsl): GPU hash + staging read-back, class arrays, et_tab lookup,
      one-pass edge-directed 4x upscale + range-limited unsharp + deband, cubic mips with coverage, aniso (dece937)
- [x] 3 R2 permutations (R2_ETEX, r2_render.c tp[][][]) + MFT FF triangle permutations (phase A, depth) (dece937)
- [x] 4 setting / menu / presets / tests (f66235b; menu test 365 checks)
- [x] 5 HD-pack folder hook (WIC worker) + PS2_TEX_DUMP=1/2 (c2944c1; tested with a tinted pack: works)
- [x] 6 batched builds / hashes (e4408af), tuning + deband line exemption (ca997a0); pictures build\tex\shots\final
- [x] 7 gates on ca997a0: run_tests 16/16, units 0 failed, m9_exact S1+S3 -Game identical (base = main 7601e3f runtime,
      build\basert from build\basesrc), gpu_diff_all 28/28, r2_sweep PS2_TEX=0 ep1 ab,tfo,tfb,mf + m2 tfo,tfb,mf 7/7,
      PS2_TEX=1 ep1/ep3/m2 tfo,tfb,mf 9/9
- [x] perf (ep1 Balanced, in-run A/B, 6000 pictures each): GPU 6.15 -> 5.93 ms (-0.22), p99 8.52 -> 8.32, worst 14.05
      (a build frame) / 9.71; build frames +0.37 ms average

## How to look
- scripts\tex_shots.ps1 -Scene <s> -Frames 70 -Every 10 -Phase 30 (A/B pictures; run.log "ab: phase" lines say which
  VBlanks are A / B: phases switch every 30 VBlanks, A from multiples of 60)
- PS2_TEX_DBG=1 coverage (red), PS2_TEX_DUMP=2 originals + enhanced level 0 in <user dir>\textures\dump
- perf: tex_shots -Phase 24 -Fps unlimited -Seconds 330 -Extra "PS2_TEX_LOG=0;PS2_GPU_AB_LOG=0" -> ab-gpu / ab-worst /
  tex-build lines

## Findings / traps
- gpu_log writes take ~7 ms on the render thread: keep logs off for worst-case timing.
- Per-image dispatches (~25 each) with UAV barriers made level-start frames take up to 50 ms GPU: batch by class + level.
- LDS histogram atomics on one bin (alpha 0x80) serialize: 8 copies of the histogram.
- Class growth copies the whole array (GPU spike): pre-size to a hub's set (790 MB).
- `line` is an HLSL keyword. PowerShell `@('a', 'b' + 'c')` is an array of three: parenthesize multi-part strings.
- The press timing of -Play scenes varies run to run: ep6 once dumped the safehouse screen; -At 5400 is safer.
- build\light\shots holds the lighting agent's owner pictures (not ours; the shell refuses to delete them anyway).

## Log
- branch next/tex from main 7601e3f; commits dece937, f66235b, c2944c1, e4408af, ca997a0, 9d06366, final.
