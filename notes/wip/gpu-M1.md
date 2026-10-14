# WIP: gpu-M1 (frontend dry run)

**Goal**: milestone M1 of notes/d3d11-renderer.md section 11 on branch gpu/M1 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M1).

**Status: complete.**

## Done
- Step 1: gs_replay `--vram-hash` (-VramHash), scripts/gs_vram_hashes.ps1 (all 24 from corpus.txt,
  -Compare), before hashes with a snapshot of the unmodified runtime (build\m1\runtime_before,
  build\m1\vram_before); combined hashes committed in tests/gpu/vram_hashes.txt.
- Step 2: runtime/gs_setup.c/.h (gs_vtx, planes, gs_tri_setup/planes, rowstep + gs_rowstep_attr,
  edges, sprite rule, line DDA step, point pixel, gs_llround, frontend hook table gs_front +
  gs_sw_draw); gs_draw.c uses them and calls the hooks (prim, xfer_start, xfer_data, tex0, reset).
  **VRAM hashes identical on all 24 recordings, every VBlank**; gs-draw 4 variants pass.
- Step 3: runtime/gpu/gpu_front.h (public: config, per-frame counters, command stream ops),
  gpu_front_int.h, gpu_front.c (classification, StateRec dedup, vertices/indices/records, draw
  merging, ROV runs, ALIAS16 batches, clears, commutation estimate, trace), gpu_own.c (targets,
  per-block ownership/serials/CPU word masks, loads, downloads, patches, shadow copies, eviction),
  gpu_clut.c (CLUT ring, content change detection, min/max alpha cache).
- Step 4: gs_replay `--backend hwdry` (-Backend hwdry, -FrontOnly, -Ref, -StraddleRov, -SelfCheck,
  -DecodeOnly, -Define), metrics JSON (per VBlank + sum + max), summary on stderr, exit 5 on PUNT,
  "replay thread CPU" (QueryThreadCycleTime) line; scripts/gpu_hwdry_all.ps1 (all 24: hwdry with sw,
  VRAM hash vs tests/gpu/vram_hashes.txt, 0 PUNT, build\m1\hwdry\table.md + summary.json).
- tests/unit/gs-front (24 checks: gs_llround, Z clear, shadow volume ROV run, ALIAS16 hazard split,
  pass-A all-pass/straddle/As>128, Z32 high, FMV uploads -> 1 patch + TBW 12, photo REGION_CLAMP
  view kept 200 frames, T8H darken feedback copy vs RGB-only no copy, AA1 outline pair pixels).
- Corpus run 2 (before the CLUT change): 24/24 ok, 0 PUNT, VRAM unchanged (table below).

## Verification (final)
- VRAM hashes (all 4 MB after every VBlank, synchronous sw replay) identical to the pre-refactor
  runtime on all 24 recordings (build\m1\vram_refactor_run.txt), and again with the frontend running
  alongside after all frontend changes incl. gs_llround (gpu_hwdry_all: "VRAM unchanged").
- scripts\gpu_diff_all.ps1: 13/13 passed, 0 differing pixels, sw hashes = tests/gpu/baseline.json
  (baseline not updated).
- tests\run_tests.py 16/16; every tests\unit\*\run.ps1 exit 0 (dma, gif, gs-draw 170/171/170/170,
  gs-front 24, gs-hw 109, gs-mem 35476, iop_kernel, iop_recomp, ipu, mc, pad, sd, threads, timer,
  vif, vu, vu_recomp, warp 113, window -UnitOnly 730).
- build_game.py debug and --release build; PS2_GS_BACKEND still defaults to sw (the frontend is only
  installed by gpu_front_start, which only gs_replay/unit tests call).
- hwdry on all 24 with -SelfCheck: 0 PUNT, 0 selfcheck mismatches, Z32 max 0x7F5C3.

