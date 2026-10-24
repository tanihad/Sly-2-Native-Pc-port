# PC wording: no PS2 memory card, console or disc talk

The owner, on the packaged build: "in the starting menu before the game starts it talks about PS2
memory card - THIS IS A PC VERSION". This is what the PC port changes, and how. Everything is in
`runtime/pc_text.c` (+ `.h`) and four SHA-1 guarded instruction patches (`recomp/patches.py`,
section "PC wording"); the disc image is never touched. `PS2_PC_TEXT=0` turns all of it off (the
game's original texts, prompt and Options items, for comparison).

Contact sheet (before / after, error dialogs, menus): `notes/pc-text/contact_sheet.png`; full-size
shots in `build\screenshots\pc_text_*.png` (not committed).

## Where the texts live

Not in the ELF and not readable on the disc (the level files are compressed). Every level's file
has a **text bank**, which the loader `0x13DF80` reads into the heap for the selected language:
`world (0x3DD490) + 0x4640` = count, `+0x4644` = a table of `{u32 id, char* text}`. The global
texts (menus, memory card, options) are in every level's bank, with the same ids, next to the
level's own (objectives, dialogue, hints): 419 texts at boot, 550-710 in the levels.

The memory-card dialog manager (`0x2E6550`) shows text id N as dialog N (`+0x40`, set by
`0x1ACB58`), so the dialog ids below are text ids.

## Texts replaced (patch `pc_text_bank`, data-level)

Right after the loader has read a bank (before `0x13E0A4`, where all its paths join), each entry
whose id is in the table **and** whose bytes are exactly the original English text (same length,
same bytes) is overwritten in place with the PC text (always shorter; the rest is zeroed). Another
game version, region or language keeps its own texts. Every level load does this (all 19 replaced
on each of the 9 levels checked).

| id | where | original (NTSC-U) | PC |
|---:|---|---|---|
| 17 | boot: card folder unusable (mc.c "no card") | There is no Memory Card (8MB) (for PlayStation(R)2) inserted in MEMORY CARD slot 1 / Sly 2 requires 100 KB of free space / Please insert a Memory Card ... containing sufficient space or you will not be able to save your progress | The Sly 2 save folder cannot be used / Sly 2 requires 100 KB of free space / Check that the save folder can be written to and that the disk is not full, or you will not be able to save your progress |
| 18 | boot: card "unformatted" (`%unformatted` marker) | The Memory Card ... is unformatted / If you do not format it ... / Do you want to format? | The Sly 2 save folder is not set up / If you do not set it up ... / Do you want to set it up now? |
| 19 | formatting | Formatting the Memory Card ... / Please do not turn off your console or remove the Memory Card ... | Setting up the save folder... / Please do not close the game |
| 20 | format failed | An error occurred while formatting the Memory Card ... | The save folder could not be set up / You will not be able to save your progress |
| 21 | boot: no save data (now skipped, see below) | No Sly 2 Save File on the Memory Card ... / Do you want to create a Sly 2 Save File? | There is no Sly 2 save data yet / If you do not create it ... / Do you want to create Sly 2 save data? |
| 22 | creating | Creating the Sly 2 Save File on the Memory Card ... / Please do not turn off your console ... | Creating save data... / Please do not close the game |
| 23 | create failed | An error occurred while creating the Sly 2 Save File on the Memory Card ... | The Sly 2 save data could not be created / Check that the disk is not full / Your progress is not being saved |
| 24 | boot: card full (< ~100 KB of the 8 MB card left) | The Memory Card ... is full / ... Please insert a Memory Card ... | The Sly 2 save folder is full / Sly 2 requires 100 KB of free space / Remove other files from the save folder or you will not be able to save your progress |
| 26 | saving | Saving... / Please do not turn off your console or remove the Memory Card (8MB) (for PlayStation(R)2) | Saving... / Please do not close the game |
| 29 | write failed | An error occurred while writing to the Sly 2 Save File on the Memory Card ... | The Sly 2 save data could not be written / Check that the disk is not full / Data may be corrupted / ... |
| 33 | erasing a slot | Erasing... / Please do not turn off your console ... | Erasing... / Please do not close the game |
| 34 | before the first save (Save and Quit, new game) | When the following icon appears ... please do not turn off your console or remove the Memory Card ... | ... While this icon is on screen, please do not close the game |
| 35 | auto-save failed | Auto-save on the Memory Card ... has failed ... | Auto-save has failed / Auto-save has been disabled ... |
| 36 | card pulled out | The Memory Card ... was removed ... | The Sly 2 save folder cannot be reached ... |
| 42 | corrupt save | Some data in the Sly 2 Save File on the Memory Card ... was corrupt | Some Sly 2 save data was corrupt / Corrupted data has been cleared |
| 43 | disc tray opened | The disc was removed. / Please insert the Sly 2 disc to continue playing. | The game data could not be read. / Check that the disc image is there. |
| 46 | "load" without a save | Please insert a Memory Card ... that contains a Sly 2 Save File | There is no Sly 2 save data |
| 79, 80 | Options | USB Headset (for PlayStation(R)2): On / Off | USB Headset: On / Off (the item is hidden anyway, below) |

("/" = paragraph break; "..." = text shortened here; exact bytes in `runtime/pc_text.c`.)

Not changed, on purpose: the button glyphs (`&2X&.` etc.: PlayStation symbols, which a DualSense
shows; glyphs for other controllers are the input stream's), "Press START/SELECT button",
"Vibrations", "Speakers: Mono/Stereo/Surround", "No Controller", the analog-stick hints, the
publisher's legal screen (it names Sony Computer Entertainment America, not the console), and
"Save and Quit" / "Load Game" / "Save Game" (the game's own menus).

## Boot without the save prompt (patches `pc_save_boot_create`, `pc_save_boot_poll`, flow)

On PS2, a card without Sly 2 data stops the boot with "Do you want to create a Sly 2 Save File?"
(SQUARE = Yes). The card manager's per-frame update `0x1AB1A0` picks the boot dialog from the card
state (`0x2E65E0 +0x8`: 1 no card -> 0x11, 2 unformatted -> 0x12, **3 no save data -> 0x15**,
4 full -> 0x18; 6 = save data present: nothing). Before it shows 0x15 (at `0x1AB360`),
`pc_save_boot_create` does what the prompt's Yes does (`0x1AD168`): it starts the game's own
"create the save file" job (`0x1AAF90(0x2F633C, PMF 0x2A6D88 = {0xFFFF0000, 0x1AB868})`) in the
card worker thread, and the patch jumps to the function's epilogue (`goto L_001AB380`), so no
dialog. The job writes `/BASCUS-97316YAOTWTD!` with the three empty slots exactly as after Yes.
`pc_save_boot_poll` (at `0x1AB1DC`, every frame) waits for the worker (`+4` = -1) and, if the job
failed (`+0x10` != 0), shows the game's own "could not create" dialog 0x17 (its Continue then plays
without saving, as on PS2). It runs once per process; if the game somehow comes back to "no save
data" it asks as it always did. The boot still shows real problems: folder unusable (0x11), full
(0x18), unformatted (0x12) - in the PC wording above.

## Options without PS2 hardware items (patch `pc_options_hide`, flow)

The Options menu (title SELECT menu and pause menu; both are built by `0x1B04E0` from the static
item table `0x2BFEC8`) lists "USB Headset" (Logitech USB headset for PlayStation 2) and "Center
Screen..." (moves the picture on a CRT TV through the GS display offsets). Neither means anything
on PC; at `0x1B058C` (the item filter's test) those two items (kinds 0x50 and 0x3F) are left out.
The rest of the menu is unchanged and still works (checked: Vibrations toggles; wraps around). (pc-polish: the page now
shows only the save items plus Options / Quit to Desktop; its other options moved into the PC Options menu and the pause
menu has Save Game... / Load Game... directly: notes/pc-menu.md "One menu".)

## Verification (release build, headless, 2026-10-22/23)

- Fresh save folder, no input at all: SCEA screen -> intro -> title "Press START button for New
  Game"; the folder then holds the save directory and save1..3 (`[pc_text] no save data yet:
  creating it (no prompt)`, `save data created`).
- Existing save: no prompt, title "Press START button to Resume Game".
- New game -> pause -> Save and Quit -> Yes: notice 34 and "Saving..." in PC wording, the game
  restarts at the intro; Resume and title SELECT -> Load Game -> slot -> playing.
- Pause -> Options -> Save Game... -> slot -> "Do you want to overwrite this game?" -> Yes: saved
  (save1 rewritten; on PC the "Saving..." dialog lasts only a few frames).
- Auto-save at an episode start (warp episode 2): the auto-save icon, save1 rewritten.
- Pause -> Exit to Episode Menu -> Yes -> episode menu -> Start Episode -> safehouse.
- Errors: folder under a file (unusable) -> 0x11 PC text; 8.15 MB file in the folder -> 0x18;
  a host directory `bascus-97316yaotwtd!` (mc.c refuses the case clash) -> silent create fails ->
  0x17 PC text -> Continue -> intro.
- Census of the text banks of 9 levels (episode hubs 2, 8, 12, 14, 17, 27, 32, 38; levels 6, 43):
  no other console wording.
- Tests: `tests/unit/pc_text` (table invariants, in-place replacement, other versions/languages left
  alone, bad pointers, the Options filter); `tests/run_tests.py` checks the four patches against
  the game's ELF (SHA-1 guards match, the goto label exists); `tests/pc_text_boot.ps1 [-Out
  build\sly2]` boots a fresh save folder headless with no input and checks: save data created
  silently, all 19 texts replaced, no memory-card dialog ever shown (PS2_PEEK of 0x2E6590), the
  title's START check reached (passes on the release and the debug build).

## Left / not covered

- No texture-only PS2 wording was found on the screens visited (title, menus, save screens, legal
  screen); the save-slot screen ("Data Access") has none. The auto-save icon is a book, not a
  memory card. Screens not visited may still show console-specific art.
- Other languages: only English (the NTSC-U disc has only English text) is replaced.
- The memory card itself is still an emulated 8 MB card (runtime/mc.c): "Sly 2 requires 100 KB of
  free space" and the "full" case refer to that limit.
