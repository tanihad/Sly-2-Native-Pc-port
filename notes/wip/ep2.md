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

- FIX (verified): run the IOP while the EE thread waits for the GS thread (gs_set_wait_hook ->
  ps2_poll_iop), unit test in gs-draw (TEST_ASYNC t_wait_hook: present blocks until the hook ran).
  Also gs_draw_pixel.c: a pixel worker that starts after the first job was posted skipped it
  (seen = pool.gen at start) -> starts from generation 0 now (found by that test). Merged main (VU
  recompiler) on 2026-10-11; release + debug build, all unit tests and run_tests.py pass.
  In game (build\live\menufix, menugap with temporary instrumentation build\tools\temp_gap2.patch):
  no IOP gap from GS waits any more (124 GS waits > 150 ms in 160 s, IOP serviced through them);
  the remaining IOP gaps (150-660 ms) have 0-90 ms of EE thread CPU time = the OS not running the
  EE thread (machine at 100%, ~10 sly2 instances). menufix's stream still stopped once at 328 s
  (EE stall 1.3 s at the same time). That cause is next/ep1's (mixer holds the voices while the
  IOP has not run, PS2_AUDIO_IOP_LIMIT) - not duplicated here. menugap: the menu music played
  for 6+ minutes to the end of the run (before the fix it stopped within seconds).
- Episode end (release, warp_run -Job t2_heist_complete -Direct, build\warp\ep2_heist_complete_direct):
  van escape cutscene ("Punch it!" subtitles), "Job Completed!" notebook, Sly-logo wipe, the
  illustrated Episode 3 story (Rajan, the jungle, the eyes), van + logo, then the episode wheel
  with Episode 3 selected. The game saved (save1: palace 100%, m2_heist_cover_murray complete,
  t2_done state 1). No new log lines. Frames ep2_end_*.png.

- Dance minigame (m2_dance_audition): input works. Rules seen: the dancer shows 3 prompts (lit dot
  of a button diamond: top/green = triangle, right/red = circle), then "? ? ?" = the player's turn;
  a press during the show phase fails at once ("Oow, my feet..."); 3 correct presses right after
  "? ? ?" appears (12 frames apart) pass the round (the icons are revealed one by one). The phase
  start drifts +-30 frames between runs (loading time), so fixed scripted frames are unreliable:
  build\tools\dance_bot.py (not committed) reads the prompt band from frames\latest.png every 2
  VBlanks and answers through the live input file; build\tools\dance_live.ps1 starts it.
  Bot results (dl2-dl4, release): rounds 1 (TTT) and 2 (TTO) passed, round 3 (O T X) failed; a
  press made before the previous slot's reveal counts for that slot (double press = fail). The
  game ran at ~25 VBlanks/s on the overloaded machine while the music plays in real time, and the
  live input file is only read every 8 pad frames (0.3 s at that speed), so the bot's timing
  (0.3-0.5 s) is about one beat: not reliable enough to finish. No runtime problem seen (inputs,
  prompts, reveals, failure path all behave). Frames ep2_dance_*.png.
  Completion-task direct starts: t2_dance_audition_complete -> Sly at the hub safehouse (no
  cutscene); t2_bomb_bridge_lower_collapse -> 2104:8 = Bentley on the upper bridge, debris falling.

- Recon photo (live, release, build\tools\live_direct.ps1 -Job t2_recon_enter_ballroom): R3
  Binocucom, left stick aims (both axes slow at first; Y inverted), right stick zooms; a target
  gets a gold circle when in view, "Too far" until zoomed in, then a green circle with its name;
  R1 then takes the photo: shutter, a Polaroid of the captured scene ("Clockwerk's Wing", the
  render-to-texture copy is correct), back to the Binocucom with the list item ticked (red check).
  One R1 of 4 frames with the green circle did nothing; the next (10 frames) worked. Frames
  ep2_recon_*.png.

