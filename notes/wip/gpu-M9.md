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
- 46673ef tools; (next commit) m9_exact.ps1, --sample-hash, game-pic-hash, D32 depth, trace bbox, DBG_DRAW.

## In progress
- ROV run absorption (executor): FF draws sandwiched between ROV draws on the same targets run on the ROV tier
  (same records: FF fast and ROV fast triangles share the 128-byte layout). Exact for every FF class except the
  dual-source lerp (ROV exact, FF +-1): record + verify with dual absorption off.

## Next
1. ROV absorption; measure; m9_exact.
2. Per-draw constants without CB rebinding (draw id via a per-instance vertex element + StartInstanceLocation).
3. ALIAS16 cost (~2 ms per game frame), clears via ClearView, texture sampling (CLUT words, swizzle in ALU / atlas).
4. Frontend / GS thread profiles (gs_replay --profile), downloads pattern, latency/in-flight, memory.
5. Gates, in-game sweep subset, appendix L, WIP complete.

## Findings / traps
- DayZ (the owner's game) may be running: check `nvidia-smi --query-gpu=utilization.gpu` before timing runs.
- A gs.hlsl change recompiles every gs.hlsl permutation: ~7.5 min (16 threads). Batch shader experiments.
- PowerShell Set-Content writes a BOM: write sources with [IO.File]::WriteAllText(..., UTF8Encoding($false)).
- SV_DepthLessEqual needs the PS position declared `noperspective centroid` without MSAA (fxc error otherwise).
