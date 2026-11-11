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
unpickler + SHA-256 check, 64 MB fp32) at PS2_AI_WEIGHTS, else `<exe dir>\models\x4plus.bin`, else `<exe dir>\
x4plus.bin` (where a 1.0.x launcher's update puts it), else `<user dir>\models\x4plus.bin`. No Python at run time. The
game only reads that file (SLYAIW01: names, shapes, float32); missing -> the cache alone is used and Faithful's images
show for the rest (logged). Shipped (owner approved, 2026-11-10): Setup and every update bundle carry it, the launcher
installs it as `game\models\x4plus.bin` (notes/install.md "AI model", THIRD-PARTY-NOTICES.txt); it is never committed.

### Numbers (RTX 4060 Ti 8 GB, shared with other agents' game runs and at times VALORANT: GPU numbers are noisy)

**Build cost** (x4plus: 35.8 MFLOP per padded input pixel; our kernel ran at 6-9 TFLOPS effective, 0.11-0.15 ms per
GFLOP, when the GPU was not shared; up to 1 ms per GFLOP while other game runs used it):

| | images | TFLOP | GPU time | play time |
|---|---|---|---|---|
| ep6 hub (PS2_AI_MS=12) | 621 | 280 | 48 s | ~5 min run |
| ep1 hub, default budget, 60 fps | 560 of ~630 in one 270 s run | 248 | 50 s (contended) | ~3 min in the hub |
| census (below) | 2746 | 1358 | ~3.2 min at 7 TFLOPS | |

**Census** (PS2_TEX_DUMP=1 while walking: 8 hubs + m2 boardroom, m3 boss, m5 ghost, m6 bear cubs, m7 boat hack, m8 engine
room; scratchpad census.ps1): **2746 unique art textures** (128x128 706, 64x64 657, 32x32 463, 64x32 213, 128x64 173,
256x128 106, ...; none above 256), 24.4 M source texels, 37.9 M padded pixels. The walks see part of each hub and 6 of
~45 jobs, so the whole game is estimated at **~5000-6000 textures, ~2700 TFLOP: ~6-7 min of GPU time on an RTX 4060 Ti**
(~2x on a GTX 1060-class card, ~2 min on an RTX 4090); in the background at the default budget (<= 4 ms of a 16.7 ms
frame) that is spread over ~25-30 min of play, ~3 min per new hub; levels seen before load from the cache.

**Cache size**: 2104 files = 69 MB (34 KB average: JPEG 4:4:4 q 0.93, PNG for the 7 % with alpha) -> ~180 MB for the
whole game. Under `<user dir>\textures\reimagined\x4plus-v3\`; never in the repo, never shipped.

**Video memory** (on top of Faithful's ~790 MB of Enhanced classes, which Reimagined reuses: the AI image replaces the
Enhanced one in the same slice): the net 294 MB (weights 64 + feature buffers for one 152 x 152 batch), surface maps
198 MB at the pre-sized classes, upload regions + residuals 17 MB: **~510 MB**. The net is only created when DXGI's
budget minus the process's use exceeds its size + 512 MB, and released when the use passes 97 % of the budget (built
images stay; the surface maps go above 99 %). System memory: 36 MB of read-back buffers.

**Frame times** (ep1 hub, 2560 x 1440, Medium, 60 fps, 9000 pictures after the first 90 s, PS2_GPU_FT=1; the GPU was
shared with the lighting agent's r2_sweep game runs the whole time, so the averages say more about that load than about
us - Faithful's own average here is 2x a quiet run's):

| ep1 hub | GPU + present avg / p99 / max | render thread avg / p99 / max | picture interval avg / p99 / max |
|---|---|---|---|
| Faithful | 12.73 / 20.76 / 26.05 ms | 1.40 / 5.80 / 7.23 | 16.69 / 23.72 / 45.98 |
| Reimagined, building (560 images during the run) | 8.66 / 15.74 / 24.74 | 1.53 / 5.84 / 16.43 | 16.68 / 21.26 / 31.73 |
| Reimagined, all from the cache | 9.35 / 14.20 / 20.88 | 1.64 / 6.48 / 29.62 * | 16.68 / 21.76 / 44.81 |

The net's own GPU time per frame while building: p50 3.75, p99 6.0, worst 14.1 ms (timestamps; the worst ones while
another process held the GPU). No added dropped pictures (interval p99 unchanged). * one render-thread peak while 631
cached images were uploaded at the hub's load with 1 M texels per frame: now 256 K per frame (0391e82). A first version
created the net's 14 kernels on the render thread at first use: 70 ms peaks; they are created on the worker now.
A clean measurement needs a quiet GPU (no other game running).

### Pictures (local only, never committed): build\ai\shots2 (worktree next/ai)

Per scene (ep1_hub, ep3_hub, ep6_hub, m2_boardroom; ep6_hub_ground = the planks): `<scene>_original / _enhanced (=
Faithful) / _ai (= Reimagined).png` (2560 x 1440), `<scene>_compare3.png` (middle 852 columns: Original | Faithful |
Reimagined) and `<scene>_closeup3.png` (640 x 720 at 2x). Original + Faithful come from one A/B run, Reimagined from
another (cache filled by a build run first: scratchpad ri_runs.ps1), so Sly's idle pose differs between columns.
`surface_maps_preview.png`: per texture AI image | lit by its surface map (light from the upper left) | normal map |
height. What shows: crisp glass streaks, paint chips and plank edges (ep6), m2's pool grout restored (x4plus alone
had faded it), no snow / bubble rings (excluded), no seams on tiling floors and walls, cut-outs clean.

### Switches (tests; not in the menu)

PS2_TEX_AI=0|1 (wins over the look), PS2_AI_WEIGHTS, PS2_AI_MS (4) / PS2_AI_MS_MIN (0.5), PS2_AI_NET=0 (cache only),
PS2_AI_RESTORE (1), PS2_AI_CLARITY (0.8), PS2_AI_GRAIN (0), PS2_AI_PAD (12), PS2_AI_LOG=1 (a line every 600 frames:
built / cached / saved / excluded / queued, TFLOP, ms per GFLOP, p50 / p99 / worst GPU ms per frame), PS2_TEX_NRM=0,
PS2_TEX_NRM_K (10), PS2_TEX_DUMP=3 (also `<key>_ai.png` and `<key>_nrm.png`), PS2_GPU_FT=1 (frame-time line every 3000
pictures after PS2_GPU_FT_SKIP = 90 s). Note: with PS2_USER_DIR set, sly2.ini is read from that folder (tex_shots'
-Ini then does not apply): use PS2_TEX_AI=1 for headless runs.

### Gates (code of 0391e82)

run_tests 16/16; unit suites all pass; m9_exact S1 + S3 -Game identical (base: git archive of main 81cde3d's runtime);
gpu_diff_all mixed S=1 28/28; r2_sweep -Parallel 1 Faithful ep1, ep3, m2_boardroom tfo, tfb, mf 9/9; Reimagined
(PS2_TEX_AI=1, fresh caches: the net built during the checks) ep1, m2_boardroom tfo, mf 4/4. Faithful / Retro: nothing of
this runs (tcf_look 0, ra_on_now 0); the shared code only gained an unused slot field and an env-gated log.

### Not done / next

- The menu still hides Reimagined unless sly2.ini says `look = reimagined` (menu.c build_choices): the owner decides
  when both halves are in.
- A "prepare all levels" button (needs a census of every level's textures from the disc data rather than walks).
  (Shipping the weights: done, notes/install.md "AI model".)
- fp16 feature buffers would halve the net's 294 MB and speed it up on cards with fast fp16.

## Look switch crash (owner, 0.1.1226, 2026-11-10)

Faithful -> Reimagined + Apply crashed "sometimes" (0xC0000005 reading 0xFFFFFFFFFFFFFFFF, in ntdll or nvwgf2umx;
the log stopped after "ai: Reimagined textures on", before "x4plus net ready").

**Cause** (proven): `ra_list` (the Reimagined cache's file list, read again every time Reimagined turns on) kept the
previous listing's count but started from a capacity of 0, so the second listing appended past the end of the first
one's buffer: a heap overflow of (files x 8) bytes per switch (the owner: 637 entries in a 1024-entry buffer, then 1348
more written behind them). Whatever lived after the buffer - heap metadata, a driver object - was overwritten, hence the
two crash sites. Repro (below, warm cache of 1763 files, Look toggles only): the logged count grew by 1763 per toggle
("26445 in the cache") and the game crashed after 28 changes with the owner's signature. Fix: the listing starts empty
and grows within its own recorded capacity (`RA.have_cap`; the HD folder's list hardened the same way).

Also made safe while there (no crash seen from them, but the same class): the AI worker thread no longer writes any RA
field the render thread uses or releases - its new compute shaders and read-back buffers go into locals and are handed
over with the net under the lock, installed by the render thread (`ra_net_poll`); `ra_release` resets the items' states
under the lock. The worker never used the immediate context (ai_gpu_create_cs only keeps the pointer; the device is
free-threaded); every context call is on the render thread.

**Stress gate**: scripts/gfx_stress_gate.ps1 [-Cold] - the owner's settings (High, Reimagined, 120 fps), ep2 hub with Sly
walking (textures building), and every 20 pad frames one change as the menu's Apply makes it (`PS2_GFX_STRESS=<n>,<actions>,
<from pad frame>`, window.c: l Look Faithful <-> Reimagined, r Retro <-> Reimagined, q Quality Low..Ultra, s render
scale 100 <-> 75 %, m MSAA 8x <-> 4x). Passes with no crash and >= 500 changes.

| build | run | changes | Reimagined re-inits (nets) | crashes |
|---|---|---|---|---|
| 0.1.1226 | warm, Look only | 28 | 15 | 1 (0xC0000005 reading 0xFF..FF, ntdll) |
| fixed | warm, Look only | 1366 | 685 | 0 |
| fixed | warm, all five (2 runs) | 2664 | 536 | 0 |
| fixed | cold, all five (2 runs) | 2843 | 572 | 0 |

Gates on the fix (alone): run_tests, 28 unit suites, m9_exact S1 + S3 -Game identical, gpu_diff_all 28/28, r2_sweep Faithful 9/9 and Reimagined 9/9, reim_stability_gate PASS (orbit 1.085 x, trees 1.031 x, no world-matrix flips).
