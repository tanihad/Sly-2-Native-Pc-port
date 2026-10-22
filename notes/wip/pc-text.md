# pc-text: remove PS2 wording and console-only flows (checkpoint)

**Goal**: no memory-card prompts at boot (save data created silently), PC wording for every save /
load / error message, the game's own menus and save logic kept. Worktree
`C:\Users\tanih\Projects\sly2-worktrees\pc-text`, branch next/pc-text.

## Done
- Found the text: one text bank in EE RAM, `{u32 id, char* text}` pairs, count at 0x3E1AD0,
  table pointer at 0x3E1AD4 (boot: 419 entries at 0x4B2830, loaded at VBlank ~289 on a fresh boot).
  Not plain on the disc (compressed). Memory-card texts are ids 17..46; see notes/pc-text.md.
- Memory-card dialog manager 0x2E6550: +0x40 dialog = text id (-1 none), +0x44 mode (1 = boot
  check, 2 = no saving, 3 = ...), +0x48 time shown, +0x4C what asked (0 at boot); set 0x1ACB58,
  close 0x1ACBB8, per-frame input/update 0x1ACCC8 (table 0x2A6D90), buttons per dialog 0x1ACC80
  (0x2A6D10), Yes 0x1AD168, No/OK 0x1ACFE0 (0x2A6E10).
- Card manager 0x2E65E0 (+0x8 state: 2 no card, 3 unformatted, 4 no save file, 5 full, 6 save
  present; worker struct +0xFD5C = 0x2F633C, busy test 0x1AB088, start job 0x1AAF90(worker, PMF);
  result word 0x2F634C). Per-frame 0x1AB1A0 shows the boot dialogs (0x11, 0x12, 0x15, 0x18, 0x2E).
  Create-save job PMF at 0x2A6D88 = {0xFFFF0000, 0x1AB868}.

## In progress
- Finding the text bank loader (debug build, PS2_CALLS around v287) for the replacement hook.

## Next
1. runtime/pc_text.c: table id -> (original, PC text), in-place replacement with exact compare.
2. recomp/patches.py: hook after the bank load; silent create at boot (0x1AB360 path) + error check.
3. Tests: tests/unit/pc_text, run_tests guards; scripted boot on a fresh card (no prompt).
4. Screens: save, autosave, Save and Quit, load; screenshots; notes/pc-text.md.

## Findings / traps
- dis helpers in the session scratchpad (mdis.py, imm.py, textbank.py); frame dumps are 9.6 MB
  each at 3x: keep DumpEvery large and delete after looking.
