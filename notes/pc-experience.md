# PC experience (2026-10-22): owner requirements and the shared plan

The owner tried the packaged build (`dist\Sly2\sly2.exe`, GPU renderer default) and rejected the
"PS2-style" experience. Their words: settings via hotkeys/ini are "the old settings from the PS2";
"when you are making a real native PC version of a game you need to be able to finally have a menu:
you pause, go to options and you get all the modern menu choices"; "when you up the resolution the
game runs like shit and the screen is shaking"; "there is no control support" (they use a **PS5
controller / DualSense**); "in the starting menu it talks about PS2 memory card - THIS IS A PC
VERSION"; "there need to be options for the graphics in general, something like original and new,
performance etc".

Their machine: RTX 4060 Ti, i7-14700F, one monitor **2560x1440 @ 165 Hz**. Their session log shows
`internal_scale auto` dropping 3x -> 2x after 90 s (GPU 89% busy at 3x, windowed or fullscreen at
1440p), then F2 pressed up to 7x.

## Workstreams (one agent each, in their own worktree from main 1f9bc28)

| stream | worktree / branch | owns |
|---|---|---|
| pacing | `pc-pacing` / next/pc-pacing | game speed independent of rendering, frame pacing on 60/120/144/165 Hz and VRR, scale changes without hitches, GPU cost at 1440p output, the auto-scale behaviour |
| menu | `pc-menu` / next/pc-menu | the in-game Options menu (UI, its renderer, navigation), the settings model and persistence, graphics presets |
| input | `pc-input` / next/pc-input | DualSense / DualShock 4 (USB + Bluetooth HID), XInput, keyboard; hot-plug; rumble; control remapping data; the menu input API |
| text | `pc-text` / next/pc-text | removing PS2 wording and flows: memory-card prompts and messages, other console-only text |

## Shared decisions and interfaces

- **Settings model** (menu owns `runtime/settings.c/.h`; others add keys through it): every option has
  an ini key in `sly2.ini` (kept for power users), a typed value, a default, whether it applies live
  or at restart, and a change callback. Existing keys stay valid. New sections as needed:
  `[display]` (mode window|borderless|fullscreen, resolution/monitor, aspect, hud, vsync/pacing,
  frame cap), `[graphics]` (preset original|performance|balanced|quality|custom, internal resolution,
  outlines, filters + toggles, scaler), `[audio]` (master, music, effects, voice volumes),
  `[controls]` (bindings, vibration, stick dead zones, invert camera axes).
- **Graphics presets** (menu defines them, pacing validates the cost): **Original** = the PS2 look
  (1x internal resolution, Faithful filters, PS2 outlines, 4:3, software or GPU at 1x); **Performance**
  = lower internal resolution (target 1x-2x), Modern look; **Balanced** = auto resolution capped so the
  GPU stays well under the frame time; **Quality** = native output resolution or higher (e.g. 1440p
  internal on a 1440p display), Modern look; **Custom** = whatever the player changed. Resolution is
  shown as real pixel sizes ("1536 x 1344 (3x)", "Native 2560 x 1440"), not just multipliers.
- **Game speed never depends on rendering**: the EE / game clock runs at the PS2's 59.94 Hz VBlank
  regardless of GPU load; when the GPU cannot keep up, rendered frames are dropped or presented late,
  the game does not slow down. (pacing)
- **Menu entry point**: an "Options" (or "PC Settings") entry in the game's own pause menu if it can be
  added cleanly through recompiler patches/hooks; otherwise a native overlay opened from the pause
  menu with a documented button (and Esc / the controller's PS/Options button). The menu is drawn by
  the runtime on top of the game picture (GPU and software renderers), pauses the game while open,
  and is fully usable with keyboard, mouse and DualSense/Xbox controllers. Text is drawn with a Windows
  font through DirectWrite/GDI (no copyrighted assets shipped).
- **Input API for the menu** (input owns `runtime/input*.c`, `runtime/pad.c`): a function the menu
  calls to read navigation events (up/down/left/right/confirm/back/tab) from any device, and a way to
  suppress game input while the menu is open.
- **Controller type**: DualSense is a PlayStation controller, so the game's own button prompts
  (Cross/Circle/Square/Triangle, START, SELECT) match it. On Xbox controllers the prompts stay
  PlayStation symbols for now (prompt glyph replacement is a later task).
- **PS2 wording** (text): no memory-card prompts at boot (save data is a file; create it silently), no
  "memory card (PS2)" / "MEMORY CARD slot 1" / "do not remove the memory card" text; PC wording such as
  "Saving... do not turn off the game" or "Saving...". Keep the game's own save/load flow and menus.
- **Interfaces: pacing and resolution** (pacing stream, see the section at the end).
- Every stream keeps the existing tests green, adds its own, keeps `notes/wip/<stream>.md`
  checkpoints, follows disk hygiene (no frame-dump piles), never modifies the main checkout, and
  tests as a player: the packaged build (`scripts\package.py` -> `dist\Sly2\sly2.exe`) on the real
  1440p/165 Hz monitor, windowed and fullscreen.