## Per-recording hwdry tables (input for M2)
| recording | VBlanks | PUNT | D3D draws max / mean | commute max | ROV draws max | ROV runs max | zresolves max | states max | state changes max | records max | ALIAS16 sprites / dispatches max | pass-A straddle % | As>128 % | Z range | front ms mean / max |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| fix-perf_rv_prompt | 20 | 0 | 606 / 470.6 | 453 | 6 | 4 | 0 | 109 | 486 | 8717 | 96 / 3 | 52.54 | 0 | Z32 0..7F5C2, Z16S 0..1C10 | 3.09 / 4.78 |
| fix-perf_rv_intro | 16 | 0 | 662 / 408.6 | 531 | 12 | 9 | 1 | 132 | 561 | 5804 | 96 / 3 | 36 | 1.63 | Z32 0..164FB, Z16S 0..410 | 1.45 / 2.76 |
| fix-perf_title | 30 | 0 | 1475 / 688.8 | 1087 | 10 | 7 | 0 | 152 | 1468 | 25100 | 96 / 3 | 39.19 | 0 | Z32 0..32DB3, Z16S 0..19A0 | 1.34 / 3.22 |
| fix-perf_brief | 30 | 0 | 2226 / 1028.1 | 1642 | 20 | 17 | 2 | 253 | 2010 | 29694 | 96 / 3 | 39.99 | 0 | Z32 0..376C7, Z16S 0..C474 | 2.62 / 6.17 |
| ep3_hub | 240 | 0 | 4633 / 1541.9 | 3373 | 61 | 61 | 52 | 536 | 4044 | 37904 | 96 / 3 | 38.2 | 3.93 | Z32 0..7F5C2, Z16S 0..193C | 3.35 / 35.11 |
| ep3_recon_int | 240 | 0 | 2742 / 937.8 | 2101 | 71 | 68 | 67 | 242 | 2548 | 24084 | 96 / 3 | 27.76 | 8.26 | Z32 0..7F52A, Z16S 0..193C | 1.99 / 7.33 |
| ep3_rope | 20 | 0 | 4049 / 1814.1 | 3235 | 39 | 39 | 36 | 481 | 3511 | 35854 | 96 / 3 | 47.56 | 2.47 | Z32 0..7F5C2, Z16S 0..193C | 3.67 / 11.06 |
| ep3_brief | 240 | 0 | 2059 / 584.4 | 1524 | 13 | 10 | 0 | 166 | 1947 | 24698 | 96 / 3 | 40.49 | 0 | Z32 0..4FC7E, Z16S 0..1DA0 | 1.53 / 5.84 |
| ep5_hub_street | 240 | 0 | 4745 / 1773 | 3487 | 80 | 80 | 68 | 662 | 4365 | 39842 | 96 / 3 | 22.48 | 2.85 | Z32 0..7F5C2, Z16S 0..193C | 5.07 / 19.86 |
| ep6_rc_battle | 120 | 0 | 1654 / 532.2 | 1223 | 51 | 48 | 47 | 220 | 1766 | 15334 | 96 / 3 | 31.35 | 2.99 | Z32 0..E3E5, Z16S 0..7074 | 1.15 / 3.93 |
| ep7_boss | 120 | 0 | 3744 / 1627.3 | 2918 | 215 | 215 | 214 | 193 | 3361 | 34150 | 96 / 3 | 63.83 | 22.45 | Z32 0..7F5C3, Z16S 0..19A0 | 2.46 / 27.81 |
| ep8_hub | 240 | 0 | 1593 / 677 | 1229 | 40 | 40 | 39 | 96 | 1541 | 10939 | 96 / 3 | 28.52 | 14.56 | Z32 0..7F5C3, Z16S 0..2D3C | 0.77 / 3.27 |
| ep8_boss | 240 | 0 | 5909 / 2390.3 | 5198 | 97 | 97 | 96 | 553 | 5810 | 30981 | 96 / 3 | 11.08 | 6.54 | Z32 0..3E1CD, Z16S 0..2D3C | 5.15 / 45.50 |
| gpu-C_ep0_museum | 150 | 0 | 3046 / 1329.7 | 2240 | 40 | 37 | 1 | 362 | 2676 | 23567 | 96 / 3 | 46.14 | 0 | Z32 0..7F5C2, Z16S 0..193C | 3.23 / 8.78 |
| gpu-C_ep1_hub | 180 | 0 | 4315 / 1791.1 | 3281 | 88 | 88 | 75 | 592 | 4023 | 32182 | 96 / 3 | 28.16 | 4.15 | Z32 0..7F5C2, Z16S 0..193C | 4.81 / 13.48 |
| gpu-C_m1_brawl | 180 | 0 | 3587 / 1258.5 | 2483 | 83 | 83 | 77 | 497 | 3319 | 30447 | 96 / 3 | 28.18 | 8.97 | Z32 0..7F5C3, Z16S 0..18D8 | 3.29 / 31.51 |
| gpu-C_ep2_hub | 180 | 0 | 5958 / 2290.6 | 4079 | 25 | 25 | 18 | 574 | 5498 | 54985 | 96 / 3 | 29.02 | 0.62 | Z32 0..7F5C2, Z16S 0..193C | 5.93 / 16.45 |
| gpu-C_ep2_photo | 240 | 0 | 4390 / 101.5 | 3384 | 86 | 83 | 76 | 196 | 3913 | 38308 | 96 / 3 | 51.94 | 2.89 | Z32 0..7F5C2, Z16S 0..C474 | 0.90 / 11.84 |
| gpu-C_ep4_hub | 180 | 0 | 4185 / 1922.9 | 3111 | 77 | 74 | 65 | 609 | 3750 | 34042 | 96 / 3 | 18.95 | 0.85 | Z32 0..7F5C3, Z16S 0..193C | 5.09 / 39.04 |
| gpu-C_hack_minigame | 180 | 0 | 372 / 300.6 | 222 | 1 | 1 | 1 | 21 | 370 | 15584 | 0 / 0 | 55.8 | 0 | Z32 0..D631, Z16S 0..1474 | 0.69 / 1.25 |
| gpu-C_fmv | 240 | 0 | 28 / 5 | 22 | 0 | 0 | 0 | 7 | 22 | 6524 | 0 / 0 | 0 | 0 | Z32 0..0, Z16S 0..9410 | 1.06 / 5.42 |
| gpu-C_load_m3 | 240 | 0 | 2450 / 1011.6 | 1798 | 18 | 18 | 15 | 175 | 2369 | 21041 | 96 / 3 | 24.33 | 2.75 | Z32 0..7F5C2, Z16S 0..1410 | 1.18 / 3.67 |
| gpu-C_pause_save | 150 | 0 | 4388 / 1696.5 | 3232 | 18 | 18 | 12 | 498 | 4021 | 45460 | 96 / 3 | 27.42 | 0.46 | Z32 0..7F5C2, Z16S 0..5074 | 3.57 / 29.04 |
| gpu-C_thiefnet | 100 | 0 | 6512 / 3126.2 | 4862 | 146 | 146 | 122 | 772 | 6207 | 71138 | 96 / 3 | 25.58 | 4.97 | Z32 0..7F5C2, Z16S 0..9074 | 4.24 / 9.68 |

