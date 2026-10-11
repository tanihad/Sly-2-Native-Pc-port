# ep5: playtest Episode 5 "A Tangled Web" (Prague)

**Goal**: play Episode 5 (hub world 17) end to end via warp saves / direct job starts, find and fix
runtime/recompiler problems at the root (crash, hang, missing syscall/RPC, rendering, audio, perf).

## Done
- Merged next/warp (178353c) into next/ep5. Release (build\rel) and debug (build\sly2) built.
- Live-play helper (scratchpad direct.ps1, recreate if lost): warp.py make --job X --direct --card
  build\live\<job>\card, copy exe to build\live\<job>\sly2_<job>.exe, set PS2_WARP_TASK +
  PS2_MEMCARD_DIR + PS2_PEEK, then scripts\live_game.ps1 -Dir build\live\<job> -Exe ... -Script
  '@187D80/1DD96C+30:start:10'. Steer with scripts\live_look.ps1 -Dir build\live\<job> -Send ...
- Played (no crash/hang, no new log warnings; only boot-time eret 0x239AA4 / syscall 0x5B):
  - Episode start (release): safehouse, X -> Sly in rainy Prague hub; walking/ledges fine.
  - m5_recon direct: Binocucom briefing, Sly on the roofs, Binocucom photo mode (R1 photo -> "No
    item of interest in view"), target list Road/Blimp/Assault Tank/Neyla's HQ.
  - m5_battlezone direct: letterboxed intro ("OK guys, I'm in a tank!"), first-person tank driving +
    firing, 0/6 counter.
  - m5_hack direct: Bentley in the crypt (world 22), terminal reachable (needs battery first).
  - t5_heist_free_carmelita_hack direct (world 22): Bentley's hacking minigame (top-down ship on a
    blue grid) renders, ship moves/turns with the left stick, enemies kill it, restart pan.
  - t5_heist_turret_intro direct: Bentley's turret vs. green biplanes in the rain; "Job Failed!
    Press X to try again" when idle.

  - m5_ghost (Sly on the cathedral roof, stained glass), t5_ghost_enter_tomb (wolftomb, world 21).
  - m5_ewok_trap (ewoktrainer 23: Binocucom briefing, Bentley at the glowing trap), ewokhall 25,
    ewoktomb 24, ewokwater 26: mojo-trap gameplay ("Use O to activate the traps", meter, enemies,
    "Job Failed! The Bad Mojo collector was destroyed").
  - m5_kidnap (Murray on the roofs), t5_pickpocket_guardbreak_interior (20),
    t5_pickpocket_waterrails_interior (19: water, waterfalls, drowning).
  - t5_heist_paraglide_to_blimp: Gadget Grid (SELECT, exit with TRIANGLE), R1 in a jump deploys the
    paraglider; landing on the disk below replays Bentley's "grab the rope" cutscene.
  - t5_heist_boss_1_intro: letterboxed intro (Contessa as a spider), fight, death -> restart intro.
  - Episode menu: --set episode.castle.percent_complete=100 -> resume opens the dvd_menu world
    (Episode 6 / UP -> Episode 5 "A Tangled Web", X: Start Episode -> safehouse). No movie for ep5.
  - --activate t5_intro changes nothing (safehouse as usual).
- Perf under load: boss fight GS recording (240 VBlanks) replays at 36.3 ms/VBlank vs the
  fix-perf briefing recording at 36.0 on the same loaded machine (8.9 when quiet): not heavier.
  EE waits on the GS thread (gswait 50-90%) everywhere incl. boot: machine load, not ep5.
- Screenshots: build\screenshots\ep5_*.png (hub, menu, tank, hack minigame, turret, boss1, crypt,
  ewok levels, guardbreak, waterrails, wolftomb, kidnap).

- Session 2: merged next/warp again (71124a3: entry hooks, so direct starts and the title anchor
  work in release builds); rebuilt build\rel and build\sly2. Private helper now in
  scratchpad\ep5\direct.ps1 (-Release works with --direct). Do not pipe its output (sly2 inherits
  the pipe and the call blocks until the game exits).
- t5_heist_intro (release direct): Murray cutscene ("I hope that General guy told the truth"),
  Sly's paraglide talk, then Sly on the tower with the blimp; paraglide jump lands on the disk ->
  Bentley's rope cutscene again (gameplay, not a bug).
- The "|" after subtitle text is the game's blinking typewriter cursor (blinks between frames).
- t5_heist_free_carmelita_intro (world 22): Bentley at the old terminal cutscene, then the hack
  minigame (ship moves, shoots with the right stick).
- t5_heist_neela_chase_intro: cutscene (Neyla with the Clockwerk eye), the chase runs; running
  straight is not enough: "Job Failed! Neyla got away with a Clockwerk Eye!" (gameplay).

