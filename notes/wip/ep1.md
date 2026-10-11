# ep1: playtest Episode 1 "The Black Chateau" (Paris)

**Goal**: play Episode 1 end to end (hub + every job type) via warp saves and live input; fix every
runtime/recompiler problem found at its root (with tests); frames in build\screenshots\ep1_*.png.

## Done
- Merged next/warp (warp tool: scripts\warp_run.ps1, scripts\warp.py, notes/warp.md).
- Built debug (build\sly2) and release (build\rel).
- Episode start (release, live): title -> Paris flyover cutscene -> safehouse ("Press X to play
  as Sly") -> Satellite Sabotage Binocucom briefing -> Sly on the roofs; L3 shows coins/bottles.
  Rendering looked right.
- Direct starts (debug, warp_run -Direct, 300 s): m1_recon (world 3 basement), m1_bug (hub,
  street), m1_waterpump (world 7, Murray, laser room) load and play; logs only show the known
  boot-time lines (syscall 0x5B, CD S-cmd fn 34, fileio fn 255).

- More direct starts, all load and play without new log lines: m1_disco (Bentley, disco lasers),
  m1_heist (Bentley in the street), m1_boss (world 5: Dimitri intro cutscene with subtitles, fight
  starts), m1_moulin_power (world 6 theater), t1_heist_4_hijack_truck (cutscene + Sly in the
  square), t1_heist_7_shoot_harpoon (truck harpoon turret view), m1_neela (cutscene + rooftop chase),
  t1_heist_2_pickpocket_guard (t1_heist_piperoom maps there), t1_heist_8_siege (see traps).
- Harpoon turret played live: left stick aims (vertical inverted), X fires; the harpoon flies with
  its green rope, a camera on Sly shows the (canned) miss and the turret resets. Five shots missed
  (aiming without a reticle at ~15 fps over the agent loop); mechanics work, no runtime issue.
- Frames: build\screenshots\ep1_*.png (safehouse, sat briefing, hub roof, recon basement, theater,
  waterpump, disco, heist, harpoon fire/miss, neela chase, siege, boss intro).

- Direct starts m1_sat (Paris street, satellite dishes, guards), m1_follow_dimitri (Binocucom
  briefing, Sly on the canal), m1_brawl (Murray briefing, alarms job) load and play (debug, 360 s);
  no new log lines. Every Episode 1 job now has a checked direct start.
- Audio: safehouse music loops cleanly for 30 min (debug, snd2: 60 s period, steady levels, 0
  underruns); roof idle 18 min (snd1) steady. Hub run hub2 (release): after ~205 s the music went
  almost silent (-65 dB, left only) while Sly stood in the hub; not yet explained (game fade of the
  music channels via spiop proc 6, or a stream problem?) -> check with PS2_LOG_SND.
- Merged the newer next/warp (entry hooks: direct starts and the title anchor in release builds);
  cherry-picked ep7's PS2_POKE debug aid (11b4ac8) to hold sly.hpCur (0x3D4AB0) in the boss fight.

- Boss (m1_boss "Printing Press Duel", release direct start, ~1 h of live play with
  PS2_POKE=3D4AB0=28): intro cutscene with subtitles, Dimitri's ring bolts (purple projectile,
  lightning hit effect on Sly), Sly's health icon flashing, Dimitri's melee kicks and teleports
  between numbered platforms all happen; Job Help shows the job text. Not won: the camera is
  fixed per area, Sly is knocked across the room, and with the poke Sly sometimes stands frozen
  next to Dimitri for ~1000 VBlanks (probably the poke fighting the game's death/stun logic).
  Direct starts of t1_dimitri_boss_complete / t1_complete only put Sly in the safehouse (the
  post-boss sequence is driven by the boss stage itself), so the episode end is untested.
- Pause menu: dpad presses of 4 frames were missed on the loaded machine (~25 game fps); 10-frame
  holds work (left stick works too). Not a runtime bug.
- Tools for live play (build\tools, not committed): pose.py (player/camera pose from PEEK lines),
  objs.py/vts.py/movers.py (objects in RAM dumps after ep7's findobj: +0xC vtable, +0x18 oid,
  +0x230 position; Sly oid 7 vt 2D2E98, Bentley 8, Murray 9; Dimitri's position is not at +0x230).

- Hub music silence, first fix attempt (WIP commit 159e2de, resumed 2026-10-11): sd.c "driver pace"
  (PS2_AUDIO_IOP_LIMIT, default 60 ms): the mixer outputs silence and holds every voice while the
  IOP's last pass (iop_last_run_time) is older than the limit. Unit tests pass (sd 686 checks,
  iop_kernel). BUT the A/B runs (build\abA old exe, build\abB fix; hub, loaded machine, 890 s) both
  lost the hub streams: abB transfers stop at ~185 s and ~321 s (EE polls spiop cmd 76 sub 8 for
  stream 0xB -> 0, then sub 2). abB held the voices 823 times (60 s of 890 s) on this loaded machine.
  So the hold alone does not explain/fix it. Added PS2_IOP_WATCH (debug builds: log entries to IOP
  functions, e.g. 22140 = 989snd error report a0=code) to see which 989snd error stops the streams.
  ep2 (next/ep2, paused) found the same 0x37 mechanism for the episode menu music and runs the IOP
  while the EE waits for the GS thread (gs_set_wait_hook); different files, merges cleanly.

- Session 2026-10-11 (resumed): the uncommitted WIP moved the IOP onto its own host thread
  (iop_thread_start, IOP lock, locks in sif.c/cdvd.c/iop_sif.c/loader). Unit tests pass. Findings:
  * PS2_LOG_SD_VOICES=<hex mask> (sd.c) logs stream voice jumps/loop starts/LSAX writes (one-block
    self-loops of idle voices on 989snd's silence block at 0x5000 are skipped: they flooded the log).
    build\tools\loops.py <run.log> reports "late" (no LSAX write since the last jump: voice loops
    inside its half -> 989snd error 0x37) vs "overridden" (LSAX written but the loop-start flag
    won). All failures were "late": 989snd's half change -> refill VoiceTrans -> next tick LSAX
    chain did not finish within the 163 ms half. Not an SPU LSA semantic problem.
  * Logging itself stalls the IOP thread: fprintf to stderr blocked 50-300 ms (PS2_PROFILE=iop, new:
    samples the IOP thread only in passes over 4 ms; 98% in fprintf). Runs with PS2_LOG_SD* are
    not representative for timing. The IOP status line itself (fprintf every 10 s from the IOP
    thread) caused one long gap per 10 s -> now formatted under the lock and written by a writer
    thread (log_async).
  * EE thread held the IOP lock up to 200 ms (sif.c ran iop_run after each RPC; EE thread
    descheduled meanwhile) -> iop_kick(): wakes the IOP thread via an event instead.
  * Status line now: IOP thread CPU %, longest gap/pass/lock wait/1 ms sleep, longest lock hold by
    another thread; [audio] status: voice transfers per 10 s (steady ~440-490 when the safehouse
    streams live; a drop = a stream stopped).
  * Without logging (r6/r7, 420 s) the safehouse streams all survived (486 transfers/10 s steady).
  * The IOP thread's 1 ms sleep sometimes took 40-120 ms at HIGHEST on this loaded machine (r9).
    pa/pb/pc A/B/C: HIGHEST ~ MMCSS (gaps 5-13 ms), TIME_CRITICAL worse (15-48 ms, noise?). Chose
    MMCSS "Pro Audio" like the audio thread (fallback HIGHEST): commit 4ce81aa.
  * m1 (release, safehouse, 960 s, WAV): 9 streams steady (485/10 s) all along, IOP gaps <= 28 ms,
    WAV continuous after the level load except one 30 ms hold at 340 s -> driver limit default
    raised 60 -> 100 ms (d8469b8): 100 ms still leaves 989snd ~60 ms of the 163 ms half.
  * h1 (release, Paris hub roof, Sly idle, 25 min, WAV build\h1\h1.wav): streams steady at 750 s
    (the old failure was at ~205 s).
- Merged main (8e05f84: VU recompiler) - clean; all unit tests + run_tests.py pass.
- Cherry-picked the PS2_TASK_SET / PS2_TASK_FILE tool (be8ede3 from next/ep3 = ep8's tool).
- Docs: notes/iop-native.md ("When the IOP runs", "Why its own thread", Debugging), audio-mixer.md;
  scripts/stream_loops.py (from build\tools\loops.py).

- h1 (hub, 25 min): streams steady to the end (485-490/10 s). WAV: holes of 20-40 ms exactly at
  each [audio] status line -> the output thread blocked in fprintf (null sink then caught up in a
  burst, which tripped the driver hold) -> host_log_async (audio_out.c, d4dd3c8): status lines,
  underrun lines, sd.c's SDLOG and the IOP status go through a writer thread. With
  PS2_LOG_SD_VOICES on (r10): no holds, streams clean; 989snd moves LSAX 4 ms (median) / 8 ms (p99)
  after a half change.
- PS2_PERF (p1, hub, release, loaded machine): 26-43 VBlanks/s, GS thread ~16-18 ms/VBlank (GS-bound
  under load), VU 3.7-5 ns/pair 100% recompiled. Not meaningful while ~10 instances run.
- Job completion flow (js1: m1_sat direct + PS2_TASK_FILE 1762/1764/1766 -> 2): dish counter 1/3,
  2/3, "Job Completed!" page, autosave (save1 written: m1_sat.is_complete 1, t1_recon_intro.state
  1), Binocucom briefing of the next job ("Breaking and Entering"), back in the hub. Frames:
  ep1_sat_job_completed.png, ep1_next_job_briefing.png.
- Boss again (b3/b4, release): intro cutscene ~3 min (START does not skip), fight starts; Dimitri
  stands at about (-1180,-1400) at the start; build\tools\boss_bot.py steers Sly there and swings
  (PEEK 2DE1C0/2DDFD0); melee fights, knock-backs, bolts, lightning effects, camera areas, death ->
  restart at the fight start all work; Dimitri's health icon never dropped (mechanic unknown; his
  actor position is not in world coords anywhere in RAM except per-frame render buffers that move).
  Gave up on winning; no runtime problem seen.

- Session 3 (2026-10-11): merged main (2d00835: EE thread fiber fix). Episode end (boss5, release,
  m1_boss direct + PS2_TASK_FILE): fight started, 1929 (boss_fight) -> 2 did nothing visible,
  1931 (boss_complete) 0 -> 2 played the outro comic cutscene, autosaved (save1: m1_boss complete,
  t1_complete active, nightclub percent 100, widCurrent 1) and loaded the episode menu: "Episode 2"
  -> X -> "A Starry Eyed Encounter, Start Episode 0%" -> X -> Episode 2 intro comic. Frames:
  ep1_boss_outro_comic.png, ep1_episode2_menu.png, ep1_to_ep2_start_episode.png.
- ct2 (warp --job m1_recon --complete t1_recon --complete m1_recon --activate t1_chalktalk1): Sly
  in the hub, no chalk talk (it is started by the recon completion event, not by its state).
- ct3 (m1_recon direct + task file 1776..1790 -> 2, spytargets 1793-1795 -> 2): the game made
  1796 (recon_complete) active, not complete (it waits for a trigger, unlike sat_complete); 1796 -> 2
  by hand: Job Help shows all goals ticked and the job ticked, chalktalk1 active, but no "Job
  Completed!" page and no chalk talk; Save and Quit (pause: Keep Playing / Save and Quit / Exit to
  Episode Menu, confirm) + Resume -> safehouse with only Sly / ThiefNet options. Chalk talk needs
  the real completion trigger (tool limitation, not a runtime problem).
- disco2 (m1_disco direct, live): pause -> Controls shows Bentley's layout (triangle drop bomb,
  square swing dart gun, R1 run/fire dart, circle sneak/activate); bombs drop with a red blast
  circle; a door marked red -> "Job Abandoned!" page -> iris wipe -> hub as Bentley (green tint near
  the job marker fades when walking away: game effect). Frame ep1_controls_bentley.png.
