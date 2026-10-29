# r2-m5b: modern renderer R5 part 2 (guard-band clipper native, then the undecoded variants)

**Goal**: the VU clipper (0x2540 / 0x2150 / 0x1E88 / 0x1DC8 / 0x2058) reproduced natively in its own arithmetic so
guard-band meshes with crossing triangles go native (A/B Z gate holds); then billboards, lit 2nd sub-batch, 0x0E30,
0x16B0, 0x13F0; sky / warps last. Profile before / after.
Worktree C:\Users\tanih\Projects\sly2-worktrees\r2-m5b, branch next/r2-m5b (from main 8e272dd).

## Done
- Base exe: build\base\sly2_base.exe (main 8e272dd, release).

## In progress
- Profile before (chaos ep1/ep3/ep5/m1/m2, PS2_R2=hybrid PS2_GPU_PROF=1 PS2_R2_STATS=1, tag prof_before).
- Reading the clipper (scratchpad vudis.py; build\rel\vu\vuc_376110651_vu1_ks0.c).

## Next
- Port the clipper (C, VU arithmetic), fans into the slot, clip pipeline drops crossing triangles.

## Findings / traps
- Worktree: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python = main .venv.
