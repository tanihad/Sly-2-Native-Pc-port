# WIP: gpu-M9 (performance)

**Goal**: milestone M9 of notes/d3d11-renderer.md section 11 on branch gpu/M9 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M9, from next/gpu 829070e): bring the GPU renderer at 3x 16:9 to
GS <= 6 ms, render <= 8 ms, GPU <= 8 ms, frontend <= 8 ms (medians) on ep8 boss / ep3 hub / ep5 street / ep1 hub /
m7 boss / m3 heist; GPU <= 12 ms at S=4/5; frames in flight 2-3 (adaptive), memory waste cut; every optimisation
bit-exact vs the un-optimised hw output (lockstep hw vs hw at S=1 and S=3).

**Status**: complete (appendix L of notes/d3d11-renderer.md has the results; gates green on the final source).

## Tools (built)
- `scripts/m9_bench.ps1 -Exe <gs_replay> [-Runs 3] [-Only ..] [-Prof] [-EnvVars "A=1,B=2"]`: replay GPU ms per game frame
  (mixed, 3x 16:9 default, --bench-skip 10), medians of interleaved runs. Recordings: ep8_boss ep3_hub ep5_street ep1_hub
  ep7_boss (= m7 boss) load_m3 ws_ep3_hud.
- `PS2_GPU_PROF=2`: per-category PS invocations / samples passing depth / primitives (pipeline statistics + occlusion).
- `scripts/m9_exact.ps1 -Exe <opt> -BaseExe build\basert\gs_replay.exe [-Scales 1,3] [-Game]`: hw-vs-hw exactness gate
  (gs_replay --sample-hash: hash of every sample of every target every N VBlanks; --game: game-pic-hash of every picture).
  Baseline exe = current scripts/gs_replay.c built against `git archive 829070e runtime` in build\basert\runtime.
- `scripts/gs_replay.ps1 -BuildOnly -Out <dir>`.
- `PS2_GPU_DBG_DRAW` bits (gpu_exec.c, measurements only, wrong pictures).

## Baseline (829070e, replay, 3x 16:9, GPU ms per game frame, median of 3)
ep8_boss 11.60, ep3_hub 12.38, ep5_street 13.06, ep1_hub 13.87, ep7_boss 20.45, load_m3 9.26, ws_ep3_hud 14.75.
Render CPU 1.3-3.7 ms. (In game M8: GPU 8.1-12.1, GS 7-8, frontend up to 10.8.)

## Findings (measured, ep5 street / ep3 hub / ep7 boss, 3x 16:9)
- PS2_GPU_PROF split per VBlank (ep5): FF tri with Z 46%, ROV tri 18%, ALIAS16 19% (gather/scatter + draws), clears 5%,
  sprites 6%, Z resolve/sync 5%.
- FF-tri pixel shader work is NOT the main cost: trivial PS -> 10.0 ms (from 12.4); no texture -> 10.8.
  Texture sampling ~1.1-1.2 ms per game frame; rest of the PS ~0.9.
- Per-draw constants (VS/PSSetConstantBuffers1 per draw) cost ~1.0-1.6 ms per game frame (PS2_GPU_DBG_DRAW=2).
  Blend/depth state changes: ~0. Fixed per-draw cost ~0.15 us.
- **ROV runs (FF <-> UAV switches) are the biggest cost**: skipping ROV draws: ep3 -2.2, ep5 -3.6, ep7 -9.4 ms; with a
  trivial ROV shader nothing changes -> it is the switches (bind, zsync, zresolve, drains), ~20-40 us per run. ROV runs
  come mostly from pass-A primitives with As > 128 (C>128: ep7 22% of pass-A lerp kicks, 90-215 runs per VBlank) and
  shadow volumes. Gaps between consecutive ROV draws: mostly 1 FF draw of < 1000 native px (gaps.py on traces).
- Early Z: 57% of FF Z-tested fragments fail the depth test, but SV_DepthLessEqual (conservative) gets no early-Z on
  this NVIDIA driver (PS invocations unchanged); +4% from it anyway, not pursued yet (needs unique vertices for an exact
  per-triangle bound and a depth encoding away from FLT_MIN: asfloat(z + 0x20000000) works).
- D32S8 -> D32 depth: no speed change, half the depth memory (kept; exact).

## Done
- 46673ef tools; 3e8c202 m9_exact.ps1, --sample-hash, game-pic-hash, D32 depth, trace bbox, DBG_DRAW.
- ROV run absorption (FF gaps between ROV draws executed on ROV): measured WORSE (ep8 boss 11.6 -> 21.4 ms): every ROV
  D3D draw costs ~20-40 us (UAV draws serialise), so more ROV draws = slower. Removed.