- t5_heist_tank_chase_intro: cutscene (Carmelita took the eye, escapes in a tank), first-person
  tank chase; holding lup + R1 fire, "Job Failed! Catch and destroy Carmelita's tank" after ~7 s
  (gameplay; the tank moves and fires).
- t5_heist_boss_2_intro: Contessa fight 2, hypnosis spiral world. During the hypnosis transition
  (~60 VBlanks) a 1-px green rectangle shows at the screen edges (x=0, rows 3 and 221). GS trace
  (PS2_GS_TRACE_RECT=0,100,0,100 then 0,0,511,223 brief MINSIZE 150): the game itself draws it - a
  linestrip (0,2.94)-(512,2.94)-(512,221.06)-(0,221.06), RGBA 007F0080, AA1, ABE 0, FBMSK FF000000
  (RGB written), ZBUF Z16S, ZTST GREATER right after clearing that Z buffer to 0 with ZB_ONLY
  sprites, so it passes everywhere. AA1 without ABE = coverage alpha only (alpha is masked here),
  as PCSX2 does (IsCoverageAlpha); on a TV the lines sit in the overscan. Not a runtime bug.
- Audio: hub run with -Wav (480 s, release, loaded machine): no dropouts except level loads/boot,
  11 click candidates (title music starts), positional stereo in the hub. Fine.

- t5_ewok_trap_hall_door (hub door stage, Bentley, "1/4"): a mojo-collector defence too, fails
  without play.
- Natural progression end to end (no warp after the start):
  - t5_heist_free_carmelita_freed (world 22) -> freed cutscene -> complete -> the game loads world
    17 by itself and starts t5_heist_neela_chase_intro (chase) - no save mid-heist (as expected).
  - t5_heist_boss_2_complete: Contessa's defeat cutscene, "Clockwerk's Eye" card, the 2D comic
    epilogue, then the episode menu "Episode 6" (dvd_menu world 1). The game autosaved:
    castle.percent_complete 100, m5_heist.is_complete 1, widCurrent 1, episodeCurrent 0 (!).
  - Resuming that save went into the Cairo museum (prologue): resume checks the current episode's
    percent_complete (episodeCurrent 0 = prologue) and warp saves had 1 there for finished episodes.
    With episode.intro.percent_complete=100 the same save resumes into the Episode 6 menu. Fix in
    scripts/warp.py: finished episodes are saved at 100 (warp unit tests pass; ep5 start still
    reaches the safehouse).
- Chalk talk 1 (warp --job t5_chalktalk1, no direct): safehouse as usual, X -> hub, no chalk talk
  (it is started by the recon-complete event, which a warp save does not replay).

- Batch of direct starts (scratchpad\ep5\batch.ps1 -> build\warp\b_<job>\sheet.png; under this
  load the title needs ~3400 pad frames = 4-5 min, so -Seconds 540): all load and play, logs
  clean: t5_recon_interior_intro (world 22: Contessa/Carmelita cutscene, Sly at the window),
  t5_heist_boss_1_complete (cutscene -> tank chase intro -> chase -> failed), t5_hack_path2/3
  (Bentley with the battery, 1/3 2/3), t5_pickpocket_guardsneak_intro (hub), _interior (world 20).
- Tank chase with lup held + R1 (build\warp\tank_chase_play): the tank drives at a normal pace,
  hits the wall at the first bend and falls (camera z to -767) -> Job Failed: needs steering.
- Ewok water (last mojo-trap stage) live: spamming O does not save the collector (gameplay).

- Batch rest (session 3 check): t5_pickpocket_waterrails_intro, t5_ghost_take_pictures,
  t5_pickpocket_intro (Binocucom briefing with its scanline overlay, then Sly on the roofs),
  t5_heist_free_carmelita_complete (cutscene -> Neyla chase -> failed): all load/play, logs clean
  (the trailing "[watchdog] 0 function calls" in release logs is the exit dump, not a hang).

- Session 3 perf (release, PS2_PERF, no other sly2 but ~45% CPU from other processes): hub
  29-40 VBlanks/s, 880k VU pairs/VBlank at ~20 ns/pair (VU = 62% of the EE thread). The intro
  cutscene on the standard path, measured right after under the same load, ran at only 25-28
  VBlanks/s (it is 59.9 on a quiet machine), so the hub is not heavier than scenes that hold 60:
  machine load + VU interpreter cost (next/vu-recomp is the planned fix), nothing ep5-specific.
