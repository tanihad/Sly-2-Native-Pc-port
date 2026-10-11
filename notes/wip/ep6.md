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

## Next
1. Deeper live play in selected jobs (scratchpad\ep6\live6.ps1 -Release -Direct): red train
   (catch the caboose, interior lasers), RC chopper battle, bear cubs (Murray), paraglide, heist.
3. Checkpoint saves (complete a job stage -> autosave on the card).
4. Perf: hub VU ~900k pairs/VBlank, blue train 720k (measure when the machine is quieter).

## Findings / traps
- Machine is shared by ~12 game instances: runs at 20-40 VBlanks/s (VU 25 ns/pair instead of ~11);
  perf numbers taken now are meaningless. Release START at pad frame 2400 can land in the attract
  (title not yet accepting) -> send START live instead.
- Task worlds: 27 hub, 28 Bison's cabin (recon), 29 blue train interior + RC chopper battle,
  30 heist train, 31 red train interior.
