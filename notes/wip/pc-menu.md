# WIP: in-game Options menu (branch next/pc-menu, worktree sly2-worktrees\pc-menu)

**Goal**: a native PC Options menu reached from the game's pause menu (Graphics / Display / Audio / Controls pages,
presets, live apply, saved to sly2.ini), drawn over the picture by both renderers; typed settings model.

## Done (commits on next/pc-menu)
- settings.c/.h: typed option table (settings_opt), settings_change (validate, store, persist keeping comments,
  re-derive preset, applier callbacks by SET_APPLY_*), presets Original/Performance/Balanced/Quality/Custom,
  internal_scale native (-1), auto_max, [audio] master/music/effects/background, display.monitor, window.fullscreen
  off|on|exclusive. [controls] belongs to input.c (merged main 1d7c382: pc-input). gfx.c hotkeys go through the table.
- menu.c/.h (model, layout, navigation, mouse, bindings via input_config), menu_draw.c (CPU: GDI text + AA shapes),
  menu_overlay.c (D3D11 pass with state save/restore; GDI AlphaBlend; screenshots "menushot"), menu_game.c (pause
  entry patches, Esc/PS -> pause -> menu, volumes through the game's 0x1FEA60, master gain audio_out.c, renderer /
  pacing appliers, host), menu_stubs.c (weak pacing stand-ins), menu_ext.h (pacing interface).
- recomp/patches.py: pcmenu_list / pcmenu_text / pcmenu_select / pcmenu_start / pcmenu_frame (24 patches total).
- gpu_present.c: overlay before Present (+ release); gpu_hw.c PS2_GS_PRESENT_SIZE; window.c: overlay in d3d/gdi
  presenters, menu_tick in the window loop (15 ms open / 40 ms closed), Esc no longer leaves fullscreen, display
  mode/size/monitor appliers, F7/F8 through settings_change; aspect.c same_aspect vs the applied ratio.
- tests/unit/menu (216 checks): settings table, presets, persistence with comments, menu nav/mouse/bindings, drawing at
  4 sizes; -Shots -> build\screenshots\pc_menu_*.png + notes/pc-menu/contact_sheet.png (scripts/menu_shots.py).
- In game (headless GPU, 2560x1440 present target): pause menu shows "PC Settings", Cross opens the menu, pages
  switch, Back returns to the pause menu. build\screenshots\pc_menu_ingame_*.png. Run script build\pcmenu_run\run.ps1.

- In game verified: Esc path during play (pause -> menu), preset / aspect / volumes changed and persisted across a
  restart (t2 -> t3), aspect applies live ("[aspect] now 16:9"), live renderer switch GPU <-> software (t4), visible
  short runs: software renderer own D3D11 presenter (t5) and GDI (t6) with shots. Docs written (pc-menu.md,
  settings.md, controls.md, pc-experience.md Interfaces: menu, README).

## Next
1. Gates (build\pcmenu_run\gates.ps1 -> gates.log): run_tests.py, every tests\unit\*\run.ps1 (-UnitOnly / -NoWindow),
   gpu_diff_all mixed S=1, gpu_game_all S=1, debug build, package.py. Fix anything red.
2. Final in-game 1440p shots of every page from the packaged exe (headless GPU, PS2_GS_PRESENT_SIZE=2560x1440) +
   rebuild the contact sheet; commit; Status: complete.

## Pause menu RE (subagent, verified with pokes on the baseline debug exe; details build\re_pause\findings.txt)
- Menu object M = 0x3D9ED0 (HUD mgr 0x3D9E70+0x60), update f_001AEEE0 every frame. Depth *(s32*)0x3D9EF4 (0 closed,
  1 top list, 2 sub-page, 3 memcard). Page records 0x1C at M+0xC+depth*0x1C: +0 count, +4 entries, +0x14 highlight.
  Page 1 entries ptr 0x3D9EFC (== 0x2F65D0 for the pause list; not cleared on close: test depth first).
  State *(s32*)0x3DA0B8: 7 = idle (only state taking input), 0 closed, 8 closing. Same object reused by title menus.
- Game frozen while paused (clock scale *(float*)0x2DDED8 = 0); rendering continues.
- Entry record 32 bytes: +0 text id, +4 select cb, +8 type, +0xC submenu, +0x10 per-frame cb, +0x14 init cb, +0x18 aux,
  +0x1C flags (&2 stay open, &1 back). Top list built by f_001B0290 into 0x2F65D0 (full at 7), then
  jal f_001AF4D8 at 0x1B0490 (a1 count, a2 list). Text lookup f_0013E368(world, id) (leaf). Cross path in f_001AEEE0:
  s0 = entry at 0x1AF150, `lw v0,4($s0)` at 0x1AF154.
- Game Options page: Music vol float 0x3DD190, Effects 0x3DD18C (sound mgr 0x3DD040), applied via
  f_001FEA60(mgr, category, 1, f12): music = category 1, effects = categories 0 and 3 (f_001AF6D0).

## Findings / traps
- .NET [IO.File] calls resolve RELATIVE paths against the MAIN checkout (cwd of the process): always absolute paths.
  (Once created two empty files in the main checkout: deleted, main clean.)
- PowerShell variables are case-insensitive ($Exe == $exe).
- Build is fast (~45-70 s). New runtime/*.c files are globbed automatically; run_tests.py links runtime/*.c without
  runtime/gpu: GPU calls from runtime/*.c must be weak.
- test_window.c #includes window.c, settings.c, gfx.c: new deps there must be weak externs.
- Renderer switch live only when the game started on the GPU (gpu_hw_request_switch); sw start -> restart needed.
- debug_write_png writes uncompressed PNGs (11 MB at 1440p): scripts/shot_small.py recompresses / shrinks.
