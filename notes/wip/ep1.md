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

## In progress
- Boss fight live (build\boss, debug, direct start m1_boss).

## Next
1. Boss fight: try to beat Dimitri (dodge 4 bolts, hit when his ring malfunctions) -> episode end.
2. Direct starts not yet run: m1_sat, m1_follow_dimitri, m1_brawl.
3. Audio (-Wav + wav_analyze) in the hub and a job; PS2_PERF in release when the machine is quieter.

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
