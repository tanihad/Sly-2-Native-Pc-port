# Reimagined lighting (Look = Reimagined)

Owner request 2026-11-06: Enhanced lighting (Faithful) was "not much of a difference"; Look = Reimagined should be a bold,
visibly modern reimagining of Sly 2's lighting ("I don't mind if it changes a lot ... go above and beyond"). Branch
next/reimagine. Faithful and Retro are unchanged: every Reimagined pass, shader and pipeline is reached only while
`lt_reim()` (graphics.look = reimagined; `PS2_REIMAGINE=0|1` wins); Faithful's 270 compiled shaders are byte-identical to
main's (block compare of build\gen\gpu_shaders.h), Reimagined has its own permutations.

Textures (AI upscale, normal / height maps) are a separate work item: notes/reimagined.md (branch next/ai).

## What it does (modern frames only, like Enhanced lighting)

| feature | how | where |
|---|---|---|
| **key light** | the game's own light rig: every lit entry's light block (up to 4 directional lights in object space) goes to the world through the entry's world x clip and the inverse of the frame's world x clip (the matrix most entries share = the static world's, object space = world); lights are clustered by world direction (a directional light repeats over the entries), the brightest cluster from above (z > 0.25) is the key light, smoothed from frame to frame (a cut: at once). Without one: a high light from (0.35, 0.25, 0.9). ep1 hub: (0.30, 0.49, 0.82), colour (0.10, 0.26, 0.48) - the moon. World is z-up. | gpu_reim.h reim_note / reim_key |
| **real shadows** | a 2048^2 shadow map along the key light: an orthographic view around the bounding sphere of the camera's frustum slice up to 3500 units, its centre snapped to texels (no crawling edges); the frame's R2 segments drawn again as casters (pass A, Z written: solid meshes incl. skinned characters; sky and effects excluded) through r2.hlsl r2_vs_shadow (clip -> world -> light, plain floats). Receivers: the post pass rebuilds each pixel's world position from the depth, 4 x 4 PCF from four gathers, slope-scaled offset towards the light. Plus a 10-step screen-space contact march for small detail. | gpu_reim.h reim_smap; light.hlsl ps_reim_shadow |
| **relight** | surfaces facing the key light (N from the depth) brighten in its hue where it reaches them (x (1 + 0.45 N.L)), darken to 0.55 x a cool tint where something casts a shadow; surfaces turned away keep the game's own lighting. This is what makes the shadows read: darkening alone was invisible at night. | light.hlsl ps_reim_comp |
| **AO** | the Enhanced pass with a wider radius (80) and strength 4, floor 0.22 | ps_light_ao |
| **bloom + halos** | lower soft threshold (0.55, knee 0.2), the scene's bright parts through a 6-level dual-filter pyramid, intensity 3, wider levels x 0.85 per level; added as light (linear) before the grade | ps_reim_bright / _up |
| **light shafts** | the bright far texels (the moon, the sky glow) smeared towards their centroid (24 taps, quarter res); the source and its strength come from a smoothed 4 x 1 statistics texture | ps_reim_stats / _shafts |
| **atmosphere** | depth fog from 2500 units + height fog (300-unit scale height below the camera) towards the scene's own sky colour (the top third of the picture, far texels weighted), at most 30 %; the sky itself is left alone | ps_reim_comp |
| **grade** | per scene mood (night = how dark the log-average picture is): split toning (night: cool darks, amber highlights; day: warm highlights, cool-teal darks), warm lamps boosted against the night, a blue floor in the deep darks (no mud), saturation 1.15 + vibrance 0.25, clarity (local contrast vs the 1/16 mip, power 0.22), a filmic contrast curve (1.15 around 0.42) with a soft shoulder for the glow above white, vignette 0.28 | ps_reim_comp |
| **characters** | R2_ENH=2 pipelines for lit meshes: a Blinn-Phong highlight (exponent 28, 0.45) from the strongest light and a rim light 1.1 in its hue - with \|colour\|: the light blocks' colours are often negative (the light then comes from -direction), which leaves Faithful's rim (max(colour, 0)) at 0 in ep1 | r2.hlsl r2_lit_px, r2_render.c |

**Normal-map hook**: r2.hlsl `r2_reim_normal(N, V)` (R2_ENH=2) is the one place a per-pixel normal enters the lit
meshes' lighting; it returns the interpolated normal by default. Define R2_NMAP and provide it (tangent frame from the
screen-space derivatives of position and UV) to plug the Reimagined normal maps in. The post pass's relight uses the
depth's normals (a G-buffer normal from the R2 pass would be the next step).

## Switches (tests, not in the menu)

PS2_REIMAGINE=0|1, PS2_GPU_AB=<n> PS2_GPU_AB_R8=1024 (A Faithful / B Reimagined in one run), PS2_REIM_LOG=1|2,
PS2_REIM_DBG=1 (key-light visibility) / 2 (shafts) / 3 (world position bands) / 4 (fog colour) / 9 (each in turn),
feature switches PS2_REIM_SS=0 (all key-light shadows), _SMAP=0, _SHAFTS=0, _FOG=0, _GRADE=0, tuning _SS_K, _SS_LEN
(0: no contact march), _KEY_I, _SM_SIZE, _SM_K, _SM_DIST, _BLOOM_T/_K/_I, _HALO, _SAT, _CONTRAST, _VIBRANCE, _EXPOSURE,
_CLARITY, _FOG0, _FOGD, _FOGMAX, _HFOG, _HFOG_H, _VIG, _SHAFT_I, _NIGHT (0..1, default from the picture), _STAT_A, _RIM,
_SPEC, _AO_R/_K/_MIN. GPU profile: PS2_GPU_PROF=1 shows "light" (the post pass) and "reim-smap" (the casters).

## Menu

Look = Reimagined works from sly2.ini (`[graphics] look = reimagined`); the menu offers it only when the ini says so
(menu.c MENU_ROW_GLOOK) until the owner approves it - then add it unconditionally there.
