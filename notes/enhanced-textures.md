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

## How

1. **Content hash** (GPU, cs_et_hash): when an art image is decoded (or first used with Enhanced on), a 64-bit hash of
   its decoded words (texels + CLUT applied, so texture data + CLUT) and its size is computed on the GPU and read back
   asynchronously (staging ring, no stall). Same content under another key (FF vs R2 keys, a level reload) shares one
   enhanced image.
2. **HD-pack hook**: `<user dir>\textures\<hash>.png` replaces the image (decoded with WIC on a worker thread, any size,
   fitted into the slice). `PS2_TEX_DUMP=1` writes every original image to `<user dir>\textures\dump\<hash>.png`.
3. **Upscale** (cs_et_up, our own edge-directed scaler): 4x (cap 1024) of the decoded image in one pass. Per output pixel
   a 4x4 source neighbourhood: the colour structure tensor (RGB + alpha Sobel gradients of the central 2x2, bilinearly
   weighted) gives the edge direction and its coherence; coherent edges are interpolated with a kernel steered along the
   edge (wide along, narrow across: smooth, anti-aliased lines without staircases), the rest with Catmull-Rom clamped to
   the central 2x2's range (no ringing). An unsharp mask at the source scale (the 4x4 Gaussian as the blur), limited to
   the local range (no halos), restores the painted detail the PS2's bilinear filter smeared. Transparent texels take
   the colour of their opaque neighbours before filtering (no dark fringes on cut-outs).
4. **Mip chain** (cs_et_mip / cs_et_cov / cs_et_store): alpha-weighted 2x2 averages; for alpha-tested images (pass A,
   GEQUAL 126) each level's alpha is scaled so the share of texels passing the test equals level 0's (coverage-preserving
   mips: foliage and fences keep their thickness in the distance).
5. **Storage**: square Texture2DArray classes 64..2048 (RGBA8, the GS bytes: alpha 0x80 = 1.0), slices allocated per
   class (grown by doubling, LRU eviction past a budget, PS2_TEX_MB, default 1024); non-square images are stretched
   to the square slice (normalized coordinates make this invisible). Lookup: `et_tab` (t30) indexed by the decoded base
   / 64 holds (decoded base + 1, class | slice); the pixel shader takes the enhanced path only when the stored base equals
   its own.
6. **Sampling** (R2_ETEX / ETEX permutations): trilinear + 16x anisotropic (SampleGrad with the S/Q, T/Q derivatives;
   REPEAT through a wrap sampler, CLAMP through a clamp sampler), the result back to GS bytes and on into the unchanged
   TFX / fog / alpha test / blend. Pass-A cut-outs keep alpha-to-coverage (MSAA), now on a smooth alpha edge.
7. **Budget**: hashing and upscaling run on the render thread's GPU queue, at most a few textures per frame
   (PS2_TEX_FRAME, output texels per frame), so a level's first frames fill in over a fraction of a second instead of a
   frame-time spike. No disk cache: the GPU scaler makes an image in well under a millisecond, faster than reading one.

## Switches (tests, not in the menu)

PS2_TEX=0|1 (wins over the setting), PS2_TEX_UP=0 (mips + anisotropic only, no upscale), PS2_TEX_ANISO=<1..16>,
PS2_TEX_SCALE=<2|4>, PS2_TEX_SHARP=<0..>, PS2_TEX_BIAS=<lod bias>, PS2_TEX_HD=0 (ignore the HD folder), PS2_TEX_DUMP=1,
PS2_TEX_LOG=1, PS2_TEX_SPLIT=1 (pictures: Enhanced on the right half only). In-run A/B: PS2_GPU_AB=<frames>
PS2_GPU_AB_R8=128 (R8P_TEX: Original in A phases, Enhanced in B).

## Results

(filled in as built)
