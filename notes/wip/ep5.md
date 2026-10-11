# ep5: playtest Episode 5 "A Tangled Web" (Prague)

**Goal**: play Episode 5 (hub world 17) end to end via warp saves / direct job starts, find and fix
runtime/recompiler problems at the root (crash, hang, missing syscall/RPC, rendering, audio, perf).

## Done
- Merged next/warp (178353c) into next/ep5. Release (build\rel) and debug (build\sly2) built.
- Live-play helper (scratchpad direct.ps1, recreate if lost): warp.py make --job X --direct --card
  build\live\<job>\card, copy exe to build\live\<job>\sly2_<job>.exe, set PS2_WARP_TASK +
  PS2_MEMCARD_DIR + PS2_PEEK, then scripts\live_game.ps1 -Dir build\live\<job> -Exe ... -Script
  '@187D80/1DD96C+30:start:10'. Steer with scripts\live_look.ps1 -Dir build\live\<job> -Send ...
- Played (no crash/hang, no new log warnings; only boot-time eret 0x239AA4 / syscall 0x5B):
  - Episode start (release): safehouse, X -> Sly in rainy Prague hub; walking/ledges fine.
  - m5_recon direct: Binocucom briefing, Sly on the roofs, Binocucom photo mode (R1 photo -> "No
    item of interest in view"), target list Road/Blimp/Assault Tank/Neyla's HQ.
  - m5_battlezone direct: letterboxed intro ("OK guys, I'm in a tank!"), first-person tank driving +
    firing, 0/6 counter.
  - m5_hack direct: Bentley in the crypt (world 22), terminal reachable (needs battery first).
  - t5_heist_free_carmelita_hack direct (world 22): Bentley's hacking minigame (top-down ship on a
    blue grid) renders, ship moves/turns with the left stick, enemies kill it, restart pan.
  - t5_heist_turret_intro direct: Bentley's turret vs. green biplanes in the rain; "Job Failed!
    Press X to try again" when idle.

  - m5_ghost (Sly on the cathedral roof, stained glass), t5_ghost_enter_tomb (wolftomb, world 21).
  - m5_ewok_trap (ewoktrainer 23: Binocucom briefing, Bentley at the glowing trap), ewokhall 25,
    ewoktomb 24, ewokwater 26: mojo-trap gameplay ("Use O to activate the traps", meter, enemies,
    "Job Failed! The Bad Mojo collector was destroyed").
  - m5_kidnap (Murray on the roofs), t5_pickpocket_guardbreak_interior (20),
    t5_pickpocket_waterrails_interior (19: water, waterfalls, drowning).
  - t5_heist_paraglide_to_blimp: Gadget Grid (SELECT, exit with TRIANGLE), R1 in a jump deploys the
    paraglider; landing on the disk below replays Bentley's "grab the rope" cutscene.
  - t5_heist_boss_1_intro: letterboxed intro (Contessa as a spider), fight, death -> restart intro.
  - Episode menu: --set episode.castle.percent_complete=100 -> resume opens the dvd_menu world
    (Episode 6 / UP -> Episode 5 "A Tangled Web", X: Start Episode -> safehouse). No movie for ep5.
  - --activate t5_intro changes nothing (safehouse as usual).
- Perf under load: boss fight GS recording (240 VBlanks) replays at 36.3 ms/VBlank vs the
  fix-perf briefing recording at 36.0 on the same loaded machine (8.9 when quiet): not heavier.
  EE waits on the GS thread (gswait 50-90%) everywhere incl. boot: machine load, not ep5.
- Screenshots: build\screenshots\ep5_*.png (hub, menu, tank, hack minigame, turret, boss1, crypt,
  ewok levels, guardbreak, waterrails, wolftomb, kidnap).

- Session 2: merged next/warp again (71124a3: entry hooks, so direct starts and the title anchor
  work in release builds); rebuilt build\rel and build\sly2. Private helper now in
  scratchpad\ep5\direct.ps1 (-Release works with --direct). Do not pipe its output (sly2 inherits
  the pipe and the call blocks until the game exits).
- t5_heist_intro (release direct): Murray cutscene ("I hope that General guy told the truth"),
  Sly's paraglide talk, then Sly on the tower with the blimp; paraglide jump lands on the disk ->
  Bentley's rope cutscene again (gameplay, not a bug).
- The "|" after subtitle text is the game's blinking typewriter cursor (blinks between frames).
- t5_heist_free_carmelita_intro (world 22): Bentley at the old terminal cutscene, then the hack
  minigame (ship moves, shoots with the right stick).
