# WIP: gpu-M7 (resolution scaling, presentation quality, graphics settings)

**Goal**: milestone M7 of notes/d3d11-renderer.md section 11 on branch gpu/M7 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M7): scale families, sample grid, per-sample triangle attributes,
single-rate sprites, 1:1 / scaled view reads, compose at scale (progressive / field), presenter resolve
(box / Lanczos-2 / Catmull-Rom / point with grid-shift compensation), thin analytic outlines, PS2_GS_SCALE,
and the owner's settings applied (internal_scale, outlines, filters presets + toggles), at run time.

**Status**: complete (see the end of Done).

## Plan (design decisions)
- Families: display (DISPFB buffers) kx = N, ky = 2N when progressive (interlacing off), else N; square N x N.
  Family is upgrade-only (square -> display): a draw binding ct + zt with different factors, or a 1:1 view
  read between targets of different factors, upgrades both; new C32 targets at a base/FBW shown before
  start as display. A factor change of a live target = GPU_FOP_TARGET with new kx/ky: the executor
  rescales its contents (cs_rescale: new sample (i, j) of native (x, y) <- old sample (i*okx/kx, j*oky/ky),
  sample 0 exact; depth through zpatch, tiles ZT_ZP).
- Run-time reconfigure (scale, progressive, thin) at a VBlank: every target's factors recomputed.
- Triangles: attributes per sample (16.16 rows + floor(dy*j/ky) + floor(dx*i/kx), Z as 3.6, S/T/Q + steps);
  sample 0 unchanged (exact). Sprites / line pixels single rate (as built).
- View reads: 1:1 sample mapping across factors; non-1:1 reads at dest samples != 0 sample the view's sample
  grid (GS bilinear / nearest in sample space; dest sample 0 keeps the native read: exact).
- Thin outlines (scale > 1 only): one record per line, quad around it, coverage max(0, 1 - |d|/w) (AA1) /
  |d| < w/2 (plain), d = minor-axis distance in native px, Z / attributes from the line at the sample.
- Compose at scale: picture W*N x H*N; circuits read target samples (or vram native, nearest); field mode:
  sample row = dby*ky + floor(rel_sub*ky/(2N)) -> ky = 2N progressive, ky = N line doubled (gs.c); flicker
  filter off = the circuit with the larger DBY of the standard pattern disabled; deinterlacer at scale (unit N
  rows) only when interlacing on; progressive: odd field-coded pictures shifted down N rows.
- Present: separable two-pass resample into the letterboxed viewport, grid shift per axis (texels):
  t = (u + 0.5) * P - shift; default auto (down: Lanczos-2, up: Catmull-Rom, 1:1: point).
- Settings: internal_scale (PS2_GS_SCALE), outlines, filters + toggles; motion blur = instruction patch at
  0x160FA0 (after `lw $v1, -0x14DC($v0)` = flag 0x2DEB24: nonzero skips the motion blur function f_00160F98):
  off -> $v1 = 1. Hotkeys F2/Shift+F2 scale, F3 outlines, F4 preset, F5 motion blur, F6 flicker, Shift+F6
  interlacing; persisted like F7/F8.

## Done
- 5215f05 families (upgrade-only; display ky = 2N progressive), cs_rescale on factor change, per-sample triangle
  attributes, compose at scale (display.hlsl: so, field / progressive, odd-field shift, deinterlacer at scale,
  flicker strip in gpu_present.c), presenter resample (present.hlsl ps_resample, gpu_present.c present_resample).
- 80332a8 thin outlines (gpu_rec_thin, PRIM 3, vs_ref_thin, ps_gs_thin, ps_ff_n*), scaled non-1:1 view reads
  (view_hires at samples != 0), signed 1:1 sample offsets (g_voff), gs_replay --game at scale (centred box +
  pixel box, field rows, field-coded pictures), gpu_game_all -Scale -Extra.
- 4837e2c settings: runtime/gfx.c (gfx_apply / gfx_key / gfx_reread), settings scaler key + settings_read_graphics,
  F2-F6 + PS2_INPUT tokens, motion blur instruction patch (verified with gs_census: FIX 48 pass gone, rest equal),
  window unit tests 983/983.
- Results so far: S=1 Faithful game gate 3816/3816 pictures equal; lockstep REF S=2 24092/0 (also S=3 on 6 recs);
  S=3 box gate: 23/24 recordings >= 32 dB (rv_prompt 31.36: vector UI text), max |mean signed error| 0.32.

- Lockstep corpus S=2/3/4 REF + mixed: 24092 comparisons each, 0 differ (before the bbox / Z changes; sample 0
  unaffected by them).
- 13690f3 FF vs REF all-sample compare tool (gs_replay --sample-dump / --sample-compare / --sample-every); fix: REF
  exact STQ at samples whose native row is above the triangle's first row (rows = -1).
- cf3674c triangle bbox widened 1 px up/left at scale (Z band in fix-perf brief), gs_z16_sample (float sub-step).

- FF vs REF corpus S=3 (every target, every sample, every 60 VBlanks; 8 for the short recordings): 24/24 within
  budget, 0 beyond; worst colour words > 2: ep8 boss 1.03% (budget 2.5%), > 16: 0.00008%, Z words 0.00008%.
