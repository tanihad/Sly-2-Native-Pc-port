# WIP: package (a double-clickable PC game folder)

**Status:** complete

**Goal:** dist\Sly2\sly2.exe (GUI subsystem, icon, version info) that boots from the user's disc
image without argv/env vars; user data in %LOCALAPPDATA%\Sly2 (or portable.txt); window state
remembered; docs in notes/install.md + notes/settings.md.

## Done
- recomp `generate` writes `ps2_elf_size` + `ps2_elf_sha256` into func_table.c (ps2_runtime.h).
- runtime/disc.c/.h: ISO 9660 lookup (SYSTEM.CNF -> BOOT2 -> file, subdirectories), SHA-256,
  disc_check with one error code per reason.
- runtime/launch.c/.h: log to <user data>\sly2.log without a console; disc/ELF resolution
  (argv[1] ELF > argv[1] image > PS2_ISO > [game] disc > file dialog only with no console and not
  headless); message boxes for fatal start-up errors (GUI only); launch_exit (mc_quiesce + exit).
- settings.c: user data folder (PS2_USER_DIR > console exe -> exe dir > portable.txt -> exe dir >
  %LOCALAPPDATA%\Sly2), wide paths, quoted values, [game] disc, [window] keys, settings_update_text
  + settings_set (key-by-key, atomic replace).
- cdvd.c: cdvd_set_image (wide path), PS2_ISO fallback. mc.c: mc_set_default_dir, RPC lock, mc_quiesce.
- window.c: title "Sly 2: Band of Thieves" (+fps with show_fps), icon resource 1, restore/save
  placement + maximized + fullscreen, WM_CLOSE/WM_ENDSESSION -> save + window_exit_hook.
- build_game.py --package DIR (make_icon.py icon, VERSIONINFO, manifest, windres, -mwindows relink);
  scripts/package.py; dist/ in .gitignore; play.ps1 notes; docs notes/install.md, notes/settings.md.
- Tests: tests/unit/disc (new, 34 checks incl. the real disc image), test_window settings/update
  checks, run.ps1 checks the window state written on close.

## Verification (2026-10-14)
- dist\Sly2 built (23.4 MB, GUI subsystem, only system DLLs, version 0.1.N).
- Portable dir, no env vars/argv, disc in ini: boots, title "Sly 2: Band of Thieves", log in
  sly2.log, exit 0 on WM_CLOSE, [window] written; with PS2_DUMP_FRAMES + PS2_INPUT (verification
  only) the memory card prompt -> save created in <dir>\memcard\slot0 -> intro.
- Restore: x/y/w/h from ini applied exactly; fullscreen=on starts borderless fullscreen and is kept.
- Fake LOCALAPPDATA: data lands in <LOCALAPPDATA>\Sly2, nothing next to the exe; no disc -> the
  file dialog appears (closed by the script -> exit 1); bad ini disc -> Retry/Cancel box (-> exit 1).
- argv[1] = .iso with the GUI build: boots; argv[1] = ELF + PS2_ISO + redirected stderr: log in the
  redirect as before. scripts\run_game.ps1 headless: works, data next to the exe as before.
- tests\run_tests.py 16/16 + checks; every tests\unit\*\run.ps1 passes (window, warp with -UnitOnly);
  test_window --close-test exit 0 with window state saved.

## Findings / traps
- Unit tests compile single runtime files (test_mc: mc.c only; test_window: window.c + settings.c),
  so new cross-file calls go through setters/function pointers (mc_set_default_dir,
  cdvd_set_image, window_exit_hook) or the tests stop linking.
- run_tests.py links all runtime/*.c with -lwinmm -lgdi32 -luser32 -lxinput1_4 -lsynchronization:
  comdlg32, ole32 and shell32 are loaded at run time.
- runtime/hw.h is a FROZEN interface: new declarations went into ps2_internal.h.
- Data location by PE subsystem (not portable.txt in build dirs): many scripts copy sly2.exe to
  other folders (warp_run, ep8_finale, vu_ab, live_warp); console builds must keep data next to
  wherever the exe is, as before.
- printf %ls stops at the first non-ASCII character (C locale): log wide paths as UTF-8.
- mingw main() gets ANSI argv; argv[1] is taken from GetCommandLineW (CommandLineToArgvW) when it
  matches, so Unicode .iso paths dropped on the exe work.
- Reading argv[1] whole to sniff ELF vs ISO would load a 4 GB image: read 4 bytes first.
- orig\SCUS_973.16: 2,597,048 bytes, SHA-256 a511a94b...6a73; the disc's BOOT2 file is identical.
