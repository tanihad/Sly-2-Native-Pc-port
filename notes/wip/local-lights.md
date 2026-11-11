# WIP: local lights in the Reimagined look (branch next/local-lights, worktree sly2-worktrees\light)

**Goal**: lamps, windows, fires, torches cast light on Sly, characters, walls and ground under Look = Reimagined,
from the game's own light objects; stable in motion; <= 1 ms GPU at 1440p; PS2_REIM_LIGHTS=0 kill switch.

## Done
- Baseline exe (e656907) kept at build\locallights\base\sly2.exe (not committed).
- RE of the game's light table (r2_dis.py func 0x1A4478 / 0x1A3F78 / 0x1A3D00): see Findings.
- Diagnostic dump PS2_REIM_LIGHTS_DUMP=1|2 (r2_capture.c lights_dump): ep2 hub 168 lights, ep1 hub 241 lights, all
  kind 1 (point), values constant over time (no game flicker).

## Done (code, commit 4bd7a3c + later)
- r2.h r2_light + slot table; r2_capture.c lights_capture at frame_begin (hybrid, Reimagined only).
- gpu_r2front.c FL: copied into fdata once per (serial, arena epoch gf_data_epoch); gpu_r2_frame.pad = off/16 + 1.
- gpu_reim.h: RLIN copy at reim_note; reim_lights_build (weights ease 0.25 s, picture-frustum cull with the picture's
  ndc range - the clip volume is the VU guard band ~8x the picture!, priority cap by Quality, flicker on orange
  fires); light.hlsl ps_reim_local (half res) + composite (lin += (lin + floor) * E); DBG=5 shows the light buffer.
- PS2_REIM_X bit 32 = local lights (A/B in one run with PS2_GPU_AB_R8=4096).

## In progress
- tuning strength / falloff (pool shape: win / (1 + x^2 / core^2)); ep1 lights' positions check (log level 2).

## Next
1. Tune on ep2 / ep1; interior (m2_boardroom), cutscene; motion videos (scratchpad vid.ps1: two deterministic runs
   PS2_REIM_LIGHTS=0 vs on, side-by-side mp4).
2. Cost (PS2_GPU_PROF "reim-lights"), before/after fps.
3. Gates; notes/reimagined-lighting.md "Local lights" section.

## Traps
- light_shots dumps only pictures whose VBlank is a multiple of -Every; the PS2_GPU_AB phase switch logged "from vN"
  is NOT yet applied to picture N: use mid-phase pictures (At 5730, Every 30: 5730 = B, 5790 = A).
- Never use [IO.File] with relative paths: the process cwd is the MAIN checkout (an edit landed there once; reverted).

## Findings
- Static world SW = 0x3DD490. SW + 0x4154: LIGHT* table[256]; SW + 0x4590: free-slot bitmask (8 words, set = free);
  SW + 0x4554 count/flag; SW + 0x4570/0x4580: 256-bit mask of always-considered lights; SW + 0x455C: BSP root of light
  influence (nodes: plane +0, children +0x10/+0x14, leaf index +0x1C -> 32-byte mask at SW + 0x4564 + leaf * 32).
- LIGHT: +0xE0 kind (1 = point), +0xF0 colour as HSV-like (hue deg, sat, value 0..255), +0x100 world position (w 1),
  +0x110 score a, +0x114 score b (score = a + b * distance, 0 at the outer radius), +0x118 table slot index,
  +0x120 VU record qw0 {type 1, y (ambient share), z (diffuse share, negative: the VU's direction convention),
  w = falloff offset}, +0x130 colour rgb (max channel 1) + w, +0x140 {position, falloff slope}: VU falloff
  f = clamp(w0 + slope * d, 0, 1) = 1 inside +0x164 (inner radius), 0 at +0x168 (outer radius). +0x150 two shading
  params (e.g. 70, 220), +0x15C flag 0/1.
- The VU per object (0x0298 / 0x1050): C = 2 col (z f(N.v) + y) x falloff, f(d) = min(d + d^3, -y/z): wrapped diffuse.
- World units ~1 cm, z up. ep2 hub: ~70 warm lights (torches, fires: (1, 0.61, 0.22), (1, 0.74, 0.37)), ~80 cool
  blue fills (0.2-0.4, 0.5-0.9, 1) of radius 400-2500 that tint characters with moonlight. ep1 hub: mostly warm yellow
  (1, 0.9, 0.41) r 800 / 1372 (street lamps), (1, 0.77, 0.45) r 300-600, some cyan.
