# warp: start the game at any episode / job

**Goal**: a reliable way (scripts/warp.py + notes/warp.md) to start Sly 2 at the beginning of any
episode (Prologue, Episodes 1-8) and ideally at a chosen job, verified by loading every episode.

## Done
- Save format reversed (see Findings). scripts/warp.py (list/make/dump/set/schema) +
  scripts/warp_schema.txt (4472 fields, extracted from a RAM dump with `warp.py schema --ram`).
- scripts/run_game.ps1 -Card <dir> (PS2_MEMCARD_DIR).
- **Episode 1 warp works** (debug build, `warp.py make --episode 1 --card build\cards\ep1`, run with
  -Pad "@187D80/1DD96C+30:start:10"): title -> START -> "Sly Cooper the thief" intro + Paris
  flyover -> safehouse table "Press X to play as Sly" (build\screenshots\warp_ep1_safehouse.png).

## In progress
- Running every episode (0-8) and pressing X at the safehouse to reach hub gameplay.

## Next
1. Episodes 0, 2-8: generate cards, run, save frames build\screenshots\warp_epN.png.
2. Jobs: figure out how the safehouse job menu picks jobs (m<ep>_*.is_complete, task states), then
   implement --job (complete the earlier jobs' tasks).
3. Episode menu (dvd_menu world 1, "Start Episode" text) - the game's own episode select after
   finishing; check whether a save can open it.
4. notes/warp.md (usage), notes/progress.md pointer, final commit.

## Findings / traps
- Save file = the game-state blob at EE 0x3D4A60 (0x5408 bytes): fields laid out by a schema the
  game loads from every level file (object 0x3E0EA8: {count, 10-byte entries {sym,type,count,
  offset,child,sibling}, 0x35-bucket symbol hash}). Trailer 0x5400 = u16 offset of the last entry
  (0x5098), 0x5404 = byte sum of the entry table (2582452); mismatch -> the game wipes the save
  (FUN_001d7c80). No checksum over the data itself.
- Fields: episodeCurrent, episodeBest, widCurrent, oidPoCurrent (7 = Sly), dt (play time; 0 =
  empty slot -> new game), flags.*, sly/bentley/murray hp/mp, powerUps, loot, per-episode dt /
  percent_complete / level booty+clues+vault, 78 missions m<ep>_<job> {focus_count, is_complete,
  time_played, suck_value}, 795 tasks t<ep>_<task> {[focus_count], complete_count, is_complete,
  state, from_memcard}. When saving, the game sets every from_memcard to 1 (FUN_001d84b0).
- Resume (FUN_00165e38): dt == 0 -> new game (episode 0 descriptor, warp 0x62f); else loads the hub
  descriptor of episodeCurrent (table *(0x3E1C50) + ep*64), or world desc 0x2a3828 if
  episode[cur].percent_complete > 99. widCurrent is NOT used for resume (it is rewritten on every
  world load from the loaded world, FUN_0020e3c0, with episodeBest = max).
- wid -> episode table *(0x3E1C44): 0-1 ep0 (exterior, dvd_menu), 2-7 ep1, 8-11 ep2, 12-13 ep3,
  14-16 ep4, 17-26 ep5, 27-31 ep6, 32-37 ep7, 38-43 ep8 (same order as the schema's level lists).
- Slot choice: the game loads all 3 slots, ignores dt==0 slots and resumes the newest by card
  mtime (FUN_001ab418 mode 6) -> warp.py zeroes the other slots and sets the %meta mtime.
- Memory card manager object 0x2E5... (slots 0x5464 bytes from +0x24); mcserv paths
  "/BASCUS-97316YAOTWTD!/save%d".
- Task state numbers seen in a prologue save: attract_mode 2 (+complete_count 1), rooftop tasks 3
  (is_complete 1, complete_count 0), security_shutdown 1 (current objective).
- Ghidra headless project for decompiling: scratchpad ghidra/ (analysis takes ~45 s); xref helper.
