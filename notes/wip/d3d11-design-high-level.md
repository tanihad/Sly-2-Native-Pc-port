# WIP: d3d11-design-high-level (RT64 / GS-HW style translation design)

Role: designer (high-level angle). Branch next/d3d11, worktree C:\Users\tanih\Projects\sly2-worktrees\d3d11.
Inputs: notes/wip/d3d11-map.md, d3d11-census.md, d3d11-prior-art.md, build/census/*.txt.

## Progress
- [x] 0 read principles, census checkpoint, gs_draw.h / hw.h seams
- [x] 1 verified in build/census/ep3-rope.txt: the backdrop is ATST NEVER aref 126 AFAIL FB_ONLY with
      ZTST ALWAYS, so it does not write Z while it samples the T8 upload stored inside the Z32 pages
      (no aliasing conflict). Shuffle sprites are 8x448 CT16 reading CT16 1024x1024 at +8 px,
      (0-0)*FIX+Cs, NEAREST, DECAL.
- [x] 2 full design (below), returned to the orchestrator

---

# Direct3D 11 renderer for Sly 2: high-level translation design

## 0. Thesis

The GS command stream is translated into Direct3D 11 work: GS frame and Z buffers become GPU
render targets and depth buffers at a scaled resolution, game textures are decoded once into a
GPU texture cache, palettes live in a GPU palette atlas, and GS blend and test quirks become shader
permutations. `gs_vram` stays in system memory as the CPU-side copy. It is synchronised with the GPU
only when the EE or a transfer needs bytes that only the GPU has (this never happens in the 13
recordings). The software renderer is kept unchanged as the reference and as the fallback.

Why this angle is right for *this* game:

1. **The vocabulary is small.** The census shows:
   - two CT32 colour buffers, one Z32 buffer, two 128x128 render-to-texture (RTT) targets;
   - exact-base addressing only;
   - no readbacks, no local-to-local copies, no CLUT loaded from drawn memory, no depth sampled
     as a texture, no mipmaps, no region clamp.

   The parts of a render-target cache that took PCSX2 twenty years (offset targets, partial
   overlaps, readback heuristics) are not exercised. Where they would be needed, this design
   detects it and falls back to exact paths instead of guessing.
2. **Scaling and AA come for free from GPU rasterisation.** Triangles rasterised at 4x are really
   at 4x: geometric edges, texture filtering and AA1 lines are drawn at the target resolution.
   A VRAM-emulating renderer only gets this by doing super-sampling bookkeeping itself.
3. **It is the RT64 / Unleashed model the owner pointed at.** Draws become draws, textures become
   textures, and the GPU does the rasterising.

Honest limit: pure fixed-function D3D11 cannot express Sly 2's shadow pipeline. That needs
wrapping 8-bit G-channel arithmetic, sub-byte FBMSK, DATE, Cd*2>>7 and 16-bit views of 32-bit
pixels. The design therefore has three *exact* mechanisms inside the high-level architecture, each
chosen per draw by a static rule (no game-specific hashing):

- **ROV draws.** The target is bound as a rasterizer-ordered UAV and the GS pixel pipeline runs in
  integer HLSL.
- **ALIAS16 dispatches.** These move halfwords between scaled samples using the GS CT16-in-CT32
  address map.
- **Target-as-texture views.** T8H, 4HL and 4HH read a CT32 target's alpha byte directly, because
  their layout is identical to CT32.

All three work per sample at any scale, so the shadows are exact at 4x and are not approximated.

## 1. Integration with the existing runtime (what changes, precisely)

**Unchanged:**
- `hw.h` and `gs_internal.h` (frozen).
- gs.c's privileged registers, CSR, SIGNAL/FINISH/LABEL handling on the EE thread.
- gs_thread.c's SPSC queue (producer side and slot format).
- gs_mem*.c (VRAM, swizzle, CLUT, transfers).
- gs_record.c.
- The whole software path (gs_draw_pixel.c, gs_draw_tex.c, deferred bands).

**New files** (runtime/, owned by the gs-hw task):

| file | role | thread |
|---|---|---|
| `gs_hw.h` | backend interface used by gs_draw.c / gs_thread.c / gs_replay.c | - |
| `gs_hw.c` | frontend: draw classification, vertex/index emission, target cache, page ownership, hazard and feedback decisions; writes a GPU command list | GS thread |
| `gs_hw_tc.c` | texture cache (memory sources, index decode, palette atlas) | GS thread |
| `gs_hw_d3d.c` | render thread: sole owner of the D3D11 immediate context; consumes the command list, Map/Unmap of rings, state objects, Draw/Dispatch/Copy, compose, Present | render thread |
| `gs_hw.hlsl` | VS, PS ubershader with permutations, ALIAS16 CS, compose/resolve PS, load/download CS | - |
| `gen/gs_hw_shaders.h` | bytecode blobs + permutation table, generated at build time | - |
| `window_gpu.h` | small non-frozen interface between window.c and the render thread | - |

**Edits to existing (non-frozen) files:**
- **gs_draw.c**
  - `draw()` (:800): `if (gs_hw_on) { gs_hw_prim(&P, q, kind); return; }` before the software
    rasterisers. `env_update` (:165) still decodes registers into `gs_prim P` (one decoder for
    both backends).
  - `env_update` also calls `gs_hw_state_changed(&P)` when `gs_prim_version` moves.
  - `tex_prepare` (:285) gets a hw branch that reuses its texel-bounding-box code but calls
    `gs_hw_tex_reads(levels, bbox)` instead of `gs_draw_set_reads`.
  - `trxdir` (:892) and `gs_exec_image_write` (:934):
    - `gs_hw_vram_write_begin(pages)` before the CPU write, which downloads GPU-newer pages only
      for a partial overwrite;
    - `gs_hw_vram_written(pages)` after it, which invalidates targets and sources.
  - `gs_exec_image_read`: `gs_hw_download(pages)` before `gs_mem_xfer_read`.
  - `tex0_write` (:956): if the CLUT pages are GPU-newer, `gs_hw_download(clut pages)` first.
    Never true in Sly, but it keeps the code correct.
- **gs_thread.c**
  - Consumer `gs_thread_main` (:216): the idle `gs_draw_kick()` also calls `gs_hw_flush()`, which
    publishes the command list to the render thread.
  - The K_VSYNC slot calls `gs_hw_vblank(snapshot)` instead of `vblank_picture` when hw is on.
    The EE-side wait in `gs_queue_vsync` (:160-170) is kept, with "presented" meaning "the render
    thread has issued Present".
- **gs.c**
  - `gs_display_regs`/`gs_compose` are used only by the software backend and by the hw "reference
    compose" test mode.
  - CSR.RESET (:67) additionally calls `gs_hw_reset()`, which drops the pending vertex queue.
    Targets keep their contents, as VRAM does.
- **ps2_runtime.c**: backend choice:
  - `PS2_GS_BACKEND=hw|sw` (default hw once M4 passes);
  - `PS2_GS_SCALE`;
  - `PS2_GS_ASPECT`;
  - pixel workers are not started in hw mode (8 fewer threads).
- **window.c**
  - In hw mode it no longer creates a device. It exposes `window_gpu_hwnd()`, the client size
    (atomic) and fullscreen state through `window_gpu.h`.
  - Its D3D11 code (`d3d_init` .. `d3d_render`) remains for the sw backend.
  - The render thread creates the swapchain for that HWND and handles ResizeBuffers when the
    client size atomic changes. DXGI allows Present from a thread other than the window's message
    thread. The window thread only pumps messages and input.

**Side-band markers.** gs_thread.c treats register 0x7F (never written by PS2 software; outside
`gs_reg_mirror`'s 0x64 range) as a private PC marker. It is passed only to `gs_hw_marker(value)`.
EE-side hooks in recompiled game code (separate EE work) use it to tag "HUD begin/end",
"post-pass begin" and similar. This is the RT64 gEX* idea: the port tells the renderer what a draw
*is* instead of the renderer guessing.

## 2. Data model

### 2.1 Page ownership (`gs_vram` vs GPU)

Per 8 KB page (512 pages):

```c
typedef struct {
    uint16_t tgt;        /* target id owning GPU-newer data, 0 = none */
    uint8_t  state;      /* PG_CPU (gs_vram valid only), PG_GPU (target newer), PG_BOTH */
    uint32_t cpu_gen;    /* bumped on every CPU write (upload, CLUT, sw draw): texture cache key */
} hw_page;
```

Transitions:

- **GPU draw into target T** marks every page in the draw's page rectangle `PG_GPU(T)`. This uses
  `gs_rect_pages` on the scissored bbox, both frame and Z.
- **Host-to-local upload** marks pages `PG_CPU` and bumps `cpu_gen`. For every target overlapping
  those pages, the pages are recorded in `T.cpu_newer` (8x8-native-pixel block bitmap) as "stale
  on the GPU".
- **Making a stale target region valid again.** Before a draw *reads* the destination of a
  stale-but-uncovered block of T (blend with Cd, Z test other than ALWAYS, DATE, sub-byte FBMSK,
  feedback sampling), the frontend emits `CMD_LOAD(T, blocks)`. A CS decodes those swizzled native
  pixels from a VRAM upload buffer and replicates them to all samples. Opaque writes (no
  destination read, FBMSK 0, ZTST ALWAYS for depth) mark the blocks they fully cover as valid
  without a load. The 8x8 coverage bitmap is 1792 bits for 512x224.
- **CPU needs GPU bytes** (TRXDIR=1, TRXDIR=2 from a target into a non-target, CLUT load from
  target pages, punt draws, partial-page uploads, sw-compose test mode): `CMD_DOWNLOAD(T, pages)`.
  - A CS writes the *native-centre sample* of each scaled pixel (not an average, so integer data
    such as the G stencil and alpha flags survive) into a staging buffer, already swizzled into
    the GS byte layout.
  - The frontend waits on a fence event and copies the result into `gs_vram`. The pages become
    `PG_BOTH`.

Sly 2's per-frame Z-region sequence works with zero loads and zero downloads:

1. T8/T4/CLUT uploads into pages 080-0A3 make those Z32 blocks stale.
2. The backdrop samples the upload from a memory source (pages are `PG_CPU`). It does not touch Z
   (ZTST ALWAYS, ATST NEVER + FB_ONLY; checked in ep3-rope.txt).
3. The 16-sprite ZB_ONLY clear at z=0 is opaque and covers all blocks, so they become valid without
   a load.
4. The scene follows.

### 2.2 Render targets

```c
typedef struct hw_target {
    uint16_t id, base_page;        /* exact FBP/ZBP in pages */
    uint8_t  fbw, kind;            /* kind: T_C32 (CT32/CT24), T_Z32, T_Z24, T_Z16S/Z16 */
    uint16_t w, h;                 /* native size covered so far (grows, page-aligned) */
    float    sx, sy;               /* scale (family scale, see 6) */
    ID3D11Texture2D* tex;          /* C32: R8G8B8A8_TYPELESS (RTV UNORM, SRV UNORM+UINT, UAV UINT)
                                      Z:   R32_TYPELESS (DSV D32_FLOAT, SRV R32_FLOAT) */
    ID3D11Texture2D* shadow;       /* same format: feedback copy target, created on first need */
    uint64_t pages[8];
    uint64_t cov_valid[28];        /* 8x8-block validity for 512x224; general: heap bitmap */
    uint32_t last_frame;
} hw_target;
```

**Lookup key:** `(base_page, fbw, kind class)`. Exact base only. A draw whose FRAME or ZBUF
starts *inside* an existing target at another base, or with another FBW, does not get a heuristic
offset target. It goes to the punt path (section 4.4) and is counted. Sly 2: never.

**Aliasing rules:**
- **CT16 FRAME at a CT32 target's base and FBW** is not a new target. It is an ALIAS16 view of
  that target (section 4.2). These are the shuffle sprites.
- **A different kind at overlapping pages** (Z16S at 070 over Z32 at 070; RTTs at 0A0/0A8 and their
  Z at 0B8 over Z32 pages 0A0-0A7) gets its *own* target.
  - Its first draw marks the overlapped blocks of the other target stale (owner switch).
  - A later read of stale blocks in the other target triggers download-then-load (exact, slow,
    counted).
  - In Sly the main Z is always cleared before reuse, so this costs nothing.
- **Growth:** height is taken from SCISSOR.Y1+1 rounded to the page height. A taller later use
  recreates the target and CopySubresourceRegion's the old content.
- **Eviction:** targets unused for 120 frames with no GPU-newer pages are released. Otherwise they
  are downloaded first.

Z mapping:
- `z_float = z_int * 2^-32` (Z32/Z24) or `* 2^-16` (Z16S) in D32_FLOAT. Z is interpolated linearly
  in screen space like the GS.
- Equal integer inputs give equal floats, so the two-pass mesh trick (pass B ZTST GEQUAL against
  pass A's Z) holds.
- Z values >= 2^24 lose integer precision. M1 measures the actual Z range per recording.
  If GREATER/GEQUAL decisions differ in the lockstep diff, the fix is the ROV-depth variant for
  those draws.
- Z clamp to the format maximum uses an SV_Depth permutation, only when a vertex Z exceeds zmax.

### 2.3 Texture cache (memory sources)

Indexed textures are 95% of textured primitives. They are **never expanded with the palette on
the CPU** (the PCSX2 "GPU palette conversion" lesson).

- **Index atlas.** One `R8_UINT` 4096x4096 texture (16 MB), shelf-allocated.
  - Each source is decoded from `gs_vram` on the frontend thread with the existing gs_mem
    swizzle tables. Decoding is linear indices for T8 and T4 (T4 stored as one index per byte),
    8H/4HL/4HH from memory, and CT16 to R8G8B8A8_UINT.
  - Key: `{TBP0, TBW, PSM, TW, TH, cpu_gen of every page covered}`. The page-gen vector is folded
    into a 64-bit hash, re-validated on lookup.
  - Sly 2 decodes about 2-8 sources per frame (only the per-frame uploads change). The other
    90-350 stay resident.
- **Direct-colour sources** (CT32/CT24/CT16 from memory) go into a second atlas
  `R8G8B8A8_UINT` 2048x2048. TEXA is applied in the shader.
- **Palette atlas.** `R8G8B8A8_UINT` 256 x 1024 (a palette per row).
  - Key: `{CBP, CPSM, CSM, CSA, CLD-load serial of the CLUT bytes}`. The serial is the `cpu_gen`
    of the CBP pages (CLUT pages are only ever uploaded in Sly).
  - That covers the 0.5-7k CLUT binds per frame with 90-350 distinct palettes, and a palette bind
    is just a row index in per-draw data.
  - Rows are filled from `gs_mem`'s CLUT after `gs_clut_on_tex0`, so CSM1 swizzle and CSA are
    already handled by the frozen code.
  - The atlas is uploaded with UpdateSubresource of the changed rows.
- **Filtering.** Point and bilinear are done manually in the shader with GS conventions: 1/16-texel
  UV, 4-bit weights, filtering after the palette lookup, REPEAT/CLAMP per axis. These are
  per-draw data, not hardware samplers.

Because texture, palette, fog colour, AREF, FIX, TEXA and wrap parameters are all **per-draw data
in a structured buffer** (`StructuredBuffer<hw_draw> draws`, indexed by a 16-bit draw id stored in
each vertex), consecutive draws that differ only in those values merge into one D3D Draw.

### 2.4 Vertices

The frontend appends each kicked vertex once to a per-frame ring:

```c
struct hw_vtx { uint16_t x, y;      /* 12.4, XYOFFSET not yet removed */
                uint32_t z;
                float s, t, q;      /* or u, v in 1/16 texel (FST=1) */
                uint8_t rgba[4];
                uint8_t fog, flags;
                uint16_t draw;  };  /* 32 bytes */
