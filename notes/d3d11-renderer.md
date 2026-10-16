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

*(M2: decode and frontend together cost 7-10 ms per game frame, over the GS budget, so the frontend
runs on a **frontend thread** of its own: the GS thread decodes and a proxy ships primitives, state
deltas, transfer contents, CLUT changes and VBlanks through an SPSC ring (`gpu_fthread.c`); the
frontend's output is identical to the same-thread one. Appendix D.)*

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
  *(M1: rows grow with the drawn/read extent instead; a scissor taller than the drawing made a field
  buffer overlap the ep2 photo copy. Same-kind targets overlapping at another base are separate
  targets too: per-block ownership makes the aliasing exact and counted, no PUNT. Appendix C.)*
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

*(M5: the FF pixel shader evaluates the fragment exactly like the REF tier from per-primitive records
(colour, fog, U/V and Z in the software renderer's integer stepping; S/T/Q interpolated by the hardware in
the default mode), so the only inexact FF class is the dual-source lerp: the RTX 4060 Ti blends UNORM8
targets at about fp16 precision, +-1 for any source bias (0.42/a balances the errors; WARP is exact with
0.496/a unless Cs*C < 64). The two-draw ATST split is made exact by starting a new draw where a pass-capable
primitive covers a pixel of an earlier fail-capable one of the same draw (CPU span bitmap). Appendix G.)*

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

*(M5: the split differed visibly (ep6, ep2 hub, ep1 hub, m1 brawl: differences above 16); the escalation
made those recordings exact but cost more GPU time than REF in the hubs (every D3D11 draw writing a UAV
waits for the previous one; each FF <-> UAV switch ~20 us), so the default is the exact split of 4.2 on
the FF tier; the escalation flag stays (`straddle_rov`). Depth in mixed frames: the DSV and zpatch are
synced lazily per 16x16-pixel tile instead of the serial scheme above; ROV draws use the REF shader
with zpatch as depth. Appendix G.)*

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

*(M1: the T8H darken writes alpha too (FBMSK 0), so it is a real feedback hazard: one shadow copy
of the blocks written since the last copy per darken pass, about one per game frame. Appendix C.)*

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

*(M4: built as **ALIAS16 runs** instead of a sprite-only CS: a gather CS copies the run's rectangle of
the target's halfwords into an R32_UINT staging texture in the 16-bit frame's own pixel space (every
sample), the run's primitives - any class, with Z, DATE, DTHE - are ordinary REF/ROV draws on it with
`ps_gs` in 16-bit mode, and a scatter CS writes the words back. ROV order makes overlapping writes
exact, so only a read of something written earlier in the run splits it. No PUNT for 16-bit frames.
Appendix F.)*

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

*(M4: the frontend sets `gs_front_swdraw`, gs_draw.c rasterizes that primitive into gs_vram and calls
the `sw_done` hook; the touched GPU data is downloaded and read back into gs_vram first (compared
with it when the software renderer runs too). Same-thread frontend only. Remaining triggers: textures
reading GPU data not at a target's base/width, or a target in a format other than CT32/24, CT16 or
an alpha index. `PS2_GPU_PUNT` forces PUNTs for tests. Appendix F.)*

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
| 13 | FMV and uploaded loading screens in display pages | CPU-owned display pages are presented from `vram_buf` via `cs_upload_target` at native resolution (bilinear/bicubic to the output). A movie hint (8.3) pillarboxes. When movies move to native playback (principles), the decoded frame is presented directly. **Measured (milestone C, appendix B): Sly never uploads into display pages.** Movies (and the SCEA legal still) are ~600 16x16 CT32 macroblock uploads per frame into a TBW 12 (768-wide) texture at TBP 1800, drawn by one 512x224 DECAL sprite per field into the normal frame buffer; the loading screen is drawn geometry. So FMV is an ordinary upload + textured sprite (needs upload coalescing and non-power-of-two TBW); `cs_upload_target` stays only as the generic fallback. |
| 14 | Exact Z (13-17-bit scene Z, ties decide outlines and decals) | Section 3.6. |
| 15 | Readbacks (TRXDIR=1, suspected in the ep2 photo mission), CLUT from drawn pages, TRXDIR=2 | Download of sample 0 (exact at any scale) then the existing CPU code; TRXDIR=2 between compatible targets with exact geometry is a scaled CopySubresourceRegion, otherwise download + CPU transfer + invalidate. Counted. **Measured (milestone C, appendix B): none of the three occurs in any of the 24 recordings, the ep2 photo included** (the photo is a GPU-side 2:1 copy of both field buffers into the Z pages at FBP 070, sampled with REGION_CLAMP; the framing test is EE-side). The paths stay as untested safety nets with a counter. |
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
- *(M1: ep4/ep5 load a 256-entry CLUT at CBP 1000 every frame whose 16x16 block holds only ~112
  uploaded entries; the rest was last written by the Z buffer. Those entries are never indexed, so
  the frontend counts such loads and does not download; lockstep checks it. Appendix C.)*
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
the C worktree's `build\gsrec\` and listed in `tests/gpu/corpus.txt` (all 24 with scene, VBlanks and
what each exercises; census results in appendix B).

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

## 13. Owner decisions (2026-10-14) — binding for M6-M11

- **Default quality: 3x internal + sharp downscale** to the window/monitor (as proposed in Q1).
- **Widescreen: 16:9 by default** once the EE projection patch and HUD hints exist; 4:3 stays an
  option.
- **Outlines: BOTH, as a user setting.** "PS2" (one native pixel wide, exact) and "Thin" (analytic
  hi-res outlines). Both must be implemented (M7) and switchable at run time; default "PS2".
- **PS2-era image filters: user settings, with presets.** The owner wants to see both a modern
  look and a faithful old look. Provide individual toggles — motion blur (the game's own effect;
  the "No Motion Blur" switch at 0x160FA0), TV flicker filter, field interlacing (weave) — and two
  presets: **Modern** (default: motion blur on, flicker filter off, progressive) and **Faithful**
  (everything as on a PS2 on a TV). This means the GPU path must also be able to reproduce the
  flicker filter and field display (M7), not drop them.
- All of these settings belong in one settings file / in-game options surface (window.c owns
  persistence), applied without restart where possible.

Remaining open questions:

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

## Appendix B. Corpus extension census (milestone C)

