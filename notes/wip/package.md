# WIP: package (a double-clickable PC game folder)

**Status:** in progress

**Goal:** dist\Sly2\sly2.exe (GUI subsystem, icon, version info) that boots from the user's disc
image without argv/env vars; user data in %LOCALAPPDATA%\Sly2 (or portable.txt); window state
remembered; docs in notes/install.md + notes/settings.md.

## Design (decided)
- ELF identity: recomp `generate` writes `ps2_elf_size` + `ps2_elf_sha256` into func_table.c.
- runtime/disc.c (new): ISO9660 reader (SYSTEM.CNF -> BOOT2 -> file), SHA-256, validation. Pure,
  unit tested in tests/unit/disc.
- runtime/launch.c (new): user data + log redirection (no console -> <user dir>\sly2.log), disc
  resolution (argv[1] > PS2_ISO > ini [game] disc > file-open dialog only when no console and not
  headless), message boxes for fatal start-up errors in GUI mode, clean exit hook.
- settings.c: user dir (PS2_USER_DIR > portable.txt next to exe > %LOCALAPPDATA%\Sly2), wide paths,
  [game] disc, [window] x/y/width/height/maximized/fullscreen/show_fps, quoted values,
  read-modify-write key updates (settings_update_text, pure).
- Dev builds (build_game.py without --package) write portable.txt into their --out dir, so dev and
  headless runs keep data next to the exe exactly as before.
- mc.c: default card dir set by launch (mc_set_default_dir), RPC lock + mc_quiesce() for clean exit.
- cdvd.c: cdvd_set_image(wide path); PS2_ISO env still the fallback.
- window.c: title "Sly 2: Band of Thieves" (+ " - N fps" when show_fps / PS2_SHOW_FPS), restore +
  save placement/fullscreen, WM_CLOSE -> save state, exit hook.
- build_game.py --package DIR: icon (scripts/make_icon.py, generated), .rc (VERSIONINFO, icon,
  manifest), windres, relink same objects with -mwindows -> DIR\sly2.exe. scripts/package.py wraps it.

## Done
- (nothing yet)

## Next
1. recomp ELF id; disc.c + unit test
2. settings.c changes + tests in test_window
3. launch.c, cdvd/mc/window changes, main
4. build_game.py --package, make_icon.py, package.py, .gitignore dist/
5. docs; verification runs

## Findings / traps
- Unit tests compile single runtime files (test_mc: mc.c only; test_window: window.c + settings.c),
  so new cross-file calls must go through setters/function pointers or the tests stop linking.
- run_tests.py links all runtime/*.c with -lwinmm -lgdi32 -luser32 -lxinput1_4 -lsynchronization:
  comdlg32 must be loaded dynamically.
- Release exe imports only system DLLs (UCRT api sets, user32, gdi32, winmm, kernel32).
- orig\SCUS_973.16: 2,597,048 bytes, SHA-256 A511A94B...6A73; SYSTEM.CNF BOOT2=cdrom0:\SCUS_973.16;1
