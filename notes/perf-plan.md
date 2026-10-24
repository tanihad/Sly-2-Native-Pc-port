# 200+ fps: where the time goes, what to change, in what order (pc-research)

Owner's goal: Sly 2 at 200+ fps on the owner's PC (i7-14700F, RTX 4060 Ti, 2560 x 1440 165 Hz), smooth, "regardless how much we
up the resolution". Question from the coordinator: is the proposed plan - Phase 2 (split the GPU frontend's per-primitive work
across cores), Phase 3 (a GPU-driven renderer: VU1 microprograms translated to compute shaders, geometry cached on the GPU,
primitive setup on the GPU) - the best one? Research (sources in section 7 and notes/wip/pc-research-sources.md),
measurements and prototypes on branch next/pc-research. Checkpoint history: notes/wip/pc-research.md.

**Short answer.** Not quite.
- Phase 2 is right, but it is one of **four** stages (worker, GS decode, frontend, GPU) that each need roughly halving: every
  stage runs on its own thread on a different frame, so the slowest one sets the frame rate, and today they are all 7-11 ms.
- Phase 3 as proposed attacks the smallest stage (VU1, ~3 ms of ~25 ms of CPU work per frame), adds work to the GPU, which is
  the limit at 3x and above, and its output feeds sequential CPU stages: it would not raise the frame rate unless the GS
  decoder and frontend also became a compute-shader GS emulator (the design the renderer rejected). It also cannot be
  bit-exact in D3D11 shaders (no round-toward-zero), and no project has made console vector-unit microcode run on a GPU.
- The largest single win found is on the GPU and is cheap: **exact depth with early rejection** (built here: GPU time per
  frame -22..-33% in the hubs, bit-identical on the whole corpus).
- Recommended (section 5): early depth -> GPU pass costs (specialised shaders, fixed passes, a native-resolution sample
  grid) and the frontend split (Phase 2) in parallel -> GS decode fast path -> VU1 / worker split -> close the in-game
  overhead; Reflex-style latency control and a VRR-friendly limiter alongside. No API port, no DLSS / frame generation in
  the game. Expected: 200 fps at 3x (render scale 75%) in the heaviest hub; at 100% (4x) ~140 there and 200 in lighter scenes
  - the honest limit of the exact renderer on a 4060 Ti.

## 1. Where the time goes (measured)

The pipeline (pc-fps2): EE thread (game code, ~2 ms) -> graphics worker (VIF1 unpack, VU1 microcode, GIF) -> GS thread
(register decode, proxy to the frontend) -> frontend thread (per-primitive exact records, ownership, draws) -> render thread
(D3D11) -> GPU. **fps = 1000 / the slowest stage**; 200 fps needs every stage at <= 5 ms per game frame (<= ~4.5 in practice).

### 1.1 In game, per game frame (pc-fps2, quiet machine, release, frame_rate 165)

| scene | fps 3x / 2x | worker | GS thread | frontend | render CPU | GPU 2x | GPU 3x |
|---|---|---|---|---|---|---|---|
| ep1 hub (heaviest) | 87 / 97 | 7.2 | 7.8 | 8.9 | ~0.7 | 6.6 | 11.4 |
| ep3 hub | 98.5 / 160 | 5.4 | 3.5 | 4.1 | ~0.6 | 5.2 | 9.6 |
| ep5 hub | 102 / 124 | 5.5 | 5.6 | 7.4 | ~0.7 | 5.5 | 9.5 |
| m8 boss | 127 / 145 | 4.5 | 4.7 | 5.8 | ~0.7 | 4.8 | 7.6 |

Worker split (ep1): VU1 microcode ~40%, VIF unpack ~20%, GIF + GS queue the rest. Per game frame of the ep1 hub at most
(hwdry): 35.8k primitive kicks, 33.9k exact records, 42.7k vertices after the vertex cache, 4085 D3D draws, 593 distinct GS
states / 4023 state changes, 96 ALIAS16 sprites; VU1 ~2.3-2.7M pairs in 4000-7500 programs.

The 1x / 3x in-game runs made for this study (fps2_batch, unlimited, 35 s windows) ran while another agent's games kept the
CPU at 74-87%: the CPU stages grew 1.3-2x (ep1 hub GS thread 10-18 ms, frontend 11-18 ms) and fps varied +-25% between runs
of the same build, so they only confirm the order of the stages. Kept for the record: 3x base -> early depth ep1 hub 64.5 ->
78.7, ep5 hub 86.6 -> 97.5, m8 boss 99.7 -> 119.4, ep3 hub 102.8 -> 101.7 (CPU-bound under that load).

