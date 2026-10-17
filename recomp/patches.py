"""Instruction patches: native C inserted at one instruction of a recompiled function.

Native overrides (recomp/overrides.py) replace a whole function; entry hooks (recomp/hooks.py) let a
debug tool see a call. A patch is the smallest change of the three: a few C statements the
recompiler emits right before (or right after) one instruction of the translated function, for
features the PC port adds to the game itself - widescreen (runtime/aspect.c) is the first user.
The game's code stays as it is; nothing in guest memory is modified by poking.

Each patch is guarded like the other tables, by a hash of the containing function's original
bytes [start, end): another executable (another region, the synthetic test ELF) gets no patch.
A patch must leave the game exactly as it was when its feature is off (e.g. 4:3 selected), so the
C code tests the runtime setting first and changes nothing otherwise.

Placement rules (checked by the recompiler, recomp/codegen.py):
- "before": the code runs on every path that reaches the instruction (after its label, before the
  instruction, and for a branch or jump before its delay slot too).
- "after": the instruction must be a plain one (no branch, jump or delay slot); the code runs after
  it, on the fall-through path only.
See notes/recompiler-design.md ("Instruction patches") and notes/widescreen.md.
"""
from dataclasses import dataclass

from .overrides import function_sha1


@dataclass(frozen=True)
class Patch:
    name: str
    func: int          # start of the function that contains `at`
    sha1: str          # of that function's original bytes [start, end)
    at: int            # address of the instruction
    when: str          # "before" or "after"
    code: str          # C statements; `ctx` is the guest context
    reason: str


# ---- widescreen (runtime/aspect.c, notes/widescreen.md); every patch is a no-op at 4:3 ------------

CM_RECALC = (0x0014DB70, "a5e0071f09d2ab6393d169c298376bc420f63da7")     # camera: frustum from FOV/aspect
FRAME_DRAW = (0x00157FA0, "3f04dc1f6453a3e67b0819ad931ccfd5f8c146ed")    # frame: sorted render list -> DMA chain
RENDER_ALLOC = (0x001CD998, "63ce148130315b99fedecd0f519809d70bbf643c")  # allocate a render-list entry
CM_PROJECT = (0x0014F308, "cc5768df6f4e07553979ba8ad0e9d47a88160e3f")    # world point -> screen [-1, 1]
CM_UNPROJECT = (0x0014F268, "a51f27e60bf66bc9db7b3c9a54ff5409f03b8db9")  # screen [-1, 1] -> world point
WORLD_LABEL = (0x001CD780, "ae9904c7f2fc8c1f1c4aef1f72d62aaf10898610")   # text label at a world point
MOVIE_PLAY = (0x001B3BD0, "7eee24abd101fcdd6690561742906d524523e157")    # play one movie (sceMpeg loop)
SCENE_DRAW = (0x00213300, "be7f4567e360dea5be27cff355abccd397b3d7f9")    # 2D scene: draw (vtable method)
DL_QUEUE = (0x0021A998, "67ef1d52df4bab0890b10adcafb8f699de8ba55a")      # 2D: queue a display list's items
ITEM_QUEUE = (0x00215960, "9e5c686306164bd9a4c1fdd5fe87f35fdffea152")    # 2D: queue one placed object
MAIN_LOOP = (0x001A7518, "a1bf2357cbfd44da199858e87f269865b5ea7ea0")     # the game's main loop
MOTION_BLUR = (0x00160F98, "fb6879611634e74a6c18e8c7bda1b2f7eeb43407")   # the motion blur post effect

