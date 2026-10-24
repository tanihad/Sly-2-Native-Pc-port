# 200+ fps: where the time goes, what to change, in what order (pc-research)

Owner's goal: Sly 2 at 200+ fps on the owner's PC (i7-14700F, RTX 4060 Ti, 2560 x 1440 165 Hz), smooth, at the resolution the
owner plays at, "regardless how much we up the resolution". Question from the coordinator: is the proposed plan - Phase 2
(split the GPU frontend's per-primitive work across cores), Phase 3 (a GPU-driven renderer: VU1 microprograms translated to
compute shaders, geometry cached on the GPU, primitive setup on the GPU) - the best one? Research (sources at the end and in
notes/wip/pc-research-sources.md), measurements and two prototypes on branch next/pc-research. Checkpoint history:
notes/wip/pc-research.md.

**Short answer.** Not quite. Phase 2 is right but is only one of four CPU stages that each need roughly halving; Phase 3 as
proposed attacks the smallest stage (VU1, ~3 ms of ~25 ms of CPU work per frame), moves work onto the GPU, which is the limit
at 3x, and its outputs are consumed by sequential CPU stages - so it would not raise the frame rate without also turning the
GS decoder and frontend into a compute-shader GS emulator (the design the renderer already rejected). The largest single
win found is on the GPU and is cheap: **exact depth with early rejection** (prototype here: -25..-34% GPU time in the hubs,
bit-identical on the whole corpus). The recommended plan (section 5) is: early depth -> GPU fixed-cost passes -> the
frontend split (Phase 2) -> GS decode fast path -> VU1 / worker split -> close the offline-vs-in-game gap; Reflex-style
latency control and a VRR-friendly limiter alongside; no API port, no DLSS / frame generation in the game.

## 1. Where the time goes (measured)

The pipeline (pc-fps2): EE thread (game code, ~2 ms) -> graphics worker (VIF1 unpack, VU1 microcode, GIF) -> GS thread
(register decode, proxy to the frontend) -> frontend thread (per-primitive exact records, ownership, draws) -> render thread
(D3D11) -> GPU. Every stage works on a different frame, so **fps = 1000 / the slowest stage** (when nothing waits on
anything else). 200 fps needs every stage at <= 5 ms per game frame, in practice <= ~4.5 ms.

### 1.1 In game, per game frame (pc-fps2 measurements on a quiet machine, release, frame_rate 165)

| scene | fps 3x / 2x | worker | GS thread | frontend | render CPU | GPU 2x | GPU 3x |
|---|---|---|---|---|---|---|---|
| ep1 hub (heaviest) | 87 / 97 | 7.2 | 7.8 | 8.9 | ~0.7 | 6.6 | 11.4 |
| ep3 hub | 98.5 / 160 | 5.4 | 3.5 | 4.1 | ~0.6 | 5.2 | 9.6 |
| ep5 hub | 102 / 124 | 5.5 | 5.6 | 7.4 | ~0.7 | 5.5 | 9.5 |
| m8 boss | 127 / 145 | 4.5 | 4.7 | 5.8 | ~0.7 | 4.8 | 7.6 |

Worker split (ep1): VU1 microcode ~40%, VIF unpack ~20%, GIF + GS queue the rest. 1x / 3x runs made for this study (fps2_batch,
unlimited) ran while another agent's game instances kept the CPU at 74-87%: ep1 hub 1x 64-79 fps with GS thread 10-12 ms and
frontend 11-13 ms per frame, i.e. the CPU stages grow ~1.4x under contention while the GPU (1x) is idle half the time. They
confirm the order of the stages, not absolute numbers (raw lines: build\warp\fps2_*.txt of the worktree; not committed).

### 1.2 Offline, each stage alone on a pinned P-core (repeatable)

| recording (corpus) | GS decode ms / game frame | frontend ms / game frame | primitives / game frame |
|---|---|---|---|
| ep1 hub | 3.7-4.2 | 5.8-5.9 | ~32k |
| ep3 hub | 5.2-5.5 | 6.9-8.2 | |
| ep5 street | 4.8-5.2 | 8.3-8.4 | ~41k |
| ep8 boss | 5.4-5.5 | 7.3-7.6 | ~39k |
| ep7 boss | 2.7-2.8 | 4.5 | |

