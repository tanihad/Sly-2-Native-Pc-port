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

## Owner's final decision (relayed, supersedes the Retro switch above)
Look row: Retro / Faithful (default) / Reimagined (plumbing only: graphics.look, settings_look(); hidden unless the ini
has it). Quality Low / Medium / High / Ultra (+ Custom), locked under Retro. Lighting / Textures moved to Advanced.
Implemented as graphics.look (enum FAITHFUL 0, RETRO 1, REIMAGINED 2) with retro_own[]; no retro_look key.

## Status
- [x] 1 settings.c/h: presets, look, migration + tests (44257b8)
- [x] 2 menu.c staging / pages / dialogs; menu_draw.c footer, dots, 3-button dialog; input F / R, Square / Triangle (44257b8)
- [x] 3 unit tests menu 453 / window 525 / all suites 0 failed; Ultra = 125 % measured; pictures build\menu\shots (c5df16f)
- [x] 4 gates (on c5df16f): run_tests 16/16; m9_exact S1+S3 -Game all identical (base = e1a20a9 runtime: git archive -o +
      tar -> build\basesrc, gs_replay -BuildOnly -Runtime -> build\basert); gpu_diff_all 28/28; r2_sweep -Parallel 1 ep1
      tfo, tfb, mf 3/3 PASS (build\gates.txt)
- [x] 5 notes/pc-menu.md, notes/settings.md
- [x] 6 headless game: an old "preset = original" + 4:3 file -> look retro in effect, file preset medium / look retro /
      aspect 16:9 / option lines Medium's (fix after the first run left renderer_mode = reference); the owner-style custom
      file unchanged, Custom / thin / auto. Menu units again 453 / window 525 after the fix.

STATUS: COMPLETE (2026-11-06)

## Findings / traps
- Gate scripts run the console exe copy in build\warp\<name> (user dir = that folder); flicker_run -Ini writes it.
- r2_sweep tf variants use ini preset=balanced/quality/performance and graphics.filters=modern: must keep parsing.
- [IO.File] in PowerShell resolves relative paths against the process cwd (the main checkout!), not `cd`: always
  absolute paths. An early slip modified runtime/menu.c, menu.h, settings.c in the MAIN checkout (uncommitted; restoring
  them was refused by the permission classifier: left for the owner; diff in the session scratchpad main_accidental.patch).
- The Edit tool keeps its own view of a file: after a PowerShell rewrite, Read the file before the next Edit.
- 150 % + 8x MSAA at 1440p exhausts the 8 GB RTX 4060 Ti (300 ms frames); 125 % + 8x holds 60 fps (GPU ~5.3 ms ep1).
- The settings template is > 4 KB now (test_window used 4 KB buffers: truncated, hid [settings] version).
