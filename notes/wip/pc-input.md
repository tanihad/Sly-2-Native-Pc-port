# WIP: pc-input (controller support, remapping, menu input API)

**Goal**: DualSense / DualShock 4 (USB + Bluetooth, native HID), XInput with rumble, generic HID
pads, hot-plug, data-driven bindings in sly2.ini [controls], a menu input API, `--pad-test`.

**Status**: in progress (code + docs done; gates and in-game keyboard check remaining)

## Design (decided)

- Only Windows' own DLLs, loaded at run time (hid.dll, setupapi.dll, cfgmgr32.dll, xinput1_4.dll).
- Files: runtime/input.h, input_internal.h, input_ps.c (pure reports/CRC), input_map.c (pure
  mapping/config/names/nav/ports), input.c (device table, keyboard, XInput, rumble, menu, capture),
  input_hid.c (input thread: HID enumeration, CM notifications, overlapped I/O, generic HID),
  input_padtest.c (`--pad-test`). window.c: window_get_input = input_get_pad + scripts; wndproc ->
  input_window_message first; close -> input_shutdown. pad.c -> input_set_rumble every VBlank.
  settings.c: settings_parse_section / settings_read_section; [controls] keys ignored by apply().
- DualSense BT output: hid-playstation layout ([1] = seq << 4, [2] = 0x10, block at 3); fallback
  PS2_DS_BT_HEADER=sdl. Rumble flag per SDL (fw >= 0x0224 improved), PS2_DS_RUMBLE override.

## Done

- All code above; tests/unit/input (961 checks), tests/unit/pad (rumble hand-off), tests/unit/window
  adapted (input tests moved), debug build OK, `--pad-test` window + `--print` verified (no pads).
- scripts/pad_test.ps1, package writes "Controller test.cmd", README controls paragraph.
- notes/controls.md (matrix, mapping, ports, rumble, ini, protocol, hardware checklist),
  notes/pc-experience.md "Interfaces: input", notes/settings.md, notes/pad-protocol.md.
- tests/run_tests.py 16/16.

## Next

1. gates: every tests/unit/*/run.ps1 (window/warp -UnitOnly), debug + release builds, package.py
2. in-game keyboard check with the packaged build in a portable folder ([game] disc = iso)
3. final report

## Findings / traps

- tests/unit/window/test_window.c #includes window.c; its run.ps1 now also compiles input*.c.
- PowerShell [IO.File] with relative paths resolves against the main checkout: use absolute paths.
- HID enumeration on this PC: 23 interfaces, no controller.
