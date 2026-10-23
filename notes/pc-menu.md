# The in-game Options menu ("PC Settings")

The PC port's own settings screen, as a PC release has one: pause, choose **PC Settings**, change graphics,
display, audio and controls with the keyboard, the mouse or a controller; everything applies while the game runs
(with the game's paused picture behind the menu) and is saved to sly2.ini at once (notes/settings.md).
Contact sheet: `notes/pc-menu/contact_sheet.png`; screenshots `build\screenshots\pc_menu_*.png`.

## How it is reached

- **The game's own pause menu** ("WE'LL BE RIGHT BACK!") has a new entry **PC Settings** after Options, drawn by the
  game in its own font: Keep Playing / Save and Quit / [Exit to Episode Menu] / Options / **PC Settings** /
  Controls... / [Job Help]. Cross / Enter on it opens the menu. Instruction patches (recomp/patches.py, SHA-1
  guarded like the widescreen ones; runtime/menu_game.c):
  - `pcmenu_list` (f_001B0290 before its `jal f_001AF4D8` at 0x1B0490): the list the game filtered into 0x2F65D0
    (full at 7) is copied to unused EE RAM 0xFF000 with our 32-byte entry {text id 0x10000, no callbacks, flags 2 =
    stay open} after Options (text 0x2C); `$a1`/`$a2` get the copy.
  - `pcmenu_text` (f_0013E368 entry, text id -> string): id 0x10000 returns "PC Settings" (EE 0xFF200).
  - `pcmenu_select` (f_001AEEE0 before 0x1AF154, the Cross path, `$s0` = the entry): ours opens the menu; the game's
    pause menu stays open, idle, underneath.
- **Esc** (keyboard) and the **PS button** (DualSense / DualShock 4) open it from anywhere: runtime/input.c reports
  INPUT_NAV_MENU, the menu asks the game to pause exactly as START does (patch `pcmenu_start`, f_001D9648 before
  0x1D96EC: `$v0` = "START pressed" where the HUD manager opens the pause menu) and opens once the pause menu is idle;
  where the game cannot pause (title, movies, loading) it opens after 0.5 s over whatever runs. Xbox pads have no
  readable Guide button: START, then the entry.
- **Closing** (Esc / Circle / B on a main page, Options / PS button anywhere) returns to the game's pause menu;
  Keep Playing (or START) resumes. While the menu is open the game gets a released pad and no keys
  (`input_set_menu_active`); a button still held when it closes stays hidden from the game until released.
- Pause signal (for the Esc path): page depth `*(s32*)0x3D9EF4 >= 1` and page 1's entry list `*(u32*)0x3D9EFC ==
  0xFF000` (ours), state `*(s32*)0x3DA0B8 == 7` = idle. The game itself is frozen while paused (its clock scale
  0x2DDED8 = 0) and keeps drawing the pause screen every frame, so the menu sits on a live picture: a changed
  internal resolution, outline or aspect shows behind it at once.

## How it is drawn

- runtime/menu_draw.c draws the whole menu on the CPU into a premultiplied BGRA bitmap **at the output's size**
  (the window's client area / back buffer: 2560 x 1440 on the owner's monitor, any size elsewhere): anti-aliased
  rounded panels, bars, triangles, rings from signed distances; text with the Windows font **Segoe UI** (title Segoe
  UI Black) through **GDI** (CLEARTYPE_NATURAL_QUALITY drawn white on black into a scratch DIB and used as coverage:
  natural widths, crisp at every size). Sizes follow the output's height (1 unit = 1/1080 of it, limited by the width
  for narrow windows), so it is the same at any DPI, 16:9 or 4:3, windowed or fullscreen. Redrawn only when the
  menu or a setting changes (menu_version: menu state serial, settings_serial, size).
- runtime/menu_overlay.c puts it on the screen for every presenter: **GPU renderer** - a pass in gpu_present.c
  `present_fit` right before Present (also the headless offscreen target; software pictures shown through the GPU
  renderer after a live switch get it too); **software renderer** - window.c `d3d_render` before Present (its own
  Direct3D 11 presenter) and `gdi_render` (GDI fallback, AlphaBlend). The D3D11 pass uploads the bitmap into a
  dynamic texture when it changed, draws one triangle with premultiplied blending and point sampling (one texel per
  pixel), and saves / restores every pipeline state it touches (the GPU renderer's state is unchanged afterwards).
  Its two shaders are compiled once with the system's d3dcompiler_47 the first time the menu is shown (on failure
  the menu is not drawn and the log says why).
- The frame rate counter (`show_fps`) is a second small layer in the picture's top left corner (also fullscreen).
- Look: a dark navy translucent panel with a gold frame (Sly 2's night blue and gold), the game's paused picture
  dimmed behind; tabs with L1 / R1 (LB / RB, Q / E) hints; selected row in blue with a gold bar; values with gold
  arrows, sliders with a gold fill; a description box for the selected row with notes in gold ("GPU renderer only",
  "Applies at the next start"); button hints for the device used last (keyboard caps, PlayStation symbols, Xbox
  letters).

## Pages and options (live = applies while the game runs)

| page | option | values | key | live |
|---|---|---|---|---|
| Graphics | Preset | Original / Performance / Balanced / Quality (Custom shown when the options match none) | graphics.preset | yes |
| | Renderer | GPU (Direct3D 11) / Software (CPU) | graphics.renderer | yes when the game started on the GPU renderer (gpu_hw_request_switch, both ways); else at the next start |
| | Internal resolution | Auto - now Nx (W x H) / 1x - original (512 x 448) / 2x .. 8x (W x H) / Native - Nx (W x H) | graphics.internal_scale | yes (GPU) |
| | Auto: highest resolution (shown with Auto) | 1x .. 8x | graphics.auto_max | yes (GPU) |
| | Scaling filter | Automatic / Lanczos / Catmull-Rom / Box / Bilinear / Nearest | graphics.scaler | yes (GPU) |
| | Outlines | Original (PS2) / Thin (high resolution) | graphics.outlines | yes (GPU) |
| | Look | Modern / Faithful (PS2 on a TV) | graphics.filters | yes |
| | Motion blur, TV flicker filter, Interlacing | Auto (the Look's value) / Off / On | graphics.motion_blur / flicker_filter / interlacing | yes (the last two GPU) |
| Display | Display mode | Windowed / Borderless fullscreen / Exclusive fullscreen | window.fullscreen | yes (exclusive: pc-pacing; borderless until then) |
| | Window size | 960 x 540 .. what fits the monitor, at the aspect | (window placement) | yes, windowed |
| | Monitor | Automatic / 1: 2560 x 1440, 165 Hz (main) / ... | display.monitor | yes (with 2+ monitors) |
| | Aspect ratio | 4:3 (original) / 16:10 / 16:9 / 21:9 / 32:9 (the last two when the display is that wide) / the display's own | display.aspect | yes (next game frame) |
| | HUD layout | At the screen's edges / Centered (4:3 area) | display.hud | yes (wide aspects) |
| | Frame pacing | VSync / Variable refresh (G-Sync / FreeSync) / Display-synced | graphics.pacing | next start (until pc-pacing's pacing_apply_mode) |
| | Show frame rate | Off / On | window.show_fps | yes |
| Audio | Master volume | 0-100 % (steps of 5) | audio.master | yes (mixer output) |
| | Music volume, Effects volume | 0-100 % | audio.music / audio.effects | yes (the game's own volumes, next frame) |
| | Sound in the background | On / Off | audio.background | yes |
| Controls | Vibration, Vibration strength (test rumble on change), Invert camera horizontally / vertically, Left / Right stick dead zone, Light bar | | [controls] (input.c) | yes |
| | Keyboard controls / Controller buttons | sub-pages: every PS2 button (keyboard also the stick directions), two bindings each; Confirm, then press | [controls] key_* / pad_* | yes |
| every page | Reset to defaults | asks first | | yes |

"Voices": not offered. The game has no voice volume of its own: its Options page sets its sound manager's
categories 0 and 3 together from the Effects slider (category 1 is music, 2 is set elsewhere); category 3 may be the
dialogue, but that was not verified, and the native mixer (sd.c) only sees anonymous SPU2 voices. A separate Voices
slider would mean driving category 3 alone - possible with the same call if a test confirms what it holds.

## Presets

notes/settings.md "Presets": Original = 1x, Faithful, PS2 outlines, 4:3; Performance = auto up to 2x, Modern;
Balanced (default) = auto up to 3x, Modern; Quality = native (at least the window's height: 4x at 1440p), thin
outlines, Modern. Choosing a preset sets its options; changing one of them (menu or F-keys) makes it Custom.

## Interfaces

notes/pc-experience.md "Interfaces: menu" (what the menu uses from pc-input, what it needs from pc-pacing -
`pacing_apply_mode`, `pacing_exclusive_fullscreen` with weak stand-ins - and the settings model for any stream).

## Code

| file | what |
|---|---|
| runtime/settings.c/.h | the option table, change / persist / appliers, presets |
| runtime/menu.c/.h | the model: pages, rows, values, navigation, mouse, bindings capture, layout (no window; unit-tested) |
| runtime/menu_draw.c/.h | the picture (CPU) |
| runtime/menu_overlay.c/.h | on the screen: D3D11 pass, GDI, the frame rate layer, screenshots |
| runtime/menu_game.c/.h | the game: pause entry patches, Esc / PS -> pause -> menu, volumes into the game, renderer / pacing appliers, the menu's host |
| runtime/menu_ext.h, menu_stubs.c | the pacing interface and its weak stand-ins |
| recomp/patches.py | pcmenu_list, pcmenu_text, pcmenu_select, pcmenu_start, pcmenu_frame |

## Testing

- `tests\unit\menu\run.ps1` (216 checks, no window): the option table (every default valid and round-tripping), presets
  (apply / detect, file precedence, old files), persistence (comments, unknown keys and [controls] kept; env-locked
  options not written; F3 through the table), the menu (Esc opens through the host, preset / scale / aspect / mode /
  window size / volumes / controls changes, the reset dialog, wrap-around, tabs, mouse clicks on tabs / arrows /
  slider bars, hover, binding capture into the second column with a key taken from where it was, back / close), and
  the drawing at 2560x1440, 1280x720, 1024x768 and 800x600. `-Shots` writes every page as PNG and the contact sheet.
- In the game, headless with scripted input (PS2_INPUT tokens `menu` = Esc, `mup mdown mleft mright mok mback mtabn
  mtabp`, `menushot` = the next presented picture with the menu as PNG into PS2_MENU_SHOTS; `PS2_GS_PRESENT_SIZE=WxH`
  sizes the headless present target): build\pcmenu_run\run.ps1 (not committed) - pause -> PC Settings, Esc during
  play, preset / aspect / volumes changed and persisted across a restart, live renderer switch GPU <-> software, a
  short visible run on the software renderer (own Direct3D 11 presenter) and with PS2_WINDOW_GDI=1.
