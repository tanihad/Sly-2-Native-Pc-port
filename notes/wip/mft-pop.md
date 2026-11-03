# mft-pop: the rare one-frame MFT grid fallback ("other target")

**Goal**: about once per ~27 000 frames MFT runs one gameplay frame on the integer grid (PS2_MFT_LOG reason "other
target") - a one-frame resolution / AA pop. Find the draw, make MFT run it (or avoid the switch), audit the other
fallback reasons, add a "gameplay frames never fall back" counter (PS2_R2_STATS) the sweep asserts.
Worktree C:\Users\tanih\Projects\sly2-worktrees\mft-pop, branch next/mft-pop (from main 38a38df). Never push / merge.
Summary for the docs: notes/modern-renderer.md "MFT pops".

## Repro
chaos_bench ep1_hub, -Secs 330, -Extra "PS2_GS_SCALE=;PS2_R2=hybrid;PS2_MFT=1;PS2_MFT_MSAA=4;
PS2_GS_PRESENT_SIZE=2560x1440;PS2_MFT_LOG=1;PS2_R2_STATS=1;PS2_FPS_TRACE=;PS2_PERF=;PS2_GS_HW_STATS="
(log: build\chaos\bench\ep1_hub_mftpop_0\run.log; the chaos_sum errors at the end are expected - no trace).
Per scene: scripts\mft_pops.ps1 -Name x <scene args> -Keep (log build\warp\mftpops_x\run.log).

## Done
- Cause 1 "other target" (HUD Z target recreated square, paired in-frame): mft_pfam overlay in mft_plan.
- Cause 2 VBlanks without a game frame (PRESENT only / cut-off end): continuation (cont, M.last_r2 kept across
  PRESENT-only frames).
- Cause 3 ALIAS16: CLUT ops in the shadow tail (shf_tail_clut) / between volumes; sequences with 0 volumes.
- Cause 4 R2 clip: MFT clip pipelines (r2_render.c mcpipe*, perms *_r2m_clip*, r2.hlsl r2_gs_clip MFT: inside only).
  Shot with PS2_R2_FANS=0 (10 guard-clean entries / frame) looks right.
- Pop counter + "mft: pop:" lines; r2_sweep check mf (scripts/mft_pops.ps1), default on.
- ep7 "target view" = Binocucom briefing (live portraits from 128x128 render targets): by design, not gameplay.
- Gates so far: run_tests 16/16; units 26/27 (window F11/Alt+Enter known flake) - on cec2880, re-run at the end.
- gs_replay builds: build\cur (this branch, rebuild after changes), build\basert (main 38a38df runtime, build\basesrc).

- Chaos audit m1_brawl / m5_boss: 0 pops.
- Gates: gpu_diff_all mixed S=1 28/28; m9_exact S1+S3 -Game all identical (vs main 38a38df runtime); pc_text_boot
  release + debug PASS; debug + release builds; package.py OK (0.1.1038); r2_sweep ab,hc,tf,mf 42/44, the two fails
  (ep2 ab, ep4 tf) PASS on re-run; run_tests + units re-run on the final code.
- QA (qa-a): PRESENT-only VBlanks = cause 2 (fixed); log budget redone (first grid frame after MFT only); Faithful
  filters on MFT not done - what it takes is in the notes section.

## Status: complete (2026-11-03)

## Findings / traps
- build_game.py uses os.cpu_count() workers: set PYTHON_CPU_COUNT=6 (Python 3.13) to cap it.
- warp_run copies build\rel\sly2.exe at each run's start: don't rebuild while a sweep runs.
- r2_hcheck -Shots at At 4800 can miss the dump window on a loaded machine (fewer VBlanks than At in At/60+12 s).
- Final: run_tests 16/16; units 26/27 (window: the known F11 / Alt+Enter flakes).
