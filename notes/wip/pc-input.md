# WIP: pc-input (controller support, remapping, menu input API)

**Goal**: DualSense / DualShock 4 (USB + Bluetooth, native HID), XInput with rumble, generic HID
pads, hot-plug, data-driven bindings in sly2.ini [controls], a menu input API, `--pad-test`.

**Status**: in progress

## Design (decided)

- Only Windows' own DLLs, loaded at run time (hid.dll, setupapi.dll, cfgmgr32.dll, xinput1_4.dll):
  nothing to install, nothing added to the link line; a missing DLL only disables its backend
  (coordinator: the game must run on other PCs with no drivers / Steam Input / DS4Windows).
- Files: `runtime/input.h` (public API), `runtime/input_internal.h`, `runtime/input_ps.c` (pure
  DualSense/DS4 report parsing + output reports + CRC32), `runtime/input_map.c` (pure: config,
  [controls] ini keys, key names, host state -> pad_input, dead zones, port assignment, nav
  repeat), `runtime/input.c` (device table, keyboard, XInput, menu queue, capture, rumble),
  `runtime/input_hid.c` (Windows HID: enumeration, hot-plug, overlapped reads/writes, generic HID),
  `runtime/input_padtest.c` (`sly2.exe --pad-test` diagnostic window).
- window.c keeps the PS2_INPUT scripts; window_get_input = input_get_pad + script_apply.
- pad.c calls input_set_rumble(port, small, large) every VBlank.
- settings.c: only `settings_read_section` (generic) + silently accepting `controls.*` keys.
- DualSense BT output: Linux hid-playstation layout ([1] = seq << 4, [2] = 0x10 tag, common at
  [3]), CRC32 over 0xA2 + bytes 0..73 at 74 (LE). Rumble flags as SDL: haptics select 0x02 always,
  firmware (feature 0x20 bytes 44-45) >= 0x0224 or unknown: flag2 0x04 raw values, else flag0 0x01
  with values >> 1.

## Done

(nothing yet)

## Next

1. input.h / input_internal.h / input_ps.c / input_map.c + tests/unit/input (pure parts)
2. input.c + input_hid.c, wire into window.c / pad.c / settings.c / ps2_runtime.c
3. pad test window, scripts/pad_test.ps1
4. docs: notes/controls.md, notes/pc-experience.md "Interfaces: input", notes/settings.md
5. gates: run_tests.py, unit tests, debug + release build, package, keyboard check in game

## Findings / traps

- tests/unit/window/test_window.c #includes window.c (input tests there must move/adapt).
