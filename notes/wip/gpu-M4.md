# WIP: gpu-M4 (Exact effects)

**Goal**: milestone M4 of notes/d3d11-renderer.md section 11 on branch gpu/M4 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M4): ALIAS16, target views, 1:1 rule, shadow copies,
byte-lane hazards, stale loads/downloads, aliased targets, PUNT path, phase labels; lockstep REF
--no-resync clean on all 24 recordings.

**Status**: in progress.

## Baseline (start, d6e5dc2)
- gs-hw -RefOnly on hw: 1162 comparisons, 0 scenes differ, 2 differ only with M4 effects
  (t_frame16: 76872 word diffs, t_formats_contexts: 116); t_texture_cache, t_deferred_hazards,
  t_direct_order use M4 effects but are identical.

## Plan / decisions (M4)
- Colour targets get an R32_UINT UAV (typeless 32-bit format cast) used by the REF/ROV pixel
  shader (`rov_c` as RasterizerOrderedTexture2D<uint>, the raw GS word) and by atomics; mem.hlsl keeps
  the R8G8B8A8_UINT UAV.
- **ALIAS16 = 16-bit staging runs** (generalises design 5.2's sprite CS): a run of primitives into a
  CT16/CT16S frame over a C32 target is executed as gather CS (target halfwords -> R32_UINT staging
  texture in CT16 pixel space, block-rounded bbox) -> ordinary ROV draws of any primitive class with
  ps_gs in 16-bit frame mode (StateRec flag; DTHE/DIMX in the StateRec) -> scatter CS (halfwords back
  with InterlockedAnd/Or). ROV orders overlapping writes; reads of the target (CT16 views) come from the
  target's SRV = the state before the run, so the frontend splits a run at read-after-write hazards
  (existing rectangle check). Triangles/lines into CT16 frames therefore need no PUNT.
- Texture views: CT32/CT24, alpha index (T8H/4HL/4HH) and CT16/CT16S views of C32 targets (inverse
  CT32 table in swz_tab) through the target SRV; self-reads with disjoint byte lanes read the bound UAV
  (no copy); overlapping lanes: shadow copy of blocks written since the last copy.
- 1:1 rule: sprites stepping one texel per pixel sample the view at the fragment's sample (i, j);
  other reads sample 0 (scaled bilinear: M7).
- PUNT (5.4): same-thread frontend; the frontend downloads the touched blocks, reads them back into
  gs_vram (hw-only) or compares them with gs_vram (both), asks gs_draw.c to rasterize the primitive in
  software (hook return value), then patches the written blocks. Forced-punt debug switch for tests.
- Lockstep: no skip after post passes; resync after a differing VBlank only without --no-resync.

## Done
- (nothing yet)

## In progress
- R32 UAV + gs.hlsl rov_c as uint.

## Next
1. R32_UINT UAV for colour targets; rov_c uint; unit scenes still 0 diff (hw).
2. StateRec 128 B (+DIMX); FB16 mode in ps_gs; gather/scatter CS; frontend ALIAS16 runs as draws;
   executor staging; t_frame16 / t_formats_contexts identical; unit harness counts M4 scenes as bad.
3. Views: CT16 views anywhere (no TEX_FMT PUNT for CT16), self-UAV reads, 1:1 sample offset,
   shadow resize on target growth.
4. Lockstep without skip; --no-resync on the 13 baseline recordings, fix divergences.
5. The 11 corpus-extension recordings.
6. PUNT path + forced-punt tests (unit scenes both mode; hw-only replay pictures).
7. Phase labels.
8. gs_post_probe comparison ep3 rope f5-f6, fix-perf brief f3-f4.
9. GPU ms REF S=1: ep8 boss, ep3 hub, ep5 street, ep2 hub, ThiefNet.
10. Full regression: run_tests.py, unit suites, gpu_diff_all, VRAM hashes, fthread check, builds.
11. Design appendix F, WIP final.

## Findings / traps
- (M3) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
