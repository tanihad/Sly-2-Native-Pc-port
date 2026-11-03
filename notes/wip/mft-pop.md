# mft-pop: the rare one-frame MFT grid fallback ("other target")

**Goal**: about once per ~27 000 frames MFT runs one gameplay frame on the integer grid (PS2_MFT_LOG reason "other
target") - a one-frame resolution / AA pop. Find the draw, make MFT run it (or avoid the switch), audit the other
fallback reasons, add a "gameplay frames never fall back" counter (PS2_R2_STATS) the sweep asserts.
Worktree C:\Users\tanih\Projects\sly2-worktrees\mft-pop, branch next/mft-pop (from main 38a38df). Never push / merge.

## Repro
chaos_bench ep1_hub, -Secs 330, -Extra "PS2_GS_SCALE=;PS2_R2=hybrid;PS2_MFT=1;PS2_MFT_MSAA=4;
PS2_GS_PRESENT_SIZE=2560x1440;PS2_MFT_LOG=1;PS2_R2_STATS=1;PS2_FPS_TRACE=;PS2_PERF=;PS2_GS_HW_STATS="
(log: build\chaos\bench\ep1_hub_mftpop_0\run.log; the chaos_sum errors at the end are expected - no trace).

## Done
- Cause 1 ("other target", 5 per 330 s run): the HUD's Z16 target (id 4, base 70) is released by the frontend after 120
  frames unused (gpu_own.c own_frame_end) and created again (square family) when a HUD element with Z returns (coin
  counter bb 8,6-96,39; a full-screen 0,0-511,223 quad, ZTST GREATER + write, FBMSK FF000000); its first draw with ct 1/3
  pairs it into the display family (GPU_FOP_TARGET in the same op list), but mft_plan judged by the executor's
  pre-frame xtarget state. Fix: mft_plan tracks the frame's TARGET / RELEASE ops (mft_pfam overlay). Verified: 0 left.
- Cause 2 (found with the pop counter, ~110 pops per run uncapped!): VBlanks whose op list is only a PRESENT (no game
  frame finished for that VBlank) or the cut-off end of the previous game frame -> "not modern" -> one grid frame.
  Fix: continuation (mft_plan_ops cont): no R2 right after a modern frame -> MFT in the phase that frame ended in;
  with display draws only right after an R2 frame (M.last_r2), so menus still go to the grid.
- Cause 3 ("ALIAS16 tail", ~3 per run): a GPU_FOP_CLUT op inside the shadow tail (between the copy draws or before the
  darken). shf_tail accepts CLUTs when shf_tail_clut (MFT only), mft_shadow runs them before the fused pass; CLUTs
  between volumes too.
- Pop counter: M.pops (runs of 1..8 grid frames between modern frames, MW_OFF excluded) in the PS2_R2_STATS mft line
  ("continued N; pops N (...)") + "mft: pop:" lines.

## Next
1. Re-run the repro with causes 2 + 3 fixed; check pops 0; look at any remaining.
2. Sweep check asserting pops == 0 (new r2_sweep check? or in tf).
3. Gates; notes/modern-renderer.md section.

## Findings / traps
- build_game.py uses os.cpu_count() workers: set PYTHON_CPU_COUNT=6 (Python 3.13) to cap it.
