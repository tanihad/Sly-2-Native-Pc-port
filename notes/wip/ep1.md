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

## Next
1. Hub music silence: PS2_LOG_SND run in the hub, see whether proc 6 volumes go to 0.
2. PS2_PERF in release when the machine is quieter.
3. If time: boss again without the poke, using cover (barrels/glass) and the 3rd/4th-shot jam.

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
