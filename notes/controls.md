# Controls

Code: `runtime/input.h` (API), `runtime/input.c` (device table, keyboard, XInput, rumble, menu
input, capture), `runtime/input_hid.c` (the input thread: HID controllers, hot-plug),
`runtime/input_ps.c` (DualSense / DualShock 4 reports), `runtime/input_map.c` (mapping, [controls]
keys, names), `runtime/input_padtest.c` (`sly2.exe --pad-test`). Tests: `tests/unit/input`.
`window.c` only forwards window messages and adds the PS2_INPUT test scripts.

Everything uses Windows' own DLLs, loaded at run time (hid.dll, setupapi.dll, cfgmgr32.dll,
xinput1_4.dll): no driver, Steam Input, DS4Windows or anything else to install, nothing added to the
link line. A PC without a controller plays with the keyboard; a missing DLL only disables its part.

## Support matrix

| controller | connection | input | rumble | lights | notes |
|---|---|---|---|---|---|
| DualSense (PS5), DualSense Edge | USB | full report 0x01 | yes (compatible rumble; "improved" flag on firmware >= 2.24) | light bar Sly blue, player LED (middle = port 0) | Edge: extra buttons ignored |
| DualSense | Bluetooth | simple 0x01 at first, full 0x31 after reading feature report 0x05 | yes (report 0x31, CRC-32, sequence) | yes | the controller stays in full-report mode until it reconnects |
| DualShock 4 (v1, v2, Sony USB adapter) | USB | 0x01 | yes | light bar | |
| DualShock 4 | Bluetooth | simple 0x01, full 0x11 after feature report 0x05 / 0x02 | yes (0x11, CRC-32) | light bar | |
| Xbox 360 / One / Series and other XInput pads | USB, Bluetooth, adapter | XInput | yes (XInputSetState) | - | Guide button not readable through XInput |
| other gamepads / joysticks (HID usage page 1, usage 4 or 5) | USB, Bluetooth | buttons 1-14, hat, X/Y + Z/Rz or Rx/Ry | no | - | "basic": DirectInput button order (below); remap in sly2.ini |
| keyboard (+ mouse buttons as keys) | | while the game window has focus | | | remappable |

Not supported: Switch Pro / Joy-Cons and others that need a vendor protocol (they work only if they
present as a standard HID gamepad), DualSense adaptive triggers / HD haptics / gyro / touch
position (the PS2 game has no use for them), the Xbox Guide button.

## Mapping (defaults)

| DualShock 2 | DualSense / DualShock 4 | Xbox | keyboard |
|---|---|---|---|
| Cross | Cross | A | Space, Numpad 2 |
| Circle | Circle | B | Left Ctrl, Numpad 6 |
| Square | Square | X | F, Numpad 4 |
| Triangle | Triangle | Y | R, Numpad 8 |
| L1 / R1 | L1 / R1 | LB / RB | Q / E |
| L2 / R2 | L2 / R2 (pressure = trigger travel) | LT / RT (pressure = travel) | 1 / 3 |
| L3 / R3 | L3 / R3 | stick clicks | C / U |
| Select | Create (DS4: Share) **and touchpad click** | View / Back | Backspace |
| Start | Options | Menu / Start | Enter |
| D-pad | D-pad | D-pad | arrow keys |
| left stick | left stick | left stick | W A S D |
| right stick (camera) | right stick | right stick | I J K L |
| (Options menu) | PS button | - | Esc |

- The touchpad click is a second SELECT, as in the PS4 remasters; the PS button is kept for the PC
  Options menu (INPUT_NAV_MENU). The mute button is not mapped.
- Digital buttons report pressure 0xFF. L2/R2 pressure is the analog trigger value; they count as
  pressed above 30/255 (XInput's threshold) or when the controller sets its digital bit.
- Sticks: radial dead zone (default 12% of the travel, per stick) with rescaling, so drift reads
  centred and full tilt reaches the end; 0x00..0xFF, 0x80 at rest, up/left smaller (DualShock 2).
  "Invert camera X/Y" mirrors the right stick, also when driven by IJKL.
- Keyboard: stick keys give full deflection, opposite keys cancel, a key-driven axis overrides the
  controller's; buttons from keyboard and controller add up. Numpad keys need Num Lock. While Alt is
  held nothing goes to the game (window shortcuts). Mouse buttons can be bound as keys (Mouse1..5).
- Generic HID pads: button 1 Square, 2 Cross, 3 Circle, 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2,
  9 Select, 10 Start, 11 L3, 12 R3, 13 Home, 14 Touchpad (the DirectInput order of most pads);
  hat = D-pad; X/Y left stick; right stick Z/Rz with Rx/Ry as analog triggers if Z and Rz exist,
  else Rx/Ry. Their bindings use these same button names.

## Ports, several controllers, hot-plug

- The first controller that shows activity (a button, a stick past half, a trigger past half) drives
  PS2 port 0, the game's; the next one port 1 (Sly 2 only opens port 0). A controller that has just
  been plugged in does nothing until it is used.
- If another controller is used while the port-0 controller has been idle for 2 s, it takes port 0
  over (picking up a different controller just works); the idle one moves to the freed port.
- Controllers can be connected and disconnected at any time, over USB or Bluetooth. HID interfaces
  are announced by `CM_Register_Notification` (re-scanned at once and again 0.75 s later; without
  notifications every 2 s); a HID read that fails closes the device; XInput slots are probed one per
  500 ms and a failed read disconnects. The game keeps running; a disconnected port-0 controller
  leaves port 0 released (neutral) until another controller is used. Each connect / disconnect is
  logged (`[input] connected: DualSense (Bluetooth) (054C:0CE6)`, `[input] disconnected: ...
  (it drove PS2 port 0 ...)`) and queued as an `input_notice` for an on-screen message.
- The keyboard always adds to port 0.

## Rumble

The game drives a DualShock 2's two motors (small: on/off, large: speed 0-255; notes/pad-protocol.md).
pad.c passes them every VBlank to `input_set_rumble`; the input thread sends them only when they
change: small motor -> the high-frequency (right) motor at 255, large -> the low-frequency (left)
motor at its speed, both times the vibration strength. Vibration off, the Options menu open, or the
game not sending for 300 ms (a stall) stop the motors; closing the window stops them too.

## sly2.ini [controls]

Read at start (and by `input_config_reload`), written by `input_config_set(c, 1)` (the Options menu)
through settings.c's `settings_set`; settings.c itself ignores the section (`settings_read_section`
hands it to input.c). Bad values are logged and keep the default.

