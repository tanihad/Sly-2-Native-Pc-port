# The in-game Options menu, the pause menu and Quit to desktop

The PC port's settings screen, as a PC release has one: pause, choose **Options**, change graphics, display, audio and
controls with the keyboard, the mouse or a controller; most options apply while the game runs (with the game's paused
picture behind the menu) and are saved to sly2.ini at once (notes/settings.md); display mode, resolution, monitor and
renderer are staged and applied with **Apply changes**, then confirmed ("Keep these display settings? Reverting in 15 s").
**Quit to Desktop** is in the pause menu, in Options and on the title screen, and the window's close button asks the
same question.

Contact sheets: `notes/pc-menu/contact_sheet.png` (every page and dialog at 2560 x 1440 and the Graphics page at other
sizes, from the unit test), `notes/pc-menu/ingame_sheet.png` (headless game runs: the pause menu's entries, the quit
question, the title screen's menu, Options pages, Keep / Revert, the game options, Save / Load / Save and Quit / Exit to
Episode Menu still working).

## One menu (pc-polish, 2026-10-23): how the game's menus and ours fit together

The owner: "there is no way to quit the game, also what's the use of the old pause menu being there?". The game's pause
menu keeps what only the game can do (its save system, job help, episode menu); everything that is a *setting* lives in
one place, our Options; nothing is duplicated and no PS2-only item is left.

**Pause menu** ("WE'LL BE RIGHT BACK!", the game's own wheel, its own font):
Keep Playing / Save and Quit to Title / [Exit to Episode Menu] / [Abandon Job] / **Save Game...** / **Load Game...** /
**Options** / [Controls...] / [Job Help] / **Quit to Desktop** ([..]: shown where the game shows them).
- The game's own "Options" entry (text 0x2C) is gone. Its page held Save Game... / Load Game... (now directly in the
  pause menu: the game's own items, copied from its Options table 0x2BFEC8, same callbacks and memory card pages), PS2
  hardware items (USB Headset, Center Screen...: hidden since pc-text) and options that are now in our menu (below).
- "Save and Quit" (the game saves and goes back to the title screen) reads **"Save and Quit to Title"** next to
  "Quit to Desktop" (text id 0x47 answered by the text lookup patch: the bank's text cannot grow in place).
- **Options** opens our menu; **Quit to Desktop** asks "Quit to desktop? Unsaved progress since the last save will be
  lost." (Quit / Cancel; Cancel returns to the pause menu).

**Title screen** (SELECT: the game's Options page, top level): New Game... / Load Game... / **Options** / **Quit to
Desktop**. Its Speakers / volume / inversion / Vibrations items are hidden (in our menu).

**The game's options in our menu, two-way** (menu_game.c): Music / Effects Volume (Audio, as before), **Speakers**
(Audio: Stereo / Mono / Surround - the game's own setting, default Stereo on PC; the disc's default is Surround, a Pro
Logic II matrix), **Vibration** (Controls: our Vibration row is both the controller's and the game's Vibrations),
**Invert Binocucom aiming**, **Invert turret aiming** (Controls; [game] invert_binocucom / invert_turret). They are read
with the game's own getter f_001AF9C8(item) (it writes the value's text id into the item) and set with its setter
f_001AFB88(menu object 0x3D9ED0, item) - the same code its Options page ran - twice a second and at once after a change;
a loaded save brings its own values and ours are put back (as for the volumes). Value text ids: Vibrations On 0x48 / Off
0x49, Speakers Mono 0x4C / Stereo 0x4D / Surround 0x4E, Invert Binocucom Off 0x51 / On 0x52, Turret Off 0x53 / On 0x54.
Our camera inversion rows (right stick) stay: the game's two inversions are different (aiming modes).

## Quit to desktop

- Asked from: the pause menu's entry, the title menu's entry, "Quit to desktop" at the end of every Options page, the
  window's close button and Alt+F4 (WM_CLOSE). With the menu closed the question opens alone over the game (no panel;
  the game is paused behind it where it can be, as for Esc); from Options it opens over the panel. A second close while
  the question shows quits at once (a game that stopped answering can always be closed). Logging off / shutting down
  Windows (WM_ENDSESSION) quits without asking.
- Quit (menu_game.c `menu_game_quit`, own thread): waits for the game's card worker (0x2F633C +4 == -1, at most 5 s: a
  save in progress finishes), then the window thread's `close_window`: the window's placement and display mode saved
  ([display] mode, [window] x/y/...; every setting is already in sly2.ini, written when it changed), the controllers'
  motors stopped and their light bar and player LEDs turned dark (input_hid.c, at the HID thread's stop), the desktop's
  display mode restored, `launch_exit(0)` (mc_quiesce: the memory card files flushed), exit code 0. Headless: the same
  without the window.
- Tests: headless runs `mquit` (the close button token) twice and the pause entry + `mok`: exit code 0, `[menu] quit to
  desktop`, `[launch] exit (0)`; a save started a few frames before the quit is on disk.

## How it is reached

- **The game's pause menu**: Options / Quit to Desktop as above. Instruction patches (recomp/patches.py, SHA-1 guarded;
  runtime/menu_game.c):
  - `pcmenu_list` (f_001B0290 before its `jal f_001AF4D8` at 0x1B0490): the list the game filtered into 0x2F65D0 is
    copied to unused EE RAM 0xFF000: the 0x2C entry replaced by Save Game... (when f_1D9880(0x3DA530) = 0, the same test
    the Options page uses), Load Game... and our "Options" (text id 0x10000, no callbacks, flags 2 = stay open); "Quit to
    Desktop" (0x10001) last. `$a1`/`$a2` get the copy.
  - `pcmenu_options_list` (f_001B04E0 before 0x1B05E4): Options and Quit to Desktop appended to the Options page
    (`$s1` = the end of its list at 0x2F6470).
  - `pc_options_hide` (f_001B04E0 at 0x1B058C): also hides Speakers 0x4C, Music / Effects Volume 0x67 / 0x68, Invert
    Binocucom 0x51, Invert Turret 0x53, Vibrations 0x48 (`pcmenu_option_hidden`).
  - `pcmenu_text` (f_0013E368 entry, text id -> string): 0x10000 "Options" (EE 0xFF200), 0x10001 "Quit to Desktop"
    (0xFF240), 0x47 "Save and Quit to Title" (0xFF280, with the PC wording on).
  - `pcmenu_select` (f_001AEEE0 before 0x1AF154, Cross, `$s0` = the entry): 0x10000 opens the menu, 0x10001 the quit
    question; the game's pause menu stays open, idle, underneath.
