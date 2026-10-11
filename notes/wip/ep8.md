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

- m8_megajump (Sly on the blimp), t8_open_head_intro (Clock-La's head in Paris cutscene, then
  Murray stage: lasers from the head, bombs) load and play.
- Boss live (build\warp\live_boss): debris-fall chase (Sly on falling blimp pieces, free fall,
  "Catch up to Clock-La") plays; not won by blind input.
- Red band in world 42: not reproduced in a second run with dumps every 6 VBlanks (900 VBlanks);
  looks like a translucent light cone (guard flashlight?) near the camera; left as game effect.
- NEW debug tool (runtime/warp.c): PS2_TASK_SET=<oid>:<state>[@vblank] / PS2_TASK_FILE=<path>
  (debug builds): calls the game's own task-state change 0x164DF0 (object found with 0x15E938) at
  the top of the main loop (entry of 0x20E890, called from 0x1A7588) to complete a stage.

- PS2_TASK_FILE on the boss (live_end): 3484/3488/3492 -> 2 change the task states but the
  boss stage being played goes on (boss state machine) -> no shortcut to the ending.
- Session 2: merged next/warp again (entry hooks: direct starts + START anchor in release);
  PS2_TASK_SET/FILE now ride an entry hook on 0x20E890 (every build), documented (warp.md,
  hle-architecture.md). Cherry-picked ep7's PS2_POKE (hold health: murray.hpCur 0x3D4AE0,
  sly 0x3D4AB0, bentley 0x3D4AC8; game-state blob 0x3D4A60 + schema offset).
- Episode menu (save with episode.blimp.percent_complete=100, int): dvd_menu world shows the
  slideshow, X -> "Anatomy for Disaster / Start Episode / 95%" (no "Watch a Movie" there).
  build\screenshots\ep8_episode_menu.png.
- live.ps1 now: direct start in both builds (-Hub = walk in instead), -Extra "VAR=v;VAR2=v".

## In progress
- Boss Murray stage (t8_open_head_intro, release, head3..5 with PS2_POKE=3D4AE0=3C): Murray walks
  to Clock-La's head (player pos peek 0x2DE1C0: start (0,-2168), beak front ~(40..250, 200..450));
  lasers hit/knock him; guides: "jump over the lasers, reach the head, pry the mouth open". Tried
  circle (= Murray's high jump-slam, letterboxed camera), triangle/square (attacks) at the beak
  and at the pink glowing cage right of the beak: no pry yet. Direct start of 3488 (Bentley stage,
  no start point) leaves a static letterboxed view: not a start point (not a bug).
  Dialogue: "I'll pry open that beak so that Bentley can go to town with his bombs."
- Learning the pry from m8_engine_room_jt's door lift (t8_engine_room_jt_lift, world 38, Murray).
- Helpers: build\ep8tools\strs.py/strrange.py (RAM strings), findword.py, hexf.py, findobj.py
  (ep7's), arr.py; boss task objects 3484 @0xD5AB00, 3488 @0xD5BB40, 3492 @0xD60AC0.

## Next
1. Win Murray stage -> Bentley stage -> ending/credits (t8_heist_done -> t8_complete).
2. Live play (build\ep8tools\live.ps1 -Job <job> -Release): hacking minigame (engine_room_murray
   hack_station), turret (m8_turret), Murray throws (jt), TNT run, megajump.
3. Audio (-Wav) and perf (PS2_PERF) when the machine is less loaded.
4. Representative frames into build\screenshots\ep8_*.png (survey dirs build\warp\s_*).

## Findings / traps
- The machine is shared with 6+ other sly2 instances (CPU 100%): debug runs reach only ~18
  VBlanks/s, the release hub ~40. Perf numbers are meaningless under this load.
- Scratchpad dir is shared with other agents (my survey.ps1 got overwritten): helper scripts live
  in build\ep8tools\ (look.ps1 sheets, live.ps1 live session from a warp, survey.ps1).
- `[sd] VoiceTrans ... overwrites the block voice cX vY is playing (1 so far)` once per level load
  (0xBE700/0xC1000-byte bank upload into 0x19040 while a menu voice still sounds): bank load.
