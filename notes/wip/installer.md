# WIP: installer (Sly2-Setup.exe, a "builder installer")

**Goal**: a native Win32 setup program (C, llvm-mingw) that installs our translator/runtime/tools, asks for the
player's disc image, validates it, builds sly2.exe on the player's PC and installs shortcuts + an uninstaller.
No game code/data in the repo or the setup exe.

**Status**: in progress (branch next/installer, worktree C:\Users\tanih\Projects\sly2-worktrees\installer).

## Design (decided)

- Sources: `installer/*.c` (main.c wizard+cmdline, engine.c install/uninstall steps, util.c files/process/
  log/sha256/registry/shortcuts, net.c WinHTTP download with resume, disc.c ISO9660 check = extract_elf.py).
- `scripts/make_installer.py`: payload = tracked recomp/**, runtime/**, the 6 build scripts, requirements.txt,
  version.txt; pins parsed from scripts/setup-tools.ps1 (single source of truth) into a generated header;
  stub exe + appended overlay (entries + table + trailer "SLY2PAY1"); `--offline` also bundles llvm-mingw
  zip, uv zip, python folder (zip of the developer's verified tools\python\cpython-3.13.16-*), uv-cache.
  Output build\installer\Sly2-Setup.exe / Sly2-Setup-Offline.exe.
- Install layout: `<dir>\sly2.exe, README.txt, Controller test.cmd, Uninstall.exe, setup.log, setup-state.ini`,
  `<dir>\builder\` = sources + tools\ (llvm-mingw trimmed: no aarch64/armv7/i686 sysroots, lldb, clangd,
  python) + .venv + orig\SCUS_973.16 + build\.
- Python: online = `uv python install 3.13.16 --no-bin --no-registry` (uv verifies its own hash; uv.exe pinned);
  packages `uv pip install --require-hashes -r requirements.txt`. Offline/local = python folder + uv cache.
- Disc preset: writes `[game] disc = <iso>` into <userdata>\sly2.ini (creates minimal file or edits).
- Test overrides: /startmenu= /desktopdir= /regkey= /userdata= /mirror= /from=; /silent mode; /uninstall.
- Updates: payload version differs from built version -> builder\build removed (zip mtimes are older than
  objects: never trust incremental across versions).

## Done

- installer/{setup.h,main.c,engine.c,util.c,net.c,disc.c}; scripts/make_installer.py (-Wall -Wextra -Werror,
  1.0 MB setup, 189 KB program); build_game.py `--progress` + version.txt fallback; setup-tools.ps1 uv flags.
- tests/unit/installer/run.ps1 (+ range_server.py, make_isos.py): all green (23 checks).
- tests/installer/e2e.ps1 written (online-via-127.0.0.1 install, boot, update, uninstall, offline exe).

- e2e run 1 green (all 6 parts; install 73 s, offline 66 s); sizes in notes/install.md "Installer".
- docs: README "Install (easy way)", notes/install.md "Installer (Sly2-Setup.exe)".
- /uishot page renders checked (layout OK); /uiinstall + /uicancel: the wizard's real install path with
  cancel + retry, used by e2e step 4.

## In progress

- e2e run 2 (-SkipOffline) with the wizard-driven update.

## Next

1. gates: run_tests.py, every tests\unit\*\run.ps1 (window/warp/menu -UnitOnly, gs-hw -NoWindow), debug +
   release builds, package.py; then a final full e2e; delete %TEMP%\sly2-installer-e2e, build\installer,
   build\unit\installer; mark complete.

## Findings / traps

- llvm-mingw extracted = 715 MB (not 250 as notes said): aarch64 85, armv7 52, i686 67, python 43, bin 193.
- uv 0.13 `python install` would put python3.13.exe into ~\.local\bin and register in HKCU\Software\Python
  unless --no-bin --no-registry.
- uv creates a junction cpython-3.13-windows-x86_64-none: tree deletion must not follow reparse points.
- No python-build-standalone archive or wheel files exist locally (uv cache has unpacked wheels only), so the
  online python/package path cannot be tested here; it is the same uv path setup.ps1 uses.
