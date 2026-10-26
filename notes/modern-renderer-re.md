# How Sly 2 renders its world (RE for a modern high-level renderer)

Research for the "modern renderer" (draw the game's own meshes/materials natively, RT64 / Ship of Harkinian style,
exact GS path as fallback). Evidence, addresses, tools and raw numbers: notes/wip/r2-re.md. Prior art and the graphics
API: notes/wip/r2-api.md (other agent). Addresses: SCUS_973.16 (NTSC-U).

Tools (all debug-only / offline, committed): `scripts/r2_dis.py` (annotated disassembly, callers, data refs, vtable
refs), `runtime/r2_trace.c` + patches `r2_alloc` / `r2_frame` (`PS2_R2_TRACE=<vblank>:<frames>:<dir>`, off by
default, read-only: sorted render list + submitter registers + EE RAM per frame), `scripts/r2_scene.ps1` (trace a warp
scene incl. GS recording), `scripts/r2_frame.py` (offline: walks the frame's VIF1 chain, attributes every tag / VIF
code to its render-list entry, per kind / priority stats, `--dump N`, `--prev` static-vs-moving), `scripts/r2_vu_kicks.c`
(replays a `PS2_VU_RECORD` recording and decodes every XGKICK GIF packet = what each microprogram outputs),
`scripts/r2_texstats.py` (texture working set from a GS recording).

## 1. The 3D data path

```
main loop 0x1A7518 -> RenderSw 0x1CDA80
   objects (SW +0x3F20) -> vtable +0x9C render -> ALO render-all 0x1F3CC8 -> lights 0x1A4478 -> 0x168638 (glob set)
   static world object (vtable 0x2D42FC) -> 0x1B4478 -> 0x1D5808 (4-ary sphere tree, entries written inline)
   particles 0x13A1E8, 0x15DD48, attachments 0x1681F8, portraits 0x169198, 2D 0x2130C0.. (scene system)
      each visible piece: VU0 micro 0x1C0 sphere cull + LOD fade -> per-frame header packet -> render-list entry
-> 0x157FA0: frame globals (camera) -> sort list (0x157AE0) -> per class begin/end + DMA CALLs -> one VIF1 chain
-> render thread 0x160B70 kicks it -> VIF1 unpack -> VU1 microprogram per batch -> XGKICK GIF packets -> GS
```

- **Render list**: 16-byte entries at 0x3B3980 (<= 0x2000, end pointer 0x3D4A50): {priority 0..25, sort key (float:
  distance^2 or fixed), CALL #1 (per-frame header packet, or a whole prebuilt static packet), CALL #2 (the static mesh
  DMA, or 0)}. Priority -> render class (table 0x2BDD20) -> begin/end functions (0x2BDD88) that load the class's VU1
  program (MPG from .vutext) and emit class state.
- **Objects**: ALO (vtable at +0xC, render mask +0x2D, glob set embedded at +0x80, fade +0x12C, light set +0x134,
  children +0x64..+0x70). Glob set: owner, bound sphere, attached objects, and N **subglobs**: 0x28-byte records
  {transform source, local matrix, flags (kind, render class bits 3-7, light/clip/fade options, hidden), static mesh
  DMA, skeleton, warp} + 0x18-byte bounds {centre, radius, LOD distance}. Same family as Sly 1's ALO/GLOBSET/SUBGLOB.
- **Mesh data = prebuilt VIF DMA in level RAM** (e.g. 0x156xxxx, 0xD3xxxx), never rebuilt: per batch (<= ~60
  vertices) UNPACKs into double-buffered VU memory, then MSCAL of a routine. Header per batch: GIF tag template,
  11 strip descriptors (V4-16: vertex ranges, NLOOP|EOP, packet addresses), and **two material packets** (9 qw each:
  2 vertex GIF tags + A+D TEX0/TEX1/CLAMP/TEST/ALPHA/FOGCOL) = pass A (alpha test >= 126, Z GREATER + write,
  (Cs-Cd)*As+Cd) and pass B (no Z write, Z GEQUAL, additive (Cs-0)*As+Cd, **a different CLUT**).
  - Prelit (world, props): STCYCL 4/1, 4 qw/vertex: UV V2-16 (4.12 fixed), colour A V4-5 (RGBA5551) + S-8 alpha,
    colour B V4-5 + S-8, position V4-32 (w = biased int flags, ADC strip restart). The VU outputs one vertex buffer
    and two GIF tags whose REGS place RGBAQ differently (4F12 / 41F2): pass A uses colour A, pass B colour B.
  - Lit / skinned (characters): UV V3-16, normal V4-8, colour V4-8, position V4-32, weights V4-8 (separate 1/1
    stream). Lights per object (EE 0x1A4478, REFS of a 13-qw light set to VU 0x15-0x20: up to 3 directional/point
    lights + ambient), bone palette at VU 0x21+.