| recording | clears Z / colour | loads (blocks) | downloads | shadow copies | ALIAS16 self-copies | uploads -> max patches/VBlank | new CLUT slots | CLUT loads over GPU words | REGION_CLAMP prims | TBW not 2^n prims | CT32 / alpha views | max targets | ROV why (ref/colclamp/fbmsk/date/blend/C>128/pabe-dthe/z32/straddle) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| fix-perf_rv_prompt | 64 / 64 | 18 (3648) | 16 | 16 | 0 | 96 -> 1 | 128 | 0 | 0 | 0 | 256 / 256 | 4 | 0/1231/512/112/0/0/0/0/0 |
| fix-perf_rv_intro | 20 / 30 | 18 (3648) | 16 | 10 | 0 | 60 -> 1 | 165 | 0 | 0 | 0 | 160 / 160 | 4 | 0/2191/320/1408/0/410/0/0/0 |
| fix-perf_title | 42 / 42 | 2 (3584) | 0 | 14 | 0 | 84 -> 1 | 157 | 0 | 0 | 0 | 224 / 224 | 4 | 0/4620/448/2072/0/0/0/0/0 |
| fix-perf_brief | 98 / 70 | 3 (3584) | 0 | 14 | 0 | 84 -> 1 | 244 | 0 | 0 | 0 | 264 / 224 | 7 | 0/3927/448/5950/0/0/0/0/0 |
| ep3_hub | 198 / 99 | 4 (3584) | 0 | 99 | 0 | 594 -> 1 | 857 | 0 | 0 | 0 | 1568 / 1584 | 4 | 0/73481/0/11768/0/45986/0/0/0 |
| ep3_recon_int | 166 / 249 | 2 (3584) | 0 | 83 | 0 | 498 -> 1 | 256 | 0 | 0 | 0 | 1312 / 1328 | 4 | 0/67335/2656/166/0/55016/0/0/0 |
| ep3_rope | 18 / 9 | 3 (3584) | 0 | 9 | 0 | 54 -> 1 | 531 | 0 | 0 | 0 | 128 / 144 | 4 | 0/10968/0/522/0/3164/0/0/0 |
| ep3_brief | 264 / 264 | 2 (3584) | 0 | 88 | 0 | 528 -> 1 | 189 | 0 | 0 | 0 | 1408 / 1408 | 4 | 0/33464/2816/13187/0/0/0/0/0 |
| ep5_hub_street | 194 / 97 | 4 (3584) | 0 | 97 | 0 | 388 -> 1 | 930 | 192 | 0 | 0 | 1536 / 1552 | 4 | 0/65106/0/15714/0/40460/0/0/0 |
| ep6_rc_battle | 82 / 123 | 2 (3584) | 0 | 41 | 0 | 246 -> 1 | 230 | 0 | 0 | 0 | 640 / 656 | 4 | 0/4299/1312/410/0/7467/0/0/0 |
| ep7_boss | 220 / 110 | 1 (1792) | 0 | 55 | 0 | 0 -> 0 | 240 | 0 | 0 | 0 | 880 / 880 | 4 | 0/56570/0/48980/0/104728/0/0/0 |
| ep8_hub | 210 / 105 | 3 (3584) | 0 | 105 | 0 | 840 -> 1 | 108 | 0 | 0 | 0 | 1680 / 1680 | 4 | 0/47061/0/420/0/35331/0/0/0 |
| ep8_boss | 137 / 100 | 3 (3584) | 0 | 100 | 0 | 1600 -> 1 | 652 | 0 | 0 | 0 | 1600 / 1600 | 4 | 0/41157/0/144/0/79629/0/0/0 |
| gpu-C_ep0_museum | 134 / 201 | 4 (3584) | 0 | 67 | 0 | 402 -> 1 | 456 | 0 | 0 | 0 | 1072 / 1072 | 4 | 0/81671/2144/9465/0/0/0/0/0 |
| gpu-C_ep1_hub | 192 / 87 | 5 (3584) | 0 | 87 | 0 | 696 -> 1 | 1260 | 0 | 0 | 0 | 1376 / 1392 | 4 | 0/80564/0/174/0/40168/0/0/0 |
| gpu-C_m1_brawl | 258 / 86 | 3 (3584) | 0 | 86 | 0 | 688 -> 1 | 781 | 0 | 0 | 0 | 1376 / 1376 | 4 | 0/37291/0/1229/0/54221/0/0/0 |
| gpu-C_ep2_hub | 264 / 88 | 2 (3584) | 0 | 88 | 0 | 528 -> 1 | 943 | 0 | 0 | 0 | 1408 / 1408 | 4 | 0/82480/0/89863/0/9008/0/0/0 |
| gpu-C_ep2_photo | 196 / 387 | 18 (2112) | 16 | 51 | 0 | 0 -> 0 | 186 | 0 | 732 | 0 | 1534 / 816 | 5 | 0/638/1632/14783/0/10028/0/0/0 |
| gpu-C_ep4_hub | 228 / 264 | 2 (3584) | 0 | 88 | 0 | 352 -> 1 | 1989 | 174 | 0 | 0 | 1392 / 1408 | 4 | 0/47197/2816/10869/0/9830/0/0/0 |
| gpu-C_hack_minigame | 352 / 176 | 0 (0) | 0 | 0 | 0 | 0 -> 0 | 8 | 0 | 0 | 0 | 2800 / 0 | 4 | 0/0/0/352/0/0/0/0/0 |
| gpu-C_fmv | 72 / 36 | 0 (0) | 2 | 0 | 0 | 121200 -> 1 | 1 | 0 | 0 | 201 | 560 / 0 | 4 | 0/0/0/0/0/0/0/0/0 |
| gpu-C_load_m3 | 245 / 144 | 2 (3584) | 0 | 107 | 0 | 2200 -> 1 | 412 | 0 | 0 | 0 | 2096 / 1712 | 4 | 0/77259/192/2490/0/11384/0/0/0 |
| gpu-C_pause_save | 137 / 58 | 2 (3584) | 0 | 58 | 0 | 17148 -> 1 | 508 | 0 | 0 | 28 | 928 / 928 | 4 | 0/22968/0/5916/0/5858/0/0/0 |
| gpu-C_thiefnet | 96 / 48 | 2 (3584) | 0 | 48 | 0 | 384 -> 1 | 805 | 0 | 0 | 0 | 752 / 768 | 4 | 0/11088/0/0/0/52176/0/0/0 |

