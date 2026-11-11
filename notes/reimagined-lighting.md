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
## Outlines anti-aliased (2026-11-07, owner-approved; 6332e6f)

What the jaggies were: PS2 outlines (Medium) are the GS line's pixels drawn as native-pixel quads (one PS2 pixel = a
5 x 6.4 block at 1440p); thin outlines (High / Ultra) were analytic lines (ps_mft_thin) with square-cut segment ends
(gaps and notches where an outline's segments meet - the "dots") and lines without the GS AA1 flag (the outline's depth
pass) decided per pixel by a discard, so MSAA added nothing.

Changes (modern frames only; Retro and the exact / replay paths untouched):
- **thin lines** (mft.hlsl ps_mft_thin / vs_mft_thin): round caps (distance to the segment, not just across it: segments
  join), and per-sample coverage (SV_Coverage from the distance at each sample position) for lines without AA1 under
  MSAA; AA1 lines keep the GS's coverage alpha from the distance. Thickness unchanged.
- **PS2 outlines in Faithful / Reimagined**: drawn as those analytic lines at the PS2's own width (1 native pixel,
  GPU_GFX_SMOOTH from gfx.c when outlines = ps2 and the look isn't Retro) instead of native-pixel blocks: the PS2's
  look and thickness, smooth edges. Retro keeps the line pixels.
- Only vs_mft_thin, ps_mft_n0 and ps_mft_n1 changed (block compare); every other compiled shader is byte-identical.

Gates (6332e6f, each run alone): run_tests 16/16; 28/28 unit suites; m9_exact S1 + S3 -Game identical; gpu_diff_all
28/28 with the same numbers as before (the replays never draw modern-frame outlines: no re-baseline); r2_sweep ep1 /
ep3 / m2 tfo, tfb, mf Faithful 9/9 and Reimagined 9/9 (tfb now draws the smoothed PS2 outlines, tfo the new thin lines).

Cost (ep1 hub, 1440p High = thin outlines, MSAA 8x, unlimited frame rate, PS2_GPU_PROF): outline draws ("ff-line")
0.457 -> 0.540 ms (+0.08), GPU per frame 6.28 -> 6.37 ms (the "after" run may have shared the GPU for a while).

Pictures (local): build\reimagine\outlines\sly_<medium|high>_<faithful|reimagined>__before_after_1to1.png (ep3, Sly,
1:1). No moving-camera pair (the owner was playing the build; runs kept short). The tfo / tfb flicker checks cover
motion: 0 blinks, 0 seams.
## Stability (the owner's 0.1.1207 report, 2026-11-07)

Reproduced the way a player sees it: the owner's settings (1440p, 120 fps, thin outlines, render scale auto, filters
modern), Sly running in the ep1 hub with the camera orbiting (pad lup + rright), every picture dumped. The idle-scene
sweeps could not see any of it. Measurement (scripts/stability_check.c): 8 x 8 block luma, a "flicker event" = the
change reversing (up then down or down then up) with both steps over 16 / 255, per 1000 blocks x pictures, divided by
the mean block change (motion) - real motion of textured content also reverses, so only a comparison on THE SAME camera
path means anything: scripts/reim_ab_stability.ps1 alternates Faithful / Reimagined (PS2_GPU_AB_R8=1024) or 0.1.1207 /
fixed (PS2_GPU_AB_R8=2048, PS2_REIM_FIX=0) every 60 pictures of one run. Separate runs differed by up to 50 % in
motion alone and gave contradicting numbers.

| ep1 orbit, dark blocks, flicker per motion | Faithful | Reimagined | ratio |
|---|---|---|---|
| 0.1.1207 (PS2_REIM_FIX=0) | 0.641 | 1.015 | 1.58 x |
| fixed | 0.688 | 0.802 | 1.17 x |

Root causes found (each with its fix, all behind PS2_REIM_FIX / R8P_REIMFIX for before / after):
- the key light was eased towards each frame's winning light cluster: its direction wandered by ~0.5 deg as entries came
  and went (149 changes of the winner in 2000 frames), enough to move a shadow 3-4 map texels at 3000 units -> held,
  changing only after another light has won 90 frames in a row (2-3 changes per run instead of a continuous drift);
- the screen-space contact march (per-pixel noise) and the stronger occlusion (radius 80, strength 4) crawled under
  motion -> no contact march where the shadow map draws, Faithful's occlusion strength;
- shadow acne under motion: depth-rebuilt receiver positions jitter by a sub-texel -> a normal-offset bias along the
  world normal, surfaces nearly edge-on to the light faded out, the map faded from half its reach;
