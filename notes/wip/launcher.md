# WIP: launcher (branch next/launcher, worktree C:\Users\tanih\Projects\sly2-worktrees\launcher)

**Goal**: one player-facing launcher (drop the .iso, import once, big Play, signed updates) that absorbs the
installer (notes/install.md "Planned: launcher").

## Design (decided)
- **The launcher absorbs the installer**: one program from installer/*.c. `Sly2-Setup.exe` = program + payload;
  the installed copy `<dir>\Sly 2 Launcher.exe` = the program without payload (like Uninstall.exe). One window
  with states (welcome/drop -> working -> ready/error) replaces the 7-page wizard. Why: one code path for first
  install, repair and update (all = build into staging, swap), one window for the player, no second UI to keep
  in sync. CLI (/silent etc.) unchanged for CI.
- **Install layout**: `<dir>\game\` (sly2.exe, README, Controller test.cmd), `game.old\` (previous, rollback),
  `game.new\` (staging), `data\sly2.img` (imported disc copy), `bundle\source.zip` (+ `source.previous.zip`),
  `builder\`, `updates\` (downloads), Uninstall.exe, setup-state.ini, setup.log.
- **Import**: whole image copied (CopyFileEx, progress/cancel) to `data\sly2.img.part` -> checked -> renamed.
  Measured: the ISO 9660 tree lists only 34 MB (4 files) but the game reads raw sectors by LBN from its own
  TOC; only 18,583 of 1,408,272 sectors are zero -> nothing worth compacting; a plain copy keeps LBNs.
- **Runtime**: launcher starts `game\sly2.exe "<dir>\data\sly2.img"` (argv[1] image, existing path) and writes
  `[game] disc` = the imported copy; runtime fallback `<exe>\..\data\sly2.img` when nothing usable is configured.
  ISO-path / ELF modes unchanged for dev builds.
- **Updates**: manifest `sly2-release.txt` (key = value, product/version/bundle URL/size/sha256/min_launcher) +
  `sly2-release.txt.sig` (Ed25519 over the manifest bytes, hex) + `sly2-source-<ver>.zip`. Default URL
  https://github.com/tanihad/sly2-decomp/releases/latest/download/sly2-release.txt. HTTPS only; plain HTTP to
  loopback only in a test build (-DSLY2_TEST_BUILD, also enables /pubkey= override). Ed25519 verify in C
  (installer/ed25519.c, own SHA-512), signing in Python (scripts/release_keygen.py, scripts/make_release.py).

## Done
- (nothing committed yet)

## Next
1. ed25519.c + sha512 + tests (RFC 8032 vectors); python ed25519 (scripts/ed25519_py.py)
2. manifest.c (parse/verify) + tests; stage.c (swap/rollback/recover) + tests
3. import (engine) + layout changes + update flow in engine
4. launcher window (launcher.c) replacing wizard; uitest/uishot
5. runtime fallback in launch.c; scripts (keygen/make_release), make_installer changes, package.py
6. tests/unit/launcher/run.ps1 (+ local update server e2e), docs notes/install.md, gates

## Findings / traps
- git: C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python: main .venv; clang: main tools\llvm-mingw\bin.
- Owner's ISO: C:\Users\tanih\Projects\sly2-decomp\orig\disc\Sly 2 - Band of Thieves (USA).iso (2.88 GB).
