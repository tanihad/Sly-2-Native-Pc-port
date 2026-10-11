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

## In progress
- fake_long (dance_audition_fake to its end, job completion + autosave), audio\hub (-Wav + PS2_LOG_SND
  release hub), turret_attack (t2_turret_attack_chopper with inputs), elephant2.

## Next
1. Dance minigame: dense dumps, work out prompts, try scripted presses.
2. Audio analysis (wav_analyze.py, audio_streams.py) of the hub run.
3. Perf: machine heavily loaded by other agents (~12 sly2.exe at once; release hub ran at 15-27
   VBlanks/s, VU 25-50 ns/pair) - hub VU load 850-970k pairs/VBlank ~ the intro cutscene (60 fps
   with 2 ms headroom on a quiet machine).

## Findings / traps
