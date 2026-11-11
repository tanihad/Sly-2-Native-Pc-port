# WIP: local lights in the Reimagined look (branch next/local-lights, worktree sly2-worktrees\light)

**Goal**: lamps, windows, fires, torches cast light on Sly, characters, walls and ground under Look = Reimagined,
from the game's own light objects; stable in motion; <= 1 ms GPU at 1440p; PS2_REIM_LIGHTS=0 kill switch.

## Done
- Baseline exe (e656907) kept at build\locallights\base\sly2.exe (not committed).
- RE of the game's light table (r2_dis.py func 0x1A4478 / 0x1A3F78 / 0x1A3D00): see Findings.
- Diagnostic dump PS2_REIM_LIGHTS_DUMP=1|2 (r2_capture.c lights_dump): ep2 hub 168 lights, ep1 hub 241 lights, all
  kind 1 (point), values constant over time (no game flicker).

## In progress
- m2_boardroom dump; then the data path EE -> slot -> frontend -> fdata -> render thread.

## Next
1. r2.h: r2_light + slot table; r2_capture.c fills it at frame_begin (hybrid, Reimagined only).
2. gpu_r2front.c: copy into fdata once per (serial, arena epoch); gpu_r2_frame.pad = offset/16 + 1.
3. gpu_reim.h: gather lights, fade weights per light id, CPU cull (frustum sphere + distance), cbuffer list.
4. light.hlsl: half-res ps_reim_local (world pos + depth normal, smooth falloff, wrap N.L), composite add.
5. Quality scaling, PS2_REIM_LIGHTS=0, tests, captures, gates, notes/reimagined-lighting.md section.

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