```ini
[controls]
vibration = on              ; on | off
vibration_strength = 100    ; 0..100 (%)
invert_camera_x = off       ; right stick X mirrored
invert_camera_y = off
deadzone_left = 12          ; 0..50 (% of the stick's travel)
deadzone_right = 12
lightbar = on               ; DualSense / DualShock 4 light bar
lightbar_color = 2864FF     ; RRGGBB (Sly blue)
key_cross = Space, Numpad2  ; up to two keys per action; "none" = unbound
key_lstick_up = W           ; key_<action>: cross circle square triangle l1 r1 l2 r2 l3 r3 select start
                            ;   up down left right lstick_up/_down/_left/_right rstick_up/_down/_left/_right
pad_cross = cross           ; pad_<action> (the 16 buttons): up to two controller buttons
pad_select = select, touchpad
```

Key names: `A`..`Z`, `0`..`9`, `Space Enter Backspace Tab LShift RShift LCtrl RCtrl CapsLock Up Down
Left Right Insert Delete Home End PageUp PageDown F1 F12 Numpad0..Numpad9 NumpadAdd NumpadSubtract
NumpadMultiply NumpadDivide NumpadDecimal Semicolon Equals Comma Minus Period Slash Backquote LBracket
Backslash RBracket Quote Mouse1..Mouse5`, or `0x<vk>`. Not bindable: Esc (menu), F2-F11 (window and
graphics keys), Alt, Windows keys. Controller button names: `cross circle square triangle l1 r1 l2
r2 l3 r3 select start home touchpad mute up down left right`, aliases `a b x y lb rb lt rt ls rs
back view create share options menu ps guide dpad_up ...`.

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

## Controller test

`sly2.exe --pad-test` (packaged: `Controller test.cmd`; scripts\pad_test.ps1) opens a window
listing every controller the game sees: name, connection, port, battery, report count, Bluetooth
full/simple mode, CRC errors, pressed buttons (PlayStation or Xbox names), raw stick and trigger
values, what is sent to the motors, and the DualShock 2 state the game would get after mapping and
dead zones; keys 1 / 2 / 3 run the small / large / both motors for 1 s on every controller.
`--print` writes the same to the console (development build), `--seconds N` closes it after N s; the
final state always goes to the log (sly2.log in the player's build). No disc needed.

## Protocol notes (DualSense / DualShock 4)

From the public documentation (Linux hid-playstation, written by Sony; SDL's HIDAPI drivers);
layouts in runtime/input_ps.c's header comment.

- USB vs Bluetooth: the HID caps' input report length (64 on USB, longer over Bluetooth).
- Bluetooth CRC-32 (zlib's): over a seed byte (0xA1 input, 0xA2 output) and the report up to its
  last 4 bytes, stored little-endian there. Output reports carry it; input reports are checked
  (a mismatch is counted for the pad test, the report is still used).
- DualSense output, Bluetooth: byte 1 = sequence number << 4 (0..15, per report), byte 2 = 0x10, then
  the common block (hid-playstation's layout; SDL instead writes 0x02 at byte 1 and the block at
  byte 2 - if the real controller ignores our Bluetooth outputs, try `PS2_DS_BT_HEADER=sdl`; and
  `PS2_DS_RUMBLE=classic|improved` overrides the firmware-based rumble flag).
  Rumble: flags0 0x02 (haptics select) + 0x01 (compatible vibration, values halved) on firmware
  before 2.24, or flags2 0x04 ("improved", full values) on 2.24+ / unknown (as SDL). Firmware:
  feature report 0x20, bytes 44-45. Light bar: first a report that releases the LEDs (flags1 0x08,
  flags2 0x02, setup 0x02), then colour (flags1 0x04) + player LEDs (flags1 0x10); re-sent 1.5 s and
  11 s after connecting (the Bluetooth connection animation).
- Feature report 0x05 (calibration) is read at open: over Bluetooth that switches both controllers
  to their full reports; the DS4 also gets 0x02 if 0x05 fails.
- Output reports are written with WriteFile, padded to the caps' output report length, and fall back
  to HidD_SetOutputReport.
- XInput's own HID side (paths with `IG_`) is skipped, so an Xbox pad is not seen twice.

## Real-hardware checks still needed (no controller was connected while this was built)

1. DualSense USB: listed as "DualSense (USB)", all buttons / sticks / triggers in the pad test,
   battery, rumble 1/2/3, light bar blue, middle player LED once a button was pressed.
2. DualSense Bluetooth: listed as "DualSense (Bluetooth)", "full Bluetooth reports" after ~1 s,
   CRC errors 0, rumble and light bar (proves the output CRC / sequence / tag layout).
3. Unplug / reconnect, Bluetooth off / on, while the pad test and while the game runs.
4. In game: move, jump, camera, L2/R2, Select (Create and touchpad), Start; rumble when Sly is hit.
5. DS4 / Xbox / generic pads if available.
