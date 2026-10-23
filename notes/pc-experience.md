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

## Interfaces: menu (pc-menu; `runtime/settings.h`, `runtime/menu_ext.h`, details in notes/pc-menu.md)

**What the menu uses from input** (as above): `input_set_menu_active`, `input_nav_flush`, `input_nav_poll`
(every window-thread turn, `menu_tick`), `input_capture_begin / _poll / _cancel`, `input_config_get / _set /
_defaults`, `input_action_label`, `input_key_label`, `input_button_label`, `input_devices`, `input_test_rumble`.
INPUT_NAV_MENU while closed asks the game to pause (as START would, patch `pcmenu_start`) and then opens the menu.

**What the menu needs from pacing** (`runtime/menu_ext.h`; weak stand-ins in `runtime/menu_stubs.c` that pc-pacing's
strong definitions replace at link time - nothing in the menu changes at the merge):

- `int pacing_apply_mode(int pacing)`: `graphics.pacing` changed to SETTINGS_PACING_VSYNC / VRR / DISPLAY (already
  stored in `ps2_settings.pacing` and sly2.ini): return 1 if it is in effect now, 0 if it applies at the next start.
  `pacing_apply_mode(-1)` only asks whether changes apply live (the menu then hides "Applies at the next start").
  Stand-in: 0.
- `int pacing_exclusive_fullscreen(int on)`: exclusive fullscreen on the GPU renderer's swap chain on / off (the
  window is already borderless fullscreen on the right monitor when it is called with 1): 1 if done; 0 = not
  supported (the window stays borderless and the menu says so). Stand-in: 0.

**For any stream: the settings model** (`runtime/settings.h`). A new option is one row of `opts[]` in settings.c
(section, key, type, range / names, default, flags, applier). `settings_change(i, v, persist)` (or
`settings_change_key(section, key, v, persist)`) validates, stores into `ps2_settings`, writes sly2.ini keeping
comments, re-derives the graphics preset and calls the applier registered with `settings_on_apply(SET_APPLY_x, fn)`
(SET_APPLY_PACING is pacing's to take over from menu_game.c if it wants: register after `menu_game_init`).
`settings_serial` / `settings_touch()`: the menu redraws. Internal resolution: `internal_scale` 1..8, 0 = auto
(up to `auto_max`), -1 = native (`gfx_native_scale(client height)`); the GPU renderer gets them through gfx.c as
before (`gpu_hw_set_graphics`, `gpu_hw_set_auto_scale(auto_max)`).

## Interfaces: pacing and resolution (pc-pacing; runtime/gpu/gpu_pace.c, gpu_present.c, gpu_hw.c, runtime/pacing_menu.c)

**Game speed never depends on the GPU.** Root cause of "the game runs slower at a high resolution": a frame the GPU
cannot draw in real time stays in the render queue; when the queue is full the GS thread, and behind it the EE, waits
at the next VBlank (gpu_exec.c `gpu_submit_sink`), so the game ran at the GPU's speed (measured, ep1 hub headless: 6x
-> 42.8 VBlanks/s, 29 game frames/s). Frames cannot be dropped (each frame's drawing builds GS memory that later frames
read), so the internal scale **governor** (gpu_pace.c `pace_gov`) lowers the scale actually drawn as soon as the GPU
cannot keep up - for auto and for a fixed scale or native alike (the player's scale is the maximum) - and raises it
again when it fits. Load = GPU ms per picture / 16.68 ms. Auto: down above 90% for 3 s (to the scale predicted at
<= 80%), up when the next scale is predicted at <= 75%, back-off 120 s doubling. Fixed / native: down above 100% for
2 s (to 85%), overload rule at once (12 pictures over 2.5 frame times or 24 over 1.4), or within the second when the
game waits >= 50 ms with the GPU >= 90% busy; up again at <= 80% predicted. Evidence beats timestamps (another program
on the GPU): >= 10% skipped presents or >= 50 ms of game waits in a second count as 105% when the GPU is >= 70% busy. No
judging for 60 pictures after a light second (GPU clocks rising). 1x still too slow for 10 s: the software renderer
takes over. Tests: `PS2_GS_SCALE` / `PS2_GS_GOVERNOR=0` keep a scale exactly.

**Presentation.** Picture v is presented at its VBlank's due time + D (D = 95th percentile of when pictures are ready +
1.5 ms) - an even 16.68 ms cadence that a VRR display shows as such - or as soon as it is drawn (immediate). The display's
own record of the presents (DXGI frame statistics) tells VRR from a fixed refresh (gpu_pace.c `pace_disp`); the log
says what it found. `graphics.pacing = auto` (default): paced until known; VRR -> paced; a fixed refresh -> immediate
(pacing cannot beat the refresh grid and only adds latency); 60 / 120 / 240 Hz -> the display's VBlanks drive the game
(every picture exactly k refreshes). `graphics.vsync` on / off (tearing) / adaptive. Exclusive fullscreen and
`display.fullscreen_refresh = auto` switch a 144 / 165 Hz display without VRR to 120 Hz while the game is fullscreen and
in front (the only way to remove the 2-or-3-refresh judder there). The present waits at most 34 ms for the swap chain
(was 100 ms).

