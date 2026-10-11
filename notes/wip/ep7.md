# ep7: playtest Episode 7 "Menace from the North, Eh!" (Canada)

**Goal**: play Episode 7 (hub world 32, jobs m7_*) as far as possible with warp saves and scripted /
live input; fix every runtime/recompiler problem found at its root (with tests); frames in
build\screenshots\ep7_*.png.

## Done
- Merged next/warp (178353c). Built release (build\rel) and debug (build\sly2).
- Episode start (release, warp save): title -> safehouse -> X -> Binocucom briefing -> Sly in the
  snow hub, running around: all rendered right, no log problems (ep7_safehouse.png, ep7_hub_start.png).
- Survey: direct starts (debug, warp_run -Direct, 240 s each; build\warp\s_<job>) of m7_bearcave,
  m7_robot_war, m7_laser (world 34), m7_lighthouse, m7_boat_hack, m7_bear, m7_thermals, m7_heist,
  m7_boss (world 37) + interiors: every intro (Binocucom briefings, letterboxed cutscenes) and the
  first gameplay frames look right; logs only have the known boot lines.
- Machine is shared and at 100% CPU (other ep testers): release runs at 20-40 VBlanks/s, so perf
  numbers from now are not meaningful.

- Survey of interiors and heist stages (t7_*_intro_int, t7_boss_battle, t7_heist1..6_intro): all
  load and render right (Murray power-chop minigame, Bison cutscenes, log flume stages).
- Audio: build\recon\audio.wav (11 min, hub + Binocucom): no dropouts, no clicks (wav_analyze).
- Autosave: the game writes save1 itself after the episode start (dt 108.9, ice_wall_trainer seen).
- Recon: Binocucom (R3), photo targets shown as blue pins; aim = left stick (with acceleration,
  vertical inverted), R1 = take picture ("&2R&.: take picture" in level text), right stick zoom.
- Power chop (t7_heist1): marker sweeps, CROSS chops; needs frame-exact timing, not finished.

- Boss battle (t7_boss_battle, world 37): Bentley vs Bison, "Press O for Flames, /\ for Saws, [] for
  Logs"; mashing loses (turtle-soup game over + retry work). PS2_POKE=3D4AC8=28 (committed debug aid)
  holds Bentley's health but Bison then pins him in a hit loop: not won.
- Post boss (direct t7_boss_battle_complete, hub): "Get to the silo before the blimp has taken off"
  timed run as Bentley; silo location unknown; timeout -> "Job Failed! ... Press X to try again"
  works (ep7_postboss_*.png). SELECT = Gadget Grid (no map).
- RC Combat Club (direct t7_robot_war, world 33): Murray's RC tank vs moose tanks; mashing r1/[]/O/X
  damages the enemy; death -> mask wipe -> retry works. Seen once: a thin blue full-width band at
  field rows 107-110 on the frame before the death fade (frame_009750) -> tracing it.

- Merged next/warp again (ce8bff7: entry hooks -> direct starts + title anchor in release builds).
- FIXED (1df4920) live input: a PS2_INPUT_FILE line > 16 KB was never read and blocked all later
  lines; a session took no input after 4096 tokens in all (my long mash.py lines did nothing in
  boss2/rcwar). Test: tests/unit/window test_live_input.

- RC combat blue band (1 frame, camera cuts only: combat start, respawn): brief trace showed it at
  v4862/4864: tristrips rows 107..111, x shifting -113..423 -> -10..526, rgba 334C6680 (the hub's
  night-sky blue) at z 1040 / z 16, ZBUF PSMZ16S, FBMSK FF000000; later frames draw the same "far
  fill" black (trifan rows 66..152, z 1040, ZTST GREATER, Z16S on the Z32 buffer: the GS emulates
  VRAM swizzles so the aliasing is faithful). Not reproduced in two release runs; judged a one-frame
  game artifact (old sky colour/frustum on the cut frame), not investigated further.
