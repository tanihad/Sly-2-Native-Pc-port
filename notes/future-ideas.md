# Future ideas (owner brainstorms)

Not scheduled. The 1.0 checklist in notes/progress.md comes first. Picked items become milestones in
notes/modern-renderer.md or their own design note.

## 2026-11-04

Agreed next big step after 1.0: **Enhanced lighting and shadows** (per-pixel lighting from the game's own lights,
shadow maps, SSAO, bloom, normal maps; each a switch, Original one click away).

**Picture**
- Temporal AA and upscalers (FSR / XeSS / DLSS): the motion vectors from R8 already exist.
- Anisotropic filtering with generated mipmaps (the PS2 data has none, so distant ground shimmers).
- Draw distance / LOD boost (distant objects at full detail, less pop-in).
- Ultrawide (21:9, 32:9) with the HUD at the edges and a FOV slider.
- HDR output (lamps, lasers, explosions).
- HD texture packs (user-made, loaded from a folder).
- An optional CRT / PS2-TV look filter for nostalgia.

**Feel and controls**
- Mouse + keyboard: free mouse camera and mouse aiming for the Binocucom and gadgets; full rebinding.
- DualSense: gyro aiming, haptics, adaptive triggers (e.g. Bentley's dart gun, Murray's throws).
- Animation smoothing at high fps: in-between frames for characters where the game updates at 60 Hz or less.
- Instant / near-zero loading (the disc image is local; skip artificial waits).

**Quality of life**
- Quick save / multiple save slots and an autosave indicator; import PS2 memory card saves.
- Skip intro logos; skippable cutscenes on first view.
- Photo mode (free camera, hide HUD, time freeze).
- Local achievements / collectible tracker (clue bottles, treasures) and a completion screen.
- Speedrun support: in-game timer, practice / level select, load-time removal.
- Accessibility: subtitle size and background, colour-blind safe lasers and markers, hold-instead-of-mash options.

**Audio**
- Higher-quality resampling of the game's samples; per-channel volume sliders (music, effects, voices).
- Surround / spatial audio.

**Platforms and distribution**
- Linux / Steam Deck, then macOS (already planned: R10/R11), handheld PC presets (ROG Ally, Legion Go).
- Update channels (stable / beta) in the launcher; opt-in crash reports.
- Launcher self-update.

**Bigger, longer term**
- Mod loader (textures, models, later level tweaks) with a mod manager in the launcher.
- Ray-traced shadows / global illumination as an "Ultra" option once Enhanced lighting exists.
- Reuse the recompiler, runtime and renderer for Sly 3 (and Sly 1): the split was deferred earlier.
