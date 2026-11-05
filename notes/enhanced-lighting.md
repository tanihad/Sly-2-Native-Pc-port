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

## As built (next/light, 2026-11-05)

- **Where it runs**: modern frames only (MFT). gpu_light.h hooks the MFT executor at the phase A -> B change (and the
  PRESENT of a frame without one): the colour target is resolved, then AO + bloom are composited onto the picture. A
  frame that goes back to phase A (ep1: a late native entry after the outlines) is not processed twice: the R2 frame
  block's serial names the game frame. Known gap: a "continued" MFT frame without R2 ops (~1 per 13 000 frames) gets no
  post pass.
- **Depth**: 1 / w = Za d + Zb from the frame block's screen model and the first entry whose clip z column is a multiple
  of its w column; view x / y from the x / y column lengths (off-centre share removed). Logged with PS2_LIGHT_LOG=1.
- **AO** (light.hlsl): half resolution; the 2 x 2 nearest view depth; normals from the flatter neighbour; 12 samples on
  a disc of 60 world units (<= 48 texels), Alchemy-style term with range falloff, strength 3, floor 0.35, fading out
  from w 4000 to 10000; 2 x 9-tap depth-aware blur; depth-aware upsample in the composite.
- **Bloom**: half-resolution bright pass (4 Karis-weighted 2 x 2 taps) of the squared excess over 0.8 of the brightest
  channel (soft knee 0.1): lamps, lit windows, the moon, splashes glow; a lit floor or snow does not haze (a first
  linear threshold at 0.55 washed the m2 boardroom's pool out: 46% of its pixels changed). 5-level dual-filter pyramid,
  intensity 1.2. Composite: one dual-source blend, picture x AO + bloom, RGB only (alpha = the GS byte, read by DATE).
- **Per-pixel lighting** (r2.hlsl R2_ENH; r2_render.c epipe / emcpipe / eppipe): the VS passes the skinned normal,
  its w, the record colour, the light block offset and the object-space camera vector; the PS evaluates r2_lit's formula
  with the renormalised interpolated normal. Rim light 0.5 x (1 - |N.V|)^3 in the key light's hue on colour A. Only
  draws of meshes with lit sub-batches use these pipelines (any_lit; for merged draws any member): +4 interpolants cost
  0.4 ms when every draw used them.
- **Soft shadows** (mft.hlsl SOFT, the fused shadow pass): see "Features" 3; 13 counter taps on a disc of 6 output
  pixels at 1440p (scaled with the resolution), only near the volumes' box and where a 4-tap cross disagrees.
- **Anisotropic filtering**: not done (no mipmaps to filter; belongs to the sharper-textures work).

## Results

Cost (in-run A/B, `PS2_GPU_AB=24 PS2_GPU_AB_R8=64`, ep1 hub, Balanced = 2560 x 1440 MSAA 4x, unlimited frame rate,
~12 000 frames per phase; GPU ms per picture; this machine's GPU was shared, the owner's is ~20% faster unloaded):

| | Original | Enhanced | delta |
|---|---|---|---|
| AO + bloom only | 4.30 ms (~233 fps) | 4.73 ms (~211 fps) | +0.42 ms |
| + per-pixel lighting (every draw) + soft shadows (first form) | 4.29 | 6.22 | +1.95 |
| + soft shadows edge-only | 4.27 | 5.53 | +1.26 |
| the same without soft shadows (AO, bloom, per-pixel on every draw) | 4.28 | 5.34 | +1.06 |
| **final** (per-pixel lighting on lit meshes only) | **4.24 ms (~236 fps)** | **4.87 ms (~205 fps)** | **+0.63** |

On the owner's unloaded PC (ep1 Balanced ~3.54 ms, ~276 fps) that is about 4.1-4.2 ms, ~240 fps: above 165.

Pictures (local only, never committed): build\light\shots\<scene>__final\ - Original and Enhanced of the same scene
from one run (alternating 30-frame phases).

### ep1 tfo "blinking seams" after the merge (2026-11-05): not a renderer change

`r2_sweep -Scenes ep1 -Checks tfo` failed on 941dd02 (Original 15 seams, Enhanced 19-28) and passed on 393ea64. Cause:
the scenario, not the lighting. ep1's dump window is VBlanks 4800-4979; the `-Play` script presses cross at pad frame
"start + 1500", and the start press is anchored to the title screen's function call, so that press lands anywhere in
VBlanks ~4880-4970+ depending on boot / load timing (measured: 4883, 4894, 4935 Enhanced; 4966 Original; 4941 393ea64).
The cross starts the job: the game's cinematic letterbox bars slide in (3-13 rows a picture at 1440p, 120 fps) while the
picture fades. Every seam reported was a bar edge (rows ~30-135 / ~1300-1434). flicker_check counted a straight edge as
a blinking seam unless a neighbour had it within 2 pixels, so a sliding bar counts once per picture. When the press
lands after ~4972 (the passing runs) the slide is outside the window. Proof: 393ea64 (build\pkg_old) with a 240-picture
window fails the same way (28 seams, same rows); Original HEAD fails (6) when its press lands at 4966. The Original
code path is unchanged (PS2_LIGHT=0: lt_enabled() false -> no light_note / light_post work, no SOFT permutations, no
R2_ENH pipelines, cb r2_misc.z 0) and the exact gates show it.
Fix (scripts/flicker_check.c): an edge that a neighbour has moved by 3-16 pixels (and the picture itself does not have
there) is an edge in motion; a neighbour's edge within 2 pixels counts also at lower contrast (> 10 instead of 24: a bar
edge fading out with the picture). A one-picture seam is still caught (synthetic check: a 4-row band in one picture ->
SEAM). The kept failing runs re-judged: Enhanced 28 -> 0, Original 6 -> 0.