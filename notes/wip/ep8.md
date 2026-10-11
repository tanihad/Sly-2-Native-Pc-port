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

- Session 3 (quiet machine for ~5 min): perf of the hub (release, build\warp\perf_hub, PS2_PERF):
  attract flyover 32-39 VBlanks/s and safehouse 30-45/s with VU 1.0-1.1M pairs/VBlank at 13-21
  ns/pair (VU = 65-70% of EE busy); blimp hub 55-59/s (VU 550-620k). Root cause = VU interpreter
  cost; next/vu-recomp fixes it: temporary merge (--no-commit, built build\vurel, merge aborted):
  safehouse VU ~4 ns/pair, EE busy 10 ms, 54-58/s, then GS thread ~16 ms/VBlank is the limit
  (machine got loaded again mid-run). PS2_VU_VERIFY with build\vurel on m8_boss (debris chase),
  t8_open_head_intro, m8_turret (turret shooting): 7.3M + 8.4M + 8.4M programs, 0 mismatches.
- Audio of the hub run (-Wav, wav_analyze): music/sfx throughout, only silences at loads; a few
  moderate "clicks" in music (percussive content), nothing systematic.
- Cherry-picked the PS2_INPUT_FILE fix (a5a3c3f: lines of any length, one time origin per line).

- Session 4: merged main (VU recompiler) into next/ep8 (6cee235); release + debug rebuilt.
  New live tokens (debug tools): `+0:calls:<n>` (call profile of the next n VBlanks) and
  `+0:watch:<n>` (PS2_WATCH logs every call for n VBlanks).
- Boss Murray stage: circle near the beak = Murray jumps to a pink glowing grab point on the RIGHT
  side of the beak (letterboxed camera), holds it ~150-200 VBlanks, lets go. Mashing circle / all
  four buttons / holding circle / stick right / triangle during the hold: no effect. Guides call
  this step "Lift Clock-La's mouth" (ep3's lever: "press all the buttons rapidly"); ep3 lever
  (t3_heist4_intro) shows an objective banner + "Job Failed" hint, the beak shows nothing.
- Level state machines evaluate conditions in 0x1E3890(sm, cond): cond+0 = type (jump table
  0x2A7D90, types 3..0x5A); type 0x42 = button pressed (mask cond+0x10, via 0x187D80 on JOY
  0x2DFC00: +0x10B4 held, +0x10B8 pressed). 0x1E3890 is only called near/at the grab point.
  Global prompt object 0x4435F0 (string id, button mask) stays -1 during the grab.
- SOLVED (game mechanic, not a bug): Murray's pry (state 6 at actor+0x13D0, update 0x1B5820) is a
  button mash: anims 0x1CB..0x1CF are stages of 1.5 s; each needs int(4*1.5)=6 presses of any
  face button (mask 0xF0 via 0x1C46C0 on the press timestamps at joy+0x88) with gaps <= 1 s,
  else it falls back a stage (0x1CB -> 0x1CA = let go). Difficulty actor+0x1534 (beak: 2 = all
  5 stages, ~7.5 s of mashing after the ~2 s grab animation; door lift levels 0/1 fewer stages).
  Tables 0x2C0908 (next) / 0x2C0948 (fall back), timer 0x2C0990, rate 0x2C099C. Working input:
  `+0:circle:6,+60:none` + 200 x `,+5:square:2,+5:triangle:2` -> Murray stage won, Bentley
  stage starts (build\screenshots\ep8_boss_bentley_stage.png). Murray actor 0xF745D0 there.
- GAME COMPLETED (release, build\warp\bent2, PS2_POKE=3D4AE0=3C,3D4AC8=28): Bentley stage =
  triangle bombs at the two laser emitters on the mouth corners (right ~ (150,320), left ~
  (-100,300)); after the second: Clock-La cutscene, the Hate Chip appears on the lower jaw:
  jump in (cross) + circle -> "The Hate Chip" title, ending cutscene ("pick me up... I can't
  walk"), ending comic (Carmelita, Sly in handcuffs), credits (scrolling mask + voice credits),
  autosave (save1: episode.blimp.percent_complete 100), episode menu ("Anatomy for Disaster /
  Start Episode / 95%"). No runtime problem seen; frames build\screenshots\ep8_boss_*,
  ep8_hate_chip_title, ep8_ending_*, ep8_credits*, ep8_after_credits_menu.
  Bentley dies fast without held health -> death mask -> the stage restarts at Murray.

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
1. (done: boss won, ending, credits, autosave)
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