- Cherry-picked debug tools (same content as next/ep4/ep6/ep8): PS2_POKE, PS2_INPUT_FILE fix,
  PS2_TASK_SET/PS2_TASK_FILE (entry hook 0x20E890; rebuilt rel + debug). Helper direct.ps1 now sets
  PS2_TASK_FILE=<dir>\tasks.txt.
- Recon finished in context (t5_recon_interior_intro direct, release): Binocucom in the tower works
  (target list Old Terminal / Clockwerk's Eyes / Carmelita / Shadow Guard, R1 "Too far, use right
  analog stick zoom in", right stick zooms). Spy targets 2634-2638 set to 2 with PS2_TASK_FILE ->
  2639 active; closing the Binocucom completed it -> "Job Completed!" book -> chalk talk 1 (slides
  with chalk arrows/circles, ~3000 VBlanks) -> safehouse. Autosave: m5_recon complete,
  t5_chalktalk1 state 3, ghost/ewok/kidnap intros active, castle 15%. Screens ep5_recon_job_completed,
  ep5_chalktalk1_*.png, ep5_safehouse_after_recon.png.
- Then in the hub: 2662/2681/2686 (the three job *_complete tasks) set to 2 directly -> "Job
  Completed" for Bentley and Murray, saving icon, safehouse; chalktalk2 went 1 -> 3 without any
  slides. Setting a job's final task directly probably bypasses the game's job-end flow.

- Session 4: merged main 8e05f84 (VU microcode recompiler) into next/ep5 (df4589a; conflict only in
  notes/hle-architecture.md, both tool lines kept); rebuilt build\rel and build\sly2. batch.ps1 now
  takes -Prefix (run dirs build\warp\<prefix><job>).
- Hub perf (release, recompiled VU, machine ~99% CPU with 2-3 other games): VU 100% recompiled at
  3-6 ns/pair, EE busy 9-13 ms/VBlank, GS thread 15-18 ms/VBlank (up to 22 when more games run)
  -> 45-58 VBlanks/s, 37-48 flips/s. The GS thread waits on full pixel-worker queues
  (150-370 ms/s) and CLUT syncs (~55/s, 130-230 ms/s): GS-bound under load, as main's regression
  note says for every scene (D3D11 renderer is the planned fix).

- Hub GS-thread profile (PS2_PROFILE=gs, 9 games running): 35% ntdll, 25% db_submit (queue-full
  wait loop that helps draw), 9% run_bands, 7% gs_draw_sync_why: the GS thread waits for the
  pixel workers, which compete with the other games for cores. Generic, not ep5.
- PS2_VU_VERIFY=1 (release, recompiled VU checked against the interpreter) on 8 ep5 scenes:
  battlezone tank, hack minigame, boss 1, boss 2 (hypnosis), turret, ewok trap, Neyla chase,
  waterrails interior: 2-6M programs each, 0 mismatches.
- Chalk talk 3 in context: direct m5_battlezone with `--complete m5_pickpocket --complete
  t5_pickpocket --complete m5_hack --complete t5_hack`; PS2_TASK_FILE 2717-2721:2 -> HUD 5/6;
  2722:2 -> the game's own flow: last tank explodes, "Job Completed!" book (Murray, 55%), autosave
  (castle 77%, battlezone complete, chalktalk3 state 1), chalk talk 3 "Operation: High Road"
  (chalk X's, arrows, blimp, Contessa) -> safehouse. Screens ep5_battlezone_*, ep5_chalktalk3_*.
- ThiefNet paraglide purchase (warp --job t5_paraglide_purchase --coins 1000, no direct):
  safehouse LEFT x2 -> Bentley's laptop, X -> Buy Gadgets list, Paraglide 600 at the bottom, X, X
  -> 400 coins left, item gone; leaving ThiefNet (TRIANGLE): t5_paraglide_purchase 3,
  t5_heist_intro 1, autosave (castle 79%, mppukfAcquired bit 27 = warp.py's STORY_GADGETS).
  Screen ep5_thiefnet_paraglide_bought.png. Menu input: presses must be ~40 frames apart under
  load (shorter gaps get lost).

- All 12 PS2_VU_VERIFY runs (also recon, ghost tomb, tank chase, paraglide): 4-6M programs each,
  0 mismatches; audio 0-2 underruns per 310 s under heavy load.
- Chalk talk 2 in context: direct t5_ghost_take_pictures with `--complete m5_ewok_trap --complete
  t5_ewok_trap --complete m5_kidnap --complete t5_kidnap`: Sly in the graveyard, camera 0/9;
  Binocucom = R3 (left stick aims, Y inverted; right stick zooms; R1 photo; the purple wisps are
  not targets: "No item of interest in view"). 2650-2657:2 -> 8/9, 2658:2 -> 2660 done by the game,
  2661 ghost_drop active ("Deliver ghosts to Neyla's HQ"); 2661:2 -> the game's own flow: delivery
  cutscene (ghosts released at Neyla's HQ, Neyla: "So, the Contessa wants war, eh? ... a few
  bombers"), "Job Completed!" (Sly, 50%), safehouse, chalk talk 2 (sepia overhead slides), safehouse;
  chalktalk2 3, pickpocket/battlezone/hack intros 1, autosave castle 45%. Screens
  ep5_chalktalk2_start.png, ep5_ghost_delivery_job_completed_sheet.png.
  => all three chalk talks, the ThiefNet purchase and job-end autosaves verified in context.
- Rule learned: set the stage tasks *before* a job's final task (tank kills, photo targets, drop)
  and the game runs its own job-end flow; setting the *_complete task itself skips it.

- GS replay bench (8 workers, machine at 100% CPU): safehouse recording (240 VBlanks, 627 MB)
  54.0 ms/VBlank vs the fix-perf briefing 51.3: about as heavy as the briefing. (The t75 X press
  came before the safehouse accepted it, so the recording is the safehouse, not the hub street.)
- **FIXED (9d6c24c): ~1 MB/s committed-memory leak in every scene.** Soak in boss 1 (release):
  private bytes 278 -> 1001 MB in 15 min, working set flat ~155 MB. PS2_LOG_THREADS: the game
  creates/starts/ExitDeleteThreads thread #6 (entry 0x1AAF18) ~once a second; CreateThread's memset
  wiped the slot's fiber pointer -> one leaked fiber (CreateFiber(1 MB) = 1 MB committed) per cycle.
  Same bookkeeping also resumed stale fibers on restart (restart after return spun forever, after
  ExitThread/TerminateThread resumed inside the old syscall). runtime/threads.c: parked fibers are
  reused, fibers stopped mid-way deleted (the exiting one by the next fiber to run). New
  tests/unit/threads; PS2_PERF prints "[perf] memory:" every 10 s. After: flat 149-151 MB over a
  16-minute boss soak; all unit tests + run_tests.py pass. (threads.c is "integrator-owned" per
  hle-architecture.md: flagged in the report.)

- Regression with the fix: standard path 70 s (149 MB flat), debug build m5_kidnap (Murray's
  Binocucom "Kidnap the General / Bring General to safehouse", walking to the General; 149-150 MB),
  release t5_heist_free_carmelita_freed -> freed cutscene -> world 17 -> Neyla chase (151 MB).
  Screens ep5_kidnap_murray_debug.png, ep5_heist_freed_to_neyla_chase_sheet.png.

## Status: Episode 5 covered end to end
Every job type and stage starts and plays; all three chalk talks, ThiefNet purchase, job-end
autosaves, heist chain and episode end (-> Episode 6 menu) verified in context; VU recompiler exact
in 12 ep5 scenes; one runtime fix (fiber leak, 9d6c24c). Not done: finishing jobs by pure play
(kidnap carry, mojo defence, chases need real steering), PS2_PERF on a quiet machine.

## In progress
- Session 5: merged main 2d00835 (has the threads fix as 81186bf; only notes/progress.md new) ->
  857b74c. Machine quieter now (1 other sly2, ~50% CPU): doing Next 1 (PS2_PERF).
- Hub perf (release, PS2_PERF, Sly walking in the street; 5-7 other games started meanwhile): 15-50
  VBlanks/s, EE busy 7.4-14 ms, GS thread 15-37 ms/VBlank: load again, no quiet window.
- GS replay bench of the hub *street* (recorded VBlank 6400-6640 while walking, build\gsrec\
  hub_street.gsrec 623 MB; the session-3 recording was the safehouse), interleaved with the
  fix-perf briefing recording, 8 workers, same load: hub 32.5/33.8/34.6/36.3 ms per VBlank vs
  briefing 42.1/36.1/34.8. The rainy hub street is no heavier on the GS than the briefing (8.9 ms
  quiet, holds 60): no ep5-specific GS cost.

## Next
1. When the machine is quiet: PS2_PERF (release) in the hub street (Sly walking), boss 1,
   battlezone tank, hack minigame; GS thread was 15-18 ms/VBlank under load.
2. Optional: finish a job by play (kidnap carry to the safehouse) with live steering.

## Findings / traps
- The machine runs 10+ other sly2 instances (CPU 100%): debug build ~18-30 VBlanks/s, release
  ~27-35 in the hub; PS2_PERF absolute numbers are meaningless now. Release START at pad frame 2400
  comes before the title accepts it under load: use live input (watch latest.png) instead.
- Hub VU load: 700-920k pairs per VBlank (safehouse/hub), close to the intro cutscene's ~950k.
