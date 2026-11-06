# Enhanced textures (Modern renderer)

Owner request 2026-11-05: "things look very PS2 with just upscaled resolution" - textures clearly sharper and cleaner at
1440p, still Sly 2 (an HD-remaster look, not a redraw), visible in side-by-side pictures, without halos, ringing, smeared
text or broken alpha edges. Branch next/tex. Setting `graphics.textures = enhanced | original` (Options > Graphics >
Textures, live). Enhanced in Balanced / Quality and for a custom sly2.ini without the key; Performance and PS2 (Original
preset) = Original. Original is bit-identical and costs nothing (separate shader permutations; no work at all).

## Where textures come from (what the code says)

| path | what it samples | how (Original) | share of the 3D |
|---|---|---|---|
| R2 materials (r2.hlsl r2_ps) | the material's TEX0 / CBP from GS memory | gs.hlsl tex_sample: level 0 only (SPEC_NOLOD), GS 4-bit bilinear, from the decoded texture cache (texdec: level 0 as 32-bit words, CLUT applied) | static world, props, lit / skinned characters: ~85-90% |
| exact FF triangles on modern frames (mft.hlsl ps_mft_tri, TEXDEC permutations) | StateRec TEX0 + CLUT ring slot | the same tex_sample, decoded cache when the prepass chose it | the VU clipper's fans (near-camera ground), undecoded meshes (billboards), the sky |
| particles (ps_ff_part) | FF state | tex_sample | effects |
| sprites / line pixels / HUD (phase B) | FF state | native-pixel sampling | HUD, text, menus |

Texture statistics, ep1 hub (PS2_TEX_STATS=1, ~670 distinct R2 (texture, CLUT) images): sizes 64x64 (185), 128x128 (135),
32x32 (117), 128x64 (48), 256x128 (40), 64x32 (38), 32x16 (34), ...: nothing above 256. PSM T4 (~80%) and T8 with CT32
CLUTs; every material bilinear but one; CLAMP REPEAT except ~20 (CLAMP or mixed); pass A alpha-tests GEQUAL 126 (of 128),
pass B (the additive highlight with the second CLUT) ATST NEVER. FF textures (fans, billboards) are the same kind.

## Classification (only "art" is enhanced)

- **Art** (enhanced): R2 materials (passes A and B) and exact FF triangles drawn in phase A (the 3D, before the resolve
  point) with a depth target, a vram texture from the decoded cache, bilinear (TEX1), CLAMP / REPEAT addressing, S/T/Q
  (not FST), at least 16 texels on the longer side.
