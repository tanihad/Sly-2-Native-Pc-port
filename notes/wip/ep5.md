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

## In progress
- m5_ghost and m5_ewok_trap direct starts (ghost run tries START presses at 400/800/1200/1600 to
  skip the boot intro).

## Next
1. Remaining direct starts: t5_ghost_enter_tomb (21), t5_ewok_trap_hall/tomb/water (25/24/26),
   m5_kidnap, m5_pickpocket + guardbreak_interior (20) + waterrails_interior (19),
   t5_recon_interior_intro (22), heist: t5_heist_intro, paraglide_to_blimp, free_carmelita_intro,
   neela_chase_intro, boss_1_intro, tank_chase_intro, boss_2_intro.
2. Chalk talks (t5_chalktalk1..3) and the episode menu / movies (--set episode.<ep5>.percent_complete).
3. Audio (-Wav) on the hub and a job; perf (PS2_PERF) when the machine is less loaded.

## Findings / traps
- The machine runs 10+ other sly2 instances (CPU 100%): debug build ~18-30 VBlanks/s, release
  ~27-35 in the hub; PS2_PERF absolute numbers are meaningless now. Release START at pad frame 2400
  comes before the title accepts it under load: use live input (watch latest.png) instead.
- Hub VU load: 700-920k pairs per VBlank (safehouse/hub), close to the intro cutscene's ~950k.
