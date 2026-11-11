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

## Results so far
- Tuned: gain 3.0, floor 0.025, pool core 0.35, kind weight (rl_kind). Positions verified (DBG=5 markers on ep1 lamps).
- Cost (in-run A/B PS2_REIM_X=32, R8 4096, High 1440p MSAA 8x 120 fps, AI net off): ep1 orbit 7.08 -> 7.22 ms
  (+0.14; pass 0.13 ms), ep2 trees 8.65 -> 8.80 (+0.14; pass 0.09).
- Stability lights off vs on (reim_ab_stability -Mask 4096 -Extra PS2_REIM_X=32): orbit dark flicker/motion 13.44 ->
  12.36 (0.92x), trees 5.36 -> 5.05 (0.94x). reim_stability_gate warm PASS (orbit 1.024x, trees 0.991x).
- Gates so far: run_tests, unit suites 0 failures, m9_exact S1+S3 -Game identical (base build\basert_ll = e656907
  runtime), gpu_diff_all 28/28 (code of 8d422de; later only Reimagined-side gpu_reim.h changes).

## Next
1. m2_boardroom / m7_robot_war (cutscene) A/B stills + cost; motion videos (scratchpad vid.ps1 off vs on).
2. reim_stability_gate -Cold, gfx_stress_gate, r2_sweep Faithful + Reimagined.
3. Final notes section numbers, Status complete.

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
