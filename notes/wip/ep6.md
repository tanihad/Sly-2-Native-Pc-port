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

## Next
1. Review batch frames/logs for each job; then deeper live play in selected jobs
   (scratchpad live6.ps1 = warp card + live_game.ps1 with PS2_WARP_TASK).
2. Remaining direct starts: t6_heist_rc_chopper_1_intro, t6_heist_train_2_intro,
   t6_heist_rc_chopper_2_intro, t6_train_red_intro (hub part), t6_train_blue_intro.
3. Episode intro (t6_intro active), episode-menu movie, checkpoint saves.
4. Perf: hub VU ~900k pairs/VBlank (measure when the machine is quieter).

## Findings / traps
- Machine is shared by ~12 game instances: runs at 20-40 VBlanks/s (VU 25 ns/pair instead of ~11);
  perf numbers taken now are meaningless. Release START at pad frame 2400 can land in the attract
  (title not yet accepting) -> send START live instead.
- Task worlds: 27 hub, 28 Bison's cabin (recon), 29 blue train interior + RC chopper battle,
  30 heist train, 31 red train interior.