- residual crawl of the shadow edges -> a temporal resolve of the key-light term (half res): last frame's value at the
  same world position (reprojected with the previous world x clip), clamped to this frame's 3 x 3 neighbourhood,
  80 % history;
- the statistics (exposure, fog colour, shaft source) adapted per frame: at 120 fps twice as fast -> per second (0.7 s);
- also checked and ruled out: the map's texel snapping (radius constant, centre in whole texels), frames without a map
  (0 of 18 000), segments after the post pass (0), the world-matrix choice (stable).

**Textures** (PS2_TEX_SWAPLOG=1: per-texture changes after a texture first shows enhanced): warm and cold caches,
ep1 / ep2 hubs, 0.1.1207 and fixed: every texture changes exactly once (Enhanced -> AI), no texture swapped back and
forth; the content changes the game makes itself are counted apart. The one path that could flip a texture between AI,
Enhanced and the original sampling - a second content change within 30 s rejected the texture for good (characters'
parts streamed into a shared area) - now shares any content seen before at once and only treats four NEW contents in
30 s as an animation. No crossfade yet (the AI image is built over the Enhanced one in place; a fade needs both).

**Build hitches**: builds were capped at 1 M output texels a frame (~3 ms under Reimagined with the surface maps) and
the AI images' applies (cached loads and finished net results) were not capped at all. Now both share one GPU-time cap:
what the frame leaves of 80 % of the frame interval at the player's frame rate (0.25..2 ms), converted with the measured
ms per million texels (timestamps). Cold cache, ep2 hub, 120 fps: no frame with builds over 10 ms in play either way;
frames with builds +0.87 -> +0.75 ms over the others; the owner's 10-22 ms frames are hub / title loads. (Logging with
PS2_REIM_LOG made a 100-150 ms CPU stall show up as GPU time in "tex-build" lines: not a hitch in play.)

Gate: scripts/reim_stability_gate.ps1 [-Cold] (one run each: Reimagined's flicker per motion <= 1.3 x Faithful's on the
same path, no texture changed more than once).

Videos (build\reimagine\videos, local; scripts/frames_to_mp4.c: Media Foundation H.264, 40 Mbps, 60 fps from the
120 fps dumps): ep1_orbit_faithful, ep1_orbit_reimagined_BEFORE (PS2_REIM_FIX=0), ep1_orbit_reimagined_after,
ep1_orbit_SIDE_BY_SIDE_faithful_left_reimagined_after_right, ep2_coldcache_reimagined_after; each with its run.log.
Follow-up (same day): clarity 0.22 -> 0.08 (local contrast amplified small flicker; with it off the ratio fell 1.42 ->
1.15 in one run), N.L from a 2-texel baseline, history 0.9. Ablation on the same path: with the key-light terms off
(PS2_REIM_SS=0) Reimagined / Faithful = 1.08 x; shafts off 1.18 x; grade off 1.15 x - the relight / shadow terms are
still the largest share. The gate's ratio varies run to run by +-0.15 (the camera path differs between runs; some runs
lose the pad timing and barely move): final runs 1.26 x (pass) and 1.44 x (fail) warm, 1.43 x cold. Status: much
better than 0.1.1207 (1.58 x, and the per-frame key drift and contact noise gone), not yet at Faithful parity.
Also: one r2_sweep ep3 mf run reported 2 stray triangles (an exact-path triangle with q = 0 at v5096, not R2 / post);
the same check passed when run again alone.

## Owner playtest of 0.1.1214 (reimagine2, 2026-11-08)

