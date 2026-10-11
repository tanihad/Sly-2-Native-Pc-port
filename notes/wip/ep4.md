# ep4: playtest Episode 4 "Jailbreak" (Prague)

**Goal**: play Episode 4 end to end (hub + every job type) with warp saves, fix every runtime /
recompiler problem found at its root (with tests), save frames to build\screenshots\ep4_*.png.

## Done
- Merged next/warp (178353c). Built build\rel and build\sly2.
- Episode 4 start (release, save warp): safehouse "Press X to play as Bentley" -> Bentley in the
  Prague hub; pause menu, Job Help ("Eavesdrop on Contessa"), Gadget Grid all fine. The game's
  autosave after entering the hub is written and decodes fine (warp.py dump: dt, prison dt).
- m4_recon direct (debug): Binocucom briefing, Bentley on the ledge, dart-gun view (R3) fine.
  Bentley controls (pause > Controls): R1 run/fire dart, Triangle bomb, Circle sneak/activate,
  Square swing dart gun, R3 Binocucom/dart gun, SELECT gadget grid. Aim: stick up = look DOWN.
- m4_hack direct: Bentley at the tram line, 0/6 stations, electrified rail, train kills him and
  he respawns ("Hack all six stations"); hack terminal not reached yet.
- m4_spycam direct: Sly on the roofs, "Pickpocket the key from the guard", Binocucom camera (R3,
  R1 = photo: "No item of interest in view") works.
- Direct starts (build\warp\d_<task>, frames + logs, all clean logs, all render plausibly):
  heist1 (Bentley "Hijack the tank"), heist3 (world 15 prison interior cutscene), heist4 (Murray
  hypnotised cutscene, lasers), heist5 (Sly, hypno-speakers 0/4; caught -> death mask -> retry),
  heist6 (Murray "Lift the gate levers"), heist7 (Contessa/Murray cutscene), lightning intro
  (Binocucom briefing), bomb_bridge intro, pickpocket intro (keys 0/3), rc_chopper intro (top-down
  RC chopper, rockets), rc_chopper_sly_escape (cutscene, Binocucom, "Return to the safe house").
- Audio (release, PS2_LOG_SND, build\lw\hubsnd): title opens 2 KPV music layers (32 kHz + 44.1 kHz,
  sectors 529824/533632) which carry on into the hub (layer 2 volume 0, layer 1 ramped by proc 6);
  the hub dialogue VAGp line plays 8.34 s vs 8.21 s header. Hub level ~-40 dBFS, L-biased (3D
  positioned stream). 0 underruns. One "[sd] VoiceTrans ... overwrites the block voice c0 v4 is
  playing" at the title->hub bank load (load screen, inaudible).

- Batch C direct starts (brawl_intro_int, heist2_sly_hide_under_tank, bomb_bridge_on_top,
  spycam_picture1) load and play, clean logs. Brawl live (lw\brawl): Murray punches guards; a big
  white translucent shape over the yard (frame 16110) is most likely a searchlight cone.
- Session 2: merged next/warp again (entry hooks: @187D80 anchor + PS2_WARP_TASK in RELEASE).
  Helper scripts moved to build\tools (lw.ps1 now: anchor in both builds, PS2_PEEK 2DE1C0/2DDFD0,
  -Wav, -Exe; decomp.ps1 = headless Ghidra on the shared scratchpad project, read-only;
  objnear.py / owner.py / objdiff.py / velfind.py RAM-dump helpers).

- Session 3: merged main (8e05f84: VU recompiler) into next/ep4 (63d79a9), rebuilt build\rel and
  build\sly2. Release boots ~60 vblanks/s when the machine is not saturated.
