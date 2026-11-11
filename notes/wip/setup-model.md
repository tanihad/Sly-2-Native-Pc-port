# WIP: setup-model (ship the Real-ESRGAN x4plus weights with Setup and updates)

**Goal**: Setup and the signed update bundle carry models/x4plus.bin; the launcher puts it at
<install>\game\models\x4plus.bin (also for a 1.0.2 launcher that applies the update); licence notice; tests.

**Status**: in progress (branch next/setup-model, worktree sly2-worktrees\setup-model)

## Design (decided)
- The model rides inside source.zip (= Setup payload = update bundle): `models/x4plus.bin` + `models/x4plus.bin.sha256`,
  `THIRD-PARTY-NOTICES.txt`. The signed manifest's bundle size + SHA-256 cover it; Setup's payload table hash too.
- x4plus.bin is exported by make_installer (scripts/ai_model.py, restricted unpickler) from the pinned .pth
  (sha 4FA0D389...), cached in build/models, and must equal the pinned bin SHA-256 recorded in ai_model.py.
- New launcher (installer/engine.c install_game_files): copies builder\models\x4plus.bin into game.new\models\ and checks it
  against the bundle's x4plus.bin.sha256 before the swap (release.c stage_place_model, unit-tested). Sets
  SLY2_SETUP_PLACES_MODEL=1 for the build.
- 1.0.2 launcher (installer code unchanged since v0.1.1136): install_game_files copies only dist's top-level files and
  dist\art\*. So build_game.py --package (from the new bundle) copies the model to dist\x4plus.bin when that env var is
  missing; the 1.0.2 launcher then puts it at game\x4plus.bin. The runtime (gpu_tex_ai.h ra_init_net) also looks at
  <exe dir>\x4plus.bin -> one small runtime change, needed for this path.
- Rollback: the model lives inside each game folder (game / game.old), so swaps stay consistent.

## Done

## In progress

## Next
1. ai_model.py pin + export helper; make_installer/make_release; check_no_game_data allow-list
2. build_game.py package (notice + legacy model); installer C; runtime fallback
3. THIRD-PARTY-NOTICES.txt (licence text: not on disk -> ask the owner; placeholder + make_release guard)
4. tests: test_release.c placement, run.ps1 payload check, local update test (scratch key), sizes; notes

## Findings / traps
- No copy of Real-ESRGAN's LICENSE anywhere on disk (searched C:\Users\tanih for "Xintao Wang").
