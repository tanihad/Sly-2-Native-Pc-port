# WIP: audio-integration (native audio path in the real game)

**Status: complete** (2026-10-10). Documentation: `notes/audio-mixer.md` ("Integration in the
game") and `notes/iop-native.md`.

**Goal**: the real game produces correct sound through recompiled 989snd/spiop + native sd.c mixer
+ WASAPI; verified by WAV captures of headless runs analysed programmatically.

## Done
- Merged audio/audio-mixer into audio/audio-iop (no conflicts; runtime/sd.c replaces the stub).
- audio_init() in ps2_runtime.c main (not in --dump-regs mode); run_game.ps1 -Wav/-Pad, headless
  runs set PS2_AUDIO_NULL=1; sd log lines carry the audio clock; scripts/wav_analyze.py.
- Merged main (9d6dd51): no textual conflicts. Removed main's duplicate cdvd_read_host (one ISO
  reader in cdvd.c, shared by iop_sif.c cdvdman and snd.c); snd.c documented as the PS2_AUDIO=0
  fallback (its spiop stream model is kept there).
- Diagnostics: `[dump] frame_N.png at audio T s` lines (frame dumps <-> WAV time);
  `[audio] status` every 10 s (underruns, mixer CPU, late/early stream refills, invalid ADPCM
  blocks); WASAPI underrun log (pad == 0); IOP status lists threads that used up time slices;
  PS2_LOG_SD=2 also logs VOLL/VOLR/PITCH/SSA/LSAX; "(silence)" marks on all-zero transfers;
  PS2_LOG_SND=1 on the native path (989snd commands + results, incl. spiop procs); warning if an
  RPC arrives while an earlier one for the same server is still queued; PS2_INPUT
  "t<seconds>:<button>" (wall clock).
- iop_libs.c: one transfer-interrupt handler call per completion (was coalesced per channel);
  unit test added.
- wav_analyze.py: click detector uses the 2nd difference (loud high-pitched speech was flagged).
- scripts/audio_streams.py: spiop streams of a run vs the disc (durations vs headers, levels,
  independently decoded dialogue vs WAV correlation).
- Runs (WAVs in build/audio/, not committed): rel5/rel8 (release, 300/240 s), dbg1 (debug,
  420 s), live1 (30 s WASAPI), off1 (PS2_AUDIO=0 fallback). Intro movie, save prompt, intro
  cutscene, title, briefing with all 8 lines advancing, gameplay. 0 underruns, 0 invalid ADPCM,
  0 late refills (release; 2 late movie refills in debug), no clipping, dialogue = header
  length +-0.08 s, decoded-vs-WAV median correlation 0.93-0.99.
- All unit tests and tests/run_tests.py pass.

## Next (follow-ups, not part of this task)
- Reverb is never requested up to the first gameplay (no 989snd command 14); check a later level.
- Movie audio in the debug build arrives late (slow EE); fine in release.

## Findings / traps
- The intro cutscene after the memcard prompt follows its music: it lasts ~40 s of real time, so
  its length in VBlanks depends on emulation speed (software GS: 16-40 fps). Main's frame-based
  START at 1500 lands inside it and skips it to the title screen (as on main; corrected by the
  review: it is not ignored), so no new game starts; use a wall-clock press (t95:start) instead.
- The memcard is in <out>/memcard: delete it for a fresh run (otherwise no prompt).
- PS2_LOG_SD=1 slows the IOP a lot (unbuffered stderr): "IOP code 15%" and ready threads in the
  status line came from logging, not from busy-waiting.
- 989snd uses negative (phase-inverted) voice volumes for sounds behind the listener (Pro Logic
  style): VOLL 7A7E = -0x582. A 3D stream (v0, 32 kHz) on the right/rear makes the intro music
  run ~3 dB louder on the right; the music voices themselves (v5/v6, 44.1 kHz) are symmetric.
- spiop channels: 0 = 32 kHz " KPV" music (3 voices), 1 = 44.1 kHz " KPV" music (5 voices),
  2 = dialogue (VAGp 22050 Hz). Buffers at SPU 0x5040 + 0x1000*slot and +0xA000. Movie sound:
  v0/v1 44.1 kHz ADPCM, 0x400 chunks into 4 KB rings at 0x19040/0x1A040 from IOP 0x111600.
- Dialogue drifts 15 samples/s against an exact 22050->48000 resampling: pitch 0759h = 22043 Hz,
  the hardware's quantisation, not a bug.
- Other agents run the game on the same machine: frame rates vary between runs.
