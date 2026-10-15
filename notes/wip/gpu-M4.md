# WIP: gpu-M4 (Exact effects)

**Goal**: milestone M4 of notes/d3d11-renderer.md section 11 on branch gpu/M4 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M4): ALIAS16, target views, 1:1 rule, shadow copies,
byte-lane hazards, stale loads/downloads, aliased targets, PUNT path, phase labels; lockstep REF
--no-resync clean on all 24 recordings.

**Status**: complete pending the last regression runs (see In progress); design appendix F written.

## Baseline (start, d6e5dc2)
- gs-hw -RefOnly on hw: 1162 comparisons, 0 scenes differ, 2 differ only with M4 effects
  (t_frame16: 76872 word diffs, t_formats_contexts: 116); t_texture_cache, t_deferred_hazards,
  t_direct_order use M4 effects but are identical.

## Decisions (M4) - details in design appendix F
- Colour targets get an R32_UINT UAV (typeless 32-bit format cast) used by the REF/ROV pixel
  shader (`rov_c` RasterizerOrderedTexture2D<uint>, the raw GS word); mem.hlsl keeps R8G8B8A8_UINT.
- **ALIAS16 = 16-bit staging runs** (generalises design 5.2): gather CS (target halfwords -> R32_UINT
  staging texture in the 16-bit frame's pixel space, whole 16x8 blocks) -> REF draws of any primitive
  class with ps_gs in 16-bit mode -> scatter CS (one thread per word). Runs split only at
  read-after-write hazards; any op on the run's target ends it first (gf_target_op).
- Views: CT32/CT24, alpha index, CT16/CT16S (inverse CT32 table SWZ_ICT32) through the target SRV;
  overlapping-lane self reads: shadow copy (written-or-loaded since last copy); disjoint lanes:
  GPU_TS_SELF (shader reads the bound target). 1:1 sprite flag -> fragment's own sample.
- Uploads keep a block's untouched GPU words with the writer (partly written words download).
- Cross-target copies (GPU_FOP_XCOPY, cs_xcopy) replace download + load between same-kind targets.
- PUNT: gs_front_swdraw + sw_done hook; download + checkpoint + read-back (gpu_front_set_mem_reader)
  into gs_vram (hw-only) or compared (both); patches flushed at once. Same-thread frontend only.
- Lockstep compares post passes; --no-resync is the gate; scripts/gpu_lockstep_all.ps1.

## Done (commits on gpu/M4)
- 611feda ALIAS16 runs, R32 UAV, StateRec 128 B, CT16 views, TS_SELF, 1:1, PUNT path, lockstep.
- 919a5b1 fix: Cd-reading blends were tiled fills.
- (next) uploads keep GPU words, opaque sprites any tier, ROV key with ct/zt (M3 bug, ep6).
- f640111 loads mark sdirty (photo), phase labels, --post-probe.
- (next) XCOPY; ref-bench game-frame GPU ms; 358ce0f gpu_lockstep_all.ps1 + appendix F.
- 9a48678 fix: vertex reuse cache ring position across arena resets (M2/M3 latent bug); PUNT
  patches flushed at once.
- 39a9d2a final hw lockstep 24/24.

## Verification (final code unless noted)
- Lockstep REF --no-resync S=1, hw: 24 recordings, 24,092 comparisons, 0 differ, pictures 0 diff
  (build\m4_lockstep_final.txt). WARP: all 24, 24,092 comparisons, 0 differ (final code,
  build\m4_lockstep_warp.txt). --scale 2 title/rope clean.
- Unit REF scenes: 0 differing on hw and WARP (1162 comparisons each), t_frame16 /
  t_formats_contexts included; forced PUNT 3:15 1078 PUNTs exact.
- PUNT on recordings: title 5:15 both-mode lockstep 184/0; hw-only pictures = REF: title 5:15,
  rope 7:15, rv_prompt 1:31 (all 16-bit prims).
- post-probe: rope 5-6 (70 lines), brief 3-4 (120 lines) identical to gs_post_probe.
- hwdry -Ref: 0 PUNT all 24; steady-state downloads 0 (FMV: 2 at an eviction at v10009).
- GPU ms per game frame REF S=1 (median/worst): ep8 boss 14.6/15.9, ep3 hub 7.5/8.6, ep5 street
  7.7/10.2, ep2 hub 9.8/12.6, ThiefNet 14.8/15.7.
- run_tests.py 16/16; all 20 unit suites (before the vertex-cache fix; gs-hw REF rerun after: pass);
  gpu_diff_all 13/13; VRAM hashes sw 24/24 and hwdry 24/24 = tests/gpu/vram_hashes.txt; build_game
  debug + --release OK; PS2_GS_BACKEND default sw; unit scene t_target_views added (hw + WARP identical).

## In progress
- gpu_fthread_check (build\m4_fthread.txt), last recordings.

## Next
1. Check it; rerun any failing pair; mark complete.

## Findings / traps
- (M3) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
  (Also: a .Replace of a multi-line literal can hit an identical declaration elsewhere - check.)
- `half` is an HLSL keyword (type).
- The lockstep bisection child replays with a checkpoint before every primitive of the range; a
  divergence that disappears there points at draw merging / run batching / arena resets (ep6: ROV key
  without zt; forced PUNT: vertex cache). Binary search with single compares (`--bisect V:k:k`) finds
  the first diverging primitive without changing the batching much.
- PS2_GPU_PROBE=x,y also logs every non-draw op the executor runs (op number, target) and probe-write
  sample positions; PS2_GPU_FRONT_TRACE logs loads, uploads, PUNTs (with primitive index), read-back
  mismatches.
- gs_replay.ps1 build failures are silent when piped to Out-Null: delete the exe before building.
