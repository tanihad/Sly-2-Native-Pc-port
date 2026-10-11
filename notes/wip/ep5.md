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

## In progress
- (survey of all levels done; world 18 "interior" has no task with a start point)

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
