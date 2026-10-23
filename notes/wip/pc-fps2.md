# pc-fps2: higher real frame rate (pipeline vs frame interpolation)

**Goal**: smooth 120-165 fps; decide by measurement between (a) pipelining / parallelising the per-frame graphics
preparation (VIF1 / VU1 / GIF off the EE thread) and (b) frame interpolation at a 60 Hz tick; implement the winner.

Status: in progress (branch next/pc-fps2 from main ff40358).

## Done
- Read notes (frame-rate, performance, hle-architecture, gpu-renderer, d3d11 appendices L/N/O, widescreen).
- Release build in build\rel (45 s).

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