Four reports, in his order. Everything new is behind `PS2_REIM2=0` (= 0.1.1214's Reimagined, used for every "before").

**1. "Fires cast beams of light."** The shafts smeared the bright texels whose depth was far towards their centroid: fire
and torch sprites write no depth, so over the sky or distant background they counted as "far" and became sources (ep2
hub: rays from the torch next to the camera, a beam from a far torch). Now the shafts come only from the key light:
its direction projected into the picture (a point at infinity; none when it is behind the camera, faded out up to 0.8
screens off the picture), the strength from bright sky texels near it, and only texels that are far AND neutral in hue
(`r_skyw`: saturation < 0.25..0.45, no warm cast) are smeared - fire, lamps, lasers and glows never are. Fires instead
light their surroundings: the warm part of the bloom (the flames' own glow, so it flickers with their animation)
multiplies the picture's colour around them (`PS2_REIM_FIRE`, 0.5). Lasers are saturated red: unchanged.

**2. "Shadows under trees flicker as if confused."** Reproduced on a deterministic ep2 path (below; Sly walks past a torch
into the trees' shadows) with the key-light term alone (`PS2_REIM_DBG=1`): over 1-3 frames whole shadow regions jumped
to other shapes. Logged per frame (`PS2_REIM_LOG=3`): key light and shadow-map radius steady, but the **world matrix**
changed at that frame (the camera rebuilt from it jumped (-3625, 3109, 449) -> (1358, -1734, 240) while the real
camera moved smoothly). Every receiver is rebuilt from the depth through that matrix, so the shadows of the whole picture
were looked up in the wrong place. 0.1.1214 took the most common matrix of whichever segment had the most entries
sharing one; under trees few world entries are drawn and a group of plants (or Sly's own entries) won. Fix
(`reim_finish`, once the frame is complete): only unlit entries (characters and lit props are never the world), counted
over the whole frame, and of those the one whose camera continues last frame's (< 600 units away); the most common only
on the first frame or a camera cut; the lights are clustered after the choice. Two more causes, both fixed:
- the casters are what the game draws for its own camera: a canopy above the picture or a wall behind the camera cast
  nothing, so shadows came and went as the camera turned (the segment count went 33 -> 38 -> 33 at such moments).
  A **static shadow cache**: the static world's casters (entry matrix = the world's) accumulate in a light-space map
  (4096^2, half size 1.5 x the per-frame sphere: 4 units a texel at R 5120 instead of 5) that keeps them; it is
  centred on a coarse grid and when the camera's sphere nears its edge it moves by whole grid steps, its content copied
  along (`ps_reim_sccopy`); a new key light or radius starts it again. Static casters are drawn into it every 4th frame,
  the rest (characters, props) into the per-frame map every frame.
- cut-outs (leaves, fronds, canopies) were solid cards in the map (no alpha test: an octagon for a canopy): draws whose
  alpha test splits their fragments now go through `r2_vs/ps_shadow_at` (the material's texture and alpha test).

**3. "Sly looks blurry."** Measured on 1:1 crops of Sly (scripts/sharp_check.c: mean gradient, |Laplacian|, acutance =
the top 5 % gradients). The AI textures already made him sharper than Faithful; what read as soft was the shading and the
outlines' edges. A contrast-adaptive sharpening pass (AMD CAS, cross form) on the Reimagined picture before the grade
(`PS2_REIM_CAS`, 0.6), weaker where the contrast is already high (no halos). anime_6B for him measured no better than
x4plus (still, Sly region 440 x 450: grad 5.19 vs 5.77, Laplacian 13.3 vs 15.2): x4plus stays. The game's own motion blur
(Display: motion blur, on in the preset) smears him in every look while the camera turns fast.

**4. Performance on High** (1440p, render scale 100 %, MSAA 8x, thin outlines, 120 fps; PS2_GPU_PROF with the new
categories reim-smap / reim-shadow / reim-bloom / reim-comp; in-run A/B with PS2_GPU_AB_R8=1024, AI textures in both).
The owner's "GPU ~9.3 ms" is the 0.1.1207 log's "tex-build ... others 9.27 ms" (before the stability fixes).

**Determinism (the stability gate, all evidence runs)**: the pad script ran in VBlanks from START, but the hub's start
moved by 400+ VBlanks between runs (several X presses at the safehouse), and frame dumps that stall the game made the
host timer add VBlanks between game frames, so Sly's path drifted (one "tree walk" ended in a van). Now: one X 2000
VBlanks after START (light_shots -PadAll), the dump window is a pad token (`dump:<n>`, `PS2_DUMP_FROM=script`),
`PS2_FRAME_DT_FIXED=1` steps the game clock 1/rate per game frame (patch frame_dt_min) and the host timer raises no VBlank
between game frames in that mode: two runs of the same path differ by 0.25 / 255 per 8 x 8 block (pic_diff.c), their
stability numbers by < 0.5 %. Also new for scripts: `@addr#k+n` anchors (the k-th call), `PS2_HOOK_LOG=1` (hooked
functions' call sites), `PS2_GPU_PROF_FRAME=1` / `PS2_GPU_PROF_FROM=<vblank>`, frames_to_mp4 `--title` / `--label`.

Gate (scripts/reim_stability_gate.ps1 [-Cold] [-Extra env]): shots orbit (ep1) and trees (ep2), Faithful / Reimagined
alternating every 6 pictures in one run, triples of consecutive pictures, the HUD's top 12 % left out; passes when
Reimagined's dark-block flicker per motion <= 1.15 x Faithful's in both shots and no texture changed more than once.

Switches: PS2_REIM2=0, PS2_REIM_SHAFT_KEY=0, PS2_REIM_FIRE, PS2_REIM_CAS, PS2_REIM_SH_AT=0, PS2_REIM_SC=0,
PS2_REIM_SC_SIZE, PS2_REIM_SC_EVERY; PS2_REIM_X=<mask> with PS2_GPU_AB_R8=4096 turns these off in A phases (1 cut-out
casters, 2 CAS, 4 key-light shafts, 8 fire light, 16 static cache).

### Results (next/reimagine2; RTX 4060 Ti, High = 1440p, 100 %, MSAA 8x, thin outlines, 120 fps)

| | 0.1.1214 | now | Faithful |
|---|---|---|---|
| ep2 torches: shafts | rays from the near torch, a beam from a far one | none (halo + warm light on the torch and ground) | - |
| ep2 trees path: frames where 0.1.1214's world-matrix choice was wrong (counted by the new code) | 119 (of one run) | 0 (camera continuous) | - |
| key-light term alone (DBG=1) under the trees: flicker per 1000 / dark / motion | 13.96 / 14.72 / 4.58 | 9.25 / 10.35 / 2.94 | - |
| Sly, close-up orbit, 440 x 480 around him: gradient / Laplacian / acutance | 3.21 / 5.09 / 27.4 | 4.33 / 11.14 / 34.9 | 2.04 / 3.28 / 19.8 |
| Sly at rest, 440 x 450: gradient / Laplacian / acutance | - | 5.77 / 15.2 / 39.2 (anime_6B: 5.19 / 13.3 / 39.0) | 2.76 / 4.65 / 23.0 |
| ep1 orbit, GPU ms per frame over the clip (deterministic path, per-frame timestamps): mean / p95 / max | 5.97 / 6.83 / 7.26 | 6.04 / 6.94 / 7.52 | - |
| in-run A/B (ep1 orbit, PS2_GPU_PROF): Faithful lighting / Reimagined | 5.44 / 6.29 | 5.58 / 6.79 (static cache every 2nd frame; now every 4th) | - |
| in-run A/B (ep2 path) | 4.36 / 4.98 | 4.36 / 5.11 | - |

Reimagined on High holds 120 fps (8.33 ms) with 17-28 % headroom on these paths (the worst single frame 7.52 ms, 10 %).
Per pass (ep1, after): reim-smap 0.33 (cut-out casters + cache at every 2nd frame), reim-shadow 0.54, reim-bloom 0.19,
reim-comp 0.33 ms; the R2 pass +0.39 ms over Faithful lighting (the Reimagined pixel shaders with the surface maps).

Evidence (local, never committed): build\reimagine\videos2 - ep2_fire_and_trees BEFORE / AFTER / SIDE_BY_SIDE,
sly_closeup_orbit FAITHFUL / BEFORE / AFTER / two side by sides, sly_1to1_<frame>_faithful_before_after.png,
sly_still_1to1_faithful_anime6B_x4plus.png, ep1_orbit_High_frametime BEFORE / AFTER (each picture's GPU time written in)
/ SIDE_BY_SIDE.

Stability gate (final code): warm 3 of 3 PASS - orbit 1.088 / 1.079 / 1.086 x, trees 0.977 / 1.027 / 1.031 x Faithful;
cold PASS (orbit 1.118 x, trees 1.029 x); no world-matrix flip, every texture changed once. The same gate on 0.1.1214
(PS2_REIM2=0) also passes the ratio (orbit 1.033 x, trees 0.978 x): the reversal count does not see a few large shadow
jumps, hence the flip check (0.1.1214 cannot log it; on its trees path the new code counts 119 wrong frames). Two runs
of the same path differ by < 0.5 %. Standard gates on the final code: run_tests, 28 unit suites (all exit 0), m9_exact S1 + S3 -Game
identical, gpu_diff_all 28/28, r2_sweep Faithful 9/9 and Reimagined 9/9 (ep1, ep3, m2_boardroom x tfo, tfb, mf); after them only Reimagined-side logging changed (the world-camera cut / flip lines the gate reads).
## Local lights (2026-11-11, branch next/local-lights)

Owner's menu item 3 ("local lights from lamps / windows / fires"): under Look = Reimagined the game's own point lights
(lamps, lit windows, torches, fires, neon, and the designers' fill lights) light the ground, walls, Sly and the other
characters around them; before, only the key light (the moon) was re-lit and the night levels looked moonlit only.

**Where the light data comes from: the game's own light table** (RE with scripts/r2_dis.py, func 0x1A4478 builds each
object's light set from the best four of 0x1A3F78's search; 0x1A3D00 registers a light):
- the static world SW = 0x3DD490; SW + 0x4154: LIGHT* table[256] (a slot's LIGHT + 0x118 = its index); SW + 0x4590 the
  free-slot bitmask (8 words, set = free); SW + 0x4570 / 0x4580 the always-considered lights (256-bit mask); SW + 0x455C
  a BSP of light influence (node: plane +0, children +0x10 / +0x14, leaf +0x1C -> a 32-byte mask at SW + 0x4564 + 32 leaf).
- LIGHT: +0xE0 kind (1 point, 0 directional), +0xF0 colour as hue / saturation / value (0..255 scale), +0x100 world
  position (w = 1), +0x110 / +0x114 the search score a + b x distance (0 at the outer radius), +0x120 the VU record (3 qw,
  copied into an object's light set at VU 0x15 + 3 i): {type, ambient share y, diffuse share z (< 0: the VU's direction
  convention), falloff offset}, {colour rgb (max channel 1), w}, {position, falloff slope}: falloff
  clamp(w0 + slope d, 0, 1) = 1 inside +0x164 (inner radius), 0 at +0x168 (outer radius); +0x150 two shading params,
  +0x15C a flag. The VU (0x1050) lights a vertex with colour x 2 (z min(d + d^3, -y / z) + y) x falloff, d = N . (vertex -
  light) / |..|: a wrapped diffuse.
- Census: ep2 hub 168 lights (~70 warm torches / fires (1, 0.61, 0.22), (1, 0.74, 0.37), ~80 cool blue moonlight fills
  of radius 400-2500), ep1 hub 241 (mostly warm street lamps / windows (1, 0.9, 0.41), (1, 0.77, 0.45), radius 130-1400,
  some cyan: the fountain), m2 boardroom 58 (one directional). All kind 1 except that one; the values are constant (the
  game animates none of them).

**Path** (all Reimagined-only; Faithful / Retro never see it): r2_capture.c lights_capture copies the point lights
(r2.h r2_light, 64 bytes) into the frame's scene slot at the frame's start (only when the look is Reimagined, or
PS2_REIMAGINE=1; PS2_REIM_LIGHTS=0: never); gpu_r2front.c writes them into the frame data once per data arena
(gf_data_epoch) and points gpu_r2_frame's unused `pad` at them; gpu_reim.h reim_note copies the table on the render
thread (RLIN; the frame data does not outlive the frame).

**Per frame** (gpu_reim.h reim_lights_build): lights whose sphere misses the picture (the picture's own ndc range: the
clip volume is the VU's guard band, ~8 x the picture) are not sent but keep their weight, so turning towards a lamp
never fades it in; the rest are ranked (strength x kind x radius / distance, 25 % hysteresis) and the Quality's count
kept (Low none, Medium 32, High 64, Ultra 96); each light's weight eases in / out over 0.25 s (a light the game adds,
one entering / leaving the count): nothing pops. Kind: warm sources fully, cool fills x 0.3, wide fills (outer radius >
1200..2500) down to x 0.4. Fires (orange, small) flicker gently: x (1 + 0.06 (three slow sines, 1.7 / 4.3 / 9.1 Hz,
phases from the light's id)), deterministic (the clock steps 1 / frame rate per post pass).

**Shading** (light.hlsl ps_reim_local, half res, after the depth pass): world position from the depth (r_world), the
normal from the depth 2 texels each way (the flatter neighbour), per light the game's own wrapped diffuse x a pool
falloff: (1 - x^2)^2 / (1 + x^2 / core^2), x = distance / outer radius, core 0.35 (half the light at a third of the
radius: it reads as coming from the lamp, a soft tail to the radius). Composite (ps_reim_comp): depth-aware upsample
with the occlusion, then lin += (lin + 0.025) x E (the picture's colour as the surface's, a floor so dark paint still
shows the pool), before the fire glow, the bloom and the grade. No shadows from local lights (light passes through
walls within a light's radius: the radii are small).

Switches: PS2_REIM_LIGHTS=0 (kill switch: EE copy and pass), _N (count), _Q (quality 0..3), _I (gain, 3.0), _FLOOR
(0.025), _COOL (0.3), _FLICK (0.06), _CORE (0.35), _POOL (1: pool falloff; 0: the game's linear ramp), _D0 / _D1 (fade
by distance, 6000 / 12000), PS2_REIM_LIGHTS_DUMP=1|2 (the table every 600 VBlanks), PS2_REIM_DBG=5 (the light buffer,
each light's place as a magenta dot), PS2_REIM_LOG=2 (each drawn light's picture position). In-run A/B: PS2_REIM_X=32
with PS2_GPU_AB_R8=4096 (A no local lights, B local lights). GPU profile category "reim-lights".