- t5_heist_neela_chase_intro: cutscene (Neyla with the Clockwerk eye), the chase runs; running
  straight is not enough: "Job Failed! Neyla got away with a Clockwerk Eye!" (gameplay).

- t5_heist_tank_chase_intro: cutscene (Carmelita took the eye, escapes in a tank), first-person
  tank chase; holding lup + R1 fire, "Job Failed! Catch and destroy Carmelita's tank" after ~7 s
  (gameplay; the tank moves and fires).
- t5_heist_boss_2_intro: Contessa fight 2, hypnosis spiral world. During the hypnosis transition
  (~60 VBlanks) a 1-px green rectangle shows at the screen edges (x=0, rows 3 and 221). GS trace
  (PS2_GS_TRACE_RECT=0,100,0,100 then 0,0,511,223 brief MINSIZE 150): the game itself draws it - a
  linestrip (0,2.94)-(512,2.94)-(512,221.06)-(0,221.06), RGBA 007F0080, AA1, ABE 0, FBMSK FF000000
  (RGB written), ZBUF Z16S, ZTST GREATER right after clearing that Z buffer to 0 with ZB_ONLY
  sprites, so it passes everywhere. AA1 without ABE = coverage alpha only (alpha is masked here),
  as PCSX2 does (IsCoverageAlpha); on a TV the lines sit in the overscan. Not a runtime bug.
- Audio: hub run with -Wav (480 s, release, loaded machine): no dropouts except level loads/boot,
  11 click candidates (title music starts), positional stereo in the hub. Fine.

- t5_ewok_trap_hall_door (hub door stage, Bentley, "1/4"): a mojo-collector defence too, fails
  without play.
- Natural progression end to end (no warp after the start):
  - t5_heist_free_carmelita_freed (world 22) -> freed cutscene -> complete -> the game loads world
    17 by itself and starts t5_heist_neela_chase_intro (chase) - no save mid-heist (as expected).
  - t5_heist_boss_2_complete: Contessa's defeat cutscene, "Clockwerk's Eye" card, the 2D comic
    epilogue, then the episode menu "Episode 6" (dvd_menu world 1). The game autosaved:
    castle.percent_complete 100, m5_heist.is_complete 1, widCurrent 1, episodeCurrent 0 (!).
  - Resuming that save went into the Cairo museum (prologue): resume checks the current episode's
    percent_complete (episodeCurrent 0 = prologue) and warp saves had 1 there for finished episodes.
    With episode.intro.percent_complete=100 the same save resumes into the Episode 6 menu. Fix in
    scripts/warp.py: finished episodes are saved at 100 (warp unit tests pass; ep5 start still
    reaches the safehouse).
- Chalk talk 1 (warp --job t5_chalktalk1, no direct): safehouse as usual, X -> hub, no chalk talk
  (it is started by the recon-complete event, which a warp save does not replay).

- Batch of direct starts (scratchpad\ep5\batch.ps1 -> build\warp\b_<job>\sheet.png; under this
  load the title needs ~3400 pad frames = 4-5 min, so -Seconds 540): all load and play, logs
  clean: t5_recon_interior_intro (world 22: Contessa/Carmelita cutscene, Sly at the window),
  t5_heist_boss_1_complete (cutscene -> tank chase intro -> chase -> failed), t5_hack_path2/3
  (Bentley with the battery, 1/3 2/3), t5_pickpocket_guardsneak_intro (hub), _interior (world 20).
- Tank chase with lup held + R1 (build\warp\tank_chase_play): the tank drives at a normal pace,
  hits the wall at the first bend and falls (camera z to -767) -> Job Failed: needs steering.
- Ewok water (last mojo-trap stage) live: spamming O does not save the collector (gameplay).

## In progress
- batch rest: waterrails intro, ghost pictures, pickpocket intro, free_carmelita_complete.

## Next
1. Heist stages not yet started: t5_heist_intro (Murray), free_carmelita_intro (22),
   neela_chase_intro, tank_chase_intro, boss_2_intro; recon interior (22).
2. Chalk talks (t5_chalktalk1..3): warp with recon complete + chalktalk1 active?
3. Audio (-Wav) on the hub and a job; checkpoint autosave (complete a task).

## Findings / traps
- The machine runs 10+ other sly2 instances (CPU 100%): debug build ~18-30 VBlanks/s, release
  ~27-35 in the hub; PS2_PERF absolute numbers are meaningless now. Release START at pad frame 2400
  comes before the title accepts it under load: use live input (watch latest.png) instead.
- Hub VU load: 700-920k pairs per VBlank (safehouse/hub), close to the intro cutscene's ~950k.