```

Strips and fans are turned into a 32-bit index list per kick.

- **Sprites** become 4 vertices + 6 indices with flat colour from v1, and their UV bounding box
  goes into the draw record.
- **Lines** become 4-vertex parallelograms (section 3.5).

VS:
- `pos.xy = ((xy - XYOFFSET)/16 + 0.5) * (sx, sy)` in target pixels. Sample k of native pixel n
  then sits at GS coordinate `n + (k+0.5)/s - 0.5`, symmetric around the integer GS pixel centre.
  For s=1 this is exactly the GS sampling point. D3D's top-left fill rule equals the GS rule.
- Colour uses nointerpolation when IIP=0; Q, S, T are passed for a perspective divide in the PS
  (GS semantics: STQ interpolated linearly, divided per pixel).

Upload per worst-case frame (ep3 rope, 127k vertices): about 4 MB vertices + 0.6 MB indices.
That is negligible on PCIe 4.0.

## 3. Draw classification and permutations

### 3.1 The per-draw path rule (static, from `gs_prim`)

```
if FRAME psm is 16-bit and a C32 target exists at that base/FBW   -> ALIAS16   (4.2)
else if target/texture relation is unsupported                     -> PUNT      (4.4)
else if needs_exact(P)                                             -> ROV       (3.3)
else                                                               -> FF        (3.2)

