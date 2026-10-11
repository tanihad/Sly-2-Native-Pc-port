# ep6: playtest Episode 6 "He Who Tames the Iron Horse" (Canada)

**Goal**: play Episode 6 end to end via the warp tool (hub + each job type), fix every runtime /
recompiler problem found at its root, with tests; frames in build\screenshots\ep6_*.png.

## Done
- Merged next/warp (warp tool) into next/ep6. Built build\rel and build\sly2.
- Hub (release, warp --episode 6): safehouse menu ("Press X to play as Sly", ticker text scrolls as
  in other episodes), X -> Sly at the station, walking, pause menu, Job Help (Cabin Crimes). OK.
- m6_recon direct start (debug): Binocucom briefing (Bentley: cabin, hook "lean back before you
  spring off the hook"), Sly on a rock below the cliff. OK.

- Direct starts (debug, build\warp\d_<task>, scratchpad batch6.ps1), all load and play, no log
  errors: t6_recon_enter_jb_cabin (world 28: Bison patrols, spots Sly -> "Job Failed", X retries),
  t6_carmelita_intro (Binocucom briefing, Sly in the snow), t6_paraglide_intro (briefing, ice-plane
  cutscene, paragliding, red spice-gas balloons, landing cutscene), t6_train_red_interior (world 31,
  Bentley drops through the roof hatch), t6_rc_chopper_battle_intro (world 29, cutscene, top-down
  RC chopper over the train, rockets), t6_kidnap_bearcubs_intro (Murray Binocucom, Murray on the
  bridge), t6_train_blue_interior (world 29, Sly in the caboose), t6_heist_train_1_intro (world 30,
  hand-car cutscene with subtitles).

## In progress
- Red train interior: right after landing Bentley takes damage (HUD icon flashes red, red outlines,
  knocked down the car). The wiki says car 1 has moving lasers; NO laser beams are visible in our
  frames (level has laser_beam_render_red / laser_beam_glow objects). Suspect: lasers not drawn.
  Tracing all primitives from v3140 (build\warp\red_trace2.txt, frames every 5 VBlanks).
  Trace result: the red outline is the game's own (character outline lines drawn with rgba
  96010180 instead of 10101080 = damage flash). Laser beams are additive untextured gouraud
  tristrips rgba 80310440/80311840 (ALPHA 0201); in that frame they are only at the far door
  (car 2's diamond lasers, z ~11800), none in car 1. Under trace the debug build ran 3-7 VBlanks
  per game frame; the game's dt comes from the COP0 Count (real time) -> large physics steps.
  Hypothesis: damage/knockback is a slowness artifact. Building -O2 WITH tracing (build\o2:
  build_game.py -O 2 --out build\o2) so -Direct works at near-release speed.
  RESULT: with build\o2 (lred2) Bentley lands without damage; walking forward through car 1 is
  fine, car 2 shows the red laser fan correctly (lasers render). Not reproducible -> not pursued.
  (Game clock 0x14D0B8: dt = Count ticks / 294.912 MHz clamped to [1/60, 1/30].)

- (session 1, after the last commit; recovered from build dirs) lmenu/lmenu5: episode menu, ep6
  intro movie and title card OK (screenshots ep6_episode_menu, ep6_intro_movie, ep6_title_card,
  ep6_complete_menu_ep7). lrc: RC chopper battle live (top-down over the train car, chopper is the
  small green/yellow model with a translucent rotor - renders). lblue: blue train caboose, walking.
- Session 2: merged next/warp again (entry hooks: PS2_WARP_TASK + @187D80 anchor in RELEASE builds,
  warp_run -Direct -Release). Scratch scripts now in scratchpad\ep6\ (scratchpad is shared).
- Release direct starts (build\warp\r_<task>, batch6.ps1 -Release): t6_heist_rc_chopper_1_intro
  (Neyla plane cutscene with subtitles, RC chopper over the train), t6_heist_rc_chopper_2_intro
  (Sly in the locomotive, Bentley/Murray hand car cutscene, chopper vs Neyla's plane, shots),
  t6_heist_train_2_intro (Sly on the moving train), t6_train_red_intro (Binocucom, Bentley chases
  Iron Horse 2, "Job Failed" when it leaves), t6_train_blue_intro (Binocucom, Sly on the cliff, train
  passes). All OK, no log errors. Every ep6 job start has now been seen.

- Live red train intro (lred3): briefing, "Job Failed" + X retry, Bentley jumps from the ledge to the
  passing train (lands beside the track; job not completed - platforming too timing-critical).
- Live RC chopper battle (lrc2, lrc3; controls: X = bomb, SQUARE = cannon): car 1 guards and the two
  side missile launchers (shells split into red bomblets) destroyed with bombs (green blasts, black
  smoke, debris), camera advances to car 2, enemy choppers attack (red shots, HUD ring drops).
  Job Help text OK. lrc2 (machine at 13-40 VBlanks/s, camera stayed zoomed in on car 1) never got
  enemies after the launchers (waited minutes; RAM dump build\lrc2\ram_stalled.bin); lrc3 at full
  speed progressed normally -> treat as unconfirmed (maybe my play: guards not all killed).

- Session 3 (recovered from build dirs after the user pause; notes were not checkpointed):
  committed PS2_POKE (debug aid: hold EE words every VBlank, e.g. 3D4AC8=28 = bentley.hpCur) and the
  PS2_INPUT_FILE reader fix (long lines / >4096 tokens silently ignored; unit test test_live_input).
  lbear: bear cubs (Murray) live, screenshot ep6_bearcubs_murray. lneyla/lneyla2/lneyla3: heist RC
  chopper 1 vs Neyla's plane (cannon hits her, her pink ring drops a little) -> "Job Failed" every
  time (RAM at failure: build\lneyla2\ram_failed.bin). lfin: heist RC chopper 2 (task 2965), also
  "Job Failed". Cause of the failures not established (play or bug?) -> Next 0.

- Session 4: merged main (VU recompiler, 4-7x cheaper VU). Reverted an uncommitted TEMP SD log
  (loop flags in VoiceTrans data; purpose not recorded). Analysis of session-3 runs: n3 (heist RC
  chopper 1, no input, 16 min) never fails; n1 dumps: chopper HP float at 0x500618 (100 -> 0 in
  steps of 20, 5 missile hits) and the job fails when it reaches 0 = normal game rule (wiki: shoot
  Neyla with SQUARE, avoid her missiles; stage 2: X bombs when she dives, SQUARE when she rises).
  So "Job Failed" = chopper shot down; next: hold 500618=42C80000 with PS2_POKE and keep shooting.

- p1 (release, heist chopper 1, chopper HP held): SQUARE hits drain Neyla's pink ring, small green
  planes appear, she retreats in flames, cutscene ("You rummy blighter", "Whatever you say Poodle"),
  control returns to Sly on train_2 = stage WORKS. Earlier failures were play (chopper shot down).
- Added PS2_POKE=@file (live poke list) + uppercase hex in lxHH tokens (commit d5dadaa).
- p2 (train_2 direct, sly.hpCur 3D4AB0=28 held): ladder, roof (trackside trees/poles and an eagle
  knock Sly; falling off respawns at checkpoints), flatcar with 3 lantern lumberjack guards - Sly
  gets knocked off repeatedly (play, not a bug). Not completed.
- p3 (chopper 2 direct, chopper HP cell 500618 = Splice var sym 9B7 held, same address as stage 1):
  Neyla dives (X bombs) and rises (SQUARE); her HP is a float at 0x5030A8 (200 -> 0). Defeated ->
  "Stand clear Sly" cutscene, furnace explodes, "Clockwerk's Stomach" loot card, 2D ending movie
  (Carmelita/Neyla/Sly), AUTOSAVE (save1: episode.train.percent_complete=100), Episode 7 slideshow
  and "Menace in the North, Eh?" start menu. Whole end-of-episode flow works.
  Perf note: in-level 45-58 VBlanks/s with GS thread 14-24 ms/VBlank on the shared machine (the GS
  thread is the known bottleneck; D3D11 renderer is the planned fix); movie/menus 60.

- p4 bear cubs live (with -Wav): Murray up the trestle, falls onto the job beacon below = job start
  ("Get the bear cubs inside the fence"); 0.56 s digital silence exactly at the black mask-wipe of
  the job start (transition, probably faithful); no other dropouts. Layout unknown, not finished.
- Session-4 direct-start batches (build\warp\s4_<task>, release, 240 s): recon get_blueprint_1/2/3
  (cabins 28: Bison sees Sly -> "You can't let Jean Bison see you"; lumberjack lantern guards),
  recon complete_blueprint_1/2/3 (hub, 1/3 and 2/3 counters), heist_intro (hand car cutscene, hub
  27 -> world 30 transition, Sly in the caboose), carmelita follow_complete (cage cutscene, 0/3 keys)
  and pickpocket_1/2/3_intro, paraglide need_2/need_1 (train counter). All load/play, no log errors.
- p5 RC chopper battle (Aerial Assault) live at 35-50 VBlanks/s (9 game instances, CPU 100%): 4
  guards and both launchers bombed, orange planes crossed, then NOTHING more for ~5 min (camera stays
  on car 1) - same symptom as lrc2 (slow machine); lrc3 at full speed progressed to car 2. RAM dump
  build\p5\frames\ram_028636.bin. Suspect timing-dependent stall -> rerun when the machine is quieter.

- RC chopper battle stall ANALYSED (p5/p6/p7, lrc2 RAM): player chopper = object 0xA98080 (vt
  0x2D5898 at +0xC, oid 50723; local pos +0x2B0, rig-frame pos +0x2F0), enemy choppers = two vt
  0x2D4B78 oid 51711 objects at 0xABE210/0xAC1E50 (pos +0x1F0, always z = 2850.0; two more parked
  at (0,-8724,2850)). The player's altitude (local z, 0xA98338) is set once when the chopper is
  deployed and differs per run: lrc2 1404, p5 2091, p6 2542 (all stalled: enemies hover ~300
  units above, our shots pass under them, they can't hurt us either) vs p7 2849.7 / 2833.5 / 2850.0
  (at the enemies' altitude: planes crash into the chopper -> "Your chopper got destroyed", i.e.
  the fight works). Game dt (0x2DDEE0, clamped to 1/30) sat at 1/30 in the slow runs: the game ran
  below 30 frames/s on this overloaded machine (9 instances, CPU 100%) during the deploy, so game
  time fell behind real time while the deploy dialogue stream plays in real time -> different
  deploy altitude. Faithful to PS2 behaviour at < 30 fps; not a port bug. Causality CONFIRMED:
  p8 (normal, ~18% of frames at the dt clamp) deploys at z 2826.4; p9 (identical, but process
  affinity 2 cores -> ~6-10 VBlanks/s) deploys at z 1353.5. The z jumps 0 -> final value in one
  step at deploy (v~4000-4140). Only remedy is speed (GS thread is the bottleneck, 14-17 ms/VBlank
  even before contention). n3 SD log: 5 voices on for 13+ min (ssa F8D60/102A50/105B80 = engine /
  rotor loops while their objects exist) - expected, not stuck.

- Session 5 (2026-10-11 08:30): merged main 2d00835 (EE thread fiber fix), rebuilt rel + debug.
  Machine quieter (1 other game instance). r1 = RC chopper battle rerun at full speed.
  Machine got loaded again (8-9 instances): r1 deployed at z 2461 (stall case again).
- AUDIO: "VoiceTrans to 00C040+1000 overwrites the block voice c0 v7" every 0.33 s right after
  the RC chopper battle level load (r1 14x, lrc3 375x over 2 min, r6 16x; not in r2-r5, r7).
  spiop music channel 1 = 5 voices, halves A = 0x9040+0x1000*i, B = 0x13040+0x1000*i (44.1 kHz,
  0.16 s per half), 989snd tracks the half from the first voice's NAX (func 0x12CCC in 989snd).
  r6 (richer report): v7 = channel 3 of the stream parked at its SSA 0C070 with PITCH 0 (env
  6FFF, sustain, LSAX = B 16040) while the other channels play -> one music layer never got
  its start pitch. Tested + reverted: SPU "LSAX written while on locks out loop-start flags"
  (no such event in this content). Added per-voice register history (KON/KOFF/PITCH/SSA/LSAX)
  to the overlap report (sd.c) -> r8/r9 to see who skips the pitch.
  RESOLVED (r11/r12): v7 is the level's WIND, a noise voice (NON 000080) keyed on without SSA/
  PITCH writes -> keeps the stale registers of its previous use as music channel 3. Harmless
  false positive. Commit 20ddfd9: check skips noise voices (+ unit test), report shows the voice
  history, PS2_LOG_SD levels 1 (+NON) / 2 (+PITCH/SSA/LSAX) / 3 (+VOLL/R).
- REAL audio problem found on the way: the 44.1 kHz music stream of the RC chopper level stops
  for good after 0.3 s .. 30 s (r5, r10-r13; lrc3 at full speed played 2+ min). 989snd sets
  LSAX=B in its first position check after the start; if the IOP runs > ~0.16 s late the voice
  loops into LSAX 0x5000 / its own half, 989snd's NAX check fails and the stream is stopped.
  The IOP is late because it only runs at EE safe points: 500-1400 of 2400 timer ticks per 10 s
  more than 8 ms late, worst 100-650 ms on this loaded machine. Fixes elsewhere: next/ep2 (and
  ep3 verbatim) keep the IOP polled while the EE waits for the GS (cherry-picked: 31bd93f);
  next/ep1 runs the IOP on its own OS thread (778e8e1, MMCSS) + async logging (d4dd3c8).
  With 31bd93f alone (r12/r13): still 500-700 late ticks, music stops after ~25 s.
  r14 = ep1's exe (copied to build\ep1rel) vs r15 = mine on the same start, -Wav, no SD log.
  RESULT (machine at 26-28 VBlanks/s): ep1's IOP thread: 0 late ticks (max 2.5-5.5 ms); mine
  (GS-wait hook): 69-98 late per 10 s, max 45-61 ms. r16 (ep1 exe + PS2_LOG_SD=1, async log):
  music refills continue 117 s+ after the level load (721 refills), vs ~25 s on my build.
  => ep6's music stop is fixed by next/ep1's IOP thread; nothing to do here (do not duplicate).
- Took ep8's PS2_TASK_SET/PS2_TASK_FILE tool verbatim (cherry-pick of ep1's b662346 = ep8 content,
  3cb9c82). g1: paraglide live (briefing "vacuum backpack... land on the caboose", glide, Sly
  explodes on hazards near the trestle -> retry; steering too slow to finish live).
- b1 (bear cubs direct + PS2_TASK_FILE): tasks 2932, 2933, 2937, 2938, 2939 -> 2 one by one;
  intermediate tasks do not save; job completion (2940 -> state 3) AUTOSAVED save1 (09:56:07):
  m6_kidnap_bearcubs.is_complete = 1, all t6_kidnap_* state 3, train percent 72. Murray then
  fights lumberjacks in the town (fire/explosions/coins render). Checkpoint saving works.
- b2: fresh boot on b1's game-written card, START -> Resume: Murray at the safehouse door (po 9)
  in the hub = the game-written save loads. (TRAP: never Stop-Process -Name sly2 - other agents'
  runs are named sly2 too; kill by unique name/path only.)
- t1: red train interior (Bentley HP held by PS2_POKE 3D4AC8=28): car 1 -> door -> car 2 rotating
  red laser fan renders right (screenshot ep6_train_red_laser_fan); Bentley knocked aside by
  lasers; game at the 1/30 dt clamp (machine). Not finished live.
- c1: recon blueprint 3 direct start + task file -> complete recon -> chalk talk 1 (cutscene type
  not yet seen in ep6).

## Next
0. RC chopper battle stall: rerun with less load, dump RAM often after the launchers; if it stalls
   again find what the wave logic waits for.
1. Deeper live play: bear cubs (Murray), paraglide, heist train_1 (lasers, Bison car), recon
   (cabin), train_blue; capture -Wav in one of them and analyse.
2. Perf when the machine is quieter.
1. Deeper live play in selected jobs (scratchpad\ep6\live6.ps1 -Release -Direct): bear cubs
   (Murray), paraglide, heist hand car.
3. Checkpoint saves (complete a job stage -> autosave on the card).
4. Perf: hub VU ~900k pairs/VBlank, blue train 720k (measure when the machine is quieter).

## Findings / traps
- PS2_INPUT_FILE "+n" tokens on one line are CHAINED (each relative to the previous token), not
  relative to the line start: "+0:lright:40,+5:cross" presses cross 5 frames after lright starts.
- Machine is shared by ~12 game instances: runs at 20-40 VBlanks/s (VU 25 ns/pair instead of ~11);
  perf numbers taken now are meaningless. Release START at pad frame 2400 can land in the attract
  (title not yet accepting) -> send START live instead.
- Task worlds: 27 hub, 28 Bison's cabin (recon), 29 blue train interior + RC chopper battle,
  30 heist train, 31 red train interior.
