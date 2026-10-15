# WIP: gpu-M5 (FF tier behind lockstep)

**Goal**: milestone M5 of notes/d3d11-renderer.md section 11 on branch gpu/M5 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M5): FF permutations, blend table (4.2), dual source, exact
SV_Depth, ATST As-range + split, tiled fill -> Clear, StateRec merging, ROV runs with zpatch + Z resolve in
a mixed frame, pass-A straddle escalation flag; classes moved ROV -> FF one at a time behind lockstep.

**Status**: in progress.

## Baseline (start, 06b9bc5)
- REF lockstep title: 184 comparisons, 0 differ (build\m5_base).

## Design decisions (M5)
- **Mixed frames** (`GPU_FD_MIXED`, gs_replay `--mixed`): FF draws = RTV (UNORM) + DSV (D32, SV_Depth =
  asfloat(z + 0x800000)); ROV and ALIAS16 draws = the REF pixel shader unchanged (colour UAV + zpatch UAV
  as depth). Same VS / viewport (REF 16384 centred) for both, so coverage is identical.
- **Depth in two places, synced lazily per 16x16-native-pixel tile** on the render thread: state per tile
  SYNC / DSV newer / ZP newer. FF draw with Z: zp->DSV for ZP-newer tiles of its bbox (quad, SV_Depth from
  zpatch = the design's ps_zresolve), then marks DSV-newer if it writes Z. REF-path draw with Z (ROV run,
  ALIAS16): DSV->zp for DSV-newer tiles of its bbox (cs), marks ZP-newer if it writes Z. Memory ops
  (load/download/xcopy/read-back) work on zpatch after a DSV->zp sync. Z24 keeps zpatch's top byte.
- **FF pixel shader = the REF pipeline's front half** (records: exact integer colour/fog/UV/Z, texture,
  TFX, fog, ATST in integers) + OM blend: BL_OFF exact in PS (incl. wrap), ADD / REVSUB (ONE,ONE) exact,
  DUAL lerp with SRC1_ALPHA = C/128 and Cs biased by -0.496/a (round -> floor; +-1 only when Cs*C < 64),
  NOCOLOR write mask. Alpha ONE/ZERO = As (| 0x80 FBA).
- FF triangle records: 256 B (exact STQ, `--ff-stq-exact`, for verification) or 128 B prefix with STQ
  interpolated by the hardware (default; the design's tolerance class "perspective-textured").
- ATST split: draw flags pass-draw / fail-draw (per-draw constant atst_mode), fail draw state from AFAIL.
- Classes moved one at a time with `gpu_front_config.ff_mask` (`--ff-mask`).

## Done (commits on gpu/M5)
- (none yet)

## In progress
- Step 1: executor mixed path + FF shaders + depth tile sync; frontend FF records/keys; gs_replay --mixed.

## Next
1. gs_replay --mixed (+ ps1 -Mixed), lockstep in mixed mode with tolerance classes; gpu_lockstep_all -Mixed.
2. ff_mask = 0 (everything ROV through the mixed path): lockstep 0 diff (proves the sync machinery).
3. Move classes: fills/BL_OFF opaque, NOCOLOR, ADD, REVSUB, DUAL, ATST split, views/feedback, lines.
4. Fast STQ (128 B records) default; tolerance stats.
5. Tiled fill -> Clear without the sprite draws; FF key without texture kind / STQ.
6. Pass-A split report (split vs --straddle-rov vs sw); escalation default decision.
7. Unit tests mixed mode (gs-hw run.ps1), perf (render ms, GPU ms vs REF), appendix G, full regression.

## Findings / traps
- (M4) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- gs_replay.ps1 build failures are silent when piped to Out-Null: delete the exe before building.