- m4_hack live (lw\hk, release, PS2_PEEK E952B0 = the train): the train shuttles on one line
  between a dead end at y=-17500 and the far east (x>8000): passes the start (-6390,-9280)
  southbound, turns at the dead end ~900 frames later, passes the start northbound, ~4550-frame
  cycle (build\tools\trainwait.py waits for the northbound pass). Station 1's green sparkle /
  terminal ~(-6386,-7896,-57) (objects at the path2 warp 60450), across a canal from the track
  ledge at (-6525,-8720,-88); hopping over the rail costs health; not reached yet (falls into the
  canal). Bentley respawns at (-6729,-9721).
  The 6 hack stations are objects of vtable 0x2DCC98 (0x759BE0, 0x758C60, ... 0xF80 apart, pos
  +0x1C0): (-6784,-8061,-160), (-10076,-4941,315), (-6986,1816,560), (-1994,5470,155),
  (4595,2877,-97), (8456,1449,-163) (build\tools\commonvt.py: vtables near every path warp).
  Route to station 1: build\tools\hk_route.ps1 (train wait, hop over the wall, along the track bed
  to (-6720,-8280)). L3 = Bentley's waypoint arrow (shows the way). The green sparkle on the
  platform east of the track is a clue bottle (guard patrols there).
- Episode end (lw\end2, t4_heist7_intro direct + PS2_TASK_FILE 2600:2): heist7_complete cutscene
  (Sly/Bentley/Murray), the camp-fire ending movie, the episode menu "Episode 5 / A Tangled Web /
  X: Start Episode"; save1 written (prison 100%, episodeBest 4, episodeCurrent 0, t4_complete 1);
  X starts the episode 5 intro movie. Menu silent after the movie: ep2's known bug (IOP starved
  while the EE waits for the GS; fixed on next/ep2 7558b12), not duplicated here.
- Recon completed in context (lw\rec, hits 2400-2403 via PS2_TASK_FILE): Bentley's outro cutscene
  with subtitles, Thievius Raccoonus page wipe, back in the hub as Bentley; save1 has m4_recon
  complete, t4_hack_intro active. Job Help: "Shoot Contessa with darts"; Binocucom: left stick aims
  (inverted Y), right stick zooms, R1 fires.
- Cherry-picked ep3's PS2_POKE_FILE (fa2cf07) for one-shot writes in live sessions. Teleport by
  poking Bentley's position copies (obj 0x1031D20 +0x230/+0xA00/+0xA20) does NOT move him (the
  game rewrites them from elsewhere) - not a usable teleport.
- Lightning completed in context (lw\ltn2, PS2_TASK_FILE 2456/59/62/65/68 then the five
  puzzleN_complete 2458..2470): Job Help "Disable all lightning rods", HUD 1/5.., then the robot
  struck by lightning cutscene (bolts/explosion render, build\screenshots\ep4_lightning_robot_struck
  .png), job-complete wipe, back in the hub, save1 written.
- Bomb bridge (lw\bb, bb2, bb3; build\tools\chain.ps1 drives task chains): under -> relocate ->
  on_top play; on_top = "protect Bentley, hide as a statue to sneak attack patrolling guards"
  (Job Failed text), Job Failed -> X retry works. Forcing on_top (2480:2) leaves the complete
  stage (2483 active) in a letterboxed shot of Sly by the truck with a sparkle, forever, no input
  accepted (twice, also without forcing 2483). Most likely an artifact of skipping on_top's own
  events (unverified: on_top not won by play).
- Heist 4 (lw\h4k, PS2_TASK_FILE): completing t4_heist4_get_to_keypad1 (2558:2) only activates
  hack1 (no minigame: it starts from the keypad interaction); completing hack1 (2560:2) puts Sly
  at keypad 1 (-4651,2798,417 feet). No memory-card write on these intermediate task changes.

