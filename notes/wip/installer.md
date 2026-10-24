# WIP: installer (Sly2-Setup.exe, a "builder installer")

**Goal**: a native Win32 setup program (C, llvm-mingw) that installs our translator/runtime/tools, asks for the
player's disc image, validates it, builds sly2.exe on the player's PC and installs shortcuts + an uninstaller.
No game code/data in the repo or the setup exe.

**Status: complete** (branch next/installer). Documentation: README.md "Install (easy way)",
notes/install.md "Installer (Sly2-Setup.exe)".

## Done

- installer/{setup.h,main.c,engine.c,util.c,net.c,disc.c}: wizard (7 pages), /silent, Uninstall.exe, WinHTTP
  resumable downloads, SHA-256 (bcrypt), tar.exe unpacking, job-object child processes, shortcuts, HKCU/HKLM
  uninstall entry, sly2.ini [game] disc preset, elevation restart for Program Files (untested: needs UAC).
- scripts/make_installer.py: Sly2-Setup.exe 1.0 MB, --offline Sly2-Setup-Offline.exe 220 MB, SHA256SUMS.txt;
  pins parsed from setup-tools.ps1, ELF values from extract_elf.py.
- scripts/build_game.py: `--progress` lines, version.txt when there is no .git. setup-tools.ps1: `uv python
  install --no-bin --no-registry`.
- tests/unit/installer/run.ps1 (tool commands, payload contents, disc check, downloads vs range_server.py on
  127.0.0.1, /uitest, damaged payload + Uninstall) and tests/installer/e2e.ps1 (install over local HTTP, game
  boots headless, wizard-driven update with cancel + retry, uninstall keeps saves, offline exe + /deletesaves):
  all green. Gates: run_tests.py, all 25 tests\unit\*\run.ps1, debug + release builds, package.py green.

## Findings / traps

- llvm-mingw extracted = 715 MB (not 250 as notes said): aarch64 85, armv7 52, i686 67, python 43, bin 193;
  the installer's tar excludes leave 340 MB and the build works with it.
- uv 0.13 `python install` would put python3.13.exe into ~\.local\bin and register in HKCU\Software\Python
  unless --no-bin --no-registry.
- uv creates a junction cpython-3.13-windows-x86_64-none: tree deletion must not follow reparse points
  (delete_tree removes the link only; tests use `cmd /c rmdir /s /q`).
- No python-build-standalone archive or wheel files exist locally (uv cache has unpacked wheels only), so the
  online python/package path (uv python install / PyPI) cannot be tested without network; it is the path
  setup.ps1 uses.
- PowerShell 5.1: Start-Process -PassThru loses ExitCode unless `$p.Handle` is touched; -ArgumentList arrays
  are not quoted (pass one quoted string).
- PrintWindow of an off-screen window lags one paint behind: pump + sleep before capturing (/uishot).
- build_game.py with a relative --out fails at windres (the .o path is relative to the package folder):
  always absolute (package.py, the installer).
