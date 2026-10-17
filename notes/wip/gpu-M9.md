# WIP: gpu-M9 (performance)

**Goal**: milestone M9 of notes/d3d11-renderer.md section 11 on branch gpu/M9 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M9, from next/gpu 829070e): bring the GPU renderer at 3x 16:9 to
GS <= 6 ms, render <= 8 ms, GPU <= 8 ms, frontend <= 8 ms (medians) on ep8 boss / ep3 hub / ep5 street / ep1 hub /
m7 boss / m3 heist; GPU <= 12 ms at S=4/5; frames in flight 2-3 (adaptive), memory waste cut; every optimisation
bit-exact vs the un-optimised hw output (lockstep hw vs hw at S=1 and S=3).

**Status**: in progress.

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

## In progress
- Per-draw constants by draw slot (gs.hlsl load_draw from t9 + DRAWID per-instance element via StartInstanceLocation,
  gpu_exec.c iota buffer) instead of a constant-buffer window per draw; hardware clears (ClearView / ClearUAV).
  First try wrong: per-instance step rate 2^30 + StartInstanceLocation gave slot 0 (start applied before the division?);
  now step rate 1 and the instanced VSs subtract SV_InstanceID.

## Next
1. Draw slots + clears: m9_exact vs basert2, bench.
2. Frontend / GS thread profiles (GS_REPLAY_PROFILE_FRONT=1 + --front-thread), downloads pattern, latency/in-flight, memory.
3. Texture sampling (CLUT words, swizzle in ALU / atlas), sprites, remaining ROV (ep7 shadow volumes).
4. Gates, in-game sweep subset, appendix L, WIP complete.

## Findings / traps
- DayZ (the owner's game) may be running: check `nvidia-smi --query-gpu=utilization.gpu` before timing runs.
- A gs.hlsl change recompiles every gs.hlsl permutation: ~7.5 min (16 threads). Batch shader experiments.
- PowerShell Set-Content writes a BOM: write sources with [IO.File]::WriteAllText(..., UTF8Encoding($false)).
- SV_DepthLessEqual needs the PS position declared `noperspective centroid` without MSAA (fxc error otherwise).