- Job Failed panel (RC combat "Your robot tank has been destroyed", post boss) renders right.
- Boat hack (release direct t7_boat_hack_intro): briefing, Bentley in the hub, found Murray with
  findobj (oid 9 vt 2D44C8 at -10463,5028,-732) + live_goto; triangle next to Murray = Murray
  throws Bentley (once), elsewhere = Bentley's bomb (explosion, damage, respawn). Water, explosion
  and respawn render right. Throw aim not worked out; hack minigame not reached (ep4/ep5 have
  direct-start hack jobs).
- Old Grizzle Face (release direct t7_bear_intro): briefing (Bentley bombs the water, fish fly,
  Murray throws them at the "oil machines" so the bear smashes them): fish fly and land, circle
  picks one up / throws it, bear and guards walk around; station not hit (aim). Logs clean.
- Machine very loaded (7 games at once): release runs at 20-35 VBlanks/s, IOP timer 165-240/s.

- FIXED (1d1a7a6) dark speckled bottom picture line (output line 446) in every frame: PCRTC field
  mode gave circuit 1 (anti-flicker, DBY 1, DH 446) 224 lines in one field, so its last line read
  buffer row 224 (Z buffer / other buffer) and was mixed 50% into the picture. Now (DH+1)/2 lines
  per field, rounded down (PCSX2's GSdx does the same). Found with the GS recording replay
  (fix-perf's build\title.gsrec) + a temporary pixel watch in draw() (build\tools\pixwatch.patch:
  PS2_GS_PIXWATCH=fbp,x,y prints every change of that pixel with the primitive; replay only).

- Episode end (release, live: direct t7_boss_battle_complete, then PS2_TASK_FILE "3290:2" =
  t7_boss_get_to_battery done): blimp/silo cutscene, "Job Completed!", the ep7 ending story movie,
  Episode 8 card (START = pause menu "We'll be right back!"), ep8 menu "Anatomy for Disaster / X:
  Start Episode", X -> ep8 intro movie (Clockwerk story), title card, ep8 safehouse "Press X to
  play as Sly". save1 rewritten: episodeCurrent 8, sawmill 100%. ep7_ending_sheet.png,
  ep7_to_ep8_sheet.png, ep7_ep8_safehouse.png. Only log line of note: one "[sd] VoiceTrans ...
  overwrites the block voice c0 v4 is playing" at the hub load (a 776 KB bank load over a voice
  still sounding from the previous level; game-side, at a level load).

