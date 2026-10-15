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
- dd341c7 mixed frames: gs.hlsl split into gs_frag (shared) + ps_gs (REF/ROV) + ps_ff (FF, `FF=1` perms without
  UAVs); zsync.hlsl (ps_zresolve, cs_zsync); gpu_exec.c exec_draw_ff, per-tile depth sync, mixed clears, draw
  slots for split halves; gpu_front.c FF records (128/256 B), FF key, split halves, ff_rov_mask classes;
  gs_replay --mixed / --ff-stq-exact / --ff-rov-mask / --lockstep-exact.
- 60dcf4a fill -> clear drops the fill's FF draws; statistical gate for the default mode; render-bench line;
  gpu_lockstep_all -Mixed -Exact -StqExact -FfRovMask -Exe.
- Class progression (exact lockstep, S/T/Q exact, 8 recordings: title, brief, rope, ep3 brief, ep6, ep8 hub,
  ep2 photo, hack minigame; 7683 comparisons each step, 0 differ): A OFF sprites, B +depth, C +triangles,
  D +lines/points, E +NOCOLOR, F +ADD, G +REVSUB, H +target views/feedback, I +ATST split (build\m5_prog.txt).

## Dual-source blend precision (scratch test blendtest.c: every Cs, Cd, C 0..128 on a UNORM8 target)
- WARP (fp32 blender): unbiased round-to-nearest gives +1 in 48.5%; source lowered by 0.496/a LSB: exact
  except Cs*C < 64 (0.37%).
- RTX 4060 Ti: the blender works at about fp16 precision (+-0.125 LSB): even a = 1 (C = 128) with a bias
  fails; no bias can be exact. Best bias ~0.45: 80% exact, rest +-1 (never 2). Chosen: bias 0.45/a for
  0 < C < 128, none at C = 0 and 128 (exact there). => DUAL stays the design's +-1 class; exact verification
  runs keep DUAL on the ROV tier (ff_rov_mask 0x10).

## In progress
- Full corpus (build\m5_full): exact mode (-Exact -StqExact -FfRovMask 0x10) then default mode.

## Next
1. Split report: mask 0 + stq exact + tolerance (Z diffs and >2 diffs come only from the split) vs the same
   with --straddle-rov; decide the escalation default.
2. Unit tests mixed mode (gs-hw run.ps1: exact variant mask 0x10 + stq exact; default variant reported).
3. Perf: render-thread draws/ms, GPU ms per game frame vs REF (ep8 boss, ep3 hub, ep5 street, ep2 hub,
   ThiefNet), Z resolve/sync counts; reduce if needed.
4. WARP subset; appendix G; regression (run_tests, unit suites, gpu_diff_all, VRAM hashes, fthread check,
   REF lockstep 24/24, builds).

## Findings / traps
- (M4) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- gs_replay.ps1 build failures are silent when piped to Out-Null: delete the exe before building.
