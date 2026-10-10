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
- r2 (same, no PS2_LOG_SND): IOP 0.13-0.19% of the time; with PS2_LOG_SND=1 it was 10-45%.
  r2 and m1 (main baseline) were killed at 1:54:08 by another agent (it stops processes named
  sly2): scratchpad rvrun.ps1 runs a renamed copy (rvgame.exe).
- s1500 (branch, "600:square:12,1500:start:10"): START at 1500 SKIPS the intro cutscene to the
  title (1440 cutscene, 1500 black, 1530 museum card, 1620 title), exactly as main does (m1). The
  implementer's "START at 1500 lands inside the cutscene and is ignored" was wrong: docs fixed.
- 4cdc375: IOP status line shows hard-timer lateness; PS2_LOG_SND one write per line, args copied
  before the call.
- 681b457: FIX ps2_poll_iop() between DMA tags/rounds (dma.c). Before: 1600-2000/2400 ticks per
  10 s > 8 ms late, up to 140-364 ms (profiling: all inside dma pump = frame rendering); the
  prompt arpeggio's note spacing 54-78 ms instead of ~45. After: 20-80 late, max 15-36 ms,
  arpeggio 41-42 ms. Unit test tests/unit/dma (test_poll_iop_between_tags), failed before.
- All unit tests (14 dirs) and tests/run_tests.py 16/16 pass after the fix.
- Docs: audio-mixer.md (START skip), iop-native.md (long runtime operations, status line,
  PS2_LOG_SND, open points incl. stale ADMA wording), wip/audio-integration.md correction.

## In progress
- r4 (release with fix, 250 s, PS2_LOG_SND=1, WAV+dumps) and off2 (PS2_AUDIO=0, 250 s) running.

## Next
1. Analyse r4: audio_streams.py --correlate, scratchpad wavcheck.py; frames vs main m1/r1.
2. off2: fallback reaches the briefing and lines advance (snd.c stream model).
3. Debug build run (rv_dbg) ~300 s; live WASAPI 30 s (PS2_AUDIO_NULL=0) for underruns.
4. Verdict + report.

## Findings
- PS2_LOG_SND=1 distorted timing (IOP share 10-45%) — the implementer's timing evidence was taken
  with it on (now one write per line).
- Residual IOP lateness (max ~20-30 ms) is single DMA tags with long VU1/GS work.
