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
- A "prepare all levels" button (needs a census of every level's textures from the disc data rather than walks; see
  "Ready before first sight" below for why it was not done). (Shipping the weights: done, notes/install.md "AI model".)
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

## Ready before first sight (no pop-in; branch next/tex-prebuild, 2026-11-11)

Owner request: Reimagined's AI textures must be there before the player sees a texture - no visible switch from the
Faithful (Enhanced) image to the AI image during play. Before: a texture's AI image was looked up (cache) or built
(net) only after its Enhanced image had been built and shown, so every texture of a level changed once on screen; with
a warm cache too (the file still had to be decoded and uploaded, behind the worker's other jobs, 256 K texels a frame).

### Options weighed

| option | verdict |
|---|---|
| (a) at the level's load, find every texture it will use and build them | the loading screen is **0.2 s** on PC (widCurrent changes at the load's start, the 3D follows ~22 frames later: the ISO reads are instant), far too short to build anything; finding a level's textures before it draws them would need the engine's level data (shaders / textures / CLUTs) parsed and decoded exactly as the GS path does, plus each use's addressing and alpha test (they are part of the image's key): a project of its own, not reliable enough here |
| (b) a one-time "prepare all levels" pass | needs the same census of every level's textures from the disc (walks only see part of a hub); not done |
| (c) cached images ready before first use | **done**, plus level lists so that "before first use" is known, a prediction for the title -> resume case and deferral for the cold case |
| holding the loading screen | not needed (no load-time increase); would need the EE stalled while the render thread works without frames |

### How it works (gpu_tex_ai.h, "Ready before first sight"; PS2_AI_READY=0 = the 0.1.1247 flow, for A/B)

1. **Direct AI images**: a texture whose AI image is in the cache never gets an Enhanced image first. At its first
   resolve (et_resolve, after the content hash) it waits (ES_AILOAD, the original sampling shows - as it does while an
   Enhanced image is being built) for the file, which is decoded and built straight into a new image slot (mips,
   coverage, surface map) and linked: one change on screen (original -> AI) instead of two.
