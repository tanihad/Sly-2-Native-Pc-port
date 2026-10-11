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

## Next
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
