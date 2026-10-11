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

- Episode menu + movies (live_warp -Set episode.palace.percent_complete=100, release): resume opens
  the van episode wheel (Episode 3 preselected, up: Episode 2 -> "A Starry Eyed Encounter, X: Start
  Episode, 95%"); the wheel's top entry is "Watch a Movie": a film strip of unlocked movies;
  "Prelude" plays as an IPU movie (Neyla/Carmelita, Clockwerk wings) with sound.

  Movies end and return to the film strip; Episode 2 "The Setup" (Rajan's backstory) plays at
  59-60 VBlanks/s with sound (build\screenshots\ep2_movie_*.png, ep2_episode_menu.png).
- BUG (audio): the episode menu's music stream (spiop -> 989snd VAG stream, 44.1 kHz, 2 voices,
  half buffers 0x1000 at SPU 9040/13040 (L) and A040/14040 (R)) stops for good a few seconds after
  it starts (hard cut to silence; also after a movie). Root cause chain (PS2_LOG_SD + temporary
  caller/NAX logging): 989snd's stream tick (989snd 0x12CCC) watches NAX; the voice reached the end
  of half A while LSAX still pointed at A (989snd sets LSAX to the other half after it sees the
  half change, 0x12EC8), so the voice looped inside A; 989snd sees NAX go backwards inside the same
  half (0x13058) -> error 0x37 -> stream stopped (0x1255C -> KOFF v0/v1). The IOP was 130 ms late
  (one half = 162 ms). IOP gaps measured (temporary PS2_IOP_GAP instrument): 1-2 s while the EE
  thread waits for the GS thread (gswait: gs_thread.c ee_wait_change does not run the IOP) and
  50-300 ms "EE" gaps; the machine is saturated (13 sly2 instances, CPU 100%).

## In progress
- Fix: run the IOP while the EE thread waits for the GS thread (gs_set_wait_hook -> ps2_poll_iop),
  unit test in gs-draw (TEST_ASYNC: present blocks until the hook ran).

## Next
1. Dance minigame: scripted presses during "? ? ?" (timing in real time? game time?).
2. Audio run (-Wav + PS2_LOG_SND, release hub) - first attempt starved by machine load.
3. Perf: machine heavily loaded by other agents (~12 sly2.exe at once; release hub ran at 15-27
   VBlanks/s, VU 25-50 ns/pair) - hub VU load 850-970k pairs/VBlank ~ the intro cutscene (60 fps
   with 2 ms headroom on a quiet machine).

## Findings / traps
