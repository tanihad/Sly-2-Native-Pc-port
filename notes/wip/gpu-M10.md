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
- (uncommitted) anamorphic plumbing: gpu_front.h (display_kx, gpu_display_kx, di.scale_x, fop scale_x),
  gpu_own.c set_scale, gpu_front.c gf_configure / present fop, gpu_exec.c, gpu_present.c + display.hlsl (sox),
  gpu_hw.c (display_kx per VBlank, PS2_GS_ASPECT, dump buffers), gs_replay.c/.ps1 --aspect.

## In progress
- First build + museum 16:9 replay at S=1 (must be unchanged: kx = 1) and S=3.

## Next
1. HUD transform out of the decoder for the GPU backend (frontend applies it from ev->hud; sw keeps the decoder's).
2. SIDE_FILL check, pillarbox at scale, F7/F8 in game.
3. Recordings: museum 16:9 (exists: widescreen\build\replay\ws_museum169.gsrec), HUD edge anchors + Binocucom.
4. Gates (4:3 unchanged), in-game screenshots, perf, appendix K.

## Findings / traps
- Shader compile from scratch takes ~25 min on the loaded machine: seed build\hlslc_cache from
  gpu-M7\build\hlslc_cache (keyed by source content).
