# WIP: integrate

**Status: complete** (2026-10-14). Branch next/integrate, not pushed.

**Goal**: merge next/warp, next/vu-recomp, next/ep1..ep8 into next/integrate (from main b0ad105),
keeping one copy of the shared fixes/tools, then run all tests, builds and the release regression.

## Done
- Worktree C:\Users\tanih\Projects\sly2-worktrees\integrate, branch next/integrate from main b0ad105.
- next/warp: already contained in main (0 commits ahead).
- Merged in order: vu-recomp (clean), ep1 (clean), ep2, ep3, ep4, ep5, ep6, ep7, ep8 (each merge
  commit message describes its resolution). All ten branches are ancestors of HEAD.
- All 17 unit test folders pass (window 903 checks); tests/run_tests.py 16/16 + both hook guards;
  debug build and release build (build\rel) OK.
- scripts/run_game.ps1 -Exe <name> and warp_run.ps1 run sly2_warp_<name>.exe (unique process names).

## Resolutions (one copy of each shared fix)
- IOP: ep1's IOP host thread (iop_kernel.c, seqlocked iop_time_hold, mixer driver hold
  PS2_AUDIO_IOP_LIMIT, async logging). ep2's GS-wait hook (gs_set_wait_hook -> ps2_poll_iop, same
  on ep2/ep3/ep6) kept but installed only when !iop_threaded() (PS2_IOP_THREAD=0); with the thread
  iop_service is a no-op and the hook only added 1 ms wake-ups. Pixel-worker gen-0 fix kept.
- Live 'pause': ep1/ep4's PS2-wide hold (ps2_debug_hold) + ep2's COP0 Count hold (timer_cop0_count).
- PS2_POKE: one block = ep5's '?addr=val' condition + ep6's '@file' list + ep1's PS2_POKE_FILE.
- Input tokens: lowercased (ep1/2/3/4), 'pause' -5, 'release' (ep7) -6, 'calls'/'watch' (ep8)
  renumbered -7/-8. ep6's [a-zA-Z0-9] sscanf change dropped (tokens are lowercased first).
- sd.c: ep1's driver hold + ep2's sd_hold + ep6's noise-voice overlap filter, register history,
  NON log, PS2_LOG_SD levels 2/3; ep1's PS2_LOG_SD_VOICES pitch logging kept; the overlap report
  goes through ep1's async sdlog (buffer 1 KB for the history).
- PS2_TASK_SET/FILE (warp.c, hooks.py): identical blob on all branches.
- ep7 gs.c field-mode fix, ep5 warp.py percent_complete 100, ep8 scripts: taken as is.

## Regression (release)
- Boot -> memory card prompt (square) -> intro -> title (t95 start) -> briefing -> museum gameplay,
  200 s, frames + WAV + PS2_PERF (build\reg\boot, boot2, boot3): all scenes reached, 0 underruns,
  0 IOP ticks > 8 ms late, memory flat 152 MB committed (main: 149), WAV has sound throughout.
  Quiet machine (boot3): 99.8% / 99.6% / 97.5% of due VBlanks (intro / title+briefing / museum),
  GS thread ~16.0-16.4 ms/VBlank (the known GS bound), EE busy 6.3-6.9 ms.
  Same-time A/B with next/vu-recomp's exe (= main + VU fix; build\reg\ab1_*): museum 98.2% vs
  98.3%, EE busy 6.6 vs 7.0 ms, late IOP ticks 20 vs 5509 -> no performance regression; the
  integrate gains come from the IOP thread.
- Every episode start, warp_run -Episode 0..8 -Play -Release (build\warp\int_ep*): all nine hubs
  load and match build\screenshots\warp_all.png from next/warp (ep1's X press came while the
  safehouse prompt was still up under 3 parallel runs; rerun int_ep1b with later presses reached
  the Paris rooftops).
- Direct job starts: m4_hack (Bentley), m1_brawl (Murray, 0/3 counter), t0_rooftop_start (rooftop
  chase, Carmelita firing): all started by the warp hook, logs clean.
- ep6's music-stream repro t6_rc_chopper_battle_intro -Direct: battle running, stream transfers
  steady to the end (130/10 s), no 989snd stop.

## Findings / traps
- PowerShell: `git commit -F -` with a here-string argument fails; use -m $msg.
- One unexplained 5.07 s EE stall in boot2 at ~144-149 s (briefing -> museum gameplay load; two
  other agents' games running): `[perf] 149.1s vblanks 1/304 due ... busy 5068 ms ... EE 100%`.
  Not seen in boot, boot3, ab1_int or the 3 base runs. Repro attempt: the boot regression with
  PS2_PERF=1, grep '[perf]' lines with busy > 1000 ms.
- Every level load logs one "VoiceTrans ... overwrites the block voice ... is playing": the bank
  upload over a voice in its release tail (known, inaudible, game-side; ep6/ep7 notes).
