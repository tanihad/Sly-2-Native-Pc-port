# Reimagined (Look = Reimagined): textures and surface maps

Owner direction 2026-11-06: Look = Retro / Faithful / Reimagined; Reimagined is "a bold, visibly modern reimagining of
Sly 2 - I don't mind if it changes a lot ... go above and beyond". Two agents build it: **textures + surface maps**
(this note; branch next/ai: runtime/gpu/gpu_tex_ai.h, gpu_tex_normals.h, shaders/tex_ai.hlsl, etn.hlsli, ai_up.*) and
**lighting / colour grading** (branch next/reimagine: gpu_light.h, light.hlsl, mft.hlsl, r2.hlsl lighting parts). They
meet at the contract below. Faithful and Retro are untouched (nothing here runs unless the look is Reimagined).

## Contract for the lighting (surface maps)

**What**: under Reimagined every enhanced image (R2 materials and exact phase-A triangles from the decoded cache - the
textures `et_sample` finds; not particles, HUD, text, render targets) has a **surface map** built with it: first from the
Enhanced image, again from the AI image when that replaces it. Maps are never on disk (rebuilt from the colour image in
the same batch: ~0.1 ms per million texels).

**Storage**: per colour class c (square slices of 64 << c texels, t31..t36) a map class of **32 << c** texels per side
(half the colour's resolution = 2x the PS2 texture's density), **the same slice index**, its own full mip chain,
`R8G8B8A8_UNORM`:

| channel | meaning |
|---|---|
| r, g | tangent-space normal x, y stored as 0.5 + 0.5 n. **+x = increasing u, +y = increasing v** (down the image); z = sqrt(1 - x^2 - y^2) |
| b | height 0..1 (0.5 = the local mean; dark paint is low: grooves, grout, cracks) |
| a | cavity 0..1 (1 = open surface, lower in creases: an ambient-occlusion factor) |

Height = luminance minus the mean of its 4x and 8x coarser mip levels (a band-pass: large shading gradients painted into
a texture do not tilt the surface, painted detail does). Slope scale `PS2_TEX_NRM_K` (default 10, in level-0 map texels):
scale x, y yourself (and renormalize) for more or less relief. Mip levels come from the colour's own lower levels with
slopes in level-0 units, so distant surfaces flatten.

**Shader side**: `#include "etn.hlsli"` after `etex.hlsli` (it uses et_tab t30, samplers s4 / s5 and e_ps b8) and call

```hlsl
bool etn_sample(uint dec, uint4 tc, uint tlev, float2 uv, float2 dx, float2 dy, out float4 m);
// same arguments as et_sample (decoded base, TEX0 / CLAMP words, filter bits, S/Q T/Q and their screen derivatives)
// true: m.xy = tangent normal x, y (-1..1), m.z = height, m.w = cavity; false: no map -> flat normal (0, 0), 0.5, 1
```

Registers: **t40..t45** map classes 0..5, **t46** `etn_ok` (ByteAddressBuffer, word `class * 4096 + slice` = 1 when the
slice has a map). The tangent frame is the lighting's to build (screen-space derivatives of the position and of uv, as
usual for meshes without tangents); the texture's v axis grows downwards.

**C side** (gpu_tex_normals.h, render thread, one translation unit gpu_exec.c): `etn_enabled()` (1 while this frame's
draws can use maps), `etn_bind_ps()` (binds t40..t46; `et_bind_ps` already calls it whenever maps are enabled),
`etn_srv(cls)`, `etn_ok_srv()`. Under Faithful / Retro `etn_enabled()` is 0 and nothing is bound.

**Turning on**: Look = Reimagined (`graphics.look = reimagined`; gfx.c calls `gpu_tex_look`) or `PS2_TEX_AI=1` for
tests; `PS2_TEX_NRM=0` builds no maps. Switching the look rebuilds every image (a burst over a few frames).

## Textures (AI images)

### Choice (evaluation 2026-11-06, test set ep1 / ep3 / ep6 hubs + m2 boardroom: 1120 textures)

Candidates, all on our own D3D11 net (runtime/gpu/ai_up.c), compared per texture (contact sheets) and in game:

| variant | verdict |
|---|---|
| **x4plus alone** (RealESRGAN_x4plus, 23 RRDB blocks) | crisp edges, dither / 4-bit noise gone, text and logos legible (the best of the four); but faint linework fades (m2 pool grout) and flat material grain goes smooth |
| 6B -> x4plus chain (anime 6B cleans, boxed back to 1x, x4plus upscales) | orange / white outline artifacts around letters, small text garbled ("THEATRE" -> "THO ATI"): rejected |
| de-dither -> x4plus (range-limited 3x3 on dithered textures) | small text garbled, flatter still: rejected |
| x4plus + grain (the source's high-pass added back) | brings the dither dots back on logos and flat paint: off by default (PS2_AI_GRAIN) |

**Chosen: x4plus + restore + clarity.** Restore = a line-coherent back-projection: per source texel the residual
(source - the 4x4 box of the net's output) is put back (bilinear 4x) only where it continues along a direction on both
sides (grout, cracks, wood grain, strokes) and is faint (fades out from 10 to 28 / 255; the net keeps strong edges) -
never isolated dots, so dither stays gone. It brings the m2 grout and painted lines back. Clarity 0.8 = the luminance's
mid band (a 5x5 Gaussian of taps 2 output pixels apart, ~1 source texel) boosted, at most +-24 levels: materials and
edges read crisper and punchier without halos. Exclusions (keep the Enhanced image): soft-alpha textures (> 20 % of
texels between 8 and 247 with a spread > 10: glows, smoke; the net turns soft blobs into rings), small sprites
(<= 32 texels, transparent all along the border: snow flakes, bubbles - their crisp rim read as rings in ep6), sources
above 256 texels, HD-pack images. Tiling: REPEAT axes are padded by wrapping when the content continues across its edge,
else by the edge; padding 12 texels (vs 24: 0.9 / 255 mean difference in the border band, no visible seams).

**Honest verdict**: at scene scale the textures read as a sharp, clean HD remaster of the same art (glass streaks, paint
chips, plank edges and tile grout crisp; no dither, no shimmer) rather than a redraw. The "reimagined" weight has to
come from the lighting using the surface maps (relief, cavity) and the grading; texture-wise this is the boldest result
without real defects that the approved models give.

### Pipeline (gpu_tex_ai.h; Look = Reimagined only)

1. An Enhanced image is linked (et_link) -> its key (content hash + addressing + alpha-tested) names a cache file
   `<user dir>\textures\reimagined\x4plus-v<RA_VER>\<key>.jpg|png`. Present: decoded on the worker thread (WIC) and
   uploaded over the Enhanced image (at most 256 K texels per frame). Absent: the source texels are read back (staging,
   no stall) for the worker.
2. Worker thread (below normal priority): colour under transparent texels, exclusions, wrap / edge padding, tiles of
   128 + 12 padding as float planes; alpha kept apart.
3. The net: same-sized tiles batched (up to 152 x 152 padded pixels per batch), the 351 dispatches spread over frames:
   each frame issues layers in bands of 8 output rows worth `budget` GFLOP at the start of the frame (et_frame_begin).
   budget = what the frame leaves of 80 % of the frame interval (from the last pictures' GPU time), clamped to
   PS2_AI_MS_MIN .. PS2_AI_MS (0.5 .. 4 ms), minus this frame's Enhanced builds; ms per GFLOP measured with timestamps.
4. cs_ai_res + cs_ai_post: restore residuals, crop, clarity, alpha (Catmull-Rom 4x, cut-outs a steep ramp), into an
   upload region; the image is built over the Enhanced one as an HD image (mips, alpha coverage) and gets its surface
   map; read back and written by the worker as JPEG (quality 0.93, 4:4:4; opaque) or PNG (alpha), via .tmp + rename.
5. Until an image is ready the Enhanced (Faithful) image shows; a slot reused for another texture loses its AI image and
   reloads it from the cache when the texture comes back.

Weights: the converted file (`python -I scripts/ai_model.py export RealESRGAN_x4plus.pth x4plus.bin`, restricted
unpickler + SHA-256 check, 64 MB fp32) at PS2_AI_WEIGHTS, else `<exe dir>\models\x4plus.bin`, else `<user dir>\models\
x4plus.bin`. No Python at run time. The game only reads that file (SLYAIW01: names, shapes, float32); missing -> the
cache alone is used and Faithful's images show for the rest (logged). Shipping it is the owner's call (BSD-3 allows it
with the licence text in the third-party notices); it is never committed.

### Numbers (RTX 4060 Ti 8 GB, shared with other agents' game runs and at times VALORANT: GPU numbers are noisy)

TODO