Frontend CPU bench (scripts\gpu_front_bench.ps1, replay thread CPU, median of 3, per game frame =
VBlank with >= 1000 primitives; machine under heavy load from other agents):
```
name                      : ep3_hub
vblanks                   : 240
game_frames               : 99
decode_ms_per_game_frame  : 4.74
front_ms_per_game_frame   : 4.43
decode_plus_front         : 9.17
sw_sync_ms_per_game_frame : 79.06
name                      : ep5_hub_street
vblanks                   : 240
game_frames               : 97
decode_ms_per_game_frame  : 6.7
front_ms_per_game_frame   : 6.68
decode_plus_front         : 13.38
sw_sync_ms_per_game_frame : 51.05
name                      : ep8_boss
vblanks                   : 240
game_frames               : 100
decode_ms_per_game_frame  : 4.34
front_ms_per_game_frame   : 5.83
decode_plus_front         : 10.17
sw_sync_ms_per_game_frame : 38.69
name                      : gpu-C_ep2_hub
vblanks                   : 180
game_frames               : 88
decode_ms_per_game_frame  : 4.56
front_ms_per_game_frame   : 8.18
decode_plus_front         : 12.74
sw_sync_ms_per_game_frame : 56.01
name                      : gpu-C_thiefnet
vblanks                   : 100
game_frames               : 48
decode_ms_per_game_frame  : 6.63
front_ms_per_game_frame   : 9.04
decode_plus_front         : 15.68
sw_sync_ms_per_game_frame : 65.71
```

