# Controls

Implemented in `runtime/window.c` (keep this file in sync with the `kb_buttons` / `kb_axes` and
`xi_buttons` tables there).

## Controllers (XInput)

Any XInput controller (Xbox 360/One/Series, or anything exposed through XInput). The first
connected controller is PS2 port 0, the second is port 1. Plugging one in is noticed within ~2 s.

| DualShock 2      | XInput controller                     |
|------------------|---------------------------------------|
| Cross            | A                                     |
| Circle           | B                                     |
| Square           | X                                     |
| Triangle         | Y                                     |
| L1 / R1          | LB / RB                               |
| L2 / R2          | LT / RT (pressure = trigger travel)   |
| L3 / R3          | press left / right stick              |
| Select / Start   | Back (View) / Start (Menu)            |
| D-pad            | D-pad                                 |
| Left/right stick | left/right stick (radial dead zone)   |

Digital buttons report pressure 0xFF when pressed. Sticks are 0x00..0xFF with 0x80 at rest; up and
left are smaller values, as on a DualShock 2.

## Keyboard (port 0, only while the game window has focus)

| DualShock 2    | Key                                  |
|----------------|--------------------------------------|
| Left stick     | W A S D                              |
| Right stick    | I J K L                              |
| D-pad          | arrow keys                           |
| Cross          | Space (or Numpad 2)                  |
| Circle         | Left Ctrl (or Numpad 6)              |
| Square         | F (or Numpad 4)                      |
| Triangle       | R (or Numpad 8)                      |
| L1 / R1        | Q / E                                |
| L2 / R2        | 1 / 3                                |
| L3 / R3        | C / U                                |
| Start          | Enter                                |
| Select         | Backspace                            |

The left hand covers movement and the face buttons (WASD, Space, Left Ctrl, F, R, Q/E, 1/3), the
right hand the camera (IJKL) or the D-pad (arrows). Stick keys give full deflection; opposite keys
cancel. Keyboard and controller combine on port 0 (buttons add up; a stick driven by keys
overrides the controller's stick). Numpad keys need Num Lock on. While Alt is held, keys are not
passed to the game (Alt+Enter, Alt+Tab, Alt+F4 are window shortcuts).

## Window

| Action                         | Key                         |
|--------------------------------|-----------------------------|
| Borderless fullscreen on/off   | F11 or Alt+Enter            |
| Leave fullscreen               | Esc (only while fullscreen) |
| Widescreen (16:9) <-> 4:3      | F7                          |
| HUD at the edges <-> centred   | F8 (wide aspects)           |
| Quit                           | close the window or Alt+F4  |

The picture is shown at the display aspect from sly2.ini (default 16:9; 4:3 is the PS2 picture
as it was; notes/widescreen.md, notes/settings.md), fitted into the window with black bars; movies
are pillarboxed to 4:3. The title bar shows the frame rate
(frames the game presents per second; "(GPU)" with the GPU renderer). `PS2_WINDOW_GDI=1` forces the
GDI presenter instead of Direct3D 11 (software renderer only).

## Graphics (GPU renderer, the default)

Each change applies from the next frame and is remembered in sly2.ini `[graphics]` (notes/settings.md); the
software renderer only uses F5 (the game's own motion blur), F7 and F8.

| Action                                                        | Key        |
|---------------------------------------------------------------|------------|
| Internal resolution up (1x .. 8x the PS2's, wraps; fixed)     | F2         |
| Internal resolution down (fixed)                              | Shift+F2   |
| Internal resolution automatic (3x, lowered on a slow GPU)     | Ctrl+F2    |
| Outlines: PS2 (one PS2 pixel wide) <-> thin                   | F3         |
| Filters preset: Modern <-> Faithful (toggles back to preset)  | F4         |
| Motion blur on / off                                          | F5         |
| TV flicker filter on / off                                    | F6         |
| Interlacing (field display) on / off                          | Shift+F6   |

Debug only (`PS2_GS_DEBUG_KEYS=1`): F9 switches GPU <-> software renderer, Shift+F9 simulates a GPU device loss.