- **Esc** (keyboard) and the **PS button** open it from anywhere: as before (`pcmenu_start`: the game pauses as with
  START, the menu opens when the pause menu is idle; where the game cannot pause it opens after 0.5 s).
- **Closing** (Esc / Circle / B on a main page, Options / PS button anywhere) returns to the game's pause menu; with
  staged changes it asks first ("Apply the changed settings?" Apply / Discard).
- Pause signal: page depth `*(s32*)0x3D9EF4 >= 1` and page 1's entry list `*(u32*)0x3D9EFC == 0xFF000`, state
  `*(s32*)0x3DA0B8 == 7` = idle.

## Resolution, made understandable

Before: "Internal resolution" (Auto / 1x..8x / Native) and "Auto: highest resolution" on the Graphics page and "Window
size" on the Display page - the owner: "Internal resolution and actual resolution are confusing". Now, as modern games:

**Display page** - the picture on the screen:
- **Display mode**: Windowed / Borderless (fills the monitor at the desktop's resolution) / Fullscreen (the game sets
  the monitor to the resolution below; the window stays a flip-model popup - exclusive's latency - and the desktop's
  mode comes back when the game goes to the background or ends). [display] mode (old [window] fullscreen = off / on /
  exclusive is read and migrated).
- **Resolution**: windowed = the window's size (client area; a maximized window is restored first); borderless = the
  desktop's, shown read-only; fullscreen = the monitor's modes ("Desktop (2560 x 1440)", then every mode, largest first;
  [display] resolution = desktop | WxH; display_mode.c switches with CDS_FULLSCREEN at the refresh rate
  `display_mode_rate_at` picks: fullscreen_refresh's choice, else the desktop's rate if that mode has it, else the
  fastest not above it).
