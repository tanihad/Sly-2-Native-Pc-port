# WIP: Reimagined AI textures lighter and faster (tex-opt)

**Status: complete** (results: notes/reimagined.md "Lighter and faster")

Branch next/tex-opt from main 336e78e, worktree sly2-worktrees\opt. Do not push / merge.
Tools: the worktree has no tools\ or .venv: `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1` then cd here.
Build: `python scripts\build_game.py --release --out build\rel`. A/B exe: build\rel_ab (PS2_TEX_BC7=2).
Baseline exe (main 336e78e): build\texopt\base\sly2.exe. Model: build\models\x4plus.bin (never commit).
Owner's cache copy: build\texopt\owner_textures. Tools: build\tools\bc7_tool2.exe (scripts/bc7_tool.c),
build\tools\ai_up.exe (AI_UP_HALF=0x7802 = the game's net). Local scripts: build\texopt\{smoke,frames,abframes,gates,
final_gates,perf,parse}.ps1; perf logs build\texopt\perf\, gate outputs build\texopt\gates\.

## Goal
BC7 AI images in VRAM, fp16 net, dedupe, full-build feasibility; no visible quality loss, measured.

## Done
- BC7 classes for opaque AI images (shaders/bc7.hlsl, gpu_tex_ai.h EBC, gpu_tex.h ET_NSL / et_info bits 31 / 30,
  etex.hlsli / etn.hlsli t50..t61, perms cs_bc7); PS2_TEX_BC7=0 off, =2 in-run A/B copies (R8P_TEXBC 8192).
- Half-float net (ai_up.hlsl U<b>_H / AI_HALF_W, ai_up.c half flags, perms cs_ai_*h; PS2_AI_HALF=0 float); tile
  loader without divisions. Cache key unchanged (x4plus-v3).
- Dedupe: alpha-test twins share a cache file (ra_file_of alias).
- Feasibility of a prepare-all pass: scripts/ai_prepare_all.ps1 (subagent), 63 min measured for 41 worlds.
- Measurements + gates: notes/reimagined.md "Lighter and faster" (tables, gates).

## In progress
- nothing

## Next
- (owner / coordinator) merge decision; gpu_diff_all 28/28 and the full m9_exact once the corpus is re-recorded.
- Possible later: BC7 for the surface maps too (another ~165 MB; the encoder is opaque-only now), shrink the RGBA8
  classes after a cold first visit, a "Prepare all levels" menu entry around ai_prepare_all's sweep.

## Coordinator changes (2026-11-12)
- NO videos / comparison clips for the owner; PSNR measurements, gates, small stills only to check my own work.
- GPU corpus deleted: gpu_diff_all pending; m9_exact with the shadow worktree's 2 recordings.

## Findings / traps
- Worktree files are CRLF; PowerShell `R` / `Rp` are aliases; `$l` and `$L` are the same variable.
- fxc: loop vars must not shadow (X3078 = error in hlslc); single-iteration loops need [unroll] (X3557).
- BC7 encoder: indexable arrays (idx[16]) made it 10x slower - pack 4-bit indices in a uint2; mode 1 3x the cost.
- BC7 staging build: etn_run checks ETN.arr[c] / ETN.on / caps - before the fix, BC7 images in classes whose RGBA8 maps
  did not exist yet got no (or stale) surface maps (e1161ab).
- bc7_tool encode's per-image timestamps are useless for speed (clocks drop): use `time`. Compiles take minutes.
- Frame dumps (dump:<n>) write nothing at 165 fps; 120 works. Repeat runs of the same exe differ (31-45 dB): use the
  in-run A/B for picture comparisons. At 165 fps the VBlank counter runs at the picture rate (~161 per s).
- Other agents' sly2* games run nearly all the time: perf.ps1 waits for 45 quiet s before each run.
- The cold first visit regrows the RGBA8 classes to ~790 MB (Enhanced first): +109 MB that session; warm -299 MB.
