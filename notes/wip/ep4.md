# ep4: playtest Episode 4 "Jailbreak" (Prague)

**Goal**: play Episode 4 end to end (hub + every job type) with warp saves, fix every runtime /
recompiler problem found at its root (with tests), save frames to build\screenshots\ep4_*.png.

## Done
- Merged next/warp (178353c). Built build\rel and build\sly2.
- Episode 4 start (release, save warp): safehouse "Press X to play as Bentley" -> Bentley in the
  Prague hub; pause menu, Job Help ("Eavesdrop on Contessa"), Gadget Grid all fine.
- m4_recon direct (debug): Binocucom briefing, Bentley on the ledge, dart-gun view (R3) fine.
  Bentley controls (pause > Controls): R1 run/fire dart, Triangle bomb, Circle sneak/activate,
  Square swing dart gun, R3 Binocucom/dart gun, SELECT gadget grid. Aim: stick up = look DOWN.
- m4_hack direct: Bentley at the tram line, 0/6 stations, electrified rail, train kills him and
  he respawns; did not find the hack terminal yet (green arrow at the start points left of the tree).

## In progress
- m4_spycam direct (debug, live, build\lw\spycam).

## Next
1. spycam (photo: possible GS readback), lightning, bomb_bridge, brawl (world 15), pickpocket,
   rc_chopper, heist stages (t4_heist3_elevator, t4_heist4_intro keypads, t4_heist5_intro,
   t4_heist6_intro, t4_heist7_intro chase), hack minigame.
2. For each: rendering, audio (-Wav), perf (PS2_PERF), saving.

## Findings / traps
- The machine is at 100% CPU (other testers' games): debug build runs 8-30 fps, release ~15-35.
  PS2_PERF numbers are not meaningful for 60 fps judgements under that load.
- Live helper (scratchpad lw.ps1): warp.py make --card <dir>\card [--job X --direct], copy exe,
  PS2_MEMCARD_DIR, then scripts\live_game.ps1 -Dir -Exe -Script '@187D80/1DD96C+30:start:10'.
  Release: one START at pad frame ~2600 may be too early under load; send another START live.
- PS2_INPUT stick tokens are lowercase hex (ryd0, not ryD0).
