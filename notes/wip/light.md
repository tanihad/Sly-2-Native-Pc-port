# WIP: Enhanced lighting (branch next/light, worktree C:\Users\tanih\Projects\sly2-worktrees\light)

Task: notes/enhanced-lighting.md design; SSAO + bloom; per-pixel lighting for lit meshes (+ rim); softer shadows;
anisotropic. Setting "Lighting: Original / Enhanced" (graphics.lighting), live; env PS2_LIGHT=0/1 + per-feature switches.

Owner decision (via main, 2026-11-05): Enhanced ON by default. Presets: Quality + Balanced = Enhanced, Performance +
Original = Original. Fresh installs (Balanced) get Enhanced. Explicit preset in ini -> follows preset; custom without the
key -> Enhanced. Original must cost nothing (skip all passes) and stay bit-identical.

## Status
- [x] 1 design note (notes/enhanced-lighting.md)
- [x] 2 SSAO + bloom (gpu_light.h, light.hlsl) - tuned on ep1 / ep6 / m2 boardroom; +0.42 ms GPU at 1440p Balanced (ep1)
- [x] 3 per-pixel lighting (r2.hlsl R2_ENH, r2_render.c epipe*) + rim 0.5
- [x] 4 soft shadow edges (mft.hlsl SOFT) - verified on ep1 (Sly's lamp shadow); edge-only for cost
- [x] 5 aniso: deferred (no mipmaps; design note)
- [x] cost: full Enhanced +0.63 ms (8703e18); owner shots build\light\shots\*_original/_enhanced/_compare_original_left.png
- [x] final gates on 8703e18: m9_exact S1+S3 -Game identical, gpu_diff_all 28/28, units 0 failed, run_tests 16/16, sweep Original ab+mf ep1/m2 4/4, Enhanced tfb+tfq+mf ep1/m2 6/6 + m2 tfo; (earlier, f2feb6a: Original ab,hc,tf,mf ep1/ep3/ep5/m2 16/16)
- STATUS: COMPLETE (2026-11-05)
- [ ] tfo follow-up (2026-11-05, after merge 941dd02): ep1 tfo 'blinking seams' = the job-start letterbox bars sliding
  inside the dump window (press timing varies run to run; 393ea64 fails the same with a wider window). Not a renderer
  change. Fix: scripts/flicker_check.c treats moved / fading edges as motion (notes/enhanced-lighting.md). Gates: pending
- [x] settings / menu / tests (graphics.lighting; 350 menu checks)
- gates on f2feb6a (AO/bloom/ppl): m9_exact S1+S3 -Game identical (on bfaa875 code), gpu_diff_all 28/28, run_tests 16/16,
  units all but gs-hw (my mid-edit; re-run), r2_sweep Original + Enhanced RUNNING (build\gates_sweep.txt)

## How to look
- scripts\light_shots.ps1 -Scene ep1_hub -At 5600 -Frames 70 -Every 10 -Phase 30 (PS2_GPU_AB phases: Original / Enhanced
  alternate every 30 frames; the frame pairs mid-phase compare); scripts\png_tool.py crop / pair / diff / stats.
- PS2_LIGHT_SPLIT=1 (Enhanced right half), PS2_LIGHT_DBG=1 AO / 2 bloom / 3 depth.
- Units: ~1 cm. ep1 near plane ~52, Sly ~190 tall.
- Pictures with graphics.motion_blur=off come out 2389x1344 (!): keep blur on for the owner's 2560x1440 shots.
- NEVER use relative paths with [IO.File] in PowerShell: they resolve against the main checkout (happened once: an empty
  scripts\light_shots.ps1 was created there and deleted again).

## Log
- worktree created from main 7018070.
- commits: 0162def (post pass), 92a1ed9 (setting), bfaa875 (design note), 061e40c (per-pixel), f2feb6a (bloom retune).