### 1.2 Offline, each stage alone on a pinned P-core (repeatable)

| recording (corpus) | GS decode ms / game frame | frontend ms / game frame |
|---|---|---|
| ep1 hub | 3.7-4.2 | 5.8-5.9 |
| ep3 hub | 5.2-5.5 | 6.9-8.2 |
| ep5 street | 4.8-5.2 | 8.3-8.4 |
| ep8 boss | 5.4-5.5 | 7.3-7.6 |
| ep7 boss | 2.7-2.8 | 4.5 |

(gs_replay --backend hwsubmit --mixed --front-thread --pin, 16:9; the CPU stages do not depend on the scale.) VU1 (vu_replay
of 9000 recorded programs, recompiled microcode, pinned): **1.55 ns per pair**, 0 programs differ from the interpreter; ~3.6
ms per frame alone.

In game the stages cost 1.5-2x their offline time (GS 3.7 -> 7.8, frontend 5.9 -> 8.9 in ep1): threads exchanging data
through rings on other cores, cache traffic, SMT siblings and preemption all count as busy time (pc-fps2 tried priority,
P-core-only CPU sets and pinning, without effect). That gap is a target of its own (P6).

### 1.3 The frontend: what can run in parallel (section profile, gs_replay -Define GPU_FRONT_PROF, ep1 hub)

| section of fr_prim | Mcycles | parallel? |
|---|---|---|
| state lookup / dedup | 228 | sequential |
| geometry (setup, culling) | 91 | pure |
| inputs (alpha / Z range) | 85 | pure |
| texture source (texel rect, source lookup) | 181 | mostly pure (reads the ownership table) |
| tier + line pixels | 155 | pure given the state |
| ownership (pages, blocks, hazards) | 227 | sequential |
| key / draw merging | 205 | sequential |
| record emission | 267 | pure computation, sequential append |
| triangle vertices (+ vertex cache) | 256 | pure computation, sequential indices |

~55% pure, ~45% sequential -> Amdahl with 4 helper threads **~1.7x** (ep1 in game 8.9 -> ~5.2 ms); ~180 ns per primitive
today, so the sequential half must also get cheaper to reach 4.5 ms. (An estimate from the profile; P3 starts with a
go/no-go prototype.) ep5 and ep8 boss split the same way (ep8: key/merge 447 - more state changes).

### 1.4 The GPU (replay of the corpus, m9_bench, quiet GPU, ms per game frame, 16:9)

| recording | 2x before | 2x after early depth* | 3x before | **3x after early depth** | 4x before (M9, quiet) | 4x after* |
|---|---|---|---|---|---|---|
| ep1 hub | 5.53 | 3.94 | 9.50 | **6.40 (-33%)** | 15.15 | ~12.1 (-20%) |
| ep3 hub | 5.98 | 4.29 | 8.82 | **6.89 (-22%)** | 13.98 | |
| ep5 street | 5.57 | 4.33 | 9.16 | **6.59 (-28%)** | 14.44 | ~11.0 (-23%) |
| ep8 boss | 4.70 | 4.75 | 7.03 | **6.92 (-2%)** | 10.97 | ~8.8 |
| ep7 boss | 8.18 | 7.05 | 11.30 | **9.19 (-19%)** | 18.00 | ~17.0 |

(*) measured while another program used ~23% of the GPU: the relative change is reliable, the ms are not. 3x 16:9 = 4 x 6
samples per native pixel (2048 x 1344), 4x = 5 x 8 (2560 x 1792); **render scale 100% at 1440p is 4x** (main 4e2158e).

Before the change, the depth-tested FF triangles shaded 13.9M fragments per VBlank at 3x in the ep1 hub, of which 5.4M passed
the depth test: exact integer depth is written through SV_Depth, which disables early depth rejection. What is left after it
(per VBlank, 3x, ep1): FF triangles 1.5-1.7 ms, ALIAS16 (CT16 channel shuffles: gather + ROV draws + scatter) 1.3-1.6 ms
in every scene, FF sprites 0.4-0.8, lines 0.25-0.5, the ROV tier 0.2-2.4 (ep7 boss), clears 0.2-0.3, compose / present
0.15-0.3. Cost per shaded fragment is high: at 4x the ep1 hub shades ~46M fragments per game frame in ~12 ms, i.e. roughly
2-3 thousand GPU lane-cycles each - the exact ubershader (integer stepping, int64 Z, software texel fetch from swizzled
memory, uniform branches on the state record) is an order of magnitude heavier than a typical game pixel shader.

