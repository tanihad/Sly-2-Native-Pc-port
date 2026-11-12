# WIP: Reimagined AI textures lighter and faster (tex-opt)

**Status: in progress**

Branch next/tex-opt from main 336e78e, worktree sly2-worktrees\opt. Do not push / merge.
Tools: the worktree has no tools\ or .venv: `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1` then cd here.
Build: `python scripts\build_game.py --release --out build\rel`. Model: build\models\x4plus.bin (copy of ai's; never commit).
Baseline exe (main 336e78e): build\texopt\base\sly2.exe.
Seeds (copied from worktree ai): build\prebuild\texcache_warm, texcache_lists2, build\reimagine\texcache.
Owner's cache copy (read only source, 887 files 29 MB): build\texopt\owner_textures.
Tools: build\tools\bc7_tool.exe (scripts/bc7_tool.c: probe / encode / time / dedupe / diff / mkbatch / cmpout),
build\tools\ai_up.exe (scripts/ai_up_tool.c; AI_UP_HALF=0x7802 env = the game's half net; AI_UP_CACHE=build\texopt\shcache).
Net test input: build\texopt\net_in.bin (400 owner images box-downscaled 4x + 12 edge padding).

## Goal
BC7 AI images in VRAM, fp16 net, dedupe, full-build feasibility; no visible quality loss, measured.

## Done
- Full-build feasibility (subagent): scripts/ai_prepare_all.ps1; summary build\texopt\fullbuild\summary.md
  (41 worlds, 60 s each, 63 min wall, 4517 images, 2346 TFLOP; PS2_REIM_FIX=0 + 30 fps + PS2_AI_MS=24 fastest).
- BC7 encoder runtime/gpu/shaders/bc7.hlsl (modes 6 + 1/3 on 2 best partitions, opaque only); tables probed from HW.
  Quality owner cache (opaque, 544): min 43.7 dB, median 54.8 (mode 6 only: min 40.6). Alpha images stay RGBA8.
- fp16 net: ai_up.hlsl U<b>_H per-buffer half storage, AI_HALF_W half weights; ai_up.c half flags (ai_up.h).
  Half D0/D1/BA/BB + weights (F, R float): vs fp32 worst 58.1 dB, overall 62.5, max 3/255; 159 vs 298 MB; same speed.
  All-half: worst 50 dB (F/R accumulate) -> not used. Larger batches: only -6 %. Loader without divisions: in.
- Game code (not yet built/tested): gpu_tex.h ET_NSL slot tables (BC7 classes = ET_BC + c), et_info bit 31,
  first_bc pre-size; gpu_tex_ai.h EBC (staging build -> cs_bc7 -> BC7 slot; ra_build_new / ra_apply), loader opacity,
  RA.half (PS2_AI_HALF); etex.hlsli / etn.hlsli BC7 classes t50..55, maps t56..61; perms.txt cs_ai_*h + cs_bc7.

## In progress
- first build with all of it; then a Reimagined run (warm seed) to check the BC7 path, PS2_TEX_DUMP=3 diffs.

## Next
1. Build, smoke-run warm + cold (PS2_AI_LOG / POPLOG lines: BC7 MB), fix.
2. BC7 speed: bc7_tool time (quiet GPU) for THRESH variants; pick default.
3. Dedupe census (bc7_tool dedupe) -> decide.
4. Measurements: VRAM, load, ms per M texels, ep2 hub cold build time, frame times at owner's settings; stills.
5. Gates (see task list); notes/reimagined.md section; checkpoint complete.

## Coordinator changes (2026-11-12)
- NO videos / comparison clips for the owner; keep PSNR measurements, gates, small stills only to check my own work.
- GPU corpus deleted (re-recorded elsewhere): gpu_diff_all = "pending" (coordinator runs it at merge); m9_exact with
  -CorpusRoot C:\Users\tanih\Projects\sly2-worktrees\shadow\build\shadows\corpus (read only, 2 recordings).

## Findings / traps
- Worktree files are CRLF; PowerShell `R` is an alias (Invoke-History) - don't name helpers R.
- fxc: loop var names must not shadow (X3078 warning = error in hlslc).
- bc7_tool encode's per-image timestamps are useless for speed (GPU clocks drop between images): use `time`.
- Other agents' sly2* games often run: check before timing.
