# WIP: integrate

**Goal**: merge next/warp, next/vu-recomp, next/ep1..ep8 into next/integrate (from main b0ad105),
keeping one copy of the shared fixes/tools, then run all tests, builds and the release regression.

## Done
- Worktree C:\Users\tanih\Projects\sly2-worktrees\integrate, branch next/integrate from main b0ad105.
- next/warp: already contained in main (0 commits ahead).

## Blob survey (identical copies across branches)
- runtime/warp.c (PS2_TASK_SET/FILE) and recomp/hooks.py identical on all ep branches.
- gs_draw_pixel.c / gs_thread.c / test_gs_draw.c: ep2 = ep3 = ep6 (wait hook + worker start fix).
- timer.c: ep1 = ep4; ep2 has more (cop0 count hold). sd.c: ep2 = ep4; ep1 and ep6 differ.
- window.c: ep1 = ep2 = ep4; ep3, ep5, ep6, ep7, ep8 each differ.

## In progress
- Merging, in order: vu-recomp, ep1, ep2, ep3, ep4, ep5, ep6, ep7, ep8.

## Next
1. Merge each branch, resolve conflicts, build + unit tests after each code merge.
2. Full tests, debug + release build, regression (boot->museum 200 s, every episode start, 3 direct jobs).
3. Report.

## Findings / traps