Eleven new recordings (gpu-C worktree, `build\gsrec\`, not committed; list, scenes and how each was
made in `tests/gpu/corpus.txt`): prologue museum gameplay, ep1 hub, m1_brawl, ep2 hub, the ep2 photo
(m2_recon ballroom: Binocucom lock, R1, Polaroid), ep4 hub (Bentley), Bentley's hack minigame (the
m4_hack minigame, reached through ep5's `t5_heist_free_carmelita_hack`: a direct m4_hack start puts
Bentley at an already-hacked station), an IPU movie ("Prelude" from the episode menu), a level load
(title -> loading screen -> m3_heist cutscene), pause menu -> Save and Quit -> "Saving..." ->
restart, and ThiefNet "Buy Gadgets". Every one replays through `gs_replay` (software) to the same
pictures as the live run's dumps (`scripts\gs_corpus_verify.ps1`: 225/226 dumped frames identical at
tolerance 0; the one exception is a recording's very first VBlank, whose alternate lines differ
because the display's field state from before the recording is not part of the stream - a replay
start artefact, not a renderer difference). Census: `gs_census -a` (new: `hazards:` line, every table
entry) over all 24, `census_table.py --new "gpu-C-*"`.

**GPU hazards (TRXDIR=1 readbacks, TRXDIR=2 copies, CLUT loads from drawn pages): zero in all 24
recordings** (old 13: 0 of 2.34M CLUT loads; new 11: 0 of 2.14M). In particular:

- **The ep2 photo does not read GS memory back.** At the shutter the game renders the scene into
  both field buffers, then two ABE=0 CTX2 sprites (FST, `(0-0)*FIX+Cs` unused) copy them 2:1
  horizontally (texels 511x223 -> 256x224) into **FBP 070 = the Z buffer's base**, as a CT32 512x224
  image (left half from 038, right half from 000). The 3D world then stops (20 prims/frame) and the
  Polaroid (4 tristrips + a flat frame) samples TBP 0E00 (= page 070) with **REGION_CLAMP**
  (minu 0 maxu 512 minv 0 maxv 224) for ~180 frames. The framing judgement ("Too far", green lock)
  issues no transfer either (a recording of a failed R1 also showed 0 readbacks), so it is EE-side.
  Design impact: none beyond 5.1/5.3 - a colour target at the Z base whose pages are sampled long
  after they were drawn (page ownership must keep them GPU-owned across frames, not only "this or
  previous frame"), plus REGION_CLAMP in the sampler (first use seen; the old census had none).
- **FMV is not "display pages written by the CPU" (hard case 13).** The movie is 640x480: 1200
  16x16 CT32 macroblock uploads per movie frame (every second VBlank, 1.2 MB) into a texture at TBP
  1800 with **TBW 12** (768 wide; TEX0 1024x1024), then one 512x224 sprite per field (DECAL, TCC 0,
  FST, CLAMP, LINEAR) scales it into the ordinary double buffer. The SCEA legal still after a
  restart uses the same path (16.8k uploads in pause_save). Needs: upload coalescing (600 1 KB
  transfers per VBlank), non-power-of-two TBW in `swz_tab` sampling, and the movie hint for
  pillarboxing; at S>1 the 640x480 source can be sampled at output resolution. TBP 1800 is also the
  T8H darken CLUT's CBP (never both at once).
- **The loading screen** is drawn geometry (black + a dim Sly-mask icon, flat `(Cs-Cd)*As+Cd`
  triangles), no upload; loads from host files last ~0.5 s.

New combinations (no new texture or target formats, no new ALPHA forms; every line below is a
known pass form on a texture/test combination the old 13 did not have):

| recording | novel (vs the old 13) | handling |
|---|---|---|
| ep2_photo | REGION_CLAMP CT32 sampling of the Z-page photo; flat ABE=0 colour-only (ATST NEVER FB_ONLY) quads; 2:1 field copy sprites into FBP 070 | sampler wrap modes in integer texel space (4.5); target-as-texture (5.1) |
| fmv, pause_save | CT32 TBW 12 DECAL TCC 0 sprite from uploaded macroblocks | ordinary upload + sprite (FF) |
| ep1_hub, m1_brawl, thiefnet, pause_save, ep2_hub | outline-style pass pairs `ATST NEVER AFAIL RGB_ONLY` with `(Cs-0)*As+Cd` / `(Cs-Cd)*As+Cd` on T8 and CLAMP/CLAMP textures (old: T4 REPEAT only); `(Cs-Cs)*As+Cd` + `ATST GREATER 0 KEEP` + Z write (= alpha-tested Z and destination-alpha write, RGB kept) on T8 and on CLAMP/CLAMP NEAREST | same FF classes, texture combo is data |
| ep0_museum | pass B with ZTST GREATER (old: GEQUAL); untextured `FBMSK FFFF80FF (Cs-0)*FIX(128)+Cd COLCLAMP 0` (G bits 0-6 wrapping add, old bit-plane step used ABE=0) | ROV tier (per-bit FBMSK, wrap) |
| ep2_hub, ep4_hub, hack_minigame | the pass-A test (`ATST GEQUAL 126 FB_ONLY`, ZTST GREATER, Z write) with the additive blend `(Cs-0)*As+Cd` on T4 REPEAT (old: pass A always `(Cs-Cd)*As+Cd`) | As-range split / ROV escalation as hard case 10 |
| load_m3 | flat untextured `(Cs-Cd)*As+Cd` triangles (fades) | FF |

New maxima (M2 submission budget): 55.8k triangles in one game frame (ep2_hub; old max 50.0k,
ep3 hub), 6.8k draw-state changes and 436 distinct states per game frame (thiefnet: the ThiefNet UI
is drawn over the full safehouse scene; old max 6.07k, ep8 boss); pause menu and ThiefNet keep
rendering the 3D scene behind the 2D UI (52.8k / 53.7k tris), so menus are not cheap frames. The
hack minigame runs a game frame every VBlank (176 of 180) but is light (1.8k tris).

| recording | VBlanks | game frames | tris/frame (max) | state changes/frame (max) | distinct textures | distinct CLUTs | ~Mpx/frame | uploads |
|---|---|---|---|---|---|---|---|---|
| ep0_museum | 150 | 67 | 23852 (24812) | 2964 (3028) | 203 | 221 | 5.5 | 402 |
| ep1_hub | 180 | 87 | 32249 (35808) | 3893 (4378) | 308 | 343 | 3.5 | 696 |
| m1_brawl | 180 | 86 | 20091 (31362) | 2813 (3738) | 223 | 253 | 4.3 | 688 |
| ep2_hub | 180 | 88 | 46209 (55791) | 4991 (5917) | 269 | 290 | 4.0 | 528 |
| ep2_photo | 240 | 51 | 20877 (37760) | 2807 (4607) | 76 | 86 | 6.6 | 0 |
| ep4_hub | 180 | 88 | 35903 (40801) | 3961 (4178) | 364 | 410 | 4.3 | 352 |
| hack_minigame | 180 | 176 | 1841 (2194) | 426 (490) | 6 | 5 | 1.2 | 0 |
| fmv | 240 | 36* | 3892 (3892) | 166 (166) | 2 | 1 | 1.0 | 121200 |
| load_m3 | 240 | 107 | 13186 (16589) | 2459 (2636) | 120 | 130 | 3.3 | 2200 |
| pause_save | 150 | 58 | 52824 (54584) | 4835 (4889) | 279 | 302 | 4.1 | 17148 |
| thiefnet | 100 | 48 | 53684 (53907) | 6818 (6827) | 399 | 430 | 3.7 | 384 |

(*game frame = VBlank with >= 1000 primitives; the movie frames have fewer.) Tools: the census
summary prints the hazards per recording and the first 64 READBACK / COPY and 16 CLUT-FROM-DRAWN
events with the drawing that last wrote the source pages, so a future recording with a readback
shows where its data came from. `PS2_INPUT` token `rec` (+ `PS2_GS_RECORD_FROM=-1`,
`PS2_DUMP_FROM=rec`) starts a recording at a chosen moment, also live through `PS2_INPUT_FILE`
(`scripts\gs_record_scene.ps1 -Live`); `scripts\gsrec_cut.py` shortens a recording.

## Appendix C. As built: M1 frontend dry run (branch gpu/M1)

- **Shared setup** `runtime/gs_setup.c/.h`: vertices, attribute planes (`gs_tri_setup/planes`),
  the 16.16 row stepping (`gs_rowstep_init`, `gs_rowstep_attr`), edges, the sprite rule, the line
  DDA step, the point pixel and `gs_llround` (exact llround without the libcall) moved out of
  gs_draw.c; both backends call them. Proof: GS memory hashed after every VBlank of all 24 recordings
  identical before/after (`scripts/gs_vram_hashes.ps1`, `tests/gpu/vram_hashes.txt`), also with the
  frontend running alongside (`scripts/gpu_hwdry_all.ps1`). Hooks: `gs_front` (prim, transfer
  start/data, TEX0, reset), NULL by default; `gs_sw_draw` switches the software rasterizer off.
- **Frontend** `runtime/gpu/gpu_front.c` (+ `gpu_front.h`, `gpu_front_int.h`): per-state analysis
  cached by a register signature (32 entries; CLUT by ring slot); tier rule 4.1 with per-primitive As
  range (closed-form alpha-test counts) and Z32 check; StateRec 96 B deduplicated per frame; 32-B
  vertices with a 3-entry reuse cache + u32 indices; records Z 32 B / full 128 B / sprite 48 B /
  line pixel 32 B; pipeline-key draw merging, two-draw split marking, ROV runs (bound by colour
  target; depth joins), ALIAS16 dispatches split at halfword-rectangle hazards, tiled fills -> clears,
  a commutation-batching estimate; command stream `GPU_FOP_*` + arenas per frame
  (`gpu_front_set_sink` for M2).
- **Ownership** `gpu_own.c`: per 256-byte block a last writer, a serial and a CPU 32-bit-word mask;
  a target's copy is current iff it holds the block's serial. Loads (stale blocks, bulk per target),
  downloads, vram_buf patches batched until something reads them, feedback shadow copies of the blocks
  written since the last copy, rows that grow with the drawing, eviction after 120 unused frames,
  clean-target fast path. **CLUT ring** `gpu_clut.c`: content change detection, 4096 slots,
  min/max alpha cache.
- **Tools**: `gs_replay -Backend hwdry` (`-FrontOnly`, `-Ref`, `-StraddleRov`, `-SelfCheck`,
  `-DecodeOnly`, `-VramHash`, `-Define`, exit 5 on PUNT, per-VBlank JSON); `PS2_GPU_FRONT_TRACE=<vblank>`
  logs one frame's draws/ops; `scripts/gpu_hwdry_all.ps1`, `scripts/gpu_front_bench.ps1`;
  `tests/unit/gs-front` (24 checks).
- **Decisions that differ from the text above**: same-kind overlapping targets are aliasing, not PUNT
  (3.4); the T8H darken is a feedback copy because it writes alpha (5.1); CLUT loads over Z-written
  words are counted, not downloaded (7); targets created by a CT16 FRAME when the 16-bit view comes
  first (recording starts).

Results on all 24 recordings (hwdry with sw alongside, `-SelfCheck`): **0 PUNT**, VRAM unchanged,
0 Z-record / alpha-count mismatches against the sw code, Z32 vertex maximum 0x7F5C3 (never above
0x3F000000), 96 ALIAS16 sprites in 3 dispatches per game frame wherever the shadow pipeline runs (0 in
the hack minigame and the FMV), shadow volumes on the ROV tier (COLCLAMP/DATE/FBMSK reasons), two
ZB_ONLY/opaque Z clears (Z32 + Z16S) and one colour fill recognised per game frame, steady-state loads
and downloads 0 (only replay-start and eviction ones), uploads -> at most 1 patch per VBlank (FMV:
121200 macroblock uploads), REGION_CLAMP views of the photo target for its 180 frames, TBW 12 FMV
sprites as plain vram sampling.

| recording | VBlanks | D3D draws max / mean | commute est. max | ROV draws / runs max | Z resolves max | StateRecs max | StateRec changes max | records max | prims max | ALIAS16 / frame | pass-A straddle % | As>128 % of lerp kicks |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| fix-perf_rv_prompt | 20 | 606 / 470.6 | 453 | 6 / 4 | 0 | 109 | 486 | 8717 | 16152 | 96 | 52.54 | 0 |
| fix-perf_rv_intro | 16 | 662 / 408.6 | 531 | 12 / 9 | 1 | 132 | 561 | 5804 | 12362 | 96 | 36 | 1.63 |
| fix-perf_title | 30 | 1475 / 688.8 | 1087 | 10 / 7 | 0 | 152 | 1468 | 25100 | 12346 | 96 | 39.19 | 0 |
| fix-perf_brief | 30 | 2226 / 1028.1 | 1642 | 20 / 17 | 2 | 253 | 2010 | 29694 | 26398 | 96 | 39.99 | 0 |
| ep3_hub | 240 | 4633 / 1541.9 | 3373 | 61 / 61 | 52 | 536 | 4044 | 37904 | 50027 | 96 | 38.2 | 3.93 |
| ep3_recon_int | 240 | 2742 / 937.8 | 2101 | 71 / 68 | 67 | 242 | 2548 | 24084 | 29631 | 96 | 27.76 | 8.26 |
| ep3_rope | 20 | 4049 / 1814.1 | 3235 | 39 / 39 | 36 | 481 | 3511 | 35854 | 42664 | 96 | 47.56 | 2.47 |
| ep3_brief | 240 | 2059 / 584.4 | 1524 | 13 / 10 | 0 | 166 | 1947 | 24698 | 16599 | 96 | 40.49 | 0 |
| ep5_hub_street | 240 | 4745 / 1773 | 3487 | 80 / 80 | 68 | 662 | 4365 | 39842 | 42774 | 96 | 22.48 | 2.85 |
| ep6_rc_battle | 120 | 1654 / 532.2 | 1223 | 51 / 48 | 47 | 220 | 1766 | 15334 | 19305 | 96 | 31.35 | 2.99 |
| ep7_boss | 120 | 3744 / 1627.3 | 2918 | 215 / 215 | 214 | 193 | 3361 | 34150 | 23774 | 96 | 63.83 | 22.45 |
| ep8_hub | 240 | 1593 / 677 | 1229 | 40 / 40 | 39 | 96 | 1541 | 10939 | 8558 | 96 | 28.52 | 14.56 |
| ep8_boss | 240 | 5909 / 2390.3 | 5198 | 97 / 97 | 96 | 553 | 5810 | 30981 | 40379 | 96 | 11.08 | 6.54 |
| gpu-C_ep0_museum | 150 | 3046 / 1329.7 | 2240 | 40 / 37 | 1 | 362 | 2676 | 23567 | 24812 | 96 | 46.14 | 0 |
| gpu-C_ep1_hub | 180 | 4315 / 1791.1 | 3281 | 88 / 88 | 75 | 592 | 4023 | 32182 | 35808 | 96 | 28.16 | 4.15 |
| gpu-C_m1_brawl | 180 | 3587 / 1258.5 | 2483 | 83 / 83 | 77 | 497 | 3319 | 30447 | 31362 | 96 | 28.18 | 8.97 |
| gpu-C_ep2_hub | 180 | 5958 / 2290.6 | 4079 | 25 / 25 | 18 | 574 | 5498 | 54985 | 55791 | 96 | 29.02 | 0.62 |
| gpu-C_ep2_photo | 240 | 4390 / 101.5 | 3384 | 86 / 83 | 76 | 196 | 3913 | 38308 | 37760 | 96 | 51.94 | 2.89 |
| gpu-C_ep4_hub | 180 | 4185 / 1922.9 | 3111 | 77 / 74 | 65 | 609 | 3750 | 34042 | 40801 | 96 | 18.95 | 0.85 |
| gpu-C_hack_minigame | 180 | 372 / 300.6 | 222 | 1 / 1 | 1 | 21 | 370 | 15584 | 2194 | 0 | 55.8 | 0 |
| gpu-C_fmv | 240 | 28 / 5 | 22 | 0 / 0 | 0 | 7 | 22 | 6524 | 3892 | 0 | 0 | 0 |
| gpu-C_load_m3 | 240 | 2450 / 1011.6 | 1798 | 18 / 18 | 15 | 175 | 2369 | 21041 | 16589 | 96 | 24.33 | 2.75 |
| gpu-C_pause_save | 150 | 4388 / 1696.5 | 3232 | 18 / 18 | 12 | 498 | 4021 | 45460 | 54584 | 96 | 27.42 | 0.46 |
| gpu-C_thiefnet | 100 | 6512 / 3126.2 | 4862 | 146 / 146 | 122 | 772 | 6207 | 71138 | 53907 | 96 | 25.58 | 4.97 |

GS-thread CPU per game frame (replay thread CPU time, median of 3, machine at 40-90% load from other
agents): decode alone 4.3-6.7 ms, frontend 4.4-9.0 ms (ep3 hub 4.4, ep8 boss 5.8, ep5 street 6.7,
ep2 hub 8.2, ThiefNet 9.0); the synchronous sw rasterizer 39-79 ms. Decode + frontend is 9-16 ms,
above the 6 ms GS budget: M2 must measure on a quiet machine and plan the split (frontend on its own
thread after decode, or cheaper decode).

## Appendix D. M2 submission go/no-go (branch gpu/M2)

**Verdict: GO** for M3. ep8 boss: render-thread worst steady-state frame 4.9 ms (median of 5 runs,
gate 8 ms); GS side 5.4 ms decode thread and 5.3 ms frontend thread per game frame (median, gate
6 ms) with the frontend on its own thread. The same-thread frontend would be NO-GO (7.2 ms). Over the
6 ms GS budget remain ThiefNet (6.7 / 7.1 ms) and the ep2 hub's frontend thread (6.2 ms): M9's
"frontend hot spots" must take ~15-20% off the frontend (list below). No render-side mitigation
(commutation batching, CB-offset batching, deferred contexts) was needed.

**What was built**
- `gpu_exec.c` + `shaders/submit.hlsl`: the render thread executes the frontend's GPU_FOP stream with
  real objects: colour (R8G8B8A8_TYPELESS: RTV/SRV/UAV) and depth (R32G8X24: DSV, read-only DSV, SRV)
  targets at the targets' k, recreated on growth with a copy; vertices, indices, records, StateRecs and
  per-draw constants in dynamic buffers mapped once per frame (WRITE_DISCARD); per-draw constants
  bound with `VS/PSSetConstantBuffers1` offsets (256 B per draw); blend (5 classes x write mask) and
  depth-state caches; a redundancy filter on every binding; FF draws through 24 trivial PS
  permutations (dual source, SV_Depth from the Z record, discard, texture kind), sprites and line
  pixels as instanced quads reading records; ATST splits as two draws; ROV draws through
  `OMSetRenderTargetsAndUnorderedAccessViews` (colour UAV, read-only DSV, depth SRV) and a ROV PS;
  Z resolves and depth loads as SV_Depth quads; masked clears as quads, full ones as Clear*View;
  ALIAS16 batches and colour loads as dispatches; shadow copies; vram_buf patches (one update of the
  covered range through a CPU mirror when a patch has many runs) and CLUT ring slots. Nothing is
  drawn correctly; every D3D call of the design is made.
- `gpu_fthread.c` (frontend thread): the GS thread decodes; proxy hooks copy each primitive (8 bytes
  plus the vertices not in the consumer's 4-entry cache, matched by the decoder's vertex serial
  `gs_vtx.serial`, 32 bytes each), the changed 8-byte words of the gs_prim fields and registers the
  frontend reads (a fixed list; no record at all when nothing changed: ~40% of Sly's state versions in
  ep8 boss are TEX0 rewrites with the same value), transfers with the contents of the blocks they
  wrote, CLUT buffer changes as changed 16-byte pieces, resets and VBlanks into an 8 MB SPSC ring
  (records staged in L1 and written with non-temporal stores; shared positions touched every 64 KB;
  the producer sleeps on a word of its own until a quarter of the ring is free). The frontend thread
  keeps mirrors of gs_prim, the registers, GS memory and the CLUT buffer and calls the frontend's hooks
  in stream order. **The command stream and data of every frame are bit-identical to the same-thread
  frontend on all 24 recordings** (`scripts/gpu_fthread_check.ps1`).
- Frontend fixes found by the executor: a line whose DDA covers no pixel inside the scissor no longer
  opens a D3D draw (82k empty draws in ep8 boss; its predicted maximum fell from 5909 to 5079 D3D
  draws per frame); the vertex reuse compare skipped the last field of gs_vtx.
- Tools: `gs_replay -Backend hwsubmit` (`-Pace` 59.94 Hz VBlanks, `-Scale`, `-FrontThread`,
  `-Profile` sampling profiler of the replay thread, `-StreamHash`, `-Pin`), per-frame render stats
  (`render_ms`, `render_cpu_ms`, `upload_ms`, `state_calls`, GPU timestamps), per-game-frame CPU of the
  GS and frontend threads (`threads:` / `thread-bench` lines), `scripts/gpu_submit_bench.ps1` (median of
  N runs, machine load per run).

**Method.** RTX 4060 Ti, i7-14700F, S=1 (CPU submission does not depend on the scale; GPU time at S=3
is M9's). Replays read the recording into memory first; `-Pace` releases VBlanks at 59.94 Hz so the
render thread is not throttled by a GPU backlog (back to back, its wall time tracks GPU time: ~0.5 us
per draw of GPU front-end work even with trivial shaders). Game frame = VBlank with >= 1000
primitives; "steady" leaves out replay-start frames (targets created, downloads). Medians of 5 runs.
The machine is shared (another agent runs two game instances at times, doubling every per-thread
time): the numbers below come from windows with < 10% other load, checked per run.

**Render thread** (paced, frontend thread on, 5 runs; ms per game frame unless noted)

| recording | D3D draws max | worst steady (5 runs) | worst incl. start | median | CPU worst steady / median | us per draw | GPU median / worst | driver threads CPU |
|---|---|---|---|---|---|---|---|---|
| ep8 boss | 5081 | **4.93** (4.8-5.4 per run) | 9.1 | 1.60 | 1.15 / 0.61 | 0.30 | 2.5 / 3.9 | 2.0 |
| ep3 hub | 4630 | 5.27 | 9.6 | 1.11 | 1.53 / 0.53 | 0.27 | 1.7 / 4.4 | 2.7 |
| ep5 street | 4728 | 5.07 | 8.9 | 1.19 | 1.58 / 0.54 | 0.25 | 2.3 / 5.3 | 3.2 |
| ep2 hub | 5948 | 4.90 | 7.9 | 1.32 | 1.58 / 0.61 | 0.25 | 1.6 / 3.0 | 1.4 |
| ThiefNet | 6513 | 6.73 | 10.8 | 1.94 | 1.84 / 0.82 | 0.31 | 3.3 / 5.1 | 2.7 |

The render thread's own CPU is 0.5-1 ms per frame (~0.1 us per D3D call after redundancy
filtering, 25-32k state calls per frame); its wall time is mostly waiting for the NVIDIA driver's
worker threads, which use 1.4-3.2 ms per game frame. The replay-start frame (3-4 target creations, a
4 MB vram_buf patch, loads) is 8-11 ms: a once-per-boot/level-load cost.

**GS side** (front-only replays, unpaced, quiet, 5 alternating runs; per game frame, median / worst
steady)

| recording | decode only | decode + frontend, one thread | frontend thread: GS (decode + proxy) | frontend thread: frontend |
|---|---|---|---|---|
| ep8 boss | 2.6 | 7.16 / 7.59 | **5.39 / 5.76** | **5.26 / 5.61** |
| ep3 hub | 3.0 | 8.25 / 9.50 | 5.80 / 6.55 | 5.68 / 6.66 |
| ep5 street | 2.8 | 7.74 / 8.39 | 5.37 / 6.23 | 5.52 / 6.67 |
| ep2 hub | 3.1 | 8.73 / 10.24 | 5.86 / 6.92 | 6.23 / 7.52 |
| ThiefNet | 3.5 | 9.76 / 9.93 | 6.70 / 7.24 | 7.10 / 7.54 |

Paced together with the render thread (the submit bench) the two threads read 0.3-0.7 ms higher
(ep8 boss 5.8 / 5.7 median): threads that sleep between frames run on colder caches and clocks, and
Windows may move them to efficiency cores.

**Where the GS-side time goes** (sampling profiles, `gs_replay -Profile`; frontend sections with
`-Define GPU_FRONT_PROF`): decode = vertex queue ~20%, CLUT loads (`clut_load`, 256 swizzled reads per
T8 load, 4-7k loads per frame) ~18%, register dispatch ~25%, env_update / gs_prim_finalize ~10% (Sly
starts a new state version for almost every draw by rewriting TEX0). Proxy = ~1.4 ms per game frame
of logic plus ~1 ms of ring traffic in ep8 boss (2-4 MB per game frame). Frontend = state lookups
19%, tier/records 14%, ownership 12%, pipeline key 10%, vertices 10%, records 8%, texel rectangles 9%
(double math), geometry 7%; nothing dominates.

**What M3 (and later) must know**
- D3D11 needs equal dimensions for the bound RTV and DSV: colour and depth targets that are bound
  together must be allocated at the same size (gpu_exec.c grows the smaller of a pair; M3 should size
  pairs together from the start). Targets are allocated in steps of 64 native rows.
- Records are addressed by byte offset (`rec_first`) and per-draw constants by 256-byte constant
  buffer windows; at ~6.5k draws that is 1.7 MB of constants per frame, mapped once.
- The frontend thread is the GS-side architecture from now on: anything the frontend starts to read
  from gs_prim or the registers must be added to `p_fields` / `r_used` in gpu_fthread.c (or it reads a
  stale value on the frontend thread); `gpu_fthread_check.ps1` catches omissions on the corpus and
  `PS2_GPU_FTHREAD_FULL=1` sends every word. Anything the frontend reads from GS memory or the CLUT
  buffer goes through `gf_vram` / `gf_clut_src` (the frontend thread's mirrors). Downloads (M3/M4)
  that patch gs_vram must synchronise with the decode thread (gpu_front_drain) and update the mirrors;
  writes into vram_buf other than patches must update gpu_exec.c's vram_buf mirror.
- The CPU budget is met on ep8 boss but ThiefNet / ep2 hub exceed 6 ms on the frontend thread; cheap
  candidates for M9: texel rectangles in integers, the state lookup signature (24 words per version),
  a faster `clut_load` in the decoder (fixed offsets for CSM1 CT32), fewer bytes per primitive (no ST/Q
  for FST or untextured vertices), the remaining ~1 ms of ring traffic.
- GPU front-end time with trivial shaders is already 1.5-3.3 ms median (5 ms worst) at S=1 for
  5-6.5k draws: draw count reduction (commutation batching, M9) also matters for the GPU budget.
- Replay-start / level-load frames cost 8-11 ms on the render thread (target creation, 4 MB patch).

## Appendix E. As built: M3 REF tier core at S=1 (branch gpu/M3)

**Result.** Every draw runs on the ROV tier (`ps_gs`) and matches the software renderer bit for bit
wherever M3's scope reaches: lockstep (hw adapter) title 58 comparisons / 0 differ, rv_prompt 39 / 0,
ep8 hub 418 / 0 (also brief 58 / 0, ep3 rope 37 / 0, rv_intro 31 / 0, ep7 boss 230 / 0), no divergence
before the first post pass of any frame; with `-Ref` alone the pictures of title and rv_prompt are
identical, ep8 hub differs where post passes run (M4). gs-draw unit scenes through the REF tier
(`tests/unit/gs-hw/run.ps1 -RefOnly`): 1162 memory comparisons per adapter, **0 differing scenes on
the RTX 4060 Ti and on WARP**; the two scenes with CT16 frames (t_frame16, t_formats_contexts) need
ALIAS16 (M4). No tolerance was needed for perspective texturing: STQ is emulated exactly (below).

**What was built**
- Frontend (gpu_front.c/h, gpu_own.c): full records. `gpu_staterec` (96 B: TEX0/CLAMP/TEX1/TEXA,
  CLUT slot, finalized TEST/ALPHA/FBMSK/Z info, texture levels with MIPTBP replicated from
  `bind_textures`), `gpu_rec_tri` (256 B: colour/fog/UV attributes as the GS's 32-bit wrapped
  row + dy*rows + dx*x, Z as int64, left edges in 64-bit fixed point, S/T/Q as doubles plus the CPU
  renderer's batch-cut table), `gpu_rec_sprite` (64 B, V as a double plane per row like gs_draw),
  `gpu_rec_linepix` (32 B, integer U/V for FST). Checkpoints (`gpu_front_checkpoint` + hook: a partial
  frame closed without PRESENT at FRAME/ZBUF changes and VBlank), `gpu_front_resync`,
  `gpu_front_gpu_blocks`, post-pass counter, `display_ky2` config.
- Ownership (gpu_own.c): patches carry **per-word bit masks** (only the bits a transfer wrote; the M2
  CPU mirror of vram_buf is gone); downloads are GPU-side (writer target sample 0 -> vram_buf, masked
  to the GPU-owned words, `gpu_dl_entry`); an upload that partially covers a GPU-written block
  downloads the block first; `blk_cpu` now means "vram_buf holds the truth"; the initial memory of a
  recording is a full resync.
- Shaders: `gsmem.hlsli` (addressing through `swz_tab`, all PSMs), `mem.hlsl` (cs_patch, cs_load,
  cs_download, cs_clear), `gs.hlsl` (REF VS for triangles / sprite quads / line-pixel quads and the
  `ps_gs` pipeline: exact coverage from the record, Z in integers, ZTST/ATST/AFAIL/DATE/blend/COLCLAMP/
  FBA/FBMSK/PABE/DTHE/SCANMSK, every PSM sampled from vram_buf incl. T8H/4HL/4HH and CLUT CSM1/2, GS
  bilinear, CLAMP/REGION, TEXA, LOD and mipmaps).
- Executor (gpu_exec.c): vram_buf (raw SRV + UAV), swz_tab, zpatch R32_UINT per depth target, REF
  draws as UAV-only passes (u0 colour ROV R8G8B8A8_UINT, u1 zpatch ROV, u2 probe), `gpu_exec_read_memory`
  (4 MB hw image = scratch copy of vram_buf + downloads), counters, `PS2_GPU_PROBE=x,y`.
- gs.c: `gs_compose_from(snapshot, memory, history, ...)` (the reference compose of a hw memory
  image; `gs_compose` calls it with gs_vram, sw output unchanged).
- gs_replay: `--ref` (hw / both: reference compose, picture compare, GPU ms), `--lockstep`
  (`-Lockstep` in gs_replay.ps1: 4 MB compare per checkpoint; skips after the frame's first post pass;
  resyncs at VBlank after one, `--no-resync`), automatic bisection (a child run with
  `--bisect V:A:B` dumps the culprit primitive, differing colour/Z pixels and PNG crops).
- Unit harness: gs-draw with `-DTEST_HW_REF` compares the hw memory with sw before every pixel read
  (`tests/unit/gs-hw/run.ps1`, `-RefOnly`, both adapters); new scenes `t_texture_formats` (every PSM
  from memory, CLUT/TEXA/wrap/bilinear/STQ) and `t_random_states` (fuzz: random states, PSMs,
  filters, LOD, uploads of every format; `PS2_FUZZ_SEED` / `PS2_FUZZ_ITERS`, default 400 iterations).
  On hw additionally seeds 1-6 and 111213 x 3000 iterations (1-4 again with uploads) were identical.

**GPU ms per frame at S=1, REF** (RTX 4060 Ti, `gs_replay --backend hw --ref`, bench path without
read-backs, GPU timestamps, median of the per-run values of runs 2-4; run 1 has a ~1 s first-use spike)

| recording | median per VBlank | median per VBlank with draws | mean | worst |
|---|---|---|---|---|
| title | 2.5 | 2.9 | 2.9 | 6.3 |
| rv_prompt | 1.9 | 1.9 | 1.9 | 4.3 |
| ep8 hub (30 fps) | 0.03 | 6.0 | 2.7 | 6.7 |

(ep8 hub: one rendered frame per two VBlanks, ~6 ms per rendered frame.) With `--lockstep` the
per-VBlank numbers are lower (title 0.8, ep8 hub 1.3) because checkpoint frames are excluded. REF is
the correctness tier; FF (M5) carries the speed.

**Deviations from the design and why**
- Depth truth in REF mode is **zpatch (R32_UINT, raw Z word)**; no DSV is bound in REF and Z resolves
  are ignored. Z24 keeps the stored top byte (the word is written whole, as the GS does).
- vram_buf has **no CPU mirror**: masked patches by CS; CPU copies of hw memory (lockstep, compose)
  download every GPU block into a scratch copy and read that back.
- **Display-family targets run 1x1 in REF** (`display_ky2 = 0`): at S=1 the display family had ky = 2
  while its Z target kept ky = 1, so Z landed on the wrong rows. M7 must give Z targets the factors
  of their colour target.
- REF viewport is 16384 x 16384 **centred on the origin** (TopLeftX/Y = -8192): WARP clipped
  primitives at negative coordinates otherwise. NDC = d3d * 2 / 16384 exactly.
- **Products are rounded before adds** (`dmul_r` / `fmul_r`: multiply by a runtime `one` from the
  cbuffer): the NVIDIA compiler fused dmul + dadd into an FMA despite `precise`.
- STQ is emulated exactly instead of within a tolerance: per-row double accumulation, span start
  `(float)(srow + sdx*xl)`, float stepping in 64-pixel column pieces with the CPU renderer's batch
  cuts (table in the record), 1/q by Newton in double plus a residual-checked neighbour pick, f2i.
- Fixes found by the fuzz scenes (also in the sw-matching logic of the frontend): PABE with A == B,
  D == Cd is not "colour unchanged" (As < 0x80 writes Cs); a writer that rewrites a block after a
  download bumps the block serial so other targets' copies go stale.
- **WARP anomaly**: in some STQ triangles of the fuzz scene WARP's `ps_gs` reads values from the
  triangle record's STQ block (offsets >= 120) that differ from what the CPU uploaded (verified with a
  record dump; NVIDIA reads them exactly; /O0 shaders, Load4 and reordering did not change it). The
  fixed STQ scenes pass on WARP; the fuzz scene keeps triangles on UV when the adapter is WARP. Loops
  in `ps_gs` are bounded so that a bad record cannot hang the device.

**What M4 must know**
- REF skips ALIAS16 batches (counter `alias16_skipped`); post passes (target views, ALIAS16) are
  counted by the frontend (`post_prims`), and lockstep stops comparing a frame after its first one and
  resyncs vram_buf from gs_vram at the next VBlank. M4 removes the skip and must turn
  `--no-resync` on to prove whole frames (gate: all 13 recordings, every VBlank and FRAME change).
- The bisection child (`--bisect`) and `gpu_front_debug_block` (ownership of one block) are the tools
  for post-pass divergences; `PS2_GPU_PROBE=x,y` prints the probe pixel's record values on the GPU.
- Downloads are per writer and masked; anything that writes vram_buf outside patches/downloads (M4's
  ALIAS16 CS, shadow copies) must keep `blk_cpu` / serials / masks consistent with gpu_own.c.
- Unit scenes t_frame16 and t_formats_contexts are the first ALIAS16 targets (they differ now only
  because REF skips ALIAS16); t_texture_cache, t_deferred_hazards and t_direct_order already pass
  while using M4 effects.
- Z targets must get their colour target's scale factors before display_ky2 can be re-enabled (M7).

## Appendix F. As built: M4 exact effects (branch gpu/M4)

**Result.** With every post pass rendered on the GPU, lockstep in REF mode at S=1 with `--no-resync`
(GS memory compared with the software renderer's, all 4 MB, at every FRAME/ZBUF change and every
VBlank, nothing restarted from sw memory) finds **0 differences in 24,092 comparisons on all 24
recordings** on the RTX 4060 Ti **and on WARP**, and every reference-composed picture is identical
(0 pixels); the WARP STQ anomaly of appendix E did not show up in any recording. 0 PUNT; 0 downloads after the
first frames except the FMV recording's one eviction (below). gs_post_probe's channel report (FRAME
changes and VBlank ends) computed from GPU memory (`gs_replay --post-probe`) is identical line for
line to the software one on ep3 rope VBlanks 5-6 (70 lines) and fix-perf brief 3-4 (120 lines).
Unit scenes (`tests/unit/gs-hw/run.ps1 -RefOnly`): 0 differing scenes on the RTX 4060 Ti and on WARP,
t_frame16 and t_formats_contexts included (the harness now counts every differing scene), plus a new
scene t_target_views (in-place T8H read with disjoint byte lanes, in-place CT16 strips in both
directions, a 1:1 bilinear CT32 view); with
`PS2_GPU_PUNT=3:15` (every third primitive PUNTed) 1078 PUNTs, all exact, read-backs equal to gs_vram.
PUNT on recordings: title with every fifth primitive PUNTed is lockstep-clean in "both" mode (184/0),
and in hw-only mode (read-back into gs_vram, the software rasterizer drawing only the PUNTed
primitives) the pictures equal the REF pictures (which equal sw's): title (every 5th primitive, 30
pictures), ep3 rope (every 7th, 20), rv_prompt with every 16-bit-frame primitive PUNTed (no ALIAS16 run
left, 20).

| recording | VBlanks | lockstep comparisons | differ | pictures differ | ALIAS16 runs | post-pass prims |
|---|---|---|---|---|---|---|
| fix-perf rv_prompt | 20 | 196 | 0 | 0 | 48 | 2048 |
| fix-perf rv_intro | 16 | 126 | 0 | 0 | 30 | 1280 |
| fix-perf title | 30 | 184 | 0 | 0 | 42 | 1792 |
| fix-perf brief | 30 | 338 | 0 | 0 | 42 | 1832 |
| ep3 hub | 240 | 1427 | 0 | 0 | 297 | 12656 |
| ep3 recon_int | 240 | 1484 | 0 | 0 | 249 | 10608 |
| ep3 rope | 20 | 127 | 0 | 0 | 27 | 1136 |
| ep3 brief | 240 | 1208 | 0 | 0 | 264 | 11264 |
| ep5 hub_street | 240 | 1403 | 0 | 0 | 291 | 12400 |
| ep6 rc_battle | 120 | 1062 | 0 | 0 | 123 | 5232 |
| ep7 boss | 120 | 1165 | 0 | 0 | 165 | 7040 |
| ep8 hub | 240 | 1920 | 0 | 0 | 315 | 13440 |
| ep8 boss | 240 | 1203 | 0 | 0 | 300 | 12800 |
| C ep0_museum | 150 | 1155 | 0 | 0 | 201 | 8576 |
| C ep1_hub | 180 | 1224 | 0 | 0 | 261 | 11120 |
| C m1_brawl | 180 | 1213 | 0 | 0 | 258 | 11008 |
| C ep2_hub | 180 | 1500 | 0 | 0 | 264 | 11264 |
| C ep2_photo | 240 | 1432 | 0 | 0 | 153 | 7246 |
| C ep4_hub | 180 | 1499 | 0 | 0 | 264 | 11248 |
| C hack_minigame | 180 | 1412 | 0 | 0 | 0 | 2800 |
| C fmv | 240 | 549 | 0 | 0 | 0 | 560 |
| C load_m3 | 240 | 1140 | 0 | 0 | 321 | 14080 |
| C pause_save | 150 | 642 | 0 | 0 | 174 | 7424 |
| C thiefnet | 100 | 483 | 0 | 0 | 144 | 6128 |

(Post-pass prims: ALIAS16 primitives, target-view reads and PUNTs. Steady-state downloads per
recording from `gs_replay -Backend hwdry -Ref` metrics after the first 4 VBlanks: 0 everywhere; loads
0 except ep8 hub 3 and ep8 boss 2 (targets created later in the recording); FMV: 2 downloads (1792
blocks) at VBlank 10009, the eviction of the 3D scene's targets after 120 unused frames - a
transition, by design. A `--scale 2` lockstep (sample 0) of title and ep3 rope is clean too.)

**What was built**
- **ALIAS16 runs** (5.2, generalised): primitives into a 16-bit frame (CT16/CT16S) over a C32 target
  with the same base and width form a run: `GPU_FOP_ALIAS16` gathers the run's rectangle (whole 16x8
  blocks, bbox filled in when the run closes) of the target's halfwords into an R32_UINT staging
  texture in the frame's own pixel space (every sample), the run's draws (tier `GPU_TIER_ALIAS16`, any
  primitive class, with Z/DATE/DTHE) are REF draws on it (`ps_gs` with `GPU_SF_FB16`: 16-bit Cd
  expansion, DATE on bit 15, DIMX dither before COLCLAMP, 5551 packing, 16-bit FBMSK), and
  `GPU_FOP_ALIAS16_END` scatters the words back (one thread per word: pixels x and x + 8 of a block row
  share it). Shaders: `shaders/alias16.hlsl`. Reads of the target (CT16 views of it) come from its SRV,
  i.e. the state before the run, so the frontend splits a run only at a read-after-write hazard
  (rectangles: exact within one view, page overlap across views). A load, grow or other op on the
  run's target ends the run first (`gf_target_op`). The Sly shuffles: 2-3 runs per game frame.
- **Target views** (5.1): CT32/CT24, alpha-index and now CT16/CT16S views of a C32 target anywhere
  (`view_word_at`: GS word address -> page relative to the target's base -> inverse CT32 table
  `SWZ_ICT32` in swz_tab). Feedback with overlapping byte lanes: shadow copy of the blocks written (or
  loaded) since the last copy; disjoint lanes: `GPU_TS_SELF`, the shader reads the bound target in
  place (no copy). 1:1 rule: sprites stepping one texel per pixel and row carry a record flag; with
  the view on the target's sample grid (`vflags`) each fragment reads its own sample (i, j) of the
  native texels (so bilinear taps stay native rows/columns at any scale); other reads take sample 0
  (scaled bilinear for non-1:1 reads is M7).
- **Colour targets as R32_UINT** in the REF pixel shader (a UAV with the typeless 32-bit format cast;
  mem.hlsl keeps the R8G8B8A8_UINT view). StateRec grew to **128 B** (DIMX, phase label, hint label).
- **Ownership** (gpu_own.c): uploads keep a block's untouched GPU words with their writer (only words
  the upload writes partly are downloaded); **cross-target copies** `GPU_FOP_XCOPY` / `cs_xcopy` (a
  stale block wholly written by another target of the same kind and scale is copied over, every
  sample, instead of download + load); loads mark blocks written-since-copy for the shadow.
- **PUNT path** (5.4): `punt_prim` downloads what the primitive touches, hands everything to the
  render thread (checkpoint), reads vram_buf back through a reader the integrator installs
  (`gpu_front_set_mem_reader`; gs_replay and the unit harness use `gpu_exec_read_memory`) into gs_vram
  (hw-only) or compares it with gs_vram (software renderer running: counted in
  `punt_why[GPU_PUNT_READBACK_DIFF]`), sets `gs_front_swdraw`; gs_draw.c binds the textures, rasterizes
  the primitive into gs_vram, syncs and calls the new `sw_done` hook, which patches the written rects
  (CPU data: every target reloads them, replicated). `PS2_GPU_PUNT=<every N>[:<class mask, 16: 16-bit
  frames only>]` forces PUNTs.
- **Phase labels**: every StateRec carries a phase (scene, fill, ROV read-modify-write, ALIAS16, view,
  feedback; plus the label of the last `PHASE` hint, now forwarded in `gs_env_view.phase` and by the
  frontend thread); per-phase primitive counters in the metrics (`phase_prims`) and in the draw trace.
- **Lockstep** compares post passes; without `--no-resync` a differing frame restarts the next one from
  sw memory. `--post-probe F:L` prints gs_post_probe's report from GPU memory. `PS2_GPU_PROBE` also logs
  every non-draw op the executor runs and the sample position of probe writes. `ref-bench` reports GPU
  ms per game frame (>= 1000 primitives).

**Fixes of earlier milestones found by the whole-frame lockstep**
- A blend reading Cd (`(Cd-0)*2>>7`, the bit-plane steps) was taken for a tiled fill (S.blend stays
  BL_OFF for forms outside the FF table): its full-screen strips became a colour clear (title, all
  recordings with shadows).
- The ROV draw key had no targets: a run's draw opened by a primitive without Z merged later primitives
  with Z, which then ran without their depth target (ep6; the bisection child, which checkpoints before
  every primitive, did not reproduce it - a sign of merging/batching).
- Steady-state downloads (34 per game frame in title: an upload of CLUT entries into Z pages
  downloaded the Z words around them) and Z-clear loads (opaque sprites were not opaque on the ROV
  tier) - gone.
- A shadow copy taken before a load of the same blocks went stale (ep2 photo: the darken reads one row
  past its frame buffer, i.e. the other buffer's memory).
- The vertex reuse cache kept its ring position across arena resets (VBlank, checkpoint): a stale entry
  could return the index of a vertex of the previous arena. Found by forced PUNTs inside strips
  (a checkpoint between two triangles that share vertices).

**GPU ms per game frame at S=1, REF** (RTX 4060 Ti, `gs_replay --backend hw --ref`, bench path without
read-backs, GPU timestamps; game frame = VBlank with >= 1000 primitives; medians of runs 2-4 of 4)

| recording | median | worst | mean per VBlank |
|---|---|---|---|
| ep8 boss | 14.6 | 15.9 | 6.2 |
| ep3 hub | 7.5 | 8.6 | 3.0 |
| ep5 street | 7.7 | 10.2 | 3.4 |
| ep2 hub | 9.8 | 12.6 | 4.9 |
| ThiefNet | 14.8 | 15.7 | 7.4 |

REF is the correctness tier (every pixel through the integer ROV pipeline); the FF tier (M5) carries
the speed.

**Deviations from the design**
- ALIAS16 is staging-based for any primitive class, not a sprite CS with a halfword hazard bitmap; no
  PUNT for 16-bit frames (5.2 note).
- CT16 views of C32 targets are allowed outside ALIAS16 (feedback rules apply); TEX_FMT PUNT remains
  only for T8/T4 (and other non-C32) reads of target data.
- Byte-lane "no copy" reads are a ROV-tier mechanism (the shader reads its own UAV); FF draws cannot
  read their RTV.
- Partial uploads over GPU words no longer download (word-level ownership with the writer kept).
- PUNT needs the same-thread frontend; on the frontend thread a PUNT is counted and not drawn.

**What M5 (FF fast path) must know**
- Lockstep `--no-resync` on all 24 recordings is the gate to keep (`scripts/gpu_lockstep_all.ps1
  [-Adapter warp] [-Scale N]`); run it for every class moved from ROV to FF. gs_replay's lockstep is
  wired to `--ref` today: the mixed FF/ROV mode needs the same comparison.
- ALIAS16 draws always execute on the REF path (`exec_draw_ref`), also in non-REF frames: their Z (none
  in Sly's shuffles) is zpatch; in a mixed frame a 16-bit run with Z must see the DSV's depth (resolve
  before / zpatch after).
- `GPU_TS_SELF` (disjoint-lane self reads) only works on the ROV tier; an FF draw with such a view
  needs a shadow copy (or the ROV tier).
- The ROV key now includes ct/zt and the scissor; FF keys already did. The FF classes must keep a
  draw's scissor equal for all its primitives (the executor uses the first one's).
- Fills: `S.fill` now requires `!S.reads_dest`; the FF clear path depends on it.
- Opaque sprites revalidate blocks in every tier; cross-target copies (XCOPY) and loads mark
  `sdirty` - an FF feedback read must keep using `own_shadow_need`.
- REF GPU cost per game frame is 7.5-15 ms at S=1 (above): FF must take the bulk of the draws.

## Appendix G. As built: M5 FF tier behind lockstep (branch gpu/M5)

**Result.** Mixed frames (`gpu_front_config.mixed`, `gs_replay --mixed`): FF draws through RTV + DSV with the
output merger's blend and depth, the read-modify-write forms on the ROV tier, ALIAS16 runs as in M4. On all
24 recordings at S=1 on the RTX 4060 Ti:
- **exact mode** (`--ff-stq-exact --ff-rov-mask 0x10 --lockstep-exact`: S/T/Q stepped exactly, dual-source
  blends kept on ROV, every other FF class on FF): lockstep `--no-resync` **24,092 comparisons, 0 differ**,
  every reference-composed picture identical - every FF class except the dual-source lerp is bit-exact
  (blend OFF/ADD/REVSUB/NOCOLOR, write masks, exact SV_Depth, ATST discard and the exact split, fills ->
  clears, target views and feedback through shadow copies, sprites, triangles, line pixels, the per-tile
  DSV/zpatch sync around ROV runs and ALIAS16);
- **default mode** (hardware S/T/Q, dual source on FF): 24,092 comparisons, 0 beyond the 10.2 budget below,
  0 pictures beyond the picture gate (min PSNR 43.9 dB, load_m3; others 47-66 dB);
- REF mode unchanged: 24,092 comparisons, 0 differ.
WARP (9 recordings: title, both briefs, rope, ep6, ep8 hub, ep2 photo, hack minigame, ThiefNet; 8,166
comparisons per mode) passes both mixed modes too (exact: 0 differ; default: within the budget, worst
comparison 0.89% above 2 in ep6, min PSNR 50.9 dB); unit scenes (gs-draw through `tests/unit/gs-hw/run.ps1`,
variants ref / mixed (exact) / mixdef (default), 1,165 comparisons each): 0 differing scenes on hw and WARP.

Classes were moved ROV -> FF one at a time (`--ff-rov-mask`, GPU_FFC_* bits), each step gated by exact
lockstep on 8 recordings (7,683 comparisons, 0 differ each): A blend-off sprites without Z, B + depth,
C + triangles, D + lines/points, E + NOCOLOR, F + ADD, G + REVSUB, H + target views / feedback, I + ATST split
(plain); then the whole corpus in exact mode, then dual source (tolerance).

**Default-mode lockstep tolerance** (`gs_replay` mixed lockstep, also the unit harness's mixdef variant): CPU
data equal; per comparison at most 2.5% of the GPU colour words differ by more than 2 in some byte, 0.1% by
more than 16, and 0.1% of the Z words differ. Measured over the corpus: words above 16: 3 in total
(load_m3); Z words: 9 in total (ep3 hub 2, ep2 hub 5, load_m3 2: alpha-test outcomes of perspective-textured
fragments); worst comparison above 2: ep8 boss 1.94%, ep6 0.76%, everything else <= 0.08%. The design's
"FF chains +-2" did not hold on this GPU: a stack of translucent dual-source layers with alpha a accumulates
up to ~1/a (ep8 boss debris, ep6), never above 16. Alpha bytes differ only where the channel shuffles moved
dual-blended green into alpha. The picture gate counts pixels above 16 on one line parity (the reference
compose's deinterlacer switches whole lines between history and average on a +-1 difference).

| recording | comparisons | +-1 words | +-2 | 3..16 | > 16 | worst comparison > 2 | Z words | min PSNR |
|---|---|---|---|---|---|---|---|---|
| ep3_brief | 1208 (0) | 41416798 | 1222745 | 1463 | 0 | 0.0161% | 0 | 50.11 |
| ep3_hub | 1427 (0) | 33809597 | 95935 | 593 | 0 | 0.0022% | 2 | 58.35 |
| ep3_recon_int | 1484 (0) | 69927364 | 592318 | 2283 | 0 | 0.0039% | 0 | 56.30 |
| ep3_rope | 127 (0) | 1108519 | 4104 | 23 | 0 | 0.0004% | 0 | 63.83 |
| ep5_hub_street | 1403 (0) | 31837805 | 239152 | 8254 | 0 | 0.0105% | 0 | 58.02 |
| ep6_rc_battle | 1062 (0) | 18400746 | 1271338 | 368977 | 0 | 0.7647% | 0 | 57.30 |
| ep7_boss | 1165 (0) | 102540291 | 1010919 | 698 | 0 | 0.0017% | 0 | 53.34 |
| ep8_boss | 1203 (0) | 114975660 | 7214609 | 666213 | 0 | 1.9357% | 0 | 51.70 |
| ep8_hub | 1920 (0) | 67235641 | 485725 | 23363 | 0 | 0.0087% | 0 | 58.80 |
| fix-perf_brief | 338 (0) | 10627734 | 40525 | 40 | 0 | 0.0004% | 0 | 60.05 |
| fix-perf_rv_intro | 126 (0) | 8745533 | 2638951 | 2993 | 0 | 0.0802% | 0 | 47.13 |
| fix-perf_rv_prompt | 196 (0) | 4142534 | 17448 | 0 | 0 | 0.0000% | 0 | 62.06 |
| fix-perf_title | 184 (0) | 2281319 | 7051 | 0 | 0 | 0.0000% | 0 | 65.69 |
| gpu-C_ep0_museum | 1155 (0) | 109922330 | 1512614 | 1705 | 0 | 0.0031% | 0 | 52.03 |
| gpu-C_ep1_hub | 1224 (0) | 35316231 | 274333 | 40187 | 0 | 0.0283% | 0 | 57.10 |
| gpu-C_ep2_hub | 1500 (0) | 60442458 | 645532 | 4519 | 0 | 0.0109% | 5 | 56.57 |
| gpu-C_ep2_photo | 1432 (0) | 42017818 | 926896 | 5950 | 0 | 0.0299% | 0 | 56.70 |
| gpu-C_ep4_hub | 1499 (0) | 44361468 | 743622 | 7422 | 0 | 0.0135% | 0 | 56.47 |
| gpu-C_fmv | 549 (0) | 4764768 | 111980 | 2146 | 0 | 0.0166% | 0 | 57.50 |
| gpu-C_hack_minigame | 1412 (0) | 19379592 | 95788 | 0 | 0 | 0.0000% | 0 | 55.67 |
| gpu-C_load_m3 | 1140 (0) | 24531705 | 285215 | 2026 | 3 | 0.0249% | 2 | 43.94 |
| gpu-C_m1_brawl | 1213 (0) | 41389945 | 408322 | 1728 | 0 | 0.0362% | 0 | 56.90 |
| gpu-C_pause_save | 642 (0) | 12178485 | 338697 | 82 | 0 | 0.0013% | 0 | 50.87 |
| gpu-C_thiefnet | 483 (0) | 5073585 | 14898 | 1920 | 0 | 0.0061% | 0 | 64.30 |

**Pass-A split report** (decides the escalation default; S/T/Q exact, dual source on FF, tolerance compare,
`build\m5_split`): the plain two-draw split vs the ROV escalation. Z differs nowhere. The split adds words
differing by more than 16 in ep6 (186,799 over the recording; 4 pictures beyond the gate, min PSNR 48.7),
ep2 hub (1,442), m1 brawl (820), ep1 hub (286), and more 3..16 words in ep3 hub (28k vs 7), rope (7.8k vs 0),
pause_save (15k vs 24), ep2 hub (585k vs 1.6k). In exact mode (dual on ROV) the split alone diverged in ep1 hub
and m1 brawl (the additive pass A `(Cs-0)*As+Cd`, ATST GEQUAL 126 FB_ONLY: bisected to a primitive whose
failing fragment was drawn after a later primitive's passing one). Visible, so the design asks for the
escalation - which made them exact but cost more GPU than REF: ep3 hub 12.2 ms (REF 7.8), ep5 street 10.7
(8.3), ep2 hub 12.5 (10.1), ep8 boss 7.0 (15.1), ThiefNet 12.4 (15.0) per game frame. A probe
(`PS2_GPU_DBG_ROVSKIP`) showed the cost is the UAV draws and FF <-> UAV switches (a trivial shader in the ROV
draws costs the same; keeping the DSV bound changes nothing), not ROV pixel work. **Default: the exact split**
(4.2 note): the open split draw keeps a bitmap of the pixels its fail-capable primitives cover (the software
renderer's spans, 64-bit words per row); a pass-capable primitive touching one starts a new draw. Exact
(ep1 hub, m1 brawl 0 differ in exact mode; ep6 / ep2 hub no word above 16), as fast as the plain split, +2-10%
D3D draws. `split_ff` (`--split-ff`) gives the plain split, `straddle_rov` (`--straddle-rov`) the escalation.

**GPU ms per game frame at S=1** (RTX 4060 Ti, `gs_replay --backend hw`, no read-backs, timestamps; game frame
= VBlank with >= 1000 primitives, the first 10 VBlanks left out (`--bench-skip 10`: target creation, first
use of each pixel shader); median of 3 runs; median / p90 / worst):

| recording | REF | mixed (default) | D3D draws REF / mixed (median, max) | render CPU ms mixed (median / max) |
|---|---|---|---|---|
| ep8 boss | 15.02 / 15.45 / 15.96 | **4.08** / 4.35 / 8.32 | 2996, 3140 / 4951, 5133 | 1.33 / 2.32 |
| ep3 hub | 7.82 / 8.54 / 8.75 | **3.18** / 3.45 / 6.44 | 1064, 1200 / 4349, 5100 | 1.11 / 2.36 |
| ep5 street | 8.26 / 9.45 / 9.68 | **3.42** / 3.62 / 7.14 | 1117, 1562 / 4488, 4818 | 1.07 / 1.95 |
| ep2 hub | 10.23 / 12.11 / 12.58 | **3.05** / 3.51 / 6.16 | 1609, 1861 / 5325, 6684 | 1.16 / 1.73 |
| ThiefNet | 14.93 / 15.10 / 15.35 | **5.95** / 6.02 / 8.49 | 2613 / 6712 | 1.57 / 2.03 |

(The worst mixed frames are the first game frames after the skipped ones: pixel shader permutations first used
there. ThiefNet's REF draw count equals its draw maximum: every frame is the same UI.) Render-thread wall time
unpaced median 1.8-2.7 ms, worst steady 7.7-9.6 ms (it waits for the GPU and the driver); with `--pace`
(59.94 Hz VBlanks as in the game) ep8 boss render CPU median 1.30 / max 3.12 ms, ThiefNet 1.53 / 2.11 ms - the
M2 render-thread budget (<= 8 ms) holds with margin on the CPU side.

**All recordings, default mixed mode** (one run each, first 10 VBlanks left out; per game frame; rv_prompt has
3 steady game frames and an idle GPU: 0.9-16 ms between runs, REF 1.7):

| recording | GPU ms median / p90 | D3D draws median / max | render CPU ms median / max | render wall ms median / max |
|---|---|---|---|---|
| fix-perf_rv_prompt | 16.20 / 17.20 | 681 / 681 | 0.60 / 0.83 | 1.56 / 22.10 |
| fix-perf_rv_intro | 1.03 / 1.20 | 710 / 713 | 0.42 / 0.43 | 1.37 / 1.41 |
| fix-perf_title | 1.84 / 1.97 | 1707 / 1707 | 0.63 / 1.45 | 2.59 / 5.29 |
| fix-perf_brief | 2.45 / 3.95 | 2442 / 2462 | 0.83 / 1.04 | 3.76 / 5.14 |
| ep3_hub | 3.23 / 3.52 | 4349 / 5100 | 1.28 / 2.61 | 2.02 / 6.89 |
| ep3_recon_int | 3.16 / 3.25 | 2757 / 2790 | 0.77 / 1.77 | 1.33 / 5.61 |
| ep3_rope | 4.35 / 5.25 | 4683 / 4688 | 1.32 / 1.50 | 5.69 / 6.78 |
| ep3_brief | 1.72 / 1.90 | 1737 / 2344 | 0.47 / 1.74 | 0.86 / 4.75 |
| ep5_hub_street | 3.43 / 3.66 | 4488 / 4818 | 1.14 / 1.87 | 1.86 / 7.87 |
| ep6_rc_battle | 1.96 / 2.46 | 1698 / 1857 | 0.58 / 1.43 | 1.02 / 4.86 |
| ep7_boss | 6.33 / 6.84 | 3662 / 3867 | 1.03 / 1.67 | 1.96 / 7.86 |
| ep8_hub | 1.60 / 1.85 | 1548 / 1596 | 0.46 / 1.03 | 1.01 / 3.87 |
| ep8_boss | 4.12 / 4.32 | 4951 / 5133 | 1.17 / 1.59 | 2.33 / 9.91 |
| gpu-C_ep0_museum | 1.92 / 2.45 | 3156 / 3246 | 0.64 / 1.11 | 1.12 / 6.46 |
| gpu-C_ep1_hub | 3.44 / 3.78 | 3906 / 4378 | 0.94 / 1.67 | 1.59 / 7.35 |
| gpu-C_m1_brawl | 2.53 / 3.90 | 2601 / 3695 | 0.62 / 1.36 | 1.12 / 7.15 |
| gpu-C_ep2_hub | 3.06 / 3.45 | 5325 / 6684 | 1.10 / 1.55 | 1.79 / 9.06 |
| gpu-C_ep2_photo | 1.49 / 1.68 | 413 / 4380 | 0.33 / 1.58 | 0.73 / 2.77 |
| gpu-C_ep4_hub | 3.00 / 3.52 | 4139 / 4279 | 0.86 / 1.34 | 1.42 / 9.05 |
| gpu-C_hack_minigame | 0.45 / 0.77 | 302 / 370 | 0.12 / 0.74 | 0.49 / 2.57 |
| gpu-C_fmv | 0.20 / 0.40 | 28 / 28 | 0.11 / 0.64 | 0.17 / 2.22 |
| gpu-C_load_m3 | 1.34 / 1.52 | 2347 / 2526 | 0.46 / 0.90 | 0.96 / 3.80 |
| gpu-C_pause_save | 2.60 / 3.64 | 4807 / 4810 | 1.04 / 1.60 | 1.64 / 7.73 |
| gpu-C_thiefnet | 5.99 / 6.04 | 6712 / 6712 | 1.54 / 2.20 | 2.63 / 10.45 |


**What was built**
- `shaders/gs.hlsl`: the fragment evaluation split out of `ps_gs` into `gs_frag` (records -> attributes, texture,
  TFX, fog), shared by `ps_gs` (REF / ROV, unchanged results) and `ps_ff` (FF: ATST discard / split halves,
  per-blend-class output, dual source, `SV_Depth = asfloat(z + 0x800000)`). FF permutations
  `ps_ff_{t,s,l}{dual}{zout}[{fast}]` are compiled with `FF=1` (no UAV declarations: a pixel shader writing
  render targets cannot have u0); `vs_ff_tri` adds interpolated S/T/Q; `ps_gs_tri_fast` is the ROV shader on
  128-byte records with hardware S/T/Q (default mixed mode). `shaders/zsync.hlsl`: `ps_zresolve` (zpatch ->
  DSV, or a constant for partial depth clears) and `cs_zsync` (DSV -> zpatch, Z24 keeps zpatch's top byte),
  both restricted to the tiles in the needed state.
- `gpu_exec.c`: mixed frames (`GPU_FD_MIXED`): `exec_draw_ff` (RTV + DSV, REF viewport and vertex shaders so
  coverage is identical, blend state per class and write mask, depth state class, split halves with the fail
  draw's state from AFAIL, two constant slots per split draw), ROV draws through `exec_draw_ref`; depth tile
  states per 16x16 native pixels (SYNC / DSV newer / zpatch newer) with lazy conversion before FF draws,
  ROV / ALIAS16 draws, loads, downloads, cross copies, clears and read-backs; per-adapter dual bias; counters
  (FF / ROV draws, Z resolves / syncs and their samples).
- `gpu_front.c`: FF triangle records (128 B prefix of `gpu_rec_tri`, or 256 B with `ff_stq_exact`), FF key
  without texture kind / S/T/Q (StateRec data), split halves and AFAIL in the draw, draw bbox and Z-write flag
  (`gpu_fop_draw`), FF views always through the shadow copy, `ff_rov_mask` classes, the exact split bitmap,
  tiled fill -> clear dropping the fill's FF draws (`fill_draws_dropped`), escalation flag sticky per state.
- Tools: `gs_replay --mixed --ff-stq-exact --ff-rov-mask --split-ff --straddle-rov --lockstep-exact
  --tol-report N --bench-skip N` (tolerance lockstep, picture gate, `render-bench` and `ref-bench-dist` lines),
  bisection child exits 0 past its range (single-comparison binary search); `gpu_lockstep_all.ps1 -Mixed
  [-Exact -StqExact -FfRovMask -StraddleRov -SplitFf -Exe]`; `gpu_fthread_check.ps1 -Mixed`; unit harness
  variants `-DTEST_HW_MIXED[_DEFAULT]` (`tests/unit/gs-hw/run.ps1 -Modes ref,mixed,mixdef`).

**Deviations from the design**
- Depth: no zpatch serials / read-only DSV for ROV draws; per-tile lazy sync between DSV and zpatch, ROV and
  ALIAS16 draws keep using zpatch as depth (the REF shader unchanged).
- FF shading is the REF fragment pipeline (records), not vertex interpolation: colour, fog, U/V and Z are
  exact; only S/T/Q is hardware-interpolated (and exact with `ff_stq_exact`).
- Dual source is +-1 on the RTX 4060 Ti whatever the bias (blender ~fp16); chains exceed the design's +-2, so
  the default-mode budget is 2.5% of colour words above 2 per comparison (measured worst 1.94%).
- Pass-A split: exact split on FF instead of the ROV escalation (measured slower than REF).
- Picture gate counts one line parity (deinterlacer heuristic of the reference compose).

**What M6 (in-game integration) must know**
- Run the frontend with `mixed = 1` (`ref = 0`, `split_ff = 0`, `ff_stq_exact = 0`, `straddle_rov = 0`):
  frames are flagged `GPU_FD_MIXED`. `display_ky2` must stay 0 until Z targets follow their colour target's
  factors (M7). The frontend thread produces the same mixed stream (`gpu_fthread_check.ps1 -Mixed`).
- There is no GPU display path yet: replays compose pictures from downloaded memory (`gs_compose_from`). M6
  needs compose/present from the colour target SRVs; before sampling a target that way nothing else is
  needed (colour has one copy); before reading depth on the CPU or in a CS, `ensure_zp` (the read-back path
  does it for every depth target).
- Every D3D11 draw that writes a UAV (ROV runs, ALIAS16, REF) serialises the GPU and an FF <-> UAV switch
  costs ~20 us on the RTX 4060 Ti: keep ROV runs few (tens to ~200 per game frame now, ep7 boss most: shadow
  volumes, As > 128 pass A). GPU per game frame at S=1 is 3-6 ms in the heavy scenes (REF 8-15 ms); M9's budget at
  S=3 needs the scale cost measured in M7.
- The dual-source lerp is the only inexact FF class (+-1 per layer); the exact verification mode
  (`--ff-stq-exact --ff-rov-mask 0x10 --lockstep-exact`) is the regression gate for everything else, the
  default-mode tolerance gate for the whole.
- PUNT still needs the same-thread frontend (unchanged from M4).
