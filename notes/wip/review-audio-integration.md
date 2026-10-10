# WIP: review-audio-integration (independent review of audio/audio-iop integration)

**Status: complete** (2026-10-10). Verdict: integration works and is reproducible; one real
integration bug (IOP starved during frame rendering) and two smaller problems fixed on the branch.

**Goal**: verify the audio-integration task (native 989snd/spiop + sd.c mixer + WASAPI in the
game) independently: rebuild, tests, runs, WAV evidence, code quality; fix bugs with tests first.

## Done
- Fresh builds: build/rv_rel (release), build/rv_dbg (debug), build/main_rel (main baseline from
  `git archive main` in build/main_src, with a .git file pointing at the main checkout's tools).
- r1 (release before fixes, PS2_LOG_SND=1, fresh card, "600:square:12,t95:start:10", 250 s):
  boot movie, prompt, intro, "Sly Cooper the thief" cards, title, briefing with all 8 lines
  (header +-0.16 s, decoded-vs-WAV correlation 0.94-0.99), gameplay; 0 underruns/late refills.
- s1500 ("600:square:12,1500:start:10"): START at 1500 skips the intro to the title, as on main
  (m1). The implementer's "ignored" was wrong; docs fixed.
- 4cdc375: IOP status line shows hard-timer lateness; PS2_LOG_SND one write per line (lines were
  split by the audio thread's output; the per-word fprintf cost 10-45% of the EE thread, so the
  implementer's timing evidence was taken under that load), arguments copied before the call.
- 681b457 FIX: ps2_poll_iop() between DMA tags/rounds (dma.c, ps2_runtime.c). Profiling showed
  all IOP gaps inside dma pump (frame rendering, up to 144 ms per call): 1600-2000 of 2400 989snd
  ticks per 10 s were > 8 ms late (max 140 ms; 364 ms at gameplay start); the prompt arpeggio
  played at 54-78 ms spacing instead of ~45. After: 2-80 late per 10 s, max 9-36 ms; arpeggio
  41-42 ms. Unit test tests/unit/dma test_poll_iop_between_tags (failed before).
- Re-runs after the fix (build/rv_audio, not committed): r4 release 250 s (all 8 lines, -0.18..
  +0.24 s, correlation 0.95-0.99, 0 underruns/late refills/invalid ADPCM, no clipping, peak -2.6
  dBFS), dbg2 debug 330 s (8 lines, -0.03..+0.21 s, 0 late refills), live2 WASAPI 30 s (0
  underruns), off2 PS2_AUDIO=0 250 s (snd.c model: 8 lines advance, gameplay reached), sd2
  (PS2_LOG_SD=2: stream pitches 0AAA x3 voices, 0EB3 x5, movie 0EB3 L/R).
- Boot movie L=R is the disc's content (PSS SShd 44.1 kHz stereo, identical L/R blocks).
- Frames: memcard prompt identical to main (pixel diff 0); boot movie plays in real time now
  (synced to sound) where main ran it faster.
- All 14 unit test dirs and tests/run_tests.py (16/16) pass.

## Findings (not fixed, minor)
- audio_out.c wav_header uses ftell/fseek (32-bit long): a WAV past 2 GB (~3.1 h) would be
  overwritten from offset 44. Use _ftelli64/_fseeki64.
- sd.c's late/early refill detector only sees writes into the block being played; a refill late
  by a whole half buffer (voice lapped) is not counted.
- cdvd_read_host is declared in both ps2_internal.h and iop.h.
- Residual IOP lateness (max ~20-36 ms release, 50 ms debug) = single DMA tags with long VU1/GS
  work; a per-VIF-command poll or an IOP OS thread would remove it.
- Other agents on this machine stop processes named sly2: run a renamed copy for long runs.
