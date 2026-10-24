# pc-polish: Quit to Desktop, one coherent menu, understandable resolution, Apply / Revert (checkpoint)

**Goal**: owner feedback "no way to quit the game, what's the use of the old pause menu", "internal vs actual
resolution confusing, things don't apply instantly (Apply button)". Worktree
`C:\Users\tanih\Projects\sly2-worktrees\pc-polish`, branch next/pc-polish (from main f3bb29f).

## Design (decided)
- Game pause menu (page 1, f_001B0290 list copied by pcmenu_pause_list): Keep Playing / Save and Quit /
  [Exit to Episode Menu] / [Abandon Job] / **Save Game...** / **Load Game...** (the game's own items 0x3E / 0x3D
  copied from the Options table 0x2BFEC8) / **Options** (ours, was "PC Settings") / Controls... / Job Help /
  **Quit to Desktop** (ours). The game's own "Options" entry (0x2C) is dropped: its page held only Save / Load
  (now direct) and PS2 options (moved into ours or hidden).
- Game Options page (0x1B04E0, title SELECT menu): New Game... / Load Game... / [Save Game...] / Options (ours) /
  Quit to Desktop (ours). Hidden: Speakers 0x4C, Music 0x67, Effects 0x68, Invert Binocucom 0x51, Invert Turret
  0x53, Vibrations 0x48 (+ USB 0x50, Center Screen 0x3F already).
- Game option values moved into our menu, two-way through the game's own getter f_001AF9C8(item) (writes the
  value's text id into item word 0) and setter f_001AFB88(0x3D9ED0, item): Vibrations 0x48 on / 0x49 off, Invert
  Binocucom 0x51 off / 0x52 on, Invert Turret 0x53 off / 0x54 on, Speakers 0x4C mono / 0x4D stereo / 0x4E surround.
- Quit: one overlay dialog (menu.c) "Quit to desktop? Unsaved progress since the last save will be lost." from the
  pause entry, the title SELECT menu entry, Options (every page footer + row), the window close button / Alt+F4
  (twice = at once). Shutdown: wait for the game's card worker (0x2F633C+4 == -1) and mc_quiesce, motors off +
  light bar reset, window state saved, exit code 0.
- Resolution: [display] mode = windowed|borderless|fullscreen (old [window] fullscreen migrated);
  [display] resolution = desktop|WxH (Fullscreen's display mode); windowed Resolution = window size.
  [graphics] render_scale = auto|original|50..200 (% of the output height)|2x..8x (exact) replaces
  internal_scale + auto_max (migrated). N(P, out_h) = ceil(P/100*out_h/448 - 0.1) clamped 1..8; Auto = governor up
  to N(100%). Presets: Original = original, Performance = 50%, Balanced = auto, Quality = 100% + thin.
- Apply / Revert: display mode, resolution, monitor, renderer are staged (row shows "Apply to use"); Apply row +
  footer button; display changes then ask "Keep these display settings? Reverting in 15 s".

## Done
- (nothing committed yet)

## Next
0. (done above: 1-4)
1. settings.c/.h: render_scale, display.mode, display.resolution, [game] options, legacy keys + file migration;
   presets; tests/unit/menu + window tests updated.
2. gfx.c: render_scale -> scale.
3. menu.c/menu_draw.c: rows, staging, Apply, keep/revert dialog, quit dialog, title "Options".
4. menu_game.c + patches.py: pause list, options page, quit entry, game option sync; window.c quit + close
   button + display modes + WM_DISPLAYCHANGE; display_mode.c resolution.
5. In-game headless runs + screenshots (notes/pc-menu/), docs, gates.

## Findings / traps
- Text ids: 0x39 Keep Playing, 0x46 Quit Game (shown "Save and Quit"), 0x42 Exit to Episode Menu, 0x6B Abandon
  Job, 0x2C Options, 0x43 Controls..., 0x6A Job Help, 0x3C New Game..., 0x3D Load Game..., 0x3E Save Game...,
  0xA/0xB "Yes, Quit"/"No, Don't Quit".
- Run script: build\polish_run\run.ps1 (not committed; card1 = a save). PS2_DUMP_FRAMES without PS2_DUMP_EVERY
  dumps every 60 frames at 9.6 MB each: the script sets DUMP_EVERY huge unless asked.
- Scratch helpers: scratchpad textbank.py (RAM dump -> texts), tables.py, jt.py.

