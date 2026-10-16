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
- (nothing yet)

## In progress
- Families + rescale + per-sample triangle attributes.

## Next
1. Families/rescale/per-sample attributes; lockstep REF at S=2/3 (sample 0) on title, rope, brief, ep8 hub.
2. View reads at scale; thin lines.
3. Compose at scale + flicker/interlace options; presenter resolve.
4. gs_replay: --scale with progressive default, --interlaced, --flicker-off, --thin, picture box-downsample
   PSNR / mean signed error vs sw; FF vs REF all-sample compare tool.
5. In-game settings + hotkeys + motion blur patch; PS2_GS_SCALE.
6. Gates at S=1 (Faithful), lockstep S=2/3/4, FF vs REF S=3, PSNR, contact sheets, in-game hubs at 3x,
   memory / latency investigation; notes + appendix I.

## Findings / traps
- (M6) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- A release game build takes ~4.5 min (build_game.py --release --out build/rel).