needs_exact = COLCLAMP==0 && blend can leave [0,255]   // additive/subtractive/factor>1
            || FBMSK has a byte that is neither 00 nor FF
            || DATE
            || C == Ad  || A/B/D uses Cd with C > 128 (FIX>128 or max(As)>128, see below)
            || PABE || DTHE (none in Sly, kept exact)
```

`max(As)` is known per draw. It is computed from the palette max alpha, cached per palette row,
times the max vertex alpha >> 7 for MODULATE (vertex alpha for untextured draws). Draws whose
alpha can exceed 0x80 = 1.0 and whose blend uses As as a factor with Cd go to ROV, because
fixed-function factors clamp at 1.

Sly 2 mapping:
- **FF:** mesh passes A and B, outlines, the backdrop, Z clears, the T8H darken, motion blur, the
  Binocucom composite, HUD.
- **ROV:**
  - shadow volumes (COLCLAMP 0, FBMSK FFFF00FF/80FF/FEFF, DATE);
  - the G-initialisation sprites;
  - the Cd*2>>7 bit move;
  - any mesh chunk whose texture alpha can exceed 128 under (Cs-Cd)*As+Cd.
- **ALIAS16:** the 96 shuffle sprites.

### 3.2 Fixed-function path (dual-source blending)

The RT is RGBA8_UNORM holding raw GS bytes (alpha 0x80 = 128/255). The PS computes the GS colour
pipeline in integers (TFX `(C*T)>>7`, fog `(C*f + Fog*(255-f))>>8` as in `fog_batch`, alpha
test). It outputs `o0` (colour or premultiplied term, and alpha = raw As | FBA) and `o1` (blend
factor in .a = min(C,128)/128):

| GS blend | D3D color blend | exactness |
|---|---|---|
| (Cs-Cd)*C+Cd, C<=128 | SRC1_ALPHA, INV_SRC1_ALPHA, ADD | +-1 LSB (rounding vs GS truncation) |
| (Cs-0)*C+Cd | PS outputs `(Cs*C)>>7`; ONE, ONE, ADD (saturate = COLCLAMP 1) | exact |
| (Cs-0)*C+0 | ONE, ZERO | exact |
| (0-Cs)*C+Cd, COLCLAMP 1 | PS outputs `(Cs*C)>>7`; ONE, ONE, REV_SUBTRACT | exact |
| (Cd-0)*C+0, C<=128 | ZERO, SRC1_ALPHA | +-1 |
| (0-0)*C+Cs / +Cd | ONE,ZERO / ZERO,ONE (or write mask 0) | exact |

- The alpha channel always uses ONE, ZERO: the GS writes As (or As|0x80 with FBA).
- FBMSK with whole bytes maps to RenderTargetWriteMask.
- **ATST + AFAIL:**
  - Constant outcomes (ATST NEVER/ALWAYS) are just state. ATST NEVER + FB_ONLY means Z write off;
    + ZB_ONLY means colour mask 0; + RGB_ONLY means mask RGB and Z write off.
  - A real per-pixel test with AFAIL KEEP is `discard`.
  - A real test with AFAIL FB_ONLY/ZB_ONLY/RGB_ONLY (mesh pass A: GEQUAL 126 FB_ONLY with Z
    write) is drawn as **two D3D draws**: (1) Z write on, discard if the test fails; (2) Z write
    off, discard if it passes.
  - This is PCSX2's scheme. It is exact unless the same draw overlaps itself at a pixel where one
    triangle passes and another fails. The lockstep diff (section 8) will show if that ever
    matters. The `ROV_DEPTH` permutation (depth as an R32_FLOAT ROV, one draw) is the exact
    fallback, switchable per draw class.

### 3.3 ROV path

- **Binding.** OMSetRenderTargetsAndUnorderedAccessViews with no RTV, the DSV, and
  `RasterizerOrderedTexture2D<uint4>` (UAV `R8G8B8A8_UINT` on the C32 target). Typed UAV loads of
  R8G8B8A8_UINT need `TypedUAVLoadAdditionalFormats`; ROV needs `ROVsSupported` (D3D11.3, Windows
  10+). Both are guaranteed on the RTX 4060 Ti (FL 12_2). The caps are checked at start-up.
- **Shader.** The PS ports `write_seg`/`tests_batch` from gs_draw_pixel.c:
  1. Read d.
  2. DATE/DATM on d.a bit 7.
  3. Blend `((A-B)*C>>7)+D` in int.
  4. COLCLAMP clamp or `&255`.
  5. FBA.
  6. `out = (new & ~fbmsk) | (d & fbmsk)` with the *bit* mask.
  7. Write.
- **Depth.** `[earlydepthstencil]` is used when the draw does not write Z or has no shader-side
  discard (the shadow volumes: ZTST GREATER/GEQUAL, no Z write). In that case the hardware depth
  test runs before the shader and nothing else changes. Otherwise the `ROV_DEPTH` permutation
  also binds depth as a ROV.
- **Ordering.** D3D11 orders ROV accesses for overlapping pixels within a draw, and the runtime
  serialises successive draws that write the same UAV. Switching a target between RTV and UAV
  binding is tracked by the D3D11 runtime (implicit hazard sync), so no manual barriers are needed.
- **Scale.** Each scaled pixel runs the exact 8-bit integer pipeline independently. The G-channel
  stencil, its wraparound, the FFFF80FF / FFFFFEFF bit planes and the Cd*2>>7 bit move are
  therefore exact *per sample*, giving shadow edges at internal resolution.

### 3.4 Shader permutation key

```c
typedef union { uint32_t u; struct {
    uint32_t tex    : 3;   /* 0 none, 1 indexed (atlas idx + palette), 2 direct colour,
                              3 target CT32, 4 target alpha-index (8H/4HL/4HH) */
    uint32_t bilin  : 1;
    uint32_t stq    : 1;   /* FST=0 */
    uint32_t tfx    : 2;  uint32_t tcc : 1;  uint32_t fge : 1;
    uint32_t atst   : 3;  uint32_t asub : 2; /* alpha-test sub-pass: 0 none, 1 pass-only, 2 fail-only */
    uint32_t path   : 2;   /* FF_DUAL, FF_PLAIN, ROV, ROV_DEPTH */
    uint32_t aa1    : 1;   uint32_t zclamp : 1;  uint32_t one2one : 1; /* same-sample target fetch */
}; } hw_ps_key;
```

Only `tex`, `path`, `aa1`, `zclamp` and `one2one` are compile-time (`#define`). That gives
5 x 4 x 2 x 2 x 2 = 160 possible, and about 30 actually used.

