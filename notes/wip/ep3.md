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

## In progress
- Does the game try to autosave at heist stage changes? (heist4 -> heist5 did not rewrite save1;
  direct starts never rewrite it). Run with PS2_WATCH=1D84B0,164DF0 (build\warp\watch_t3_heist4_intro).

## Next
1. Perf: machine is shared by ~15 game instances (100% CPU), so PS2_PERF numbers are inflated
   (VU 24-27 ns/pair instead of ~11). GS replay of the world-13 interior (build\gsrec\recon_int.gsrec,
   240 VBlanks): 69 ms/VBlank vs 40 ms for fix-perf's rv_brief.gsrec in the same conditions ->
   ~1.7x the briefing's GS cost (~15 ms/VBlank unloaded?). Re-measure when the machine is quiet.
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