- Merged main 8e05f84 (VU microcode recompiler) -> 60268a2; rebuilt rel + debug. Ep7 start
  (release, PS2_PERF, 6 other games running, CPU 100%): Binocucom briefing and hub render right
  with the recompiled VU; GS thread 15-30 ms/VBlank is the bottleneck under this load (EE busy 6-10
  ms), so 30-50 VBlanks/s; not meaningful on this machine (D3D11 renderer is main's plan).
- Thermal Ride controls (level text + wiki): HOLD R1 in the air = paraglider ("Press &2R&. while
  jumping"; &2R = R1, &2r = R2, &2L = L1, &2l = L2, &2S/T/O/X = square/triangle/circle/cross,
  &2+ = d-pad, &2( / &2) = left / right stick). Short R1 taps do nothing. Gliding descends ~5/VBlank;
  flying low over a burning oil main destroys the glider (pieces fly off, Sly falls) -> "Job
  Failed! You need to paraglide out to the icebergs using the thermals", X retries on the
  lighthouse top. 0x2DE1C0 (camera focus) does not follow jumps; Sly's object in this level:
  0x10E46C0 (vt 2D2E98), position +0x230.

- Thermal Ride play (release live sessions build\therm2..5): paraglider + thermals work. Letting
  go of R1 = the glider comes apart (pieces fly off) and Sly falls: that was every "glider broke"
  I saw (no bug). PS2_INPUT holds could not be ended early -> added the "release" token
  (4f432a6, test in tests/unit/window). Thermal columns (objects vt 2D9978, oids 33541..33545, base
  positions; rising ~9/VBlank inside, constant full-left stick keeps Sly circling inside):
  T1 (-10046,-270) tops out ~6000, T2 (-7323,5657) ~6900, T3 (-750,5565) ~7550, T4 (2476,984),
  T5 (-1664,-4454) (T4/T5 not reached yet). Glide: ~8/VBlank forward, ~5 down (ratio ~1.6);
  arriving at T2 below ~1900 hits its hatch structure. 33549 (-4909,-18578) is probably the
  iceberg (seen from the lighthouse top). t7_thermals_get_height (3180) stays 1 even at 7558 high;
  forcing it (PS2_TASK_FILE 3180:2) -> eagle_attack active, camera flips to look at Sly from the
  front, no eagles seen nearby; Job Failed resets the job's tasks. Water = respawn/Job Failed,
  underwater view renders right. While the camera faces Sly, the stick is still camera-relative.
- Tools (build\tools): glide.py (autopilot: @x:y[:oR][:zZ][:tS] waypoints, orbit, camera-relative
  stick, PS2_PEEK 2DE1C0 + 2DDFD0), ptrace.py (peek values over time), objbox.py / objmove.py
  (objects by +0x230 position / moving between two RAM dumps), strs.py (level text in a RAM dump),
  live_game7.ps1 (RING env = PS2_DUMP_RING), live7.ps1 -Extra "K=V;K=V".

- Session 3 (resumed after the session limit): merged main 2d00835 (EE thread fiber leak fix) ->
  177c3ff; rebuilt rel + debug. Cherry-picked next/ep3's live 'pause' token (204385d -> 5685f68;
  conflict with my 'release' token resolved: PAUSE -5, RELEASE -6; window unit test passes).
- Hub perf (release, PS2_PERF, 4-5 other games): 45-55 VBlanks/s, 30-43 flips/s; GS thread 15-19 ms
  per VBlank is the bottleneck (EE busy 7-10 ms, VU 100% recompiled 4-6 ns/pair); memory flat 149 MB
  (thread fix works). Later the machine went to 100% CPU (10 games): no perf numbers from then on.
- New direct starts (release): t7_laser_outside (laser intro cutscene with the beam, Sly in the hub
  with a red "!" alert marker), t7_bearcave_back_outside ("have to place the radio", Sly in the snow),
  t7_robot_war_sly_get_costume (world 33 interior): all load and render right.