Everything else is a uniform branch on per-draw data. Blend selectors, FIX, AREF, DATM, FBMSK,
COLCLAMP and FBA are per-draw data read in the ROV path, so the ROV shader count stays at the
`tex` count. Blend, depth-stencil and rasterizer state objects are cached in small hash maps keyed
by their D3D11 descs.

### 3.5 Lines and AA1

GS lines follow the DDA in `draw_line` (:593). Each line becomes a parallelogram along the major
axis, 1 native pixel thick across the minor axis (`s` scaled pixels).

AA1 lines are 2 native pixels thick across the minor axis. Their coverage alpha is
`1 - |d|`, where d is the minor-axis distance (in native pixels) from the ideal line. At s=1 that
is exactly the GS two-pixel coverage pair. At s>1 the same formula evaluated per sample gives a
smooth, correctly thin outline.

The coverage replaces As, as in the software renderer. The Z-only first pass stays aliased, as on
the GS. Sly's outline pair (ZB_ONLY line, then AA1 RGB_ONLY) is FF:
- the first is colour mask 0 with Z write;
- the second is mask RGB, Z write off, SRC1_ALPHA blend.

Outlines are 500-1900 per frame and merge into a few Draws, because state is constant across them.

## 4. Reinterpretation and feedback

### 4.1 Target sampled as a texture

