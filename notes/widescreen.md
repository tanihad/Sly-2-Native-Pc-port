# Widescreen (16:9 by default, any W:H)

The game renders a 512x448 picture for a 4:3 TV (two 512x224 fields). The PC port shows that
picture stretched to the display aspect A (default 16:9, `aspect` in sly2.ini, notes/settings.md)
and makes the game draw for it:

- the **3D view is wider**: the camera's horizontal range grows by A/(4/3); the vertical field of
  view is unchanged (Hor+). Culling follows, since the frustum planes come from the same numbers;
- **2D content keeps its 4:3 proportions**: HUD, menus, text and the vector-art screens are drawn
  scaled by (4/3)/A about the screen centre (they sit in the centre 4:3 area), except primitives that
  span the whole width (backgrounds, fades, bars), which still cover the whole picture;
- **movies are pillarboxed** (shown at 4:3 with black bars).

At 4:3 none of this happens: every patch tests `ps2_aspect.wide` first, no hint is emitted, frames
are bit-identical to the game without the patches.

Everything is done in the recompiled code with **instruction patches** (recomp/patches.py: native C
inserted before/after one instruction, guarded by the SHA-1 of the function's original bytes) that
call runtime/aspect.c; nothing in guest memory is poked. The renderer side is the GS side-band
register 0x7F (runtime/gs_hint.h).

## Patches (addresses: SCUS_973.16, NTSC-U)

| patch | where | what |
|---|---|---|
| ws_projection | 0x14DBA4 in f_0014DB70 (camera: recompute frustum) | `lwc1 $f12, 0x3C($s0)` loads the camera aspect (1.2987 = 640/492.8, written once by the camera init at 0x14D750, the community patch's constant); multiplied by A/(4/3). It feeds the X range (+0x40 = tan(fov/2)*aspect), the near-clip radius (+0x48), the projection matrix (0x11C448) and the frustum planes (f_0014DC58). The stored aspect stays the game's own. |
| ws_screen_project | after 0x14F36C in f_0014F308 (world point -> screen [-1,1]) | the X range loaded into $f4 times (4/3)/A, so the result is the 4:3 screen position (= community "widescreen bug fix" at 0x14F370). Callers: 2D labels/markers at world points (0x1CD780, 0x202D38, 0x218220), flying pick-ups (0x1D04E8), on-screen tests (0x173768 via 0x173588, 0x14AA30), camera near-plane test (0x2236E8). 2D content is laid out on the 4:3 screen and drawn with the HUD transform, which puts it exactly on the world point of the wider view; on-screen tests keep the game's 4:3 area. |
| ws_screen_unproject | after 0x14F2B4 in f_0014F268 (screen -> world point) | the inverse: same 4:3 X range, so 0x1D05A0 / 0x2236E8 round trips stay exact (the community patch leaves it, breaking the round trip). |
| ws_label_size | after 0x1CD7E0 in f_001CD780 (text label at a world point) | the label's size is 1/(xrange*depth); with the 4:3 range it keeps its size next to the 3D object after the HUD transform. |
| ws_render_alloc | 0x1CD9D4 (jr $ra) in f_001CD998 (allocate a render-list entry) | remembers which entries are 2D scene set-ups (allocated from 0x2130C0, return 0x213278) |
| ws_render_entry | 0x158378 in f_00157FA0 (sorted render list -> DMA chain) | before an entry's DMA CALLs: HUD_BEGIN(centre) when 2D content starts, HUD_END when it stops |
| ws_render_done | 0x1583FC in f_00157FA0 | HUD_END after the last render class |
| ws_movie_begin / ws_movie_end | entry 0x1B3BD0 / return 0x1B3EB4 of f_001B3BD0 (play one movie) | ASPECT_4_3_BEGIN / END straight into the GS stream |

The community "character portraits fix" (0x1BE808 in f_001BE6F0, portrait projection x 4/3) is not
needed: see "Portraits" below.

## How the game draws a frame (what the hints rely on)

- One VIF1 DMA chain per frame, kicked once. It is built by the frame builder ("DMAS", Sly 1 name)
  at 0x2DEA28: +0x00 end, +0x04 start, +0x08 open CNT tag, +0x0C write position, +0x10 VIF word
  mode, +0x14 open GIF primitive. Hints emitted by EE code at call time would land in the wrong
  place of the GS stream, so `aspect_hint` writes them **into the chain** at the builder's write
  position: [DMA CNT,] VIF NOPx3 + DIRECT 2, a PACKED GIFtag (NLOOP 1, EOP, A+D), A+D(0x7F, value).
- Render list: 16-byte entries {render priority, sort key, DMA CALL address, second CALL} at
  0x3B3980 (end pointer 0x3D4A50), allocated by 0x1CD998, sorted (0x157AE0) and emitted by
  f_00157FA0 as CALL tags, with a begin/end function per render class at each class change
  (priority -> class table 0x2BDD20, class functions 0x2BDD88).
- **All 2D is render priority 21** (class 5: begin 0x211A80 uploads the 2D VU1 program's constants
  and clears alpha, end 0x211C10). Creators: 0x2130C0 (scene set-up: GS context + 16 sprites that
  write Z = 0 over the whole screen; 2D shapes are layered with ZTST GREATER), 0x217298 (pass), 0x2174C8 /
  0x2177E0 (vector shapes), 0x216750 / 0x2163B0 (text). 2D scenes (vector art, drawn by the vtable
  method 0x213300) work in a 640x492.8 space; their GS X offset is at +0x9C (1792 = 2048 - 256).
  Scene set-ups stay untransformed (their Z clear must cover the whole picture, or full-width
  backgrounds fail the Z test at the sides); everything else in priority 21 gets HUD_BEGIN(C).
- The pause menu ("We'll be right back!") is a scene with a full-width black background: with the
  full-width rule it looks pillarboxed (black sides) with the composition centred. Loading, memory
  card and title screens work the same way.

## Renderer side (GS register 0x7F, runtime/gs_hint.h)

Payload {u8 kind, u8 arg, u16 0, u32 value}: HUD_BEGIN (arg anchor L/C/R, value float bits of the
X scale (4/3)/A), HUD_END, ASPECT_4_3_BEGIN/END, PHASE (diagnostic). gs_record.c records them as
ordinary register writes, so 16:9 recordings replay with scripts/gs_replay.

Software renderer (runtime/gs_draw.c): for a primitive kicked inside HUD_BEGIN/END, each vertex X
(12.4, before XYOFFSET) becomes `c + (x - c) * scale` with c = XYOFFSET.x + {0, W/2, W} * 16
(W = FRAME width, FBW*64 = 512 here) - unless the primitive's X range covers the whole FRAME width
(within one pixel), then it is drawn as given. The vertex queue keeps the game's coordinates (strips
and fans share vertices). Scissor is not transformed (the 2D pass uses the full-screen scissor).
ASPECT_4_3 state is sampled at each VBlank's place in the stream (gs_thread.c) and travels with the
picture to the presenter (`gs_picture_content43`).

Presenter (runtime/window.c): each frame carries its display aspect (`window_present_aspect`):
A for game pictures, 4:3 for 4:3 content (pillarboxed inside a 16:9 window). Default window 960
pixels high at A (1707x960 at 16:9). Frame dumps (PS2_DUMP_FRAMES) are written as shown: square
pixels at the picture's height (796x448 at 16:9, 4:3 content with bars); PS2_DUMP_RAW=1 keeps the
512-wide picture; at 4:3 dumps are raw as before.

## Portraits

Render classes 8 and 9 (priorities 15 and 18, begin 0x1BE990 / 0x1BE9E8 -> 0x1BE6F0) draw a 3D
character into a 2D rectangle with their own projection (aspect = rect width / height). See the
verification notes for whether they need the HUD transform (they are not in priority 21).

## Not done / for later

- L/R anchors: every 2D element is centre-anchored (HUD in the 4:3 safe area). Edge anchoring per
  HUD widget needs the widget's extent at the EE (2D scene bounds); the hint format already has the
  anchor. World-positioned 2D (markers, labels) must stay centre-anchored.
- Binocucom / photo framing: the game's on-screen tests use the 4:3 area (ws_screen_project), so
  framing is judged as on a PS2 while the view shows more at the sides.

## Notes for the GPU renderer (M10)

- Same contract as the sw renderer: transform per primitive at kick, full-width primitives exempt,
  anchor c from XYOFFSET.x and the FRAME width, scale from the hint's value (or the configured A).
- Hints arrive inside the frame's VIF1 chain (PATH2 DIRECT, an A+D write of register 0x7F), and from
  the movie player as direct writes; both go through gs_write_reg like any register.
- Pillarbox: use the ASPECT_4_3 state at the VBlank (gs_picture_content43 in the sw path).
- 4:3 setting: the stream has no 0x7F writes at all.
