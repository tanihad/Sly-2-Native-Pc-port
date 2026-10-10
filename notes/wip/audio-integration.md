# WIP: audio-integration (native audio path in the real game)

**Goal**: the real game produces correct sound through recompiled 989snd/spiop + native sd.c mixer
+ WASAPI; verified by WAV captures of headless runs analysed programmatically.

## Done
- Merged audio/audio-mixer into audio/audio-iop (no conflicts; runtime/sd.c replaces the stub).

## In progress
- Wire audio_init() into ps2_runtime.c main (not in --dump-regs mode); run_game.ps1 -Wav.

## Next
1. Build, run headless with PS2_AUDIO_WAV over intro movie / logo / memcard prompt.
2. Analyse WAVs (levels, clipping, gaps, spectrum, balance); fix integration problems.
3. Scripted input (PS2_INPUT="600:square:12,1500:start:10") to title + briefing; analyse.

## Findings / traps
