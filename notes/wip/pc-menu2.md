# pc-menu2 (checkpoint)

**Goal**: Esc / PS button open the game's own pause menu; its Options opens a Sly-looking settings menu (the game's
font from the disc at run time) drawn at the display's refresh with an immediate mouse; launch defaults borderless at
the desktop's resolution + 60 fps (migration keeps the player's choices).

**Owner change (coordinator, mid-task)**: never switch the monitor's refresh by default; default = borderless at the
desktop's native resolution and refresh, frame rate 60, NO display lock by default (55 fps lock only as an opt-in
"Match display" choice); exclusive + refresh change only as an explicit Display choice; skip the real-monitor check.

## Done
- (nothing yet) read notes; RE start: text draw f_00216750 -> font = text +0x8; glyph = f_00216B40(font, ch) = font+0x14
  + 0x40 * index (index from f_00215A50: binary search of font+0x24 char table, font+0x20 count); advance =
  f_00216B80: font+0x1C floats; glyph draw f_00217058 emits a DMA CALL to glyph+0x1C (the glyph is a VECTOR SHAPE,
  a precompiled VIF packet for the 2D VU1 program, not a texture).

## In progress
- RE of the glyph packet format (EE RAM dump at the pause menu).

## Next
1. Decode glyph geometry from EE RAM at run time -> triangle meshes (vector font: crisp at any resolution).
2. New GPU menu renderer (quads + glyph meshes, drawn at present), menu frames presented between game pictures.
3. Flow: Esc/PS -> game pause menu only; Options -> ours; Back -> pause menu. Title: Esc -> title SELECT menu?
4. Defaults + migration (display.mode borderless, frame_rate 60, lock opt-in; "chosen" flags).
5. Tests, gates, contact sheet, notes.

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
