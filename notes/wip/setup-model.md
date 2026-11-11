# WIP: setup-model (ship the Real-ESRGAN x4plus weights with Setup and updates)

**Goal**: Setup and the signed update bundle carry models/x4plus.bin; the launcher puts it at
<install>\game\models\x4plus.bin (also for a 1.0.2 launcher that applies the update); licence notice; tests.

**Status: complete** (branch next/setup-model, worktree sly2-worktrees\setup-model), except one owner input:
THIRD-PARTY-NOTICES.txt still has a placeholder for Real-ESRGAN's exact LICENSE text (no copy on disk; the brief says
ask, not download). make_release.py refuses to make a release bundle while the placeholder is there (unless
--allow-dirty, the test option); make_installer.py prints a "must not be released" warning.

## Design
- The model rides inside source.zip (= Setup payload = update bundle): `models/x4plus.bin` + `models/x4plus.bin.sha256`,
  `THIRD-PARTY-NOTICES.txt`. The signed manifest's bundle size + SHA-256 cover it; Setup's payload table hash too.
- x4plus.bin is exported by make_installer (scripts/ai_model.py export_x4plus, restricted unpickler) from the pinned .pth
  (sha 4FA0D389...), cached in build/models, and must equal ai_model.X4PLUS_BIN_SHA256 (B3FF1E5F..., 66,819,626 B).
- New launcher (installer/engine.c install_game_files): copies builder\models\x4plus.bin into game.new\models\ and checks it
  against the bundle's x4plus.bin.sha256 before .complete (release.c stage_place_model, unit-tested); a mismatch deletes
  game.new and fails ("installed version unchanged"); builder\models removed after the swap. Sets
  SLY2_SETUP_PLACES_MODEL=1 for the build.
- 1.0.2 launcher (installer code unchanged since v0.1.1136): install_game_files copies only dist's top-level files and
  dist\art\*. So build_game.py --package (from the new bundle) copies the model to dist\x4plus.bin when that env var is
  missing; the 1.0.2 launcher then installs it as game\x4plus.bin. The runtime (gpu_tex_ai.h ra_init_net) also looks at
  <exe dir>\x4plus.bin: one 1-line runtime change, needed for this path.
- Rollback: the model lives inside each game folder (game / game.old), so swaps stay consistent.

## Done
- Code: ai_model.py, make_installer.py, make_release.py, check_no_game_data.py (ALLOWED_THIRD_PARTY by exact SHA-256),
  build_game.py, installer/engine.c, release.c/.h, runtime/gpu/gpu_tex_ai.h, .gitignore, THIRD-PARTY-NOTICES.txt.
- Notes: install.md "AI model", reimagined.md.
- Tests: test_release.c test_model; tests/unit/launcher/run.ps1 (payload model hash + sidecar, notice without "PS2",
  a fake models/x4plus.bin refused); tests/unit/installer/run.ps1 (payload listing allows exactly models/x4plus.bin).
  All green: launcher unit tests, installer unit tests, tests/run_tests.py 16/16.
- Scratch e2e (session scratchpad model_e2e.ps1, /noimport, test key in the scratchpad, 127.0.0.1): A) v0.1.1136 test
  Setup installs, its launcher applies the new bundle: game\x4plus.bin (pinned hash), notices, game boots headless with
  "ai: x4plus net ready", rollback/forward. B) new Setup: game\models\x4plus.bin, boots with the net; a signed bundle
  whose model fails its .sha256 is refused, game untouched, no game.new; good update keeps a model in game and game.old;
  rollback/forward; uninstall.
- Sizes: Setup 1.3 MB (1.0.2) -> 60.7 MB; bundle 1.24 MB -> 60.5 MB; source.zip deterministic (same inputs, same SHA-256).

## Next
- Owner: the exact Real-ESRGAN LICENSE text (BSD-3-Clause, copyright line) for THIRD-PARTY-NOTICES.txt.

## Findings / traps
- No copy of Real-ESRGAN's LICENSE anywhere on disk (searched C:\Users\tanih for "Xintao Wang").
- make_release --count must stay < 65536 (VERSIONINFO 16-bit fields): 999999 fails the game's resource compile.
- In PowerShell `H` is an alias of Get-History (don't name a helper H).
- `| Select -First N` after make_installer kills it before the test build is written.
