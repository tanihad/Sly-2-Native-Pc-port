# pc-research: next big performance step (200+ fps) - checkpoint

**Status: complete** (2026-10-23). Deliverable: notes/perf-plan.md. The branch also carries the conservative-depth prototype in
runtime/gpu (m9_exact identical 28/28 S=1/3; gs-hw: 4 conformance checks (hardware + WARP) still test the old depth
encoding and fail - rewriting them is part of milestone P1). Quiet-GPU m9_bench 3x GPU per game frame: ep1 9.50 -> 6.40, ep3
8.82 -> 6.89, ep5 9.16 -> 6.59, ep8 boss 7.03 -> 6.92, ep7 boss 11.30 -> 9.19. Big files deleted (VU recording, frames).

**Goal**: decision document notes/perf-plan.md (options compared with measured evidence, recommended plan with
verifiable milestones, NVIDIA features adopt/skip) for reaching 200+ fps at native 1440p.

Branch next/pc-research (worktree C:\Users\tanih\Projects\sly2-worktrees\pc-research), from main f3bb29f.

## Done
- Read frame-rate.md, gpu-renderer.md, performance.md, native-port-principles.md, vu-microcode.md, hle-architecture.md,
  widescreen.md, d3d11-renderer.md sections 0-4 + appendices L/M.
- Web research by two agents: notes/wip/pc-research-sources.md (sources per bullet).
- Release build build\rel\sly2.exe (46 s). Baseline gs_replay build\gsr_base (pristine runtime from git archive in
  build\base_src\runtime). Prototype gs_replay build\gsr_ez (conservative depth, below).
- scripts/earlyz_probe.c (headless D3D11 probe) - RESULT on the RTX 4060 Ti (2048x1344, 16 quads, heavy PS):
  hidden/visible GPU time ratio: no depth output 0.001; SV_Depth 1.000 (no early rejection at all, also with a plain
  occluder); **SV_DepthLessEqual + GREATER / GEQUAL 0.001-0.002 (early rejection works)**; SV_DepthGreaterEqual + LESS
  0.001; wrong-direction combos 1.0. So M9's "conservative depth gives no early rejection" was due to its bound (pos.z
  "below every depth" = a useless bound, and the old encoding asfloat(z + 0x00800000) is ~1e-38: denormal gradients),
  not the hardware.
- Prototype "conservative exact depth" (commit f084829, unverified): depth encoding asfloat(min(z,0xFFFFFF) +
  0x3E800000) in [0.25,1) (linear below 2^23, convex above: interpolated chord is an upper bound); vertex Z (gvtx.pad,
  already filled) as input TEXCOORD1; vs_ff_tri pos.z = enc(z + 64) (upper bound); sprites / line pixels pos.z = their
  exact constant Z; FF ps_ff (PRIM != 3) output SV_DepthLessEqual with SV_Position noperspective centroid; z_high
  threshold for Z32 0x3F000000 -> 0x00FFFFFF. Files: gs.hlsl, int64.hlsli, gpu_exec.c (input layout, Z clear encode),
  gpu_front.c (threshold). gpu_conf.c conformance kernels still expect the OLD encoding (update if adopted).
- In-game baseline batch (fps2_batch, unlimited, 1x) got 3 scenes before the pause, on a machine shared with pc-smooth's
  game runs (contended, numbers worse than pc-fps2's): ep1 1x 69 fps (worker 10.0 / GS 11.1 / frontend 11.9 / render 1.5
  / GPU 5.9 ms), ep3 1x 103.5 (6.9 / 4.6 / 4.7 / 0.7 / 1.8), ep5 1x 80.2 (8.3 / 6.6 / 8.3 / 1.1 / 5.2). Rerun when quieter.

- **Conservative exact depth VERIFIED** (commit 3eb99a4): the first encoding ([0.25,1) via bit patterns) gave no gain:
  Sly's Z (< 2^19, max 0x7F5C3 over the whole corpus) then spans only 0.004 of the depth range, and the probe shows
  NVIDIA's coarse culling loses conservative-depth rejection at such separations (earlyz_probe args `16 64 d32s8 0.004`).
  Final encoding: z / 2^20 (exact, linear for z < 2^20; Z32 above -> z_high/zpatch path). m9_exact full corpus S=1/3
  Every 7: **all identical**. scripts/gpu_prof_ab.ps1 (PS2_GPU_PROF=2, GPU shared with another agent at ~40%, so ms are
  inflated, PS counts are not), S=3 16:9, per VBlank: ep1 hub GPU 7.46 -> 4.93 ms, PS 21.7M -> 13.8M (tri-z 13.85M ->
  5.98M, 5.42M pass); ep3 6.74 -> 5.01; ep5 7.26 -> 5.11; ep8 boss 4.85 -> 4.78; ep7 boss 9.45 -> 8.02. Remaining fixed
  costs per VBlank: alias16 + alias16-draw ~1.3-1.6 ms (CT16 shuffles), ff-sprite 0.4-0.8, ff-line 0.25-0.5, ref-tri
  (ROV) 0.2-2.4, clear 0.2-0.3, present 0.15-0.3.

