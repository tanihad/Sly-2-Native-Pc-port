# fix-input: memory card prompt ignores the controller

**Goal**: find why the "No Sly 2 Save File ... create?" prompt ignores input, fix the root cause in
the runtime, then get as far into the game as possible with scripted input.

## Done
- Debug tools (runtime): `PS2_CALLS=<vblank>[:<frames>]` prints every recompiled function called in
  that window with call counts and a thread bitmask (ps2_runtime.c/.h; debug builds only);
  `PS2_LOG_THREADS=1` logs thread create/start/exit/priority and dumps all threads every
  PS2_PEEK_EVERY VBlanks (threads.c).
- **Prompt "ignores input": not a runtime bug.** The prompt's "Yes" glyph is a pink SQUARE, not O
  ("[] : Yes   No : /\"). Its input handler is 0x1ACCC8 (dialog manager 0x2E6550, called every frame):
  for a two-button dialog it tests JOY+0x10B8 (newly pressed) with 0x80 (Square) -> 0x1AD168 and
  0x10 (Triangle) -> 0x1ACFE0; one-button dialogs test 0x80 only. Cross (0x40) and Circle (0x20)
  are not looked at, which is why the earlier test (cross, circle) did nothing.
  With PS2_INPUT="600:square:12" the game creates /BASCUS-97316YAOTWTD! with
  BASCUS-97316YAOTWTD!, icon.sys, memcard_icon.ico, save1..3 and goes on to the title scene
  (frames build/frames1/frame_000900.png, frame_001080.png).

## In progress
- Scripted run past the title screen.

## Next
1. Title screen -> menus -> first gameplay with scripted input; dump frames.
2. Fix each runtime blocker found on the way.

## Findings / traps
- Env vars do not persist between PowerShell tool calls; set PS2_INPUT etc. in the same command.
- vblank_count counts *serviced* VBlanks (game runs ~77% speed), so 22 s of run ~ v900.
- Memcard manager: per-frame update 0x1AB1A0 (this = 0x2E65E0); operations run in a worker thread
  (entry 0x1AAF18, priority 3, created by 0x1AAF90 each time; struct at this+0xFD5C = 0x2F633C);
  GetInfo poll re-created every ~0.5 s. Main thread runs at priority 5.