- b513aee **source-alpha bound from the palette entries the texels use** (gpu_front.c tex_used_alpha, gpu_own.c
  own_rect_serials, gpu_clut.c clut_entry_alpha): 100% of the corpus' "As > 128" pass-A lerps never use a palette entry
  above 0x80 (measured by scanning their texel rects) -> they take the FF dual tier instead of ROV runs; ATST straddles
  (22-64% of pass-A kicks) mostly become all-pass / all-fail (exact). ROV runs per VBlank: ep7 90.7 -> 1.8, ep5 29 -> 4.6.
  GPU 3x 16:9: ep8 11.6->8.2, ep3 12.4->9.8, ep5 13.1->10.1, ep1 13.9->10.4, ep7 20.4->12.9, load_m3 9.3->8.1,
  ws_ep3 14.7->12.1. Exactness: PS2_GPU_NO_TEX_ALPHA=2 (bound used for ATST outcomes only) is bit-identical to 829070e on
  all 27 recordings at S=1 and S=3 (m9_exact -Every 7); the full mode moves those primitives from ROV (exact) to the FF
  dual-source class (+-1 per blend, the 10.2 tolerance class every other As <= 128 lerp is in): lockstep mixed S=1
  ep7/ep8 boss, ep5, ep3, ep1: 0 beyond, e.g. ep7 +-1 words 102.5M -> 104.0M, +-2 1.01M -> 1.23M, min PSNR 53.34 -> 53.30.
  RECORDED TRADE: exact -> +-1 for these primitives. New exactness baseline for later steps: build\basert2 (b513aee).
- ALIAS16 gather/scatter: a thread per sample, the target as R32_UINT: 0.56 -> 0.25 ms per VBlank (ep5), bit-identical.
- 1b648f0 per-draw constants by draw slot (gs.hlsl load_draw from t9 drawc; slot = StartInstanceLocation -> per-instance
  element DRAWID of an iota vertex buffer, step rate 1; instanced VSs subtract SV_InstanceID) instead of a CB window per
  draw; hardware clears (ClearView of full-mask colour rects, ClearUnorderedAccessViewUint of whole Z32/Z16 zpatch).
  27 recordings bit-identical at S=1 and S=3 vs basert2. GPU: ep8 7.44, ep3 9.20, ep5 9.60, ep1 9.95, ep7 12.27,
  load_m3 7.56, ws_ep3 11.69 ms (3x 16:9).
- bc4883b frontend: split bitmap (bitmap row = hull of 3 triangle rows, one span per row; bbox reject), 2-way 1024-entry
  state cache kept across frames (CLUT ring slot generation in the signature), FF record rows only for varying planes,
  block-wise uploads (whole blocks at once) and CLUT vstale checks, no commutation estimate in real frames.
  Frontend thread per game frame (replay, paced): ep1 7.6 -> 5.6, ep3 10.0 -> 6.9, ep5 9.9 -> 7.5 ms. Bit-identical
  (m9_exact 27/27 S1+S3), gpu_fthread_check -Mixed 27/27.
- a049c17 memory log (PS2_GS_HW_STATS: committed / working set / frame arenas every 10 s and at start), photon
  latency (DXGI frame statistics: queued -> the vsync that showed it) + queue depth in the stats line, adaptive
  in-flight queue (PS2_GS_INFLIGHT_BASE default 2, full depth 6 for 2 s after a slowdown that hit 2 after a calm queue),
  Z clears of the whole drawn size as hardware clears, cs_clear per sample, --sample-hash over drawn rows only,
  CSM1 clut_load offset tables (gs_mem_tex.c, GS thread). m9_exact 27/27 identical S1+S3 (basert2 rebuilt with the
  drawn-rows hash). GPU 3x 16:9: ep8 7.14, ep3 8.88, ep5 9.20, ep1 9.47, ep7 11.64, load_m3 7.10, ws_ep3 11.10.

