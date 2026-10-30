# r2-flicker: shadows flicker + a blinking line, Modern renderer only (owner report, ep1 hub outdoors)

Branch next/r2-flicker (from main 05097e6, main 35bfeef with R8 merged in), worktree
C:\Users\tanih\Projects\sly2-worktrees\r2-flicker. Owner: render_scale auto, filters modern, fullscreen 120 fps, 1440p.
Owner clue (via coordinator): it happens with light around Sly (lamps / lit areas, ep1 hub at night).

## Status
- [x] release build in worktree (build\rel; python scripts\build_game.py --release --out build\rel)
- [x] temporal tool: scripts\flicker_check.c (spikes, longest row run, blobs, blinking straight seams; --ab A/B of two
      replays; --png [crop]) + scripts\flicker_run.ps1 (warp run, every VBlank dumped)
- [ ] reproduce: searchlight / explosion near VBlank 7100-7400 (pad below)
- [ ] fix(es)
- [ ] temporal check in r2_sweep / gates.json
- [ ] gates

## How to reproduce (60 fps, 3x, raw BMP dumps of every VBlank)
  $hpt='r1:lup:1.5s,r2:rright:1s,r1:lup:1s,r1:cross:0.1s,r1:lleft:1s,r3:lup:2s,r2:rleft:1.3s,r5:lup:3.3s,r3:square:0.1s,r5:lup:2.5s'
  .\scripts\flicker_run.ps1 -Name x -Episode 1 -Job m1_brawl -Play -At 7000 -Frames 400 -Pad "+3000:cross:6,$hpt,$hpt" -Env "PS2_DUMP_WAIT=0;PS2_DUMP_RAW=1"
  (Sly walks into a guard's searchlight ~7100, gets hit ~7150-7170: explosion, camera shake.)

## Findings
- Frame dumps: PNG writer too slow for every VBlank at 3x; PS2_DUMP_BMP=1 (raw BMP) + PS2_DUMP_RAW=1 + NDUMP 32 keep up
  at 60 fps. PS2_DUMP_WAIT=1 at 60 fps stalls the EE (VBlanks pile up behind the blocked GS thread); at 120 fps OK.
- Bracket replay A/B (exact vs hybrid, 3x, 500 VBlanks incl. searchlight + explosion): no outliers (max 0.024% > 40).
  So the GPU fx passes on a fixed stream are fine; the bug needs the in-game hybrid path / timing.
- In game with PS2_R2_ALT (fx stay on in the exact frames too: gf_r2_fx_ttl): (1) a thin bright sliver along the top
  edge of the searchlight volume alternates (brighter in one mode); (2) v7162: a vertical seam at raw column 1024
  (native x 256), the light volume's tint on one side only, for the explosion frames.
- Scale switches (scaleup/scaledown tokens, auto): no glitch at the switch frames.
