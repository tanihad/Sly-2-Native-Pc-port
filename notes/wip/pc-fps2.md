# pc-fps2: higher real frame rate (pipeline vs frame interpolation)

**Goal**: smooth 120-165 fps; decide by measurement between (a) pipelining / parallelising the per-frame graphics
preparation (VIF1 / VU1 / GIF off the EE thread) and (b) frame interpolation at a 60 Hz tick; implement the winner.

Status: complete (branch next/pc-fps2 from main ff40358). Write-up: notes/frame-rate.md "High frame rate: approaches
compared". Decision: (a) - graphics worker (runtime/gfx_async.c, above 60 fps), raw PATH1 packets (gif_parse.h), VIF
unpack fast paths. (b) stays as an env-only prototype (runtime/interp.c, PS2_INTERP).

## Done
- Profile (all threads), prototypes (a) and (b), measurements at 3x and 2x, 120 / 165, decision, notes.
- Correctness: PS2_GIF_CHECK / PS2_VIF_UPK_FAST=check 0 differ over 16 scenes; PS2_GFX_ASYNC_CHECK found the effect-object
  pool race -> worker only above 60; physics / game speed identical at 60 / 120 / 165 (fps_trace); sw renderer, renderer
  switch, device loss, pause with the worker.
- Gates on the final source: tests\run_tests.py 16/16; every tests\unit suite (scripts\fps2_units.ps1: 24 suites, 0 failed;
  window / warp / menu -UnitOnly, gs-hw -NoWindow); gpu_diff_all mixed S=1 28/28; gpu_game_all S=1 4196 pictures, 0 failed;
  pc_text_boot (60 and PS2_FRAME_RATE=165); debug + --release builds; scripts\package.py (dist\Sly2).
- Experiments without effect (frontend priority, P-core CPU sets, pinning, 2 pictures in flight, 32 MB GS queue): code
  removed, results in frame-rate.md "Left".

## Next (left for later)
- The GPU frontend thread (~9 ms per frame in the ep1 hub at 2x) limits the heaviest hubs to ~97 fps; GPU-bound at 3x.
- RC chopper interpolation (pacing + completeness of the prototype) or a frame-rate-independent chopper controller.

## Baseline (build\base\sly2_base.exe = ff40358 + profiler, headless, alone, 3x, fps2_batch.ps1 -Tag base)
| scene | 120: fps med / p10 | 165 | EE busy ms | GS thr | front | GPU (3x) |
|---|---|---|---|---|---|---|
| ep1 hub | 74.5 / 69.7 | 72.8 / 63.2 | 13.4 (EE 12% GS 33% VU 29% VIF 24%) | 8.2 | 9.2 | 11.5 |
| ep3 hub | 64.0 / 61.2 | 64.8 / 60.3 | 15.6 | 9.5 | 10.6 | 11.7 |
| ep5 hub | 82.8 / 79.5 | 83.0 / 79.4 | 12.1 | 6.9 | 8.2 | 9.7 |
| m8 boss | 103.6 / 94.3 | 100.7 / 94.3 | 9.5 | 5.6 | 5.7 | 8.5 |
EE thread profile (ep1 hub @120): vuc_376110651 (main VU1 renderer) 24% self / 56% incl, gs_write_reg 22%, upk_write 13%,
gif_feed 11%, feed 9.5%; recompiled game code 7%. EE thread 0% idle: the EE is the limit everywhere.
GS thread: gs_thread_main 33% self, px_prim 14%, vertex 7%, draw_prim 6%, reserve 6%, ntdll 11%.
Frontend: fr_prim 34% self (69% incl), ntdll 15%.
Per frame (ep1 hub): ~51k prims, ~2.5M VU pairs, ~7500 VU1 programs.
Per frame DMA (ep1 hub): ch2 normal 9 qw (0x2DEBD0/0x2DECC0), ch1 VIF1 chain (0x35F060 / 0x3070E0 alternating),
ch2 chain (0x2FD980 / 0x2F7980), ch2 chain 0x442C20 (static).

