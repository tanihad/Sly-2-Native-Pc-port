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
- Every stream keeps the existing tests green, adds its own, keeps `notes/wip/<stream>.md`
  checkpoints, follows disk hygiene (no frame-dump piles), never modifies the main checkout, and
  tests as a player: the packaged build (`scripts\package.py` -> `dist\Sly2\sly2.exe`) on the real
  1440p/165 Hz monitor, windowed and fullscreen.

## Interfaces: input (pc-input; `runtime/input.h`, details in notes/controls.md)

Every function is thread-safe (callable from the window thread, the emulation thread or a menu
thread). Only Windows' own DLLs are used, loaded at run time; no controller = keyboard only.

**Opening / closing the menu**

- `input_set_menu_active(1)` when the menu opens, `(0)` when it closes. While active the game gets a
  released controller and no keys, and the motors stop. Buttons/keys still held when it closes stay
  hidden from the game until released (the Cross that confirmed "Close" does not make Sly jump).
- Call `input_nav_flush()` right after opening (drops stale events, syncs the controllers' held state).
- Open triggers: `input_nav_poll` returns `INPUT_NAV_MENU` also while the menu is closed, for **Esc**
  and the **PS button**; while it is open, Options/Start also give MENU (close). Start alone with the
  menu closed stays the game's (its own pause menu). Xbox pads have no readable Guide button: the
  pause-menu entry (or Esc) is their way in. Note: window.c's Esc still leaves fullscreen when
  fullscreen; whoever owns that key should drop that behaviour if Esc opens the menu.

**Navigation** - call `input_nav_poll(&ev)` in a loop every menu frame until it returns 0:

| ev.nav | from |
|---|---|
| UP / DOWN / LEFT / RIGHT | d-pad or left stick (any controller), arrows or WASD, mouse wheel (UP/DOWN); held directions repeat after 400 ms every 110 ms (`ev.repeat = 1`; keyboard repeats at the Windows rate) |
| CONFIRM | Cross / A, Enter, Space |
| BACK | Circle / B, Esc, Backspace, right mouse button |
| TAB_NEXT / TAB_PREV | R1 / L1 (RB / LB), Tab / Shift+Tab, E / Q, PageDown / PageUp |
| MENU | PS button, Options / Start (menu open), Esc (menu closed) |
| CLICK | left mouse button, `ev.x, ev.y` in client pixels of the game window |
| POINTER | mouse moved (coalesced), `ev.x, ev.y` (for hover highlight) |

`ev.source` is INPUT_SRC_KEYBOARD / MOUSE / CONTROLLER, `ev.device_id` the controller. While the menu
is active, `input_window_message` (called first by window.c's wndproc) consumes keys (except F11),
clicks and the wheel, so the window does not act on them.

**Remapping**

- `input_config_get(&c)`: the current `input_config` (bindings `c.key[action][2]` = virtual keys,
  `c.pad[action][2]` = `INPUT_BTN_*`, `vibration`, `vibration_strength` 0-100, `invert_camera_x/y`,
  `deadzone_left/right` 0-50 %, `lightbar_on`, `lightbar[3]`). `input_config_defaults(&c)` = "reset".
- `input_config_set(&c, save)`: applies at once (the light bar changes immediately); `save = 1` writes
  every `[controls]` key to sly2.ini (via settings.c `settings_set`).
- Capture: `input_capture_begin(INPUT_CAPTURE_KEYBOARD or INPUT_CAPTURE_CONTROLLER)`, then each frame
  `int r = input_capture_poll(&code)`: PENDING, KEY (code = virtual key; mouse buttons too),
  BUTTON (code = INPUT_BTN_*), CANCELLED (Esc / PS button). No nav events while capturing; the
  captured key/button produces no CONFIRM afterwards. Unbindable keys (F2-F11, Esc, Alt) are ignored.
- Labels for the UI: `input_action_label(act)` ("Cross", "Left stick up", "Right stick up (camera)";
  actions 0-15 the DualShock 2 buttons, 16-23 the stick directions, keyboard only),
  `input_key_label(vk)` ("Left Ctrl", "Numpad 2"), `input_button_label(btn, xbox_style)`
  ("Create" / "View"). Show controller bindings for INPUT_ACT_PAD_COUNT actions only.
- Suggested Controls page: Vibration on/off + strength + "Test" (`input_test_rumble(id, 255, 255,
  500)` on each `input_devices` entry), Invert camera X / Y, Left / Right stick dead zone, Light bar
  on/off, a "Keyboard" and a "Controller" binding list (two columns each), "Reset to defaults".

**Status for messages**: `input_devices(list, max)` (name, kind, bus, port, battery, live state);
`input_poll_notice(&n)` returns connect / disconnect notices (`n.connected`, `n.name`, `n.port`: 0 =
the controller that drove the game) for an on-screen message ("Controller disconnected").
