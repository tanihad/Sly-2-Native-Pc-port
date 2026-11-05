# Enhanced lighting (Modern renderer)

Owner request 2026-11-05: the Modern renderer's picture clearly better and more modern, "Original" one click away.
Branch next/light. Setting `graphics.lighting = enhanced | original` (Options > Graphics > Lighting, live). Owner
decision (same day): Enhanced is the default; presets Balanced and Quality = Enhanced, Performance and Original =
Original; an explicit preset in sly2.ini decides it, a custom file without the key gets Enhanced.

## What lighting data exists

| data | where | who has it |
|---|---|---|
| baked vertex colours (colour A for pass A, colour B for the additive pass B) | prelit meshes (static world, glob props): ~85% of the hub vertices | R2 native (r2_mesh.c); no normals |
| light block: up to 4 light slots (directions in object space, transposed: T0..T2 rows; colours x z x 2; thresholds) + ambient | per lit entry, computed on the EE by r2_lights_prep from the game's light set (VU 0x15-0x20) | R2 native: r2.hlsl r2_lit (per vertex, VU-exact) |
| normals (V4-8, object space; skinned: 4 bones) and a per-vertex colour / normal w | lit / skinned sub-batches (characters, some props): ~10% of the vertices | R2 native (the VS skins them) |
| depth | the modern frame's MS depth surface: GS Z / 2^20, affine in 1 / w (GS Z = bz + az z / w, clip z = a w + b) | every 3D draw that writes Z (R2 and exact FF triangles) |
| motion vectors | R8 (`PS2_MFT_MV=1`) | R2 entries only |
| shadows | the game's own: shadow volumes (VU program 188054619) into a G-channel counter, darken pass (shfuse / MFT fused pass) | exact stream, native GPU passes |
| what stays exact (no R2 data) | the sky, particles / effects, guard-band clipper fans, some characters' parts, the HUD | only their pixels and (if written) depth |

Units: world units are ~1 cm (Sly crouching ~190 units; the ep1 hub's near plane ~52 units, the far town 4000-8000).

## Features and what they need

1. **Ambient occlusion + bloom (screen space)** - need only the frame's depth and colour, so they work on everything on
   screen (prelit, lit, exact). Run once per game frame at the modern frame's resolve point (before the HUD), as one
   post pass on the picture: `runtime/gpu/gpu_light.h`, `shaders/light.hlsl`. Done first: the biggest visible win.
2. **Per-pixel lighting of lit meshes** - the light block per entry already reaches the GPU (fdata extra qwords), the
   normals are in the vertex stream: interpolate the (skinned) normal and evaluate r2_lit's formula per pixel, same light
   model. Separate R2 MFT pipelines (permutations) so Original stays bit-identical. Rim light from the view direction in
   object space (camera = null vector of the x / y / w columns of world x clip).
3. **Softer shadows** - the shadow pass darkens where the volume counter is odd; a soft edge needs a blurred mask before
   the darken (MFT fused pass). Shadow maps would need a depth pass of the casters from the dominant light: design only.
4. **Anisotropic filtering** - R2 samples the GS's own 4-bit bilinear from decoded level-0 textures (no mipmaps), so
   anisotropic filtering has nothing to work with until the sharper-textures work adds mip chains: deferred to it.

All of it runs only on modern frames (MFT: Modern renderer, percentage render scale). Reference, the integer grid, the
exact-path gates and every replay tool never run it; with Lighting = Original no pass runs at all (no cost).

## Order (as planned; findings did not change it)
AO + bloom -> per-pixel lighting (+ rim) -> shadows -> anisotropic.

## As built

See "Results" below (filled in as each step lands).
