# WIP: gpu-M10 (widescreen renderer side)

**Goal**: milestone M10 of notes/d3d11-renderer.md section 11 on branch gpu/M10 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M10, from next/gpu e1fce93): native 16:9 on the GPU renderer -
anamorphic display targets (kx = round(N * W)), the HUD transform in the GPU backend from the StateRec hint
state (anchors L/C/R, full-width exemption), SIDE_FILL, pillarboxing of hinted 4:3 content, run-time F7/F8.

**Status**: in progress.

## Plan (design decisions)
- Display family kx = gpu_display_kx(N, aw, ah) = max(N, round(N * A * 3/4)) (cap 16), aw:ah = the game's
  aspect (gpu_hw_config.aspect(0, ...), per VBlank: F7), PS2_GS_ASPECT=W:H overrides; square family unchanged.
  The frontend gets it with GPU_DI_CONFIG (gpu_display_info.scale_x -> gf_configure -> own_rescale_all).
- Compose: picture W*sox x H*so (sox = kx of the display family, gpu_fop_present.scale_x); display.hlsl
  circuit_x / read_px in 1/sox units; present resamples to the letterboxed viewport (already per axis).
- gs_replay --aspect W:H (gs_replay.ps1 -Aspect): game pictures shown at it, display_kx for --mixed/--ref, box
  gate with sox columns per output pixel.

## Done
- 54fda15 anamorphic plumbing: gpu_front.h (display_kx, gpu_display_kx, di.scale_x, fop scale_x),
  gpu_own.c set_scale, gpu_front.c gf_configure / present fop, gpu_exec.c, gpu_present.c + display.hlsl (sox),
  gpu_hw.c (display_kx per VBlank, PS2_GS_ASPECT, dump buffers), gs_replay.c/.ps1 --aspect.
  HUD transform: gs_draw.c gs_hud_prim (the one rule; decoder gives the frontend the game's vertices, the sw
  rasterizer the transformed ones; side_fill's sprites with hud off), gpu_front.c fr_prim transforms from ev->hud.
- Replays: museum169 S=1 --game exact 60/60, --game-verify 1: verifier 60 comparisons 0 beyond, vs sw 0 beyond
  (min 55.56 dB); S=3 16:9 box gate min 41.02 dB; sw-hash unchanged (1b2a539b41d19923) with the HUD change.
  New recording build\gsrec\ws_ep3_hud.gsrec (ep3 hub 16:9, L/R/C anchors, L3 counters, R3 Binocucom SIDE_FILL,
  4839-5038): --game-verify 1: 200 comparisons 0 beyond, vs sw min 54.04 dB; S=3 box gate min 37.73 dB, max |mse|
  0.114; lockstep mixed S=3 16:9: museum169 481 / 0 differ, ws_ep3_hud 1421 / 0 differ.
- gs-hw -RefOnly -Adapter hw: all scenes identical incl. t_hud_hints (frontend transform vs sw).
- In game (release, headless, PS2_GS_BACKEND=hw, 3x, 16:9 default): screenshots build\screenshots\gpu_m10_*.png:
  museum (edges), ep3 hub L3 counters (R), ep1 boss (Dimitri meter R), Binocucom SIDE_FILL, movie (Prelude)
  pillarboxed + Watch-a-Movie menu, pause (centred, = sw hud_pause_169), F7 4:3 / F7 16:9 / F8 centered / F8
  edges (log: display targets 3x6 -> 4x6 samples per pixel at each F7). Contact sheets notes/m10/ingame.png,
  switch.png, scene.png + hud_counters.png (1x | 3x kx=3 (M7 16:9) | 3x kx=4 (M10), scripts/m10_sheet.py).
- corpus.txt: M10 section (museum169, ws_ep3_hud) with "aspect=16:9"; gpu_game_all / gpu_lockstep_all pass
  --aspect from it.

- Gates so far: run_tests 16/16; unit suites (disc dma gif gs-draw gs-front gs-mem iop_kernel iop_recomp ipu mc pad
  sd threads timer vif vu vu_recomp warp, window -UnitOnly 983 checks) pass; gs-hw full (hw + WARP) passed;
  VRAM hashes hwdry vs e1fce93 (git archive) identical on 26 recordings; gpu_fthread_check + -Mixed 26/26
  identical; gpu_diff_all 13/13; debug + release builds; package.py (dist\Sly2, version 0.1.629 6b8ff31).
- In game verifier at 3x 16:9 (ep3 hub, L3 / R3 Binocucom / walking, PS2_GS_VERIFY=60): 180 comparisons, 0 beyond.
- gpu_front_hud_stats + gs_replay "hud:" line; no widening at 1x (gpu_display_kx(1) = 1).
- Appendix K drafted (PERF placeholder).

## In progress
- Perf numbers: the GPU is saturated by other work right now (user's game + M8 runs: 98% util), measurements
  meaningless (ep3 3x 16:9 / 4:3 mixed bench 39-55 / 34 ms). Retry when quiet.

## Next
1. Gates: run_tests, unit suites, gpu_diff_all, VRAM hashes, fthread check, lockstep S=1, gpu_game_all S=1;
   debug + release builds; package.py.
2. Perf at 3x 16:9 vs 4:3 (replay mixed bench + in game) when the GPU is quiet.
3. Design notes 8.3 as built + appendix K; widescreen.md "Notes for the GPU renderer" update; settings.md
   (PS2_GS_ASPECT).

## Findings / traps
- Shader compile from scratch takes ~25 min on the loaded machine: seed build\hlslc_cache from
  gpu-M7\build\hlslc_cache (keyed by source content).