**Functions for the menu** (all any thread, live):
- `pacing_apply_mode(int pacing)` (menu_ext.h, runtime/pacing_menu.c): -1 -> 1 (pacing and vsync apply while the game
  runs); else applies `ps2_settings.pacing` / `vsync` to the presenter and the display clock, returns 1.
- `pacing_exclusive_fullscreen(int on)`: the window stays a popup over the monitor (DXGI flip model: independent flip,
  exclusive's latency) and the game owns the display mode while in front (display_mode.c: 120 Hz on a 165 Hz display
  without VRR; the desktop's rate when it is a multiple of 59.94 Hz already). Returns 1.
- `gfx_apply_pacing()`: pacing / vsync -> renderer (gfx_apply calls it). `gfx_scale_status(gfx_scale_info*)`: requested
  vs drawn scale, `limited` ("4x requested, drawing 3x: GPU limit"), load, total game wait, picture size.
- Options: `graphics.pacing` auto | paced | immediate | display | vrr (old), `graphics.vsync` on | off | adaptive,
  `display.fullscreen_refresh` desktop | auto | Hz (settings.c table rows; the menu's Display page has Frame pacing and
  VSync rows). Diagnostics: `PS2_PACE_LOG=1|2|3`, `PS2_GS_HW_STATS=1` ("pacing" line per second), scripts/pace_run.ps1,
  pace_report.ps1, pace_plot.py, pace_cost.ps1.

**Measured on the owner's PC** (RTX 4060 Ti, Dell S2722DGM 2560 x 1440 @ 165 Hz; the display runs a FIXED 165 Hz refresh
for the game: variable refresh is not active - G-Sync Compatible / FreeSync is off in the driver or the monitor):

| run (50 s from boot, intro + attract) | VBlanks/s (min) | shown intervals | VBlank -> shown |
|---|---|---|---|
| before: 6x headless (old code) | 42.8 (29.9) | - | - |
| borderless, auto, 3x | 59.92 (59.9) | 12.1 / 18.2 ms grid, std 3.4 ms | median 14.7 ms |
| windowed maximised, auto | 59.92 (59.9) | std 3.3 ms | median 14.8 ms |
| borderless, paced (first version) | 59.71 | std 6.4 ms | median 51.8 ms |
| borderless, 7x fixed (governed 7 -> 5 -> 4 -> 3) | 59.26 (46.1 while stepping down) | std 11.6 | median 14.5 |
| borderless, native (4x -> 3x in the heaviest scenes) | 59.61 | std 10.6 | median 13.2 |
| headless 1440p, ep1 / ep3 / m7, fixed 1x..4x | 59.6-59.94 at every scale | - | - |

notes/pc-pacing-frametimes.png: the shown intervals, one dot per picture. On the fixed 165 Hz refresh 59.94 Hz pictures
can only last 2 or 3 refreshes (12.1 / 18.2 ms): that pattern is the owner's "shaking"; with VRR on (or 120 Hz) it goes.

**GPU cost per scale at 1440p 16:9** (gs_replay back to back = full clocks; GPU ms per picture, 16.68 ms budget; the
present's resample to 2560 x 1440 adds 0.15-0.25 ms; 165 Hz does not add presents - one per picture):

| scale (picture) | ep1 hub | ep3 hub | ep5 street | ep7 boss | ep8 boss | load |
|---|---|---|---|---|---|---|
| 1x (512 x 448) | 1.6 | 1.8 | 2.0 | 2.1 | 2.6 | 10-16% |
| 2x (1536 x 896) | 5.3 | 5.4 | 5.6 | 6.5 | 4.9 | 29-39% |
| 3x (2048 x 1344) | 9.4 | 8.9 | 9.2 | 11.3 | 7.1 | 43-68% |
| 4x (2560 x 1792, native 1440p) | 17.6 | 16.3 | 17.6 | 18.7 | 13.3 | 80-112% |
| 5x / 6x / 8x | 25.1 / 34.2 / 62.6 | 23.0 / 30.9 / 55.8 | - | 29.9 / 40.7 / 76.0 | - | 140-460% |

**Presets (validated, no change to the menu's table)**: Original 1x (~2 ms); Performance auto up to 2x (~5.5 ms, a third
of the budget: fits weaker GPUs too, lowered to 1x where needed); Balanced auto up to 3x (default; 43-68% here, stays at
3x); Quality native (4x at 1440p: fits ep3 / ep8, drawn at 3x in the heaviest hubs and the ep7 boss on this GPU - the
governor decides per scene, the game keeps its speed). Every preset adapts to the PC through the governor's measurement
(any GPU vendor); the pacing adapts to the display (60 / 120 / 144 / 165 / 240 Hz, VRR or not) through DXGI.