When TEX0's TBP0 page equals a target base with TBW == FBW:
- **CT32/CT24 of a C32 target** (motion blur reads A; Binocucom reads RTTs at 1400/1500): bind
  the target SRV. Sampling is in native texel units, mapped to scaled texels.
  - **`one2one` rule:** the draw is a sprite or flat primitive, du/dx = dv/dy = 1 native texel per
    pixel, nearest or bilinear with a texel-aligned offset, and source scale == destination scale.
    Then the PS fetches `src[dst_scaled_pixel + (uv0 - xy0)*s]`: the same sample of the
    corresponding native pixel.
  - With this, post passes stay sample-aligned at any scale, with no half-pixel hacks. It removes
    PCSX2's "align to native" requirement for Sly. The motion blur's 1-row scissor offset is
    preserved because it is just an integer native offset.
  - Otherwise (RTT 128x128 scaled onto screen geometry) the PS does GS bilinear on the scaled
    texture, clamped to the UV bounding box.
- **T8H / T4HL / T4HH of a C32 target** (the shadow darken reads B's alpha through the CLUT at CBP
  1800): by the GS manual the 8H texel (x,y) *is* the alpha byte of CT32 pixel (x,y). The PS reads
  `.a` through the UINT SRV at the same sample (one2one) and looks it up in the palette atlas row.
  No conversion pass is needed.
- **Any other format of a target** (T8/T4 of a CT32 target, CT32 of a Z target): PUNT. Sly 2:
  never.

**Feedback** (the draw samples the target it writes). D3D11 cannot bind a subresource as SRV and
RTV at once:
- The frontend emits `CMD_COPY_SHADOW(T, rect)`: CopySubresourceRegion of the page-rounded texel
  bbox into `T.shadow`, and the draw samples the shadow.
- The shadow stays valid for later draws while their read rectangles do not intersect the
  "written since copy" 8x8-block bitmap.
- The T8H darken (16 column sprites, each reading only the pixels it writes) needs **one** copy
  per frame. The census "in-place" classification (`tex_prepare` :339) identifies these.
- A draw whose read rect hits blocks written since the copy gets a fresh copy. That is exact GS
  order semantics.

### 4.2 ALIAS16: 16-bit views of 32-bit targets (exact, at scale)

GS fact (gs_mem_swizzle): a CT16 page is 64x64 pixels of 2 bytes and a CT32 page is 64x32 of
4 bytes, both 8 KB. For equal FBW, page n of the CT16 view is page n of the CT32 buffer. Inside a
page there is a fixed bijection:

`m16(x16 & 63, y16 & 63) -> (x32, y32, halfword h)`

It is built at init from `gs_addr16`/`gs_addr32` and stored as an `R16_UINT` 64x64 LUT, packing
x32 (6 bits), y32 (5 bits) and h (1 bit).

An ALIAS16 draw is a run of CT16 sprites. In Sly they are nearest, DECAL, copy blend, any FBMSK,
the texture being a CT16 view of a C32 target. The run is executed by `cs_alias16`:
1. One thread per (destination CT16 native pixel, sample k).
2. Compute the source CT16 texel from the sprite's linear UV (nearest; bilinear goes to PUNT).
3. Map destination and source through m16 and the page arithmetic to (C32 target pixel, h).
4. Read source sample k's halfword.
5. Expand RGBA5551 and run the 16-bit blend + COLCLAMP + FBA in integers, then repack. The GS
   16-bit write mask is derived from FBMSK exactly as `gs_prim_finalize` derives `fmask`.
