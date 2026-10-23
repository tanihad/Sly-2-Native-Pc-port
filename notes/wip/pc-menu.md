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

## Pause menu RE (subagent, verified with pokes on the baseline debug exe; details build\re_pause\findings.txt)
- Menu object M = 0x3D9ED0 (HUD mgr 0x3D9E70+0x60), update f_001AEEE0 every frame. Depth *(s32*)0x3D9EF4 (0 closed,
  1 top list, 2 sub-page, 3 memcard). Page records 0x1C at M+0xC+depth*0x1C: +0 count, +4 entries, +0x14 highlight.
  Page 1 entries ptr 0x3D9EFC (== 0x2F65D0 for the pause list; not cleared on close: test depth first).
  State *(s32*)0x3DA0B8: 7 = idle (only state taking input), 0 closed, 8 closing. Same object reused by title menus.
- Game frozen while paused (clock scale *(float*)0x2DDED8 = 0); rendering continues.
- Entry record 32 bytes: +0 text id, +4 select cb, +8 type, +0xC submenu, +0x10 per-frame cb, +0x14 init cb, +0x18 aux,
  +0x1C flags (bit2 stay open, bit1 back). Top list built by f_001B0290 into 0x2F65D0 (full at 7), then
  jal f_001AF4D8 at 0x1B0490 (a1 count, a2 list). Text lookup f_0013E368(world, id) (leaf). Cross path in f_001AEEE0:
  s0 = entry at 0x1AF150, `lw v0,4($s0)` at 0x1AF154.
- Plan (proven by pokes): patch before 0x1B0490 -> copy list to EE 0xFF000 (+our entry {id 0x10000+, flags 2}),
  a1+1; patch f_0013E368 entry -> v0 = EE string (0xFF200) for our id; patch before 0x1AF154 -> open overlay.
  EE 0x80000-0xFFFFF unused (zeros). Mask game input while the overlay is open; swallow the closing button.
- Game Options page: Music vol float 0x3DD190, Effects 0x3DD18C (sound mgr 0x3DD040), applied via f_001FEA60;
  flags blob 0x3D4A60 (+0x24 fNoVibration, +0x30 reverse look, +0x34 reverse aim). Pad bits: START 0x800, tri 0x10,
  circle 0x20, cross 0x40, up 0x1000, down 0x4000.

## Findings / traps
- Build is fast (~45-60 s for all 116 files). New runtime/*.c files are globbed automatically (paths.runtime_sources).
- test_window.c #includes window.c, settings.c, gfx.c: keep those self-contained (new deps need weak stubs).
- Renderer switch live only when the game started on the GPU (gpu_hw_request_switch); sw start -> restart needed.