2. **Loader threads** (a third of the CPU's threads, 2..6, below normal priority, their own WIC factories) decode cache
   files; on-demand requests first, preloads after; at most 160 MB decoded and not yet uploaded. The worker keeps the
   net's creation, tile preparation and saving (a long net creation no longer delays cached images).
3. **Level lists**: every level (the game's widCurrent, 0x3D4A68, read from EE memory each frame) keeps the list of
   the textures it showed (key, size, addressing, alpha test): `<cache>\levels\w<world>.bin` ("SLYAIL01", count, 16-byte
   records, written by the worker via .tmp + rename at a level change and every 3600 frames). When the level changes (the
   game writes widCurrent as its load starts), its list is read by a loader and every listed AI image in the cache is
   decoded and built into image slots - free ones, or slots of the previous level not used since the change; a class
   not made yet is made, a full one grows only while no 3D is drawn (never a growth copy during play) - so the level's
   textures find their AI image at the first hash (0 extra frames). Images in the list already resident are kept from
   reuse for 20 s. `levels\last.bin` = the last level played: the title (world 0, which the game's boot logos give ~5 s
   to prepare) also preloads that level's list, so **Resume Game** starts with every listed image ready.
4. **Per-frame share**: cached images' builds are timed with their own timestamps (~1.7 ms per million texels: no
   upscale) - 256 K texels a frame normally; while textures wait for their image what the frame leaves of 80 % of its
   interval (<= 2 ms); while no 3D is drawn (boot, loading screens) up to 5 ms.
5. **Deferred swaps** (the cold cache's first visit): an AI image built by the net (or loaded) for a texture whose
   Enhanced image a draw used in the last 2 frames is not applied: the image is written to the cache as usual and the
   texture waits (RI_DEFER) until it is out of sight (no draw for 2 frames) or no 3D is drawn (loading, menus), then it
   is loaded again and applied - never while visible. A texture that never leaves the screen keeps its Enhanced image
   until then (the next visit of the level has it from the list).

Faithful / Retro: nothing of this runs (ra_on_now 0); the shared code gained an art-use counter and two entry fields
(statistics) and one guarded call in et_resolve.

### Measurement (scripts/tex_pop_gate.ps1)

Owner's settings (High: 2560 x 1440, 100 %, MSAA 8x, thin outlines; Reimagined; 120 fps), deterministic
(PS2_FRAME_DT_FIXED), START at the title (Resume Game -> ep2 hub's safehouse), X 2000 VBlanks later, then ~3 min of
walking with camera turns; transition runs also load the ep1 hub at VBlank 9000 (PS2_TASK_SET=w2@9000). PS2_AI_POPLOG=1
counts from the level's first 3D frame on: **swaps** (an AI image replacing an image already shown; visible = a draw
used it in the last 2 frames) and each texture's **first image** with its delay after the first draw that used it (the
original sampling shows meanwhile; <= 3 frames is the content hash's read-back latency, the same in Faithful).
Frame times: every picture after the first 3D frame (~25,800), the statistics lines' frames left out.

| run | swaps after 1st 3D frame (visible) | first image AI / Enhanced | original shown <= 3 / 4-63 / 64-255 / > 255 frames | GPU+present p99 / max ms | render thread p99 / max ms |
|---|---|---|---|---|---|
| warm, before | 354 (349) | 381 / 507 | 517 / 316 / 55 / 0 | 8.64 / 10.81 | 4.43 / 13.64 |
| warm, after (lists) | **0 (0)** | 801 / 81 | **869 / 13 / 0 / 0** | 8.53 / 10.64 | 3.75 / 13.23 |
| warm, after, first run with the update (no lists yet) | 0 (0) | 814 / 83 | 526 / 251 / 120 / 0 | 8.59 / 11.01 | 3.42 / 12.16 |
| cold, before | 283 (39) | 7 / 882 | 521 / 348 / 20 / 0 | 8.87 / 11.90 | 2.56 / 32.26 |
| cold, after | 249 (**0**) | 7 / 878 | 517 / 331 / 37 / 0 | 8.86 / 12.14 | 3.89 / 12.63 |
| ep2 hub -> ep1 hub, before | 327 (321) | 137 / 439 | 173 / 222 / 181 / 0 | 9.35 / 12.92 | 4.93 / 12.14 |
| ep2 hub -> ep1 hub, after (list) | **0 (0)** | 542 / 48 | 500 / 90 / 0 / 0 | 9.31 / 10.79 * | 5.72 / 31.7 * |
| ep2 hub -> ep1 hub, after, first visit with the update | 0 (0) | 552 / 48 | 161 / 149 / 277 / 13 | 9.32 / 13.64 | 5.14 / 20.72 |

\* another agent's game run shared the GPU during part of it. Enhanced first images "after" = textures without an AI
image (excluded soft-alpha / small sprites, sources above 256 texels, HD-pack images). Cold after: 46 AI images were
deferred (18 applied out of sight later, 28 still waiting at the end); the rest of the 249 swaps happened out of sight.

Load (START -> the level's first 3D frame): 0.20-0.27 s in every run, before and after (no hold). Video memory with
the title + hub preloaded: 2.0-2.7 GB in use of the 7.2 GB DXGI budget (image classes 791 MB = the pre-size; 876 MB
after the ep1 hub's load grew one class while no 3D was drawn; surface maps 198-219 MB). Boot / title frame times
unchanged (title GPU p99 15.3 vs 15.5 ms, render thread max 11.2 vs 10.7 ms) while the hub's list is preloaded.

Videos (local, never committed): build\prebuild\videos\ep2_start_before_after_sbs.mp4 (left before, right after; from
the load into the safehouse, 9 s at 60 fps of the 120 fps run), ep2_start_before.mp4, ep2_start_after.mp4. Before: the
table top, the blueprints and the laptop sharpen in visible steps 1.0-1.9 s after the level appears (a static region's
change against the last picture drops 8.6 -> 7.8 -> 6.3 -> 3.3 -> 0.5 in jumps); after: the first frames already show
the final images (only the scene's own fade-in changes).

### What remains

- The first 2-3 frames of a texture's first appearance show the original sampling (the content hash's read-back
  latency; the same in Faithful).
- The first visit of a level after this update (cache files but no list yet): textures wait for their AI image
  instead of showing Enhanced first; at a level start ~300 of ~600 show the original for 0.5-2 s (before: Enhanced
  after a similar delay, then the AI image swapped in). The list is written on that visit.
- A cold cache's first visit shows Enhanced images for whatever stays in view; their AI images come when out of sight
  or at the next visit. A "prepare all levels" pass (option b) is still the only way to a fully AI first visit.
- Switches: PS2_AI_READY=0 (old flow), PS2_AI_POPLOG=1 (ai-pop / ai-pre lines), PS2_GPU_FT=2 (a line per picture).

### Gates (code of this branch's last commit)

run_tests 16/16; unit suites 0 failed; m9_exact S1 + S3 -Game all identical (base: git archive of main e656907's
runtime); gpu_diff_all mixed S=1 28/28; r2_sweep -Parallel 1 Faithful ep1, ep3, m2_boardroom tfo, tfb, mf 9/9,
Reimagined (PS2_TEX_AI=1, fresh caches) ep1, m2_boardroom tfo, mf 4/4; reim_stability_gate PASS (orbit 1.04 x, trees
1.028 x, no texture changed more than once, no world-matrix flips; warm seed with level lists); gfx_stress_gate -Cold
PASS (1183 changes, 0 crashes, 238 nets) and warm PASS (1033 changes, 0 crashes, 209 nets). reim_stability_gate built its
checker with a clang path that only exists for worktrees inside the main checkout's parent: it now takes project.ps1's.

## Lighter and faster (branch next/tex-opt, 2026-11-12)

Owner: "how can we optimize these things without losing quality". Four parts, each measured; Faithful / Retro untouched
(nothing below runs unless the look is Reimagined; the shared shader code only gained branches on bits that are never set
there - m9_exact identical).

### 1. BC7 AI images in video memory (gpu_tex_ai.h "BC7 classes", shaders/bc7.hlsl)

- **What**: an opaque AI image (a JPEG cache file, or a net build without alpha) is built as before (mips, coverage,
  surface map) into a one-slice RGBA8 staging array, then every level is BC7-encoded on the GPU (one thread per 4 x 4
  block into a R32G32B32A32_UINT scratch, copied into the class's BC7 array: a block-compatible copy) into a slot of the
  **BC7 classes** (slot ids ET_BC + c, et_tab info bit 31; the pixel shaders sample t50..t55, their surface maps t56..t61,
  the map is copied from the staging build). 1 byte per texel instead of 4. Images with alpha (cut-outs, glass: ~8 %),
  excluded and Enhanced images stay RGBA8; those classes are pre-sized small under Reimagined (205 instead of 790 MB).
  Alpha is stored x 255 / 128 so an opaque block decodes as exactly 0x80 in every mode.
- **Encoder** (our own; partition / anchor tables read back from the hardware decoder, `bc7_tool probe`): mode 6 (one
  subset; p-bits 1 for alpha 255); where its block error exceeds 16, mode 3 (two subsets, 7+1-bit endpoints) on the two
  partitions with the least within-subset variance. Endpoints: the principal axis' extremes, p-bits by their own
  quantization error, one least-squares refinement; indices by projection (+-1 checked), packed in a uint2 (indexable
  arrays made it 10x slower). Mode 1 was tried too: +0..0.4 dB for 3x the time - off.
- **Quality** (scripts/bc7_tool.c encode: every cache file as the game holds it - GS alpha, square, box mips - encoded,
  decoded by the GPU's own BC7 sampler, compared):

| cache | images | kept RGBA8 (alpha) | BC7 level 0 PSNR min / p1 / p5 / median | all levels min / median | below 42 dB |
|---|---|---|---|---|---|
| the owner's (x4plus-v3) | 887 | 86 | 42.4 / 44.0 / 48.1 / 54.5 dB | 42.4 / 54.1 dB | 0 |
| warm seed (ep1/ep3/ep6 hubs + jobs) | 1764 | 143 | 42.3 / 44.9 / 48.4 / 54.4 dB | 41.3 / 54.0 dB | 0 |

  Threshold 42 dB (RMS error 2 / 255): the JPEG the cache already stores costs 40-48 dB against the image the net
  produced (measured on 18 images of a cold run), so BC7 adds less than the cache itself. Per-pixel worst errors are at
  hard edges (median of the per-image maximum 4 / 255). The staged image is bit-identical to 0.1.1267's RGBA8 build
  (720 AI dumps and 720 surface maps compared, ep2 hub): BC7 is the only change.
- **On screen**: a deterministic camera path is not repeatable across runs (the same exe twice: 31-45 dB, the camera and
  Sly differ), so the comparison is in one run: PS2_TEX_BC7=2 keeps 0.1.1267's RGBA8 images and adds a BC7 copy of each
  (et_tab bit 30) which the shaders sample in the B phases of PS2_GPU_AB=2 PS2_GPU_AB_R8=8192 (owner's settings, his
  cache, ep2 hub, 50 picture pairs in 5 windows): pairs across a phase change vs inside one: 38.2 vs 38.4 dB (motion
  dominates); in the window with a still camera 49.2 vs 50.9 dB, i.e. the BC7 part of a picture ~54 dB.
- PS2_TEX_BC7=0: the AI images stay RGBA8 (0.1.1267's layout).

### 2. Half-float net (ai_up.hlsl U<b>_H, ai_up.c AI_GPU_HALF_*)

- The dense-block and 4x feature buffers (D0, D1, BA, BB) and the weights are stored as half floats (two channels per
  word); the arithmetic stays float, F and R (which accumulate over the 23 blocks) stay float. D3D11 has no native
  16-bit math (SM 5.0) and Ada's non-tensor FP16 rate equals FP32: the gain is memory, not speed.
- Net vs float net (scripts/ai_up_tool.c on 400 of the owner's images box-filtered back to their source size, padded as
  the game does): worst image 58.1 dB, all 62.5 dB, max error 3 / 255. All buffers half: worst 50.0 dB (F / R lose
  precision) - not used. Weights only half: 63.1 dB, BA / BB + weights: 58.6 dB.
- In the game (cold ep2 runs, the same textures built by both nets): the JPEG cache files differ by 45.8-59 dB (median
  ~51; JPEG requantization amplifies 1-3 / 255 into larger steps). The cache key stays x4plus-v3: the owner's ~800 files
  stay valid, new images mix in invisibly. PS2_AI_HALF=0: the float net.
- Also: the layer kernel's tile loader without integer divisions (outputs bit-identical); batches of 2-4 tiles instead of
  one: only 6 % faster (the kernel, not occupancy, is the limit) - not done.

### 3. Dedupe

Keys are the decoded content hash ^ addressing (bits 61, 62) ^ alpha test (bit 60). Census (bc7_tool dedupe): the owner's
cache 7 of 887 files, the warm seed 18 of 1764 were twins of another key with the same content: 18 alpha-test twins (7 + 11),
all identical images (the alpha test only changes the coverage of the mips, built at load), and 7 addressing twins in the
seed (4 of them different images: wrap vs edge padding - not shared). Now (ra_file_of): a key without a file uses its alpha-test twin's file (never built or stored twice).
Same content under other addresses / levels already shares one image (et_map_find). ~1 % of disk and build time.

### 4. Prepare every level ahead of time (feasibility, scripts/ai_prepare_all.ps1)

- 41 start worlds (9 hubs + 32 job worlds; `PS2_TASK_SET=w<world>@<vblank>` loads any of them in one process in 0.2 s).
- Measured chain (baseline exe, empty cache, 60 s per world, 4 processes of 10 levels, frame rate 30, PS2_REIM_FIX=0,
  PS2_AI_MS=24 - the fastest drain; the normal budget caps the net at ~20 % of the GPU): **4517 images, 2346 TFLOP,
  167 MB, 63 min** on this PC (GPU time alone 4.7 min quiet); the net keeps up with what the sweep shows, coverage ~90 %
  of a hub's long-walk textures for ep1, 60-75 % for other hubs from the default start point.
- Estimate: ~40 min with an early stop per level (queue empty, list stable); a GTX 1060-class GPU ~1.5-2 h. Textures a
  sweep misses are built lazily as now. A census from the disc's level data instead: no parser exists (compressed level
  files, keys depend on each use's addressing / alpha test) - days to weeks, uncertain.
- Verdict: feasible as an optional one-time pass (menu entry later); prototype: `ai_prepare_all.ps1 -Levels ep1,w3,...
  -PerRun 4 -Seconds 60 -UserDir <dir>`.

### Disk cache of BC7 blocks: considered, not done

The owner's 887 files: 29 MB as JPEG / PNG, 207 MB as BC7 blocks with mips (whole game ~1.2 GB instead of 167 MB). The
JPEG decode costs 1.3 ms per file on the loader threads (background, 2-6 of them), and the surface map still needs the
colour image: 7x the disk for a saving the player does not see.