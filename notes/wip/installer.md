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

(nothing yet)

## Next

1. installer C sources + make_installer.py; build the stub.
2. unit tests (tests/unit/installer/run.ps1): sha256, disc check (synthetic + real image), download against a
   local range-capable server (resume, 404, bad hash), payload table.
3. e2e: silent install into %TEMP% test dir with /from + /mirror, game boots headless, uninstall.
4. docs (README Install (easy way), notes/install.md), gates, sizes.

## Findings / traps

- llvm-mingw extracted = 715 MB (not 250 as notes said): aarch64 85, armv7 52, i686 67, python 43, bin 193.
- uv 0.13 `python install` would put python3.13.exe into ~\.local\bin and register in HKCU\Software\Python
  unless --no-bin --no-registry.
- uv creates a junction cpython-3.13-windows-x86_64-none: tree deletion must not follow reparse points.
- No python-build-standalone archive or wheel files exist locally (uv cache has unpacked wheels only), so the
  online python/package path cannot be tested here; it is the same uv path setup.ps1 uses.
