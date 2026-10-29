# r2-m5b: modern renderer R5 part 2 (guard-band clipper native, then the undecoded variants)

**Goal**: the VU clipper (0x2540 / 0x2150 / 0x1E88 / 0x1DC8 / 0x2058) reproduced natively in its own arithmetic so
guard-band meshes with crossing triangles go native (A/B Z gate holds); then billboards, lit 2nd sub-batch, 0x0E30,
0x16B0, 0x13F0; sky / warps last. Profile before / after.
Worktree C:\Users\tanih\Projects\sly2-worktrees\r2-m5b, branch next/r2-m5b (from main 8e272dd).

## Done
- Base exe: build\base\sly2_base.exe (main 8e272dd, release).
- Profile before (chaos, PS2_R2=hybrid PS2_GPU_PROF=1 PS2_R2_STATS=1, tag prof_before, prof_window): GPU ms
  ep1 3.17 (ff-tri-fast-z 0.77, r2 0.71), ep3 3.33 (ff-tri-fast-z 1.10, r2 0.48), m1 2.98 (r2 0.65, ff-tri-fast-z 0.64),
  m2 4.62 (ff-tri-fast-z 1.75, ref-tri 0.72, part 0.66). Census guard vertices: ep1 7055 (9%), ep3 6831, ep5 3831,
  m1 4887 (23%), m2 3725 (28%) - the largest exact share.
- Clipper (commit e56d3e4 + this step): runtime/r2/r2_clip.c. Not a C port: the game's own recompiled 0x2540 runs on a
  private VU1 (vu.c vu_private_run / vu_private_init / vu_compiled_image; thread-local XGKICK redirect) with the three
  records of each clipped triangle built exactly as 0x06D0 / 0x0BE0 leave them (VU MXCSR SSE). 0x0C60's verdict per
  triangle in C (r2_clip_verdict: unscaled CLIP flags; all three +z / -z or all three off one screen side (x 7.93,
  y 18.1) culled; else clipped). Oracle (scripts/r2_mesh_check.c, build: scratchpad mc.ps1 command): ep1 / ep3 / ep5 hub
  recordings: verdicts identical for every triangle (~50k each), clipper packets identical (only 2-8 clipped
  triangles per frame: most guard-band triangles are culled off screen).
- Hybrid: r2_capture.c clip_fans (R2E_FANS): packets into the slot, R2H_OUTLINE hint after the entry, the run ends
  there, epilogue after the fans unless the last sub-batch kicked fans. r2.hlsl r2_gs_clip discards crossing triangles
  (GPU clipping only with PS2_R2_CLIP=1, misc bit 64). Bracket: R2D_FANS -> gf_r2_skip = 2 keeps TRIFAN prims.
  PS2_R2_FANS=0 off. Stats "clipper:".

## In progress
- Sweep ep1 / ep5 / m2 (ab + hc) with the clipper build.

## Next
- Full gates; chaos benchmark before / after (3x, 4x; ep1/ep3/ep5/m1/m2).
- Then: undecoded variants by vertex share (billboards, lit 2nd sub-batch, 0x0E30, 0x16B0, 0x13F0), sky / warps.

## Findings / traps
- Worktree: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python = main .venv; clang from main
  tools\llvm-mingw\bin (the worktree's scripts\env.ps1 points at missing worktree tools).
- r2_capture.c is CRLF: PowerShell here-string replaces silently miss; use the Edit tool.
- VU marks in the vertex w word (base 0x7000): +0x1000 culled, +0x2000 clipped ([10].z = 1), +0x4000 backface.
- CF (clip flag) latency 4 cycles; the recompiled C (build\rel\vu\vuc_376110651_vu1_ks0.c) encodes it.
