# gpu-M11: hardening and default switch (branch gpu/M11, from next/gpu 4eaf32f)

**Goal**: GPU renderer by default (3x, 16:9, Modern, 60 fps) with a robust software fallback, a no-ROV path,
automatic internal scale for weaker GPUs, bounded memory over long play, flaky iop_kernel test fixed, docs.

**Status**: in progress.

## Done
- (nothing yet)

## In progress
- Reading code: settings.c / gfx.c / gpu_hw.c / gpu_d3d11.c caps / ps2_runtime.c start-up.

## Next
1. Default switch: settings renderer default gpu; fallback log line; GUI no crash.
2. No-ROV path (force caps off by env) -> test on corpus.
3. internal_scale = auto.
4. Long-session memory (warp hub -> job -> hub -> next episode, 20-30 min).
5. iop_kernel flaky test.
6. Docs (settings, install, controls, dist README).
7. Gates + in-game sweep + package + 200 s regression; appendix M.

## Findings / traps
