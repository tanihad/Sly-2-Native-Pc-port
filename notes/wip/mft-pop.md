# mft-pop: the rare one-frame MFT grid fallback ("other target")

**Goal**: about once per ~27 000 frames MFT runs one gameplay frame on the integer grid (PS2_MFT_LOG reason "other
target": a full-screen blended draw into another target, fmask FF000000) - a one-frame resolution / AA pop. Find the
draw, make MFT run it (or avoid the switch), audit the other fallback reasons, add a "gameplay frames never fall back"
counter (PS2_R2_STATS) the sweep asserts.
Worktree C:\Users\tanih\Projects\sly2-worktrees\mft-pop, branch next/mft-pop (from main 38a38df). Never push / merge.

## Done
- (nothing yet)

## In progress
- release build (build\rel, PYTHON_CPU_COUNT=6 limits build_game.py's pool to 6 jobs)

## Next
1. Reproduce: ep1 hub uncapped, PS2_MFT=1, PS2_MFT_LOG=1, a few minutes; capture the logged op.
2. Identify the draw (EE side: which game code), fix in gpu_exec_mft.h.
3. Audit MW_* reasons; add the counter + sweep assertion.
4. Gates.

## Findings / traps
- build_game.py uses os.cpu_count() workers: set PYTHON_CPU_COUNT=6 (Python 3.13) to cap it.
