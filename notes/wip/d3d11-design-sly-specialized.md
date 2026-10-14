# D3D11 renderer design: Sly-2-specialised hybrid (designer checkpoint)

Status: measurements done (probe below), design written. Inputs: d3d11-map.md, d3d11-census.md,
d3d11-prior-art.md, census reports in build/census, plus two new probes run this session.

## New measurements (this run; probes in the session scratchpad, read .gsrec files in place)

1. Shadow counter ops (rope, fix-perf brief, ep6 rc_battle):
   - G init: FBMSK FFFF00FF, ABE=0, flat G = 126, 16 sprites (ZTST ALWAYS).
   - Volumes: FBMSK FFFF00FF, flat (IIP=0) tristrips, (0-Cs)*128+Cd with G = 2 or 254 (i.e. -2 / +2
     mod 256), ZTST GREATER (rope) and (Cs-0)*128+Cd with G = 2/254 under ZTST ALWAYS (Binocucom).
     DATE=1. Steps are always +-2: bit 0 of the counter never changes through the volumes.
   - Bit-plane steps (brief, rc_battle): FBMSK FFFF80FF ABE=0 G=62 (write G bits 0-6 := 62), then
     FBMSK FFFFFEFF (Cd-0)*FIX2>>7 (G bit 0 := G bit 6). 1312 kicks each in rc_battle.
2. Post-pass texel alignment (all FST sprites): UV - XY = (+0.5, +0.5) for the darken (T8H) and
   copies, (+0.5, +1.0) with bilinear for the motion blur (TEX1 0x61), (-7.5/+8.5, +0.5) for the
   CT16 shuffles. Sly samples texel centres: pixel x reads u = x + 0.5.
3. XYOFFSET in rope is constant (1792, 1936): no per-field half-line jitter in gameplay.
4. Blend alpha above 1.0 (probe_alpha: CLUT max alpha x vertex alpha, CLUT contents tracked
   through CT32 uploads), rope:
   - Pass A (Cs-Cd)*As+Cd, ATST GEQUAL 126: vertex A = 128 and CLUT max 128 in 94%; As can exceed
     128 on 0.4% (T4) / 5.4% (T8) of vertex kicks (CLUTs with alpha 245).
   - Pass B (Cs-0)*As+Cd, ATST NEVER: vertex alpha 0..127 (lighting weight); >128 possible on <1%.
     Additive forms are exact in fixed function when the shader premultiplies (see 4.2).

## Full design (deliverable)

# Direct3D 11 renderer for the Sly 2 port: a Sly-specialised hybrid

## 0. Thesis

Sly 2 is not an arbitrary GS program. The 13 recordings show one frame recipe, used in every scene. It has three layers:

1. **Plain draws.** A two-pass textured mesh pass, outlines, a backdrop and a HUD. These are ordinary depth-tested triangles, lines and sprites.
2. **Post-pass sequence.** A fixed set of passes:
   - three CT16 channel moves;
   - a green-channel shadow counter that wraps;
   - a T8H palette darken;
   - motion blur;
   - two 128×128 render targets used only by the Binocucom.
3. **Rare transfers.** Uploads of textures and CLUTs.

Layer 1 maps directly onto fixed-function D3D11. Layer 2 is a short, closed list, and each item has a native D3D11 equivalent that is exact: a hardware stencil buffer, channel-move shaders and render targets sampled as textures. The few operations that fixed function cannot express exactly go to a shader-blend tier, or to the software renderer through a native-resolution download and upload. That fallback is exact by construction, and the verifier makes it visible.

This design keeps the parts of the existing runtime that are already correct and fast:

- the queue;
- register decoding;
- `env_update`;
- vertex assembly;
- the CLUT state machine;
- VRAM as the authority for uploads.

It replaces the 16.4 ms software rasteriser with GPU work. It gets resolution scaling and widescreen from real render targets, not from PCSX2-style upscaling hacks. The reason: every frame-buffer effect Sly uses is recognised and re-expressed in a way that does not depend on resolution.

Measured this run (new, beyond the census; the probes read the .gsrec files in place):

- **Shadow counter ops.**
  - G is set to 126, then the volumes add ±2 mod 256. The steps are never ±1, so bit 0 of the counter never changes in the volumes.
  - The bit-plane steps are "G bits 0–6 := 62" and then "G bit 0 := G bit 6".
- **Texel alignment.** Sly samples texel centres in every post pass: UV − XY = +0.5 for the darken and the copies, (+0.5, +1.0) bilinear for the motion blur, and −7.5/+8.5 for the shuffles.
- **XYOFFSET.** It is constant in gameplay, so there is no per-field half-line jitter.
- **Blend alpha above 1.0.** As > 128 with a Cd-dependent blend is reachable on 0.4–5.4% of pass-A vertex kicks, from CLUTs with alpha 245.

## 1. Where it plugs in (Seam A, plus a render thread)

