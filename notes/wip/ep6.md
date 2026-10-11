# ep6: playtest Episode 6 "He Who Tames the Iron Horse" (Canada)

**Goal**: play Episode 6 end to end via the warp tool (hub + each job type), fix every runtime /
recompiler problem found at its root, with tests; frames in build\screenshots\ep6_*.png.

## Done
- Merged next/warp (warp tool) into next/ep6. Built build\rel and build\sly2.
- Hub (release, warp --episode 6): safehouse menu ("Press X to play as Sly", ticker text scrolls as
  in other episodes), X -> Sly at the station, walking, pause menu, Job Help (Cabin Crimes). OK.
- m6_recon direct start (debug): Binocucom briefing (Bentley: cabin, hook "lean back before you
  spring off the hook"), Sly on a rock below the cliff. OK.

## In progress
- Background batches of direct starts (scratchpad batch6.ps1 -> build\warp\d_<task>):
  A: t6_recon_enter_jb_cabin, t6_carmelita_intro, t6_rc_chopper_battle_intro, t6_kidnap_bearcubs_intro
  B: t6_train_red_interior, t6_paraglide_intro, t6_train_blue_interior, t6_heist_train_1_intro

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