(gs_replay --backend hwsubmit --mixed --front-thread --pin, 16:9, S=1 and S=3: the CPU stages do not depend on the scale.)
VU1 (vu_replay of 9000 recorded programs, recompiled microcode, pinned): **1.55 ns per pair**, ~2.3M pairs and 4000-7000
programs per game frame -> ~3.6 ms per frame alone; 0 programs differ from the interpreter.

So the in-game stages cost 1.5-2x their offline time (GS 3.7 -> 7.8, frontend 5.9 -> 8.9 in ep1): threads handing data to each
other through rings on other cores, cache traffic, SMT siblings and preemption all count as busy time (pc-fps2 tried priority,
P-core-only CPU sets and pinning without effect). That gap is itself a target (milestone P6).

### 1.3 The frontend: what can run in parallel (section profile, gs_replay -Define GPU_FRONT_PROF, ep1 hub)

| section of fr_prim | Mcycles | parallel? |
|---|---|---|
| state lookup / dedup | 228 | sequential (StateRec ring, but per state change, not per primitive) |
| geometry (setup, culling) | 91 | pure |
| inputs (alpha / Z range) | 85 | pure |
| texture source (texel rect, source lookup) | 181 | mostly pure (lookups of the ownership table) |
| tier + line pixels | 155 | pure given the state |
| ownership (pages, blocks, hazards) | 227 | sequential |
| key / draw merging | 205 | sequential |
| record emission | 267 | pure computation, sequential append |
| triangle vertices (+ vertex cache) | 256 | pure computation, sequential indices |

About 55% pure and 45% sequential -> Amdahl with 4 helper threads: **~1.7x** (ep1 frontend 8.9 -> ~5.2 ms in game). ~180 ns
per primitive today; making the sequential half cheaper matters as much as the split.

### 1.4 The GPU at 3x (replay, PS2_GPU_PROF=2, ep1 hub, per VBlank; the ep1 recording draws a game frame every 2 VBlanks)

Before: GPU 6.0-7.5 ms per VBlank, of which the depth-tested FF triangles (ff-tri-fast-z) 3.0-3.2 ms: **13.9M pixel shader
invocations of which only 5.4M pass the depth test** - the exact integer depth is written through SV_Depth, so the GPU
cannot reject hidden fragments before shading them (appendix L of d3d11-renderer.md measured this). Fixed-cost passes:
ALIAS16 (CT16 channel shuffles) + their draws ~1.3-1.6 ms, FF sprites 0.4-0.8, lines 0.25-0.5, ROV tier 0.2-2.4 (ep7 boss),
clears 0.2-0.3, present 0.15-0.3.

### 1.5 VU1 input: what is static

PS2_CHAIN_STATS (runtime/chain_dump.c, scripts/chain_stats.py) hashes every DMA block of a frame's VIF1 chain:
- ep3 hub, Sly walking, camera moving: 1.2 MB of UNPACK data + 272 KB of PATH2 per frame, 1782 MSCALs; **91.4% of the unpack
  bytes are identical (same address, same content) in the next frame**, 1.9% the same content elsewhere, 6.7% new.
- Safehouse views (2.5-2.9 MB unpack, ~4000 MSCALs): 97% identical.
- The camera is a few quadwords (pc-fps2); static objects' world matrices and all mesh data repeat. But every VU1 program
  reads the camera, so every *output* changes every frame: caching helps uploads (VIF), not the VU1 work or anything after it.

VU1 program dependencies (vu_replay VU_REPLAY_LIVEIN=1: inherited registers scrambled, outputs compared): **98.9% of programs
(91% of pairs) depend only on VU memory**; 1.1% (mostly 188054331 at 0x07F0 after its 0x0738 set-up) use registers left by
earlier programs. So VU1 can run program-parallel on CPU threads given per-program memory (constants + its input buffer).

## 2. What the best projects do (research; URLs in section 7)

