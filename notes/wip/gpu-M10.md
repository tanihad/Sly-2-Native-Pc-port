# WIP: gpu-M10 (widescreen renderer side)

**Goal**: milestone M10 of notes/d3d11-renderer.md section 11 on branch gpu/M10 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M10, from next/gpu e1fce93): native 16:9 on the GPU renderer -
anamorphic display targets (kx = round(N * W)), the HUD transform in the GPU backend from the StateRec hint
state (anchors L/C/R, full-width exemption), SIDE_FILL, pillarboxing of hinted 4:3 content, run-time F7/F8.

**Status: complete** (as built, results, deviations and notes for M11: design doc appendix K).

## Plan (design decisions)
- Display family kx = gpu_display_kx(N, aw, ah) = max(N, round(N * A * 3/4)) (cap 16; 1 at N = 1), aw:ah = the
  game's aspect (gpu_hw_config.aspect(0, ...), per VBlank: F7), PS2_GS_ASPECT=W:H overrides; square family
  unchanged. The frontend gets it with GPU_DI_CONFIG (gpu_display_info.scale_x -> gf_configure -> own_rescale_all).
- Compose: picture W*sox x H*so (sox = kx of the display family, gpu_fop_present.scale_x); display.hlsl
  circuit_x / read_px in 1/sox units; present resamples to the letterboxed viewport (already per axis).
- HUD transform: gs_hud_prim (gs_draw.c) shared; the frontend transforms the game's vertices from ev->hud before
  setup (not the VS: records need the transformed coordinates on the CPU anyway).
- gs_replay --aspect W:H (gs_replay.ps1 -Aspect); corpus "aspect=W:H" read by gpu_game_all / gpu_lockstep_all.

## Done
- 54fda15 anamorphic plumbing + frontend HUD transform; b8c811b ws_ep3_hud recording, corpus, in-game runs, sheets;
  6b8ff31 no widening at 1x, HUD statistics (gs_replay "hud:" line), notes 8.3 / settings / widescreen;
  (next) museum169 copied into build\gsrec; appendix K.
- Replays: S=1 16:9 --game exact + --game-verify 1 (0 beyond); S=3 16:9 box gate museum 41.02 / ep3 37.73 dB;
  lockstep mixed S=3 16:9: 481 + 1421 comparisons, 0 differ.
- In game 3x 16:9: screenshots build\screenshots\gpu_m10_*.png (museum, ep3 counters, ep1 boss, Binocucom, movie,
  menu, pause, F7/F8 sequence); verifier at 3x 16:9 180 comparisons 0 beyond. Sheets notes/m10/*.png.
- Gates: run_tests 16/16; unit suites pass (gs-hw hw + WARP, window -UnitOnly 983); VRAM hashes hwdry identical to
  e1fce93 on 26 recordings; fthread + -Mixed 26/26; gpu_diff_all 13/13; lockstep S=1 REF / mixed 25994 / 0 failed;
  gpu_game_all S=1 4076 pictures 0 differ, -Scale 3 on the 16:9 recordings 260 pictures 0 beyond; debug + release
  builds; package.py.
- Perf (contended GPU, ratios only): 16:9 vs 4:3 at 3x +22% (ep3) / +32% (museum) replay, +31% in game.

## Findings / traps
- Shader compile from scratch takes ~25 min on the loaded machine: seed build\hlslc_cache from
  gpu-M7\build\hlslc_cache (keyed by source content).
- Corpus paths must contain \build\ or \build\gsrec\ exactly once: the scripts derive output names from them and a
  remaining backslash becomes a sub-directory (gs_vram_hashes failed on widescreen\build\replay\...).
- .NET file APIs in PowerShell resolve relative paths against the process directory (the main checkout), not the
  PowerShell location: use absolute paths with [IO.File].
- The Edit tool's atomic rename can fail with EPERM while a compiler has the file open: retry later.
- In-game runs under GPU contention run at 35-50 fps: PS2_INPUT timings (pad frames) stretch; use generous
  -Seconds and PS2_DUMP_FROM. START came at VBlank ~2800-2900.
- PS2_GS_TRACE_FROM counts VBlanks of the replay from 0, not the recording's VBlank numbers.
- The 2D HUD is triangles (vector shapes, untextured / textured strips), so it is per-sample at scale; its blocky
  edges at 3x are the same at 4:3 (native-resolution masks/lines), not an M10 effect.
