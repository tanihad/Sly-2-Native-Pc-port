# WIP: Reimagined AI textures lighter and faster (tex-opt)

**Status: in progress** (code done; gates + timing runs running detached; then notes section + final commit)

Branch next/tex-opt from main 336e78e, worktree sly2-worktrees\opt. Do not push / merge.
Tools: the worktree has no tools\ or .venv: `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1` then cd here.
Build: `python scripts\build_game.py --release --out build\rel` (gates use build\rel: don't rebuild it while
build\texopt\gates.ps1 / perf.ps1 run). A/B exe: build\rel_ab. Baseline exe (main 336e78e): build\texopt\base\sly2.exe.
Model: build\models\x4plus.bin. Seeds: build\prebuild\texcache_warm, texcache_lists2, build\reimagine\texcache.
Owner's cache copy: build\texopt\owner_textures (887 files, 29 MB; levels w0, w8).
Tools: build\tools\bc7_tool2.exe (scripts/bc7_tool.c), build\tools\ai_up.exe (AI_UP_HALF=0x7802 = the game's net).
Scripts (local): build\texopt\{smoke,frames,abframes,gates,perf}.ps1.

## Goal
BC7 AI images in VRAM, fp16 net, dedupe, full-build feasibility; no visible quality loss, measured.

## Done (commits up to 45c5f2c)
- Full-build feasibility (subagent): scripts/ai_prepare_all.ps1; build\texopt\fullbuild\summary.md.
- BC7: runtime/gpu/shaders/bc7.hlsl (mode 6 + mode 3 on the 2 best partitions when mode 6's block error > 16;
  mode 1 off: 3x time for <= 0.4 dB), tables probed from HW. Game: BC7 classes ET_BC + c (gpu_tex.h ET_NSL, et_info
  bit 31), gpu_tex_ai.h EBC (staging build -> encode -> BC7 slot + map copy), opaque only (alpha images RGBA8),
  RGBA8 pre-size first_bc 205 MB, BC7 first {64,256,512,256,64}. PS2_TEX_BC7=0 off, =2 in-run A/B copies (bit 30,
  R8P_TEXBC 8192).
- Quality (bc7_tool encode, final encoder): owner 801 BC7 / 86 RGBA8: L0 min 42.36, p1 44.0, median 54.5 dB, 0 < 42;
  warm 1621 / 143: min 42.25, median 54.4, 0 < 42 (all levels min 41.3).
- fp16 net 0x7802 (D0, D1, BA, BB + weights half; F, R float): tool 400 images worst 58.1 dB, overall 62.5, max 3/255;
  159 vs 298 MB (game log 155 MB). All-half worst 50 dB (not used); BA/BB only 58.6 / 176 MB; weights only 63 / 266 MB.
  In-game cold (JPEG-amplified) cache files fp16 vs fp32: 45.8..59 dB, median ~51.
- Loader without integer divisions (outputs bit-identical); batch size 2x/4x only -6 %.
- Dedupe: 0.8-1.0 % of cache files are alpha-test-bit twins (identical images) -> ra_file_of alias (key ^ bit 60).
- Owner settings warm ep2 (contended): VRAM 1657 -> 1308 MB; images 791+198 -> 199+50 + BC7 219+171 MB;
  0 swaps, first-image delays unchanged; cached build 3.69 -> 6.50 ms per M texels; 645 BC7 / 77 RGBA8.
- Disk BC7 cache: rejected (owner cache 29 MB JPEG/PNG -> 207 MB BC7; decode 1.3 ms per file on loader threads).
- Gates so far: run_tests 16/16; units all pass except shot (failed under heavy load: rerun quiet);
  m9_exact S1+S3 -Game: the 2 available recordings identical (26 missing: corpus deleted -> coordinator);
  r2 Faithful 8/9 (ep1 tfb NO DATA: 0 pictures under load - rerun), r2 Reimagined 4/4.
- Frame PSNR: runs are not repeatable (same exe twice 31-45 dB: camera / Sly differ) -> in-run A/B (ab2, phase 2):
  adjacent same/cross pairs 38.4 vs 38.2 dB overall; still window 49.2 vs 50.9 -> BC7 at frame level ~54 dB.
  165 fps: dump:<n> writes no pictures (interpolated frames); 120 fps works.

## In progress
- build\texopt\gates.ps1 (detached): stability warm/cold, stress cold/warm, pop warm -> build\texopt\gates\summary.txt
- build\texopt\perf.ps1 (detached, after gates): net/BC7 timings, warm + cold owner-settings runs -> build\texopt\perf\

## Next
1. Rerun r2 ep1 tfb and the shot unit suite when the PC is quieter.
2. notes/reimagined.md "Lighter and faster" section with the tables; checkpoint complete; final commit; report.
3. Delete build\texopt\frames\* (disk hygiene) after the numbers are in the notes.

## Coordinator changes (2026-11-12)
- NO videos / comparison clips for the owner; keep PSNR measurements, gates, small stills only to check my own work.
- GPU corpus deleted (re-recorded elsewhere): gpu_diff_all = "pending" (coordinator runs it at merge); m9_exact with
  -CorpusRoot C:\Users\tanih\Projects\sly2-worktrees\shadow\build\shadows\corpus (read only, 2 recordings).

## Findings / traps
- Worktree files are CRLF; PowerShell `R` / `Rp` are aliases - don't name helpers so.
- fxc: loop var names must not shadow (X3078 = error in hlslc); single-iteration loops need [unroll] (X3557).
- BC7 encoder: indexable arrays (idx[16]) made it 10x slower: pack indices in a uint2; mode 1 was 3x the cost.
- bc7_tool encode's per-image timestamps are useless for speed (clocks drop): use `time`. Compiles take minutes.
- Other agents' sly2* games run nearly all the time (gpuC corpus re-recording): timings are contended.
- The cold first visit still grows the RGBA8 classes back to ~790 MB (Enhanced images first); VRAM saving is for
  warm caches (the normal case once a level was played).
