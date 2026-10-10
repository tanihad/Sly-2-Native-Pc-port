# fix-render: rendering accuracy on the screens shown so far

**Goal**: the intro movie, mask logo and memory-card prompt render as on a PS2 (button glyph,
fonts, rules, fades, movie colours); every fix covered by a unit test.

## Done
- Debug trace in `runtime/gs_draw.c`: `PS2_GS_TRACE_RECT=x0,y0,x1,y1` (drawing-buffer pixels) logs
  primitives touching the rectangle with vertices, FRAME/ZBUF/TEST/ALPHA/TEX0/TEX1/CLAMP/TEXA and the
  covered texels; `PS2_GS_TRACE_FROM`, `_MAX`, `_TEXELS=0`, `_BRIEF=1`, `_MINSIZE=n`, `_OUT=file`
  (buffered, much faster than stderr). TRXDIR transfers are logged too. `gs_vsync_count` in gs.c.

## Findings (memory-card prompt)
- The "grey blob" left of ": Yes" is the SQUARE button glyph, not a circle: VU1 program 188055835
  (PATH1) draws it as flat trifans: a pink rounded square 7F4C66 (alpha 7F) behind a grey button
  body (19/33 grey) with higher-z "hole" fans making it an outline. The VU1 input colour is
  7F4C667F with the header colour transform mul=1.0 add=0 (VU1 data at TOP+5/+6, see
  vu-microcode.md 188055835 0x0310-0x0348: colour = clamp(in*mul + add, 0, 255)). The same bytes
  are in the game's loaded data (EE 0x0064B403...). GS output matches GS semantics exactly.
- Square IS the Yes button: `PS2_INPUT=500:square:10` creates the save (BASCUS-97316YAOTWTD! in
  build\sly2\memcard). (Moved aside to build\memcard_saved_by_square_test so the prompt returns.)
- All UI colours are authored in the 0..0x80 (= GS 1.0) range: text = add 102.0 (0x66), rules
  334C66, orange 7E4B00, pink 7F4C66. No GS pass doubles them (whole frames traced: 3D scene,
  channel-shuffle post effect (CT16 view of CT32, G->A, T8H CLUT lookup), black clear with alpha
  0x7D, UI, then 37.5% blend of the previous frame (ALPHA 0121 FIX 0x30)). Not a GS bug.
- Display: PMODE 0x8067 in-game (both circuits, ALP 0x80): circuit 1 = FBP 56/0 with DBY 1,
  circuit 2 DBY 0 -> anti-flicker average of lines y and y+1 (implemented in gs_display).
  During the movie PMODE 0x8066 (circuit 2 only) and the game draws the two buffers with
  XYOFFSET y 1936.0 (FBP 0) / 1936.5 (FBP 56): field rendering, one buffer per field.

## In progress
- Interlace: gs_display line-doubles one 224-line field buffer; for field-offset renders (movie)
  this halves vertical resolution and makes the picture jump half a line every field.

## Next
1. Decide/implement field weave (or offset bob) in gs_display, unit test.
2. AA1 lines (rule at y=162 is an AA1 linestrip) - GS semantics uncertain, maybe skip.
3. Check movie colours vs IPU (legal screen is B/W only), mask fade, fonts.

## Findings / traps
- Every build recompiles all 44 files (~60 s) because `recomp generate` rewrites the C files.
- stderr is unbuffered: tracing thousands of prims to stderr slows the game a lot; use
  PS2_GS_TRACE_OUT.
- PS2_PEEK works on GS privileged registers (0x12000000 PMODE, 0x12000070 DISPFB1, ...).
- Pressing Square at the prompt writes a save into build\sly2\memcard; later runs then skip the
  prompt. Move that folder away to get the prompt back.