- Monitor, Aspect ratio, HUD layout, Frame rate, Frame pacing, VSync, Variable refresh (status), Show frame rate.

**Graphics page** - how sharp the game is drawn: **Render scale**: Auto (up to 100%) / Original PS2 (512 x 448) / 50% /
67% / 75% / 100% / 125% / 150% / 200%, each with the size it means on this output ("100% (2560 x 1440)"); the
description says what is really drawn: "Rendering at 2560 x 1792 -> shown at 2560 x 1440". The renderer draws integer
multiples N of the PS2's 512 x 448 (display targets N x 2N samples); a percentage P becomes the smallest N whose 448 N rows
reach P % of the output's rows, with 10 % slack (`settings_scale_for`): N = ceil(P/100 * rows / 448 - 0.1), 1..8.

| rows \ P | 50 | 67 | 75 | 100 | 125 | 150 | 200 |
|---|---|---|---|---|---|---|---|
| 720 | 1 | 1 | 2 | 2 | 2 | 3 | 4 |
| 1080 | 2 | 2 | 2 | 3 | 3 | 4 | 5 |
| 1440 | 2 | 3 | 3 | 4 | 4 | 5 | 7 |
| 2160 | 3 | 4 | 4 | 5 | 6 | 8 | 8 |

("Next higher" so 100 % is never softer than the screen - 1440p at 100 % draws 1792 rows, not 1344; the slack keeps
125 % of 1440 = 1800 rows at 4x = 1792 instead of jumping to 5x.) Auto = the governor's choice up to N(100 %) (3x at
1080p, 4x at 1440p: on the owner's RTX 4060 Ti it settles at 3x in the heaviest hubs, pc-pacing's measurements); every
scale is a maximum the governor may lower while the GPU cannot keep up. An exact multiple (F2, an old file's
internal_scale) shows as "3x (2048 x 1344)". [graphics] render_scale = auto | original | 25%..400% | 2x..8x (old
internal_scale / auto_max read and migrated). Headless runs (no window) assume 1080 output rows.

Presets: **Original** = Original PS2, Faithful, PS2 outlines, 4:3; **Performance** = 50 % (2x at 1080p / 1440p);
**Balanced** (default) = Auto; **Quality** = 100 % + thin outlines; **Custom** = anything else.

## Apply / Revert

