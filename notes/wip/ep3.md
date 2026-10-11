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
- Turret again with m3_turret.suck_value=1.0 (the game's own easing after failures; warp.py --set)
  and a pause-stepped time-lapse (build\tools: append "+0:none,+90:pause" in a loop, copy
  latest.png): 22 x 90 frames without a hit on the dam; the waterfall/dam stays at the lower right
  under the HUD's temple icon, the turret's pitch/yaw limits keep it ~50 px below the crosshair at
  that point of the path. Turret dialogue strings: "Nice shooting Bentley! Hope Rajan knows how to
  swim." / "Whuh? A piece of the dam struck the chopper!" follow a successful run. Left as game skill.
- Spice Grinder (TNT, live): Sly in the catacombs next to the TNT barrel (red "!" marker), jumped,
  mask transition + respawn at the entrance arch with a blue glow (death/checkpoint). Not completed.
- Lead Rajan (live, release): Bentley's dart gun view (R3) + R1 dart works; 0/3 blueprint counter.
  Not completed (lead Rajan to the watermelon with darts, then pickpocket).
- Perf after the main merge (release, VU 100% recompiled, 4.0-6.5 ns/pair), machine at 100% CPU
  with ~10 game instances: hub GS thread 18-28 ms/VBlank, world-13 interior 21-36 ms, EE busy
  6-13 ms; the EE waits for the GS (gswait 50-85%). GS-bound (software rasterizer; the planned
  Direct3D 11 renderer is the fix). Not measurable properly until the machine is quiet.

- Audio (WAV, release, boss intro + heist1 direct starts, build\tools\wavrun.ps1): no underruns,
  no ADPCM errors; the digital-silence gaps are the attract cards' pauses and level loads. But the
  IOP status showed the 989snd hard timer 1500-1900 times per 10 s more than 8 ms late (max ~1 s):
  the IOP stood still while the EE thread waited for the GS thread (gs_thread.c ee_wait_change).
  next/ep2 had already fixed exactly this (gs_set_wait_hook -> ps2_poll_iop) together with the
  same pixel-worker race I fixed in 2b996f3. d7a9611 takes ep2's code verbatim (gs_draw_pixel.c,
  gs_thread.c, hw.h, gs-draw tests identical to next/ep2; ps2_runtime.c with ep2's two hunks) so
  the branches merge cleanly; my variant + GS_TEST_WORKER_DELAY_MS test are dropped. Re-run of the
  boss intro with it: 20-124 late ticks per 10 s, max 50-150 ms (one 500 ms OS stall).

- Episode 2 -> 3 transition (live, warp --episode 2 --set episode.palace.percent_complete=100):
  START -> episode menu on Episode 3 (Rajan portrait) -> X -> "The Predator Awakes / Start
  Episode 0%" -> X -> episode 3 intro comic (gang at the temple, spice pot, Rajan, jungle) ->
  safehouse "Press X to play as Sly"; autosave (episodeCurrent 3, t3_intro 3, recon_intro 1).
  Frames ep3_episode_menu, ep3_episode_start, ep3_intro_comic_*.

- Chalk talk 2 (live, lead_rajan direct start with --complete t3_waterbug/t3_free_elephant
  (+ m3_*), then PS2_TASK_FILE 2245/2251/2257 -> 2 (blueprints) and 2258 -> 2): job ends, chalk
  talk 2 MPEG movie (Rajan, heart room, ruby, chalk lines), safehouse "Press X to play as Bentley",
  autosave (chalktalk2 3, lead_rajan_complete 3, temple 50%, turret/ruby intros active).
- Boss (live): direct start = Sly beside Murray on the temple roof; walking forward leads to the
  lily-pad pool; boss tasks via PS2_TASK_FILE change state but not the fight (own state machine).
- PS2_INPUT tokens now case-insensitive (2c3c73d).
- Session 3 (cont.): heist live session (build\live_heist2, not noted before the interruption):
  Murray walks/fights to the elephant's mouth (heist1), heist2 lift via task file, heist3 Bentley
  turret: hunter-seeker bombs fly in, ticker text scrolls; all rendered fine.

- Session 4 (2026-10-11): merged main (2d00835, EE thread fiber-leak fix); release build OK.
- Chalk talk 3 (live, turret direct start, --complete ruby/heartneela/tntbarrel, task file
  2284 -> 2): "Job Completed!" book, chalk talk 3 MPEG movie (clean), safehouse "Press X to play
  as Bentley" -> Bentley in the hub; autosave (temple 80%, chalktalk3 3, t3_heist_intro 1).
  2297 -> 2 (heist intro) just activates heist1_intro (a location trigger). Frames
  ep3_chalktalk3_movie, ep3_turret_job_completed.
- Free the Elephant (live, direct start): spice HUD 0/6; spice tasks 2197..2207 -> 2 via the task
  file count up to 6/6 and the ticker asks for the feed basket; walked there (live_goto) and pressed
  O at the basket by gameplay -> elephant cutscene ("Sayonara satellite array!"), Job Completed
  book, autosave (free_elephant complete, temple 30%). Frame ep3_free_elephant_cutscene.
- build\tools\objpos.py <ram> <regex>: level objects by name with instance positions (name table
  'FK$X'+name: -0x18 instance array, -0x14 count, -8 oid; props pos at +0x90, pickups +0x180,
  actors +0x230). Boss level: rajan at (6, 4177, 3600), Murray at the start (-1402, -1641, 1195).
- Movie audio: build\tools\pss_audio.py <iso> --scan finds the ~45 movies inside the packed
  archive (no ISO directory entries; Sly 2's own pack header 00000000 011E..., resync on 000001BD)
  and checks L/R: the 22050 Hz movies (chalk talks) and the boot movie are mono on the disc
  (identical L/R blocks), the 44100/48000 Hz ones are stereo. Chalk talk 3 (WAV, wav_ct3):
  43 s continuous, corr 1.00 = source. Episode 3 intro comic (lba 124141, stereo; live_ep3intro
  with -Wav -LogSif): output corr 0.76, so stereo movies stay stereo. But 650 "late/early stream
  refills" (VoiceTrans into the 1 KB block the movie voice c0 v0/v1 plays, rings 0x19040/0x1A040)
  during that run: EE at ~35 VBlanks/s (100% machine load + PS2_LOG_SIF), so the movie decoder
  fed the audio late. wav_ct3 (no SIF log) had 0-1. Re-check on a quiet machine with PS2_PERF.
- Boss "Get to Rajan" (job help text; Binocucom shows the same goal): from the roof start, a
  mushroom bounce pad by the ledge throws Sly up beside the balustrade staircase (z 1300 -> 2250)
  but not onto it; walking ahead drops into the lily-pad pool pit (13 i_buoy_lillypad). Rajan
  waits at (6, 4177, 3600). Poking Sly's position (actor oid 7, +0x230 and copies) does not
  teleport him. Not reached; navigation, not a runtime issue.
- Rip-off the Ruby (live): Sly on the statue under the ruby; job help "Get Murray to the ruby";
  2287 completes, 2288 (intro_murray) becomes active (a location/character switch).
- VU recompiler on Episode 3 content (merged from main this session): build\tools\vuverify.ps1
  runs direct starts with PS2_VU_VERIFY=1 (each compiled microprogram also interpreted and
  compared), 540 s each, own exe names. hub, recon_int (world 13), heist3 turret, turret, boss
  Murray-vs-Rajan pool fight (lightning), tntbarrel (world 13), waterbug, heartneela interior
  checkpoint (world 13), lead Rajan: 10-14.7M programs each (~115M in all), 0 mismatches, no
  interpreted fallbacks. (A first 240 s sweep over all 19 start tasks: 0
  mismatches too, but mostly title/load content; its waterbug run was killed externally after
  22 s - warp_run's exe is named sly2.exe.) Frame ep3_boss_murray_pool_fight.
- Salmon/red climbing ropes in the hub and on the boss roof checked (GS record at VBlank 7700 +
  replay with PS2_GS_TRACE_RECT on one rope pixel, build\tools\trace_prims.py /
  trace_texcolors.py): Sly 2 draws every model twice with two CLUTs of the same 4-bit texture -
  a grey "shadow" CLUT with normal blending (ATST GEQUAL 7E), then the coloured CLUT additively
  (A0 B2 C0 D1) weighted by the vertex alpha. The rope texture (TBP 12542, 32x16 T4) is
  red-brown in its coloured CLUT (avg 69,40,35), so the pink rope is game data, not a CLUT bug.
- GS cost of the hub (load-independent ratio): build\gsrec\hub.gsrec (240 VBlanks of hub
  gameplay from VBlank 7000) vs brief.gsrec (title) and recon_int.gsrec, gs_replay -Workers 8
  -Bench interleaved at 100% machine load: hub 21.2/24.6 ms, title 14.6/17.2, interior
  19.9/23.6 -> hub = 1.43x the title (7.0 ms unloaded) = ~10 ms per VBlank: fits 60 fps on a
  quiet machine. In-game PS2_GS_STATS at 100% load: the GS thread mostly waits for the pixel
  workers (queue full ~400 ms/s, CLUT syncs ~36/s); i.e. CPU-starved, not a hot spot.
- build\tools\taskcap.ps1: the input parameter was named $Input (PowerShell's automatic $input
  enumerator), so input lines were never written; now -Line.

- Session 5 (2026-10-11 13:20): the soak_hub run had stopped after 25 s (killed by the usage-limit
  interruption; memory 149 MB committed, flat). Machine was idle at 13:22, but 4 other testers
  started their own "quiet machine" hub perf runs within the same minute, so perf_hub (release,
  120 s) is contaminated again: ~46 VBlanks/s, GS thread 15-24 ms/VBlank, EE busy 8-10 ms.

## Next
1. Soak: 20 min hub with input and PS2_PERF memory lines - rerun (first one was cut off).
2. On a quiet machine: in-game PS2_PERF in the hub / world 13 (expect 60/60 from the GS replay
   ratio), and the episode-3 intro comic (stereo movie) for late movie-audio refills.
3. Gameplay left (skill/navigation, no runtime blockers known): turret dam hit, boss "Get to
   Rajan" route, heist chain by play, ruby carry, Neyla keys, TNT barrel run.

## Findings / traps
- Episode 3 tasks: scripts\warp_tasks.txt (t3_*), oids 2167-2350. Level names in the save schema:
  episode.temple.level.exterior / interior.
- The scratchpad directory is shared with other agents (ep4 overwrote batch.ps1): keep helper
  scripts in build\tools\ of this worktree (live3.ps1, ep3batch.ps1, strs.py).
- Under load the release build's fixed START frame (2400) comes before the title accepts input;
  use the debug build's anchor or a live START.
- Dialogue/objective text is plain ASCII in RAM: build\tools\strs.py <ram dump> <regex>.
