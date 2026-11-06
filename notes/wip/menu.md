# WIP: Options menu overhaul (branch next/menu, worktree C:\Users\tanih\Projects\sly2-worktrees\menu)

Goal: fewer, clearer options; Apply for everything (stage / Apply / Discard / leave prompt); Retro look switch;
Quality Low / Medium / High / Ultra (+ Custom); old ini files migrate cleanly. Base next/tex e1a20a9.

## Design (decided)
- Retro look (owner, relayed 2026-11-06): ON = everything as the original game looked (today's Original preset:
  Reference rendering, render scale auto on the integer grid, TV effects, original outlines / lighting / textures, 4:3).
  Quality list has no Original. While On the rows it controls are greyed with the original values (Quality shows
  "Original"); Off restores the player's own options. sly2.ini: [graphics] retro_look = on|off; the option keys keep
  the player's own values while it is on (ps2_settings holds the effective ones, ps2_settings.retro_own[] the player's).
  Old preset = original / ps2 -> retro_look = on + preset = medium (+ aspect 16:9 if 4:3).
- Presets: ini names low / medium / high / ultra / custom (old performance / balanced / quality read as aliases and
  rewritten). Enum ORIGINAL (internal: retro), LOW, MEDIUM, HIGH, ULTRA, CUSTOM.
- Graphics: Quality, Resolution scale, Anti-aliasing (Off / Low = 4x / High = 8x; 2x shown only if the file has it),
  Lighting, Textures, Outlines, Retro look, Advanced... Advanced: Renderer, Rendering, Scaling filter, Motion blur,
  TV effects (filters), HUD layout.
- Display: Display mode, Resolution, Monitor (2+ monitors only), Aspect ratio, Frame rate limit, VSync, Show frame rate.
  Pacing / VRR rows removed (auto; ini keys still work).
- Controls: Invert camera (Off / Vertical / Horizontal / Both), Vibration, Keyboard controls, Controller buttons,
  Advanced... (strength, Binocucom, turret, dead zones, light bar).
- Apply: every row stages (menu.c stg); row "changed" mark, footer "N changes not applied" + Apply / Discard (pad
  Square / Triangle, keys F / R, mouse); leaving with changes -> Apply / Discard / Cancel; display changes -> Keep (15 s).

## Status
- [ ] 1 settings.c/h: presets, retro_look, migration + tests
- [ ] 2 menu.c: pages / rows / staging / dialogs; menu_draw.c footer, changed mark, 3-button dialog; input nav keys
- [ ] 3 unit tests (menu, window), Ultra measurement, pictures build\menu\shots
- [ ] 4 gates: run_tests, m9_exact S1+S3 -Game, gpu_diff_all, r2_sweep -Parallel 1 ep1 tfo,tfb,mf
- [ ] 5 notes/pc-menu.md, notes/settings.md

## Findings / traps
- Gate scripts run the console exe copy in build\warp\<name> (user dir = that folder); flicker_run -Ini writes it.
- r2_sweep tf variants use ini preset=balanced/quality/performance and graphics.filters=modern: must keep parsing.
