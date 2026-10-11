# WIP: renderer-map (map of the current GS pipeline for a GPU backend)

Role: renderer-map. Branch next/d3d11, worktree C:\Users\tanih\Projects\sly2-worktrees\d3d11.
Status: started. Reading runtime/gs*.c, gif.c, dma.c, vif.c, hw.c, window.c, scripts/gs_replay*.

## Progress
- [ ] 1 data flow EE -> GS (PATH1/2/3, queue, privileged regs, transfers, readback, waits)
- [ ] 2 software renderer structure (state, prims, sampling, CLUT, batches, hazards, snapshots)
- [ ] 3 display (PMODE/DISPFB/DISPLAY/SMODE2, interlace, compose, window.c D3D11)
- [ ] 4 seams for a GPU backend, tests, gs_replay

## Findings
