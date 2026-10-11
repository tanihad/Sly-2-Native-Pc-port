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

## In progress
- Survey of interiors and heist stages (t7_*_intro_int, t7_boss_battle, t7_heist1..6_intro).

## Next
1. Episode start: `scripts\warp_run.ps1 -Episode 7 -Play -Release`, look at hub, PS2_PERF.
2. Jobs in order (warp.py list --episode 7): m7_recon, m7_bearcave, m7_robot_war, m7_laser,
   m7_lighthouse, m7_boat_hack, m7_bear, m7_thermals, m7_heist, m7_boss. Direct starts (debug).
3. Cutscenes/movies, checkpoint saves, audio (-Wav), perf.

## Findings / traps
