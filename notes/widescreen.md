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
| ws_scene_begin / ws_scene_end | entry 0x213300 / return 0x2137E8 of f_00213300 (2D scene: draw) | the scene being drawn (its object id picks the anchor policy) |
| ws_dl_begin / ws_dl_end / ws_dl_object | entry 0x21A998, return 0x21AA54, loop head 0x21A9F8 of f_0021A998 (queue a display list) | nesting depth and the root-level object (one widget) the items are queued under |
| ws_dl_queue_mask, ws_item_queue | 0x21AA24 (f_0021A998), 0x2159CC (f_00215960) | an item queued: remembered with its root-level object |
| ws_item_draw | 0x213408 (f_00213300, loop over the queued items) | the anchor of the item being drawn, for the render entries it allocates |
| aspect_switch | 0x1A75A8 in f_001A7518 (main loop, after the camera update 0x14D470) | run-time aspect / HUD switch (F7/F8): applied here, camera frustum recomputed (0x14DB70). Runs at 4:3 too, does nothing unless a switch is pending |

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

## HUD anchors (`hud = edges`, the default)

As a PC release would, the HUD widgets at the left / right of the 4:3 screen sit at the left / right
edges of the wide picture (keeping their distance to the edge, in screen heights); everything
designed as a centred layout stays centred. `hud = centered` (or F8) keeps the whole HUD in the
centre 4:3 area (every HUD_BEGIN anchored C, as before).

**How the game places 2D** (Flash-like "scenes"; RE in notes/wip/hud.md, Findings): a scene object
(vtable at +0xC 0x2DAF30 or 0x2C9670, object id at +0x18, size 640x480 at +0x94/+0x98, its GS
offsets at +0x9C/+0xA0 centre it on the 512-wide screen, layer +0xA4) is drawn by 0x213300. That
first queues the items of its root sprite's display list (0x2185D0 -> 0x2185F8 -> 0x21A998, nested
sprites recursively through 0x215960) into a list at +0xFC: 0x70-byte placed objects with their
world matrix (+0x10: a b c d tx ty, scene units), character (+0x30: shape, text, sprite) and colour.
Then it draws them in order (loop at 0x213408, 0x2159E0 -> 0x2174C8 / 0x216BC8 / 0x2177E0, each
allocating render-list entries). A widget is a **root-level object** of a scene's display list; its
position is its matrix translation, set by the scene's timeline (the art is authored in place).

**What gets which anchor** (runtime/aspect.c `scene_policies`, `scene_is_meter`, `root_anchor`):

| scene (object id) | owner | anchor |
|---|---|---|
| 0x36B health meter + gadget gauge | HUD manager 0x3D9E70 +0x10 (0x1D9968 / 0x1D9DF8) | by root: L (top left) |
| boss health meters (level objects, e.g. ep1 Dimitri 0x78A) | boss classes: +0x2A0 (0x17FD08/0x180100), +0x568 (0x1D4940/0x1D4C88), script command 0x10DD08 | by root: R (top right) |
| 0x36D coin counter, 0x36E pick-up counter, 0x36F timed counter | world +0x3BAC/+0x3BB8/+0x3BC4 (0x20F710; shown by 0x161868, 0x1618B0, 0x1618F8; L3 shows them) | by root |
| everything else: ticker 0x371, letterbox + subtitles 0x373, briefings 0x35F-0x36A, pause menu, gadget menus 0x375-0x377, ThiefNet, title / episode menus, level-specific scenes | | C |
| 2D outside scenes (text labels at world points 0x1CD780, dance prompts 0x152C58, gadget button glyphs 0x1DAC58) | | C |

"By root": each root-level object is anchored by the third of the scene's width its origin (tx) is
in (L < 1/3, R > 2/3, else C). Meters are recognised by their content, not by id: a scene with a
"meter fill" sprite instance (id 0x34F or 0x350, what the game's 0x212E40 sets from a health
fraction) - so every boss's meter is found without a list of levels. Origins are only used for
those scenes: in general a root's art can be far from its origin (the ticker's right arrow has tx
188 but is drawn at 489..527).

The anchor travels per render-list entry (allocated while the item is drawn); f_00157FA0's loop
emits HUD_BEGIN(anchor) whenever the anchor changes between 2D entries (no HUD_END in between),
HUD_END when 2D stops. Full-width primitives stay exempt, whatever the anchor.

## Run-time switch (F7 / F8)

F7 switches between the wide aspect and 4:3, F8 between `hud = edges` and `centered` (window.c;
also when sly2.ini's [display] was edited and the window is activated again; PS2_INPUT tokens
`aspect` / `hud`). The request is applied at the top of the game's next frame (`aspect_switch`
patch, after the camera's per-frame update, before anything is drawn), so a frame is built with one
aspect; the camera's X range, projection and frustum planes are recomputed then with the game's own
0x14DB70 (at 4:3 from the game's own numbers). The pictures already built when the switch happens
(at most one or two) are shown with the new display aspect. Movies and loading keep running; the
switch applies at the next main-loop frame.

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

- Binocucom / photo framing: the game's on-screen tests use the 4:3 area (ws_screen_project), so
  framing is judged as on a PS2 while the view shows more at the sides.
- Scenes not listed are centred: a level-specific HUD scene (a job's own counter or timer) at a
  side of the screen that is not a meter stays in the 4:3 area. Add its object id to
  `scene_policies` (PS2_HUD_LOG=1 prints every scene and root-level object with its extent).

## Notes for the GPU renderer (M10)

- Same contract as the sw renderer: transform per primitive at kick, full-width primitives exempt,
  anchor c from XYOFFSET.x and the FRAME width, scale from the hint's value (or the configured A).
- The anchor can change between HUD_BEGINs without a HUD_END (L, then R, then C in one 2D pass):
  each HUD_BEGIN replaces anchor and scale. All three anchors occur in normal play (hud = edges).
- The aspect can change while the game runs (F7): take the display aspect per picture (the sw
  presenter reads ps2_aspect / ps2_settings when it presents) and the HUD scale from each hint,
  never cache them at start-up. After a switch to 4:3 the stream has no hints at all.
- PHASE hints (diagnostics, PS2_HUD_LOG=1 only) label 2D entries; ignore them.
- Hints arrive inside the frame's VIF1 chain (PATH2 DIRECT, an A+D write of register 0x7F), and from
  the movie player as direct writes; both go through gs_write_reg like any register.
- Pillarbox: use the ASPECT_4_3 state at the VBlank (gs_picture_content43 in the sw path).
- 4:3 setting: the stream has no 0x7F writes at all.
