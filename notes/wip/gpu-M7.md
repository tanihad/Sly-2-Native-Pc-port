# WIP: gpu-M7 (resolution scaling, presentation quality, graphics settings)

**Goal**: milestone M7 of notes/d3d11-renderer.md section 11 on branch gpu/M7 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M7): scale families, sample grid, per-sample triangle attributes,
single-rate sprites, 1:1 / scaled view reads, compose at scale (progressive / field), presenter resolve
(box / Lanczos-2 / Catmull-Rom / point with grid-shift compensation), thin analytic outlines, PS2_GS_SCALE,
and the owner's settings applied (internal_scale, outlines, filters presets + toggles), at run time.

**Status**: in progress.

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

## In progress
- Lockstep corpus S=2/3/4 REF + mixed (scratchpad lockall.ps1, results build\ls_{ref,mix}{2,3,4}.txt); the first
  attempt died of a full disk (C: had 28 MB free: keep build outputs small, GS_REPLAY_MAX_PNG=6).

## Next
1. FF vs REF all-sample compare at S=3 (gs_replay --sample-dump / --sample-compare of every target, 10.2 budget).
2. Box gate at S=2 and S=4 (gpu_game_all -Scale 2/4); Modern (--modern) runs.
3. Contact sheets (notes/m7/*.png, small): shadows, outlines ps2/thin, darken, motion blur on/off (in-game
   dumps), Binocucom, Modern vs Faithful at 1x/3x.
4. In-game: hw at 3x hubs ep3/ep5/ep8 windowed, PS2_GS_HW_STATS (GS <= 6, render <= 8, GPU <= 8 ms medians,
   60 flips/s); run-time switching (F2..F6 via PS2_INPUT tokens); memory (850 MB) and present latency.
5. Gates: run_tests, unit suites (gs-hw, gs-front, gs-draw, window...), gpu_diff_all, VRAM hashes,
   gpu_fthread_check, lockstep S=1, gpu_game_all S=1, debug + release builds, package.py.
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