- Performance (gs_replay --mixed --bench, game-frame GPU medians, S=1 -> S=3): ep3 hub 3.90 -> 9.50 ms, ep5 street
  3.70 -> 11.18, ep8 hub 1.80 -> 5.97, ep8 boss 4.24 -> 10.41. Draw count doubles at scale (exact-split bitmap
  widened) but costs nothing on the GPU (9.42 ms with the widening off). PS2_GPU_PROF=1 (gpu_exec.c): GPU time per
  kind of work. ep5 S=3: FF fast triangles 47%, ROV triangles 18%, ALIAS16 16%, FF sprites 8%, clears 3%, Z resolve
  3%. GS texture sampling is 27% of the FF triangle time (experiment: constant texel). Conservative depth (M9) is
  blocked by the depth encoding: asfloat(z + 0x800000) is near FLT_MIN for Z24, not interpolable (denormal
  products), so it would need per-triangle constant bounds (unique vertices). The 8 ms budget is M9's (design 11).

- Box gate S=2 / S=4 (gpu_game_all -Scale 2/4, 3816 pictures each): 23/24 recordings; rv_prompt 31.42 / 31.20 dB
  (S=3 31.36): its full-width UI rules are Gouraud strips with sharp horizontal edges at a third of a field row
  (e.g. 38.33): the native picture takes the row centre's side, the supersampled one the true coverage, so five
  512-pixel rows differ by 20-40 per channel (diff map: only the rules and glyph edges) - inherent to supersampling,
  not a defect. Min PSNR otherwise S=2 33.45 (pause_save) .. 47.49, S=4 33.08 .. 46.70; max |mean error| 0.34.
- f43fb1c contact sheets notes/m7/*.png (scripts/m7_sheet.py: same native box, 3 output px per native px; 1x tile
  nearest, 3x tile 1:1): shadows (ep5 street v6500), shadows_edge (zoom 6), outlines (ep3 hub v7100: 1x, 3x ps2,
  3x thin), darken (museum v5200), binocucom (fix-perf brief v2515), presets (ep5: Modern 1x | Faithful 1x /
  Modern 3x | Faithful 3x, zoom 2).

- Unit suites 21/21 pass (gs-hw 602 s incl. WARP; window 1010 checks); run_tests 16/16.
- In-game (release, windowed, hw, quiet machine; scratchpad m7game.ps1 = warp_run -Play + PS2_GS_HW_STATS, medians
  of the last 20 one-second lines; committed = private bytes sampled every 5 s), 3x / 1x:
  ep3 hub: 59.9 / 59.9 flips/s, GS 10.4 / 10.1, frontend 15.2 / 13.9, render CPU 1.9 / 1.3, GPU 10.6 / 4.6,
  latency (queued -> Present returned) 3.1 / 2.1 ms, 54.5k prims/VBlank, committed last 935 / 727 MB.
  ep5 hub: 59.9 / 56.2, GS 8.3 / 7.9, frontend 11.1 / 9.2, render 1.3 / 1.0, GPU 9.5 / 4.7, latency 2.1 / 1.8,
  44k / 36k prims, 1047 / 924 MB. ep8 hub: 59.9 / 59.9, GS 4.2 / 4.3, frontend 5.3 / 5.1, render 0.8 / 0.7,
  GPU 8.0 / 4.3, latency 1.6 / 1.4, 21k / 19k prims, 1102 / 898 MB. Peaks 1.4-1.7 GB at load at both scales.
  ep3 at 4:3 1x: 51k prims (not widescreen). GS / frontend are scene-driven (same at 1x).

- 2dccb42 motion_blur.png (in-game 3x ep3 camera turn, on | off); museum in-game 3x / 1x: GS 3.9 / 3.6, frontend
  6.0 / 5.6, GPU 9.3 / 4.6, 59.9 flips/s (quiet machine); run-time switching in game: all tokens applied, no error.
- Debug + release builds and package.py OK (disk ran full once: clean build\warp frames etc.).
- Design doc appendix I written (GATES placeholder for the gate results).

- Gates: gpu_diff_all 13/13 (title rerun after a disk-full failure), VRAM hashes hwdry identical to cd997cd (base
  runtime + scripts from git archive, 24/24), fthread + Mixed 24/24, lockstep S=1 REF / mixed 24092 / 0 failed,
  gpu_game_all S=1 3816 / 0 differ, -Scale 3 -Verify 30 on 6 recordings 0 beyond, in-game PS2_GS_VERIFY=60 at 3x
  ep3 hub 60 comparisons 0 beyond. Appendix I complete.

**Status: complete** (M7 done on gpu/M7; GPU <= 8 ms at 3x on the heavy hubs left to M9, appendix I).
6. Design doc: 3.1/8.1/8.2 notes, appendix I; WIP complete.

## Findings / traps
- (M6) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- A release game build takes ~4.5 min (build_game.py --release --out build/rel).
- Disk C: is nearly full (other agents): pictures at 3x are 6 MB PNGs; delete build outputs after use.
- `line` is a reserved word in HLSL; fxc treats signed int division as a warning-error (use uint).
- Single-rate classes must stay storage-aligned (samples x*k .. x*k+k-1 of the native pixel): a centred
  "nearest native pixel" variant broke the CT16 channel shuffles (16-bit pixel x + 8 -> x, x - 8 -> x + 8: a
  storage pixel written by its neighbour's sprite) - vertical stripes every 8 px. So sprites / line pixels sit
  (k-1)/2k px right/down of the triangles' (sample-centred) position; the presenter centres the triangles.
- The box gate needs two boxes: centred on sample 0 (triangle content) and the native pixel's own samples
  (single-rate content); the better per picture. Field pictures compare per field row (the reference
  line-doubles); field-coded pictures (half-line offset: movies) on the field's own lines.
- Motion blur: f_00160F98 tests *(0x2DEB24) first; nothing writes that word by address (debug flag).
- powershell -File with an array argument (-Extra 'a','b') passes one string "a,b": split on ','.