NOTE: the "ep1 hub" above was really the ep1 safehouse (warp_run -Play's +1500 pad frames come 2x early at 120 fps).
fps2_scene.ps1 now presses X at r25 s (wall time); ep1_hub = save of job m1_brawl (briefing done) -> Sly on the roofs.

## Prototype (a) A1+A2 vs base, 3x, 120 / 165 (build\warp\fps2_cmp1.txt; 35 s windows, medians)
| scene | base 120 | a12 120 | base 165 | a12 165 | a12 worker ms | GS thr | front | GPU 3x |
|---|---|---|---|---|---|---|---|---|
| ep1 hub | 68.3 (p10 64.8) | 86.7 (83.9) | 71.6 | 87.0 | 8.9 | 7.4 | 9.2 | 11.4 |
| ep3 hub | 98.0 (67.7) | 100.7 (81.1) | 99.2 | 98.5 (80.5) | 5.5 | 3.7 | 4.0 | 9.6 |
| ep5 hub | 90.4 (79.5) | 101.9 (96.2) | 92.5 | 102.0 | 7.0 | 5.5 | 7.8 | 9.5 |
| m8 boss | 118.0 (95.6) | 118.2 (103.9) | 113.0 | 126.7 | 4.6-5.5 | 4.1-5.3 | 4.8-6.2 | 7.5-8.1 |
| m3 heist | 119.2 | 119.2 | 145.8 | 143.6 | 1.8 | 1.4 | 1.8 | 6.8 |
With A1+A2 the EE thread is ~2 ms per frame (60-80% idle); at 3x the GPU is the limit everywhere (fps ~ 1000 / GPU ms).
PS2_GIF_CHECK: 3.1M raw packets, 0 differ.

## Scale 2 (build\warp\fps2_cmp1s2.txt, fps2_cmp2s2.txt): GPU no longer the limit
| scene | base 120 | a12 120 | a12v 120 | base 165 | a12 165 | a12v 165 | limit after |
|---|---|---|---|---|---|---|---|
| ep1 hub | 69.3 | 96.0 | 96.2 | 69.0 | 96.3 | 97.0 | frontend 9.0 / GS 7.8-9.1 ms (worker 7.2 with VIF fast) |
| ep3 hub | 117.5 (p10 67) | 118.7 (p10 81) | | 120.6 (p10 66) | 159.6 (p10 81) | | |
| ep5 hub | 91.1 | 118.2 | 118.4 | 92.2 | 121.0 | 124.0 | frontend 7.4 |
| m8 boss | 115.0 | 118.7 | 118.7 | 110.8 | 149.7 | 145.1 | frontend 5.8 / GPU 4.8 |
| m3 heist | 119.5 | 119.0 | | 163.6 | 163.5 | | (light: at the cap) |
a12v = a12 + VIF unpack fast paths: worker 8.8 -> 7.2 ms (VIF 34% -> 20%), fps unchanged (frontend / GS thread limit).

## Prototype (b) (runtime/interp.c, PS2_INTERP=k at frame_rate 60)
- Works for camera + rigid objects (world matrix V4-32 x4 to VU1 0x000 in per-frame CNT packets, keyed by the next CALL's
  static geometry address + occurrence; camera = VU1 0x002/0x006 of the frame's first packet). ~800 matrices / frame in
  the ep1 hub, ~97% matched. Dumps (N-1, in-between, N) look right in ep1 hub and m8 boss.
- Traps found: Sly 2 uses the VIF1 i bit (handler 0x167F78 reads VU1 0x320 results, then FBRST.STC) - a re-run chain must
  resume the VIF itself; at 60 the PS2_PERF "flips" count VBlanks (DISPFB changes per field), not game frames; the
  prototype runs the in-between chains on the render thread, so the game's own frame rate drops (ep1 hub 60 -> ~42):
  count distinct pictures ([fps2] interp lines).
- Cost: a re-run of the frame's chains ~5.3-6.8 ms on the worker (= a real frame's job, 6.2-7 ms) + the same GS thread /
  frontend / GPU work per picture.

## (b) measured right (distinct pictures/s, scale 2; build\warp\fps2_cmp3s2.txt) vs (a) a12v
| scene | (a) 120 | (a) 165 | (b) k=1: game / distinct | (b) k=2: game / distinct |
|---|---|---|---|---|
| ep1 hub | 96.2 | 97.0 | 40.3 / 80.7 | 30.0 / 89.9 |
| ep5 hub | 118.4 | 124.0 | 57.9 / 115.8 | 37.3 / 112.0 |
| m8 boss | 118.7 | 145.1 | 59.0 / 117.9 | 58.9 / 176.8 (scene differs: at a 60 tick the GP pad ends with Sly falling; clouds only) |
A re-run costs what a frame costs (same VU1 pairs per job: 470k = 470k); (b) <= (a) where the content is the same, and
the game tick drops below 60 in heavy scenes. DECISION: (a).

## Correctness sweeps (scripts\fps2_check.ps1; 16 scenes at 120 / 165, 4 in parallel)
- PS2_GIF_CHECK and PS2_VIF_UPK_FAST=check: 0 differ in every scene (10-20M packets, 14-40M unpack calls per run).
- PS2_GFX_ASYNC_CHECK: 5-14% of jobs read memory the EE changed after the kick: always the effect-object pool at
  0x1CBxxxx-0x1CDxxxx (pairs 0x5A0 / 0x840 apart: per-object double buffers), written by f_0015B428 (allocates /
  initialises effect objects from a pool, 0x1390D8) and the main loop (PS2_WRITE_WATCH=1CC26A0). Same race as on a
  PS2 (the DMA runs beside the EE there too); effect: those objects one frame early in that frame. So the worker is used
  only above 60 fps (frame_rate_high()); at 60 the chain runs on the EE as before (exact).

## In progress
- A1 graphics worker (runtime/gfx_async.c/.h, hooks in dma/vif/gif/vu/hw/gs_thread/ps2_runtime/perf) + A2 raw PATH1
  packets (gif_parse.h, gif.c path1_raw, gs_thread.c K_GIF / gs_gif_packet; PS2_GIF_RAW=0 off, PS2_GIF_CHECK=1 checks).
  Written, not yet built.

## Next
1. Profile all threads at 120 / 165 (ep1 / ep3 / ep5 hubs, ep8 boss, a light scene).
2. Prototype (a): VIF1 DMA chain on a worker thread (render thread waits on GS FINISH semaphore 0x2DFB50).
3. Prototype (b): re-kick the frame's chain with interpolated camera / rigid matrices.
4. Compare in notes/frame-rate.md, decide, implement, gates.

## Findings / traps
- Render thread f_00160B70: WaitSema 0x2DEB18 -> 0x16F8B8(ch *0x2DE594, frame+0x24), 0x16F8B8(ch *0x2DE598, frame+0x4)
  -> (frame+0x30 & 1: 0x160F98) -> 0x2417F0(ch *0x2DE594) (DMA sync) -> SignalSema 0x2DEB1C -> 0x16F590 (VBlank wait).
- 0x16F8B8: lock, 0x2DFB54 = 1, CSR |= 2 (FINISH), IMR enables FINISH, sceDmaSend(ch, addr) (0x241728), WaitSema 0x2DFB50
  (signalled by the GS FINISH interrupt handler 0x16F868), unlock. In the port the whole chain runs inside the CHCR write,
  so the render thread never blocks there.
