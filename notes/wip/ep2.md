# ep2: playtest Episode 2 "A Starry Eyed Encounter" (India)

**Goal**: play episode 2 (hub world 8, jobs m2_*) as far as possible with warp + scripted/live
input; fix every runtime/recompiler problem found at its root, with tests; frames in
build\screenshots\ep2_*.png.

## Done
- Merged next/warp (178353c). Release build in build\rel, debug build in build\sly2.
- Episode 2 start (release, live input in build\live_ep2): title -> safehouse -> X -> Sly in the
  palace grounds hub; walking, pause menu, Job Help ("Recon the Ballroom") all fine.
- Direct starts (debug, warp_run -Direct, build\warp\ep2_<job>_direct\sheet.png): m2_recon,
  m2_turret, m2_rc_chopper, m2_dance_audition_fake (cutscene with subtitles), m2_lower_drawbridge,
  m2_steal_tuxedo (world 9): all load and play, no new log lines (only one
  "[sd] VoiceTrans ... overwrites the block voice c0 v4 is playing" at a level load).

- All 12 job starts load and play: also m2_dance_audition (dance minigame prompts, fails without
  input: "Job Failed!"), m2_boardroom_brawl (world 10; Sly swims in a pool - the game has
  swimming), m2_elephant_rampage, m2_heist_bomb_bridge (Binocucom briefing + camera tour, then
  Bentley), m2_heist_dance (world 11, dance), m2_heist_cover_murray (Clockwerk's Wings title card,
  RC chopper). t2_intro / t2_chalktalk1/3 direct: just Sly at the safehouse (triggered elsewhere).
- Live (scripts\live_warp.ps1, new): RC chopper flies, bombs drop (smoke). Recon in the ballroom
  (t2_recon_enter_ballroom): Binocucom view, R1 photos with shutter, the game judges framing
  ("Too far, use right analog stick zoom in" / "Bad framing, point at object"); Binocucom Y aim is
  inverted. Autosave writes save1 after level loads in the hub.

- Job completion end to end: m2_dance_audition_fake (cutscene at the ballroom door) completes,
  the game returns to the hub, autosaves (warp.py dump: m2_dance_audition_fake.is_complete 1,
  t2_steal_tuxedo_intro.state 1 = next job open).
- Turret (t2_turret_attack_chopper direct, anchored input): Murray's turret aims and fires
  (muzzle flashes), "Job Failed! You must destroy the chopper." when not destroyed.
- Dance minigame (m2_dance_audition, dumps every 10 frames): a memory game - three button prompts
  are shown (e.g. triangle triangle triangle, then triangle triangle circle), then "? ? ?" while the
  player repeats them; no input -> "Oow, my feet can't take any more", Job Failed.

## In progress
- movie: episode 2 at 100% -> episode menu (dvd_menu world) -> "Watch a Movie" (IPU).

## Next
1. Dance minigame: scripted presses during "? ? ?" (timing in real time? game time?).
2. Audio run (-Wav + PS2_LOG_SND, release hub) - first attempt starved by machine load.
3. Perf: machine heavily loaded by other agents (~12 sly2.exe at once; release hub ran at 15-27
   VBlanks/s, VU 25-50 ns/pair) - hub VU load 850-970k pairs/VBlank ~ the intro cutscene (60 fps
   with 2 ms headroom on a quiet machine).

## Findings / traps