- Offline thread-bench (gs_replay hwsubmit --mixed --front-thread --pin, 16:9, per game frame, GS decode / frontend ms):
  ep1 4.2/5.9 (S1) 3.7/5.8 (S3); ep3 5.2/6.9, 5.5/8.2; ep5 5.2/8.3, 4.8/8.4; ep8 boss 5.5/7.3, 5.4/7.6; ep7 boss
  2.8/4.5. ~32-41k primitives per game frame (ep1 2.81M prims / 87 frames) -> ~180 ns per primitive in the frontend.
- Frontend section profile (gs_replay -Define GPU_FRONT_PROF, build\gsr_fprof; rdtsc inflates totals ~1.7x), ep1 Mcycles:
  state lookup 228, geometry 91, inputs 85, texture 181, tier (+line pixels) 155, ownership 227, key/merge 205, record
  emission 267, triangle vertices 256, early exits 39. Pure per-primitive (geometry, inputs, records, vertices, most of
  texture/tier) ~55%; sequential (state dedup, ownership, merge) ~45% -> Amdahl with 4 workers ~1.7x.
  ep5 / ep8 boss similar (key/merge larger in ep8: 447).
- VU1 input per frame (PS2_CHAIN_STATS, chain_stats.py): ep3 hub walking (camera moving, VBlank 5100): 1.2 MB unpack, 272 KB
  PATH2, 1782 MSCALs, 2644 tags; next frame: 91.4% of unpack bytes unchanged at the same address, 1.9% same content
  elsewhere, 6.7% new. Safehouse menu views (ep1 / ep3, static camera, 2.5-2.9 MB unpack, ~4000 MSCALs): 97% unchanged.
  The camera is a few qwords (pc-fps2); world matrices of static objects and all mesh data repeat.
  TRAP: fps2_scene's time-based pad tokens are relative to START (X at START+25 s ~ 57 s wall): earlier windows show the
  safehouse "Press X" view, not the hub.
- VU replay (ep1 safehouse recording, 9000 programs, compiled, pinned CPU 4): 1.55 ns/pair, 0 differ. Live-in test
  (VU_REPLAY_LIVEIN=1, registers scrambled): 95 programs (1.1%, 8.9% of pairs; mostly 188054331 at 0x07F0 after its
  0x0738 set-up) depend on inherited registers; 98.9% of programs (91% of pairs) are functions of VU memory only.
  (First version counted the scrambled run's pairs - runaway loops - and said 97%: fixed.) Programs per frame: 376110651
  entries 0x2EE8 (2102 programs, 423 pairs avg), 0x2E20 (1274, 582), 0x2EB0 (504, 1267), 0x2FC8 (122, 3575), ...;
  the [E] nop stub 2375 times.
- build\rel_base\sly2.exe = baseline game (f3bb29f GPU files), build\rel\sly2.exe = with conservative depth; in-game A/B
  batch running (fps2_batch ab_s3 / ab_s1 -> build\warp\fps2_ab_s*.txt); machine load ~87% CPU from another agent.

## In progress
- In-game A/B batch (background); meanwhile drafting notes/perf-plan.md.

## Next
2. VU1 input data static vs per-frame: add a compact PS2_CHAIN_STATS mode to runtime/chain_dump.c (per DMA tag: id,
   data address, qwc, content hash, unpack/direct/mpg words), dump 2-3 frames, classify in Python.
3. Offline per-stage costs with gs_replay --backend hwsubmit --mixed --front-thread (thread-bench), and
   GS_REPLAY_PROFILE_FRONT / --profile for frontend parallelisable share; vu_replay for VU1 ns/pair; programs /
   vertices / prims per frame.
4. In-game per-thread breakdown rerun (fps2_batch unlimited 1x / 3x, ep1 ep3 ep5 m8) - note machine load.
5. Write notes/perf-plan.md; finish checkpoint.

## Findings / traps
- PowerShell 5.1 Set-Content -Encoding utf8 writes a BOM: fxc rejects it ("Illegal character"). Write files with
  [IO.File]::WriteAllBytes / no-BOM encoding, or strip the BOM after.
- SV_DepthLessEqual needs the PS SV_Position input `noperspective centroid` (fxc error otherwise).
- Research verdict so far: VU1-microcode-on-GPU has no precedent (RT64 = HLE of known semantics; paraLLEl-RDP/GS keep
  geometry on CPU; PCSX2 MTVU threads VU1 on CPU). DLSS FG needs DX12; no motion vectors here. Smooth Motion (driver FG)
  works for DX11 on RTX 40 (user-side). Reflex via public MIT NVAPI possible on D3D11.
