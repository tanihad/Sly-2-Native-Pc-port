# warp: start the game at any episode / job

**Goal**: a reliable way (scripts/warp.py + notes/warp.md) to start Sly 2 at the beginning of any
episode (Prologue, Episodes 1-8) and ideally at a chosen job, verified by loading every episode.

## Done
- (nothing yet)

## In progress
- Reverse-engineering the save file (save1..3, 21512 bytes) and the game's save/load code.

## Next
1. Find the save/load code in the EE executable (strings "save1", "BASCUS-97316", mcRead/mcWrite
   callers), the checksum and the game-state struct.
2. Write scripts/warp.py to generate saves for each episode start.
3. Verify each episode loads; dump frames to build\screenshots\warp_epN.png.

## Findings / traps