PATCHES: list[Patch] = [
    Patch("ws_projection", *CM_RECALC, 0x0014DBA4, "after",
          "if (ps2_aspect.wide) ctx->f[12].f *= ps2_aspect.proj_x;",
          "lwc1 $f12, 0x3C($s0) loads the camera's aspect (1.2987, set by the camera init at 0x14D750) "
          "for the X range (tan(fov/2) * aspect -> +0x40), the near-clip radius (+0x48) and the "
          "projection matrix (0x11C448) - which also drive the view frustum planes (0x14DC58). Widened "
          "by A/(4/3) here, the stored aspect stays the game's own"),
    Patch("ws_screen_project", *CM_PROJECT, 0x0014F36C, "after",
          "if (ps2_aspect.wide) ctx->f[4].f *= ps2_aspect.hud_x;",
          "lwc1 $f4, 0x40($s0) loads the camera's X range for 'world point -> screen position in "
          "[-1, 1]' (x = -y / (xrange * depth)). Its callers place 2D things at world points (labels "
          "0x1CD780, markers 0x202D38/0x218220, flying pick-ups 0x1D04E8) and test whether something is "
          "on screen (0x173768 via 0x173588, 0x14AA30). With the 4:3 X range the answer is the game's "
          "own: 2D content is laid out on the 4:3 screen and drawn with the HUD transform, which puts "
          "it on the world point of the wider view; on-screen tests keep the 4:3 area (the community "
          "16:9 patch's 'widescreen bug fix' at 0x14F370 does the same)"),
    Patch("ws_screen_unproject", *CM_UNPROJECT, 0x0014F2B4, "after",
          "if (ps2_aspect.wide) ctx->f[2].f *= ps2_aspect.hud_x;",
          "the inverse of 0x14F308 (screen position -> world point; 0x1D05A0 and 0x2236E8 use the two "
          "together): the same 4:3 X range, so the round trip stays exact"),
    Patch("ws_label_size", *WORLD_LABEL, 0x001CD7E0, "after",
          "if (ps2_aspect.wide) ctx->f[0].f *= ps2_aspect.hud_x;",
          "a text label at a world point is scaled by 1 / (xrange * depth): with the 4:3 X range it keeps "
          "its size next to the 3D object once the HUD transform draws it at 4:3 proportions"),
    Patch("ws_movie_begin", *MOVIE_PLAY, 0x001B3BD0, "before",
          "if (ps2_aspect.wide) aspect_hint_now(GS_HINT_ASPECT_4_3_BEGIN, 0);",
          "entry of the movie player (one movie: sceMpeg decode loop with its own GS output, called "
          "by the playlist 0x1B3B08 from the main loop's top): the pictures until it returns are 4:3 "
          "video, shown pillarboxed. Outside the frame's DMA chain, so the hint goes to the GS directly"),
    Patch("ws_movie_end", *MOVIE_PLAY, 0x001B3EB4, "before",
          "if (ps2_aspect.wide) aspect_hint_now(GS_HINT_ASPECT_4_3_END, 0);",
          "the movie player's only return"),
    Patch("ws_render_alloc", *RENDER_ALLOC, 0x001CD9D4, "before",
          "if (ps2_aspect.wide) aspect_render_entry_alloc(ctx);",
          "jr $ra of the render-list entry allocator: $v0 = the new entry, $ra = who draws it. "
          "aspect.c remembers which 2D entries are scene set-ups (drawn as is) and which are 2D "
          "content (drawn with the HUD transform)"),
    Patch("ws_render_entry", *FRAME_DRAW, 0x00158378, "before",
          "if (ps2_aspect.wide) aspect_render_entry(ctx, 0x002DEA28u, ctx->r[18].uw[0], ctx->r[16].uw[0]);",
          "in the loop over the sorted render list ($s2 = entry, $s0 = its render priority), after the "
          "render class switch and before the entry's DMA CALLs are written into the frame's chain "
          "(DMAS at 0x2DEA28): HUD_BEGIN/HUD_END hints around the 2D entries (priority 21)"),
    Patch("ws_render_done", *FRAME_DRAW, 0x001583FC, "before",
          "if (ps2_aspect.wide) aspect_render_done(ctx, 0x002DEA28u);",
          "after the last render class's end function: close an open HUD_BEGIN"),
    Patch("ws_scene_begin", *SCENE_DRAW, 0x00213300, "before",
          "if (ps2_aspect.wide) aspect_scene_begin(ctx);",
          "entry of a 2D scene's draw method ($a0 = the scene): the render-list entries allocated until "
          "it returns belong to that scene (aspect.c picks the scene's HUD anchor)"),
    Patch("ws_scene_end", *SCENE_DRAW, 0x002137E8, "before",
          "if (ps2_aspect.wide) aspect_scene_end(ctx);",
          "the scene draw method's only return"),
    Patch("ws_item_draw", *SCENE_DRAW, 0x00213408, "before",
          "if (ps2_aspect.wide) aspect_item_draw(ctx);",
          "head of the scene's loop over its queued items ($s0 = the item): its render-list entries get "
          "the anchor of the root-level object the item was queued under"),
    Patch("ws_dl_begin", *DL_QUEUE, 0x0021A998, "before",
          "if (ps2_aspect.wide) aspect_dl_begin(ctx);",
          "entry of 'queue a display list's placed objects' (recursive through nested sprites): nesting "
          "depth, so aspect.c knows the root-level objects (one per HUD widget)"),
    Patch("ws_dl_end", *DL_QUEUE, 0x0021AA54, "before",
          "if (ps2_aspect.wide) aspect_dl_end(ctx);",
          "its only return"),
    Patch("ws_dl_object", *DL_QUEUE, 0x0021A9F8, "before",
          "if (ps2_aspect.wide) aspect_dl_object(ctx);",
          "head of its loop over the placed objects ($s0 = the object)"),
    Patch("ws_dl_queue_mask", *DL_QUEUE, 0x0021AA24, "before",
          "if (ps2_aspect.wide) aspect_item_queued(ctx);",
          "a mask-end object is queued ($a1 = the object)"),
    Patch("ws_item_queue", *ITEM_QUEUE, 0x002159CC, "before",
          "if (ps2_aspect.wide) aspect_item_queued(ctx);",
          "a shape or text object is queued into the scene's item list ($a1 = the object)"),
    # Graphics settings (runtime/gfx.c, notes/settings.md): motion_blur = off is the game's own switch.
    Patch("motion_blur_off", *MOTION_BLUR, 0x00160FA0, "after",
          "if (ps2_motion_blur_off) ctx->r[3].ud[0] = 1;",
          "lw $v1, -0x14DC($v0) loads the word at 0x2DEB24, which the motion blur function (f_00160F98: "
          "previous frame blended into the new one, FIX 48, the 16 32x222 sprites of notes/wip/d3d11-census.md) "
          "tests first: nonzero returns at once (bne -> 0x161218), before any GS work. Nothing in the "
          "game's code writes that word by address (a debug switch); the community 'No Motion Blur' "
          "patch nops this load instead, which leaves $v1 at whatever the caller had. motion_blur = off "
          "makes the load read 1: the game's own off path, nothing else changes"),
    # Not a ws_ patch: it must also run at 4:3 (to switch to a wide aspect); it does nothing unless
    # the player asked for a switch (F7/F8), and a switch to 4:3 leaves the game's own numbers.
    Patch("aspect_switch", *MAIN_LOOP, 0x001A75A8, "before",
          "if (ps2_aspect_pending) aspect_apply(ctx);",
          "main loop, right after the camera's per-frame update (jal 0x14D470) and before anything of "
          "the frame is drawn (0x1CDA80, 0x157FA0): a run-time aspect / HUD switch asked for by "
          "window.c takes effect here, for the whole frame; aspect_apply recomputes the camera's "
          "frustum with the game's 0x14DB70"),
]


def patches_for(prog, start: int, end: int) -> dict[int, list[Patch]]:
    """The patches of the function at start whose bytes are the expected ones, by address."""
    out: dict[int, list[Patch]] = {}
    sha = None
    for p in PATCHES:
        if p.func == start and start <= p.at < end:
            sha = sha or function_sha1(prog, start, end)
            if sha == p.sha1:
                out.setdefault(p.at, []).append(p)
    return out
