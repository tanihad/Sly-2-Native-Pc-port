# ep3: playtest Episode 3 "The Predator Awakens" (India, jungle)

**Goal**: play Episode 3 (hub world 12 jungle, world 13 spice factory interior) end to end via
warp saves / direct job starts; fix every runtime/recompiler problem found at its root.

## Done
- Merged next/warp (178353c). Release build (build\rel) and debug build (build\sly2) OK.
- Hub (resume -> safehouse -> X): Sly in the rainy jungle, walking, pause menu: OK. Autosave into
  slot 1 after the hub load (save1 rewritten ~70 s after start).
- Direct starts (debug, build\warp\d_<task>\, 300 s each, 2 lanes): every one loads and renders
  plausibly, no log errors (only the known boot-time syscall 0x5B / eret lines):
  t3_recon_int_intro (world 13 interior), t3_waterbug_intro (pick up the bug at the fountain),
  t3_free_elephant_intro (Binocucom briefing), t3_lead_rajan_intro (Bentley, Binocucom + gameplay
  with sonic darts HUD), t3_turret_intro (cutscene: Bentley's chopper bursts out of the truck, then
  the gunship turret view), t3_steal_ruby_intro (Sly under the ruby), t3_heartneela_intro (Neyla
  cutscene), t3_heartneela_int_checkpoint (world 13), t3_tntbarrel_intro_ext (world 13, Binocucom
  + TNT barrel), t3_heist1_intro (Murray), t3_heist3_intro (Bentley turret vs hunter-seeker bombs,
  fails without input -> "Job Failed" screen), t3_heist4_intro (lever mash), t3_heist5_intro (Sly,
  TNT into the elephant's mouth), t3_boss_sly_capture (cutscene: Murray jumps into Rajan's pit).
- Played live (PS2_INPUT_FILE): recon start (guards spot Sly, fight), waterbug (picked up the bug),
  heist4 lever mash (scripted button mash -> lever lifted -> heist5 started), boss fight (Murray vs
  Rajan: punches, lightning water attacks, hit effects), turret (gunship fires with R1, overheat
  bar; the dam was not found -> "Job Failed! You must destroy the dam", retry works).
- Episode menu (save with episode.temple.percent_complete=100): shows Episode 4 "Jailbreak",
  Start Episode -> episode 4 intro comic cutscene plays (the ep3 -> ep4 transition works).
- Frames: build\screenshots\ep3_*.png.
- Heist stage changes do not autosave (PS2_WATCH=1D84B0,164DF0: 0x164DF0 state changes during
  heist4 -> heist5, no 0x1D84B0 save call) - as in the game, which saves at job/episode ends.
- Episode end: direct start of t3_boss_complete (2342) -> Neyla/Carmelita/Contessa cutscene ->
  autosave (save1: episode.temple.percent_complete 100, episodeBest 3, t3_complete state 1) ->
  episode 4 title card. Mid-job tasks without a start warp (-1) just resume Sly on the hub hill.
- Merged next/warp again (7729410: entry hooks, PS2_WARP_TASK and @187D80 anchors in release).
- Cherry-picked from next/ep7: PS2_POKE (11b4ac8) and the PS2_INPUT_FILE long-session fix (1df4920:
  a live session stopped taking input after 4096 tokens) - identical patches, merge cleanly.
- Audio, hub (release, -Wav, build\audio_hub): continuous music/rain, no underruns, silences only
  at the safehouse->hub load; one small click at 19.4 s (title). OK.
- Turret (Blow the Dam), live: the chopper flies a fixed path, turret yaw is limited (~30 deg
  right), R1 fires (bottom bar = gun heat, overheat works); top-left "C" gauge darkens with damage;
  without shooting back the chopper is destroyed after ~50 s of game time -> "Job Failed! You must
  destroy the dam" (legit). A full-width blue-gray band seen once = the Job Failed box's opening
  animation (GS trace: untextured tristrips 334C6680 at UI z, x -317..219 -> -10..526). Not a bug.
  Chopper health is not bentley.hpCur (0x3D4AC8 stays 0x28); address not found yet.
