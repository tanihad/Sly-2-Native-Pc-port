# pc-text: remove PS2 wording and console-only flows (checkpoint)

**Goal**: no memory-card prompts at boot (save data created silently), PC wording for every save /
load / error message, the game's own menus and save logic kept. Worktree
`C:\Users\tanih\Projects\sly2-worktrees\pc-text`, branch next/pc-text.

## Done
- Found the text: one text bank per level in EE RAM, `{u32 id, char* text}` pairs, count at
  world+0x4640 = 0x3E1AD0, table pointer at 0x3E1AD4 (boot: 419 entries). Not plain on the disc
  (compressed). Memory-card texts are ids 17..46.
- Memory-card dialog manager 0x2E6550: +0x40 dialog = text id (-1 none), +0x44 mode (1 = boot
  check, 2 = no saving, 3 = after a failed job), +0x48 time shown, +0x4C what asked (0 at boot);
  set 0x1ACB58, close 0x1ACBB8, per-frame input/update 0x1ACCC8 (table 0x2A6D90), buttons per
  dialog 0x1ACC80 (0x2A6D10), Yes 0x1AD168, No/OK 0x1ACFE0 (0x2A6E10).
- Card manager 0x2E65E0 (+0x8 state: 1 no card, 2 unformatted, 3 no save file, 4 full, 6 save
  present; worker struct +0xFD5C = 0x2F633C (+4 thread, -1 idle; +0x10 result), start job
  0x1AAF90(worker, PMF)). Per-frame 0x1AB1A0 shows the boot dialogs (0x11, 0x12, 0x15, 0x18, 0x2E).
  Create-save job PMF at 0x2A6D88 = {0xFFFF0000, 0x1AB868}.
- Text bank loader = 0x13DF80 (world 0x3DD490 in $s5); patch "pc_text_bank" before 0x13E0A4.
- runtime/pc_text.c/.h (19 texts, exact-match in-place replacement; PS2_PC_TEXT=0 = original),
  patches pc_text_bank / pc_save_boot_poll (0x1AB1DC) / pc_save_boot_create (0x1AB360 -> goto
  L_001AB380). ps2_runtime.h includes pc_text.h. tests/unit/pc_text passes.
- VERIFIED (release, headless): fresh card -> no prompt, save created, intro + title without input;
  19/19 texts replaced; Save and Quit notice + "Saving..." PC wording; save via Options > Save Game;
  load via title SELECT > Load Game; resume title. Shots in build\screenshots\pc_text_*.png.
- Census: episode 1 hub bank has no other PS2 words except "Center Screen..." (id 63).
  The Options menu shows "USB Headset: Off" and "Center Screen..." (PS2 hardware options).

## In progress
- "Before" shots with PS2_PC_TEXT=0; error dialogs (full card, unusable folder, create failure).

## Next
1. Error scenarios: card full (8 MB file in card root -> dialog 0x18), folder unusable (0x11),
   create failure (a file named BASCUS-97316YAOTWTD! -> dialog 0x17 via pc_save_boot_poll).
2. Decide USB Headset / Center Screen (hide from Options? at least document).
3. run_tests.py guard checks for the 3 patches; all gates; package.py.
4. notes/pc-text.md + contact sheet notes/pc-text/; census of more levels (warp hubs).

## Findings / traps
- Scratchpad helpers (session-local): mdis.py, imm.py, textbank.py, ramstr.py, sheet.ps1 (contact
  sheets with System.Drawing), live.ps1 (headless live game at 1x: PS2_GS_SCALE=1, dumps ~1 MB),
  step.ps1 (send live input, save latest frame). Frame dumps at 3x are 9.6 MB: use PS2_GS_SCALE=1.
- Live input "pause" token freezes the game at the next dumped frame: use it to catch short dialogs.
  In-game "Saving..." lasts < 4 frames on PC (host files are instant).
- Pause menu (wheel): Keep Playing, Save and Quit, [Exit to Episode Menu], Options, Controls, Job
  Help. Options: Load Game..., Save Game..., Speakers, Music Volume, Effects Volume, USB Headset,
  Invert Binocucom, Invert Turret, Center Screen..., Vibrations. Overwrite prompt: SQUARE = Yes.
