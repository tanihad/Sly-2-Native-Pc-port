# WIP: gpu-M5 (FF tier behind lockstep)

**Goal**: milestone M5 of notes/d3d11-renderer.md section 11 on branch gpu/M5 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M5): FF permutations, blend table (4.2), dual source, exact
SV_Depth, ATST As-range + split, tiled fill -> Clear, StateRec merging, ROV runs with zpatch + Z resolve in
a mixed frame, pass-A straddle escalation flag; classes moved ROV -> FF one at a time behind lockstep.

**Status**: complete (verification passed; design appendix G written).

## Design decisions (details in design appendix G)
- Mixed frames (`gpu_front_config.mixed`, `GPU_FD_MIXED`, `gs_replay --mixed`): FF = RTV + DSV, OM blend and
  depth (SV_Depth = asfloat(z + 0x800000)); ROV / ALIAS16 = the REF shader with zpatch as depth; same VS and
  viewport as REF (identical coverage).
- Depth synced lazily per 16x16 tile between DSV and zpatch (zsync.hlsl), no zpatch serials.
- FF pixel shader = the REF fragment pipeline (gs_frag) + OM; records 128 B with hardware S/T/Q (default) or
  256 B exact (`--ff-stq-exact`).
- Dual source: +-1 on the RTX 4060 Ti whatever the bias (blender ~fp16); bias 0.42/a (hw), 0.496/a (WARP).
- ATST split: exact split (span bitmap of fail-capable primitives per split draw) is the default; plain split
  `split_ff`; ROV escalation `straddle_rov` (measured slower than REF in the hubs).
- Tiled fill -> clear drops the fill's FF draws.

## Done (commits on gpu/M5)
- dd341c7 mixed frames core; 60dcf4a fill drop, tolerance gate, gpu_lockstep_all -Mixed; 3fd6cb7 unit harness
  variants (paused commit); 0c062e8 escalation experiment, fast-STQ ROV, per-adapter dual bias, picture gate;
  397b6b8 exact split default; ef9ffe7 design appendix G; 1ba7e13 hlslc threads + cache, fthread check
  builds once (+ -Mixed).

## Verification (final code unless noted)
- hw, all 24, lockstep --no-resync: mixed default 24,092 comparisons 0 beyond budget, 0 pictures beyond the
  gate; exact mode (-Exact -StqExact -FfRovMask 0x10) 24,092 / 0 differ, pictures identical; REF 24,092 / 0.
  (build\m5_final\out2.txt)
- WARP 9 recordings: mixed default and exact 8,166 / 0 each.
- Unit: gs-hw full on hw (55 base checks + scenes ref / mixed / mixdef: 0 differing scenes), scenes on WARP
  (ref / mixed / mixdef: 0); all other unit suites pass (gs-front expectations updated for M5); run_tests
  16/16; hwdry_all 24/24 (0 PUNT, VRAM unchanged); sw VRAM hashes 24/24 = tests/gpu/vram_hashes.txt; game
  build debug + --release OK; PS2_GS_BACKEND default unchanged (sw).
- Class progression A-I exact on 8 recordings (build\m5_prog.txt).
- GPU per game frame S=1 steady (REF -> mixed, median): ep8 boss 15.0 -> 4.1, ep3 hub 7.8 -> 3.2, ep5 street
  8.3 -> 3.4, ep2 hub 10.2 -> 3.1, ThiefNet 14.9 -> 6.0; render CPU median 1.1-1.6 ms, max 1.7-2.4 ms.

- gpu_fthread_check 24/24 identical, also with -Mixed (24/24); gpu_diff_all 13/13 (0 px).

## Next
- Nothing: M5 complete.

## Findings / traps
- (M4) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- gs_replay.ps1 build failures are silent when piped to Out-Null: delete the exe before building.
- `pass` is an HLSL keyword. A pixel shader writing render targets cannot declare UAVs at u0 (outputs share
  the namespace): FF permutations compile with FF=1 (no ROV declarations).
- Native stderr redirected by PowerShell (`2> file`) is wrapped at 120 columns: join lines before parsing.
- The lockstep bisection child (compare before every primitive) changes batching; use single comparisons
  (`--bisect V:k:k`) for a binary search (scratch bsearch.ps1 in the session; the child now exits 0 past
  its range).
- D3D11 on the RTX 4060 Ti: every draw writing a UAV waits for the previous one; FF <-> UAV switches ~20 us.
- The reference compose's deinterlacer turns +-1 into whole-line switches: gate one line parity.
- hlslc compile of the ubershader permutations took ~10 min sequentially: now threaded + cached.
- GPU timings: the first game frames after the start (and after first use of each permutation) are slow;
  short recordings with an idle GPU vary a lot (rv_prompt 0.9-16 ms).
