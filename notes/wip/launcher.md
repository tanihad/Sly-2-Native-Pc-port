# WIP: launcher (branch next/launcher, worktree C:\Users\tanih\Projects\sly2-worktrees\launcher)

**Status: complete** (2026-11-03). Documentation: notes/install.md "Launcher" (design, player flow, layout, import,
staging, updates, publishing an update, command line, before going public).

**Goal**: one player-facing launcher (drop the .iso, import once, big Play, signed updates) that absorbs the
installer.

## Design (decided)
- **The launcher absorbs the installer**: one program from installer/*.c. `Sly2-Setup.exe` = program + payload;
  the installed copy `<dir>\Sly 2 Launcher.exe` = the program without payload (like Uninstall.exe). One window
  with states (welcome/drop -> working -> error / ready) replaces the 7-page wizard. Why: one code path for first
  install, repair and update (all = unpack sources, build, stage, swap), one window for the player, no second UI.
- **Install layout**: `game\` (+ `sly2-version.txt`), `game.old\` (previous), `game.new\` (staging, `.complete`),
  `data\sly2.img` (imported disc copy), `bundle\sly2-source-<ver>.zip` (sources of game + game.old), `builder\`
  (tools kept by default), `updates\`, `Sly 2 Launcher.exe`, Uninstall.exe, setup-state.ini, setup.log.
- **Import**: whole image copied (the ISO 9660 tree lists only 34 MB; the game reads 2.7 GB by sector number;
  only 1.3% of sectors are zero -> a plain copy).
- **Runtime**: launcher starts `game\sly2.exe "<dir>\data\sly2.img"`; sly2.ini `[game] disc` = the copy;
  runtime/launch.c falls back to `<exe>\..\data\sly2.img`. Dev modes unchanged.
- **Updates**: signed manifest (Ed25519, key in installer/release_key.h) -> bundle size + SHA-256 -> staged build
  -> swap; HTTPS only (loopback HTTP + /pubkey only in -DSLY2_TEST_BUILD builds).

## Done
- installer/ed25519.c, release.c, update.c, launcher.c, main.c, engine.c, net.c, disc.c (wording); runtime/launch.c
- scripts/release_keygen.py (release key made: %USERPROFILE%\.sly2-release\release-ed25519.key, public
  2d2fa1da...a9bde4 in installer/release_key.h), scripts/make_release.py, scripts/sly2_ed25519.py,
  make_installer.py (--test-build, deterministic source.zip, per-monitor DPI manifest)
- tests: tests/unit/launcher (122 C checks + 20), tests/unit/disc (+ fallback case), tests/installer/e2e.ps1 rewritten
- Gates 2026-11-03: run_tests 16/16; every tests/unit suite passes except window's known F11/Alt+Enter flake (2);
  e2e -SkipOffline passed (install 90 s incl. 2.9 GB import, update 58 s); pc_text_boot; debug + release builds;
  package.py; make_installer.py.

## Next (after the owner's review)
- Code-sign Sly2-Setup.exe; make the repository / releases public; publish the first signed release (make_release.py
  prints the gh command); optionally a launcher self-update (today a new launcher comes with a new Setup;
  `min_launcher` sends players there).

## Findings / traps
- git: C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python: main .venv; clang: main tools\llvm-mingw\bin.
  make_installer.py needs `. <main>\scripts\env.ps1` (llvm-rc on PATH) and an ABSOLUTE --out.
- .NET file APIs resolve relative paths against the main checkout (process cwd): always absolute paths.
- IsWindowVisible() is false for children of a hidden window: /uitest checks WS_VISIBLE instead.
- A Setup whose payload is newer than the installed game offers a local update (UPD_LOCAL) instead of the net check.
- Windows CNG has no EdDSA (only X25519 ECDH): Ed25519 verification is our own code; SHA-512 comes from bcrypt.
