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

## Results (b3663a4)

**Cost** (in-run A/B `PS2_GPU_AB=24 PS2_GPU_AB_R8=1024`, ep1 hub, 2560 x 1440 Medium (MSAA 4x), unlimited frame rate,
6000 pictures per phase; the GPU was shared with other agents' game runs the whole time, so the absolute numbers are
~1.8x an unloaded machine's):

| ep1 hub | Faithful | Reimagined | delta |
|---|---|---|---|
| GPU per picture, average | 7.81 ms | 9.17 ms | +1.28 |
| GPU + present p99 | 16.10 ms | 17.60 ms | |
| worst | 19.30 ms | 22.26 ms | |

GPU profile (PS2_GPU_PROF=1, Reimagined, same shared GPU): post pass ("light") ~1.7 ms, shadow casters ("reim-smap")
~0.65 ms before the lean caster VS. Unloaded estimate: Faithful ~4.3 ms here (notes/enhanced-lighting.md) -> Reimagined
~5.0-5.5 ms (~180-200 fps), within the 6 ms / 165 fps budget; to be confirmed on an idle GPU.

**Gates**: run_tests 16/16; every tests\unit suite passes; m9_exact S1 + S3 -Game identical and gpu_diff_all 28/28 (on
25d1062; later commits touch only Reimagined-only shader code and constants); r2_sweep -Parallel 1 ep1 / ep3 /
m2_boardroom tfo, tfb, mf: Faithful 9/9 and Reimagined (PS2_REIMAGINE=1) 9/9 - no flicker, 0 pops, 0 stray triangles.

**Pictures** (local only): build\reimagine\shots\<scene>__retro_faithful_reimagined.png (Retro from its own run;
Faithful / Reimagined from one A/B run, 30 VBlanks apart) for ep1 hub night, ep3 hub (night, rain), ep6 hub dusk, m2
boardroom; closeup_sly_ep3__faithful_reimagined.png. (ep1 / ep6 / m2 were taken before the last softening of the
shadows: 0.45 -> 0.32.)

**Judgement**: a player sees at once that it is a different look - ep1's moon throws real shadows (Sly's own on the ledge,
the fountain's), moonlit floors and roofs, a glowing moon with a halo, warmer lamps, a cooler graded night; Sly gets a
rim and highlights. It stays pleasant at night; weaker spots: indoor / pool scenes (m2) come out lighter and less
saturated than Faithful (the relight brightens the floor), and the key light is the game's character light, which in some
levels is a weak overhead light (the effect is then subtler). Day scenes were not checked (none of the four).

## Next ideas
- Plug the next/ai normal maps into r2_reim_normal (and a normal-mapped relight of the prelit world: the R2 pass would
  write a normal G-buffer); per-level tuning of the key-light strength; temporal accumulation of the shadow edges.
## Tuning pass + surface maps (2026-11-07, coordinator review)

Review: ep1 good; ep6 / ep3 drained the warm colours, went teal and darker than Faithful. Changes:
- **Grade keeps the game's colours**: relight in (nearly) white light - it was tinted by the moon's blue, and the relight
  boost had been lost in an edit, so the "relight" only darkened; mild split tone (no night teal / blue floor), fog only
  half the sky's hue, contrast around the scene's own mid-tone (log-average) instead of a fixed 0.42 pivot, saturation 1.06
  + vibrance 0.35 with skin-like warm tones and the sky protected, warm lights x 1.3 boost kept, vignette 0.16.
- **Brightness**: an exposure that gives back what occlusion, shadows and relight take on average (statistics texel 3,
  a 32 x 18 sample of AO and the key-light terms, smoothed; gain 1..1.35): Reimagined >= Faithful's mean luminance.
- **Rim / highlight on characters only** (skinned sub-batches): world lit meshes seen at grazing angles got a bluish rim.
- **next/ai merged** (5c0afec) and its surface maps used: prelit R2 meshes under Reimagined draw through R2_REIM
  permutations in tp[0]'s place: relief lit from the key light's side (the map normal's slope projected on screen
  against the key light's screen direction, +-45 %) and cavity deepening creases (60 %); lit meshes (characters) are
  normal-mapped in r2_reim_normal (cotangent frame from the screen derivatives of the object-space position and uv:
  diffuse, highlight, rim). Both fade out where a map texel gets smaller than ~0.5 pixel (no shimmer / speckle at a
  distance). Switches PS2_REIM_RELIEF / _CAVITY / _NMAP. Faithful's 270 shaders stay byte-identical (block compare).

**Outline jaggies (finding, not changed)**: the pictures use Medium, i.e. PS2 outlines: the game's line pixels drawn as
native-pixel quads (gpu_exec_mft.h mft_draw_setup, ps_mft_l*) - at 1440p one PS2 pixel is a 5 x 6.4 block, hence the
stair-steps and the dots of the GS's AA1 coverage pattern. With thin outlines (High / Ultra, tfo) ps_mft_thin computes a
coverage from the distance to the line only when the GS AA1 flag is set and otherwise discards per pixel; either way the
coverage decision is per pixel in the shader (discard), so the MSAA surfaces they are drawn into (phase A, before the
resolve) add no anti-aliasing. A fix would output SV_Coverage from the analytic distance (as R2_A2C does) in
ps_mft_thin, and offer thin outlines under Reimagined; it changes Faithful's thin-outline output (High / Ultra), so it
was left for an owner decision.

**Cost, idle GPU** (RTX 4060 Ti, nothing else on the GPU; ep1 hub, 2560 x 1440 Medium, unlimited frame rate, 6000+
pictures per phase, AI texture cache warm; Faithful from a look = faithful run, Reimagined = the B phases of a
look = reimagined run with PS2_GPU_AB_R8=1024; 255cb7e):

| ep1 hub | Faithful | Reimagined (AI textures + maps + lighting) |
|---|---|---|
| GPU + present, average | 4.77 ms (~210 fps) | 5.85 ms (~171 fps) |
| p99 | 8.44 ms | 9.32 ms |
| worst | 9.0-11.1 ms | 10.2 ms |

**Gates** (255cb7e; 8ffdafb changes only Reimagined shader code): run_tests 16/16; all 28 unit suites; m9_exact S1+S3
-Game identical; gpu_diff_all 28/28; r2_sweep ep1 / ep3 / m2 tfo, tfb, mf Faithful 9/9 and Reimagined (lighting + AI
textures + maps) 9/9 - each run alone.

Pictures: build\reimagine\shots3 (local only).