6. Write `(d & m) | (new & ~m)` into destination sample k's halfword.

Scaled sample k of one native pixel corresponds to sample k of another, so every sub-sample's
green and alpha move exactly. The shuffles therefore keep full internal-resolution detail. PCSX2
handles this case with heuristic "texture shuffle" detection.

**Ordering inside a run.** The frontend checks each sprite's read and write sets on a native
bitmap of (C32 pixel x halfword); 512x224x2 bits = 28 KB, computed on the CPU at native
resolution.
- Sprites in one dispatch must not read anything written earlier in the same dispatch, and must
  write disjoint 32-bit pixels. Otherwise the RMW of two halfwords of one uint could race.
- If a sprite breaks either rule, the run is split into a new dispatch. D3D11 serialises
  successive dispatches on the same UAV.

Expected: shuffle 1 (A <- B) is one dispatch. Shuffles 2 and 3 are one or two dispatches each.
That is 3-6 dispatches per frame instead of 96 draws.

### 4.3 Depth and RTT aliasing in the Z region

Handled by separate targets plus stale-block tracking (section 2.2):
- **Z16S at 070:** after its 16-sprite Z-only clear it is a separate `T_Z16S` target. It is a
  D32_FLOAT DSV with `2^-16` scale and the same family scale as B.
- **Binocucom:** RTTs 0A0/0A8 (FBW 2) with Z32 at 0B8 are three small targets with their own
  square scale. Their 0A0-0A7 pages are marked stale in the main Z, which the next frame's clear
  re-validates.
- **Per-frame texture uploads into 080-0A3** make those Z blocks stale. They are sampled from
  memory sources.

None of this needs data conversion in Sly. If a game path ever reads a stale aliased region, the
exact download-then-load path runs and is counted.

### 4.4 PUNT: the exact escape hatch

For anything outside the rules (offset targets, bilinear ALIAS16, T8 of a C32 target, CLUT from
GPU pages):
1. The frontend flushes.
2. It downloads the affected pages at the native centre sample.
3. It runs the software rasteriser synchronously on the GS thread for that draw (`gs_batch_*`
   with workers off).
4. It marks the pages `PG_CPU` and loads them back into the target (replicated to all samples).

Exact at native resolution, blocky at higher scales, and slow (a GPU round trip). It is counted
in `PS2_GS_HW_STATS` and logged once per signature. **Gate: zero punts per frame on all 13
recordings.**

## 5. EE-visible behaviour and sync points

- **SIGNAL/FINISH/LABEL, CSR, IMR, SIGLBLID:** unchanged. They are executed on the EE thread at
  queue time; CSR reports an empty FIFO and FIELD toggling as today.
- **Local-to-host reads (TRXDIR=1):**
  1. `gs_image_read` -> `gs_sync()` (queue drained, as today).
  2. -> `gs_hw_download(src pages)`: the frontend publishes, the render thread runs the download
     CS, copies to staging and signals.
  3. The frontend Maps (blocks), writes `gs_vram`.
  4. -> `gs_mem_xfer_read`.

  Cost: about 1-3 ms of GPU latency per readback, only when the game does one. None in the
  recordings; the ep2 photo mission is the suspected user.
- **Local-to-local (TRXDIR=2):**
  - target to the same-format target with exact geometry: GPU CopySubresourceRegion at scale, with
    `gs_vram` updated lazily (pages `PG_GPU`);
  - otherwise download the source, run the CPU transfer, then invalidate the destination.
- **Host-to-local:** unchanged CPU path into `gs_vram` plus invalidation. A partial-page upload
  into a GPU-newer page downloads that page first (exact).
- **CSR.RESET:** `gs_sync` + `gs_draw_reset` + `gs_hw_reset`. Targets keep their contents, as the
  GS keeps VRAM.
- **VBlank:** K_VSYNC -> `gs_hw_vblank(priv snapshot)` -> `CMD_PRESENT(snapshot)`. The EE still
  blocks at its next VBlank until the previous picture is presented (`gs_queue_vsync` contract).
  So the game can never run more than one VBlank ahead of presentation, as today. GPU queue depth
  is bounded separately by the swapchain's frame-latency waitable object (max latency 2), which
  the render thread waits on before recording a new frame.

## 6. Resolution scaling, field handling, widescreen, presentation

**Scale families.** A target's scale is fixed at creation:
- **Display-family targets** (FBP/ZBP drawn with the field-rendering setup: display buffers,
  the Z bound with them, Z16S): `sx = S * W`, `sy = 2S` when SMODE2 is INT=1 FFMD=1, else `S`.
  W is the widescreen factor, below.
- **Other targets (RTTs):** `S x S`.
- A target sampled one2one by a target of another family is forced to the sampler's family when
  created (the Binocucom RTTs are not one2one, so they keep S x S).

