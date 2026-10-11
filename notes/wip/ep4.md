# ep4: playtest Episode 4 "Jailbreak" (Prague)

**Goal**: play Episode 4 end to end (hub + every job type) with warp saves, fix every runtime /
recompiler problem found at its root (with tests), save frames to build\screenshots\ep4_*.png.

## Done
- Merged next/warp (178353c). Built build\rel and build\sly2.
- Episode 4 start (release, save warp): safehouse "Press X to play as Bentley" -> Bentley in the
  Prague hub; pause menu, Job Help ("Eavesdrop on Contessa"), Gadget Grid all fine. The game's
  autosave after entering the hub is written and decodes fine (warp.py dump: dt, prison dt).
- m4_recon direct (debug): Binocucom briefing, Bentley on the ledge, dart-gun view (R3) fine.
  Bentley controls (pause > Controls): R1 run/fire dart, Triangle bomb, Circle sneak/activate,
  Square swing dart gun, R3 Binocucom/dart gun, SELECT gadget grid. Aim: stick up = look DOWN.
- m4_hack direct: Bentley at the tram line, 0/6 stations, electrified rail, train kills him and
  he respawns ("Hack all six stations"); hack terminal not reached yet.
- m4_spycam direct: Sly on the roofs, "Pickpocket the key from the guard", Binocucom camera (R3,
  R1 = photo: "No item of interest in view") works.
- Direct starts (build\warp\d_<task>, frames + logs, all clean logs, all render plausibly):
  heist1 (Bentley "Hijack the tank"), heist3 (world 15 prison interior cutscene), heist4 (Murray
  hypnotised cutscene, lasers), heist5 (Sly, hypno-speakers 0/4; caught -> death mask -> retry),
  heist6 (Murray "Lift the gate levers"), heist7 (Contessa/Murray cutscene), lightning intro
  (Binocucom briefing), bomb_bridge intro, pickpocket intro (keys 0/3), rc_chopper intro (top-down
  RC chopper, rockets), rc_chopper_sly_escape (cutscene, Binocucom, "Return to the safe house").
- Audio (release, PS2_LOG_SND, build\lw\hubsnd): title opens 2 KPV music layers (32 kHz + 44.1 kHz,
  sectors 529824/533632) which carry on into the hub (layer 2 volume 0, layer 1 ramped by proc 6);
  the hub dialogue VAGp line plays 8.34 s vs 8.21 s header. Hub level ~-40 dBFS, L-biased (3D
  positioned stream). 0 underruns. One "[sd] VoiceTrans ... overwrites the block voice c0 v4 is
  playing" at the title->hub bank load (load screen, inaudible).

- Batch C direct starts (brawl_intro_int, heist2_sly_hide_under_tank, bomb_bridge_on_top,
  spycam_picture1) load and play, clean logs. Brawl live (lw\brawl): Murray punches guards; a big
  white translucent shape over the yard (frame 16110) is most likely a searchlight cone.
- Session 2: merged next/warp again (entry hooks: @187D80 anchor + PS2_WARP_TASK in RELEASE).
  Helper scripts moved to build\tools (lw.ps1 now: anchor in both builds, PS2_PEEK 2DE1C0/2DDFD0,
  -Wav, -Exe; decomp.ps1 = headless Ghidra on the shared scratchpad project, read-only;
  objnear.py / owner.py / objdiff.py / velfind.py RAM-dump helpers).

## In progress
- RC chopper (m4_rc_chopper, lw\rc*): with the stick released the chopper keeps swaying ~+-500
  units at near full speed (never settles); ep2's RC chopper trace shows the same. Not a dt effect:
  same with Count tied to VBlanks (rcv) and to flips (rcv3: dt exactly 1/60). Chopper = object
  0x9727D0 (vt 0x2D5898), child of a parent at (5270,-5779) (rotated); local pos +0x2B0, world
  matrix +0x2C0..+0x2F0, local velocity +0x330 (accelerates without input). Tracing every call
  with a0 = 0x9727D0 (build\expd = debug + PS2_EXP_A0 experiment, lw\rca0) to find the
  controller code. Is the sway the game's design or a recompiler/FPU/VU0 bug? Undecided.

## Next
1. Hacking minigame (m4_hack station, heist4 keypads), lightning puzzle, Murray brawl.
2. Saving at a checkpoint (complete a task -> autosave), audio in a job.

## Findings / traps
- The machine is at 100% CPU (other testers' games): debug build runs 8-30 fps, release ~15-35.
  PS2_PERF numbers are not meaningful for 60 fps judgements under that load.
- The scratchpad dir is SHARED with the other testers (they overwrote look.ps1): keep helper
  scripts in scratchpad\ep4\ (lw.ps1 live warp, look.ps1, sheet.ps1, batch.ps1).
- Live helper lw.ps1: warp.py make --card <dir>\card [--job X --direct], copy exe,
  PS2_MEMCARD_DIR, then scripts\live_game.ps1 -Dir -Exe -Script '@187D80/1DD96C+30:start:10'.
  Release: one START at pad frame ~2600 may be too early under load; send another START live.
- PS2_INPUT stick tokens are lowercase hex (ryd0, not ryD0).
- Direct-start runs do not autosave at start (only the episode-start hub entry does).
