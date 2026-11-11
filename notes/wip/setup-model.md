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
- 00dcb78: ai_model.py pin (bin sha B3FF1E5F..., 66,819,626 B; export deterministic = the light worktree's copy) +
  export_x4plus; make_installer (model_bin, make_source_zip with models/, notice warning, NEED_* sizes); make_release
  (refuses a pending notice unless --allow-dirty); check_no_game_data ALLOWED_THIRD_PARTY; build_game.py
  (notices + package_model_for_old_launcher); engine.c (env var, stage_place_model, skip top-level x4plus.bin,
  builder\models removed after the swap, old-sources list); release.c stage_place_model + release_sha256_file;
  gpu_tex_ai.h fallback <exe dir>\x4plus.bin; test_release.c test_model (102 passed).
- Sizes: Setup 1.3 MB (1.0.2) -> 60.7 MB; source.zip / bundle 1.24 MB -> 60.5 MB (63,387,684 B).
- Scratch e2e (scratchpad\model_e2e.ps1, 1.0.2 test Setup from a detached worktree of v0.1.1136): part A install +
  update by the 1.0.2 launcher OK: game\x4plus.bin pinned hash, notices, game.old without model.

## In progress
- e2e part A boot check (headless, PS2_TEX_AI=1: does it log "x4plus net ready"?), rollback; then part B.

## Next
1. run.ps1: payload model hash check + check_no_game_data refuses a changed model
2. run tests/unit/launcher, tests/unit/installer, tests/run_tests.py (runtime change)
3. notes/install.md, notes/ai-textures.md / reimagined.md; delete scratch Setups / worktree wt102 / e2e folder
4. licence text: ask the owner (not on disk)

## Findings / traps
- No copy of Real-ESRGAN's LICENSE anywhere on disk (searched C:\Users\tanih for "Xintao Wang").
- make_release --count must stay < 65536 (VERSIONINFO 16-bit fields): 999999 fails the game's resource compile.
- In PowerShell `H` is an alias of Get-History (don't name a helper H).