- shop1 (--job m1_bug --coins 5000): safehouse -> right -> "Connecting to ThiefNet..." -> Buy
  Gadgets list (Trigger Bomb 200, Fists of Flame 400, Smoke Bomb 300) -> buy + confirm -> 4800
  coins, card saved; Sly's Gadget Grid says no powerups (Trigger Bomb is Bentley's). Works.
- build\tools\burst.ps1: send a live line and get the next N dumps as one contact sheet
  (build\<run>\burst.png) - much faster live play than single frames.
- Boss mechanic (slycooper.wiki.gg walkthrough): Dimitri is only open after his ring jams on
  the 4th bolt; melee near him does nothing. build\tools\boss_bot2.py kites at a distance, then
  rushes and swings, in a drifting cycle.
- **Boss won legitimately (boss7, release, PS2_POKE=3D4AB0=28)**: Dimitri's position is the float
  triple at 0x685620 (object 0x685460, vt 2D9978, oid 13105, +0x1C0; found with
  build\tools\objmove.py = objects whose position-like triples moved between two RAM dumps, then
  checked against his known start spot (-1194,-1364)). boss_bot2.py --target 685620 --far 800
  --hold 6 --rush 4 --range 230: his health pie (portrait, top right) went down, he teleported
  around, and after ~4 min the outro comic played (mugshot, jail, van Paris -> Monaco, card game)
  -> episode menu "Episode 2". Frames ep1_boss_won_outro.png, ep1_outro_*.png. So the whole
  episode is completable; no runtime problem in the fight.

## Next
1. Chalk talk test (ct1: m1_recon direct + task file: 1793/1794/1795 -> 2 -> recon complete ->
   chalk talk 1 in the safehouse?).
2. More jobs in depth (disco bombs/lasers, theater, waterpump, neela chase, heist stages).

## Findings / traps
- The scratchpad dir is shared with other agents (a file there was overwritten): keep helper
  scripts in build\tools (live session helper: build\tools\live.ps1).
- Machine is overloaded by ~7 parallel sly2 instances: release hub runs ~20-27 VBlanks/s with VU
  at ~25 ns/pair (11 ns idle), so PS2_PERF speed numbers are not meaningful right now; compare VU
  pairs per VBlank instead (Paris hub/safehouse ~600-930k pairs per VBlank).
- t1_heist_8_siege has no start warp: the direct start puts Sly in the safehouse with the truck
  gauge up; its door abandons the job (warp tool limitation, not a runtime bug).
- PS2_INPUT_FILE: "+n" tokens on one line are relative to the previous token, not to the read.
- live_goto.py with negative coordinates: put options first, then `--`, then the points.
- Release + fixed START frame 2400 is too early when the machine is slow (title not ready): send
  START live, or use the debug build's anchor.