```
EE thread ──gs_write_reg/gs_image_write──► SPSC queue (gs_thread.c, unchanged)
GS thread:  gs_exec_write_reg → env_update → vertex kick → draw()
               └─ [GPU mode] gpu_translate(&P, verts, n, prim)  (new, gpu_rec.c)
                      ├─ recognisers (gpu_fx_sly.c)
                      ├─ page/target ownership (gpu_own.c)
                      └─ appends to command chunks + vertex arena ─► render thread
Render thread (new, gpu_d3d11.c): owns ID3D11Device/ImmediateContext/swapchain,
               executes commands, downloads for fallback, composes and presents
Window thread (window.c): message pump only in GPU mode
```

### Frozen interfaces

- **hw.h and gs_internal.h:** no change.
- **SIGNAL, FINISH, LABEL, CSR, IMR and FIELD/VSINT:** still run on the EE thread when the write is queued (gs.c is untouched). The EE-visible timing is therefore identical.
- **The only blocking points the game can observe stay the same:**
  - a full queue;
  - `gs_sync` (local→host readback and CSR.RESET);
  - the one-picture-in-flight VBlank back-pressure.

### Internal changes (all in non-frozen files)

- **gs_draw.c.**
  - `draw()` dispatches to `gpu_translate` when `gs_backend == GS_BACKEND_GPU`.
  - `trxdir`/`gs_exec_image_write` also call `gpu_on_upload(pages, rect)`.
  - `gs_exec_image_read` first calls `gpu_download(pages)`.
  - `tex0_write` calls `gpu_on_clut()` after `gs_clut_on_tex0`.
  - `gs_draw_reset` calls `gpu_reset()`.
  - A new `gs_sw_draw_record(const gs_prim*, const vtx*, n, prim)` exposes the existing rasterisers to the fallback.
- **gs_thread.c.**
  - `vblank_picture` emits `OP_PRESENT{priv snapshot}` instead of queueing `picture_run`.
  - `picture_done` is signalled by the render thread after `Present`.
  - New optional slot kind `K_HINT`, written from a new header `gs_hint.h` (for HUD and aspect hints, §8).
- **gs.c.** `decode_circuit` moves into a pure function `gs_display_decode()` in gs_draw.h, so the GPU presenter uses the same decoding. Behaviour is unchanged.
- **window.c.**
  - In GPU mode `window_present` is not used.
  - New `window_gpu.h` exports `window_hwnd()`, `window_client_size()`, `window_fullscreen_state()` and a resize event. The render thread creates the swapchain on that HWND (DXGI allows presenting from a non-window thread).
  - The existing window.c D3D path stays for `PS2_GS_BACKEND=sw`.
