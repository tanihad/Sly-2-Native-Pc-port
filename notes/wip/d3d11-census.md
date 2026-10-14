# WIP: gs-census (what Sly 2 asks of the GS, measured over all GS recordings)

Role: gs-census. Branch next/d3d11, worktree C:\Users\tanih\Projects\sly2-worktrees\d3d11.

## Progress
- [x] 0 read the recording format (runtime/gs_record.c), gs_replay.c, register decode conventions in gs_draw.c
- [x] 1 tool: scripts/gs_census.c (+ scripts/gs_census.ps1, scripts/census_summary.py): standalone .gsrec parser
      with GS state tracking (commits 11ff9cc, cb7a15e)
- [x] 2 run over all 13 recordings (outputs in build/census/<worktree>-<name>.txt + tables.md via
      scripts/census_table.py, not committed)
- [x] 2b scripts/gs_post_probe.c (+ .ps1): replays a recording through runtime/ GS synchronously and
      snapshots both frame buffers at every FRAME change -> exact per-channel effect of each post step
      (resolved the shadow / channel-shuffle pipeline below; rope VBlank 5-6, fix-perf brief 3-4)
- [x] 3 census tables + ranked hard cases: final report returned to the orchestrator (see "Shadow
      pipeline (resolved)" below for the new part)

## Shadow pipeline (resolved with gs_post_probe, every recording, every game frame)
B = buffer being drawn, A = the other (displayed) buffer. All post passes run in field space.
1. Scene into B (CT32 512x224). FBA=1 meshes force alpha bit 7 (exempt from shadows).
2. Shuffle 1 (32 sprites 8x448, CTX2, FRAME = A as CT16 FBMSK 00003FFF = write bits 8-15 of each
   halfword, texture = B as CT16, u+8 offset): A.alpha <- B.G exactly (green saved in A's alpha byte;
   display ignores alpha, MMOD=1).
3. G channel becomes a stencil counter: 16 full-screen sprites FBMSK FFFF00FF set/offset G (abe 0
   or (Cs-0)*FIX+Cd 128 cc0), then the shadow volumes (untextured tristrips/trifans, FBMSK FFFF00FF,
   (0-Cs)*128+Cd / (Cs-0)*128+Cd with COLCLAMP=0 = wrapping 8-bit +-, ZTST GREATER/GEQUAL no Z
   write, ATST NEVER AFAIL FB_ONLY, DATE=1 DATM=0 on most). Some scenes (ep3 brief/recon, ep6, all
   fix-perf ones) then do bit-plane logic: FBMSK FFFF80FF (write G bits 0-6) + (0-Cs)*128+Cd, and
   FBMSK FFFFFEFF (write G bit 0 only) with (Cd-0)*FIX(2)+0 = G bit0 <- G bit6.
4. Shuffle 2 (in place, B CT16 FBMSK 7FFFBFFF = halfword bits 8 and 15): B.alpha bit0/bit7 <-
   B.G bit0/bit7 (stencil result into alpha).
5. Shuffle 3 (B CT16 FBMSK 3FFF, texture A CT16): B.G <- A.alpha exactly (green restored).
6. 16 sprites 32x224 read B as T8H (alpha byte -> 256-entry CLUT at CBP 1800) blending into B
   itself: darkens shadowed pixels (~20k px in rope); then alpha reset to 0x80 everywhere.
7. Z16S at ZBP 070 (16-bit view of the Z32 memory) cleared by 16 Z-only sprites, then HUD-ish 3D
   objects drawn with it (FBMSK FF000000 / 0).
8. Binocucom: RTTs 0A0/0A8 (+Z32 0B8) drawn after 6., sampled in the same frame.
9. Next VBlank: motion blur, 16 sprites 32x222 blend A (CT32, TCC 0) into B at FIX 48.

## Tool design (scripts/gs_census.c)
- Parses GSREC001 directly (no renderer link): header (4 MB VRAM, CLUT, 0x64 regs), then records
  {tag,len,payload}: 1 reg write, 2 IMAGE data, 3 VBlank + privileged register snapshot (16 u64:
  PMODE, SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, EXTBUF,
  EXTDATA, EXTWRITE, BGCOLOR, vsync count).
- Vertex queue identical to gs_draw.c (XYZ2/XYZF2 kick, XYZ3/XYZF3 no kick, strips/fans), context from
  PRIM or PRMODE per PRMODECONT, TEX2 folded into TEX0.
- Per kick: pixel bbox (XYOFFSET, scissor) -> page set of the FRAME / ZBUF targets written (FBMSK,
  ZMSK, ATST NEVER+AFAIL honoured); texel bbox (UV or ST/Q, CLAMP modes, mip levels via MIPTBP/MTBA)
  -> page set read. A 512-entry page table remembers the last writer (draw-frame / draw-z / upload /
  local copy, frame number, PSM, target base) so feedback = texture pages written by drawing this or
  the previous frame; in-place = texture pages inside the current FRAME target.
- Draw-state batches: FNV hash of the effective state (attr bits, FRAME, ZBUF, TEST, ALPHA, SCISSOR,
  XYOFFSET, TEX0/TEX1/CLAMP/MIPTBP/TEXA when TME, FOGCOL when FGE, PABE, FBA, COLCLAMP, DTHE/DIMX, CLUT
  version); counts changes between consecutive kicks and distinct keys per frame.
- Page granularity (8 KB) for memory overlap, pixel bboxes for sizes: exact enough for a census, not a
  renderer.

## Findings (interim, from rv_prompt/rv_intro/title/brief + ep3 hub/recon_int/rope/brief)

The frame structure is the same in every recording so far (menus, cutscene, Binocucom, gameplay):
- Field rendering: FRAME = 512x224 PSMCT32 (FBW 8), double-buffered at FBP 000 / 038 (pages), one
  Z32 buffer at ZBP 070, scissor 0..511 x 0..223, XYOFFSET (1792,1936) = (112.0, 121.0) px; the intro
  movie buffer uses Y offset 1936.8 (half-line field offset). SMODE2 INT=1 FFMD=1, DISPLAY 2559/4+1 =
  640 x 448: each VBlank shows one 224-line field, PMODE alternates EN2-only and EN1+EN2 (both circuits
  on the same buffer, DBY 0/1, ALP 128 = line-average flicker filter).
- Per game frame (one VBlank when 60 fps; the loaded-machine recordings show a game frame every
  2-3 VBlanks): 13-50k kicked triangles (tristrips 97-99%), 40-150k vertices, 1.0-5.1k draw-state
  changes, 0.5-3.7k distinct states, 0.6-1.9k TEX0 writes but only 55-270 distinct textures and
  70-290 distinct CLUT addresses per frame; ~9 triangles per state change.
- Every textured mesh chunk is drawn twice, back to back with the same texture: pass A: ATE GEQUAL
  aref 126 AFAIL FB_ONLY, ZTST GREATER, Z write, ALPHA (Cs-Cd)*As+Cd; pass B: ATE NEVER AFAIL
  FB_ONLY (= no Z write), ZTST GEQUAL, ALPHA (Cs-0)*As+Cd (additive). IIP+TME+FGE+ABE on all of them.
  Hence TEST and ALPHA change on ~90% of state changes, TEX0 on 40-67%, CLUT reload on 50-78%
  (CLD=1 on every TEX0 write; the CLUT data itself is resident in VRAM, 2-4 CLUT uploads per frame).
- Cel outlines: lines (and a few linestrips) drawn twice: ATST NEVER + AFAIL ZB_ONLY (Z only),
  then AA1 + ATST NEVER + AFAIL RGB_ONLY (RGB only, coverage alpha blend). 500-2000 lines/frame.
- Shadows/stencil: flat tristrips with FBMSK FFFF00FF (G channel only) and ALPHA (0-Cs)*FIX+Cd FIX
  128 with COLCLAMP 0 (wrapping subtract), DATE=1 on part of them (destination-alpha test); trifans
  with FBMSK FF000000 (RGB only, no alpha) and ZBUF **Z16S** at the same ZBP 070 with FBW 8 (a
  16-bit view of the top half of the Z32 buffer), ZTST GREATER/GEQUAL *with Z write*.
- Post passes every frame (full screen, 16-64 narrow sprites each):
  1. 64 sprites 8x448 (FRAME re-addressed as **PSMCT16 512x448, FBMSK 00003FFF**, scissor
     0..512 x 0..448, XYOFFSET 0) reading the *same buffer* as a CT16 1024x1024 texture with an
     8-px x offset, DECAL, NEAREST, ALPHA (0-0)*FIX+Cs = copy; then 32 sprites with FBMSK 7FFFBFFF.
     = the G->A channel shuffle (bits of G moved into A through the CT16 view of the CT32 pixels).
  2. 16 sprites 32x224 reading the frame as **PSMT8H** (alpha byte as palette index, CLUT at CBP
     1800, 256 entries) with ALPHA (Cs-Cd)*As+Cd: palette-driven darkening (shadow intensity).
  3. On the following VBlank 16 sprites 32x222 (scissor y 1..222) blend the *other* buffer (CT32
     1024x1024 texture, CLAMP, NEAREST) into the one just drawn with (Cs-Cd)*FIX+Cd FIX 48 = 37.5%
     motion blur (the "No Motion Blur" patch address 0x160FA0).
- Binocucom (fix-perf brief.gsrec): two extra 128x128 CT32 targets FBP 0A0 / 0A8 (FBW 2, XYOFFSET
  1984) with Z32 at ZBP 0B8, drawn each frame and used as CT32 128x128 textures (TBP 1400 / 1500)
  on tristrips in the main view, alpha (Cs-0)*FIX+0 FIX 128, ATST NOTEQUAL. These pages (0A0-0AF)
  alias the main Z buffer's last rows (Z32 070 covers 070-0A7).
- Transient textures: every frame uploads 1024x256 T8 (256 KB) to TBP 1080, 128x128 + 128x64 T4
  to 1016/1006 and their CLUTs (16x16 + 8x2 CT32) to 1000/1004: block 0x1000 = page 0x80, i.e.
  *inside the Z32 buffer* (pages 070-0A7); they are consumed before the Z buffer is rewritten.
  No local->local copies, no local->host readbacks in any recording so far.
- Texture formats: T4 (60-65% of textured prims) and T8 (35-40%) with CT32 CLUTs, CSM1, CSA 0;
  sizes 8x8..256x128 (one 1024x256 T8 atlas); TEX1 always LCM 1, MXL 0 (no mipmaps), LINEAR
  (a few NEAREST); CLAMP REPEAT/REPEAT (99%), REPEAT/CLAMP, CLAMP/CLAMP; TEXA 0/128 AEM 0 always;
  TFX MODULATE except DECAL on the CT16 shuffle; TCC 1 except the motion-blur sprite (TCC 0).
  No REGION_CLAMP/REPEAT, no CT24, no mipmaps, no DTHE, no PABE, no CSM2.
- ALPHA combos: (Cs-Cd)*As+Cd, (Cs-0)*As+Cd (+FBA variants = alpha forced 1), (0-Cs)*FIX+Cd 128
  cc0, (0-0)*FIX+Cd (black), (0-0)*FIX+Cs (copy), (0-0)*As+Cd, (Cd-0)*FIX+0 FIX 2 (x2/128 = darken
  to 1/64 ... used for 160-1328 prims), (Cs-Cd)*FIX+Cd 48 (motion blur).
