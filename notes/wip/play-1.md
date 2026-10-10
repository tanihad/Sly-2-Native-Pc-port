# play-1: from the Binocucom briefing into gameplay

**Goal**: push the game from the Bentley/Sly Binocucom briefing into gameplay and beyond, fixing
runtime/recompiler blockers at the root; check rendering per stage; best frames in build\screenshots.

## Done
- Baseline (eb96e20): briefing ends by itself and **gameplay works**: Sly walks/runs (left stick),
  jumps, attacks; HUD mask; camera follows. Pause menu ("WE'LL BE RIGHT BACK!", Keep Playing /
  Save and Quit / Options / Controls) opens with START and closes again.
- Merged fix/fix-perf 7491a63 (GS thread + parallel pixel workers): debug build ~52 VBlanks/s
  instead of ~26, so boot -> gameplay takes ~90 s instead of ~170 s.
- Input script (runtime/window.c, unit-tested in tests/unit/window): tokens can be relative
  (`+n:btn`) or anchored to game code (`@func[/ra][+n]:btn` = after a call of func from the call
  site returning to ra; `@!func[+n]:btn` = after func was called and then silent for n frames);
  `none` = wait marker. Every press logs its pad frame and VBlank. ps2_runtime.c counts anchor
  calls (ps2_func_calls), debug builds only.
- PS2_DUMP_FROM=<vblank>; PS2_CALLS takes up to 8 windows ("1900:30,3500:30"); scripts/contact_sheet.py
  tiles dumped frames into one PNG (no PIL).

## Script that reaches gameplay (save file already on the memory card)
`PS2_INPUT="600:square:12,@187D80/1DD96C+30:start:10,@!227820+90:none,+30:lup:100,..."`
- 0x187D80 called from 0x1DD968 (ra 0x1DD96C) = the title screen's START check (only when the
  title is up); 0x227820 = Binocucom dialogue text (every frame of the briefing).

## In progress
- Pause menu -> Save and Quit (memory card save), then load from the title menu.

## Next
1. Save and Quit + reload (mc write path).
2. Explore the Cairo museum prologue; look for runtime blockers (logs) and render bugs.
3. Unimplemented syscalls seen at boot: 0x4A/0x4B (Set/GetOsdConfigParam), 0x68, 0x6B, 0x5B HLE.

## Findings / traps
- VBlanks are wall-clock driven; the voice streams (spiop model) too, so the briefing length in
  VBlanks depends on emulation speed -> anchor scripts to game code, not to absolute frames.
- Title: 0x1DD920 (title handler) runs from boot on (~v300) but ignores START until this+0x24 != 0.
- The big brown rounded shape in front of the camera near the start is real geometry (textured
  PSMT8 256x128 at TBP 15232 + additive second pass), not a GS bug.
