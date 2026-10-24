# pc-menu2 (checkpoint)

**Goal**: Esc / PS button open the game's own pause menu; its Options opens a Sly-looking settings menu (the game's
font from the disc at run time) drawn at the display's refresh with an immediate mouse; launch defaults borderless at
the desktop's resolution + 60 fps (migration keeps the player's choices).

**Owner change (coordinator, mid-task)**: never switch the monitor's refresh by default; default = borderless at the
desktop's native resolution and refresh, frame rate 60, NO display lock by default (55 fps lock only as an opt-in
"Match display" choice); exclusive + refresh change only as an explicit Display choice; skip the real-monitor check.

## Done
- RE: text draw f_00216750 -> font = text +0x8; glyph = f_00216B40(font, ch); advance f_00216B80; glyph draw f_00217058
  CALLs the glyph's own VIF packet (a VECTOR shape for the 2D VU1 program).
- runtime/sly_font.c/.h: finds the font in EE RAM (slyf_find ~1.5 ms), extracts signed triangles (slyf_extract),
  rasterises a 2048x1574 R8 coverage atlas with 8 mips (4x4 samples, nonzero rule, ~110-150 ms on a worker thread),
  Windows stand-in (Segoe UI) until then. Polled from pcmenu_ee_frame (EE thread, once a second until found).
- runtime/menu_draw.c: the menu as a primitive list (box/capsule/arrow/glyph), Sly look; CPU raster for GDI + tests.
- runtime/menu_overlay.c: D3D11 renders the list every present (no CPU picture); GDI keeps a CPU picture.
- gpu_present.c gpu_present_menu + gpu_d3d11.c rt_main: while the menu is open the render thread presents the last
  picture + menu whenever the swap chain has room (1 ms polls); window.c presenter likewise; pointer shown/hidden at
  once on open/close.
- menu.c: fractional scroll + animations (menu_animate), slider/scroll-bar drag (SetCapture), wheel scrolls the list.
- Flow (menu_game.c, patch pcmenu_title at 0x1DD964): Esc/PS -> game pause (START patch); on the pause list -> START
  (resume); on a sub page -> Triangle; on the title -> f_001B04E0(0) (its SELECT menu); fallback after 0.6 s ->
  Options directly. Verified in game (build\warp\flow): Esc -> pause, Options -> ours, mback -> pause menu, Esc -> play.

- Defaults (4fdd448): display.mode borderless, frame_rate 60, lock only with pacing = display ("Match display"),
  [settings] version 2 + settings_migrate_defaults (windowed -> borderless unless [chosen] display.mode), [chosen]
  written by menu changes, Apply of a mode, Alt+Enter / F11. test_menu 318 checks.
- Measurements (release, headless, 2560x1440, ep2 pause -> Options -> 48 nav steps):
  * BEFORE (32389dc menu_draw.c on the CPU, scratch bench_old.c -O2): one redraw 78 ms at 1080p, 137 ms at 1440p,
    417 ms at 2160p - on the render thread, in the present, for every hover / selection change (+ a 14.7 MB upload);
    drawn only with game pictures (55-60/s).
  * AFTER, game cadence only (PS2_MENU_SIM_HZ unset: what a fixed 60 Hz display shows): 60 pictures/s, input ->
    picture avg 13-19 ms, max 22 ms; CPU per picture 0.085-0.09 ms (first one 31 ms: atlas upload).
  * AFTER with menu presents at a simulated 165 Hz (PS2_MENU_SIM_HZ=165, headless stand-in for the swap chain):
    224 pictures/s (165 menu + 60 game), input -> picture avg 2.3-3.3 ms, max 6.4 ms; CPU 0.09 ms per picture.

