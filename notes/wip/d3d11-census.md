# WIP: gs-census (what Sly 2 asks of the GS, measured over all GS recordings)

Role: gs-census. Branch next/d3d11, worktree C:\Users\tanih\Projects\sly2-worktrees\d3d11.

## Progress
- [x] 0 read the recording format (runtime/gs_record.c), gs_replay.c, register decode conventions in gs_draw.c
- [ ] 1 tool: scripts/gs_census.c (+ scripts/gs_census.ps1): standalone .gsrec parser with GS state tracking
- [ ] 2 run over all 13 recordings (outputs in build/census/*.txt, not committed)
- [ ] 3 census tables + ranked hard cases below

## Tool design (scripts/gs_census.c)
- Parses GSREC001 directly (no renderer link): header (4 MB VRAM, CLUT, 0x64 regs), then records
  {tag,len,payload}: 1 reg write, 2 IMAGE data, 3 VBlank + privileged register snapshot (16 u64:
  PMODE, SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, EXTBUF,
  EXTDATA, EXTWRITE, BGCOLOR, vsync count).
- Vertex queue identical to gs_draw.c (XYZ2/XYZF2 kick, XYZ3/XYZF3 no kick, strips/fans), context from
  PRIM or PRMODE per PRMODECONT, TEX2 folded into TEX0.
- Per kick: pixel bbox (XYOFFSET, scissor) -> page set of the FRAME / ZBUF targets written (FBMSK,
  ZMSK, ATST NEVER+AFAIL honoured); texel bbox (UV or ST/Q, CLAMP modes, mip levels via MIPTBP/MTBA)
  -> page set read. A 512-entry page table remembers the last writer (draw-frame / draw-z / upload /
  local copy, frame number, PSM, target base) so feedback = texture pages written by drawing this or
  the previous frame; in-place = texture pages inside the current FRAME target.
- Draw-state batches: FNV hash of the effective state (attr bits, FRAME, ZBUF, TEST, ALPHA, SCISSOR,
  XYOFFSET, TEX0/TEX1/CLAMP/MIPTBP/TEXA when TME, FOGCOL when FGE, PABE, FBA, COLCLAMP, DTHE/DIMX, CLUT
  version); counts changes between consecutive kicks and distinct keys per frame.
- Page granularity (8 KB) for memory overlap, pixel bboxes for sizes: exact enough for a census, not a
  renderer.

## Findings
(filled in as the runs complete)