- **RT64 / Zelda64Recomp**: high-level translation of display lists; the RSP's vertex work runs on GPU compute by *HLE of known
  semantics*, not microcode translation; ubershaders against stutter; frame interpolation by matching draw calls (game patches
  tag matrices). Works because N64 vertex transforms are a few known microcodes. pc-fps2 measured interpolation here: lost
  (every in-between picture costs a whole frame's VU1/GS/frontend work).
- **Unleashed Recompiled**: draw calls translated (no GPU emulation), shaders recompiled ahead of time, pipelines compiled at
  load; real higher tick with patches; "modern CPUs usually single-thread bound before the GPU".
- **PCSX2**: MTVU (VU1 on its own thread, VIF commands in an ordered ring, placeholder PATH1 packets), MTGS; at max blend
  accuracy a barrier per primitive kills GPU throughput. No VU-on-GPU.
- **paraLLEl-GS**: setup, binning, rasterisation, blending in compute; the CPU keeps geometry and hazard tracking and **is the
  bottleneck** (SotC CPU-bound at 8x SSAA). paraLLEl-RDP keeps the RSP on a CPU dynarec.
- **Dolphin**: hybrid ubershaders; most "GPU-bound" games are limited by EFB copies; SSE vertex loader 35 -> 100 fps.
- **Xenia**: shader depth writes / ROV "severe" cost, lose early-Z and Hi-Z; conservative SV_DepthGreaterEqual keeps coarse
  early testing; their render-target path beat ROV 1.3-3.4x.
- **RPCS3**: moving vertex decode from CPU to GPU shaders was a big win (CPU prep up to 200 ms/frame); MT RSX 5-10%, off by
  default.
- **Nobody runs console vector-unit microcode itself on the GPU** (no precedent found): successes are HLE of known semantics.
- **APIs**: D3D11 has indirect draws (+ NVAPI multi-draw); D3D12/Vulkan cut per-draw CPU cost 5-10x, which matters at tens of
  thousands of draws; here ~4k draws cost the render thread 0.6-1.5 ms on its own thread.
- **NVIDIA**: Reflex works through the public NVAPI (MIT headers; NvAPI_D3D_SetSleepMode / Sleep / SetLatencyMarker take a
  D3D11 device) - latency, not fps. DLSS SR needs motion vectors (the GS has none); DLSS Frame Generation requires D3D12 and
  motion vectors; the DLSS licence requires notifying NVIDIA and attribution. **Smooth Motion** (driver frame generation in the
  NVIDIA app) now supports RTX 40 and D3D11 games, per game, no integration; it adds latency. Low Latency Mode (D3D11) is the
  driver's queue control, superseded by Reflex. G-SYNC: VRR + V-Sync + a cap >= 3 fps below refresh.
- **Windows**: flip model + waitable object + ALLOW_TEARING (already used); high-resolution waitable timers (used); on hybrid
  CPUs Intel recommends CPU sets (EfficiencyClass) instead of affinity, HighQoS (EcoQoS opt-out, already done), SMT siblings
  last.

## 3. Prototypes

### 3.1 Exact depth with early rejection ("conservative exact depth") - built, verified, measured

`scripts/earlyz_probe.c` (headless D3D11 probe, RTX 4060 Ti, D32S8 like the renderer): hidden quads behind an occluder cost
the same as visible ones with SV_Depth (ratio 1.000), but ~0 with **SV_DepthLessEqual + GREATER/GEQUAL** (ratio 0.002) as long
as the depth values are spread over the depth range: with all depths inside 0.25..0.254 (what the old encoding produced for
Sly's 17-19-bit Z) NVIDIA's coarse culling lost the rejection (ratio 1.0). M9's "conservative depth gives nothing" came from
that and from a constant bound below every depth.

The change (runtime/gpu: gs.hlsl, int64.hlsli, gpu_exec.c, gpu_front.c; ~25 lines):
- depth encoding `z / 2^20` (exact and linear for z < 2^20; the whole corpus has Z32 <= 0x7F5C3; larger Z32 takes the existing
  z_high / zpatch path) instead of `asfloat(z + 0x00800000)`;
- the vertex shader of FF triangles gives the rasterizer an upper bound of the exact Z (the vertex's Z + 64, from the vertex
  record's spare word); sprites and line pixels their exact constant Z;
- the FF pixel shaders write the exact integer Z as `SV_DepthLessEqual` (position `noperspective centroid`): the exact test
  still decides every tie (outlines), but fragments whose bound already fails are never shaded.

Results: **m9_exact over all 28 corpus recordings at S=1 and S=3 (every sample of every target every 7 VBlanks): identical**.
GPU per VBlank at 3x 16:9 (gpu_prof_ab.ps1; the GPU was shared with another agent's game at ~40%, so the ms are inflated, the
shader counts are not):

| recording | GPU before | after | pixel shaders before -> after (tri-z) | samples passing |
|---|---|---|---|---|
| ep1 hub | 7.46 | 4.93 (-34%) | 13.85M -> 5.98M | 5.42M |
| ep3 hub | 6.74 | 5.01 (-26%) | 9.86M -> 5.10M | 4.36M |
| ep5 street | 7.26 | 5.11 (-30%) | 10.06M -> 4.62M | 3.94-4.14M |
| ep8 boss | 4.85 | 4.78 (-2%) | 5.53M -> 5.25M | 5.10M |
| ep7 boss | 9.45 | 8.02 (-15%) | 13.23M -> 8.18M | 7.00M |

Still to do before adopting: the gs-hw conformance kernels (gpu_conf.c / conformance.hlsl) test the old encoding; the in-game
verifier sweep; a quiet-GPU m9_bench.

### 3.2 The riskiest assumption of Phase 3 (VU1 on the GPU) - measured instead of built

Translating one microprogram to HLSL was not built, because the measurements above decide it without it:
1. **VU1 is the smallest CPU stage** (~3 ms of the worker's 7 in ep1; worker, GS and frontend are 7-9 ms each in game). Its
   output (GIF packets) feeds the sequential GS decoder and the frontend on the CPU: on the GPU it would need a read-back per
   frame (a sync point and latency) or the decoder and frontend would have to move to compute too - a GS emulator in compute
   (design 2 of d3d11-renderer.md, rejected; paraLLEl-GS shows the CPU side then still limits).
2. **Exactness**: the VU rounds toward zero with flush-to-zero and clamps to +-FLT_MAX; D3D11 shader float arithmetic is
   round-to-nearest with no rounding-mode control, so bit-exact VU results would need every FMAC operation emulated (e.g. in
   doubles, 1/64 rate on GeForce). The recompiled C is exact today and gated by PS2_VU_VERIFY.
3. **Parallelism exists on the CPU too**: 98.9% of programs depend on VU memory only, so a CPU thread pool can run them
   program-parallel (no GPU, no read-back), and the EE-visible VU1 effects (the VIF1 handler 0x167F78 reading VU1 results,
   XGKICK order) stay where they are.
4. **Static input caching** pays only for uploads (VIF ~20% of the worker): 91-97% of the input repeats, but the camera
   changes every output.

## 4. Options compared (owner's PC; heaviest hub = ep1; "native 1440p" = 16:9 display targets with 5 x 6 samples per pixel,
2560 x 1344, ~25% more pixel work than 3x)

Expected fps = 1000 / the slowest stage after the change, from the measurements in section 1 (in-game costs; offline ratios
applied to them). Ranges because the in-game stages vary +-15% with machine load.

| option | ep1 hub 3x | ep1 hub 2x | lighter hubs 3x | effort | risk to correctness | other PCs |
|---|---|---|---|---|---|---|
| today | 87 | 97 | 98-127 | - | - | - |
| A. Phase 2 only (frontend split) | ~90 (GPU-bound) | ~125 (GS thread next) | 100-135 | 2-3 weeks | low (stream hashes) | helps CPUs with >= 6 cores |
| B. Phase 3 as proposed (VU1 -> HLSL compute, GPU geometry cache, GPU setup) | <= 90 unless GS decode + frontend also move to compute; then GPU +2-4 ms | ~100-130 | | months | high (VU float semantics, EE-visible VU1 effects, GS state on GPU) | needs a strong GPU; weak GPUs lose |
| C. Conservative exact depth (prototype 3.1) | ~110 (frontend-bound) | 97 | 120-140 | 2-3 days | none measured (bit-identical) | helps every GPU (early-Z is universal) |
| D. GPU fixed-cost passes (ALIAS16, sprites, lines, present) | GPU 7.6 -> ~5.5 ms | - | | 1-2 weeks | low (m9_exact) | helps every GPU |
| E. GS decode fast path (bulk PACKED vertex streams) | | GS 7.8 -> ~4.5 | | 1-2 weeks | low (VRAM hashes) | helps all |
| F. Worker split (VU1 program-parallel pool; VIF/GIF beside it) | | worker 7.2 -> ~4 | | 2-3 weeks | medium (VU verify, GIF check exist) | helps >= 6 cores |
| G. In-game efficiency (offline-vs-in-game gap 1.5-2x) | all CPU stages -20..-40% | | | 1-2 weeks (investigation) | none | helps all |
| **H. = C + D + A + E + F + G (recommended)** | **~180-200** | **200+** | **200+** | ~2-3 months | low, each step gated | scales with cores and GPU |
| I. HLE renderer (hook the render list, cached meshes + matrices, VU1 renderers re-implemented in HLSL) | 300+ (GPU-bound) | 300+ | | many months | high: not the PS2's arithmetic, every effect path re-done | very good on any PC |
| J. D3D12 / Vulkan port | +0 (render thread is 0.6-1.5 ms) | +0 | | 1-2 months | medium | - |
| K. Frame interpolation (RT64 style) | measured in pc-fps2: fewer real frames (30-40 in ep1) | | | | | |
| L. DLSS SR / DLSS FG in the game | not possible without motion vectors; FG needs D3D12 | | | | | NVIDIA only |
| M. NVIDIA Smooth Motion (driver, user switch) | ~2x shown frames, + latency, HUD artefacts | | | none (user) | none (outside the game) | RTX 40/50 only |

Native 1440p at the heaviest hub: after C + D the GPU needs ~6.5-7 ms per frame -> ~145-155 fps on the 4060 Ti; the lighter
hubs and most levels reach 200. 200 at native 1440p in ep1 needs the GPU work cut by a further ~30% (D's stretch goals) or
the Performance preset (2x) there - the honest limit of an exact renderer on this GPU.

## 5. Recommended plan (each milestone measurable like M0-M11)

Gates for every step: tests\run_tests.py; tests/unit (gs-hw, gs-front, gs-draw, window, ...); gpu_diff_all mixed/ref S=1/3;
gpu_fthread_check (stream hashes identical); m9_exact (bit-identical where the step claims it); gs_vram_hashes -Backend
hwdry; an in-game sweep (m8_batch -Verify 60); fps2_batch on the scenes of section 1 alone on the machine (no other agent's
games: the CPU stages grow 1.4x under contention).

| # | milestone | done when |
|---|---|---|
| P1 | **Exact depth with early rejection** (prototype 3.1 -> product): update gpu_conf / conformance.hlsl to the z / 2^20 encoding, keep a switch (`PS2_GPU_EARLYZ=0`) | m9_exact 28/28 identical S=1/3 (done for the prototype); gs-hw units pass; quiet-GPU m9_bench: ep1/ep3/ep5 GPU per game frame >= 25% lower; in game ep1 3x >= 105 fps |
| P2 | **GPU fixed-cost passes**: ALIAS16 gather/scatter and its draws, FF sprites / lines (bounds for thin lines too), clears, compose / present at 3x; native-1440p sample grid (kx 5, ky 6) as an option | replay GPU per game frame at 3x 16:9 <= 5.5 ms in ep1/ep3/ep5 (quiet GPU), exact by m9_exact |
| P3 | **Frontend split (Phase 2)**: fr_prim = a pure "prepare" step (setup, ranges, texel rect, tier, records, vertices) on a small pool, ahead of the sequential "commit" (state dedup, ownership, merge, appends); plus cheaper commit (per-state batching) | gpu_fthread_check identical; offline frontend <= 3.5 ms, in game <= 4.5 ms per frame in ep1 (from 8.9) |
| P4 | **GS decode fast path**: GIF PACKED loops of (ST, RGBAQ, XYZ2) decoded into vertex batches without per-register dispatch; proxy records in bulk | gs_vram_hashes hwdry identical; in-game GS thread <= 4.5 ms per frame in ep1 (from 7.8) |
| P5 | **Worker split**: VU1 programs that depend on memory only (98.9%) run on a pool with per-program memory snapshots (constants + input buffer), register-dependent ones chained, XGKICK packets spliced back in program order; VIF unpack of the next batch beside them | PS2_VU_VERIFY / PS2_GIF_CHECK / PS2_VIF_UPK_FAST=check 0 differ; worker <= 4.5 ms per frame in ep1 (from 7.2) |
| P6 | **In-game efficiency**: why stages cost 1.5-2x offline (ring sizes, cache-line sharing, pool threads on P-cores via CPU sets with EfficiencyClass, SMT siblings last, audio/IOP/logging allowed on E-cores) | in-game / offline ratio <= 1.25 for GS and frontend; ep1 2x >= 200 fps, 3x >= 180 |
| P7 | **Latency and pacing**: optional NVIDIA Reflex through nvapi_QueryInterface (SetSleepMode low latency + Sleep + markers; ids from the public NVAPI headers, as gpu_vrr.c does), default frame limit = refresh - 3 with VRR, PresentMon / DXGI-statistics validation | frame-time p99 / median <= 1.3 in the 35 s scenes; Reflex on: input-to-present latency (markers) lower at 3x when GPU-bound |

Order: P1 first (days, biggest single gain, independent). P2 and P3 in parallel (different files: shaders/executor vs
gpu_front.c). Then P4, P5, P6. P7 any time (render thread only). After P1-P6 re-measure; only if 200 at native 1440p in the
heaviest hub is still required, open option I (HLE renderer) as research - not before.

### NVIDIA / driver features: adopt or skip

| feature | decision | why |
|---|---|---|
| Reflex (NVAPI sleep mode + markers) | adopt (P7), optional, NVIDIA only | latency when GPU-bound (3x); no SDK download needed (public NVAPI interface ids, as gpu_vrr.c already uses) |
| G-SYNC / VRR | keep (already supported) | the owner's display has VRR off in the driver (frame-rate.md): turning it on is the biggest smoothness win at a varying 100-200 fps |
| DLSS Super Resolution / DLAA | skip | needs motion vectors; the renderer supersamples exactly already |
| DLSS Frame Generation | skip | requires D3D12 + motion vectors; licence/attribution burden |
| Smooth Motion (driver FG) | document as a user option, not a default | works on RTX 40 + D3D11 without integration; doubles shown frames, adds latency, may smear the HUD |
| Low Latency Mode (driver) | superseded by Reflex | |
| Threaded optimisation / shader cache | leave to the driver | the game creates every shader at start (no run-time compiles); render thread is not the limit |
| App-written driver profile (NVAPI DRS) | skip | not needed; touching driver settings is the player's decision |

## 6. Why not the coordinator's Phase 3 (summary)

Phase 3 solves the wrong bottleneck (VU1 ~3 ms; the frontend, GS decode and worker are each 7-9 ms in game, and the GPU is the
limit at 3x), adds GPU work where the GPU is already the limit, needs either a per-frame read-back or a compute-shader GS
emulator downstream, cannot be bit-exact with D3D11 float semantics, and has no precedent: the projects it is modelled on run
*known semantics* (HLE) on the GPU, not microcode. Its good idea - keep static geometry on the GPU - is real (91-97% of VU1
input repeats), but it only pays as part of option I (an HLE renderer), which is a different, much larger project.

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
More, with what each says: notes/wip/pc-research-sources.md.

## 8. Tools added

`scripts/earlyz_probe.c` (early-depth behaviour of a GPU), `scripts/gpu_prof_ab.ps1` (GPU phases and shader counts of gs_replay
builds), `PS2_CHAIN_STATS` in runtime/chain_dump.c + `scripts/chain_stats.py` (VU1 input reuse), `VU_REPLAY_LIVEIN=1` in
scripts/vu_replay.c (programs that depend on inherited VU registers).