- Live (apply as they change, as before): everything except the four below.
- **Staged** (`SET_STAGED` in the settings table; the menu keeps the chosen value, the row says "Choose Apply to use
  it", the header shows an **Apply changes** button and the page an "Apply changes" row): Display mode, Resolution,
  Monitor (a display mode change) and Renderer (a renderer switch; started on the software renderer it needs a restart:
  "Choose Apply - needs a restart of the game", afterwards "Restart the game to use it"). Stepping back to the value in
  effect un-stages it.
- After a display change is applied: "Keep these display settings? Reverting to the previous settings in 15 s." Keep /
  Revert; no answer in 15 s (or Back / Esc / the PS button) restores the previous display mode, resolution, monitor and
  window size.
- Closing the menu with staged changes: "Apply the changed settings?" Apply / Discard.
- Needs a restart: the renderer when the game started on the software renderer; frame pacing only if pc-pacing's
  live switch is missing (the stand-in): the row's note says so.

## Pages and options

| page | option | values | key | applies |
|---|---|---|---|---|
| Graphics | Preset | Original / Performance / Balanced / Quality (Custom when the options match none) | graphics.preset | live |
| | Renderer | GPU (Direct3D 11) / Software (CPU) | graphics.renderer | Apply (live switch when started on the GPU renderer, else next start) |
| | Render scale | Auto / Original PS2 / 50..200 % (W x H) [/ Nx] | graphics.render_scale | live (GPU) |
| | Scaling filter, Outlines, Look, Motion blur, TV flicker filter, Interlacing | as before | graphics.* | live |
| Display | Display mode | Windowed / Borderless / Fullscreen | display.mode | Apply + Keep |
| | Resolution | window sizes / desktop (read-only) / the monitor's modes | [window] size, display.resolution | Apply + Keep |
| | Monitor | Automatic / 1: 2560 x 1440, 165 Hz (main) / ... | display.monitor | Apply + Keep (2+ monitors) |
| | Aspect ratio, HUD layout, Frame rate, Frame pacing, VSync, Variable refresh (status), Show frame rate | as before | | live |
| Audio | Master, Music, Effects volume, **Speakers** (Stereo / Mono / Surround), Sound in the background | | audio.* | live |
| Controls | Vibration (also the game's), strength, Invert camera X / Y, **Invert Binocucom aiming**, **Invert turret aiming**, dead zones, Light bar, Keyboard controls / Controller buttons | | [controls], game.invert_* | live |
| every page | Reset to defaults (asks; keeps display mode / resolution / monitor), **Quit to desktop** (asks) | | | |

"Voices": not offered (the game has no voice volume of its own: its Effects slider sets sound manager categories 0 and 3).

## How it is drawn

As before (runtime/menu_draw.c on the CPU at the output's size, Segoe UI through GDI; runtime/menu_overlay.c puts it on
the screen for every presenter). The title is "OPTIONS". Dialogs (`draw_dialog`): a title, an optional line, two buttons
with the device's hints (Enter / Esc, Cross / Circle, A / B); with `dialog_only` (Quit from the game's menus or the close
button) only the dimmed game and the dialog are drawn.

## Code

| file | what |
|---|---|
| runtime/settings.c/.h | the option table (render_scale, display.mode / resolution, game options, SET_STAGED, SET_LEGACY + migration), presets, `settings_scale_for` |
| runtime/menu.c/.h | the model: pages, rows, staging, Apply / Keep / Revert, dialogs (reset, quit, keep, pending), navigation, mouse, layout |
| runtime/menu_draw.c/.h | the picture (CPU) |
| runtime/menu_overlay.c/.h | on the screen: D3D11 pass, GDI, the frame rate layer, screenshots |
| runtime/menu_game.c/.h | the game: pause list / Options page patches, texts, game options two-way, Esc / PS -> pause -> menu, volumes, quit, the menu's host |
| runtime/window.c | close button -> quit question, WM_APP_QUIT, display modes list, Fullscreen resolution, WM_DISPLAYCHANGE |
| runtime/display_mode.c | Fullscreen's display mode (resolution + refresh) |
| recomp/patches.py | pcmenu_list, pcmenu_options_list, pc_options_hide, pcmenu_text, pcmenu_select, pcmenu_start, pcmenu_frame |

## Testing

- `tests\unit\menu\run.ps1` (305 checks, no window): option table, render scale values and the N(P, rows) mapping,
  legacy keys (the new key wins; files migrated on load; old keys through settings_change), presets, persistence, the
  menu (render scale texts and "Rendering at ... -> shown at ...", staged renderer / mode / resolution, Fullscreen's
  modes, Apply -> Keep with a fake clock: countdown, revert after 15 s, Keep, Revert; close with staged changes -> Apply /
  Discard; quit from the game's menus / the close button (dialog only), from the Quit row, Cancel, mouse on the dialog's
  buttons), the drawing. `-Shots` writes every page and dialog as PNG and the contact sheet. `tests\unit\window`:
  `display_mode_rate_at`, F2 on render_scale, the template.
- In the game, headless with scripted input (PS2_INPUT tokens `menu`, `mup mdown mleft mright mok mback mtabn mtabp`,
  `menushot`, `mquit` = the close button; `PS2_GS_PRESENT_SIZE=WxH`): build\polish_run\run.ps1 (not committed; a copy of
  a memory card with a save): pause list, Save Game... (save1 rewritten), Load Game..., Job Help, Exit to Episode Menu,
  Save and Quit to Title, title SELECT menu -> Options / New Game..., Options pages, Apply -> Keep, the game options
  (log: `[menu] game option invert binocucom: text 0x51 -> 0x52`), quit (exit code 0).
- Not testable here (headless only, no visible window): a real display mode change for Fullscreen at another
  resolution and the close button of a visible window; the code paths are the ones above (display_mode.c,
  window.c WM_CLOSE) and the pure parts are unit-tested.