- Session 4: merged main (2d00835: EE thread fiber fix from ep5) -> d1d912d; rebuilt rel + debug.
- Heist 4 live (lw\h4b, release direct t4_heist4_intro): cutscene ("defended by laser barriers.
  When you get close I'll hack..."), Sly at (-1266,184,-472). Sweeping floor lasers burn Sly: hp
  (sly.hpCur 0x3D4AB0, 0x28 full) -20 per hit, death mask + respawn at the second; held with a
  background loop appending `3D4AB0=28` to PS2_POKE_FILE. Walked past the lasers: big hall with
  hypno-speaker niches (green spiral + speakers; heist5's targets), bleachers west, dining tables
  at (-3400..-4100, 2600, -275). Keypad 1 is on an upper level: objects near (-4651,2798,417):
  oid 77563 (vt 0x2DD090, the post-hack warp), oid 2562 (vt 0x2D8C48) at (-4502,2469,515). Route
  up not found; ep5 already reached the (same) hack minigame (t5_heist_free_carmelita_hack).
- **Brawl won by play** (lw\br2, release direct t4_brawl_intro_int, PS2_POKE=3D4AE0=28 holds
  murray.hpCur, background driver build\lw\br2\fight.ps1 = stick direction + SQUARE x9 + TRIANGLE
  every ~2 s): intro cutscene (Murray behind bars), beatdown in the yard with the wall counter of
  KOs, ZONK! comic text, coins (107), yellow searchlight cones; t4_brawl_beatdown 1 -> 3 after
  ~6 min, outro cutscene (Murray to solitary), Job Completed (Murray), safehouse. **Checkpoint
  autosave**: save1 written right after (m4_brawl complete, prison 33%, oidPoCurrent 9).
  Screens ep4_brawl_outro.png, ep4_brawl_job_completed.png. Log clean (only the boot-time eret).
- RC chopper again (lw\rc5, release direct, PS2_PEEK 972A80 local pos / 972AC0 world / 972B00 local
  velocity): control starts ~33 VBlanks after the first X; the chopper then accelerates +105 per
  frame along local +x to 2100 and flies straight at full speed (~600 frames, 15700 units) over the
  city and past the lit area to local x ~17000, where it bounces around (boundary?) until the wall
  turret shoots it down ("Your chopper got destroyed", X retries: identical path). Stick test
  (ldown/lright/lup/lleft 90 frames each): the velocity direction turns toward the stick direction
  (lright -> local -x, lup -> -y, lleft -> +x), speed stays ~2100, and after release it keeps flying
  at full speed (never decelerates). So with a neutral stick it flies as if pushed LEFT. Design
  ("RC chopper always flies, stick steers") or a port bug - still undecided; the thrust comes from
  the Splice script (no ELF strings: Splice names live in the level data). JOY struct at 0x2E0BC0
  is not referenced by lui+imm anywhere (build\tools\luiref.py), so the reader is still unknown.
  **Resolved as game design** (experiment build build\exprel / build\expr from build\shadow, a
  copy of runtime/recomp/scripts patched by build\tools\xw_patch.py: PS2_EXP_XWATCH=<lo>:<len>:rw
  logs guest loads/stores by function, PS2_EXP_EE_MXCSR sets the EE thread's MXCSR):
  the pad object is JOY at 0x2DFC00 (second at 0x2E0D00; FUN_001c4468 picks by 0x2DFBC0 & 2):
  +0x103C/+0x1040 left x/y, +0x1044 magnitude, +0x1048/9 raw bytes, +0x1070/74 right x/y,
  +0x10B4 buttons. FUN_00187768 (raw -> floats, auto-calibrated 4-direction dead zone 0.35) gives
  x 0, y -0.0, magnitude 0 at rest; FUN_001c4468 (per input object, every frame, called from the
  world update FUN_00226550) copies them and FUN_001c4330 derives angle/magnitude with a 0.2/frame
  release decay -> exactly 0 at rest. So the chopper sees a neutral stick and the Splice script still
  thrusts (+105/frame to 2100 along its heading): it always flies forward and the stick steers it
  (velocity turns toward the stick direction at constant speed). Same with the EE FPU in
  round-toward-zero (MXCSR 7F80, PS2-like): identical trajectory. Not a port bug.
- Lightning puzzle sites (lw\ltn3/ltn4, build\tools\commonnear.py = vtables with an object near
  every given point): each of the 5 sites has vt 0x2CABD8 oid 88297, vt 0x2CC7B0 oid 77452/55/57/
  59/61, vt 0x2D56F8 oid 9334 (rod top, z+550) and a cluster vt 0x2D8C48 oid 88361 + 0x2CAD78 oid
  9216 + 0x2D1EE8 oid 80398: site 1 (-1537,1471,3886), 2 (4902,4,3891), 3 (2240,-10122,939),
  4 (-11147,-767,1051), 5 (-4307,-1705,3888). Site 1 reached by play (hop from the start toward
  (-1537,1440) lands on the ridge, a second hop onto the rod platform; no electrocution with hp
  held): two insulator poles, red glowing ground wire down to a machine with a dark red octagonal
  valve, blue Sly-mask sparkle markers (Circle spots). Circle near the machine only crouches; the
  turn mechanic not found yet. Lightning bolts strike other rods (render fine).
- Bomb bridge retried with the thread fix (lw\bbx, chain.ps1 direct on_top, 2480:2): Sly stands
  by the truck with a blue Circle marker (no letterbox this time), Circle = hide under the truck,
  then nothing; 2483 (complete) is already state 3 right after 2480:2, so its stage events never
  ran - an artifact of forcing on_top (the job's ending cutscene played in an earlier chain).
- **Hack minigame reached** (lw\hk2, release direct t4_hack_path2 = Bentley at station 1's warp
  60450, then PS2_TASK_FILE 2414:1 re-activates station 1: HUD 0/6, Bentley next to the green
  terminal; Circle -> letterboxed hacking cutscene -> the minigame at (x,y,25000): blue cyberspace
  maze seen from above, Bentley's green ship, red enemies, green pickups, a ring "worm" on the
  circular track, red dot barriers. Twin-stick: left stick moves (lup = +x, lleft = +y; walls
  block), right stick fires in its direction (yellow shots). Renders clean.
  Screens ep4_hack_minigame.png, ep4_hack_minigame_fire.png. Maze not finished.
  hk3 (same start; NO PS2_POKE health hold - with hp held Bentley falling into the canal never
  dies and stays stuck swimming): maze explored with build\tools\probe.py (live stick probes +
  position; ship start (923,-855), room x 889..1600, y -926..-74; up the left corridor at
  y -160 to (2075,-115); right to y -640; a double row of red dots blocks the corridor: shooting
  (rup) removes them dot by dot, then through to the top area (3326,-644); red tank enemies shoot
  back (portrait health ring drains). Forcing 2414:2 inside the minigame does not end it (the
  minigame's own state machine does). Goal not reached. Everything renders and behaves sanely.
- Heist 1 (lw\h1b, release direct, -Wav): Bentley in the Prague streets at night, "Hijack the
  tank" (Job Help: "Use one of Contessa's tanks to secretly get into the prison"), L3 HUD (coins,
  0/30 clue bottles), two tank clusters patrol (objects 0x923B30/0x911B20, vt 0x2D6E08), guards
  with flashlights spot him (alarm). Audio 400 s: 0 underruns, zero runs only at load screens,
  8 isolated clicks, alarm music peaks at 0 dBFS with 11 clipped samples in 25 s (game mix).
- PS2_INPUT relative tokens chain (each +n counts from the previous token), so lw.ps1's four
  "+700/+760/+820/+880:cross" presses land at ~+0.7k/1.5k/2.3k/3.2k frames: the last ones are
  jumps in gameplay, which explains Bentley jumping into the canal at the m4_hack starts.

## In progress
- RC chopper (m4_rc_chopper, lw\rc*): with the stick released the chopper keeps swaying ~+-500
  units at near full speed (never settles); ep2's RC chopper trace shows the same. Not a dt effect:
  same with Count tied to VBlanks (rcv) and to flips (rcv3: dt exactly 1/60). Chopper = object
  0x9727D0 (vt 0x2D5898), child of a parent at (5270,-5779) (rotated); local pos +0x2B0, world
  matrix +0x2C0..+0x2F0, local velocity +0x330 (accelerates without input). Tracing every call
  with a0 = 0x9727D0 (build\expd = debug + PS2_EXP_A0 experiment, lw\rca0) to find the
  controller code. Is the sway the game's design or a recompiler/FPU/VU0 bug? Undecided.
  Per-frame fit (rcv3, build\tools\swayfit.py): constant accel phases (+1.0/frame up to ~61/frame,
  then -2.71/frame braking, reversal) = a velocity controller whose desired velocity keeps
  changing. Store watchpoint on +0x330 (experiment PS2_EXP_WWATCH, debug, build\tools\
  exp_wwatch_runtime.c/.h, lw\rcww): writers are 0x1F4898 (called by the Splice script VM via
  0x200950 with a vector arg 105.0: "+105 local x per frame" thrust phases of ~23 frames) and the
  physics step (fn 0x22DD00 path, ra 0x22E100). So the chopper is driven by a script; the thrust
  comes in periodic bursts with the stick neutral. PARKED (no evidence yet that it is a port bug).
  The stick really reads neutral: the game's JOY struct is at 0x2E0BC0 (+0x80 left stick
  angle/magnitude floats + raw lx/ly bytes, +0xB0 right stick; neutral = magnitude 0, found by
  RAM diffs with lright/rup held, build\tools\joyfind.py).
- Job Failed screen silence is the game's own: 989snd cmd 9 (master volume, groups 0/1) fades
  0x3BB -> 0 over ~40 VBlanks right after the failure (PS2_LOG_SND, lw\rcsnd). Not a bug.
- RC chopper audio "clicks" (wav_analyze finds ~70 in 4 min): noise bursts that switch on/off
  every ~0.1-0.2 s with no KON (PS2_LOG_SD=2 run lw\rcsd, build\tools\wavwin.py): the looping rotor
  sound ("whup-whup" noise bursts) - content, not a mixer artifact.
- Lightning job (lw\ltn): briefing (red bolts render), Sly on the roof, 0/5 rods; touching the
  electrified rod platform kills Sly -> respawn. Rods = "turn the wheels" (Circle?) - not solved.
- Heist 7 (lw\h7): Murray/Sly/Bentley escape over the bridges after Contessa's "time is up";
  guards attack; objective unknown ("Chase the Contessa"?). Warp 77611 (heist7_complete start)
  is at (400,-4960,2693), next to the heist7 start.

## Next
1. Hacking minigame (m4_hack station, heist4 keypads), lightning puzzle, Murray brawl.
2. Saving at a checkpoint (complete a task -> autosave), audio in a job.

## Findings / traps
- The machine is at 100% CPU (other testers' games): debug build runs 8-30 fps, release ~15-35.
  PS2_PERF numbers are not meaningful for 60 fps judgements under that load.
- The scratchpad dir is SHARED with the other testers (they overwrote look.ps1): keep helper
  scripts in scratchpad\ep4\ (lw.ps1 live warp, look.ps1, sheet.ps1, batch.ps1).
- Live helper lw.ps1: warp.py make --card <dir>\card [--job X --direct], copy exe,
  PS2_MEMCARD_DIR, then scripts\live_game.ps1 -Dir -Exe -Script '@187D80/1DD96C+30:start:10'.
  Release: one START at pad frame ~2600 may be too early under load; send another START live.
- PS2_INPUT stick tokens are lowercase hex (ryd0, not ryD0).
- Direct-start runs do not autosave at start (only the episode-start hub entry does).