- Boardroom brawl start (live brawl2): Sly starts in a pool (the job's start warp), swims with
  the left stick (position changes, upright swim animation, underwater part tinted), camera turns;
  did not find the way out of the pool in a few tries. Elephant rampage (live ele): Job Help text
  "Break Rajan's prize elephants out of their pens..."; walking in the hub fine.

- 2026-10-11 (session 3): merged main 2d00835 (thread churn fix). Hub run (release, live hubperf,
  PS2_PERF + -Wav, 2 other sly2 running, ~90% CPU): walking, combat with guards fine; 49-58
  VBlanks/s, flips 33-50/s, EE busy 9-13 ms, GS thread 14-17 ms/VBlank = the limit (known, main's
  notes: GS thread ~16.4 ms, fix = Direct3D 11 renderer; not duplicated here). VU 600-820k
  pairs/VBlank at 4-5 ns, memory flat 149 MB. Audio (234 s wav): continuous from 48.6 s (hub) to
  the end, 0 dropouts, 5 small clicks, no clipping.

- Bomb the Bridge (live bomb, release): Bentley walks the ledge to the bridge (clue bottle 1/30),
  HUD 0/9 cleats; TRIANGLE at a cleat plants a bomb (red pulsing light), it explodes (smoke,
  debris), counter 1/9. Circle/R1 do nothing for Bentley here. ep2_bomb_*.png.
- Elephant rampage (t2_elephant_rampage_intro_2 direct): Binocucom briefing ("snatch the headdress
  jewels worn by both elephants"), 0/6 jewels HUD, two red light beams mark the elephants across the
  moat; falling into the water respawns Sly. Did not reach the elephants (hub navigation blind).
- Monkey run (build\tools\mk.ps1 = warp direct start + random input, monkey.py) on
  t2_heist_cover_murray: RC chopper covers Murray, bombs, Job Failed/retry, hints; no log anomaly.
  Speed 40-58 VBlanks/s, GS thread 15-17 ms (machine 100%, 7 other games).
- Cherry-picked the shared debug tools (identical patches on ep3/ep4/ep6/ep7/ep8): PS2_POKE,
  PS2_INPUT_FILE any length, PS2_TASK_SET/PS2_TASK_FILE, PS2_POKE_FILE, live 'pause' token,
  case-insensitive tokens (ep3 order; notes conflict resolved). Release rebuilt, all unit tests +
  run_tests.py pass.

- FIX (debug tool): live 'pause' now freezes the whole PS2 (ps2_debug_hold: VBlank schedule moved
  by the pause, EE timers timer_hold, IOP time iop_time_hold, SPU2 mixer sd_hold). Found with the
  dance: Sly 2's frame time comes from T2 and 989snd's music tick from the IOP hard timer, both
  real time, so each step saw the wait as elapsed time; and a VBlank fell due at once on every
  resume (2-frame steps held ~1 frame of time). Commits 6df1c88, 29ef294 (+ VBlank shift, pending
  commit). Tests: timer debug_pause_hold, sd debug_hold, iop_kernel time_hold, window live_pause.
- Dance mechanics (paused stepping, build\tools\step_dance2/3.py, dance_track.py = blue beat
  cursor x + prompt classification per frame): show phase = cursor walks slots 0-2 one beat
  (~13 VBlanks) each while the partner's moves light up; then "? ? ?" (Q) and the cursor walks
  again; icons get revealed in the answer phase (slot 0 at Q+2..4 in every run, even with no
  press). Fixed offsets (Q+14/29/44, Q+16/31/46, Q+18/33/48) pass round 1 at best, round 2 fails.
  The dance does not use the pad test 0x187D80 (PS2_WATCH): only the cutscene skip 0x211D38 does.
  Pad object 0x2DFC00: +0x10B4 held, +0x10B8 pressed (latched until consumed by 0x187DE0), +0x10BC.

- 2026-10-12 (session 4): merged main 411bb22 (notes only). Stepped dance runs after the VBlank
  shift: dR4 (quiet machine) ball lands on the slots ~33 VBlanks apart (= real time, 0.55 s), dA1
  (loaded machine) 12-20 VBlanks apart -> a time source still ran during the pause.
- FIX: COP0 Count (ps2_mfc0 reg 9) came from raw host time; it is now timer_cop0_count() on timer.c's
  held time base (2 x bus cycles). Test timer cop0_count_hold; run_tests.py and timer suite pass.
  Commit 89e93a6. (The game measures real time with Count: dialogue, the dance beat.)
- Dance rules (dR4 + dA1 + the real-time runs): the icon reveal of slot k is not the cue. With no
  press the reveal comes when the blue ball lands on the slot (the beat); a press reveals the icon
  ~12 VBlanks later; pressing after a slot's auto-reveal counts for the next slot (dR4: three late
  presses = an extra press -> fail after slot 2); pressing all three right after "? ? ?" 12-16
  VBlanks apart (dA1) failed in round 2. Probably a rhythm window around each landing; the stepped
  bot is step_dance4.py (press slot 0 at Q, slot k+1 at reveal k; frames must be stable 2 steps -
  the loading screen produced fake prompts). Under 13 game instances the stepping runs at 2
  VBlanks/s (each step waits for the GS thread's PNG), too slow to finish a dance.
- Job flows through PS2_TASK_FILE (build\tools\taskflow.ps1 = live_direct + task lines + frames):
  m2_lower_drawbridge (dbr2): keys 2004-2008 -> HUD 5/5, keys_complete 2009 auto 2, 2010 active;
  2010:2 -> "Job Completed!" notebook, back to the hub, 2010 state 3, t2_turret_intro 2013 active,
  autosave written. Frame ep2_drawbridge_completed.png. Setting 2013:2 in the same session only
  activates 2014 (Sly stays in the hub): each job needs its own direct start.

## Next
1. Flows (taskflow.ps1): elephant (ele2), tuxedo (tux2) running; then recon, bomb bridge (lower ->
   collapse -> upper), boardroom brawl, rc chopper, turret, heist dance, dance audition (2059:2).
2. Dance minigame: longer stepped run (-Every 4, 60 min) when the machine is quieter; try pressing
   when the ball lands (cursor x near the slot, wide y band) if the anticipating rule fails again.
3. Play deeper: m2_heist_bomb_bridge (Bentley), m2_heist_dance (tango), boardroom brawl past the pool.

## Findings / traps
