# WIP: audio-integration (native audio path in the real game)

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
  PS2_LOG_SD=2 also logs VOLL/VOLR/PITCH/SSA/LSAX; PS2_INPUT "t<seconds>:<button>" (wall clock).
- iop_libs.c: one transfer-interrupt handler call per completion (was coalesced per channel).
- wav_analyze.py: click detector uses the 2nd difference (loud high-pitched speech was flagged).
- Release run (rel5, 300 s, fresh memcard, PS2_INPUT="600:square:12,t95:start:10"): intro movie,
  memcard prompt, intro cutscene with music, title, START -> Binocucom briefing with all dialogue
  lines advancing (frames 3240-4500) -> gameplay in the museum (frame 4590+). No underruns, no
  late/early refills.

## In progress
- Analysis of the WAVs (build/audio/rel5.wav + rel5.txt; frame <-> time map in the run log).

## Next
1. Rebuild, rerun full sequence (release, PS2_LOG_SD=1) -> check health counters, reverb in
   gameplay (SetEffectAttr), clicks on the beat grid = drums (content), movie sound.
2. Debug build run over the same sequence (slower EE: stream timing robustness).
3. Live WASAPI run (-Window not needed: PS2_HEADLESS=1 without PS2_AUDIO_NULL), check underruns.
4. Unit tests + tests/run_tests.py; notes/audio-mixer.md + notes/iop-native.md; final report.

## Findings / traps
- The intro cutscene after the memcard prompt follows its music: it lasts ~40 s of real time, so
  its length in VBlanks depends on emulation speed (software GS: 16-40 fps). Main's frame-based
  START at 1500 lands inside it and is ignored; use a wall-clock press (t95:start) instead.
- The memcard is in <out>/memcard: delete it for a fresh run (otherwise no prompt).
- PS2_LOG_SD=1 slows the IOP a lot (unbuffered stderr): "IOP code 15%" and ready threads in the
  status line came from logging, not from busy-waiting.
- 989snd uses negative (phase-inverted) voice volumes for sounds behind the listener (Pro Logic
  style): VOLL 7A7E = -0x582. A 3D stream (v0, 32 kHz) on the right/rear makes the intro music
  run ~3 dB louder on the right; the music voices themselves (v5/v6, 44.1 kHz) are symmetric.
- Stream layout seen: 989snd stream buffers at SPU 0x5040 + 0x1000*slot (first halves) and
  +0xA000 (second halves); voices v0-v2 (32 kHz) and v4-v8 (44.1 kHz); refill one 0x1000 half
  per voice per half period. Movie sound: v0/v1 44.1 kHz, 0x400 chunks into 4 KB rings at
  0x19040/0x1A040 from IOP 0x111600.