### 1.5 VU1 input: what is static, what depends on what

PS2_CHAIN_STATS (runtime/chain_dump.c, scripts/chain_stats.py) hashes every DMA block of a frame's VIF1 chain:
- ep3 hub, Sly walking, camera moving: 1.2 MB of UNPACK data + 272 KB of PATH2 per frame, 1782 MSCALs; **91.4% of the
  unpack bytes are identical (same address, same content) in the next frame**, 1.9% the same content elsewhere, 6.7% new.
- Safehouse views (2.5-2.9 MB unpack, ~4000 MSCALs): 97% identical.
- The camera is a few quadwords (pc-fps2); static objects' world matrices and all mesh data repeat - but every VU1 program
  reads the camera, so every *output* changes: caching saves uploads (VIF, ~20% of the worker), not VU1 work or anything after.

VU1 program dependencies (vu_replay VU_REPLAY_LIVEIN=1: inherited registers scrambled, outputs compared): **98.9% of programs
(91% of pairs) depend only on VU memory**; 1.1% (mostly 188054331 at 0x07F0 after its 0x0738 set-up) use registers left by
earlier programs. So VU1 work can run program-parallel on CPU threads given each program's memory (constants + input buffer).

## 2. What the best projects do (research)

- **RT64 / Zelda64Recomp**: high-level translation of display lists; the RSP's vertex work runs in GPU compute by *HLE of known
  semantics*, not microcode translation; ubershaders against stutter; frame interpolation by matching draw calls (game patches
  tag matrices). Cheap there because a few known N64 microcodes do all transforms. pc-fps2 measured interpolation here: lost
  (each in-between picture costs a whole frame of VU1 / GS / frontend work).
- **Unleashed Recompiled**: draw calls translated (no GPU emulation), shaders recompiled ahead of time, pipelines compiled at
  load; a real higher tick with patches; "modern CPUs usually single-thread bound before the GPU".
- **PCSX2**: MTVU (VU1 on its own thread, VIF commands in an ordered ring, placeholder PATH1 packets; a speedhack because of
  PATH1 SIGNAL/FINISH), MTGS; at max blend accuracy a barrier per primitive kills GPU throughput. No VU on the GPU.
- **paraLLEl-GS**: setup, binning, rasterisation, blending in compute; the CPU keeps geometry and hazard tracking and **is the
  bottleneck** (SotC CPU-bound at 8x SSAA). paraLLEl-RDP keeps the RSP on a CPU dynarec.
- **Dolphin**: hybrid ubershaders (specialised shaders compiled in the background replace the ubershader; exclusive
  ubershaders "incredibly demanding" at high resolution); SSE vertex loader 35 -> 100 fps.
- **Xenia**: shader depth writes / ROV cost "severe", lose early-Z and Hi-Z; conservative SV_DepthGreaterEqual keeps coarse
  early testing; their render-target path beat ROV 1.3-3.4x.
- **RPCS3**: moving vertex decode from CPU to GPU shaders was a big win (CPU prep up to 200 ms per frame); MT RSX 5-10%.
- **No precedent for running console vector-unit microcode itself on a GPU**; the successes are HLE of known semantics.
- **APIs**: D3D11 has indirect draws (+ NVAPI multi-draw indirect); D3D12 / Vulkan cut per-draw CPU cost 5-10x, which matters
  at tens of thousands of draws; here ~4k draws cost the render thread 0.6-1.5 ms on its own thread.
- **NVIDIA**: Reflex works through the public NVAPI (MIT; NvAPI_D3D_SetSleepMode / Sleep / SetLatencyMarker take a D3D11
  device) - latency, not fps. DLSS SR needs motion vectors (the GS has none); DLSS Frame Generation requires D3D12 and motion
  vectors; the DLSS licence requires notifying NVIDIA and attribution. **Smooth Motion** (driver frame generation, NVIDIA app)
  supports RTX 40 and D3D11 games per game without integration, adding latency. Low Latency Mode (D3D11) = driver queue
  control, superseded by Reflex. G-SYNC: VRR + V-Sync + a cap >= 3 fps below refresh.
- **Windows**: flip model + waitable object + ALLOW_TEARING (used); high-resolution waitable timers (used); on hybrid CPUs Intel
  recommends CPU sets by EfficiencyClass instead of affinity, HighQoS (EcoQoS opt-out: done), SMT siblings last.

## 3. Prototypes and experiments

### 3.1 Exact depth with early rejection - built, verified, measured