- Laser live (build\laser2): laser beam = thin red/orange additive line (seen from a distance and
  ending at a house window); Binocucom shows "Use crystals to free book / Laser Redirection"; level
  text: "Use O to place crystals at the end of the laser". Guard fight: knocked-down Sly with stars,
  ghosting afterimages (game's own feedback blur, judged OK), coins burst. Fell in the water (respawn).
- Episode 6 -> 7 in the game's own flow (build\ep7intro: direct t6_heist_complete): ep6 ending
  cutscenes, "Job Completed!", ep6 ending movie, Episode 7 card (slideshow; START = "We'll be right
  back!" menu, X = episode menu "Menace in the North, Eh? X: Start Episode 0% 0:00"), X -> ep7 intro
  movie (Bison story), "Sly Cooper and the Gang in: Menace from the North, eh!" title card, safehouse,
  X -> Sly in the hub; autosave save1 (episodeCurrent 7, widCurrent 32). ep7_title_card.png,
  ep7_intro_movie_sheet.png, ep7_hub_after_intro.png. Log: one more "[sd] VoiceTrans ... overwrites the
  block voice c0 v9 is playing" at a level load (same game-side pattern as before).
- Heist 6 (log flume cheat, Sly, hub): Bison log-rolling, judge on a platform, gadget meter; Gadget
  Grid -> Alarm Clock assigned to L1 (button glyphs L1/L2/R2 render), L1 = aim arc, L1 = throw (meter
  drops). "Lure the guards into the cave with your Alarm Clock gadget." Not completed (cave unknown).
- Boss (build\boss3/boss4, pause-token stepping, build\tools\step.ps1): Bentley vs Bison; idle =
  "Looks like I'm tougher than you are clever!" game-over cutscene + mask wipe + retry. SQUARE fires
  the fire-pad grates (big flame burst ~30 frames later, Bentley burns and is charred): fire renders
  right (ep7_boss_flames.png). Traps per level data: c_ewok_trap_fire_pad / _circular_saw / _log.
  With PS2_POKE health hold Bison pins Bentley in an endless hit loop (as before) -> not won;
  parked (game logic, no runtime issue seen).
- Laser job finished in context (build\laser3, PS2_TASK_FILE crystals 3099..3113 -> 2): laser melts
  the ice block (steam where the beam hits), a mammoth walks out, "Log Chopping Guide!" loot card,
  "Job Completed!" book, autosave icon; save1: m7_laser complete, t7_laser_complete state 3,
  sawmill 25%. 3069/3083 (bearcave/robot war complete) set from state 0 -> chalktalk2 (3118) state 1
  but no chalk talk (it plays in context only; chalk talks verified by ep2/ep3/ep5).
- Heist 4 (wall hook cheat, Murray, build\wallhook): cutscene (judges with score cards, Bison climbs
  the ice wall), idle -> "Job Failed! Jean Bison was able to make it to the top of the wall in
  time", X retry, hint marquee. Renders right. (--job t7_heist3_wall_hook starts heist4_intro: tasks
  without a world go to the next stage with one.)
- VU recompiler check on ep7 content (release, PS2_VU_VERIFY=1, build\warp\vv_*): hub gameplay
  9.4M programs, RC combat (world 33) 9.4M, lighthouse interior (35) 5.2M, bear cave (36) 5.2M, boss
  arena (37) 5.2M: 0 mismatches.
- Power chop (heist1, Murray, build\chop): meter marker (green inside the log icon, red outside)
  sweeps ~60 VBlanks per pass; CROSS freezes it and Murray chops (strength by distance from the
  centre: wood chips + growing crack). Bot build\tools\chop.py (pause token, predicts the centre
  crossing, latency ~4.4 VBlanks) froze it within +-30 px of the centre; on this 100%-loaded machine
  the game's frame steps vary (2-10 px per VBlank), so prediction is loose. Log split after ~10 hits ->
  "Job Failed! You need to break the log in fewer hits to beat Jean Bison", X retry works.

## In progress
- Perf sweep (release, PS2_PERF) of the ep7 hub and the heavy job levels while the machine is quiet.
- Thermal Ride: full tour T1..T5 with glide.py (build\therm5\glide.out) to see where get_height
  completes (gameplay only, lower priority).

## Tools (build\tools, not committed; the scratchpad is shared with other agents!)
- live7.ps1 -Name x -Make "--episode 7 --job <task> --direct" -Script "@187D80/1DD96C+30:start:10"
  -Debug: live session with warp card, PS2_PEEK player/camera; sheet.ps1 contact sheets.
- findobj.py ram.bin 7 8 9: character objects (+0xC vtable in .data, +0x18 oid; 7 Sly, 8 Bentley,
  9 Murray), world position at +0x230 (player: same as 0x2DE1C0 focus minus 125 in z).
  hexf.py ram.bin addr: words as hex+float. RAM dump: live token "+0:ram".

## Next
1. Episode start: `scripts\warp_run.ps1 -Episode 7 -Play -Release`, look at hub, PS2_PERF.
2. Jobs in order (warp.py list --episode 7): m7_recon, m7_bearcave, m7_robot_war, m7_laser,
   m7_lighthouse, m7_boat_hack, m7_bear, m7_thermals, m7_heist, m7_boss. Direct starts (debug).
3. Cutscenes/movies, checkpoint saves, audio (-Wav), perf.

## Findings / traps
