# warp: start the game at any episode / job

**Goal**: a reliable way (scripts/warp.py + notes/warp.md) to start Sly 2 at the beginning of any
episode (Prologue, Episodes 1-8) and ideally at a chosen job, verified by loading every episode.

## Done
- Save format reversed; scripts/warp.py (list/make/dump/set/schema/tasks), scripts/warp_schema.txt
  (4472 fields), scripts/warp_tasks.txt (795 tasks: oid, job, prerequisites, start world/warp,
  character; 77 job titles), scripts/warp_run.ps1 (-Job -Direct -Play -Release), run_game.ps1 -Card.
- All 9 episode starts verified (debug build; ep0/ep4/ep7 also release): frames
  build\screenshots\warp_ep0..8.png (+ warp_all.png).
- Job warps verified via Job Help (check marks = done, target available): m1_brawl, m1_disco,
  m1_boss, m2_rc_chopper, m3_heist, m4_hack (as Bentley), m5_hack, m6_paraglide, m7_boss, m8_boss.
- Direct job start (runtime/warp.c, PS2_WARP_TASK, debug build): m1_brawl (Murray, hub),
  m1_moulin_power (world 6), t1_heist_5_peacock_climb (mid-heist stage) all start the job by itself.
- Story gadgets (Paraglide bit 27, Alarm Clock bit 26) confirmed in ThiefNet.
- tests/unit/warp (113 checks); all unit tests + run_tests.py pass; release build OK.

## In progress
- notes/warp.md (usage guide), progress.md/hle-architecture.md pointers, final commit.

## Next
1. Write notes/warp.md; mark this file complete.

## Findings / traps
- Save file = the game-state blob at EE 0x3D4A60 (0x5408 bytes), laid out by a schema loaded from
  every level file (object 0x3E0EA8: {count, 10-byte entries {sym,type,count,offset,child,sibling},
  0x35-bucket symbol hash}). Trailer 0x5400 = u16 offset of the last entry (0x5098), 0x5404 = byte
  sum of the entry table (2582452); mismatch -> the game wipes the save (FUN_001d7c80).
- Resume (FUN_00165e38): dt == 0 -> new game (pending task 0x62f, flags 0x48); else hub descriptor
  of episodeCurrent (*(0x3E1C50) + ep*64), or 0x2a3828 (episode menu world) if
  episode[cur].percent_complete > 99. widCurrent is rewritten on every load (FUN_0020e3c0).
- Task objects (vtable 0x2CF1E8): +0x18 oid, +0x54 state, +0x7C mission, +0x94/+0x98 prerequisites,
  +0xA0/+0xA4 successors, +0xB8 warp, +0xBC world, +0xC0 character, game-state paths at +0xC8..+0xE8.
  Mission objects: +0x64 start task, +0x68 final task, +0x6C title text id, path at +0xD0.
- Loading (FUN_00164270): a job task saved in state 1/2 is reset (0/1) -> save finished tasks as 3.
  Progression is event driven: completing tasks in a save does not activate the next ones, so
  warp.py activates the frontier (state 1) itself.
- Pending-task start: 0x2DEB48 = task oid + transition flag 8 -> after the load 0x165280 starts the
  task (state 1, character, warp DAT_003e10c8). FUN_001651d8 uses this to start a job in another
  level; runtime/warp.c turns the resume's 0x22B490 call (ra 0x165FE4) into that.
- Safehouse menu default = saved player (oidPoCurrent) if selectable, else ThiefNet; episode 4
  starts with Bentley only. Episode menu (dvd_menu world) only offers the current episode.
- Power-up table 0x2BCD00 (32-byte records: kind, character oid, cost, ?, shop slot, text ids).
