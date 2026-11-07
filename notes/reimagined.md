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

TODO: results of the evaluation, pipeline, cache, budget, census, numbers.
