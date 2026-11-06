# AI textures (prototype, phase 1)

Owner request 2026-11-06: an AI texture upscaler for the PC port, judged first as a before/after, no game integration.
Owner direction during the work: the menu gets **Look = Retro / Faithful / Reimagined**; AI textures are for Reimagined
("I want Sly to be kinda reimagined ... go above and beyond"), Faithful keeps the Enhanced textures
(notes/enhanced-textures.md). So the bar is "a striking modern reimagining", not "faithful", while real defects (tiling
seams, broken cut-outs, garbled text / logos, mush) still count. Branch next/ai.

## Model, licence, safety

- **Real-ESRGAN `RealESRGAN_x4plus_anime_6B.pth`** (xinntao, BSD-3-Clause; the only file downloaded, approved by the
  owner): `tools\models\realesrgan\` in the main checkout, 17.11 MB, SHA-256 F872D837...99DA. Never committed. BSD-3 allows
  redistributing the weights (with the copyright notice and licence text in the release's third-party notices).
- Network, verified from the state dict (scripts/ai_model.py `check_arch`): basicsr **RRDBNet, num_feat 64, num_grow_ch
  32, num_block 6, scale 4**, 3 input channels (no pixel-unshuffle), 192 tensors, 4.47 M parameters. ~5.7 M
  multiply-adds per input pixel (91 convolutions at 1x, 4 at 2x / 4x).
- **Loading without pickle.load / torch.load**: the .pth is a zip; `data.pkl` is read by a restricted `pickle.Unpickler`
  whose `find_class` knows only `collections.OrderedDict`, `torch._utils._rebuild_tensor_v2` and the `torch.*Storage`
  tags, all mapped to inert stand-ins in ai_model.py; `persistent_load` accepts only `('storage', tag, key, location,
  numel)`; everything else raises. Tensor bytes come from the zip's `data/<key>` entries (sizes checked). The file's
  SHA-256 is checked first. Python runs with `-I`. Tested: pickles naming os.system, builtins.eval,
  torch.serialization.load and subprocess.Popen are refused. Standard library only (there is no numpy / PIL in the venv
  or the system Python, and nothing may be installed).
- `python -I scripts/ai_model.py export` writes `build\ai\x4anime6b.bin` (SLYAIW01: named float32 tensors) for the C code.

## Implementation (our code)

| file | what |
|---|---|
| scripts/ai_model.py | safe loader, architecture check, weight export, pure-Python forward pass (reference) |
| runtime/gpu/ai_up.c / .h | weight file reader, plain C reference forward pass, D3D11 inference (`ai_gpu_create / run`) |
| runtime/gpu/shaders/ai_up.hlsl | `cs_conv`: one 3x3 convolution (+ bias, lrelu, residual mix, nearest 2x input) per dispatch |
| scripts/ai_up_tool.c | build\tools\ai_up.exe: `cpu`, `gpu`, `batch` (the net), `alpha` (alpha upscaling), headless device |
| scripts/ai_textures.py | `build`, `selftest`, `gpucheck`, `upscale <dump> <pack>`: dump folder -> HD-pack folder |
| scripts/ai_shots.py | Original / Enhanced / AI side-by-sides and 2x close-ups from two tex_shots.ps1 A/B runs |

**GPU kernel** (fp32): feature maps are float planes [image][channel][y][x] in 8 structured buffers (input, conv_first
features, the RRDB input, two 192-channel dense-block buffers used ping-pong, two 64-channel 4x buffers, output). A
group (8 x 8 threads) computes 32 x 8 output pixels x 16 output channels; each thread 4 pixels (columns tx + 8k: no LDS
bank conflicts) x 16 channels in float4 accumulators; input channels in chunks of 8 through group-shared memory (tile
34 x 10 x 8 + weights 8 x 9 x 16). Weights are re-packed per layer ([oc block][ci chunk][ci][tap][oc 16]). The dense
blocks append their 32 growth channels to the buffer they read; conv5 and the RRDB end fold the 0.2 residual scalings
into one epilogue (`v * a0 + res1 * a1 + res2 * a2`, optional second output). One dispatch chain per batch of
same-sized images (97 dispatches). The buffers a layer uses are compile-time macros (13 permutations): a `switch` over
8 UAVs inside the unrolled kernel never finished compiling in d3dcompiler_47. Compiling costs ~4 s per permutation, so
the tool caches the DXBC (AI_UP_CACHE); the game would embed them through scripts/hlslc.c like every other shader.

**Correctness**: `ai_textures.py selftest` (5 x 4 image): pure Python vs C max |diff| 4.8e-7; C vs GPU 1.0e-5.
`gpucheck 70x37` (multi-tile, all channel blocks, the 4x stages): C vs GPU max 3.3e-6, no output value off by 1/255.

**Per texture** (ai_textures.py `upscale`):
1. colour under transparent texels: mean of opaque neighbours, grown outwards (no dark fringe into the net);
2. dither: a detector + range-limited 3x3 smoothing exists but is **off** (AI_DITHER): it fired on 1108 of 1173
   textures (4-bit palettes everywhere) and cost all grain; the net removes dither by itself (see verdict);
3. padding 16 texels per side: **wrapped** on an axis where the texture continues across its own edge (seam step no
   larger than 2x the typical inner step), else edge-replicated; > 128 texels: tiles of 128 + 16 context, cropped and
   stitched (no visible tile joins);
4. the net (4x); AI_UP_DETAIL = g adds the source's own fine grain back (texel minus its 3x3 Gaussian, Catmull-Rom 4x,
   times g; default 1.0) under the net's lines;
5. alpha is not the net's: Catmull-Rom 4x, wrapped / clamped like the RGB; cut-out textures (alpha mostly 0 / 255; 49
   of 157 non-opaque ones) get a steep ramp (x2.5 around 50 %): smooth contours, the same coverage;
6. `<pack>\<hash>.png` at 4x (powers of two stay powers of two): the HD-pack hook's names.

## Test set and results

Dumps (PS2_TEX_DUMP=1, headless, PS2_USER_DIR = build\ai\user): ep1 hub, ep3 hub, ep6 hub, m2 boardroom = **1173
unique textures, 9.8 M texels** (64x64 291, 128x128 270, 32x32 210, 64x32 91, 256x128 64, ...; 157 with alpha). In
game every one of them was taken from the pack (`tex: ... 140 HD, 0 built` per scene).

**Speed** (RTX 4060 Ti 8 GB, shared with the desktop; the whole test set):

| step | time |
|---|---|
| the net on the GPU (1269 tiles, 17.5 M padded input pixels, 142 batches) | **22 s** (0.8 M input px/s, ~9 TFLOPS effective) |
| preparation (bleed, tiling test, padding; pure Python) | 16 s |
| alpha (C) / grain (C, single thread) | 1 s / ~10 s |
| PNG writing (pure Python zlib) | 18-25 s |
| total | 58 s (net alone) / 77 s (with grain) |

The Python steps would be C / compute in phase 2 (they are a few operations per texel against the net's 11 MFLOP).
**Whole game, projected**: the four scenes give ~1200 textures; Sly 2 has 8 hubs plus their jobs, with heavy sharing;
assuming 10 - 15 k unique textures (~100 - 130 M texels) the net needs **~4 - 6 min on a 4060 Ti** (~3x that on a
GTX 1060-class GPU, ~1 min on a 4090). Not a guess-free number: the true count needs a census over all levels.

**Video memory**: 1.33 GB peak for the process (1347 MB of buffers at the default batch bound of 131 k padded pixels;
the 4x feature buffers dominate). With `--max-pixels 32768`: 351 MB, but 60 s instead of 22 s (small batches leave the
GPU idle). fp16 storage would halve either.

**Pictures** (local only, never committed): `build\ai\shots\` - per scene `<scene>_original / _enhanced / _ai /
_ai_pure.png` (2560 x 1440), `<scene>_compare3.png` (middle 852 columns: **Original | Enhanced | AI**),
`<scene>_closeup3.png` (the Enhanced-textures pictures' 640 x 720 regions at 2x, same order),
`<scene>_pure_vs_grain_closeup3.png` (Original | AI net alone | AI + grain), `ep6_hub_ground_closeup3.png` (planks,
barrel). "AI" = the default (grain 1.0). Each column comes from its own A/B run (A = Original, B = Enhanced or the HD
pack); the runs' Original pictures differ by 3 - 6 / 255 on average (Sly's idle animation, particles), so Sly's pose
can differ between columns.

## Verdict (judged as "Reimagined")

What it does well:
- **Dither and 4-bit noise are gone**: ep6's teal wall boards and window glass (dotted in Original, grainy in Enhanced)
  become clean painted surfaces with crisp brush streaks; the best-looking change in the set.
- **Crisp shapes and outlines**: flames, the coin / orb sprites, signs and icons (the no-entry sign stays a clean circle
  and bar, no garbling), crack lines on ep3's rocks, leaf outlines on ep1's vine wall: they read like a clean
  illustration instead of a blurred bitmap.
- No tiling seams found: wrap-padded tiling textures' seam steps match their inner steps (median ratio 1.1 over 118
  sampled tiling textures; the few high ones already have a step at the edge in the original); none visible in game
  (ep6 planks, m2 tiles). Cut-out alpha (ep3 moss, foliage) keeps clean edges.

What it does badly:
- **It is conservative and flattening, not a reimagining**: the anime model removes material grain instead of adding
  detail - wood grain, grass, cloth and stone go flat and cel-like. At scene scale AI and Enhanced look alike; the
  difference is "cleaner / more cartoon", not "stunning new detail". The grain re-injection (default) brings some
  material back, but also some of the dither on 4-bit textures.
- **Low-contrast linework fades**: the m2 pool floor's tile grout (which Enhanced keeps crisp) gets weaker - mush there.
- **Soft particle sprites turn into rings**: ep6's falling snow (a soft dot) becomes small hollow rings with a centre dot
  (both with and without grain) - a visible artifact; particle / soft-blob textures must be excluded.
- Text: these four views show almost no legible lettering, so text is **not verified**; icons came out clean, but the
  anime model redraws strokes - signs with small lettering (ep1's menu board, posters) need a dedicated close-up first.

**Bottom line**: works (correct, fast, seamless), but anime 6B alone does **not** deliver "a stunning modern
reimagining": it is a clean-up filter with a cel look. As it stands it is a Faithful-plus option, not Reimagined.
To go further (not downloaded; the owner decides):
- **RealESRGAN_x4plus** (the general 23-block model, BSD-3, ~64 MB): hallucinates material texture (wood, stone, fabric)
  instead of flattening it; ~4x the compute of 6B (23 vs 6 blocks) - ~20 min for the whole game on a 4060 Ti.
- **Community ESRGAN models** (e.g. 4x-UltraSharp, 4x_foolhardy_Remacri, game-texture models): sharper, more detail;
  the same RRDBNet, so this code runs them unchanged (num_block from the weights) - **check each licence** (several are
  CC BY-NC: not shippable in a release).
- **Two-pass chain**: 6B to clean dither / palette noise, then x4plus (or a 2x detail model) on the cleaned image -
  likely the best "clean + rich" combination for 4-bit PS2 art.
- **Diffusion img2img / ControlNet-tile upscalers** (Stable Diffusion family): the only route to a true redraw (new
  detail, materials); heavy (GBs of weights, minutes per texture), consistency across textures and licence terms are the
  hard part; offline-only.
- **Material maps**: generate normal / roughness maps from the albedo (e.g. a small normal-estimation net or a height-
  from-luminance filter) and feed them to the Enhanced lighting's per-pixel path (notes/enhanced-lighting.md): this
  would change the look more than any upscaler.

## Plan for phase 2 (in-game "Reimagined")

1. **Never ship generated textures** (they are derived from the owner's / player's disc): generate on the player's PC
   into a local cache (`<user dir>\textures\ai\<model id>\<hash>.png` or a packed cache file), keyed by the content hash
   the HD-pack hook already uses, plus the model id and pipeline version (a change regenerates).
2. **Where**: (a) lazily in game - a texture seen for the first time is queued; a low-priority compute pass (or a worker
   thread with its own D3D11 device) upscales a few per frame within a time budget; until then the Enhanced image is
   shown (the ET cache already handles late replacement); or (b) an installer / first-launch pass over every level's
   textures - needs a texture census of all levels (decode each level's texture data with the runtime's decoder,
   offline); (a) first, (b) as an optional "prepare all" button with a progress bar.
3. **Inference in the runtime**: ai_up.c as is (D3D11, no dependencies), shaders compiled at build time through
   hlslc (perms.txt), fp16 storage for half the memory, a batch bound chosen from free VRAM (~350 MB is enough).
   Port the pre / post steps (bleed, tiling test, padding, alpha, grain) to C or compute.
4. **Weights**: read the .pth with a C port of the restricted unpickler (zip stored entries + a pickle opcode subset:
   no code execution possible), or ship the converted float16 weights (BSD-3 allows it; 9 MB for 6B).
5. **Exclusions**: particle / soft-blob textures (snow rings), textures drawn with additive blending, very small (<= 16
   px) sprites, the HUD and fonts (already outside the art path); per-texture fallback to Enhanced for low-contrast
   line art (a grout / line detector like Enhanced's deband exemption).
6. Menu: Look = Reimagined turns it on; Faithful = Enhanced; Retro = Original. A model choice (e.g. "Clean" = 6B,
   "Detailed" = x4plus) if the owner wants one after seeing x4plus.

## How to reproduce

```
. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1; cd <worktree>
$py = 'C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe'
& $py -I scripts\ai_textures.py selftest; & $py -I scripts\ai_textures.py gpucheck 70x37
scripts\tex_shots.ps1 -Scene ep6_hub -At 5400 -Frames 70 -Every 10 -Phase 30 -Tag enh -Extra "PS2_TEX_DUMP=1;PS2_USER_DIR=<dir>"
& $py -I scripts\ai_textures.py upscale <dir>\textures\dump <dir>\textures
scripts\tex_shots.ps1 ... -Tag ai -Extra "PS2_USER_DIR=<dir>"
& $py -I scripts\ai_shots.py ep6_hub build\tex\shots\ep6_hub__enh build\tex\shots\ep6_hub__ai build\ai\shots --a 5410 --b 5440 --ref <closeup.png>
```
A phases are Original (from multiples of 60 VBlanks, 30 long), B phases the Enhanced path (with the HD pack: AI).
