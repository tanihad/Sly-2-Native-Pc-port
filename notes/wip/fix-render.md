# fix-render: rendering accuracy on the screens shown so far

**Status: complete**

**Goal**: the intro movie, mask logo and memory-card prompt render as on a PS2 (button glyph,
fonts, rules, fades, movie colours); every fix covered by a unit test.

## Done
- Debug trace in `runtime/gs_draw.c`: `PS2_GS_TRACE_RECT=x0,y0,x1,y1` (drawing-buffer pixels) logs
  primitives touching the rectangle with vertices, FRAME/ZBUF/TEST/ALPHA/TEX0/TEX1/CLAMP/TEXA and the
  covered texels; `PS2_GS_TRACE_FROM`, `_MAX`, `_TEXELS=0`, `_BRIEF=1`, `_MINSIZE=n`, `_OUT=file`
  (buffered, much faster than stderr). TRXDIR transfers are logged too. Documented in
  notes/hle-architecture.md. `gs_vsync_count` in gs.c.
- Interlaced display (`runtime/gs.c` deinterlace(), `gs_fb_yfrac`/`gs_fb_psm` recorded in
  gs_draw.c env_update): the intro movie is drawn as fields (odd-field buffer with XYOFFSET.Y + 0.5,
  sceGsSetHalfOffset). gs_display used to line-double every field: half the vertical resolution and
  a half-line jump every field. Now each field goes on its own scanlines, the other lines are woven
  in from the other field where the picture is still and interpolated where it moves. Buffers drawn
  without the half-pixel convention (menus, 3D) stay line-doubled exactly as before.
  Unit test: `t_display_weave` in tests/unit/gs-draw/test_gs_draw.c (149/149 in all three builds).

## Findings (memory-card prompt) - not a GS bug
- The "grey blob" left of ": Yes" is the SQUARE button glyph, not a circle: VU1 program 188055835
  (PATH1) draws it as flat trifans: a pink rounded square 7F4C66 (alpha 7F) behind a grey button
  body (19/33 grey) with higher-z "hole" fans making it an outline. The VU1 input colour is
  7F4C667F with the header colour transform mul=1.0 add=0 (VU1 data at TOP+5/+6; 188055835
  0x0310-0x0348: colour = clamp(in*mul + add, 0, 255)). The same bytes are in the game's loaded
  data (EE 0x0064B403...). GS output matches GS semantics exactly.
- Square IS the Yes button: `PS2_INPUT=450:square:10` creates the save (BASCUS-97316YAOTWTD! in
  build\sly2\memcard); cross and circle leave the prompt up.
- All UI colours are authored in the 0..0x80 (= GS 1.0) range: text = add 102.0 (0x66), rules
  334C66, orange 7E4B00, pink 7F4C66; the in-engine 3D (Cairo intro) peaks around 0x60-0x70 too.
  Whole frames were traced: 3D scene, Z-only clears (ATST NEVER + AFAIL ZB_ONLY), stencil shadow
  volumes in the G channel (COLCLAMP 0 wrap, +-2), G->A channel shuffle through CT16 views of the
  CT32 buffer, T8H CLUT lookup on alpha (shadow darkening), alpha 0x7D fill, 77% black overlay and
  black band, UI, AA1 rule line, then a 37.5% blend of the other buffer (ALPHA 0121 FIX 0x30).
  No pass doubles colours, so the dim UI is what the GS stream specifies. If a reference capture
  ever shows brighter UI, look upstream (data / EE), not in the GS.
- Display: PMODE 0x8067 in-game (both circuits, ALP 0x80): circuit 1 = same buffer with DBY 1,
  circuit 2 DBY 0 -> anti-flicker average of lines y and y+1 (already implemented). During the movie
  PMODE 0x8066 (circuit 2 only).
- Rules: tristrips 334C66 + a horizontal AA1 linestrip exactly on y = 162.0 (full coverage on one
  row, so AA1 would change nothing here; AA1 is still unimplemented in general).
- Movie: legal screen is black/white (IPU CSC gives 000000/FFFFFF as expected); 640x480 RGB32
  texture at TBP 6144 drawn with bilinear DECAL sprites. No colour content to check channel order.
- Fades (loading mask, prompt fade-in) step smoothly; nothing wrong found.

## Findings / traps
- Every build recompiles all 44 files (~60 s) because `recomp generate` rewrites the C files.
- stderr is unbuffered: tracing thousands of prims to stderr slows the game a lot; use
  PS2_GS_TRACE_OUT.
- PS2_PEEK works on GS privileged registers (0x12000000 PMODE, 0x12000070 DISPFB1, ...).
- gs_display runs at vblank *before* the game's VBLANK handler, so it shows the DISPFB the game set
  one vblank earlier (one field of latency); CSR.FIELD at that point is the parity of the *next*
  field. The deinterlacer therefore takes the parity from the buffer's half-pixel offset instead.
- Pressing Square at the prompt writes a save into build\sly2\memcard; later runs then skip the
  prompt. Move that folder away to get the prompt back (old ones: build\memcard_tmp_*).
- Never use [IO.File] with relative paths from PowerShell: .NET's current directory is the main
  checkout, not the worktree.
