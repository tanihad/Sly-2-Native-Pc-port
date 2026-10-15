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
- 611feda: R32_UINT colour UAV (rov_c uint); StateRec 128 B (DIMX, phase, hint label); ALIAS16 runs
  (frontend alias_prim/close_alias, GPU_FOP_ALIAS16 begin + ALIAS16_END, executor exec_alias16,
  shaders/alias16.hlsl gather/scatter, ps_gs FB16 mode incl. DTHE/DIMX, 16-bit DATE/blend/FBA/mask);
  CT16 views anywhere (view_word_at + SWZ_ICT32 inverse table); TS_SELF reads; 1:1 sprite flag +
  vflags; PUNT path (punt_prim/fr_sw_done, gs_front_swdraw in gs_draw.c, mem reader, PS2_GPU_PUNT);
  phase hint forwarded (gs_env_view.phase, fthread); lockstep compares post passes, resync only after
  a differing frame without --no-resync. Unit REF: 0 differing scenes on hw and WARP (t_frame16,
  t_formats_contexts included); forced PUNT every 3rd prim: 1078 PUNTs exact, read-backs equal.
- 919a5b1: fix: blends reading Cd were taken for tiled fills (bit-plane steps -> a colour clear).
- next commit: uploads over GPU words keep untouched words GPU (no download; was 34 downloads per
  game frame in title), opaque sprites in every tier (Z clear loads gone), ROV draw key includes ct/zt
  (M3 bug: ep6 divergence). Baseline 13: all lockstep --no-resync clean except ep6 before the key fix;
  ep6 1062/0 after.

## In progress
- lockstep --no-resync on all 24 (scratchpad lk_all.ps1 -> build\lk\summary_all.txt).

## Next
1. Steady-state downloads/PUNT per recording (dry.ps1, hwdry -Ref metrics) on all 24.
2. Phase labels in traces/metrics.
3. gs_post_probe comparison ep3 rope f5-f6, fix-perf brief f3-f4.
4. GPU ms REF S=1: ep8 boss, ep3 hub, ep5 street, ep2 hub, ThiefNet.
5. Full regression: run_tests.py, unit suites (gs-front may need updated expectations), gpu_diff_all,
   VRAM hashes, fthread check, builds debug/release; WARP unit scenes.
6. (bonus) lockstep at --scale 2 (sample 0) on a few recordings: scale-readiness of ALIAS16/views.
7. Design appendix F, WIP final.

## Findings / traps
- (M3) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
  (Also: a .Replace of a multi-line literal can hit an identical declaration elsewhere - check.)
- `half` is an HLSL keyword (type).
- The lockstep bisection child replays with a checkpoint before every primitive of the range; a
  divergence that disappears there points at draw merging / run batching (ep6: ROV key without zt).
- PS2_GPU_PROBE=x,y now also logs every non-draw op the executor runs (op number, target).