- **Per-frame inputs** (header packet, 6..100 qw): world matrix (V4-32 x4 -> VU 0), fade/LOD params (VU 8), light
  set (REFS), near-clip flag (VU 0x12, shadow/clip program), bone palette (VU 0x21+), warp matrices.
  Frame globals: VU 0-1 depth/screen constants (camera near/far), 2-5 world->clip, 6-9 camera->world, 0xA fog.
- **VU1 programs by function** (8 overlays; 0x2E20.. = routine byte addresses):

| program | class / priorities | function | output |
|---|---|---|---|
| 376110651 main | 0, 3 (sky), 11 | prelit meshes 0x2E20/0x2E68/0x2EB0 (+0x2EE8 next batch); lit/skinned 0x2EF8/0x2F80/0x2FC8/0x3358; Z/stencil volumes 0x3628/0x37A8 (prio 13); flush 0x3AB0 | tristrips TME FGE ABE, two passes; untextured ALPHA=Cd strips |
| 188054331 | 1 (prio 9/17/20/24) | **cel outlines**: static winged-edge lists (v0, v1, opposite vertices) -> silhouette test | lines x2 (Z-only, then AA1 RGB) |
| 188054619 "clip" | 6/7 (prio 7/8) | **shadow volumes** with near clipping; also the shadow post passes (0x1AA0 / 0x1C48: CT16 channel shuffles, G-stencil init) | (0-Cs)*128+Cd wrapping strips, DATE; REGLIST sprites |
| 376109499 | 2 (prio 12) | **particles** (VU-simulated from emitter params + per-particle records; Sly 1 "blips") | textured strips |
| 188055835 | 5 (prio 21) | 2D scenes, text, vector art | AA1 line strips, flat strips, sprites |
| 752221467 | 4 (prio 0) | attachments (0x1681F8), ELENG/ERLENG | not seen in the scenes measured |
| 188054331 / VU0 lib | - | VU0 macro: cull 0x1C0, matrix 0x60 | - |

- **Materials/textures**: per batch A+D packets (above): T4 (65-80%) / T8 with CT32 CLUTs, no mipmaps, LINEAR,
  REPEAT/CLAMP. Level textures are **resident in VRAM** (pages 0xA8-0x1FF, uploaded at load; none during gameplay);
  each frame re-uploads a small set from level RAM (0x164C020..: a 1 KB CLUT, 256 KB T8, 12 KB T4) into the **Z-buffer
  pages** (TBP 0x1000..) for the sky, drawn before the Z clear. 91-210 textures (0.5-1.2 MB) and 170-410 CLUTs per frame
  (most textures with two palettes: pass A base and pass B "highlight").
- **Animation**: skeleton on the EE (0x1684D0: per bone local x node world matrix via VU0 macro 0x60, all bones in one
  UNPACK); vertex blending in VU1 with per-vertex 8-bit weights; procedural warps (0x22C028, sin-driven matrices,
  Sly 1 "WR") as up to 4 extra matrices.
- **Camera / culling / LOD / fog**: camera 0x2DDF40 (fov +0x20, aspect +0x3C, ranges +0x40/+0x44, near/far
  +0x4C/+0x50, world->clip +0x140, planes +0xA0); per object and per subglob sphere cull + distance fade on VU0
  (0x1C0 -> fade factor uploaded to VU 8; fading meshes switch pass A to RGB_ONLY); per-vertex fog (FGE, VU computes F;
  FOGCOL per material).

## 2. Classification

| feature | verdict | why |
|---|---|---|
| static world (0x1D5808 tree, world-space prebuilt DMA) | **native** | fully static data, decode once per level into vertex/index buffers |
| prelit props/objects (glob, matrix + fade per frame) | **native** | same format + world matrix |
| two-pass material (alpha-test pass A, additive pass B with second CLUT) | **native** | fixed GS states, reproduce as two draws (or one shader) |
| per-vertex fog, LOD fade | **native** | formula from VU 0/1/0xA constants; verify against VU output |
| textures / CLUTs | **native** | sample the GS VRAM mirror by TEX0/CBP (as the GPU renderer does), or decode+hash-cache |
| skinned characters, lit meshes | **native with effort** | port the VU lighting (light set) and blend semantics; oracle = r2_vu_kicks |
| procedural warps | native with effort | matrices are in the header packet; blend rule from VU |
| sky (prio 1, textures in Z pages, before Z clear) | native with effort | ordering + per-frame texture source in RAM |
| cel outlines | native with effort | edge lists + silhouette on GPU; should move **together with meshes** (Z ties) |
| shadow volumes + G-channel stencil + CT16 shuffles + T8H darken | **exact path** first; native later | relies on GS bit tricks; native = real stencil (removes ~1.3-1.6 ms alias16 cost) |
| prio 13 untextured Z/stencil volumes | exact path | semantics not identified (Sly 1 "murk"/projected volume?) |
| particles (VU-simulated) | exact path; native with effort later | VU physics must be ported |
| prio 0 attachments, portraits (class 8/9) | exact path | rare / unmeasured |
| 2D scenes, HUD, text (prio 21) | exact path | already widescreen-aware; cheap |
| motion blur | native (trivial full-screen blend) or exact | 16 sprites, FIX 48 |
| Binocucom RTTs, ep2 photo copy, FMV | exact path (FMV already native) | target aliasing tricks |

