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
- Second complete run with -Wav (build\warp\bossrun): ending + credits audio (455-875 s) clean:
  no dropouts inside sound, clicks only on percussive transients (explosions), peaks -0.6 dBFS
  in the comic/credits music, 1 underrun in the whole run (machine at 100% CPU).
- Turret (m8_turret, release): shooting works (blue shots, overheat bar), missiles hit the
  chopper, "Job Failed!" box (no hint line: the level has no hint string; default "" is right).
- Mega-jump (m8_megajump): R2 launches Sly high over the blimp and lands; 0/4 switches.
- jt door (t8_engine_room_jt_lift): circle at the door didn't grab from where I stood (Murray
  punched instead); same pry code as the beak (levels 0/1), not pursued.
- NEW scripts\ep8_finale.ps1 (+ scripts\live_waitpos.py): unattended final boss -> Hate Chip ->
  ending -> credits -> autosave (exit 0), optional -Wav: a regression run for the end of the game.

- Session 5: merged main 2d00835 (threads fiber-leak fix); rebuilt rel + debug. Hub PS2_PERF
  (release, 4 other games at ~5 cores each, CPU 99%): EE busy 6-11 ms/VBlank, VU 100% recompiled
  4-6 ns/pair, GS thread 15-20 ms/VBlank (25-38 when more load) -> 45-56 VBlanks/s; title and
  intro in the same run had GS 13-16 ms: GS-bound under load, as in ep5 (not ep8-specific).
- Recon in context (direct t8_recon_switch_slowdown, world 39, release, build\warp\s9_recon):
  Sly at a switch box, HUD 0/4; PS2_TASK_FILE 3366/3368/3370 -> HUD 3/4, 3372 -> the game's own
  flow: post-recon confrontation cutscene (Neyla, "Sly Cooper!? Of course this'd be your doing",
  Arpeggio in his cage explaining the hypnosis plan), Clock-La revealed, autosave icon, chalk talk 1
  (chalk sketch over the blimp interior), safehouse. Autosave: blimp 25%, m8_recon/m8_post_recon
  complete, t8_chalktalk1 3, tntbarrel/jt/bentley/murray intros active. All as the game intends.
- TNT run (direct m8_tntbarrel, warp --complete m8_engine_room --complete t8_engine_room,
  build\warp\s9_tnt): briefing ("walk one of these TNT barrels up to that engine room"), Sly on the
  blimp 0/3; fell off once (respawn), spotted by a searchlight (alarm). PS2_TASK_FILE 3382-3384 ->
  the game completes destroy_engine_room 3385, Sly carries the charged barrel ("weak point marked
  in your binocucom, now get to the engine room"); 3386:2 -> chalk talk 2 (film-look planning
  sequence: chalk circles on the blimp, helicopter, Clock-La, missiles) -> safehouse. Autosave
  blimp 81%, m8_tntbarrel complete, t8_chalktalk2 3, t8_megajump_intro 1.
  build\screenshots\ep8_chalktalk2_*.png, ep8_after_chalktalk2_safehouse.png.
- Live frames: PS2_DUMP_RING=200 x DumpEvery 30 keeps only ~2 min: copy frames right away.
- Mega-jump in context (direct m8_megajump, build\warp\s9_mj): 3456/3458/3460 -> HUD 3/4, 3462 ->
  the game's flow: switch towers power up (cutscene), "Job Completed!" (Sly), autosave, turret
  intro cutscene (Carmelita: "I need an experienced tail gunner"), turret gameplay.
  ep8_megajump_complete_to_turret_sheet.png.
- Stuck "y|" subtitle under a letterboxed "Job Failed!" (s9_mj) = my own doing: the turret job had
  already failed (idle, ~40 s) when I set 3469:2; the completion cutscene started behind the
  paused failure box. Repro without the tool (s9_tur): normal failure box over the turret view.
- Turret in context (direct m8_turret, 3469:2 while playing, build\warp\s9_tur2): completion
  cutscene ("You should have been a cop! That was some terrific shooting!"), blimp explodes,
  Bentley "Clock-La is looking mighty upset", Sly-mask load of world 43, debris-fall gameplay.
  => natural chain recon -> chalk talk 1 -> TNT -> chalk talk 2 -> mega-jump -> turret -> boss
  (-> ending, session 4) verified. ep8_turret_complete_to_boss_sheet.png.
- Per-instance CPU: other agents' sly2 instances use 5-6.5 cores each (pixel workers spin 20000
  pauses before WaitOnAddress, gs_draw_pixel.c WORKER_SPINS); with 4-6 games the machine is at
  99% and every game is GS-bound (GS thread 20-90 ms/VBlank in s9_tur). Not changed (generic).
  My own release instance in the debris fall: 3-4.5 cores, of which pixel work 1.2-2.0 s/s
  (PS2_GS_STATS), critical path 0.5-0.7 s/s: spinning is ~1 core, not the main cost.
- GS cost of ep8 scenes vs the reference (gs_replay -Bench, 8 workers, interleaved, machine at
  100%): hub walk (build\gsrec\hub.gsrec, 240 VBlanks) 14.6-22.2 ms/VBlank, debris fall
  (boss.gsrec) 25.6-31.9, fix-perf briefing (holds 60 when quiet: 8.9) 27.5-44.1 => ep8 scenes need
  0.5-0.8x the briefing's GS work: they hold 60 where the briefing does. EE side (perf_hub2): busy
  6-11 ms/VBlank under load, VU 4-6 ns/pair. Slowdowns seen here are machine load only.
- Murray engine room (direct t8_engine_room_murray_lift_complete, world 40, build\warp\s9_mur):
  Murray above the engine room with 8 red switch pedestals and spark enemies (they shock him).
  3439-3447:2 -> the game completes destroy_switches 3448 (cutscene) and activates 3449; 3449:2
  set directly shows nothing (that job's end is level-script driven; skipped). Flop not played.
- Turret audio (-Wav, s9_tur/s9_tursnd): music/explosions until "Job Failed!", then 116-203 s of
  digital silence. PS2_LOG_SND shows the game's own fade: 989snd cmd 9 (master volume, groups 0/1)
  0x3DD -> 0 over v6684-6721, then SUKR proc 3 on stream channels 3/4 (pause) => intended.
  6 underruns in 410 s at 100% CPU, clicks only on explosion transients.
- A run of mine died at v73 with a truncated log line (no crash report; not reproducible): my
  perfrun.ps1 used the exe name sly2.exe, which other agents stop by name. Helpers now always copy
  the exe as sly2_ep8_<name>.exe (stage_end.ps1, perfrun.ps1 in build\ep8tools).
- Bentley engine room (direct t8_engine_room_bentley_open_door, world 41, s9_ben): Bentley at the
  exit door ("Warning! Using this passageway will abandon your current job!"); 3414/3416:2 ->
  cutscene, boxes stage (17551) starts; 3417:2 leaves 3418 active (needs play). No problems.
- Hack station (hack_path2): Bentley at the wall near the station, searchlight alert; terminal
  prompt not found by blind steering (prompt object 0x4435F0 stays -1). The minigame itself is
  verified in ep5 (same code); not pursued.
- Regression after the main merge: scripts\ep8_finale.ps1 -Release -Wav (build\warp\finale3):
  boss Murray + Bentley stages, Hate Chip, autosave (blimp 100%), ending + credits 8 min, episode
  menu; exit 0, 1 underrun in 1100 s, log clean. All unit tests + run_tests.py pass.

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
