# WIP: in-game Options menu (branch next/pc-menu, worktree sly2-worktrees\pc-menu)

**Goal**: a native PC Options menu reached from the game's pause menu (Graphics / Display / Audio / Controls pages,
presets, live apply, saved to sly2.ini), drawn over the picture by both renderers; typed settings model.

## Done
- Read: pc-experience.md, settings.md, controls.md, widescreen.md, recompiler-design.md, d3d11 sec 13 / app. M,
  settings.c/.h, gfx.c/.h, window.c, gpu_present.c (present_fit), ps2_runtime.c main / vblank, window_gpu.h, gpu_hw.h.
- Baseline builds (before any edit): build\base\sly2_rel_base.exe, build\base\sly2_dbg_base.exe (+ build\dbg_base\recomp
  generated C for grepping).

## Design (decided)
- Menu = overlay while the game sits in ITS OWN pause menu (the game keeps running/presenting, so the overlay is
  composited every frame and graphics changes are visible live behind it). Game pad input is neutral while open.
- Opening: from the pause menu (prompt drawn by the overlay while the pause scene is up) with a key/button; Esc /
  controller during gameplay injects START (game pauses itself) then opens. Closing returns to the pause menu.
- Drawing: the menu is rasterised on the CPU (Direct2D software WIC target + DirectWrite text) into a BGRA premultiplied
  bitmap at the output (client) size, only when its state changes; both presenters upload it as a texture and blend it:
  gpu_present.c present_fit (before Present), window.c d3d_render (software renderer, own presenter), gdi_render
  (AlphaBlend). Weak/registered hook so gpu_present.c changes stay one line.
- Files: runtime/settings.c/.h (typed option table), runtime/menu.c/.h (pure model + navigation, unit-testable),
  runtime/menu_draw.c (D2D/DWrite), runtime/menu_overlay.c (D3D11 blend pass), runtime/menu_input.h + weak stubs
  (pc-input provides the real ones), pacing interface header + weak stubs (pc-pacing).

## In progress
- Pause-menu RE (scene object id, entries, selection) - subagent or self.

## Next
1. settings model refactor (table), keep test_window.c test_settings green.
2. menu model + tests; drawing; overlay hooks in both presenters; input stubs; open/close + game input suppression.
3. pause-menu integration; audio volumes (sd.c); presets; docs (settings.md, controls.md, pc-experience.md Interfaces).
4. Verification: run_tests.py, tests\unit\*\run.ps1 -UnitOnly, gpu_diff_all S=1, gpu_game_all S=1, debug+release,
   package.py; screenshots build\screenshots\pc_menu_*.png, contact sheet notes/pc-menu/.

## Findings / traps
- Build is fast (~45-60 s for all 116 files). New runtime/*.c files are globbed automatically (paths.runtime_sources).
- test_window.c #includes window.c, settings.c, gfx.c: keep those self-contained (new deps need weak stubs).
- Renderer switch live only when the game started on the GPU (gpu_hw_request_switch); sw start -> restart needed.
