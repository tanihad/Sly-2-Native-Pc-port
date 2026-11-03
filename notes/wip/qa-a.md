# qa-a: visual QA of the Modern renderer, episodes 1-4 (+ menus / transitions)

Branch next/qa-a (main 38a38df), worktree C:\Users\tanih\Projects\sly2-worktrees\qa-a. Part B (ep 5-8) is qa-b.
Not fixing renderer code; harness only. Findings go to the final report; evidence under build\qa\ (not committed except the
small PNG pairs listed below, copied to notes\qa-a\).

## Harness (new)
- scripts\qa_run.ps1: one matrix cell (scene x cfg x rate): sly2.ini written next to the exe copy, dumps 2560x1440 BMPs of a
  window, runs flicker_check (tf), counts grid-fallback pictures by size, compares samples to the ref run via scripts\qa_cmp.c.
- scripts\qa_scenes.txt: scenes. scripts\qa_cmp.c: block-mean clusters / crops / side-by-side pairs.
- runtime\gpu\gpu_exec_mft.h: PS2_MFT_LOG_MAX (diagnostics only: the 400-line grid-frame log was used up by loading screens).
- Traps: the governor lowers a fixed scale on this shared GPU (ref at 4x ran at 1x until PS2_GS_GOVERNOR=0);
  run cwd for .NET calls is the main checkout, use absolute paths.

## Status
- [x] read notes, built release (build\rel), harness works (ep1_hub ref+bal ~130 s per run)
- [ ] ref runs per scene (60, 120), then the config matrix
- [ ] menus / loading / binocucom / cutscenes
## Findings (confirm each twice)
(none yet)