**Vertical doubling.** Sly renders a full-FOV picture into a 512x224 field buffer, and the PS2
shows every line twice (DISPLAY DH 447, flicker filter). Rendering that buffer at `sy = 2S` gives
true 448*S-line progressive output with no game patch. The flicker filter (circuit 1 = same FBP,
DBY+1, ALP 128) and the field weave are TV artefacts and are not applied.
- The half-line field offset some scenes use (XYOFFSET.Y fraction 8, e.g. the intro's 1936.8)
  would make the high-resolution image jump half a field line every field. In progressive mode
  the frontend clears the 1/2-pixel bit of OFY for display-family targets. This is recorded as a
  per-target flag so the lockstep test mode can turn it off.
- The EE "No-Interlacing" switch (0x240B4C) is optional and not required by this design.

**Compose (`CMD_PRESENT`):**
1. Decode PMODE/DISPFB/DISPLAY from the snapshot (shared decode with `decode_circuit`).
2. Resolve the displayed buffer to a target (by FBP).
   - If its pages are `PG_CPU` (FMV frames uploaded by the IPU path, loading screens uploaded as
     images), compose from a native upload texture instead.
   - Mixed pages: load the stale blocks first.
3. Draw a full-screen pass into an intermediate RGBA8 picture of the target's scaled size,
   cropped to DISPLAY. A generic two-circuit ALP/MMOD blend is used only when the circuits differ
   in something other than the flicker-filter pattern.
4. Resolve to the swapchain back buffer. Use a box or bicubic downsample when the internal size is
   bigger than the window (SSAA anti-aliasing), bilinear when smaller, optional integer scaling.
5. Letterbox to the output aspect.

`letterbox` from window.c (:282) moves into shared code.

**Anti-aliasing.** It is supersampling via S: 2x-4x per axis, with lines and AA1 analytic.
- MSAA is not offered. ROV draws on MSAA targets would need per-sample shading, and the GS
  integer pipeline per sample is already what S gives.
- FXAA/SMAA could be added as an optional post shader later.

**Widescreen**, renderer side (the projection and culling patches in f_0014D6D8 / f_0014F308 /
f_001BE6F0 are separate EE work):
1. The output aspect is configurable (4:3, 16:9, 21:9).
2. Horizontal scale `W = aspect/(4/3)` for display-family targets, so the anamorphically squeezed
   512-wide buffer still has square-pixel density when stretched to 16:9. At S=3, 16:9 this is a
   2048x1344 target.
3. **HUD pinning.** Draws between "HUD begin/end" side-band markers (section 1) get
   `x' = 256 + (x-256)*(4/3)/aspect` in the VS (centre-pinned 4:3). The pinning variant (centre,
   or edge anchors per marker value) is chosen by the EE hook.
   - Without markers the HUD is stretched, as with the PCSX2 patch.
   - Post passes, motion blur and shadows cover the buffer and are aspect-independent.
4. **2D cutscene portraits** are handled by the f_001BE6F0 EE hook.

**Frame pacing:**
- **Rate.** The game's rate comes from the host VBlank timer (59.94 Hz), as today.
- **Swapchain.** The render thread presents with a FLIP_DISCARD swapchain, 3 buffers, a
  frame-latency waitable object and ALLOW_TEARING when supported.
  - On a 60 Hz display it calls Present(1,0).
  - On VRR displays (borderless fullscreen with G-Sync) it calls Present(0, ALLOW_TEARING), which
    gives exact 59.94 cadence.
  - On fixed 120/144 Hz displays it calls Present(1,0) on the latest picture. Judder there is
    inherent without frame interpolation, which is a later option.
- **Unchanged VBlanks.** VBlanks with no new picture (no draw since the last VBlank and identical
  display registers) re-present the previous picture without recomposing.
- **Device lost.** Recreate the device and all targets and reload every page from `gs_vram`.
  GPU-newer content is lost, but the game redraws everything every frame, so the cost is one bad
  frame. After three failures, switch to the software backend (`window_present` path).

## 7. Threading and cost

**Threads in hw mode:**
- EE (unchanged);
- GS frontend (the existing gs_thread consumer: register decode, `env_update`, vertex assembly,
  classification, texture/palette cache, page ownership, command-list writing);
- **render thread** (new; owns the immediate context and swapchain);
- window thread (messages and input only);
- timer;
- audio.

The 8 pixel workers are not started. That frees cores for other agents' instances and for the
NVIDIA driver's own submission thread.

**Command list.** An SPSC ring of fixed records (`CMD_DRAW{first_index, count, ps_key, state
ids, target ids}`, `CMD_DISPATCH_ALIAS16`, `CMD_COPY_SHADOW`, `CMD_LOAD`, `CMD_DOWNLOAD`,
`CMD_UPLOAD_SRC`, `CMD_PALETTE_ROWS`, `CMD_PRESENT`, `CMD_FENCE`), plus three per-frame data
arenas (vertices, indices, draw records, texture bytes). The arenas are uploaded by the render
thread with Map WRITE_NO_OVERWRITE into large dynamic buffers, with DISCARD on wrap.

**Deferred contexts are not used.** A single render thread on the immediate context is simpler,
and NVIDIA's driver already offloads submission. If M8 measurements show submission over 8 ms,
the frame can be split into two deferred-context command lists recorded in parallel; the command
list format allows it.

**CPU estimate per game frame**, worst recording (ep8 boss: 40k triangles, 114k vertices, 10.5k
state changes):
- **Frontend: about 4-6 ms.** Register decode is already part of today's GS thread cost: about
  114k vertices x 6 register writes, roughly 25 ns each, plus classification and 7k cache lookups
  at about 0.2 us each. Rasterisation (most of today's 16.4 ms) leaves this thread.
- **Render thread: about 6-9 ms (estimate).**
  - D3D draws are about 1.3-1.5x the state changes, because of the pass-A two-draw split, minus
    merges where only texture/palette/fog changed: about 12-16k Draws.
  - On average 1.5 state calls per draw at about 0.25-0.35 us each on NVIDIA.
  - This is the biggest uncertainty. M2 has a go/no-go microbenchmark: replay ep8 boss's draw
    list with real state churn and time it.
  - Mitigations, in order:
    - merge pass A sub-passes through ROV_DEPTH (one draw, exact);
    - two deferred contexts;
    - a compute-rasterised path for the dense mesh passes. That last one is a design change
      toward the other proposal.
- **EE thread:** unchanged (about 7.7 ms).

At 60 fps every thread fits in 16.7 ms with margin, and they run in parallel. Today the GS thread
alone is at 16.4 ms.

**GPU estimate (RTX 4060 Ti)**, native 512x224 = 115 kpx:
- **S=2** (sy 4): 0.9 Mpx targets. Mesh overdraw about 3 gives about 3 Mpx FF. Shadow volumes +
  G init are about 2-6 Mpx ROV. Under 2 ms.
- **S=4** (sy 8): 3.7 Mpx targets, about 12 Mpx FF, about 15-40 Mpx ROV. About 4-8 ms.
- **At S >= 4 the ROV shadow volumes dominate.** The optional fast path (M8) turns volume draws
  whose per-draw G delta is +-1 and whose FBMSK is exactly G into stencil INCR_WRAP/DECR_WRAP
  (8-bit wrap matches COLCLAMP 0 exactly), with DATE as a stencil pre-pass. Only the final
  shuffle-2 read uses the stencil, through a stencil SRV in the ALIAS16 CS. That is an
  optimisation behind the lockstep test, not a correctness dependency.

## 8. Shader compilation

- HLSL source: `runtime/gs_hw.hlsl`. Shader models: vs_5_0, ps_5_0, cs_5_0.
- **Build time.** `scripts/hlsl_build.c` is compiled by the existing clang. It
  `LoadLibrary("d3dcompiler_47.dll")` (ships with Windows; nothing is downloaded) and calls
  `D3DCompile` for every permutation in `scripts/hlsl_perms.txt`, emitting `runtime/gen/
  gs_hw_shaders.h` (byte arrays + key table). A compile error fails the build.
- **Run time.** A permutation missing from the table is compiled through the same DLL, as
  window.c already does, and kept in memory. M0 must verify that d3dcompiler_47 accepts
  `RasterizerOrderedTexture2D` in ps_5_0 and that it runs on the 4060 Ti (risk item).
- **ROV fallback** if not available: the "barrier-copy" path. Split ROV-class draws into groups of
  primitives with disjoint bboxes, CopySubresourceRegion the bboxes into the shadow, read Cd from
  the shadow in a plain PS, and use an RTV write with a write mask. This is exact but slower.

## 9. Differential testing

**Lockstep mode** (the main tool), `gs_replay.exe -Backend both -Scale 1`:
- Feeds the same recording to the software renderer (synchronous) and to gs_hw (headless device,
  hardware or WARP).
- At every VBlank, and optionally every N draws or every FRAME change (as `gs_post_probe` does),
  downloads all GPU-newer pages at the native-centre sample and compares them bytewise with
  `gs_vram` per page, per channel and per format (Z compared as integers).
- Reports the first diverging draw index, its state signature (gs_census format) and a crop PNG of
  both. The agent bisects by draw, not by picture.
- Tolerances at S=1:
  - **0** for ROV, ALIAS16, one2one copies, Z, alpha channel and all integer-data channels
    (the G stencil between shuffle 1 and shuffle 3).
  - **+-1 per channel** for FF blends with As/FIX factors (rounding).
  - **+-2** after fog + blend chains.
  - **AA1 line pixels: +-8**, then exact again after the next opaque overwrite.
- Any VRAM byte outside tolerance fails.

**Picture mode**, `-Backend hw -Out dir`, then `compare_frames.py swdir hwdir --tol 2`:
- At S=1 in "reference compose" mode (flicker filter and weave emulated as in `gs_compose`):
  at most 0.2% of pixels differ by more than 2, and none by more than 16.
- At S=2/4 in progressive mode: downsample the hw picture to 512x448 (box) and compare with the
  sw picture. PSNR must be at least 32 dB per frame, and the post-pass alignment check passes:
  the mean signed difference per channel over the frame stays under 0.5, which catches motion
  blur or chromatic offsets.
- `scripts/contact_sheet.py` produces side-by-side sheets for visual review.

**Corpus.** All 13 recordings, every VBlank: fix-perf {rv_prompt, rv_intro, title, brief},
ep3 {hub, recon_int, rope, brief}, ep5 hub_street, ep6 rc_battle, ep7 boss, ep8 {hub, boss}.
They are read in place. New recordings for gaps: ep1, ep2 photo mission, ep4, a loading screen,
an FMV, the pause menu, the memory card save screen.

**Counters** (`PS2_GS_HW_STATS=1`, per frame): draws, D3D draws, merges, ROV draws, ALIAS16
dispatches, shadow copies, loads, downloads, punts, source decodes, palette rows uploaded,
frontend ms, render ms, GPU ms (timestamp queries). Gates: punts = 0 and downloads = 0 on all
recordings.

**Unit tests.** `tests/unit/gs-hw/run.ps1` reuses the scene generators of
`tests/unit/gs-draw/test_gs_draw.c` (fill rule, blend, CLUT, bilinear, alpha test, fog, 16-bit,
perspective, lines, deferred hazards, display) against the hw backend at S=1 with the tolerances
above. It adds new scenes: wrapping G add/sub, FBMSK bit planes, DATE, ALIAS16 shuffle triplet,
T8H-from-target, feedback runs and the Z-region aliasing sequence.

**In game.** `PS2_GS_BACKEND=hw PS2_GS_SCALE=1` versus `sw`, using PS2_DUMP_FRAMES at the same
warp points (scripts/warp.py, live_*.ps1) in every episode hub, a boss per episode, the Binocucom,
the title, the menus and an FMV. Then S=2/3/4 and 16:9 visual passes, and timing with the stats
counters on a quiet machine.

## 10. Milestones

See the structured list. Each milestone ends with a commit on its branch, a checkpoint update and
a passing `tests/run_tests.py` plus the gs-hw unit tests.

## 11. Effort

About 30 agent-days in total (M0 1, M1 2, M2 5, M3 5, M4 3, M5 3, M6 2, M7 2, M8 3, M9 3).
The largest uncertainty is draw-submission cost (M2 gate) and ROV throughput at S >= 4 (M5/M8).