- **Render targets / views** (never): target views are not in the decoded cache at all (TK_VIEW*), so they are excluded by
  construction (Binocucom, portraits, the shadow pipeline's shuffles, motion blur).
- **Dynamic textures** (CLUT animation, per-frame uploads such as the sky): an image whose content version changes a second
  time is rejected for the rest of the cache's life (stays Original): no per-frame upscaling, no flicker between versions.
- **Data / lookup textures**: nearest-sampled (TEX1 MMAG/MMIN 0), REGION_CLAMP / REGION_REPEAT addressing, FST sprites
  and post passes are left alone.
- **HUD, fonts, menus**: left alone. They are 2D sprites drawn in phase B by the exact path's single-rate sprite shaders
  (not the decoded-cache paths); text is designed for its pixel grid and an edge-directed scaler rounds glyph corners and
  smears thin strokes. The HUD keeps its exact look (a future "HD HUD" would need its own sprite path and pack art).
- **Particles**: left alone (soft blobs; nothing to sharpen, and they are fill-bound).


## How (as built: runtime/gpu/gpu_tex.h, shaders/tex.hlsl, shaders/etex.hlsli)

1. **Noted at use**: td_r2 (R2 materials) and td_state (the FF prepass) call et_use for every art use of a decoded entry
   (addressing flags, the alpha test's reference); a new decode of the entry calls et_decoded in stream order.
2. **Content hash** (cs_et_hash, one dispatch per frame, one group per request): at the next frame start a 64-bit hash
   of the entry's decoded words (texels with the CLUT applied: texture data + CLUT) and its size, read back through a
   4-deep staging ring (Map DO_NOT_WAIT: no stall; ~2-3 frames of latency). The image's key = the hash + addressing +
   alpha-tested: the same content under another key (FF vs R2 keys, level reloads) shares one image. A re-decode with the
   same content (a neighbour's upload into the same page) keeps the image; a changed content builds a new one; a second
   change within 30 s marks the texture dynamic (CLUT animation, per-frame uploads): Original sampling from then on.
3. **HD-pack hook**: `<user dir>\textures\<hash>.png` (16 hex digits, the raw content hash) replaces the image. The
   folder is listed once; files are decoded with Windows' own WIC (ole32 loaded at run time) on a worker thread, uploaded
   at a frame start; power-of-two sizes up to 2048, alpha 255 = the GS's 0x80. `PS2_TEX_DUMP=1` writes every original
   image as `<user dir>\textures\dump\<hash>.png` (=2 also the enhanced level 0 as <hash>_enh.png). Tested with a
   generated pack of 470 tinted replacements (ep1 hub): every replaced texture showed it.
4. **Upscale** (cs_et_up, our own scaler): 4x (cap 1024) straight to the square slice in one pass. Per output pixel a 4x4
   source neighbourhood: transparent texels take their opaque neighbours' colour (no dark fringes); the colour structure
   tensor (RGB + alpha Sobel gradients at the central 2x2) gives the edge direction and coherence; coherent strong edges
   use a Gaussian steered along the edge (sigma 1.2 along, 0.35 across: smooth, anti-aliased lines, no staircase), the
   rest Catmull-Rom clamped to the central 2x2 (no ringing); then an unsharp mask at the source scale (amount 2.2, the
   4x4 Gaussian of sigma 0.9 texels as the blur) limited to the range of the 3x3 texels around the nearest one (no
   halos). Low-contrast neighbourhoods (range < 6..18 of 255: a gradient in 4-bit palette steps) are smoothed instead of
   sharpened (no ridges), except thin lines and dots (the nearest texel darker / brighter than both neighbours along x or
   y: grout, seams, strokes), which keep the full mask.
   **Dither resolve first** (cs_et_dd, one dispatch per class before cs_et_up; 2026-11-06 follow-up): most 4-bit textures
   draw smooth shading as ordered / scattered dither between two close palette entries (ep6's teal planks and window
   glass, ep1's grass and stone, ep3's bark). The PS2's bilinear filter blurred it; upscaling + the unsharp mask (and the
   dot exemption above) turned it into crisp dot grain. Per source texel, from a 16x16 group-shared tile: the luminance
   residual against a 3x3 binomial and its sign; a texel whose residual is coherent with neither pair of opposite
   neighbours (an isolated dot) or with both diagonals only (a checker) is dither-like, one coherent along a direction (3 in
   a row: grain, grout, strokes) is a line. Dither-likeness = most active texels of the 5x5 dither-like (>= 45..70%), at
   least 1..3 of them, local contrast below PS2_TEX_DD_RANGE (40; the distance is the luminance difference or 2.5x the
   hue / saturation difference, so rust dots on teal planks are detail), a line texel itself x0.15. Where >= 4..7 of the 25
   are dither-like (an area, PS2_TEX_DD_DENSE) the texel takes the range-limited 3x3 binomial (the dither's average;
   neighbours beyond the range or line texels next to it count little); a sparse row of dots (stitching, rivets, a dotted
   streak) stays. cs_et_up then: no dot / line exemption on a dither-like nearest texel (its own value, not the
   interpolated one: a dithered tile's would spill onto its grout line and soften it) and no deband blur; resolved texels get the
   full unsharp mask (it now sharpens the shading's real structure - cracks, rust, streaks; PS2_TEX_DD_SHARP), unresolved
   dither-like ones none (soft dots as the PS2 drew them). PS2_TEX_DEDITHER=0 turns it off (exactly the earlier output).
   The resolved sources and weights go to their own 4 MB buffer (written through a UAV, read by cs_et_up through an
   SRV - the cached load path; reading them back through the scratch UAV cost more on build frames).
   Tried and dropped: an "edge" guard (the binomial's gradient against the residuals) - steep dithered ramps such as glows
   pass it too and came back as grain; resolving every dotted pattern (range 40, no density rule) - also erased stitch rows
   and the dotted streaks the original filter showed softly.
5. **Mip chain** (cs_et_mip / cs_et_cov / cs_et_store): RGB is the Catmull-Rom value half-way between each 2x2 (crisper
   than a box), clamped to the 2x2's range; alpha the box average. Alpha-tested images (pass A: GEQUAL 126) get each
   level's alpha scaled so the share of texels passing the test equals level 0's (coverage-preserving mips; histograms in
   group-shared memory, 8 copies against atomic contention).
6. **Batches**: all images built in a frame are one job table; each pass is one dispatch per class and mip level over all
   of them (SV_GroupID.z = job). The first version dispatched per image and level (~25 per image, ~400 per frame at a
   level start) and the barriers between them cost up to 50 ms GPU in one frame.
7. **Storage**: square Texture2DArray classes 64..2048 (RGBA8 GS bytes), pre-sized to a hub's set (64: 64, 128: 256,
   256: 256, 512: 256, 1024: 64 slices = 790 MB; ep1 builds 690 images) because a growth copies the whole array (a GPU
   spike); grown by doubling within PS2_TEX_MB (default 1024), then LRU reuse of images unused for 10 s. Non-square images
   are stretched to the square slice (normalized coordinates). Lookup: `et_tab` (t30), entry = decoded base / 64:
   (decoded base + 1, class | slice << 3); the pixel shader uses it only when the stored base equals its own.
8. **Sampling** (R2_ETEX / ETEX permutations, shaders/etex.hlsli): SampleGrad with the S/Q, T/Q derivatives (taken
   before any divergent branch), trilinear + 16x anisotropic, LOD bias -0.75; REPEAT through a wrap sampler, CLAMP through
   a clamp sampler (a REPEAT axis wrapped in the shader), REGION modes and nearest (TEX1) uses the original way. The texel
   goes back to GS bytes into the unchanged TFX / fog / alpha test / A2C / blend.
9. **Budget**: at most 1 M output texels built per frame (PS2_TEX_FRAME); a hub's images fill in over the load's last
   seconds. No disk cache: building an image costs well under a millisecond of GPU, less than reading one from disk.

## Original stays Original

graphics.textures = original (or PS2_TEX=0): et_cfg() is false, so no resource exists and no hash / build / table write
happens; the pipelines are the old ones. The 250 shaders main has compile byte-identical on this branch (compared in
build\gen\gpu_shaders.h); the 19 new ones (12 R2 PS, 2 MFT PS, 5 CS) are only used in Enhanced.

## Switches (tests, not in the menu)

PS2_TEX=0|1 (wins over the setting), PS2_TEX_UP=0 (mips + anisotropic of the original images only), PS2_TEX_SCALE=2|4|8,
PS2_TEX_SHARP, PS2_TEX_SIGMA_C / _SIGMA_A, PS2_TEX_EDGE, PS2_TEX_FLAT / _DETAIL, PS2_TEX_DEDITHER=0..1 (0: off),
PS2_TEX_DD_RANGE (40), PS2_TEX_DD_DENSE (4), PS2_TEX_DD_SHARP (1), PS2_TEX_ANISO=1..16, PS2_TEX_BIAS,
PS2_TEX_MB, PS2_TEX_FRAME, PS2_TEX_HD=0, PS2_TEX_DUMP=1|2, PS2_TEX_LOG=1, PS2_TEX_SPLIT=1 (Enhanced right of the middle),
PS2_TEX_DBG=1 (enhanced texels red: coverage) / 2 (grey by class), PS2_TEX_STATS=1 (every texture's format and use).
In-run A/B: PS2_GPU_AB=<frames> PS2_GPU_AB_R8=128 (R8P_TEX), PS2_GPU_AB_LOG=1 logs the phase switches; the ab-gpu line
now has an ab-worst companion (p99 and maximum of GPU and render-thread time per phase), and tex-build reports the
frames that built images. Pictures: scripts\tex_shots.ps1 (A/B pictures of one run, like light_shots.ps1).

## Tuning findings

- Coverage (PS2_TEX_DBG=1, ep1 hub): every 3D surface but particles (the sky's moon and clouds included; it is not
  re-uploaded); the HUD untouched. ep1: ~690 images, 0 re-decodes / rejections over 3 minutes.
- An 8x scale looked no sharper than 4x on screen (ep6 close-ups): the source's information, not the resolution, is the
  limit; the unsharp mask's amount is what reads as "sharper".
- More negative LOD biases (-1.25, -1.75) changed little at a distance; -0.75 keeps distant surfaces as crisp as the
  original's aliased level 0 (zoomed: Enhanced is the more legible, e.g. ep1's "BISTRO DE PARIS" menu board) without its
  shimmer.
- The deband first faded the m2 pool floor's thin grout lines (low contrast, mistaken for palette steps): thin lines and
  dots are now exempt and come out crisper than Original.
- Traps: `line` is an HLSL keyword; PowerShell `@('a', 'b' + 'c')` is ('a','b') + 'c' (an array of three) - multi-part
  strings in array literals need parentheses.

## Results

**Cost** (in-run A/B on this machine, shared GPU: `PS2_GPU_AB=24 PS2_GPU_AB_R8=128`, ep1 hub, Balanced = 2560 x 1440 MSAA
4x with Enhanced lighting in both phases, unlimited frame rate, 6000 pictures per phase, ca997a0):

| ep1 hub | Original | Enhanced | delta |
|---|---|---|---|
| GPU per picture, average (+ present) | 6.15 ms | 5.93 ms | -0.22 |
| GPU p99 | 8.52 ms | 8.32 ms | -0.20 |
| GPU worst | 14.05 ms (a build frame) | 9.71 ms | |
| render thread p99 / worst | 1.96 / 22.79 ms (one outlier) | 1.96 / 2.64 ms | |
| frames that built images (the hub's load, 164 frames) | | 6.34 ms average, worst 14.05 | +0.37 vs 5.97 |

Enhanced is cheaper on the GPU than Original: hardware trilinear / anisotropic fetches of an RGBA8 mip chain cost less
than the decoded cache's four loads and the 4-bit bilinear arithmetic per pixel. On the owner's PC (ep1 Balanced ~3.54 ms
unloaded) that is ~3.4 ms (~290 fps): above 165. Building costs ~1.6 ms of GPU per million output texels here (budget 1 M
per frame), only while a level's textures first appear (the load / fade-in); with the classes pre-sized no array grows
in ep1. Memory: ~790 MB of VRAM for the image classes while Enhanced is on (none with Original).

**Gates** (on ca997a0): run_tests 16/16; units 0 failed (menu 365 checks); m9_exact S1 + S3 -Game identical (base:
main 7601e3f's runtime); gpu_diff_all mixed S=1 28/28; r2_sweep -Parallel 1 PS2_TEX=0 ep1 ab, tfo, tfb, mf + m2_boardroom
tfo, tfb, mf 7/7; PS2_TEX=1 ep1, ep3, m2_boardroom tfo, tfb, mf 9/9 (0 pops, 0 stray triangles, 0 blinking seams).

**Pictures** (local only, never committed): build\tex\shots\final\ - for ep1_hub (night), ep3_hub, ep6_hub (dusk, close
wooden houses), m2_boardroom: <scene>_original.png / _enhanced.png (2560 x 1440, the same scene from one A/B run),
<scene>_compare.png (the middle 1280 columns of each side by side, Original left) and <scene>_closeup.png (a 640 x 720
region of each, 2x, Original left); ep6_hub_ground_*: the planks and barrel. What shows: wood grain, boards and window
frames crisp (ep6), leaf outlines on ep1's vine wall, legible signs (ep1's menu board), crisp tile grout (m2), smooth
alpha edges on ep3's hanging moss; no halos, no ringing, no fringes along cut-outs.

**Known limits**: the HUD, text, menus and particles keep the PS2's sampling (by design); Reference / integer-grid frames
(Modern with an exact multiple, menus, loading screens) are never enhanced; FST (fixed-point UV) exact triangles and
region-addressed or nearest-filtered textures keep the original sampling; textures that change content twice within 30 s
are left Original; HD-pack images must be power-of-two sized (up to 2048); dithered areas are resolved to their average
shading, so a texture whose fine noise was painted as such (some grass, stone and carpet) comes out smoother than the
PS2's filtered look, and sparse dotted rows stay as soft as the PS2 drew them (not sharper); the first seconds of a level show a texture's original until its image is built.