## 3. Measurements (one game frame, release build; table with all columns in notes/wip/r2-re.md)

| scene | entries | prelit (entries / src verts) | lit+skinned | outline / shadow / particles / 2D entries | GS prims | textures / CLUTs |
|---|---|---|---|---|---|---|
| ep1 hub | 894 | 755 / 63.9k | 63 / 8.4k | 13 / 41 / 8 / 12 | 54.3k | 210 (1.16 MB) / 406 |
| ep3 hub (open view) | 606 | 464 / 65.8k | 95 / 12.3k | 11 / 10 / 8 / 12 | (not recorded) | - |
| ep5 hub | 561 | 460 / 51.3k | 46 / 4.6k | 11 / 22 / 4 / 12 | 41.5k | 150 (0.93 MB) / 292 |
| m8 boss | 195 | 110 / 18.2k | 24 / 3.4k | 11 / 6 / 10 / 34 | 24.0k | 124 (0.53 MB) / 226 |
| museum | 393 | 297 / 30.9k | 30 / 3.9k | 11 / 33 / 2 / 12 | 28.0k | 91 (0.80 MB) / 174 |

Prelit meshes are 47-84% of entries and 57-87% of source vertices (hubs: 77-84% / 83-87%); between consecutive frames 85-96% of prelit
entries keep the same world matrix (ep3 96% open view / 85% at a wall, museum 94%); only the moving airship level (m8:
12%) changes most. Per frame: 0.9-2.9 MB of static mesh DMA referenced, 32-67 KB of per-frame packets,
1.3-4.6k MSCALs. So most of the 3D work is static data that only needs a matrix per frame.

## 4. Proposed interception point

**Per render-list entry, in f_00157FA0's emission loop** (patch "before" 0x158378, where `ws_render_entry` already
sits): the entry ($s2), its class and both CALL targets are known; the per-frame header packet is complete in RAM.

1. A classifier keyed by the entry's VU routine set / priority (as r2_frame.py's `kind_of`) decides native vs exact.
   Unknown kinds default to exact.
2. Native entry: copy what the native renderer needs (matrix, fade, light set, bone palette, warp matrices from the
   header packet; the static mesh DMA address = cache key for the decoded GPU mesh) into a native draw list, write a
   side-band GS hint into the chain at the DMAS write position (like `aspect_hint`: NATIVE_DRAW <index>), then **zero
   entry +8/+0xC** - the game's own `beq v0, zero` skips both CALLs, no control-flow change (the list is discarded
   after the frame anyway).
3. Exact entries emit their CALLs as today, in the same sorted order. The GS thread / GPU frontend executes the
   NATIVE_DRAW hint at its place in the stream: flush pending GS draws, run the native batch on the **same colour and
   depth targets** at the same internal scale, continue. Order between native and exact draws is the game's sort
   order, so blending, the shadow pass (which reads scene depth and destination alpha) and 2D work unchanged.
4. Depth/colour sharing contract: native draws write depth in the renderer's exact-Z encoding (z / 2^20 of the GS Z
   the VU would produce, from VU 0/1 constants) and the GS alpha semantics (alpha byte 0x80 / FBA forced bit 7, used by
   DATE in the shadow pipeline). Outlines and pass B depend on Z ties with their mesh: move meshes, their pass B and
   their outlines native together.
5. Validation per entry: VU recordings are an oracle (r2_vu_kicks): decoded native vertices must match the VU's
   XGKICK output (XYZ, STQ, RGBA, fog); in game, an A/B mode renders an entry both ways into scratch targets.
6. Bonus: with static meshes on the GPU and only matrices per frame, the frame-interpolation idea (notes/frame-rate.md
   (b)) becomes cheap - an in-between picture is a re-draw with lerped matrices, no VU/GS re-run.

## 5. Biggest unknowns / risks

- VU lighting formula for lit meshes, the colour scale (hypothesis: RGBA5551 x 8 -> 0..248, 128 = 1.0; outputs 112/120/200 seen), fog formula and the
  fade-dependent TEST/pass variants (TEST 0x53001, 0x5000D, 0x317E1 seen): need reading routines 0x0070, 0x0610,
  0x0298, 0x0D28, 0x2D38 of 376110651 or fitting against r2_vu_kicks output.
- Exact-Z ties between native and exact draws (outlines, decals, shadow volumes vs scene) - float transforms vs the
  VU's truncating FMAC.
- Shadow pipeline needs the destination-alpha / G-channel conventions preserved by native draws.
- Coverage outside the 6 traced scenes (water, RC chopper, hack/turret minigames, Binocucom views): the classifier
  must fall back; prio 13 volumes and prio 0 attachments are unidentified.
- The game writes effect-object pools while a chain runs (5-14% of async jobs, notes/frame-rate.md): copy header data
  at interception time.
- Resident textures' EE-RAM source was not traced (level load path); the plan reads them from the GS VRAM mirror.
