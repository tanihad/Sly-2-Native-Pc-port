# WIP: review-audio-integration (independent review of audio/audio-iop integration)

**Goal**: verify the audio-integration task (native 989snd/spiop + sd.c mixer + WASAPI in the
game) independently: rebuild, tests, runs, WAV evidence, code quality; fix bugs with tests first.

## Done
- Worktree clean at f25cf5c; main at 9d6dd51 (already merged in a142048).
- Read integration diffs (8283817, a41247c, db3f957, 7ce09d9).
- Fresh builds: build/rv_rel (release), build/rv_dbg (debug), build/main_rel (main baseline from
  `git archive main` in build/main_src, with a .git file pointing at the main checkout's tools).
- r1 (release, PS2_LOG_SND=1, fresh card, "600:square:12,t95:start:10", 250 s): boot movie, prompt,
  intro cutscene (frames 900-2220), "Sly Cooper the thief" cards, title, briefing with all 8 lines
  (audio_streams.py: header +-0.16 s, correlation 0.94-0.99), gameplay. 0 underruns/late refills.
- r2 (same, no PS2_LOG_SND): IOP 0.13-0.19% of the time; with PS2_LOG_SND=1 it is 10-45%
  (logging cost). r2 and m1 (main baseline) were killed at 1:54:08 by another agent (it stops
  processes named sly2): use scratchpad rvrun.ps1, which runs a renamed copy (rvgame.exe).
- s1500 (branch, "600:square:12,1500:start:10"): START at 1500 SKIPS the intro cutscene to the
  title (1440 cutscene, 1500 black, 1530 museum card, 1620 title), exactly as main does (m1). The
  implementer's "START at 1500 lands inside the cutscene and is ignored" is wrong (docs to fix).

## In progress
- r3: release run with a new timer-lateness diagnostic (iop_kernel.c status line) to measure how
  late the 989snd tick runs (sequenced notes at the prompt were 60-80 ms apart in r1 vs 45 ms in
  sd1; suspect IOP jitter from EE stretches without safe points).

## Next
1. Evaluate r3 lateness; decide whether IOP scheduling jitter needs a fix.
2. snd_log (iop_sif.c): one write per line (lines interleave with the audio thread's status line;
   many unbuffered fprintf calls cost 10-40% of the EE thread with PS2_LOG_SND=1).
3. Unit tests + tests/run_tests.py; debug build run; live WASAPI run; PS2_AUDIO=0 run.
4. Fix docs (START at 1500 skips the intro; stale ADMA open point in iop-native.md).
5. Verdict.

## Findings
- PS2_LOG_SND=1 distorts timing (IOP share 10-45%) — the implementer's timing evidence was taken
  with it on.