- **New files (in runtime/gpu/):** gpu.h, gpu_rec.c, gpu_fx_sly.c, gpu_own.c, gpu_d3d11.c, gpu_present.c, gpu_verify.c and shaders/*.hlsl.

### Why a render thread rather than calling D3D on the GS thread

- The GS thread's floor is register decoding at about 2–4 ms.
- D3D11 submission is about 0.7–1.2 µs per draw. With 2–8k draws that is 2–9 ms.
- Running them in series on one thread would waste the multi-core machine, and the driver would sit on the critical path of every register write.
- Command chunks (64 KB of commands plus 4 MB vertex-arena chunks, SPSC with the same publish and wake pattern as gs_thread.c) stream within the frame, so this adds no frame of latency.
- D3D11 deferred contexts were rejected. They give no benefit over our own command list, they restrict Map to WRITE_DISCARD, and our own list lets the GS thread deduplicate state.

## 2. Resources on the GPU

| resource | format | purpose |
|---|---|---|
| `vram_buf` | 4 MB ByteAddressBuffer (SRV+UAV) | mirror of `gs_vram` for every CPU-owned page: textures, CLUTs, uploads; also the download/upload staging layout |
| `swz_tab` | Buffer<uint> | GS block/column tables per PSM (from gs_mem_swizzle.h) |
| `clut_ring` | StructuredBuffer<uint>, 2048 × 256 entries (2 MB) | CLUT snapshots, content-hash deduplicated |
| `state_buf` | StructuredBuffer<StateRec> per frame (ring) | per-draw state that does not break batches |
| vertex arena | DYNAMIC VB, Map NO_OVERWRITE ring, 16 MB | `gpu_vtx` triangles, plus sprite and line records |
| colour targets | R8G8B8A8_TYPELESS (RTV UNORM, SRV UNORM + UINT) | one per exact (FBP, FBW) CT32/CT24 target, size (64·FBW·kx, rows·ky) |
| depth targets | R32G8X24_TYPELESS (DSV D32_FLOAT_S8X24, SRV R32F and X32_G8 stencil); Z16/Z16S get D16-semantic clamping in VS | one per exact (ZBP, FBW, Z-format) |
| scratch | colour copy of the current target (feedback), 1 per size | channel moves, T8H darken, exact-blend tier |

### Target table (gpu_own.c, GS thread)

```c
typedef struct gpu_target {
    uint16_t bp_page, fbw;        /* exact base (8 KB pages) and width in 64-px units */
    uint8_t  kind;                /* TGT_C32, TGT_Z32, TGT_Z16; CT16 views only via recognisers */
    uint8_t  aspect_class;        /* 0 = square scale, 1 = display (anamorphic in widescreen) */
    uint16_t rows;                /* native rows allocated; grows with copy */
    uint32_t id;                  /* render-thread resource handle */
    uint64_t pages[8];            /* pages covered */
    uint64_t stale[8];            /* pages whose truth is in gs_vram/vram_buf (upload before use) */
} gpu_target;
typedef struct { uint8_t owner_kind; uint16_t target; uint32_t version; } gpu_page;  /* x512 */
```

- **Page ownership.** Each page is owned by either the CPU (`gs_vram` and `vram_buf` hold the truth) or the GPU target that wrote it last.
- **Host→local upload.** It writes `gs_vram` (existing code), queues an `OP_UPLOAD_VRAM` (byte ranges into `vram_buf`, ordered in the context's own stream, so D3D11 orders it against earlier texture reads with no hazard tracking of our own), and sets the covered target pages to stale.
- **A stale target page that is about to be used** (bound and touched by a draw that is not a recognised full overwrite) is refreshed with `OP_UPLOAD_TARGET`. That is a compute shader that unswizzles from `vram_buf` and replicates each native pixel into its kx×ky block.
- **Example: Sly's per-frame T8 upload into the main-Z pages (TBP 0x1080).**
  1. The upload makes those Z pages stale.
  2. The backdrop samples them from `vram_buf`.
  3. The 16-sprite Z clear is recognised as a full overwrite and becomes a `ClearDepthStencilView`.
  4. The pages become valid again, with no upload.
- **Two targets on the same memory** (main Z32 vs. Z16S at 070, main Z vs. the Binocucom RTTs at 0A0/0A8): the last writer owns the pages and the other target's pages go stale. If a stale page is ever needed in a different format, it goes through `vram_buf`. The owner is downloaded at native resolution in GS layout, then uploaded into the other target. Format reinterpretation therefore always goes through the real GS memory layout, never through a special-case conversion shader (lesson 1 of the prior art). Sly never needs it, because each aliased target is cleared before use.

## 3. Coordinates and resolution scaling

- **Scale factors.** The internal scale is (kx, ky) per target.
- **Vertex shader mapping.** It maps a GS window coordinate (after XYOFFSET, 12.4 exact) to D3D pixels as `d3d = (gs + 0.5) · k`, with w = 1. Depth is `z · 2⁻³²` (reverse-Z semantics: ZTST GREATER and GEQUAL map to D3D GREATER and GREATER_EQUAL).
- **Interpolation.** Colour, STQ, UV and fog are `noperspective`, because the GS interpolates linearly in screen space and divides S/Q and T/Q per pixel. That is exactly what the pixel shader does.

Consequences:

- **At k = 1 coverage matches the GS exactly.**
  - The GS samples pixel x at integer x, and D3D samples at x + 0.5.
  - The vertices land on D3D's 1/256 sub-pixel grid exactly.
  - Both use the top-left fill rule.
  - Sprites [X0, X1) map the same way.
- **At k > 1 the sub-samples of native pixel x are centred on it:** x − 0.5 + (j + 0.5)/k.
  - This is the right convention for Sly specifically, because Sly samples texel centres (u = x + 0.5).
  - A layout-identical render-target read then hits upscaled texel x·k + j exactly, so the darken, the copies and the Binocucom composite stay pixel-aligned at any scale. PCSX2 needs Half Pixel Offset mode 5 for Sly for exactly this reason. Here it holds by construction.
  - The motion blur's v + 1.0 bilinear sample becomes "previous frame shifted down half a native row", which is its native meaning, now at high resolution.
  - For odd k, the middle sub-sample sits exactly at native x. That gives the verifier a decimated native image (§10).
- **Field targets.** They are 224 rows shown on 448 lines. The user's setting N× means an N·512 × N·448 picture: kx = N and ky = 2N for the field targets. The game keeps rendering in field mode, the GPU renders every frame at full vertical resolution, and the PMODE flicker filter (EN1+EN2, DBY 0/1, ALP 128) is not reproduced in PC mode. No "No-Interlacing" EE patch is needed.
- **Lines.**
  - A line is expanded in the VS (from a 2-vertex record, SV_VertexID) into a parallelogram one native pixel wide across the minor axis. Its coverage reproduces `draw_line`'s DDA (pixel (x, round(y(x)))) at k = 1.
  - AA1 computes analytic coverage in native units (1 − |d| over two native pixels, which is what the GS writes as alpha).
  - At k > 1 the outline stays one native pixel wide and is smooth, not jagged. PCSX2's outline breakage comes from scaled lines, and that cannot happen here. A user option "outline width" scales the half-width.
- **No mipmaps** (Sly uses none), so there is no LOD-related scaling issue.

## 4. The common path: fixed-function D3D11

### 4.1 Batching model

Per-draw state that would break batches in a naive port is moved into `StateRec`, indexed per vertex. Only D3D pipeline state splits D3D draws.

```c
typedef struct {            /* gpu_vtx, 32 bytes */
    float    x, y;          /* GS window coords, native px */
    uint32_t z;             /* GS Z */
    uint32_t rgba;          /* vertex colour, A 0..255 with 0x80 = 1.0 */
    float    s, t, q;       /* STQ; UV in texels with q=1 when FST=1 */
    uint32_t fog_state;     /* fog<<24 | StateRec index */
} gpu_vtx;
typedef struct {            /* StateRec, 64 bytes */
    uint32_t tex0_lo, tex0_hi, clamp_lo, clamp_hi, tex1, texa;
    uint32_t clut_slot;     /* clut_ring index */
    uint32_t src;           /* 0 none | 1 vram_buf | 2 RT SRV slot | view: CT32/CT24/T8H/T4HL/T4HH */
    uint32_t fogcol, test, blend, flags, fbmsk, aref_split, rt_k, pad;
} StateRec;
```

The pipeline key decides which D3D state objects are bound; a change of key starts a new D3D draw:

```
blend id (6b) | colour write mask (4b) | depth func (2b) | depth write (1b) |
stencil program (6b) | prim class tri/line/aa1 (2b) | RTV/DSV gen | RT-SRV gen | scissor gen
```

- All blend and depth-stencil state objects are created at init (D3D11 deduplicates them anyway).
- Texture, CLUT, fog colour, TFX/TCC, the alpha-test reference and TEXA changes are only a new `StateRec`.

The census shows this matters:

- A CLUT reload alone is 11–42% of all state changes, and TEX0/FOGCOL another large share. All of those merge into one draw.
- The remaining splits come mostly from the pass-A/pass-B alternation, which changes blend and depth state.
- Estimated D3D draws per frame:
  - about 1.5–4k in typical gameplay;
  - about 6–8k in ep8 boss (10.5k state changes, 42% of them CLUT-only).

An optional optimisation (M7) is commutation batching: a draw may join an earlier batch with the same key if its bounding box does not intersect anything drawn since that batch. Disjoint pixels make the reordering exact.

### 4.2 Blend mapping (exact for Sly's table)

The shader computes the GS term `T = (X·C) >> 7` in integers and outputs `T/255`. That is exact in UNORM8 when T ≤ 255; otherwise it is clamped, which is equivalent because the final result clamps anyway. Dual-source output is not needed for these forms.

| GS form | D3D11 | exact? |
|---|---|---|
| (Cs−0)·C+Cd | o = min(T,255); ONE/ONE ADD | yes, any C incl. As>128 |
| (0−Cs)·C+Cd | ONE/ONE REV_SUBTRACT | yes |
| (Cs−0)·C+0, (0−0)·C+Cs | blend off, o = T or Cs | yes |
| (Cd−0)·C+0 | Dest=SRC1_ALPHA (C/128), Src=ZERO | ±1 LSB (only used on G: stencil path) |
| (0−0)·C+Cd, (Cs−Cs)·As+Cd | colour write mask 0 | yes |
| (Cs−Cd)·C+Cd, C ≤ 128 | dual-source SRC1_ALPHA / INV_SRC1_ALPHA | ±1 LSB (float vs >>7 truncation) |
| (Cs−Cd)·C+Cd, C > 128 possible | **exact tier** (4.4) | yes |

- **When "C > 128 possible" is decided.** It is decided per draw on the GS thread from max(CLUT alpha of this snapshot, or texture alpha bound) × max(vertex alpha) >> 7, or from FIX > 128. Each CLUT slot stores its max and min alpha when it is created.
- **Alpha channel.** It is never blended on the GS: SrcBlendAlpha = ONE, DestBlendAlpha = ZERO, with FBA ORed in the shader. Destination alpha bytes are therefore always exact integers, which DATE and the T8H darken rely on.
- **COLCLAMP=0** with a form that cannot overflow (copies) is treated as COLCLAMP=1. With a form that can overflow, outside the recognised G counter, it goes to the exact tier.

### 4.3 Tests

- **ZTST.** Maps directly to the D3D depth func; ZTE is always on in Sly.
- **ATST + AFAIL:**
  - ATST NEVER with FB_ONLY, ZB_ONLY or RGB_ONLY is a pure write-mask selection (exact).
  - ATST X with KEEP is a `discard` (exact).
  - ATST X with FB_ONLY (pass A: GEQUAL 126) needs colour always written but Z written only where the test passes.
    - The GS thread checks the draw's As range (from CLUT min/max × vertex min/max). If every texel passes, it is one draw with Z write; if every texel fails, one draw without.
    - Otherwise the draw is split into an "opaque" D3D draw (Z write, discard As < AREF) followed by a "translucent" draw (no Z write, discard As ≥ AREF).
    - The split reorders pixels only where a translucent texel and a later opaque triangle of the same small draw overlap. That is measurable (§10), and an exact variant groups triangles whose bounding boxes do not overlap (the same grouping as 4.4).
- **DATE:**
  - On the shadow volumes it is a PS `discard` that reads the colour target's alpha bit 7 as an SRV. No colour channel is bound, because G is virtual (§5.3).
  - Elsewhere, when the draw provably cannot set alpha bit 7 (vertex/texture alpha bound < 128 and FBA = 0), a stencil pre-pass copies alpha bit 7 into stencil bit 0 over the bounding box and the draw uses a stencil test.
  - Otherwise it goes to the exact tier.
- **Fog, TFX and TEXA** are in the shader in integer arithmetic, as PCSX2's tfx.fx does: `(C·T)>>7`, fog `(F·C + (255−F)·FOG)>>8`.

### 4.4 Exact tier (shader blend from a feedback copy)

Used for:

- (Cs−Cd)·As+Cd when As can exceed 128;
- wrapping COLCLAMP=0 outside the shadow counter;
- partial-bit FBMSK outside the recognised effects;
- DATE when the draw can change alpha bit 7.

How it works:

1. The draw's triangles are greedily grouped so that no two bounding boxes in a group overlap.
2. For each group, the render thread copies the group's bounding box from the target into the scratch copy (`CopySubresourceRegion`).
3. It draws with `PS_GS_EXACT`. This shader reads Cd from the copy, computes the full GS pixel pipeline in integers (((A−B)·C)>>7 + D, clamp or wrap, FBMSK bitwise, FBA, DATE) and writes with blending off.
4. Hardware depth and stencil stay in use, so the AFAIL split still applies.

Expected volume: 0.4–5% of pass-A draws, a few hundred copies per frame. If the number of groups ever explodes, the verify log shows it. Plan B for colour-only cases is a RasterizerOrderedTexture2D variant (RTX 40 supports ROV), but ROV cannot replace pass A because the depth write must stay conditional (§12).

### 4.5 Textures

- `PS_GS` samples straight from `vram_buf`, using the GS swizzle and the GS rules:
  - CT32, CT24 and CT16 with TEXA;
  - T8 and T4 through `clut_ring[clut_slot]`;
  - the T8H, T4HL and T4HH views;
  - REPEAT, CLAMP and REGION wrapping in integer texel space;
  - bilinear with GS 4-bit weights, so the software renderer and the GPU agree.
- **Why this is coherent by construction.** Uploads and draws run in one ordered D3D11 stream, so a texture cache needs no invalidation. Sly's 0.5–7k CLUT binds per frame cost one integer each.
- **CLUT snapshots.** The GS thread snapshots the 1 KB CLUT (gs_mem_tex.c `clut[]`, through the frozen `gs_clut_lookup`) after each TEX0 load that changes it. The snapshot is deduplicated by content hash into a 2048-slot ring with an LRU; ~300 distinct CLUTs per frame mostly hit.
- **A texture whose TBP/TBW matches a live colour target with a layout-identical view** (CT32, CT24, T8H, T4HL, T4HH: all read the same 32-bit words) samples the target's SRV at high resolution:
  - Sly's darken (T8H of B into B);
  - the motion blur (CT32 of A into B);
  - the Binocucom composite (CT32 from the 0A0 and 0A8 targets).

  If the texture is the currently bound target, the bounding box is copied to scratch first (one 512×224-equivalent copy for the darken). Layout-changing views of a live target (CT16, T8, T4 of a CT32 target) are only allowed through the channel-move compiler (§5.2); otherwise the pages are downloaded and the texture is sampled from `vram_buf` at native resolution (exact).
- **Performance option (M7).** An index atlas: T4/T8 textures decoded once per page version into an R8_UINT atlas, so the shader uses `Gather` (one fetch for 4 indices) plus 4 CLUT loads.

## 5. Recognised Sly effects

### 5.1 Tiled fills to clears

- **What matches.** Consecutive untextured flat sprites with the same state, no blend, ZTST ALWAYS, and columns of 8, 16 or 32 px that tile a rectangle.
- **What they become.** Each run is accumulated. When the union covers a target, it becomes `ClearRenderTargetView` / `ClearDepthStencilView` for full-channel writes, or one quad otherwise.
- **Effects covered.**
  - the Z clear (z = 0, ZB_ONLY);
  - the Z16S clear;
  - the alpha reset to 0x80 (alpha-only write mask);
  - the G = 126 initialisation, which is applied to real G as an ordinary masked write (§5.3).
- **Ownership.** A full overwrite makes stale pages valid without an upload.

### 5.2 Channel-move compiler (the CT16 shuffles)

This avoids a per-game pattern table. The recogniser derives the meaning of a CT16-view sprite group from the GS address maths:

1. **Preconditions.** FRAME PSM CT16 on the pages of a live CT32 target with the same FBW; TEX0 CT16 on the pages of a live CT32 target; nearest; TFX DECAL or the copy blend (0−0)·FIX+Cs; flat sprites whose UV − XY is constant.
2. **Bit-level description.** The compiler evaluates `gs_addr16`/`gs_addr32` over one 64×32 CT32 page and turns FBMSK into a 16-bit mask. FBMSK applies to the 8-bit expanded colour, so 00003FFF → halfword bits 8–15 and 7FFFBFFF → bits 8 and 15. The result is: dest 32-bit pixel bits ← source 32-bit pixel (x + dx, y + dy) bits, with a shift.
3. **Validity.** It verifies that the mapping is identical for every pixel of the page and that the 5551 round trip is lossless (TEXA TA0 = 0, TA1 = 0x80, AEM 0).
4. **Caching.** It caches the result by signature (FBMSK, offsets, bases).
5. **Coverage.** It accumulates the group's sprites until they cover the region the mapping needs: 32 sprites of 8×448 for a 512×224 page set.
6. **What it emits.** One `OP_CHANNEL_MOVE{dst, src, dx, dy, dst_bitmask, src_shift}` full-screen pass at high resolution: `PS_CHANNEL_MOVE` loads source and destination as uint, computes `(d & ~m) | (rot(s) & m)` and writes UNORM, so the result is exact.

For Sly this derives exactly what gs_post_probe measured:

- shuffle 1: A.alpha ← B.G;
- shuffle 2: B.alpha bits 0 and 7 ← B.G bits 0 and 7;
- shuffle 3: B.G ← A.alpha.

A group that does not match, or stops partway, is replayed through the exact fallback (§7). This is safe because the sprites are buffered until the group is complete; it is at most 32 sprites.

### 5.3 Green-channel shadow counter in the stencil buffer

- **Problem.** The counter lives in B.G and is updated with wrapping adds of ±2, masked bit-plane writes and a bit move. D3D11 colour blending cannot express wrapping, and write masks cannot select single bits.
- **Mapping.** The stencil buffer of the bound D32S8 target can, provided the counter is stored rotated: stencil = ROR8(G, n), where 2ⁿ is the step (n = 1 for Sly). The supported operations are listed below, for n = 1.

| GS op (FBMSK, blend, flat G = c) | stencil program |
|---|---|
| FFFF00FF, write c | REPLACE ref ROR8(c,1), mask FF |
| FFFF00FF, (0−Cs)·128+Cd, c=2 / c=254 | DECR_WRAP / INCR_WRAP, mask 7F (wraps mod 128 in bits 0–6, i.e. G mod 256; bit 7 = G bit 0 untouched) |
| FFFF00FF, (Cs−0)·128+Cd, c=2 / 254 | INCR_WRAP / DECR_WRAP, mask 7F |
| +128 | INVERT, mask 40 |
| FFFF80FF, write c | REPLACE ref ROR8(c,1), mask BF |
| FFFFFEFF, (Cd−0)·2>>7 (G0 ← G6) | two passes: test (s & 20) == 20 → REPLACE ref A0 mask 80; test (s & 20) == 0 → REPLACE ref 00 mask 80 |

- **Entering the mode.** It is entered lazily at the first wrapping G-only op on a CT32 target whose DSV is bound. That fixes n. The render thread first fills the stencil from the real G channel, rotated (8 bit passes: discard if the bit is clear, REPLACE with a 1-bit write mask, because D3D11 on NVIDIA has no SV_StencilRef export). Until the mode is entered, G writes (the 126 init) are ordinary masked colour writes. That is exact, and the PS2 also overwrites real G here.
- **Draws while in the mode:**
  - Volumes are drawn with no RTV, the main DSV with depth test and no depth write, the stencil program above, and `PS_DATE_ONLY`.
  - Reads of B.G by a recognised channel move come from the stencil SRV (rotated back).
  - Writes to B.G by shuffle 3 overwrite the virtual G completely, so the mode is left without materialising.
- **Leaving the mode** (exact materialisation) happens on any other read or write of B.G, a ZBUF change, a download, a present or the end of the frame. One full-screen pass writes G = ROL8(stencil, n), with a G-only write mask and a read-only DSV.
- **Anything unsupported** (Gouraud G, a step that is not ±2ⁿ, a mixed n) materialises first, then goes to the exact fallback.
- **Result.** Sly's shadows become a real stencil shadow-volume pass, exact including wraparound at any internal resolution. The counter is per high-resolution sample, so shadow edges are sharp at 4×.

### 5.4 T8H darken, motion blur, Binocucom

These need no special code. They are plain draws with a render-target texture source (§4.5):

- The darken samples the scratch copy of B's alpha byte, which is exact after the shuffle-2 channel move, and looks it up in CLUT 1800.
- The motion blur samples A's high-resolution SRV with bilinear filtering.
- The Binocucom RTTs are ordinary 128×128 targets (kx = ky = N, square) with their own Z32.

### 5.5 Phase tracking (for HUD and verification)

A per-frame phase machine is driven by the effects above:

```
SCENE → SHUFFLE1 → SHADOW → RESOLVE (shuffle2 / shuffle3 / darken / alpha reset) → OVERLAY → (next VBlank) MOTION_BLUR
```

It labels draws for the widescreen HUD heuristic (§8) and for per-phase diff statistics. If the phase machine gets confused, nothing goes wrong: it only affects labelling.

## 6. Display, presentation and frame pacing

`OP_PRESENT{priv[16]}` is handled on the render thread:

- **Standard Sly display.** Field mode; circuits reading FBP 000 or 038 with DBY 0/1 and ALP 128; the target is live. The colour target is presented as a progressive picture.
  - Final pass into the swapchain: box/Lanczos-2 downsample when internal > output (SSAA), Catmull-Rom when upscaling, point when 1:1.
  - The picture is letterboxed or pillarboxed to 4:3, or filled to 16:9 in widescreen mode.
- **Target pages that are CPU-owned** (uploads straight into the frame buffer: loading screens or movies that blit) are uploaded and presented at native resolution.
- **Any other register combination:** the display pages are downloaded and composed with the existing `gs_compose` on the CPU, which is exact, then uploaded and presented.

Swapchain:

- created on `window_hwnd()`;
- FLIP_DISCARD, 3 buffers, `SetMaximumFrameLatency(1)` plus a waitable object;
- ALLOW_TEARING when supported (for VRR or uncapped).

Pacing:

- The EE VBlank stays on the host 59.94 Hz timer by default, and the one-picture-in-flight back-pressure is unchanged. `picture_done` fires when `Present` returns.
- Optional `PS2_VBLANK=display`: when the monitor refresh is within 0.5% of 59.94/60 Hz, the waitable object drives the EE VBlank. This removes the 1-frame-per-16-s judder, as PC ports do.
- High-refresh monitors show each game frame once and are not interpolated.
- Device lost: recreate the device, mark all target pages stale, and recover them from VRAM, which costs one native-quality frame.

## 7. Exact fallback (anything unrecognised)

`gpu_fallback(draws, read_pages, write_pages)` runs on the GS thread:

1. Flush the command chunk.
2. `OP_DOWNLOAD(pages)`: CS_DOWNLOAD reads each GPU-owned page's target, writes native GS words (swizzled) into `vram_buf`, then copies to staging and maps it.
   - Colour is the box average of the k×k block. At odd k the centre sample is used instead, so that k = 1 or 3 is bit-exact.
   - Z is the centre sample, converted ·2³².
   - Stencil-virtual G is materialised first.
3. Wait on the fence (expected 0.5–2 ms) and memcpy into `gs_vram`.
4. Run the draws through `gs_sw_draw_record` (the existing rasteriser, bands and `gs_draw_sync`).
5. The written pages become CPU-owned and their target pages stale. The next GPU use uploads them replicated, so that region loses high-resolution detail; this is logged.

The same mechanism serves:

- TRXDIR=1 readbacks (the EE waits through `gs_sync` as today and gets native data);
- a CLUT loaded from drawn pages;
- TRXDIR=2 copies involving GPU pages, except exact-rectangle copies between compatible targets, which use `CopySubresourceRegion` scaled;
- CSR.RESET (`gpu_reset`: drop the targets).

`PS2_GPU_FALLBACK=sw|skip|abort` and a per-frame fallback counter are shown in the overlay/log. The goal from M3 on is zero fallbacks on all 13 recordings.

## 8. Widescreen: what the renderer must provide

The projection patch is EE work: f_0014D6D8 (projection X), f_0014F308 (frustum/cull), and f_001BE6F0 (portraits, which the renderer HUD handling makes unnecessary). The renderer provides the following.

1. **Anamorphic display targets.** Targets with `aspect_class = display` (bases seen in DISPFB, i.e. 000 and 038) get kx = N · A_out/(4/3) and ky = 2N (field targets). That is, 512 GS units span the full-width output. Example for 1440p 16:9: kx = 5.0, ky = 6.43. Off-screen targets such as the Binocucom RTTs stay square. GS scissor (0..511), FRAME and every full-screen post pass are untouched and automatically cover the full width. The coordinate convention works for any real k.
2. **HUD at 4:3 proportions.**
   - The VS applies x' = c + (x − c)·(4/3)/A_out (c = screen centre in GS units) to draws classified as HUD, optionally anchored left, centre or right.
   - Classification comes first from EE hints: `gs_hint(GS_HINT_HUD_BEGIN, anchor)` / `END`, inserted by recompiled HUD functions as `K_HINT` slots (RT64's gEX approach).
   - Until hints exist, a phase heuristic is used: draws in phase OVERLAY that are sprites, FST=1, ZTST ALWAYS or use Z16S, excluding the recognised full-screen passes.
3. **Pillarbox policy.** `GS_HINT_ASPECT_4_3` for content that must stay 4:3 (movies, menus if desired); the presenter then pillarboxes that frame.
4. **Backdrop.** The full-screen T8 backdrop is stretched horizontally. That is acceptable, or the EE can widen its texture coordinates.

## 9. Shaders and HLSL compilation

- **Build-time tool.** `tools-src/hlslc.c`, built with llvm-mingw clang, `LoadLibrary("d3dcompiler_47.dll")`, `D3DCompile`, with an include handler. It turns `runtime/gpu/shaders/*.hlsl` into `build/shaders/*.h` byte arrays (vs_5_0, ps_5_0, cs_5_0, OPTIMIZATION_LEVEL3, /WX).
- **Build script.** `scripts/build_shaders.ps1` runs it as part of the normal build. Nothing is downloaded: d3dcompiler_47 ships with Windows 10/11.
- **Development.** `PS2_GPU_SHADER_DIR` compiles at run time from source for iteration, using the same DLL.
- **Shader set:**
  - **VS:** `VS_TRI`, `VS_SPRITE` (expands 2-vertex records), `VS_LINE`/`VS_AA1` (parallelogram), `VS_FULL`.
  - **PS:** `PS_GS` (the ubershader: per-draw-uniform branches on StateRec for texture kind, filter, TFX/TCC, fog, ATST/split side, FBA, render-target source), `PS_GS_EXACT`, `PS_DATE_ONLY`, `PS_STENCIL_BIT`, `PS_MATERIALIZE_G`, `PS_CHANNEL_MOVE`, `PS_PRESENT_{POINT,BOX,CUBIC}`.
  - **CS:** `CS_DOWNLOAD_{C32,Z32}`, `CS_UPLOAD_{C32,Z32,Z16}`, `CS_INDEX_ATLAS` (M7).
- **No run-time permutation compilation,** hence no stutter. Branches are uniform within a D3D draw except across StateRecs merged into one draw, where they are coherent per triangle.

## 10. Differential testing

- **Replay harness.**
  - `gs_replay.c` gains `-Backend sw|gpu|both`, `-Scale N`, `-Aspect 4:3|16:9` and `-Adapter hw|warp`.
  - In `both` mode the software and GPU backends consume the same stream. At each VBlank it writes `frame_N_sw.png`, `frame_N_gpu.png` and a diff heat map, plus `metrics.json`:
    - per channel, pixel counts with |Δ| > 0, 2, 16;
    - PSNR;
    - bounding box;
    - fallback count;
    - exact-tier groups;
    - draw count.
  - `scripts/gpu_diff_all.ps1` runs all 13 recordings and checks them against `tests/gpu/baseline.json`.
- **Scales.**
  - k = 1 is compared directly.
  - k = 3 is compared on the decimated centre sub-sample (the same sampling points as native).
  - k = 2 and 4 are smoke-tested (no fallbacks, PSNR against a box-downsampled image).
- **Tolerances at k = 1:**
  - **Exact classes, which must be 0 diff:** alpha bytes after each recognised post step (checked at effect checkpoints by downloading B and comparing it to the software renderer's `gs_vram` target pages), clears, channel moves, stencil→alpha, darken indices, exact-tier pixels, all untextured flat sprites.
  - **Frame RGB:** |Δ| ≤ 2 on ≥ 99% of pixels, |Δ| > 16 on ≤ 0.2% (AFAIL split ordering, float interpolation, ±1 blend rounding), PSNR ≥ 40 dB.
  - **Regression gate:** no recording may lose more than 0.25 dB or double its count of > 16 pixels against the baseline.
- **Unit tests.**
  - tests/unit/gs-draw scenes run through the GPU backend on WARP (deterministic, works while other agents use the GPU).
  - New scenes:
    - random green-counter programmes (±2, +128, bit-plane ops) checked against software;
    - the channel-move compiler checked against brute-force `gs_px_read/write` on random VRAM;
    - a line DDA match on 10k random lines;
    - the coordinate convention at k = 1/2/3.
- **In-game checks.**
  - `PS2_GPU_VERIFY=1` runs the software renderer as well (its existing thread and workers) and compares every 60th picture at k = 1 (forced). Mismatches are dumped to build/verify/.
  - A hotkey switches backends live. Switching to software downloads all GPU pages, and switching back marks targets stale, which exercises both handover paths.
- **Corpus gap.** Ep1, ep2 (including the photo mission, which forums link to readbacks) and ep4 have no recording yet. M8 records them and adds them to the gate.

## 11. Cost estimates (i7-14700F, RTX 4060 Ti)

- **EE thread:** 7.7 ms, unchanged.
- **GS thread:** register decode, vertex conversion (strips and fans into lists, about 120k vertices × 32 B = 4 MB per frame) and StateRec building: an estimated 2–4 ms, down from 16.4.
- **Render thread:** 1.5–8k draws × about 1 µs, plus maps and copies: 2–9 ms (worst case ep8 boss).
- **GPU:**
  - native coverage is about 1–1.5 Mpx per frame including post passes;
  - at 3× (1536×1344) about 1.5–3 ms;
  - at 2560×1440 16:9 about 3–5 ms;
  - at 4K about 6–10 ms (manual palette bilinear dominates; the M7 index atlas roughly halves it).
- **Post passes, channel moves and stencil passes:** under 0.3 ms at 4×.
- **Overall:** all four threads stay under the 16.7 ms frame with margin, so 60 fps no longer depends on how busy the machine is.

## 12. Why this angle, and what was rejected

- **Pure compute GS (paraLLEl-style) on cs_5_0.**
  - It would be exact for everything, but cs_5_0 has no subgroups, no bindless and no 16-bit storage.
  - Rebuilding a binning rasteriser from scratch is the largest-risk path.
  - Sly needs that generality for about 0 of its draws.
- **ROV everywhere (Play!/Xenia shape).**
  - ROV cannot make depth writes conditional on ATST while keeping hardware depth.
  - Sly's two-pass meshes (AFAIL FB_ONLY) are half of all draws, so depth would have to move into a UAV and lose early-Z for every draw.
  - ROV is kept only as plan B for colour-only exact cases.
- **PCSX2-style generic render-target cache with reinterpretation shaders.** Rejected: the list of heuristics never ends.

Here, every reinterpretation goes through real GS memory layout (download, `vram_buf`, upload). The only special-case code is three recognisers derived from GS semantics (tiled fills, channel moves, a rotated stencil counter), and each one is checked against the software renderer.