- gs_vram_hashes: base sw (build\basert2\runtime; basert's runtime no longer builds with the new gs_replay.c) vs current
  hwdry: 27/27 identical (after a049c17).
- b6ffc32 stats lines by a logger thread (GS-thread cost of the stats 0.044 -> 0.002 ms per game frame; PS2_PROFILE=gs
  attributing 8% to hlog was a sampling artefact), memory peak + PS2_GS_MEM_MAP, gs_replay mem-bench.
- 5c72f3c (verified: m9_exact 27/27 S1+S3 identical) **texel reads in loops**: every texel() call was inlined with all
  formats, ~50 copies per pixel shader (tex_sample's 8 level reads x 5 taps + view_hires): ps_ff_* were 500 KB of DXBC
  each and cost 7-11 MB of NVIDIA driver memory each (163 MB for the 85 shaders, measured per CreatePixelShader). Now
  one texel() in a 1-or-4-tap loop inside a 1-or-2-level loop: DXBC 13.0 -> 1.3 MB, driver memory 163 -> 40 MB, shader
  build 7 min -> 10 s. GPU: ep8 7.03, ep3 8.85, ep5 9.19, ep1 9.46, ep7 11.31, load_m3 7.18, ws_ep3 11.15; p90
  -0.6..-1.1 ms. Also: upload buffers recreated at their start size after a 300-fill window under a quarter of capacity.
- Memory findings (in game, ep1): 118 MB before the GPU renderer (= the sw build's runtime), device up 240, frontend +
  rings 332, buffers 372, shaders +148 (now ~+35), first 60 frames up to a 1.7 GB peak, steady 970-1150 MB. WDDM charges
  process commit for every video-memory allocation: a 2048x1536 colour target +12 MB, a depth target +24 MB (D32 +
  zpatch); replay ep1 S1 vs S3: +316 MB. Dynamic-buffer renaming is not the cause in game (caps 8/2/8/1/1/16 MB).
- In-game baseline (829070e exe build\base\sly2_base.exe, headless, verify off, from VBlank 3600, release; scratchpad
  ingame.ps1 polls private bytes every 5 s): m8_boss GS 6.70 front 8.06 render 1.00 GPU 11.39 mem max 1533 MB; ep3_hub
  8.18 / 8.77 / 1.06 / 10.92 / 1582; ep5_hub 7.12 / 9.49 / 1.04 / 12.09 / 1593; ep1_hub 7.47 / 10.98 / 1.29 / 15.08 /
  1609; m7_boss done (build\warp\m8_m7_boss_base, summarise); m3_heist base still to run (paused).
- In-game GS thread ep1 current 6.2 ms (replay 3.7: cross-core queue reads + preemption counted as busy). Profile: px_prim
  30% of busy, vertex 15%, draw_prim 13%, px_tex0 9%, reserve 9%, write_reg 8%, finalize 8%, clut_load 3%.

- dd67e34 memory: upload buffers start at 1 MB (grow to the next power of two, shrink after peaks), the 64 MB data
  ring (software pictures only) committed on first use, at most 3 free frame-arena sets; video-memory numbers in the
  memory log (DXGI QueryVideoMemoryInfo). In game ep1: peak 1.7 GB -> 860 MB, steady ~1.0-1.15 GB -> 856 MB (local
  video 235, non-local 130-280 = driver pools). Replay load_m3 1022 -> 848 MB.
- m3_heist base: GS 2.76 front 3.69 render 0.48 GPU 10.06 mem max 1021; m7_boss base: GS 2.55 front 3.58 render 0.70
  GPU 16.25 draws 3457 mem max 1632.
- FF per-draw overhead (PS2_GPU_DBG_DRAW=8, one primitive per draw): ep1 3.79 / ep5 4.17 ms per game frame with all
  draws vs 2.70 / 3.19 with no FF draw (=4): ~0.28 us per FF draw, ~1.1 ms for 3.9k draws -> commutation batching
  (12% fewer draws, M1 estimate) is worth ~0.15 ms: not done. FF pixel work ~5.7 ms of ep1's 9.5.
- ff-tri-fast-z: 13.9M PS invocations per VBlank, 5.4M pass the depth test (ep1). Conservative depth retried
  (SV_DepthLessEqual, PS position noperspective centroid, VS z = FLT_MIN as a test bound): invocations unchanged
  (14.1M) -> this driver does no early rejection with conservative depth output: the optional variant is dropped.

- In game current (dd67e34 + logger, headless, verify off, from VBlank 3600; base -> current):
  m8_boss GS 6.70->5.18 front 8.06->5.63 render 1.00->0.68 GPU 11.39->8.50 mem 1533->768;
  ep3_hub 8.18->6.07 / 8.77->5.74 / 1.06->0.62 / 10.92->9.02 / 1582->830;
  ep5_hub 7.12->5.41 / 9.49->6.58 / 1.04->0.65 / 12.09->9.21 / 1593->796;
  ep1_hub 7.47->5.67 / 10.98->7.49 / 1.29->0.73 / 15.08->10.55 / 1609->857;
  m7_boss 2.55->1.95 / 3.58->2.66 / 0.70->0.35 / 16.25->10.88 / 1632->776;
  m3_heist 2.76->1.40 / 3.69->1.83 / 0.48->0.29 / 10.06->7.30 / 1021->831.

- 638d100 GPU phases line (PS2_GPU_PROF=1 + PS2_GS_HW_STATS). Windowed ep1: photon (queued -> shown) 14.3 ms median,
  max 17.5; queue 1 frame with adaptive and with fixed depth alike. S=4/5 replay: see appendix L.
- 8e010e9 baseline.json 25 -> 27 (M10's ws recordings); eb42f25 gs-front unit test.
- Gates on the final source: all green (appendix L); in-game verify sweep six scenes 0 beyond, 0 PUNT.

## Next
- Nothing: M9 done. M11 notes in appendix L.
2. Memory: target sizes (alloc granularity, taller allocations kept, shadow copies, a16 staging), frame arenas.
3. GS thread (decode + proxy): clut_load fast path; texture sampling; sprites; ROV shadow volumes (ep7 ~2.5 ms).
4. Gates, appendix L, WIP complete.

## Findings / traps
- DayZ (the owner's game) may be running: check `nvidia-smi --query-gpu=utilization.gpu` before timing runs.
- A gs.hlsl change recompiled every permutation in ~7.5 min before 5c72f3c; now ~10 s.
- PowerShell Set-Content writes a BOM: write sources with [IO.File]::WriteAllText(..., UTF8Encoding($false)).
- SV_DepthLessEqual needs the PS position declared `noperspective centroid` without MSAA (fxc error otherwise).
