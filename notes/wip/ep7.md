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

## In progress
- RC combat blue band: PS2_GS_TRACE_RECT=250,104,252,112 MINSIZE=300 BRIEF, build\rcwar\trace.txt.

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