## Next (later milestones)
- M2: execute the GPU_FOP stream (gpu_front_set_sink) on the render thread; measure on a quiet
  machine; draws per frame max 6.5k (ThiefNet), 5.9k (ep8 boss, ep2 hub), commutation estimate saves
  ~25%; ROV runs up to 215/frame with a Z resolve each (ep7 boss, As>128 pass-A draws).
- GS-thread budget: decode + frontend 9-16 ms/game frame here vs 6 ms target.
## Findings / traps
- Recordings start mid-frame: the CT16 shuffle can come before its C32 target was drawn in the
  recording -> a CT16 FRAME creates its C32 target (generic, exact via loads).
- Target height from SCISSOR is wrong (a field buffer drawn with scissor up to 255 overlapped the photo
  copy at 070): rows now grow with the drawn/read extent (own_grow). Same-kind targets overlapping at
  another base are separate targets (block ownership makes the aliasing exact, counted
  `target_overlaps`); the design's "starting inside -> PUNT" rule is not needed.
- Per-block ownership (256-byte GS blocks: blk_writer, blk_serial, 64-bit CPU word mask) instead of
  per-page: Sly uploads CLUTs/textures into Z pages every frame, partial blocks included.
- **CLUT loads read Z-buffer words**: ep4/ep5 load a 256-entry T8 CLUT at CBP 1000 every frame of
  which only ~112 entries were uploaded; the rest of the 16x16 block was last written by the Z buffer.
  Downloading would stall every frame; the unused entries are never indexed (else the PS2 shows Z
  garbage too), so the frontend counts it (`clut_gpu_reads`) and does not download. M3 lockstep checks.
- The T8H darken writes alpha too (FBMSK 0), so it IS a feedback hazard (design 5.1 assumed RGB only):
  one shadow copy of the dirty bbox per darken pass (block-level dirty tracking), ~1 per game frame.
- Outline pairs (ZB_ONLY line, then AA1 RGB_ONLY line) alternate pipeline keys: 2 D3D draws per
  outline; commutation by bbox barely helps (segments touch). Biggest draw-count contributor with
  pass A/B alternation. See commute column.
- ROV runs: shadow volumes alternate Z-tested / not: runs merge across zt (a run binds the colour
  target, depth when any draw needs it); ROV draws merge across blend/test/Z mode (data).
  Remaining runs come from pass-A draws with As > 128 (lerp blend): each is a ROV run + Z resolve
  (ep7 boss: 215/frame, 22% of lerp kicks). M2/M5 must weigh this.
- Frontend cost (front-only minus decode-only, replay thread CPU): ep8 boss ~1.9 ms per VBlank
  (~4.5 ms per game frame), decode alone ~1.9 ms/VBlank, on a machine at ~90% load. Hot spots fixed:
  per-pixel ALIAS16 hazard bitmaps -> rectangles; state analysis cache (32 entries, signature incl.
  CLUT slot); clean-target fast path; texture source cache; gs_llround (llround libcall).
- gs_draw.c: in frontend-only mode bind_textures skips gs_tex_bind (sw texture cache); a live
  backend switch (M6) must set env_dirty.
- PowerShell: here-strings without a trailing newline glue lines; `R` is an alias (Invoke-History).
  The agent sandbox blocks commands whose text contains `Remove-Item $var` in some forms.
