# pc-research: next big performance step (200+ fps) - checkpoint

**Goal**: decision document notes/perf-plan.md (options compared with measured evidence, recommended plan with
verifiable milestones, NVIDIA features adopt/skip) for reaching 200+ fps at native 1440p.

Branch next/pc-research (worktree C:\Users\tanih\Projects\sly2-worktrees\pc-research), from main f3bb29f.

## Done
- (nothing yet) 2026-10-23: started; read frame-rate.md, gpu-renderer.md, performance.md, native-port-principles.md.

## In progress
- Reading the remaining notes (d3d11-renderer.md appendices, vu-microcode.md, hle-architecture.md, widescreen.md).

## Next
1. Research (web): RT64, N64Recomp, Unleashed Recompiled, PCSX2 (GS HW, MTVU), Play!, paraLLEl-GS, Dolphin, Xenia,
   RPCS3, GPU-driven rendering in D3D11 vs D3D12, NVIDIA Reflex / DLSS / FG, Windows flip model / thread placement.
2. Measure: per-thread cost breakdown at unlimited fps, 1x and 3x, ep1 / ep3 / ep5 hubs + m8 boss.
3. Measure: static vs per-frame VU1 input data; programs / vertices / prims per frame; frontend parallelisable share.
4. Prototype: frontend parallelisation on a gs_replay recording and/or one VU1 microprogram -> HLSL compute.
5. Write notes/perf-plan.md; finish checkpoint.

## Findings / traps
