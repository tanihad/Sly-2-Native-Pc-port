# WIP: gpu-M5 (FF tier behind lockstep)

**Goal**: milestone M5 of notes/d3d11-renderer.md section 11 on branch gpu/M5 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M5): FF permutations, blend table (4.2), dual source, exact
SV_Depth, ATST As-range + split, tiled fill -> Clear, StateRec merging, ROV runs with zpatch + Z resolve in
a mixed frame, pass-A straddle escalation flag; classes moved ROV -> FF one at a time behind lockstep.

**Status**: in progress (resumed after a pause by the user; 3fd6cb7 = the paused state, verified since).

## Baseline (start, 06b9bc5)
- REF lockstep title: 184 comparisons, 0 differ (build\m5_base).

## Design decisions (M5)
- **Mixed frames** (`GPU_FD_MIXED`, gs_replay `--mixed`): FF draws = RTV (UNORM) + DSV (D32, SV_Depth =
  asfloat(z + 0x800000)); ROV and ALIAS16 draws = the REF pixel shader (colour UAV + zpatch UAV as depth).
  Same VS / viewport (REF 16384 centred) for both, so coverage is identical.
- **Depth in two places, synced lazily per 16x16-native-pixel tile** on the render thread: state per tile
  SYNC / DSV newer / ZP newer. FF draw with Z: zp->DSV for ZP-newer tiles of its bbox (quad, SV_Depth from
  zpatch = the design's ps_zresolve), then marks DSV-newer if it writes Z. REF-path draw with Z (ROV run,
  ALIAS16): DSV->zp for DSV-newer tiles of its bbox (cs_zsync), marks ZP-newer if it writes Z. Memory ops
  (load/download/xcopy/read-back) work on zpatch after a DSV->zp sync. Z24 keeps zpatch's top byte. Clears:
  zpatch by cs_clear, the DSV by ClearDepthStencilView (whole) or a constant SV_Depth quad.
- **FF pixel shader = the REF pipeline's front half** (gs_frag: records -> exact integer colour/fog/UV/Z,
  texture, TFX, fog, ATST) + OM: BL_OFF exact in PS (incl. wrap), ADD / REVSUB (ONE,ONE) exact, DUAL lerp
  SRC1_ALPHA = C/128 with the source lowered by dual_bias/a (WARP 0.496: exact unless Cs*C<64; hw 0.42:
  +-1 balanced), NOCOLOR write mask. Alpha ONE/ZERO = As (| 0x80 FBA).
- Triangle records in mixed frames: default 128 B (integer part; S/T/Q interpolated by the hardware, FF and
  ROV: ps_ff_t*1, ps_gs_tri_fast); `--ff-stq-exact`: 256 B, S/T/Q stepped exactly (verification).
- ATST split: draw `split` bits 1 pass draw / 2 fail draw (atst_mode constant, 2 constant slots when both),
  fail-draw state from AFAIL. **Escalation default ON in mixed frames** (straddling pass-A primitives -> ROV,
  sticky per state for the frame); `split_ff` / `--split-ff` keeps the split.
- Classes moved one at a time with `gpu_front_config.ff_rov_mask` (`--ff-rov-mask`, GPU_FFC_* bits).
- Tiled fill -> Clear: the fill's FF draws are dropped from the command stream when the clear is recognised.

## Done (commits on gpu/M5)
- dd341c7 mixed frames: gs.hlsl gs_frag + ps_gs + ps_ff (`FF=1` perms without UAVs); zsync.hlsl; gpu_exec.c
  exec_draw_ff, tile depth sync, mixed clears, split draw slots; gpu_front.c FF records, FF key, split halves,
  ff_rov_mask; gs_replay --mixed / --ff-stq-exact / --ff-rov-mask / --lockstep-exact.
- 60dcf4a fill -> clear drops FF draws; statistical gate; render-bench line; gpu_lockstep_all -Mixed etc.
- 3fd6cb7 (paused) unit harness variants: test_gs_draw TEST_HW_MIXED (exact) / TEST_HW_MIXED_DEFAULT
  (tolerance), gs-hw run.ps1 -Modes ref,mixed,mixdef.