`scripts/earlyz_probe.c` (headless D3D11 probe on the 4060 Ti, D32S8 like the renderer, hidden quads behind an occluder,
GPU time hidden / visible): SV_Depth 1.000 (no rejection, whatever the occluder wrote); **SV_DepthLessEqual + GREATER /
GEQUAL 0.002**; SV_DepthGreaterEqual + LESS 0.002; wrong-direction pairs 1.0. But with all depth values inside 0.25..0.254 the
GREATER-direction rejection was lost (ratio 1.0; at 0.05 base 0.69): NVIDIA's coarse culling needs the values spread over
the range. M9's "conservative depth gives nothing" came from that (the old encoding asfloat(z + 0x00800000) puts Sly's Z near
1e-38, and its bound was a constant below every depth).

The change (runtime/gpu: gs.hlsl, int64.hlsli, gpu_exec.c, gpu_front.c; ~25 lines):
- depth encoding `z / 2^20` (exact and linear for z < 2^20; the whole corpus has Z32 <= 0x7F5C3; larger Z32 takes the
  existing z_high / zpatch path) instead of `asfloat(z + 0x00800000)`;
- the FF triangle vertex shader gives the rasterizer an upper bound of the exact Z (the vertex's Z + 64, from the vertex
  record's spare word, already filled); sprites and line pixels their exact constant Z;
- the FF pixel shaders write the exact integer Z as `SV_DepthLessEqual` (position `noperspective centroid`): the exact test
  still decides every tie (outlines), but fragments whose bound already fails are never shaded.

Results: **m9_exact over all 28 corpus recordings at S=1 and S=3, every sample of every target every 7 VBlanks: identical**;
depth-tested FF fragments shaded per VBlank (ep1 hub 3x) 13.85M -> 5.98M (5.42M pass); GPU per game frame in section 1.4.
gs-hw unit tests: the depth conformance kernels (gpu_conf.c / conformance.hlsl) still test the old encoding up to 0x3F000000
and fail (hardware and WARP) - to be rewritten for the new range in P1; the other checks pass.

### 3.2 The riskiest assumption of Phase 3 (VU1 on the GPU) - measured instead of built

A microprogram -> HLSL translation was not built, because measurements decide it without one:
1. **VU1 is the smallest CPU stage** (~3 ms of the worker's 7 in the ep1 hub; worker, GS and frontend are 7-9 ms each in
   game). Its output (GIF packets) feeds the sequential GS decoder and frontend on the CPU: on the GPU it needs a read-back per
   frame (a sync point and latency) or the decoder and frontend must move to compute too - a GS emulator in compute (design 2
   of d3d11-renderer.md, rejected; paraLLEl-GS shows the CPU side then still limits).
2. **Exactness**: the VU truncates (round toward zero, flush to zero, +-FLT_MAX clamps); D3D11 shader arithmetic rounds to
   nearest with no rounding-mode control, so bit-exact VU results need every FMAC operation emulated (e.g. in doubles: 1/64
   rate on GeForce). The recompiled C is exact today and gated by PS2_VU_VERIFY.
3. **The parallelism is available on the CPU**: 98.9% of programs depend on VU memory only (1.5), so a thread pool can run
   them program-parallel - no GPU, no read-back - and the EE-visible VU1 effects (the VIF1 handler 0x167F78 reading VU1
   results, XGKICK order) stay where they are.
4. **Static input caching** pays only for uploads: 91-97% of the input repeats, but the camera changes every output.

## 4. Options compared

Owner's PC, ep1 hub (the heaviest) unless noted. Expected fps = 1000 / the slowest stage after the change, from section 1
(in-game costs; offline ratios applied). 3x = render scale 75% at 1440p, 4x = 100%.

| option | ep1 hub 3x | ep1 hub 4x | ep1 hub 2x | effort | risk to correctness | other PCs |
|---|---|---|---|---|---|---|
| today | 87 | ~65 | 97 | - | - | - |
| A. Phase 2 only (frontend split) | ~90 (GPU-bound) | ~65 | ~125 (GS thread next) | 2-3 weeks | low (stream hashes) | helps >= 6 cores |
| B. Phase 3 as proposed | <= 90 unless GS decode + frontend also go to compute; then GPU +2-4 ms | lower | ~100-130 | months | high (VU float semantics, EE-visible VU1 effects, GS state on GPU) | weak GPUs lose |
| C. Exact depth with early rejection (3.1, built) | ~110 (frontend-bound) | ~80 | 97 | days | none measured (bit-identical) | helps every GPU |
| D. GPU pass costs: specialised FF shaders, ALIAS16 / sprite / line passes, native sample grid | GPU 7.7 -> ~5 ms in game | GPU ~12 -> ~7 ms | - | 2-4 weeks | low (m9_exact) | helps every GPU |
| E. GS decode fast path (bulk PACKED vertex streams) | | | GS 7.8 -> ~4.5 | 1-2 weeks | low (VRAM / stream hashes) | helps all |
| F. Worker split (VU1 program pool; VIF beside it) | | | worker 7.2 -> ~4 | 2-3 weeks | medium (VU verify, GIF check exist) | helps >= 6 cores |
| G. In-game overhead (1.5-2x over offline) | all CPU stages -20..-40% | | | 1-2 weeks | none | helps all |
| **H. = C + D + A + E + F + G (recommended)** | **~200** | **~140** | **200+** | ~2-3 months | low, each step gated | scales with cores and GPU |
| I. HLE renderer (hook the render list, meshes cached on the GPU, the VU1 renderers re-implemented in HLSL) | 300+ (GPU) | GPU-bound | 300+ | many months | high: not the PS2's arithmetic; every effect path redone | very good anywhere |
| J. D3D12 / Vulkan port | +0 (render thread 0.6-1.5 ms) | +0 | +0 | 1-2 months | medium | - |
| K. Frame interpolation (RT64 style) | measured in pc-fps2: fewer real frames (30-40 in ep1) | | | | | |
| L. DLSS SR / FG in the game | not possible without motion vectors; FG needs D3D12 | | | | | NVIDIA only |
| M. NVIDIA Smooth Motion (driver, user's choice) | ~2x shown frames, + latency, HUD artefacts | | | none | outside the game | RTX 40/50 |

Lighter scenes (ep3 / ep5 hubs, m8 boss) are 20-40% cheaper in every stage: with H they reach 200 at 4x as well.

## 5. Recommended plan (each milestone measurable like M0-M11)

Gates for every step: tests\run_tests.py; tests/unit (gs-hw, gs-front, gs-draw, window, ...); gpu_diff_all mixed/ref S=1/3;
gpu_fthread_check (stream hashes identical); m9_exact (bit-identical where the step claims it); gs_vram_hashes -Backend hwdry;
the in-game verifier sweep (m8_batch -Verify 60). Frame rates: fps2_batch on the four scenes of 1.1 **with no other game
running** (the CPU stages grow 1.3-2x under another agent's load) and m9_bench on a quiet GPU.

| # | milestone | done when |
|---|---|---|
| P1 | **Exact depth with early rejection** (3.1 -> product): conformance kernels for the z / 2^20 encoding, a switch `PS2_GPU_EARLYZ=0`, z_high counted | m9_exact 28/28 identical S=1/3 (done for the prototype); gs-hw passes; m9_bench 3x GPU per game frame ep1 <= 6.6, ep3 <= 7.0, ep5 <= 6.8 (quiet); in game ep1 3x >= 105 fps |
| P2 | **GPU pass costs**: (a) specialised FF pixel shaders per texture class / TFX / fog / blend (build-time permutations, ubershader kept as fallback - Dolphin / Unleashed style), measured per class with PS2_GPU_PROF; (b) ALIAS16 1:1 shuffle sprites as a direct word permutation instead of gather + ROV draws + scatter; (c) bounds for thin lines and conservative depth for ROV-tier depth reads; (d) a native-resolution sample grid for render scale 100% (5 x 6 samples at 1440p instead of 5 x 8: -25% samples) | replay GPU per game frame at 3x <= 5.0 ms and at 4x <= 7.5 ms in ep1 / ep3 / ep5 (quiet GPU); exact (m9_exact) except (d), which is a new grid validated by gpu_diff_all / sample gates |
| P3 | **Frontend split (Phase 2)**: fr_prim = a pure "prepare" step (setup, ranges, texel rect, tier, records, vertices) on a small pool ahead of the sequential "commit" (state dedup, ownership, merge, appends); a cheaper commit (per-state batches). First a go/no-go prototype on gs_replay (offline frontend ms) | gpu_fthread_check identical; offline frontend <= 3.5 ms, in game <= 4.5 ms per frame in ep1 (from 8.9) |
| P4 | **GS decode fast path**: GIF PACKED loops of (ST, RGBAQ, XYZ2) decoded straight into vertex batches without per-register dispatch; proxy records in bulk | gs_vram_hashes hwdry identical; in-game GS thread <= 4.5 ms per frame in ep1 (from 7.8) |
| P5 | **Worker split**: memory-only VU1 programs (98.9%) on a pool with per-program memory snapshots (constants + input buffer), register-dependent ones chained, XGKICK packets spliced back in program order; next VIF unpack beside them | PS2_VU_VERIFY / PS2_GIF_CHECK / PS2_VIF_UPK_FAST=check 0 differ; worker <= 4.5 ms per frame in ep1 (from 7.2) |
| P6 | **In-game overhead**: why the stages cost 1.5-2x offline (ring sizes and batching, cache-line sharing, pools on P-cores via CPU sets by EfficiencyClass, SMT siblings last, audio / IOP / logging free to use E-cores) | in-game / offline <= 1.25 for GS and frontend; ep1 hub 2x >= 200 fps, 3x >= 190 |
| P7 | **Latency and pacing**: optional NVIDIA Reflex through nvapi_QueryInterface (SetSleepMode low latency + Sleep + markers; ids from the public NVAPI interface list, as gpu_vrr.c does), default limit = refresh - 3 with VRR, frame-time validation from DXGI statistics / PresentMon | frame-time p99 / median <= 1.3 in the 35 s scenes; with Reflex, lower present latency (markers) when GPU-bound |

### P1 results (branch next/perf-p1, 2026-10-23; design, exactness proof per Z format and tests: d3d11-renderer.md appendix P)

Done: the z / 2^20 encoding everywhere (DSV, clears, ps_zresolve, cs_zsync with a saturation sentinel for stored values
>= 2^20, downloads / read-backs through zpatch), conservative SV_DepthLessEqual on every FF class (triangles, sprites,
lines, points, thin outlines, HUD), a proven per-triangle bound decision (vertex bound / no bound / ROV) in the frontend,
Z24 and Z32 above the FF range on the old zpatch path, `PS2_GPU_EARLYZ=0`, the conformance kernels rewritten (round trip,
ties against true stored values, zsync for Z16/Z24/Z32, the triangle bound on 28672 triangles: min slack 127 of 128 on the
RTX 4060 Ti and WARP).

GPU ms per game frame (m9_bench, 16:9, interleaved medians, GPU idle but for another agent's paused game) and PS invocations
per game frame:

| recording | 3x before -> after | 4x before -> after | PS 3x | PS 4x |
|---|---|---|---|---|
| ep1 hub | 9.48 -> **6.41** | 15.03 -> **9.87** | 47.2 -> 30.1 M | 78.6 -> 50.1 M |
| ep3 hub | 8.84 -> **6.90** | 13.94 -> **10.80** | 42.3 -> 30.3 M | 70.5 -> 50.4 M |
| ep5 street | 8.98 -> **6.57** | 14.27 -> **10.30** | 46.2 -> 32.9 M | 77.0 -> 54.6 M |
| ep8 boss | 7.00 -> **6.93** | 10.72 -> **10.58** | 31.3 -> 30.6 M | 52.1 -> 50.9 M |
| ep7 boss | 11.02 -> **9.04** | 17.99 -> **14.54** | 73.4 -> 61.5 M | 122.4 -> 102.1 M |

P1 targets met: 3x ep1 6.41 <= 6.6, ep3 6.90 <= 7.0, ep5 6.57 <= 6.8; m9_exact 28/28 identical at S = 1 and 3 (and --thin
at 3 and 4); gs-hw passes on hardware and WARP. (The in-game "ep1 3x >= 105 fps" check needs the CPU stages too: not
re-run here - pc-fps2's batch with no other game running.) Left for P2: early-depth bounds give nothing on ep8 boss
(already ~1.05 shaded per passing fragment); what remains there and everywhere is the per-fragment cost.

### P3 results (branch next/perf-p3, 2026-10-24; as built: d3d11-renderer.md appendix S, history notes/wip/perf-p3.md)

**Chaos benchmark** (shared with P2): `scripts\chaos_bench.ps1` plays the scenes of `tests/gpu/chaos.txt` - m1_brawl
(Murray's brawl), m2_boardroom (fight, fire), m3_boss (Rajan, rain), m5_boss (Contessa, rain), m8_boss (Clock-La debris
fall), ep1 / ep3 / ep5 hubs - from warp cards with time-based scripted input, release build, headless, frame_rate
unlimited, 3x 16:9, and `scripts\chaos_sum.py` reports over each scene's window the median fps, the **1% / 0.1% lows
(99th / 99.9th percentile frame time)** and per-stage ms (worker, GS thread, frontend, render, GPU; medians and p90 of
the 60-VBlank stats windows). `-Scout` dumps a picture every 4 s (contact sheets: chaos_sheet.ps1), `-Record` makes GS
recordings of the scenes (build\gsrec\chaos_*.gsrec, 240 VBlanks each) for gs_replay; builds are interleaved per scene
(`-Exes "a=exe,b=exe|ENV=V"`) and `--table` gives medians over repeats. Offline: `scripts\front_bench.ps1 -Hold`
(PS2_GPU_FTHREAD_HOLD + a 2 GB ring: the frontend thread starts after the whole stream is decoded, so its CPU is its own -
+-3% between runs instead of +-10%), `scripts\gpu_stream_ab.ps1` (command-stream hash of a build against a baseline exe:
same thread / frontend thread, S=1 / S=3, mixed / plain, corpus + chaos).

**The parallel prepare is a no-go** (measured, two designs, offline and in game; both bit-identical):
- v1 (commit 25e6f78): fr_prim split into a pure gf_prepare (setup, culling, ranges, texel rectangle, early-depth
  decision, FF / sprite records) and the commit; the frontend thread cuts the ring into 128-primitive segments with a
  state snapshot, helpers prepare them. Hold-mode ms per game frame (base / split / 3 helpers): ep8 boss 6.87 / 7.37 /
  7.44, ep1 hub 6.06 / 6.38 / 6.37, m1 brawl 3.51 / 3.85 / 4.04. The prepared part is only 23-27% of fr_prim (the
  section profile's "55% pure" counted work that needs the sequential state), cutting segments cost ~15% (a second walk
  of the ring), reading 300-byte preparations written on other cores ~10%, and the split itself 5-7%.
- v2 (commit b62522a): segments cut by the GS thread as it writes them (no second walk), preparations prefetched ahead.
  Offline: the frontend thread -0..-13%, but the GS thread +15-30% (helpers reading freshly streamed ring lines, clocks);
  in game (ep1 hub / m5 boss / m1 brawl, 2 repeats each): the frontend stage itself not lower (6.12 -> 6.50, 4.77 -> 5.26,
  4.21 -> 5.03 ms), the GS thread higher (7.9 -> 9.6 ms in ep1). Per-primitive work is ~180 ns spread over many small
  steps whose results are as large as their inputs: moving any of it to another core costs about what it saves.

**What was kept: exact sequential work on the frontend** (stream hashes identical to main on the corpus and the chaos
recordings, all modes; m9_exact identical):
- the exact-ATST-split bitmap (sb_prim, 12% of the frontend in alpha-tested scenes): triangle rows by exact integer
  quotient/remainder stepping instead of a double estimate + correction loops per edge per row (PS2_GPU_SB_CHECK=1
  compares with gs_tri_row: 5.5M rows, 0 differ), and a test whose rectangle holds no set bit returns at once;
- own_draw's clean fast path per target (a change of a block takes only the targets whose pages hold it off it, not every
  target; gpu_own.c touch / page_tg, PS2_GPU_OWN_TGEN=0 = the old global generation); scan_clean also requires the
  block not shared (xcopied) - without that ep2_photo and m6_rc_feedback differed;
- the CLUT ring's content hash kept per 16-byte piece, updated from the pieces a load changed (gpu_clut.c; the CLUT part
  of the state lookups 167 -> 90 Mcycles on chaos_ep1_hub);
- FF record planes of three equal vertex values taken as the constant row (bit-identical: proven in the code comment);
- texel rectangles without libm floor / ceil / fmin calls, the vertex cache compare inline, block divisions as shifts,
  the ring prefetched 4 KB ahead on the frontend thread (PS2_GPU_RING_PREFETCH; the producer streams past the caches).

Offline frontend thread, hold mode, S=3, ms per game frame (median of 3 interleaved runs; base -> new): ep8 boss 6.58 ->
6.30, ep1 hub 5.98 -> 5.47, ep5 street 8.01 -> 7.55, ep3 hub 7.31 -> 6.99, chaos_ep1_hub 9.03 -> 8.44, chaos_ep5_hub 7.41
-> 6.98, chaos_m1_brawl 5.30 -> 4.98, chaos_m3_boss 8.13 -> 7.89, chaos_m5_boss 5.77 -> 5.53, chaos_m8_boss 5.75 -> 5.63
(-3..-8%; p95 alike). The P3 target (offline <= 3.5 ms in the ep1 hub) is **not met**; see "what limits" below.

Order: P1 first (days; the biggest single gain; independent). P2 and P3 in parallel (shaders / executor vs gpu_front.c). Then
P4, P5, P6. P7 any time (render thread). Re-measure after each; only if 200 at 100% render scale in the heaviest hub is
still required after P1-P6, start option I (HLE renderer) as research.

Expected ep1-hub frame rate along the way (3x / 4x): today 87 / ~65; P1 ~110 / ~80; + P3 ~125 / ~80; + P4 ~135; + P5
~150-160 (GPU-bound); + P2 ~200 / ~140; + P6 margin (190-210 / 140-150). 2x: 200+ after P3-P6.

### NVIDIA / driver features: adopt or skip

| feature | decision | why |
|---|---|---|
| Reflex (NVAPI sleep mode + markers) | adopt (P7), optional, NVIDIA only | lower latency when GPU-bound; no SDK download (public NVAPI interface ids, as gpu_vrr.c) |
| G-SYNC / VRR | keep (supported) | VRR is off for the owner's display in the driver (frame-rate.md): turning it on is the biggest smoothness win at a varying 100-200 fps |
| DLSS Super Resolution / DLAA | skip | needs motion vectors; the renderer already supersamples exactly |
| DLSS Frame Generation | skip | D3D12 + motion vectors required; licence / attribution |
| Smooth Motion (driver frame generation) | document as a user option, not a default | works on RTX 40 + D3D11 without integration; doubles shown frames; adds latency; can smear the HUD |
| Low Latency Mode | superseded by Reflex | |
| Threaded optimisation / shader cache | leave to the driver | every shader is created at start; the render thread is not the limit |
| App-written driver profile (NVAPI DRS) | skip | not needed; driver settings are the player's |

## 6. Why not the coordinator's Phase 3 (summary)

It speeds up the smallest stage (VU1 ~3 ms; frontend, GS decode and worker are 7-9 ms each in game, the GPU 9-11 ms at 3x),
adds GPU work where the GPU is the limit, needs a per-frame read-back or a compute-shader GS emulator downstream, cannot be
bit-exact with D3D11 float semantics, and has no precedent - the projects it is modelled on run *known semantics* (HLE) on the
GPU. Its good observation - static geometry could live on the GPU - is real (91-97% of VU1 input repeats), but it only pays
inside option I (an HLE renderer), a different and much larger project.

## 7. Sources

RT64 https://github.com/rt64/rt64 , extended GBI https://raw.githubusercontent.com/rt64/rt64/main/include/rt64_extended_gbi.h ;
Zelda64Recomp https://github.com/Zelda64Recomp/Zelda64Recomp ; Unleashed Recompiled
https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md , XenosRecomp
https://cdn.jsdelivr.net/gh/hedge-dev/XenosRecomp@main/README.md ; PCSX2 MTVU https://pcsx2.net/blog/2011/threading-vu1 , MTGS
https://pcsx2.net/blog/2010/benchmarking-multithreaded-pcsx2 , GSdx https://wiki.pcsx2.net/GSDX ; paraLLEl-GS
https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation ;
paraLLEl-RDP https://www.libretro.com/index.php/parallel-rdp-how-the-upscaled-rendering-works/ ; Dolphin
https://forums.dolphin-emu.org/Thread-exclusive-vs-hybrid-ubershaders , https://forums.dolphin-emu.org/Thread-attention-developers?pid=351404 ;
Xenia https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html ; RPCS3
https://blog.rpcs3.net/2017/08/ , https://github.com/RPCS3/rpcs3/pull/6112 ; NVAPI https://github.com/NVIDIA/nvapi ,
https://docs.nvidia.com/nvapi/group__dx.html ; Streamline / DLSS FG https://developer.nvidia.com/rtx/streamline/get-started ;
DLSS licence https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt ; Smooth Motion
https://www.nvidia.com/en-us/geforce/news/nvidia-app-global-dlss-overrides-rtx-40-series-smooth-motion/ ; G-SYNC 101
https://www.blurbusters.com/gsync/gsync101-input-lag-tests-and-settings/14/ ; DXGI flip model
https://devblogs.microsoft.com/directx/dxgi-flip-model/ ; Intel hybrid guide
https://www.intel.com/content/www/us/en/developer/articles/guide/12th-gen-intel-core-processor-gamedev-guide.html .
(Fetched 2026-10-23; dolphin-emu.org and rpcs3.net refused the fetcher, so those points come from forum posts / snippets.)

## 8. Tools added

`scripts/earlyz_probe.c` (early-depth behaviour of a GPU: `earlyz_probe <quads> <iterations> [d32s8] [span] [base]`),
`scripts/gpu_prof_ab.ps1` (GPU phases and fragment counts of gs_replay builds), `PS2_CHAIN_STATS` in runtime/chain_dump.c +
`scripts/chain_stats.py` (VU1 input reuse between frames), `VU_REPLAY_LIVEIN=1` in scripts/vu_replay.c (programs that depend
on inherited VU registers).
