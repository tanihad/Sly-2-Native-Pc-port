# WIP: d3d11-design-high-level (RT64 / GS-HW style translation design)

Role: designer (high-level angle). Branch next/d3d11, worktree C:\Users\tanih\Projects\sly2-worktrees\d3d11.
Inputs: notes/wip/d3d11-map.md, d3d11-census.md, d3d11-prior-art.md, build/census/*.txt.

## Progress
- [x] 0 read principles, census checkpoint, gs_draw.h / hw.h seams
- [x] 1 verified in build/census/ep3-rope.txt: backdrop is ATST NEVER aref 126 AFAIL FB_ONLY, ZTST ALWAYS
      => no Z write while it samples the T8 upload living inside the Z32 pages (no aliasing conflict).
      Shuffle sprites: 8x448 CT16 reading CT16 1024x1024 at +8 px, (0-0)*FIX+Cs, NEAREST, DECAL.
- [ ] 2 full design written here (next commit)

## Key decisions so far
- Fast path: fixed-function D3D11 draws into scaled RGBA8_UNORM targets (dual-source blend so the
  stored alpha is the raw GS byte while the blend factor is As/128).
- Exact path: same draw, but the target bound as a ROV UAV (R8G8B8A8_UINT typed view on RTX 40 /
  R32_UINT always) with the GS pixel pipeline in integer HLSL, chosen per draw by a static rule
  (COLCLAMP=0, partial-byte FBMSK, DATE, Ad/Cd-as-factor, As>128 possible, FIX >128).
- 16-bit views of 32-bit targets (shuffles): exact "alias16" compute pass using the CT16-in-CT32
  halfword map, per sample at scale; T8H/4HL/4HH of a target = alpha byte at the same coords.
