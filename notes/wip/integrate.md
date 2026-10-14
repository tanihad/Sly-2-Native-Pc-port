# WIP: integrate

**Goal**: merge next/warp, next/vu-recomp, next/ep1..ep8 into next/integrate (from main b0ad105),
keeping one copy of the shared fixes/tools, then run all tests, builds and the release regression.

## Done
- Worktree C:\Users\tanih\Projects\sly2-worktrees\integrate, branch next/integrate from main b0ad105.
- next/warp: already contained in main (0 commits ahead).
- Merged in order: vu-recomp (clean), ep1 (clean), ep2, ep3, ep4, ep5, ep6, ep7, ep8 (merge commits
  describe each resolution). All ten branches are ancestors of HEAD (checked with merge-base).
- Affected unit tests passed after each code merge (timer, sd, iop_kernel, gs-draw, window).

## Resolutions (one copy of each shared fix)
- IOP: ep1's IOP host thread (iop_kernel.c, seqlocked iop_time_hold, mixer driver hold
  PS2_AUDIO_IOP_LIMIT, async logging). ep2's GS-wait hook (gs_set_wait_hook -> ps2_poll_iop, same
  on ep2/ep3/ep6) kept but installed only when !iop_threaded() (PS2_IOP_THREAD=0); with the thread
  iop_service is a no-op and the hook only added 1 ms wake-ups. Pixel-worker gen-0 fix kept.
- Live 'pause': ep1/ep4's PS2-wide hold (ps2_debug_hold) + ep2's COP0 Count hold (timer_cop0_count).
- PS2_POKE: one block = ep5's '?addr=val' condition + ep6's '@file' list + ep1's PS2_POKE_FILE.
- Input tokens: lowercase (ep1/2/3/4), 'release' (ep7, -6), 'pause' (-5), 'calls'/'watch' (ep8,
  renumbered -7/-8). ep6's [a-zA-Z0-9] sscanf change dropped (tokens are lowercased first).
- sd.c: ep1's driver hold + ep2's sd_hold + ep6's noise-voice overlap filter, history, NON log,
  PS2_LOG_SD levels; ep1's PS2_LOG_SD_VOICES pitch logging kept; overlap report via async sdlog (1 KB).
- PS2_TASK_SET/FILE (warp.c, hooks.py): identical blob on all branches.
- ep7 gs.c field-mode fix, ep5 warp.py percent_complete 100, ep8 scripts: taken as is.

## In progress
- Full unit tests + tests/run_tests.py, debug + release builds.

## Next
1. All unit tests, run_tests.py, debug build, release build (--out build\rel).
2. Regression (release): boot->museum 200 s with frames/WAV/PS2_PERF; warp_run -Episode 0..8 -Play
   -Release; three direct jobs (Bentley hack, Murray brawl, chase). Compare frames with testers'.
3. Report.

## Findings / traps
- PowerShell: `git commit -F -` with a here-string argument fails; use -m $msg.