- FIXED boot hang under load (1 in ~100 headless runs at 100% CPU, build\tools\bootloop.ps1):
  pixel workers read pool.gen when their thread first ran; a worker starting after the first job
  (first VBlank picture) skipped it, and if all did, the picture was never drawn and
  gs_queue_vsync waited forever. Workers now start from the generation at creation (gen0).
  Unit test gs-draw test_gs_draw_late_workers (GS_TEST_WORKER_DELAY_MS=300) fails without the fix.
  24 boot runs (6 parallel) after the fix: 0 hangs.

- Session 3 (2026-10-11): committed PS2_POKE_FILE (fa2cf07), merged main (ca9f1d3: VU recompiler);
  release + debug builds OK, all unit tests + run_tests.py pass after the merge.
- Live input token `pause` (204385d, test window test_live_pause): the EE waits in the pad read at
  the next dumped VBlank until the next input line. Needed because a look-and-steer round trip
  takes 10-40 s of game time (the turret attempt is over in 53 s). build\tools\live3.ps1 now takes
  -Ring (build\tools\live_game2.ps1 = scripts\live_game.ps1 with a ring size parameter).
- Turret: lower-case stick hex only (lxFF was dropped as an unknown button). lxff turns the view
  right; the "dam" ticker reads "Destroy the dam."; the attempt fails after ~53 s every time.
  With pause: firing (green tracers, heat bar), enemy gunships + ground fire, retry all work; the
  dam was not found/destroyed (game skill, not a runtime issue). Game clock (0x14D0B8): dt from
  COP0 Count, clamped to [1/60, 1/30] s, so a pause costs at most one 1/30 s step.
- Recon (live, release, t3_recon_int_intro direct + PS2_TASK_FILE 2174/2175/2176 -> 2): Binocucom
  camera mode (checklist Rajan / Crane Controls / Clockwerk's Heart / Entrance, R1 shutter effect,
  "No item of interest in view", target brackets, "Object hidden, move to new location") works;
  spytargets 2178-2181 -> 2: all ticked, Sly's "bring those photos back" cutscene, job-complete
  book splash, chalk talk 1 MPEG movie (pre-rendered guard/crane/fountain scenes, clean), load,
  safehouse "Press X to play as Sly"; autosave save1 (recon_complete 3, chalktalk1 3, temple 17%).
  Frames ep3_recon_spycam, ep3_recon_complete_book, ep3_chalktalk1_movie, ep3_safehouse_after_recon.

## Next
1. Perf: machine is shared by ~15 game instances (100% CPU), so PS2_PERF numbers are inflated
   (VU 24-27 ns/pair instead of ~11). GS replay, interleaved, same load: title (build\gsrec\
   brief.gsrec, actually the title menu) 21-24 ms/VBlank vs world-13 interior (recon_int.gsrec)
   30-36 ms -> ~1.45x the title (title = 7.0 ms unloaded -> ~10 ms?). In-game release run in the
   interior under load: 22-30 VBlanks/s, same as the briefing baseline in the same minute (both
   load-bound). Re-measure when the machine is quiet.
2. Audio: -Wav in the hub (rain ambience, music).
3. More gameplay: turret dam, boss to the end (t3_complete -> episode end).

## Findings / traps
- Episode 3 tasks: scripts\warp_tasks.txt (t3_*), oids 2167-2350. Level names in the save schema:
  episode.temple.level.exterior / interior.
- The scratchpad directory is shared with other agents (ep4 overwrote batch.ps1): keep helper
  scripts in build\tools\ of this worktree (live3.ps1, ep3batch.ps1, strs.py).
- Under load the release build's fixed START frame (2400) comes before the title accepts input;
  use the debug build's anchor or a live START.
- Dialogue/objective text is plain ASCII in RAM: build\tools\strs.py <ram dump> <regex>.
