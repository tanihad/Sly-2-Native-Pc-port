# mft-cover: MFT covers the gameplay stretches QA found on the grid

**Goal**: make the Modern frame-target path (runtime/gpu/gpu_exec_mft.h mft_plan) cover the gameplay grid stretches
from qa-b (m6_paraglide ALIAS16 + target view, m5_ewok / m5_ghost / m5_battlezone / m6_train_red target view, m7_boss
not modern), same picture as Exact apart from resolution / AA.

Worktree C:\Users\tanih\Projects\sly2-worktrees\mft-cover, branch next/mft-cover (from main ab15b52).

## Done
- QA harness copied from qa-b (scripts/qa_scene.ps1, qa_scenes.txt, qa_pair.c, qa_batch.ps1).

## In progress
- release build (build/rel), triage runs.

## Next
1. Triage per scene (no-dump runs with PS2_MFT_LOG=<n> + sparse dumps every ~25 VBlanks to see what is on screen).
2. Root-cause per reason; extend mft_plan / mft_op.
3. m7_thermals banded strip: try to reproduce.
4. Verify: fallback table, gates.json, m9_exact, r2_sweep ab/hc/tf ep1, run_tests.py, PS2_GPU_AB frame times.

## Findings / traps
