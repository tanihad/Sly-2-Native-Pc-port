# ep8: playtest Episode 8 "Anatomy for Disaster" (final episode)

**Goal**: play Episode 8 (blimp hub world 38; interiors 39-42; final boss world 43) as far as
scripted/live input allows, fix every runtime/recompiler problem found at its root (with tests),
frames in build\screenshots\ep8_*.png.

## Done
- Merged next/warp (warp tool, notes/warp.md). Release (build\rel) + debug (build\sly2) built.
- Hub (release, warp_run -Episode 8 -Play -Release -Name ep8_hub): safehouse, Sly on the blimp OK.
- Survey of direct job starts (debug, 200 s each, build\warp\s_<job>): all load, briefings and
  cutscenes play, no crash/hang/missing syscall: m8_recon (Binocucom briefing, Sly climbing the
  rope), t8_recon_intro_int_entrance (world 39), m8_post_recon (cutscene), m8_tntbarrel (briefing),
  m8_engine_room_jt (Murray, 0/5), t8_engine_room_jt_open_door (world 42, Sly), m8_engine_room_bentley,
  t8_engine_room_bentley_open_door (world 41, Bentley), m8_engine_room_murray (Bentley 0/3),
  t8_engine_room_murray_hack_path2 (Bentley 1/3), t8_engine_room_murray_open_door (world 40,
  Murray), m8_turret, m8_boss (world 43 cutscene with Carmelita's helicopter, Sly on the blimp).

## In progress
- Survey chain: m8_megajump, t8_open_head_intro.

## Next
1. Check the red translucent band sweeping across world 42 (s_t8_engine_room_jt_open_door frames
   3720/3840): game effect (alarm light?) or rendering bug? Dump every 5 frames + GS trace.
2. Live play (build\ep8tools\live.ps1 -Job <job>): hacking minigame (engine_room_murray
   hack_station), turret (m8_turret), boss (m8_boss), Murray throws (jt), TNT run, megajump.
3. Ending/credits after the boss (t8_heist_done -> t8_complete).
4. Audio (-Wav) and perf (PS2_PERF) when the machine is less loaded.

## Findings / traps
- The machine is shared with 6+ other sly2 instances (CPU 100%): debug runs reach only ~18
  VBlanks/s, the release hub ~40. Perf numbers are meaningless under this load.
- Scratchpad dir is shared with other agents (my survey.ps1 got overwritten): helper scripts live
  in build\ep8tools\ (look.ps1 sheets, live.ps1 live session from a warp, survey.ps1).
- `[sd] VoiceTrans ... overwrites the block voice cX vY is playing (1 so far)` once per level load
  (0xBE700/0xC1000-byte bank upload into 0x19040 while a menu voice still sounds): bank load.