- (next) bisect child exits 0 past its range (single-comparison binary search, scratch bsearch.ps1).
- 0c062e8 escalation default + sticky; fast-STQ ROV triangles; per-adapter dual bias; parity-aware picture
  gate; 2.5% chain budget; --tol-report; PS2_GPU_DBG_ROVSKIP.
- Class progression (exact lockstep, S/T/Q exact, 8 recordings; 7683 comparisons each step, 0 differ):
  A OFF sprites, B +depth, C +triangles, D +lines/points, E +NOCOLOR, F +ADD, G +REVSUB, H +views/feedback,
  I +ATST split (build\m5_prog.txt).
- Exact mode on all 24 (-Exact -StqExact -FfRovMask 0x10, split FF): 22/24 0 differ; ep1_hub and m1_brawl
  differ only through the additive pass-A split (bisected: prim 33268 frame after VBlank 7144 of ep1 hub,
  TEST GEQUAL 126 FB_ONLY, (Cs-0)*As+Cd: alpha 6D vs 7E, G F3 vs FF); with escalation all 24 0 differ.

## Pass-A split report (S/T/Q exact, DUAL FF, tolerance compare; split vs escalation; build\m5_split)
Z differs nowhere in either. Split adds differences above 2 / above 16 (words over the recording):
ep6 721k / 186799 (vs 355k / 0 escalated; 4 pictures beyond the gate, PSNR 48.7), ep2 hub 585k / 1442 (vs 1.6k / 0),
ep3 hub 28k / 0 (vs 7), ep1 hub 38.7k / 286, m1 brawl 2.5k / 820, rope 7.8k (vs 0), pause_save 15k (vs 24),
rv_intro 6.5k (vs 2.8k); others equal. => visible: escalation default ON (design rule 4.3).
Cost (ep8 boss, S=1, GPU ms per game frame median): REF 15.1, escalation 7.0, split 3.9. Experiment
(PS2_GPU_DBG_ROVSKIP): the cost is the FF <-> UAV switches and UAV draws (each D3D11 UAV draw waits for the
previous; ~20 us per switch pair), not ROV pixel work (a trivial shader in the ROV draws costs the same).

## Dual-source blend precision (scratch blendtest.c: every Cs, Cd, C 0..128 on a UNORM8 target)
- WARP (fp32 blender): bias 0.496/a exact except Cs*C < 64 (0.37%).
- RTX 4060 Ti: ~fp16 blender (+-0.125 LSB): no bias exact; 0.45: 80% exact (-1 13%, +1 6%); 0.42: 80% exact,
  -1 and +1 10% each (chosen: balanced errors do not drift). C = 128 and C = 0 exact unbiased.
- Chains: translucent stacks with small alpha accumulate up to ~1/a: ep8 boss worst comparison 1.94% of words
  in 3..16 (never > 16), ep6 0.75% => lockstep budget for > 2 raised to 2.5% (> 16: 0.1%, Z 0.1%, CPU 0).
- Reference-compose pictures: the deinterlacer's still detection (px_close threshold 6) turns +-1 into whole
  line switches (load_m3 VBlank 2346: 1564 px > 16 on odd lines only) => picture gate counts the parity with
  fewer differences.

## Next
1. Full corpus default mode (escalation) hw; REF regression 24/24; exact mode 24/24 with escalation (done
   for 8, rerun all with final code).
2. Unit tests mixed mode hw + WARP (gs-hw run.ps1), all other unit suites.
3. Perf table: GPU ms per game frame (median/worst) REF vs mixed for ep8 boss, ep3 hub, ep5 street, ep2 hub,
   ThiefNet; render-thread ms and D3D draws all 24 (render-bench); M2 budget.
4. WARP subset; appendix G; regression (run_tests, unit suites, gpu_diff_all, VRAM hashes, fthread check,
   builds debug/release).

## Findings / traps
- (M4) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- gs_replay.ps1 build failures are silent when piped to Out-Null: delete the exe before building.
- `pass` is an HLSL keyword. A pixel shader writing render targets cannot declare UAVs at u0 (outputs share
  the namespace): FF permutations compile with FF=1 (no ROV declarations).
- Native stderr redirected by PowerShell (`2> file`) is wrapped at 120 columns: join lines before parsing.
- The lockstep bisection child (compare before every primitive) changes batching; use single comparisons
  (`--bisect V:k:k`, scratch bsearch.ps1) for a binary search.
