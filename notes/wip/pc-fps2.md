# pc-fps2: higher real frame rate (pipeline vs frame interpolation)

**Goal**: smooth 120-165 fps; decide by measurement between (a) pipelining / parallelising the per-frame graphics
preparation (VIF1 / VU1 / GIF off the EE thread) and (b) frame interpolation at a 60 Hz tick; implement the winner.

Status: in progress (branch next/pc-fps2 from main ff40358).

## Done
- Read notes (frame-rate, performance, hle-architecture, gpu-renderer, d3d11 appendices L/N/O, widescreen).
- Release build in build\rel (45 s).

## In progress
- STEP 1 profile.

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