- Title flow verified (Esc -> its menu, Options -> ours, Back -> its menu, Esc on its menu -> Triangle -> title);
  pause sub-pages: Esc on the Data Access question -> back; Job Help takes Esc as START (the game's pause over it).
- Contact sheet notes/pc-menu2/contact_sheet.png; notes: pc-menu.md (pc-menu2 section), settings.md, frame-rate.md,
  d3d11-renderer.md appendix R.
- Tests: test_menu 327 (defaults migration, [chosen], synthetic font), input 964 (wheel / release), window 525.
- Gates (final state): tests\run_tests.py 16/16 + guards; every tests\unit suite passes (window / warp / menu -UnitOnly,
  gs-hw -NoWindow); gpu_diff_all mixed S=1 28/28; tests\pc_text_boot.ps1 (build\rel) passed; debug + release builds;
  scripts\package.py (defaults) packaged 0.1.841.

## Status: complete

## Left
- The game's menu sounds (scene timelines; no callable sound function found).
- package.py with a RELATIVE --out (build/rel) fails in windres ("llvm-rc: No such file") - pre-existing: the -I path is
  relative to the changed cwd; the defaults (absolute) work.
- The machine was shared with the owner's own sly2.exe during the runs: scripted navigation timing slipped (one run
  saved into slot 2 of a test card); the GPU governor fell back to software in one run (use PS2_GS_SW_FALLBACK=0).

## Findings / traps
- The game's font (one object, found in the ep2 hub at EE 0x94CC50, a heap object of the level data): +0x10/+0x18/+0x20
  counts (224 = chars 0x20..0xFF, Windows-1252), +0x14 glyph array (0x40 bytes each; glyph for table index i is at
  +0x40*(i+1)), +0x1C advances (float per table index), +0x24 sorted u32 char table. Glyph +0x1C = DMA chain (CNT..RET)
  of VIF codes for the 2D VU1 program (vuc_188055835, MSCAL 40): STROW 0; V2-32 @7 = segment count; STCYCL 2,1;
  V3-8 @8 = per segment (kind, vertex count, flag); V4-8 @9 = colour (0,0,0,0x80; 0 for flag segments);
  STCYCL 1,1; STMASK 0x50505050; V2-16 masked = int16 x,y per vertex (y negative up, units: adv*16); optional
  STMASK 0x3F3F3F3F + V1-8 masked at the same address = per-vertex w (255 = ADC: no triangle).
  kind 1 -> GIF tag VU14/15 = TRIANGLE FAN, kind 2 -> VU16/17... (read the branch at pc 112..130: kind==2 -> vf21 =
  VU14 strip type 4; kind==1 -> vf23 = VU16 fan type 5; else line strip).
- The 2D renderer is stencil-then-cover: triangles add +-2 to the GREEN channel (FBMSK FFFF00FF, ALPHA (0-Cs)*FIX+Cd,
  sign = orientation), then sprites read the buffer as a texture through a CLUT. Reproduced on the CPU: fan triangles
  add sign(orientation), strip triangles (ADC vertices skipped) add +1, pixel inside where the sum != 0 -> exact glyphs
  with holes (scratch wind_glyph.py; the flag-1 segments are the counters, wound the other way).
- No PlayStation button glyphs in the font (0x80..0xFF = cp1252 symbols, many empty boxes).
- Game pause menu colours (pause dim included): selected text 0x966533, others 0x263340..0x324354, title red 0x650000;
  flat colour text, no outline; the scene is darkened.
- PS2_INPUT "+n" tokens are relative to the PREVIOUS token. warp ep2 -Play: pause at +2700 after X.
- Reference frames: build\warp\ref\frames (not committed).
- TRAP: [IO.File]::ReadAllText/WriteAllText with RELATIVE paths resolve against the main checkout (process cwd) - it
  modified C:\...\sly2-decomp\tests\unit\menu\test_menu.c once (reverted with git checkout). Absolute paths only.
- Kind mapping corrected: kind 1 = FAN, kind 2 = STRIP (VU branch at pc 112..130). Counter segments (flag 1, colour
  alpha 0) subtract: fans normalised by their net orientation, strips -1.
