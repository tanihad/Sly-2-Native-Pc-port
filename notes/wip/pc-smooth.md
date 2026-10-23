# pc-smooth (checkpoint)

**Goal**: remove the owner's stutter, menu shaking and shadow flicker on the PC port (GPU renderer, Balanced 3x, 16:9,
2560x1440 @ 165 Hz fixed refresh, frame rate 60 / 120 / 165).

## Done
- Read notes (frame-rate.md, d3d11-renderer.md N/O, pc-menu.md, pc-experience.md pacing), owner logs
  (C:\Users\tanih\AppData\Local\Sly2\sly2.log is from 10/22, an old build: F2 scale cycling, one governor drop
  3x -> 2x "busy 89%"; nothing about the latest session).
- Release build in the worktree: build\rel\sly2.exe (43 s).

## In progress
- Measuring: headless frame-time distributions; menu overlay / paused picture frame sequences.

## Next
1. Baseline measurements (headless): per-present intervals, spikes > 1.5x, at 60 / 120 / 165, ep1 hub / ep3 hub.
2. Menu shake: frame sequence with the PC Settings open (game picture + overlay), find what moves.
3. Shadow flicker: frame sequences at 60 vs 120 (worker), PS2_GFX_ASYNC=0, 1x vs 3x.
4. Fixes, gates, notes.

## Real-monitor runs (log each)
(none yet)

## Findings / traps
