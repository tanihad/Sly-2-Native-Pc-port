# WIP: launcher (branch next/launcher, worktree C:\Users\tanih\Projects\sly2-worktrees\launcher)

**Goal**: one player-facing launcher (drop the .iso, import once, big Play, signed updates) that absorbs the
installer (notes/install.md "Planned: launcher").

## Design (decided)
- **The launcher absorbs the installer**: one program from installer/*.c. `Sly2-Setup.exe` = program + payload;
  the installed copy `<dir>\Sly 2 Launcher.exe` = the program without payload (like Uninstall.exe). One window
  with states (welcome/drop -> working -> ready/error) replaces the 7-page wizard. Why: one code path for first
  install, repair and update (all = build into staging, swap), one window for the player, no second UI to keep
  in sync. CLI (/silent etc.) kept for CI.
- **Install layout**: `<dir>\game\` (sly2.exe, README, Controller test.cmd, sly2-version.txt), `game.old\`
  (previous, rollback), `game.new\` (staging, `.complete` marker), `data\sly2.img` (imported disc copy),
  `bundle\sly2-source-<ver>.zip` (sources of game + game.old), `builder\`, `updates\` (downloads),
  `Sly 2 Launcher.exe`, Uninstall.exe, setup-state.ini, setup.log.
- **Import**: whole image copied (CopyFileEx, progress/cancel) to `data\sly2.img.part` -> checked -> renamed.
  Measured: the ISO 9660 tree lists only 34 MB (4 files) but the game reads raw sectors by LBN from its own
  TOC; only 18,583 of 1,408,272 sectors are zero -> nothing worth compacting; a plain copy keeps LBNs.
- **Runtime**: launcher starts `game\sly2.exe "<dir>\data\sly2.img"` (argv[1] image, existing path) and writes
  `[game] disc` = the imported copy; runtime fallback `<exe>\..\data\sly2.img` when nothing usable is configured.
  ISO-path / ELF modes unchanged for dev builds.
- **Updates**: manifest `sly2-release.txt` (key = value) + `sly2-release.txt.sig` (Ed25519 over the manifest
  bytes, hex) + `sly2-source-<ver>.zip`. Default URL
  https://github.com/tanihad/sly2-decomp/releases/latest/download/sly2-release.txt. HTTPS only; plain HTTP to
  loopback only in a test build (-DSLY2_TEST_BUILD, also enables /pubkey=). Ed25519 verify in C
  (installer/ed25519.c, SHA-512 from bcrypt), signing in Python (scripts/sly2_ed25519.py).

## Done
- installer/ed25519.c, release.c (manifest, URL policy, stage_*), update.c, launcher.c (window; replaces the wizard),
  main.c (CLI: /update /repair /rollback /checkupdate /noimport /updateurl /pubkey /waitpid /noupdatecheck),
  engine.c (import, staged install, bundles kept, launcher stub + shortcuts), net.c (https policy, size limit, redirects)
- runtime/launch.c fallback `<exe>\..\data\sly2.img` (+ tests/unit/disc case): disc suite passes
- scripts/release_keygen.py (release key made: %USERPROFILE%\.sly2-release\release-ed25519.key; public half in
  installer/release_key.h), scripts/make_release.py, make_installer.py --test-build, make_source_zip (deterministic)
- tests/unit/launcher/{run.ps1,test_release.c,release_tool.py}: all pass (122 C checks + 20 suite checks)

## Next
1. tests/installer/e2e.ps1 rewrite for the launcher (temp dir in build\): uitest install (drop+import+cancel/retry),
   boot from imported image, update via 127.0.0.1 (bad sig, tampered, failing build, good), rollback, uninstall
2. tests/unit/installer/run.ps1 still passes? docs notes/install.md (launcher, release process)
3. gates: run_tests.py, all tests/unit suites, pc_text_boot, debug+release builds, package.py, make_installer.py

## Findings / traps
- git: C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python: main .venv; clang: main tools\llvm-mingw\bin.
  make_installer.py needs `. <main>\scripts\env.ps1` (llvm-rc on PATH) and an ABSOLUTE --out.
- .NET file APIs resolve relative paths against the main checkout (process cwd): always absolute paths.
- Owner's ISO: C:\Users\tanih\Projects\sly2-decomp\orig\disc\Sly 2 - Band of Thieves (USA).iso (2.88 GB).
- IsWindowVisible() is false for children of a hidden window: /uitest checks WS_VISIBLE instead.
- A Setup whose payload is newer than the installed game offers a local update (UPD_LOCAL) instead of the net check.
