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

PATCHES: list[Patch] = [
    Patch("ws_projection", *CM_RECALC, 0x0014DBA4, "after",
          "if (ps2_aspect.wide) ctx->f[12].f *= ps2_aspect.proj_x;",
          "lwc1 $f12, 0x3C($s0) loads the camera's aspect (1.2987, set by the camera init at 0x14D750) "
          "for the X range (tan(fov/2) * aspect -> +0x40), the near-clip radius (+0x48) and the "
          "projection matrix (0x11C448) - which also drive the view frustum planes (0x14DC58). Widened "
          "by A/(4/3) here, the stored aspect stays the game's own"),
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
