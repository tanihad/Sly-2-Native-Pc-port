# Direct3D 11 renderer for the Sly 2 PC port: final design

Status: design approved by synthesis (branch next/d3d11). The decision is fixed here; implementation
proceeds by the milestones in section 11. Inputs, all in `notes/wip/` on next/d3d11:
`d3d11-map.md` (software GS map), `d3d11-census.md` (what Sly 2 sends the GS, 13 recordings),
`d3d11-prior-art.md` (paraLLEl-GS, PCSX2, Play!, RT64, Unleashed), the three candidate designs
(`d3d11-design-high-level.md`, `d3d11-design-gpu-exact.md`, `d3d11-design-sly-specialized.md`) and
the three judgements (fidelity, PC-native, delivery). Read `notes/native-port-principles.md` first.

Abbreviations: **FF** fixed-function D3D11 blending/depth; **ROV** rasterizer-ordered UAV;
**S / kx, ky** internal scale factors; **sw** the existing software GS (runtime/gs_draw*.c);
**hw** the new GPU backend; **gsrec** a GS recording (runtime/gs_record.c format).

---

## 0. Decision

**Architecture: GS draws become Direct3D 11 draws into GPU render targets at an integer scale
(design 1's backbone), with a small set of generic, per-draw exact mechanisms for the operations
fixed-function D3D11 cannot express, an exact integer depth path, and a VRAM-level lockstep
harness against the software renderer.** The software renderer is unchanged, stays the default
and remains the oracle until the GPU backend passes the harness on the whole corpus.

### How the judges' evidence was weighed (fatal issues first)

| design | fatal / decisive issue | outcome |
|---|---|---|
| 2 gpu-exact (compute GS, sample planes) | It is a GS emulator on compute shaders. The owner's principles say "GS primitives -> Direct3D 11 draw calls" and "no pixel-exact GS emulation" (PC-native judge: fatal). It also rewrites the arithmetic of the shipped software renderer, so the oracle itself changes (delivery judge: near-fatal), produces no GPU frame before its M4, and needs 9-14 ms of GPU at 4x. | Rejected as the architecture. Kept as the documented fallback for the mesh passes if the submission gate (M2) fails, built against the *existing* sw arithmetic. Its best ideas are grafted: sample 0 = native ("plane 0 exact"), VRAM-level lockstep with bisection, per-byte-lane hazards, WARP runs, build-time hlslc. |
| 3 Sly-specialised hybrid | Correctness of the main effects rests on recognisers fitted to 13 recordings; its fallback box-averages downloads at even scales, which corrupts integer alpha data (T8H indices, DATE bit 7); its stencil-virtualised G is hidden state invisible to a VRAM diff (fidelity and delivery judges). | Not the backbone. Grafted: all-fallback M0 with a 0-diff gate, StateRec per vertex (CLUT/texture/fog changes do not break draws), direct sampling from a swizzled VRAM buffer first, content-hashed CLUT ring with min/max alpha, As-range check for pass A, tiled-fill -> Clear, command chunks to a render thread, display-driven VBlank pacing, its measured constants as test inputs. Its stencil G counter survives only as an optional performance path behind lockstep (M9). |
| 1 high-level | Major issue verified by the fidelity judge: depth as interpolated `D32_FLOAT` does not reproduce the GS's per-pixel *integer, truncated* Z. Sly's vertex Z is only 13-17 bits, so ties are common, and outline visibility is decided by `ZTST GREATER` ties (gs_draw.c:619-623; the PCSX2 Sly outline bug #2964). Also: 12-16k D3D draws per frame (always splitting pass A) and an optimistic 0.25 us/call submission estimate. | **Adopted as the backbone, with the Z defect fixed generically** (section 3.6: exact integer Z from per-primitive integer records written through `SV_Depth` with an order-preserving float encoding), draw count reduced with design 3's StateRec and As-range check, and the submission cost made a hard early gate (M2). |

Two facts settled the remaining contradictions: the delivery judge verified on this machine that
d3dcompiler_47 compiles `RasterizerOrderedTexture2D<uint4>` in ps_5_0 and the RTX 4060 Ti reports
`ROVsSupported=1`, `TypedUAVLoadAdditionalFormats=1` and doubles; and reading gs_draw.c shows the
software Z is pure integer arithmetic after setup (`rowstep_init` gs_draw.c:496 computes a 16.16
`int64` row value and x/y steps; `tests_batch` gs_draw_pixel.c:427 takes `z >> 16` and clamps), so a
pixel shader can reproduce it bit for bit from a per-primitive record without touching the
software renderer.

### Contradictions resolved explicitly

| question | designs disagreed | resolution and reason |
|---|---|---|
| Depth | D1/D3 float interpolation; D2 integer in compute | Exact integer Z in the PS from a per-primitive record, `SV_Depth = asfloat(z + 0x00800000)` into D32. Order-preserving and exact for z <= 0x3F000000 (Sly: 13-17 bits). Keeps hardware depth test and write. |
| Exact mechanism for non-FF draws | D1 ROV + ROV_DEPTH; D3 disjoint grouping + copies; D2 everything in compute | One **ROV tier**: colour as a ROV UAV, depth *read* from an SRV of the read-only DSV, conditional Z *writes* into a ROV "zpatch" UAV resolved into the DSV after the run. Handles self-overlap exactly without grouping. A **REF mode** forces every draw through this tier (oracle at any scale). |
| Sample positions | D1/D3 symmetric around the GS centre; D2 plane 0 at offset 0 | Sample (i, j) of native pixel n sits at GS position `n + (i/kx, j/ky)`: **sample 0 is the native pixel**. Downloads, readbacks, CLUT-from-target, in-game verification and lockstep at any scale read sample 0, never an average. The presenter removes the (k-1)/2k px shift. |
| Scale factors | D1 fractional `S*W`; D3 fractional anamorphic; D2 powers of two | Integer `kx, ky` per target family; display family `ky = 2*kx`; widescreen `kx = round(N*W)`. The presenter does the fractional fit. Keeps per-sample correspondence exact for 1:1 reads, ALIAS16 and AA1. |
| Pass A (ATST GEQUAL 126, AFAIL FB_ONLY, Z write) | D1 always split in two draws; D3 As-range then split; D2 exact | As-range check first (one draw when the CLUT/vertex alpha range proves all-pass or all-fail). Straddling draws: FF two-draw split by default, **measured** by lockstep against REF; if the split ever changes a visible pixel class, those draws move to the ROV tier (zpatch makes that exact). As > 128 with a Cd-dependent blend always takes the ROV tier. |
| Shadow G counter | D1 ROV; D3 virtual stencil; D2 compute | ROV tier (generic, exact, visible to VRAM diffs). Volumes merge into one D3D draw because blend selectors are data in the ROV shader. The D3 stencil path is an M9 optimisation only if measured ROV cost requires it. |
| Texture source | D1 index/colour atlases from M2; D3 direct swizzled sampling first | Direct sampling from `vram_buf` first (no cache invalidation bugs in bring-up; uploads ordered in the D3D stream). R8 index atlas in M9 as a measured optimisation, before the default switch. |
| Lines / AA1 | D1/D3 analytic parallelograms; D2 integer DDA | Exact first: the CPU runs the existing DDA (refactored, same arithmetic) and emits one record per native pixel, drawn as native-pixel quads. Exact at sample 0 at any scale. Thin analytic hi-res outlines are an M7 option, default chosen by the owner (open question 4). |
| Fallback | D1 PUNT at native; D3 box-average download | PUNT keeps design 1's rule: download sample 0, run the sw rasteriser, re-upload replicated. Last resort only (unsupported target relationships); generic GPU exact paths cover everything Sly does. Gate: zero on the corpus. |
| HUD in widescreen | D3 heuristic; D1/D2 game code | Game code: side-band hint register 0x7F written by EE hooks (recorded by gs_record automatically). Heuristic exists only as a debug toggle, default off. |
| Device ownership | D2 shared device, GS thread records; D1/D3 render thread | Dedicated render thread owns the immediate context and the swapchain; the GS thread never calls D3D. Keeps driver time off the GS critical path. |
| Missing corpus | D1 last milestone; fidelity judge: before integration | Recording ep1, ep2 photo mission, ep4, FMV, menus, save and loading screens is milestone C, done with the existing sw renderer in parallel with M0-M2. |

---

## 1. Goals and non-goals

**Goals**

1. Sly 2 at a locked 60 fps on the owner's machine (i7-14700F, RTX 4060 Ti) independent of
   background load: GS thread <= 6 ms, render thread <= 8 ms, GPU <= 8 ms per game frame at the
   default quality, in the heaviest recorded scenes (ep8 boss, ep3 hub, ep5 street).
2. Native resolution scaling (integer internal scale 1x-6x, default 3x), anti-aliasing
   (supersampling via the scale plus analytic AA1 outlines), widescreen (renderer side), modern
   presentation (flip model, VRR, display-driven pacing).
3. Every effect Sly uses renders like the PS2 at every scale: G-channel shadow counter, CT16 channel
   shuffles, T8H darken, motion blur, Binocucom render-to-texture, AA1 cel outlines, Z-region
   aliasing, FMV/loading screens. All 8 episodes.
4. Exactness is measurable: at sample 0 the GPU backend equals the software renderer bit for bit
   for Z, alpha, coverage and every integer-only draw class; fixed-function blends and perspective
   texture coordinates are within stated tolerances.
5. The software renderer stays the default and the fallback until the hw backend passes the
   harness on the whole corpus; switching is one setting.

**Non-goals**

- A general PS2 GS emulator or PCSX2-style render-target cache with heuristics. Unsupported
  target relationships go to an exact, counted PUNT; they are not guessed.
- Pixel-exact GS emulation as the shipping path (it exists only as the REF oracle mode and as the
  ROV tier for the draws that need it).
- Bit-exact perspective texture coordinates (the sw renderer accumulates float S/T/Q per span;
  reproducing that is design 2's rejected rewrite).
- TV output emulation in the default path (field weave, flicker filter, interlace). It remains
  available in the sw backend and as "reference compose" for tests.
- MSAA (ROV with MSAA needs per-sample shading; supersampling covers AA).
- D3D12/Vulkan, other platforms, GPUs without ROV in the first release (a copy-based path for
  them is M10 hardening, not a blocker).
- The EE-side widescreen projection, culling and portrait patches (separate EE task; the renderer
  supplies what they need, section 8).

---

## 2. Architecture

### 2.1 Threads and data flow

```
EE thread      game code, VIF/VU, GIF  --gs_write_reg / gs_image_write-->  SPSC queue (gs_thread.c, unchanged)
               SIGNAL/FINISH/LABEL/CSR executed at queue time (gs.c, unchanged)

GS thread      gs_exec_write_reg -> env_update -> vertex queue -> draw()        (gs_draw.c, shared decode)
  (frontend)     sw mode: rasterisers -> gs_batch_* -> pixel workers             (unchanged)
                 hw mode: gs_hw_prim()  (runtime/gpu/gpu_front.c)
                            classify tier, build StateRec, vertices, records,
                            page/lane ownership, hazards, CLUT ring, uploads
                            -> command chunks (64 KB) + data arenas (4 MB chunks)
                                       | SPSC, published like gs_thread.c
Render thread  runtime/gpu/gpu_d3d11.c: sole owner of ID3D11Device(Context1) + swapchain
                 Map NO_OVERWRITE rings, state objects, Draw/Dispatch/Copy, ZRESOLVE,
                 downloads (fence + staging), compose, Present, timestamps
Window thread  window.c: messages, input, fullscreen/resize events (no D3D in hw mode)
Timer, audio   unchanged
```

In hw mode the 8 pixel workers are not started. The EE observes exactly what it observes today
(section 7).

### 2.2 Files

New directory `runtime/gpu/` (non-frozen; the frozen `hw.h` and `gs_internal.h` do not change):

| file | role | thread |
|---|---|---|
| `gpu.h` | backend interface used by gs_draw.c / gs_thread.c / gs.c | - |
| `gpu_front.c` | classification, StateRec dedup, vertex/index/record emission, line pixel expansion, sprite records | GS |
| `gpu_own.c` | page and byte-lane ownership, target table, block validity, hazards, feedback copies, ALIAS16 batching | GS |
| `gpu_clut.c` | CLUT ring (content hash, min/max alpha), `vram_buf` upload patches | GS |
| `gpu_cmd.c/.h` | command chunk and arena SPSC to the render thread | both |
| `gpu_d3d11.c` | device, caps, resources, state-object caches, command execution | render |
| `gpu_present.c` | compose, resolve/downscale, letterbox, swapchain, pacing | render |
| `gpu_verify.c` | lockstep download/compare, stats, in-game verify | GS + render |
| `shaders/*.hlsl` | `vs_tri`, `vs_sprite`, `vs_linepix`, `ps_gs` (ubershader), `cs_alias16`, `cs_upload_target`, `cs_download`, `ps_zresolve`, `ps_compose`, `ps_resolve`, `cs_conformance` | - |

Edits to existing non-frozen files (all behind `PS2_GS_BACKEND`, sw behaviour byte-identical):

- **gs_draw.c**: `draw()` (:800) calls `gs_hw_prim(&P, verts, kind)` in hw mode. `env_update` (:165)
  stays the single register decoder for both backends and calls `gpu_state_changed(&P)` when
  `gs_prim_version` moves. The triangle plane setup (`PLANE`, `rowstep_init`) and the line DDA loop
  are factored into `gs_setup.c/.h` helpers used by both backends **without any arithmetic change**
  (proved by the existing gs-draw unit tests and by sw replay hashes before/after, M1).
  `trxdir` (:892), `gs_exec_image_write` (:934), `gs_exec_image_read`, `tex0_write` (:956),
  `gs_draw_reset` (:946) call the corresponding `gpu_*` hooks.
- **gs_thread.c**: in hw mode `vblank_picture` emits `OP_PRESENT{priv[16]}`; the idle kick also
  publishes the command chunk; `picture_done` is signalled by the render thread after Present.
  Register 0x7F (hints, section 8.3) is routed to `gpu_hint()` and ignored by sw.
- **gs.c**: `decode_circuit` (:138) becomes the pure `gs_display_decode()` in gs_draw.h, shared with
  the presenter. CSR.RESET also calls `gpu_reset()`.
- **ps2_runtime.c**: settings `PS2_GS_BACKEND=sw|hw` (default sw until M11), `PS2_GS_SCALE`,
  `PS2_GS_ASPECT`, `PS2_GS_REF=1`, `PS2_GS_VERIFY=N`, `PS2_GS_HW_STATS=1`.
- **window.c**: in hw mode creates no device and exposes `window_gpu.h` (`window_hwnd()`, atomic
  client size, fullscreen state, resize event). The existing D3D path stays for sw.
- **gs_record.c**: unchanged; hint register writes are recorded as ordinary tag-1 records.
- **scripts/gs_replay.c/.ps1**: `-Backend sw|hw|both|hwdry`, `-Scale`, `-Aspect`, `-Adapter hw|warp`,
  `-Ref`, `-Lockstep`, `-Metrics file`, `-Bench`.

### 2.3 Integration rules with the software renderer

- One decoder (`env_update` -> `gs_prim P`) feeds both backends; there is no second register
  interpreter.
- `gs_vram` remains the CPU copy and the authority for CPU-owned pages (uploads, CLUTs, FMV
  frames). GPU-owned pages are downloaded (sample 0) only when the CPU needs them.
- `gs_mem_xfer_*`, `gs_clut_on_tex0`, `gs_tex_fetch` and the swizzle tables are reused, never
  ported, except the swizzle tables, which are uploaded to the GPU as data generated from
  `gs_mem_swizzle.h` (one source of truth).
- `both` mode (offline and in-game verify) runs the sw backend on the same stream and compares.

---

## 3. Data model

### 3.1 Sample grid and scale families

- Every GPU target has integer factors `kx, ky`. Native pixel `(x, y)` owns samples
  `(x*kx + i, y*ky + j)`, `0 <= i < kx`, `0 <= j < ky`, positioned at GS coordinate
  `(x + i/kx, y + j/ky)` (GS pixel centres are integers).
- **Sample (0, 0) is the native pixel.** Its coverage and integer attributes equal the sw renderer's;
  this is the basis of every download and of lockstep at any scale.
- VS mapping: `d3d = gs * k + 0.5` per axis (D3D pixel `x*kx + i` has its centre at
  `x*kx + i + 0.5`, which is GS position `x + i/kx`). With 12.4 vertices and integer k this lies on
  D3D's 1/256 grid, so coverage at k=1 is exact (verified in M3 by the coverage unit scenes).
- Families:
  - **display**: buffers shown through DISPFB, the Z bound with them, Z16S at 070:
    `kx = N` (4:3) or `round(N*W)` (widescreen, W = aspect/(4/3)), `ky = 2N` when SMODE2 is field
    mode (INT=1, FFMD=1, Sly always), else `ky = N`.
  - **square**: other targets (Binocucom RTTs 0A0/0A8, their Z 0B8): `kx = ky = N`.
  - A target sampled 1:1 by another target adopts the sampler's family (motion blur A -> B).
- Default `N = 3` (1536x1344 internal for 4:3).

### 3.2 GPU resources

| resource | format | purpose |
|---|---|---|
| `vram_buf` | 4 MB `ByteAddressBuffer`, DEFAULT, SRV+UAV | mirror of `gs_vram` for CPU-owned data (textures, CLUTs, uploads, FMV pages); also the staging layout for downloads/uploads |
| `swz_tab` | `Buffer<uint>` | page/block/column tables for CT32/24/16/16S, Z32/24/16/16S, T8, T4, generated from `gs_mem_swizzle.h` |
| `alias16_lut` | R16_UINT 64x64 | CT16 page pixel -> (CT32 pixel in page, halfword), built from `gs_addr16/gs_addr32` |
| `clut_ring` | `StructuredBuffer<uint>` 4096 x 256 | CLUT snapshots (1 KB each), content-hashed, with min/max alpha per slot |
| `state_ring` | `StructuredBuffer<StateRec>` | per-GS-state data, 96 B each |
| `prim_ring` | `StructuredBuffer` | per-primitive exact records (Z record 32 B; full record 128 B for ROV-tier prims) |
| `vtx_ring`, `idx_ring` | DYNAMIC VB/IB, NO_OVERWRITE rings, 16 MB / 4 MB | vertices (32 B), u32 indices |
| colour target | `R8G8B8A8_TYPELESS` (RTV UNORM, SRV UNORM + UINT, UAV UINT) | one per exact (FBP, FBW); holds raw GS bytes |
| depth target | `R32G8X24_TYPELESS` (DSV D32S8, SRV R32_FLOAT_X8X24) | one per exact (ZBP, FBW, Z format) |
| `zpatch` | `R32G32_UINT` texture, UAV | per depth target: (z, run serial) written by ROV-tier draws with Z writes |
| `shadow` | same as colour target | feedback copies of the bound target (bbox regions) |
| `picture[3]` | `R8G8B8A8_UNORM` | composed pictures for the presenter |

### 3.3 Page ownership, byte lanes, block validity (`gpu_own.c`)

Per 8 KB page: `{ uint16 target; uint8 owner; uint8 lanes_gpu; uint32 cpu_gen; }`.

- **owner**: `PG_CPU` (`gs_vram`/`vram_buf` hold the truth), `PG_GPU(target)`, `PG_BOTH`.
- **lanes** (design 2 graft): writes and reads are tracked per byte lane, *low 24 bits* vs *top
  byte*. A CT32 draw with FBMSK FF000000 writes only the low lane; a T8H/4HL/4HH read reads only the
  top lane. The T8H darken (reads B.alpha, writes B.RGB) is therefore not a feedback hazard and
  needs no copy.
- **block validity**: each target keeps a bitmap of 8x8-native-pixel blocks that hold current data.
  A host->local upload makes the overlapping blocks of every target stale (and patches `vram_buf`).
- **Load**: before a draw *reads* the destination over stale blocks (blend with Cd, ZTST other than
  ALWAYS, DATE, partial FBMSK, feedback sampling, ROV tier), `OP_UPLOAD_TARGET(T, blocks)` unswizzles
  native pixels from `vram_buf` and replicates them to all samples.
- **Opaque writes** (no destination read, FBMSK 0, ZTST ALWAYS for depth) revalidate blocks without
  a load. A recognised tiled fill covering the whole target becomes a Clear (design 3 graft).
- **Download** (`OP_DOWNLOAD(pages)`): `cs_download` writes **sample 0** of each native pixel into
  `vram_buf` in swizzled GS layout (resolving any pending zpatch first), copies to staging; the GS
  thread waits on the fence and copies into `gs_vram`. Used by TRXDIR=1, TRXDIR=2 from GPU pages,
  CLUT loads from GPU pages, partial-page uploads into GPU pages, PUNT, lockstep, verify.

Sly's per-frame Z-region sequence costs nothing: uploads into 080-0A3 make main-Z blocks stale; the
backdrop samples them from `vram_buf` (ZTST ALWAYS, ATST NEVER FB_ONLY: no Z access); the 16-sprite
ZB_ONLY clear is recognised as a full overwrite (ClearDepthStencilView) and revalidates them.

### 3.4 Targets (`gpu_own.c`)

```c
typedef struct gpu_target {
    uint16_t base_page, fbw;      /* exact base and width only */
    uint8_t  kind;                /* TGT_C32 (CT32/CT24), TGT_Z32, TGT_Z24, TGT_Z16 (Z16/Z16S) */
    uint8_t  family;              /* display / square */
    uint8_t  kx, ky;
    uint16_t rows;                /* native rows; grows by recreate + copy */
    uint32_t id;                  /* render-thread handle */
    uint64_t pages[8];            /* covered pages */
    uint64_t *block_valid;        /* 8x8-native-pixel blocks */
    uint32_t last_used_frame;
} gpu_target;
```

- Lookup key: `(base_page, fbw, kind class)`. A FRAME/ZBUF starting inside a target at another base
  or FBW is a PUNT (never in the census).
- A CT16/CT16S FRAME at a C32 target's base and FBW is an **ALIAS16 view** of that target, not a
  target. A CT16 texture over a C32 target likewise.
- Overlapping targets of another kind (Z16S over Z32 at 070; RTTs 0A0/0A8 and Z 0B8 over main Z):
  separate targets; the last writer owns the pages and the other's blocks go stale. Reading stale
  aliased blocks performs download (sample 0) then load, exact and counted; in Sly each aliased
  target is cleared before use, so this never happens.
- Height from SCISSOR.Y1+1 rounded to page height; a taller later use recreates and copies.
  Targets unused for 120 frames are released (after a download if GPU-newer).

### 3.5 StateRec, vertices, records

`StateRec` (96 B, one per distinct GS state version, dedup by 64-bit hash + memcmp within a frame):
TEX0 (lo/hi), CLAMP (lo/hi), TEX1, TEXA, CLUT slot, texture source (`none | vram | target SRV slot +
view CT32/CT24/T8H/4HL/4HH | shadow`), FOGCOL, TEST, ALPHA selectors + FIX, FBMSK, FBA, COLCLAMP,
DATE/DATM, AREF, TFX/TCC/FGE/IIP/FST, XYOFFSET-relative sprite params, HUD transform id,
record-buffer base, phase label (diagnostic).

Vertex (32 B): `float x, y` (GS 12.4 minus XYOFFSET, as float, exact) | `uint32 rgba` | `uint32
fog8 | state_index24` | `float s, t, q` (ST with the low 8 mantissa bits dropped as gs_draw.c:836)
or `u, v` | `uint32 pad`.

Records, indexed by `SV_PrimitiveID` (triangles, indexed lists) or `SV_InstanceID` (sprites, line
pixels) plus a per-D3D-draw base passed through `PSSetConstantBuffers1` offsets (D3D11.1, one call
per draw):

- **Z record** (32 B, every triangle with ZTE and Z test or write): `int64 row` (16.16 at `by0`),
  `int64 dy`, `int64 dx`, `int16 by0`, `uint16 zfmt`. Computed by the shared `rowstep_init` code.
- **Full record** (128 B, ROV-tier triangles): row/dy/dx for R, G, B, A, F, U, V (FST=1) in the same
  16.16 integer form, plus the Z record.
- **Sprite record** (48 B): native pixel rectangle `[x0, x1) x [y0, y1)` computed by the sw rule
  (draw_sprite gs_draw.c:386), flat colour/Z/fog from v1, U/V start and per-pixel step in draw_sprite's
  fixed point.
- **Line pixel record** (32 B): one per native pixel produced by the refactored `draw_line` DDA:
  `px, py, z, rgba (or coverage in A for AA1), fog, s/t/q or u/v`.

### 3.6 Exact integer depth

The PS computes, for sample (i, j) of native pixel (px, py):

```
z16 = row + dy*(py - by0) + dx*px + floor(dy*j/ky) + floor(dx*i/kx)   // int64, emulated with uint2
z   = clamp(z16 >> 16, 0, zmax[zfmt])                                     // tests_batch, gs_draw_pixel.c:427
SV_Depth = asfloat(z + 0x00800000)                                        // order-preserving, no denormals
```

- At sample 0 this is exactly the sw value (`rowstep_start` + per-pixel `dz` stepping is the same
  integer sum).
- The encoding is monotonic and injective for `z <= 0x3F000000` (encodes to <= 1.0). Equal integers
  compare equal, so `GREATER`/`GEQUAL` ties (outlines vs mesh, pass B over pass A, decals) decide
  exactly as on the GS. Z24, Z16, Z16S always fit; Z32 above the limit makes the frontend route the
  draw to the ROV tier and switch the depth target to zpatch-as-depth until its next full clear
  (counted; M1 reports the Z range per recording, expected never).
- `ZTST GEQUAL/GREATER/ALWAYS/NEVER` map to D3D `GREATER_EQUAL/GREATER/ALWAYS/NEVER`. Clears use
  `ClearDepthStencilView(asfloat(z0 + 0x00800000))`.
- Cost: `SV_Depth` disables early-Z for those draws. At 3x with Sly's overdraw this is ~10-20 M PS
  invocations per frame, well within budget; conservative-depth variants are an M9 option.
- Sprites and line pixels carry a constant Z per native pixel (as sw).

---

## 4. Draw classification

### 4.1 Tiers (static rule on `gs_prim P` + StateRec data; no game pattern matching)

```
FRAME psm 16-bit over a C32 target (same base/FBW)          -> ALIAS16 (compute)
unsupported target/texture relationship                     -> PUNT
REF mode                                                    -> ROV
needs_rov(P):                                               -> ROV
    COLCLAMP==0 && blend can leave [0,255]
 || FBMSK has a byte that is neither 00 nor FF
 || DATE
 || blend form not in the FF table (4.2), e.g. Cd*FIX>>7, C==Ad
 || Cd-dependent blend && (FIX > 128 || maxAs > 128)
 || PABE || DTHE
 || Z32 vertex above 0x3F000000
 || pass-A straddle escalation enabled (4.3) for this draw
else                                                        -> FF
```

`maxAs`/`minAs` = CLUT slot min/max alpha (or texture-format bound) combined with the vertex alpha
range under TFX (MODULATE: `(T*V)>>7`), TCC, and AA1 coverage.

Sly mapping (from the census):

| tier | draws |
|---|---|
| FF | mesh pass A (As <= 128) and pass B, outline line pairs, backdrop, clears, alpha reset, T8H darken, motion blur, Binocucom composite, HUD |
| ROV | shadow volumes (COLCLAMP 0, FBMSK FFFF00FF, DATE), bit-plane steps (FFFF80FF, FFFFFEFF, Cd*2>>7), G init sprites inside a ROV run, pass-A draws with As > 128 (0.4-5.4% of pass-A kicks) |
| ALIAS16 | the 96 shuffle sprites per frame (3-6 dispatches) |
| PUNT | none (gate) |

### 4.2 FF tier

Colour target bound as `R8G8B8A8_UNORM` RTV holding raw GS bytes. The PS computes TFX
`(C*T)>>7`, fog `(f*C + (255-f)*F)>>8` and the alpha test in integers, outputs bytes /255.

| GS blend `((A-B)*C>>7)+D` | D3D11 | exact |
|---|---|---|
| `(Cs-0)*C+Cd` | `o = (Cs*C)>>7`; ONE, ONE, ADD (UNORM saturate = COLCLAMP 1) | yes, any C |
| `(0-Cs)*C+Cd`, COLCLAMP 1 | `o = (Cs*C+127)>>7` (the GS floors the negative term); ONE, ONE, REV_SUBTRACT | yes |
| `(Cs-0)*C+0`, `(0-0)*C+Cs` | blend off, `o = (Cs*C)>>7` / `Cs` | yes |
| `(0-0)*C+Cd`, `(Cs-Cs)*C+Cd` | colour write mask 0 (alpha per FBMSK/As rule) | yes |
| `(Cs-Cd)*C+Cd`, C <= 128 | dual source: o1.a = C/128; SRC1_ALPHA, INV_SRC1_ALPHA | +-1 LSB |
| anything else | ROV tier | - |

- Alpha: blend `ONE, ZERO`, output As (OR 0x80 with FBA). Destination alpha bytes are therefore exact
  integers (DATE and T8H depend on this).
- Byte-aligned FBMSK -> `RenderTargetWriteMask`; any other FBMSK -> ROV.
- Depth: exact `SV_Depth` (3.6) when ZTE and the test or write matters.
- **ATST + AFAIL**: constant outcomes are state (NEVER+FB_ONLY: Z write off; NEVER+ZB_ONLY: colour
  mask 0; NEVER+RGB_ONLY: RGB mask, Z write off); real test + KEEP: `discard`. Real test with
  FB_ONLY/ZB_ONLY/RGB_ONLY: if `minAs/maxAs` prove all-pass or all-fail, one draw; otherwise two draws
  (Z write + discard on fail, then no Z write + discard on pass). See 4.3.
- Pipeline key (a change starts a new D3D draw): tier, PS permutation, blend-state id,
  depth-stencil-state id, write mask, scissor, primitive class, target binding generation, texture
  SRV binding (vram_buf vs a target SRV), record base page. Texture, CLUT, fog colour, TFX/TCC, AREF,
  TEXA, wrap modes live in the StateRec, so CLUT-only and TEX0/FOGCOL changes merge into the current
  draw. Expected 1.5-4k D3D draws per frame, 6-8k in ep8 boss (M2 measures).

### 4.3 ROV tier

Bindings: `OMSetRenderTargetsAndUnorderedAccessViews(no RTV, DSV read-only, UAVs: colour as
RasterizerOrderedTexture2D<uint4>, zpatch as RasterizerOrderedTexture2D<uint2>)`, depth SRV of the
same target (legal with a read-only DSV).

Per fragment, in primitive order:
1. Attributes from the full record (integer, exact) or vertex STQ (float).
2. Texture, TFX, fog, ATST in integers (the gs_draw_pixel.c pipeline ported: `tfx_batch`,
   `fog_batch`, `tests_batch`, `write_seg`).
3. Current Z = `zpatch.serial >= run_start ? zpatch.z : decode(depth SRV)`; ZTST in integers.
4. Read Cd from the colour ROV; DATE/DATM; blend `((A-B)*C>>7)+D` in int; clamp or `&255`; FBA;
   `out = (new & ~fbmsk) | (d & fbmsk)` per bit; write.
5. Z write (per AFAIL rules) -> zpatch `(z, serial)`.

A **ROV run** is a maximal sequence of consecutive ROV-tier draws on one target pair. At the end of a
run that wrote Z, `ps_zresolve` (one quad over the union bbox, `SV_Depth` from zpatch where
`serial` is in the run) moves Z into the DSV. Blend selectors, FIX, FBMSK, DATE, COLCLAMP are data,
so all shadow-volume strips of a frame merge into one D3D draw (few UAV barriers).

**REF mode** (`-Ref`, `PS2_GS_REF=1`): every draw takes this tier with zpatch as the depth buffer for
the whole frame. It is exact at sample 0 except perspective texture coordinates, and is the oracle
for FF classes at any scale.

**Pass-A straddle escalation**: the FF two-draw split reorders fragments only where one draw covers
a pixel twice with one fragment failing ATST and the other passing. M5 measures this with lockstep
(FF vs REF, and vs sw). If any visible difference is found, the escalation flag routes straddling
pass-A draws to the ROV tier (exact, at the cost of ROV runs; M5 measures that too).

### 4.4 Primitive classes

- **Triangles** (strips, fans, lists): indexed triangle lists, hardware coverage per sample.
  Records via `SV_PrimitiveID`.
- **Sprites**: instanced quads snapped to the native rectangle computed by the sw rule; *single
  rate*: all samples of a native pixel take the native pixel's coverage, UV and colour (paraLLEl-GS
  rule). UI never bleeds; 1:1 post passes stay texel exact.
- **Lines and AA1**: native-pixel quads from the CPU DDA records; all samples of the native pixel
  take its coverage (AA1 coverage replaces As exactly as gs_draw.c:631-641) and its Z. At sample 0
  identical to sw. M7 adds an optional analytic thin-line mode for scale > 1 (coverage `1-|d|` in
  native units, Z from the line plane at the sample), selected by a setting.
- **Points**: one native-pixel quad. AA1 on triangles is ignored, as sw (logged).

### 4.5 Shader permutations

`ps_gs` compile-time key (everything else is a uniform branch on the StateRec):

```c
typedef union { uint32_t u; struct {
    uint32_t tier  : 2;  /* FF_DUAL, FF_PLAIN, ROV */
    uint32_t tex   : 3;  /* none, vram indexed (T4/T8/T8H/4HL/4HH), vram direct (CT32/24/16), target CT32/24, target alpha-index */
    uint32_t prim  : 2;  /* tri, sprite, linepix */
    uint32_t zmode : 2;  /* none, exact SV_Depth, ROV (shader test + zpatch) */
    uint32_t stq   : 1;  /* perspective STQ vs integer UV */
}; } gpu_ps_key;
```

About 60 reachable variants, all compiled at build time. VS: `vs_tri`, `vs_sprite`, `vs_linepix`,
`vs_full`. CS: `cs_alias16`, `cs_upload_target`, `cs_download`, `cs_conformance`. PS: `ps_zresolve`,
`ps_compose`, `ps_resolve_{box,lanczos,point}`.

---

## 5. Reinterpretation, feedback, aliasing

### 5.1 Target as texture

When TBP0's page equals a live colour target's base and TBW equals its FBW:

- **CT32/CT24 view** (motion blur reading A; Binocucom RTTs at TBP 1400/1500): bind the target SRV.
  - **1:1 rule**: a sprite or flat primitive that steps one native texel per native pixel with an
    integer offset, reading a target of the same family, fetches sample (i, j) of the corresponding
    native texel. GS bilinear for 1:1 reads is evaluated in *native* texel units per sample, so the
    motion blur's +1.0-row bilinear tap stays a blend of native rows N and N+1 at any scale (PC-native
    judge's correction), and the measured offsets (+0.5 darken/copies, (+0.5, +1.0) bilinear motion
    blur with TEX1 0x61, -7.5/+8.5 shuffles) are unit-test assertions.
  - Other reads (Binocucom composite): GS bilinear on the scaled texture, clamped to the UV bbox.
- **T8H / 4HL / 4HH view** (shadow darken through the CLUT at CBP 1800): same layout as CT32, so the
  PS reads `.a` (or a nibble of it) through the UINT SRV at the same sample, then the CLUT slot.
- **Other formats of a target** (T8/T4/CT16 texture over CT32 outside ALIAS16): PUNT.

**Feedback** (sampling the bound target): D3D11 cannot bind one subresource as SRV and RTV, so
`OP_COPY_SHADOW(T, rect)` copies the page-rounded texel bbox into `T.shadow`. The shadow stays valid
while reads avoid blocks written since the copy (bitmap); a read of such a block re-copies, which
matches GS order. With byte-lane tracking the darken needs no copy at all.

### 5.2 ALIAS16 (CT16 views of CT32 targets)

A CT16 page and a CT32 page are both 8 KB; with equal FBW, CT16 page n is CT32 page n. `cs_alias16`
runs a batch of CT16 sprites:

1. One thread per (destination native CT16 pixel, sample (i, j)).
2. Source texel from the sprite's linear UV (nearest; bilinear -> PUNT).
3. Map destination and source to (CT32 pixel, halfword) with `alias16_lut`.
4. Read the source sample's halfword, unpack 5551 with TEXA (TA0=0, TA1=0x80, AEM 0 in Sly), run the
   16-bit blend/COLCLAMP/FBA, repack, apply the 16-bit write mask derived from FBMSK exactly as
   `gs_prim_finalize` does, write the halfword with InterlockedAnd/Or.

The GS thread checks reads and writes on a 28 KB native (CT32 pixel x halfword) bitmap and splits a
dispatch when a sprite reads something written earlier in it or writes a 32-bit pixel already written
in it (two halfwords of one uint must not race). Expected 3-6 dispatches per frame. Exact at any scale
because sample (i, j) of one native pixel corresponds to sample (i, j) of every other.

### 5.3 Z-region aliasing

- Uploads into 080-0A3 (inside main Z32) are memory sources sampled from `vram_buf`.
- Z16S at ZBP 070 is its own `TGT_Z16` target, created after its own clear.
- Binocucom RTTs 0A0/0A8 and their Z at 0B8 are square targets; their pages make main-Z blocks
  stale; the next frame's clear revalidates them.

### 5.4 PUNT

Flush; download sample 0 of the affected pages; run the sw rasteriser synchronously for the draw
(`gs_sw_draw_record`, the existing code); mark the pages CPU-owned and reload them into the target,
replicated to all samples. Exact at sample 0, blocky at scale, slow. Counted and logged per state
signature. Gate: zero on the corpus.

---

## 6. Census hard cases

| # | case (census section 7) | handling |
|---|---|---|
| 1 | Shadow counter in G: wrapping +-2 adds (COLCLAMP 0), G init 126, FBMSK FFFF00FF, DATE on alpha bit 7, ZTST GREATER/GEQUAL without Z write | ROV tier: integer blend with `&255` wrap, per-bit FBMSK, DATE on the ROV-read alpha, exact Z test from the depth SRV. Exact per sample; all volume strips of a frame merge into one D3D draw. Optional M9 stencil path (design 3's ROR8 + INCR/DECR_WRAP mask 0x7F) only if ROV cost exceeds budget, gated bit-exact against ROV. |
| 2 | Bit-plane steps FFFF80FF (G=62) and FFFFFEFF with `(Cd-0)*2>>7` | ROV tier (partial-byte FBMSK and Cd*FIX forms are data in the same shader). |
| 3 | Three CT16 shuffles (A.alpha <- B.G; B.alpha bits 0/7 <- B.G bits 0/7 in place; B.G <- A.alpha) | ALIAS16 compute (5.2). In-place group: hazard split on the halfword bitmap. |
| 4 | T8H self-read darken through CLUT 1800, then alpha reset to 0x80 | Target alpha-index view through the UINT SRV at the same sample (5.1); no feedback copy thanks to byte lanes; FF blend `(Cs-Cd)*As+Cd` (+-1). Alpha reset is a tiled fill -> one quad with write mask A. |
| 5 | Field-space post passes (motion blur 32x222 FIX 48 with 1-row scissor offset; texel-centre alignment) | 1:1 rule with native-texel-unit bilinear; scissor is a native rectangle times k. Sample 0 exact, other samples aligned by construction (no half-pixel hacks). |
| 6 | Z-region aliasing (uploads in Z32, backdrop before Z clear, Z16S, Binocucom RTT/Z) | Page ownership + block validity + exact-base targets (3.3, 5.3). |
| 7 | AA1 outlines, each drawn ZB_ONLY then AA1 RGB_ONLY, visibility by GREATER ties | Exact integer Z (3.6) + CPU DDA line pixels (4.4); FF colour mask 0 + Z for the first, RGB mask + dual-source coverage blend for the second. Lockstep tolerance 0 on outline pixel presence. |
| 8 | State churn 2-11k changes, 0.5-7k CLUT binds/frame | StateRec per vertex; only pipeline-key changes break D3D draws; CLUT ring content-hashed; render thread submission; M2 go/no-go. |
| 9 | Alpha precision (0x80 = 1.0, ATST 126/128, texture alpha up to 245) | All tests and factors in integer bytes; FF only where provably within range (`maxAs`), else ROV; destination alpha never blended. |
| 10 | Pass A ATST GEQUAL 126 FB_ONLY with Z write | As-range single draw, else two-draw split; measured against REF; escalation to ROV tier (exact via zpatch) if visible. |
| 11 | Binocucom 128x128 RTTs drawn and sampled in the same frame | Square targets found by exact base; ordered in the command stream; scaled bilinear read clamped to UV bbox. |
| 12 | Display: 512x224 field buffers, flicker filter (EN1+EN2 same FBP, DBY 1/0, ALP 128, MMOD 1), half-line OFY on one VBlank, DY 51 phase | Display family renders at `ky = 2kx` (true 448N progressive lines); the standard flicker pattern is recognised by `gs_display_decode` and circuit 2 is shown; the OFY half-line fraction is ignored in progressive mode (toggle for tests); other PMODE patterns use the GPU two-circuit mix; reference compose (download + `gs_compose`) for S=1 picture tests. |
| 13 | FMV and uploaded loading screens in display pages | CPU-owned display pages are presented from `vram_buf` via `cs_upload_target` at native resolution (bilinear/bicubic to the output). A movie hint (8.3) pillarboxes. When movies move to native playback (principles), the decoded frame is presented directly. |
| 14 | Exact Z (13-17-bit scene Z, ties decide outlines and decals) | Section 3.6. |
| 15 | Readbacks (TRXDIR=1, suspected in the ep2 photo mission), CLUT from drawn pages, TRXDIR=2 | Download of sample 0 (exact at any scale) then the existing CPU code; TRXDIR=2 between compatible targets with exact geometry is a scaled CopySubresourceRegion, otherwise download + CPU transfer + invalidate. Counted. |
| 16 | Memory sources: T4 60-65% / T8 35-40% of textured prims, CT32 CLUTs CSM1, sizes 8x8..256x128, REPEAT/CLAMP, no mipmaps | `vram_buf` direct sampling through `swz_tab`, CLUT slot lookup, GS 4-bit bilinear after palette lookup, wrap modes in integer texel space; MIPTBP/LOD supported generically (unused by Sly). Index atlas in M9. |
| 17 | Unrecorded content (ep1, ep2, ep4, menus, save, FMV) | Milestone C records it before GPU bring-up; generic tiers mean variants (Gouraud G, other FBMSK) stay exact on the GPU; PUNT counter + in-game verify (10.5). |
| 18 | Widescreen HUD | Hint register 0x7F from EE hooks; VS HUD transform (8.3). |

---

## 7. EE-visible behaviour and synchronisation

- **Unchanged**: SIGNAL/FINISH/LABEL, CSR, IMR, SIGLBLID, FIELD/VSINT executed on the EE thread at
  queue time (gs.c). The queue, its full-queue back-pressure and slot format are unchanged.
- **VBlank**: the EE still waits at VBlank while the previous picture is in flight; "done" now means
  the render thread has issued Present. GPU depth is bounded separately by the frame-latency
  waitable object and an event query (max 2 frames in flight).
- **`gs_sync` / `gs_image_read` (TRXDIR=1)**: GS thread drains the queue, publishes the command
  chunk, issues `OP_DOWNLOAD(src pages)` (sample 0), waits on the fence, patches `gs_vram`, then the
  existing `gs_mem_xfer_read`. Cost ~0.5-2 ms, only when the game reads back.
- **Host->local**: existing CPU path into `gs_vram`, plus a `vram_buf` patch op (ordered in the
  stream) and invalidation. Partial-page uploads into GPU-owned pages download first.
- **CLUT load (TEX0 CLD)**: from CPU pages via the existing `gs_clut_on_tex0` (all of Sly); from GPU
  pages after a download.
- **CSR.RESET**: `gs_sync` + `gs_draw_reset` + `gpu_reset` (submit pending work, drop the vertex
  queue; targets keep contents, as VRAM does).
- **Backend switch**: chosen at start (`PS2_GS_BACKEND`). A live switch (debug hotkey, M6) is: hw ->
  sw: `gs_sync`, download all GPU-owned pages, start pixel workers; sw -> hw: `gs_sync`, mark all
  pages CPU-owned. Device loss three times -> automatic switch to sw.

---

## 8. Scaling, AA, widescreen, presentation

### 8.1 Scaling and AA

- `PS2_GS_SCALE=N` (1-6): display family `kx = N` (or `round(N*W)`), `ky = 2N`; square family
  `N x N`. Default 3. Memory at 3x for the four display targets + shadow + zpatch is ~60 MB.
- Anti-aliasing = supersampling: the presenter downsamples the target to the window with a box or
  Lanczos-2 filter (upscales with Catmull-Rom, 1:1 with point). The sample-grid shift of
  `(k-1)/2k` native px is removed in the resolve so the picture is centred.
- AA1 outlines keep the GS two-pixel coverage (exact) by default; optional thin analytic mode (M7).
- Optional later post AA (FXAA/SMAA) on the resolved picture: not planned, noted.

### 8.2 Presentation and pacing

- Swapchain on `window_hwnd()`, created by the render thread: FLIP_DISCARD, 3 buffers,
  `R8G8B8A8_UNORM`, frame latency 1 with the waitable object, `ALLOW_TEARING` when supported; no DXGI
  Alt+Enter (window.c keeps borderless fullscreen).
- Game clock: the host 59.94 Hz VBlank timer (unchanged) by default. Modes:
  - VRR (G-Sync/FreeSync borderless): `Present(0, ALLOW_TEARING)`, every picture shown once.
  - `PS2_VBLANK=display` on a 59.94/60 Hz display (within 0.5%): the waitable object drives the EE
    VBlank, which removes the repeat-a-frame judder.
  - Otherwise `Present(1, 0)` of the latest picture.
- Letterbox moves from window.c into `gpu_present.c` (shared helper), aspect 4:3 / 16:9 / stretch.
- Device lost: GPU contents are gone; recreate device and targets, mark every page CPU-owned and
  reload targets from `gs_vram` (GPU-owned pages are one frame stale there, so at most one frame is
  wrong; Sly redraws everything each frame); after 3 failures switch to sw.

### 8.3 Widescreen: what the renderer supplies

The projection, culling and portrait patches are EE work (f_0014D6D8 projection constant at
0x14D750/54, f_0014F308 hook, f_001BE6F0 portraits; see prior art section 5). The renderer provides:

1. **Anamorphic display targets**: `kx = round(N*W)` so the 512-unit-wide buffer keeps pixel density;
   scissor, FRAME and all full-screen post passes are untouched and cover the full width.
2. **Side-band hints**: GS register **0x7F** (never written by PS2 software, outside
   `gs_reg_mirror`'s 0x64 range, recorded by gs_record as an ordinary register write). Payload
   `{u8 kind, u8 arg, u16 reserved, u32 value}`; kinds `HUD_BEGIN(anchor L/C/R)`, `HUD_END`,
   `ASPECT_4_3_BEGIN/END` (movies, menus to pillarbox), `PHASE(label)` (diagnostic). sw ignores it.
   EE hooks emit them (separate EE task) via `gs_write_reg(0x7F, v)`.
3. **HUD transform**: draws between HUD_BEGIN/END get `x' = c + (x - c) * (4/3)/A` in the VS with the
   anchor's `c`. Without hints the HUD stretches (as with the PCSX2 patch). A draw-classification
   heuristic exists only as a debug toggle, default off.
4. **Pillarbox** of hinted 4:3 content by the presenter.

---

## 9. Shader build

- `scripts/hlslc.c` (~150 lines), built by `scripts/build_game.py` with llvm-mingw clang into
  `build/tools/hlslc.exe` (binary not committed). It `LoadLibrary`s System32 `d3dcompiler_47.dll`
  (ships with Windows; nothing is downloaded) and calls `D3DCompile` with an include handler,
  `/O3`, warnings as errors, for every entry/permutation listed in `runtime/gpu/shaders/perms.txt`.
  Output: `build/gen/gpu_shaders.h` (DXBC byte arrays + permutation table). Compile errors fail the
  build.
- Development: `PS2_GPU_SHADER_DIR=<dir>` compiles at run time with an in-memory cache keyed by
  FNV-64 of source + defines + compiler version.
- `[loop]`/`[branch]` attributes on the ubershader; int64 helpers in `shaders/int64.hlsli`.
- `cs_conformance` (M0) checks the int64 helpers, the Z formula and the depth encoding against the C
  implementation on hardware and WARP.

---

## 10. Differential test harness

### 10.1 gs_replay modes

`scripts/gs_replay.ps1 -Rec <file> -Backend sw|hw|both|hwdry [-Scale N] [-Aspect a] [-Adapter
hw|warp] [-Ref] [-Lockstep] [-Out dir] [-Metrics file.json] [-Bench]`, reading the 13 recordings
in place (paths in section 10.4).

- **hwdry**: frontend only (classification, records, ownership), no GPU; prints counters.
- **hw**: GPU backend; `-Out` writes pictures; `-Bench` reports frontend ms, render ms, GPU ms
  (timestamp queries per pass class) per VBlank.
- **both**: sw and hw consume the same stream in one process.
  - **Lockstep** (`-Lockstep`): at every FRAME/ZBUF change and every VBlank, download sample 0 of all
    GPU-owned pages and compare with `gs_vram` per page, per channel, per format; Z as integers. On
    a mismatch beyond tolerance, bisect automatically to the first diverging draw (replay with
    compares after every draw in the failing interval) and dump its state + vertices in gs_census /
    PS2_GS_TRACE format, plus crop PNGs.
  - **Picture**: `frame_N_sw.png`, `frame_N_hw.png`, diff heat map; at S>1 the hw picture is
    box-downsampled and also sample-0-decimated.
- `scripts/gpu_diff_all.ps1` runs all recordings and gates against `tests/gpu/baseline.json`.
- `metrics.json` per VBlank: pixel counts above |d| 0/2/16 per channel, PSNR, bbox, counts of D3D
  draws, merges, FF/ROV/ALIAS16/PUNT draws, ROV runs, zresolves, shadow copies, loads, downloads,
  decodes, CLUT slots, frontend/render/GPU ms.

### 10.2 Tolerances (sample 0, any scale)

| class | tolerance |
|---|---|
| Z (all formats), coverage / pixel presence (incl. outline pixels), alpha byte | 0 |
| ROV tier, ALIAS16, 1:1 target copies, clears, untextured flat/Gouraud, FST=1 textured, sprites | 0 |
| G channel between shuffle 1 and shuffle 3; alpha after shuffle 2; darken indices | 0 |
| FF dual-source blends | +-1 |
| FF chains (fog + blend, pass A + pass B) | +-2 |
| Perspective-textured pixels (STQ float differences) | >= 99.5% within +-2, <= 0.1% above 16, per frame |
| REF mode vs sw | as above with FF rows not applicable |
| FF vs REF at S > 1 (all samples) | same rows; this is how scaled FF classes are verified |

Whole-picture gates (reference compose, S=1): PSNR >= 40 dB, <= 0.2% pixels differ by > 16. At S>1:
sample-0 decimation must meet the S=1 gates; box-downsample PSNR >= 32 dB and mean signed error
< 0.5 per channel (catches post-pass misalignment); regression gate vs baseline: <= -0.25 dB and no
doubling of > 16 pixels.

### 10.3 Unit tests

`tests/unit/gs-hw` (headless; hardware adapter and WARP; per-adapter baselines where WARP rounds
differently) reuses `tests/unit/gs-draw` scenes through `-Backend both` at S=1 and adds: wrapping G
add/sub, FBMSK bit planes and Cd*2>>7, DATE, the shuffle triplet vs brute-force
`gs_px_read/gs_px_write`, T8H from a target, feedback runs, Z-region aliasing sequence, exact Z ties
(line vs triangle GREATER at equal integer Z), pass-A straddle with self-overlap, 10k random lines vs
the DDA, coordinate convention at k = 1, 2, 3, design 3's measured constants (G steps +-2, offsets
+0.5 / (+0.5,+1.0) / -7.5,+8.5, As up to 245, constant gameplay XYOFFSET 1792/1936).

### 10.4 Corpus

Read in place, never modified:
`sly2-worktrees\fix-perf\build\{rv_prompt,rv_intro,title,brief}.gsrec`,
`sly2-worktrees\ep3\build\gsrec\{hub,recon_int,rope,brief}.gsrec`,
`sly2-worktrees\ep5\build\gsrec\hub_street.gsrec`, `sly2-worktrees\ep6\build\gsrec\rc_battle.gsrec`,
`sly2-worktrees\ep7\build\boss.gsrec`, `sly2-worktrees\ep8\build\gsrec\{hub,boss}.gsrec`;
plus milestone C's new recordings (ep1 hub + mission, ep2 hub + photo mission, ep4 hub + mission,
prologue, an FMV, a loading screen, pause and save menus, the ThiefNet/safehouse menus), stored under
the C worktree's `build\gsrec\` and listed in `tests/gpu/corpus.txt`.

### 10.5 In-game checks

- `PS2_GS_BACKEND=hw PS2_GS_VERIFY=N`: the sw backend also consumes the stream (on its usual
  workers); every Nth VBlank both are synced, GPU-owned pages are downloaded (sample 0) and compared
  with `gs_vram` under 10.2; a mismatch dumps both pictures and starts a `PS2_GS_RECORD` of the next
  60 VBlanks for offline bisection.
- Warp sweep (`scripts/warp_run.ps1 -Job ... -Direct -Release`, notes/warp.md): prologue, the hub of
  every episode 1-8, one job per episode (m1_brawl, m2_rc_chopper, m3_heist, m4_hack, m5_hack,
  m6_paraglide, m7_boss, m8_boss), the ep2 photo mission, Binocucom briefing, title, menus, an FMV.
  Each with `PS2_DUMP_FRAMES` at S=1 and S=3 for hw and sw, `compare_frames.py`, contact sheets, and
  `PS2_PERF=1` per-thread ms. A playable run per episode with verify N=60 for 5 minutes.

---

## 11. Milestones

Each milestone is one autonomous agent task in its own worktree
`C:\Users\tanih\Projects\sly2-worktrees\gpu-<id>` on branch `gpu/<id>`, created from the integration
branch **`next/gpu`** (created from main at M0, including this document). The agent keeps
`notes/wip/gpu-<id>.md` per `notes/agent-checkpoints.md`, commits after each step, and merges back
into `next/gpu` only when its verification passes and `PS2_GS_BACKEND` still defaults to sw with
`tests/run_tests.py` and all unit suites passing. Heavy runs stay modest (other agents share the
machine): GPU timings are medians of repeated runs.

| id | name | depends on | effort |
|---|---|---|---|
| C | Corpus extension | - (parallel) | 2-3 days |
| M0 | Harness, render thread, all-fallback backend | - | 3 days |
| M1 | Frontend dry run (classifier, records, ownership) | M0 | 3-4 days |
| M2 | Submission go/no-go benchmark | M1 | 2 days |
| M3 | REF tier core at S=1 | M2 go | 7-9 days |
| M4 | Exact effects: ALIAS16, target views, feedback, PUNT | M3 | 4-5 days |
| M5 | FF tier (fast path) behind lockstep | M4 | 5-7 days |
| M6 | In-game integration (opt-in) | M5 | 3-4 days |
| M7 | Resolution scaling and presentation quality | M6 | 4-5 days |
| M8 | Episode sweep and corpus gate | M7, C | 3-4 days |
| M9 | Performance | M8 | 4-6 days |
| M10 | Widescreen renderer side | M7 (+ EE patch task) | 2-3 days |
| M11 | Hardening and default switch | M8, M9 | 3 days |

Total ~45-55 agent-days. The software renderer stays the default until M11.

### C. Corpus extension
- **Deliverable**: new .gsrec recordings with the existing sw renderer (`PS2_GS_RECORD`,
  `PS2_GS_RECORD_FROM/FRAMES`) via `warp_run.ps1`: prologue, ep1 hub + m1_brawl, ep2 hub + photo
  mission, ep4 hub + m4_hack, an FMV, a loading screen, pause/save/ThiefNet menus, with a TRXDIR=1 /
  TRXDIR=2 / CLUT-from-drawn-memory counter added to `scripts/gs_census.c`; census reports for each;
  `tests/gpu/corpus.txt`.
- **Files**: scripts/gs_census.c, scripts/census_table.py, tests/gpu/corpus.txt,
  notes/wip/gpu-C.md (recordings stay in build/, not committed).
- **Verification**: every recording replays through `gs_replay -Backend sw` and gives pictures
  matching `PS2_DUMP_FRAMES` from the live run; census table updated in notes/d3d11-renderer.md
  appendix with anything new (readbacks, new formats, new blend forms) flagged.

### M0. Harness, render thread, all-fallback backend
- **Deliverable**: `runtime/gpu/` skeleton; `gpu_d3d11.c` device creation (HWND or headless,
  hardware or WARP, FL 11_1) with caps check (ROVsSupported, TypedUAVLoadAdditionalFormats,
  dual-source, D3D11.1 constant buffer offsets); `scripts/hlslc.c` + build integration;
  `cs_conformance` (int64 helpers, Z formula, depth encoding round trip); render thread + command
  chunks; an **all-fallback hw backend** that runs sw underneath and only uploads/presents the sw
  picture through the render thread and new swapchain code; gs_replay `-Backend sw|hw|both`,
  `-Adapter`, `-Metrics`; `scripts/gpu_diff_all.ps1`, `tests/gpu/baseline.json`.
- **Files**: runtime/gpu/*, scripts/hlslc.c, scripts/build_game.py, scripts/gs_replay.c/.ps1,
  scripts/gpu_diff_all.ps1, tests/unit/gs-hw/*.
- **Verification**: conformance kernel bit-identical vs C on the 4060 Ti and on WARP (WARP ROV support
  recorded); `-Backend both` on all 13 recordings: 0 differing pixels; `tests/run_tests.py`, gs-draw
  (4 variants), gs-mem, window unit tests pass; sw replay hashes unchanged.

### M1. Frontend dry run
- **Deliverable**: `gpu_front.c`, `gpu_own.c`, `gpu_clut.c` hooked into gs_draw.c; `gs_setup.c`
  refactor of plane/rowstep/line-DDA code (no arithmetic change); tier classification; StateRec
  dedup; vertex/index/record emission; target table; page/lane ownership; block validity; CLUT ring
  keys and min/max alpha; `-Backend hwdry` counters including the pass-A straddle fraction, the
  As > 128 fraction, the Z range per format, predicted D3D draw count and ROV runs.
- **Files**: runtime/gs_draw.c, runtime/gs_setup.c/.h (new), runtime/gpu/*, scripts/gs_replay.c.
- **Verification**: sw replay VRAM hashes identical before/after the refactor on all 13 recordings
  (every VBlank); unit tests pass; hwdry on all recordings: 0 PUNT, 96 ALIAS16 sprites per frame,
  shadow volumes on ROV, ZB_ONLY Z clears recognised as full clears, no Z32 above 0x3F000000;
  counts consistent with the census tables.

### M2. Submission go/no-go
- **Deliverable**: the render thread executes hwdry's command stream with real state objects,
  bindings, buffers and a trivial PS (no correctness), measuring CPU microseconds per D3D draw and per
  frame on ep8 boss, ep3 hub, ep5 street.
- **Files**: runtime/gpu/gpu_d3d11.c, scripts/gs_replay.c (`-Bench`).
- **Verification**: report in the WIP note. **Go** if the worst frame of ep8 boss submits in <= 8 ms
  on the render thread (median of 5 runs) and the GS frontend is <= 6 ms. **No-go** -> before M3,
  apply in order: commutation batching of disjoint bboxes (design 3), D3D11.1 constant-offset
  batching of records, two deferred contexts; if still no-go, write `notes/wip/gpu-M2-fallback.md`
  re-planning the mesh passes on design 2's cs_5_0 tile rasterizer against the existing sw
  arithmetic, and stop for owner review.

### M3. REF tier core at S=1
- **Deliverable**: targets (colour, depth, zpatch), `vram_buf` with ordered upload patches, `swz_tab`,
  CLUT ring, `ps_gs` ROV tier for triangles/sprites/line pixels/points with full records, exact Z,
  ZTST/ATST/AFAIL/DATE/blend/COLCLAMP/FBA/FBMSK/PABE/DTHE in integers, direct texture sampling for all
  PSMs incl. T8H/4HL/4HH from memory, GS bilinear, CLAMP/REGION modes, TEXA, LOD/mipmaps; clears;
  downloads (sample 0); reference compose; `-Ref` and `-Lockstep` with bisection.
- **Files**: runtime/gpu/*, runtime/gpu/shaders/*, scripts/gs_replay.c.
- **Verification**: gs-draw unit scenes through `-Backend both -Ref` on hardware and WARP: 0 diff
  except perspective-textured pixels within tolerance; lockstep on title, rv_prompt, ep8 hub with no
  divergence before the first post pass (shuffles are M4).

### M4. Exact effects
- **Deliverable**: ALIAS16 CS with LUT and hazard split, target CT32/CT24 and alpha-index views, 1:1
  rule with native-unit bilinear, shadow copies with written-since-copy bitmaps, byte-lane hazards,
  stale-block load and download, aliased targets (Z16S, RTTs), PUNT path, phase labels.
- **Verification**: lockstep in REF mode on **all 13 recordings, every VBlank and every FRAME change**,
  within 10.2 (0 on Z, alpha, G between shuffles, ALIAS16); 0 PUNT, 0 downloads; post-pass channel
  results equal gs_post_probe's on ep3 rope f5-f6 and fix-perf brief f3-f4.

### M5. FF tier behind lockstep
- **Deliverable**: FF permutations, the blend table (4.2), dual-source, exact `SV_Depth`, ATST
  handling with As-range and split, tiled-fill -> Clear, StateRec-based draw merging, ROV runs with
  zpatch + `ps_zresolve` in a mixed frame, pass-A straddle escalation flag. Classes are moved from ROV
  to FF one at a time, each gated by lockstep vs REF and vs sw.
- **Verification**: all 13 recordings within 10.2 in default (mixed) mode; a report of pixels where
  the pass-A split differs from REF (decides the escalation default); D3D draw counts and render ms
  per recording within the M2 budget.

### M6. In-game integration (opt-in)
- **Deliverable**: `PS2_GS_BACKEND=hw`, window_gpu.h, swapchain on the render thread (FLIP_DISCARD,
  waitable, tearing), resize/fullscreen, pacing modes, device-lost recovery and sw fallback,
  hint register 0x7F plumbing (no producers yet), `PS2_GS_HW_STATS`, `PS2_GS_VERIFY`, live backend
  switch hotkey (debug).
- **Files**: runtime/ps2_runtime.c, runtime/gs_thread.c, runtime/gs.c, runtime/window.c,
  runtime/window_gpu.h, runtime/gpu/*.
- **Verification**: game boots to title and gameplay with hw at S=1; `PS2_DUMP_FRAMES` hw vs sw at
  the fix-perf scripted path and ep3/ep5/ep8 hubs within 10.2; per-thread ms: GS <= 6, render <= 8,
  60 flips/s on a quiet machine and with the usual background load; 5-minute verify run per hub clean;
  tests/unit/window passes.

### M7. Resolution scaling and presentation quality
- **Deliverable**: scale families, sample grid, single-rate sprites at scale, 1:1 reads at scale,
  presenter resolve (box/Lanczos/Catmull-Rom/point, grid-shift compensation), progressive display
  path, optional analytic thin-outline mode, `PS2_GS_SCALE`.
- **Verification**: at S=2, 3, 4 on all recordings: sample-0 decimation meets the S=1 gates; FF vs
  REF at S=3 within 10.2 on all samples; box-downsample PSNR >= 32 dB and mean signed error < 0.5;
  contact sheets of shadows, outlines, darken, motion blur and Binocucom at 1x/3x committed under
  notes/ (small PNGs) for the owner.

### M8. Episode sweep and corpus gate
- **Deliverable**: run the gate on C's recordings and the in-game warp sweep (10.5); fix every
  divergence or PUNT found (generic fixes only; any Sly-specific rule needs owner sign-off).
- **Verification**: `gpu_diff_all.ps1` green on the full corpus at S=1 and S=3 with 0 PUNT; the photo
  mission works with hw at S=3 (readback count logged); sweep screenshots hw vs sw for every episode.

### M9. Performance
- **Deliverable**: R8 index atlas for T4/T8 (Gather + CLUT), commutation batching, state/SRV binding
  caches, GPU timestamps per phase in the overlay, optional conservative-depth variants, optional
  stencil G-counter path (only if ROV volumes exceed 2 ms at S=3), any frontend hot spots.
- **Verification**: every optimisation bit-exact against the un-optimised hw output at S=1 and S=3
  (lockstep hw vs hw); ep8 boss / ep3 hub / ep5 street at S=3: GS <= 6 ms, render <= 8 ms, GPU
  <= 8 ms (median), and at 4K-class output (S=4/5) GPU <= 12 ms.

### M10. Widescreen renderer side
- **Deliverable**: `PS2_GS_ASPECT`, anamorphic `kx`, HUD VS transform with anchors, pillarbox hint,
  coordination note for the EE projection/HUD hook task (hint call sites it must emit).
- **Verification**: replays with synthetic hints (`-Aspect 16:9`): HUD-hinted bboxes inside the
  centre 4:3 band, full-screen passes cover the full width, 4:3 output identical to M7; in-game 16:9
  screenshots once the EE patch exists.

### M11. Hardening and default switch
- **Deliverable**: eviction, no-ROV copy path (disjoint groups + shadow copies, design 1/3), WARP
  run of the game (slow), device-loss simulation, shader cache, `notes/gpu-renderer.md` (as-built
  documentation), then default `PS2_GS_BACKEND=hw` with sw kept as fallback setting.
- **Verification**: fallbacks exercised by unit tests; full corpus gate green; owner sign-off on
  the default quality settings (section 13).

---

## 12. Risks and mitigations

| risk | mitigation |
|---|---|
| D3D11 submission cost (6-8k draws in ep8 boss, ~1 us each) | M2 go/no-go before any shading work; StateRec merging; commutation batching; deferred contexts; design 2's compute rasteriser for mesh passes as the documented fallback. |
| ROV throughput for shadow volumes at high scale | Volumes merge into one draw per frame; M9 measures; stencil G path (design 3) as an optional, lockstep-gated optimisation. |
| `SV_Depth` disables early-Z | Budget allows it at 3x; M9 conservative-depth variants; depth-only pre-pass not needed. |
| Hardware coverage tie rule differs from the GS rule in some edge case | M3 coverage unit scenes and lockstep catch it; fallback: exact edge test from the record with a 1/256 px expanded triangle. Huge off-screen triangles clipped by the guard band: CPU pre-clip if lockstep shows divergence. |
| Pass-A split reorders visible pixels | Measured in M5 vs REF; escalation to ROV tier (exact via zpatch) by flag. |
| Perspective STQ tolerance hides real bugs | Tolerances only on textured-perspective pixels; Z, alpha, coverage, integer classes exact; REF mode at S>1 as a second oracle; bisection makes every reported divergence inspectable. |
| Ownership/validity bookkeeping bugs (stale textures) | Exact-base-only targets, counters, lockstep at every FRAME change, in-game verify. |
| Unrecorded episodes use new GS patterns | Milestone C before bring-up; generic tiers; PUNT counter; in-game verify; M8 sweep. |
| FXC compile time/miscompiles on the integer ubershader | Build-time compile, `[loop]/[branch]`, conformance kernel, every permutation run on hardware and WARP in unit tests. |
| WARP may lack ROV in D3D11 | M0 records it; headless tests then use the hardware adapter (shared GPU, repeated runs) and WARP covers only FF paths. |
| Threading/device handover (resize, fullscreen, device loss across threads) | Render thread owns all D3D objects; window thread only posts events; window unit tests extended in M6. |
| Upload volume (~5 MB vertices/records per frame over PCIe x8) | Records only where needed (Z record 32 B; full record only for ROV tier); NO_OVERWRITE rings; measured in M2. |
| Widescreen depends on EE patches and hints | Renderer side is complete without them; HUD stretches until the EE task lands. |
| Shared machine distorts timings | Medians of repeated runs; gates also checked under the usual background load. |

---

## 13. Open questions for the owner

1. **Default quality**: proposed 3x internal (1536x1344 for 4:3) with Lanczos downsample, VRR when
   available. Acceptable, or prefer a resolution-target setting (e.g. "match window")?
2. **Widescreen**: 16:9 only together with the EE projection patch and HUD hints (HUD at 4:3
   proportions, movies/menus pillarboxed). Should 16:9 become the default once those exist?
3. **Original effects**: keep motion blur on by default (it is the game's own effect; the "No Motion
   Blur" EE switch at 0x160FA0 can be an option)? The TV flicker filter and field weave are dropped
   in the GPU path; confirm that is wanted.
4. **Outlines at high resolution**: keep the PS2 look (outlines one native pixel wide, i.e. thicker
   at 3x, exact) by default, or thin analytic hi-res outlines (M7 option)?
5. **Merge policy**: milestones merge into `next/gpu`; may passing milestones also merge into main
   (hw stays opt-in, sw default unchanged), or should everything stay on `next/gpu` until M11?
6. **GPU support**: is it acceptable that the first release requires a ROV-capable GPU (D3D feature
   level 12_1-class hardware, e.g. NVIDIA since Maxwell 2), with the no-ROV path only in M11?

---

## Appendix B. As built: M0 (branch gpu/M0)

- **Interface** `runtime/gpu/gpu.h`: `gpu_start(cfg)` (adapter hw/WARP, `hwnd` or headless offscreen
  present target, timestamps) creates the render thread, which creates the device (FL 11_1, falls
  back to 11_0 with `caps.full = 0`), checks caps and, with a window, the swap chain
  (FLIP_DISCARD, 3 buffers, waitable object with latency 1, ALLOW_TEARING when supported).
  `gpu_picture()` = the all-fallback backend (sw picture -> data ring -> upload -> `ps_compose`
  into `picture[3]` -> present). `gpu_read_picture()` = asynchronous read-back (staging ring of 4,
  `Map DO_NOT_WAIT`). `gpu_run_sync(fn)` runs code on the render thread (used by the conformance
  test; M1-M3 can use it for bring-up). `gpu_take_frame_stats()` = per-picture D3D call counts,
  front/render CPU ms, GPU ms (timestamp queries). `gpu_debug_state()` for hang diagnostics.
- **Command rings** `gpu_cmd.c/.h`: SPSC byte rings with free-running 64-bit positions (4 MB
  commands published every 64 KB or on flush, 64 MB data arena freed by `data_end` per command).
  Exactly one producer at a time; the picture callback on gs_thread's pixel worker is one.
- **Render thread loop** (`gpu_d3d11.c`): execute commands; when idle, Flush, finish pending
  read-backs (blocking only when nothing else is queued), then sleep in WaitOnAddress.
- **Shaders**: `runtime/gpu/shaders/perms.txt` -> `scripts/hlslc.c` (System32 d3dcompiler_47,
  `/O3`, strictness, warnings as errors) -> `build/gen/gpu_shaders.h` (`gpu_shaders[]`,
  `GPU_SH_<name>`; shader objects created lazily by `gpu_vs/ps/cs(id)`). Built by
  `scripts/build_game.py`, `scripts/gpu_shaders.ps1` (replay, unit tests). Runtime shader
  directory (`PS2_GPU_SHADER_DIR`) is not implemented yet.
- **Conformance** `gpu_conf.c` + `shaders/int64.hlsli`, `conformance.hlsl`: int64 add/sub/mul/
  mul_i32/asr/shl/floor-div-small, the exact Z formula of 3.6 (`gs_z16`, `gs_z_clamp`), the
  encoding, plus an SV_Depth round trip through D32S8 (raw bits, SRV decode, GREATER/GEQUAL ties).
  2 x 131072 vectors + 65536 texels: **0 mismatches on the RTX 4060 Ti and on WARP**.
- **Caps found**: 4060 Ti and WARP both FL 11_1 with ROVsSupported=1,
  TypedUAVLoadAdditionalFormats=1, dual source, ConstantBufferOffsetting=1, doubles=1; swap chain
  waitable and tearing on this machine. WARP can therefore run ROV-tier tests headless.
- **fxc pitfall** (found by the conformance kernel): `(uint)clamp((int)u + d, 0, K)` with a uint
  `u` compiles to `iadd; umin` (no max with 0, unsigned min). Clamp signed values built from uint
  casts with explicit comparisons only, and give every new integer helper a conformance case.
- **Harness**: `scripts/gs_replay.ps1 -Backend sw|hw|both -Adapter hw|warp -Metrics f.json -Log f`
  (sw-hash of all sw pictures in every mode; both: exit 3 on any differing pixel; watchdog exit 4
  after 60 s without progress). `scripts/gpu_diff_all.ps1 [-Update]` gates the 13 recordings
  against `tests/gpu/baseline.json` (frames, sw hash, max_diff_px 0, min_psnr). M0 result: 13/13,
  2036 pictures, 0 differing pixels; render-thread median ~0.15 ms, GPU ~0.17 ms per picture.
- **Unit tests** `tests/unit/gs-hw/run.ps1 [-Adapter hw|warp|both] [-NoWindow]`: caps,
  conformance, ring stress (both rings wrap, SKIP padding, run_sync), picture round trip (sizes
  1x1 .. 1024x1024, display off), 120 GS scenes through gs_thread with 3 workers in "both" mode,
  restart, swap chain on a small window with a resize.
- **Game**: `scripts/build_game.py` compiles the shaders and links `runtime/gpu/*.c`;
  `PS2_GS_BACKEND` defaults to sw and anything else only logs that the GPU backend is not in the
  game yet (M6 wires it to window.c).

## Appendix A. Per-thread budget targets (ep8 boss, S=3)

| thread | today (sw) | target (hw) |
|---|---|---|
| EE | ~7.7 ms | unchanged |
| GS (frontend) | ~16.4 ms + 8 workers | <= 6 ms (decode 2-3, classification/records 1-2, ownership 0.5-1) |
| render | - | <= 8 ms (submission; M2 gate) |
| GPU | - | <= 8 ms (estimate 2-4 ms at 3x before M9) |
