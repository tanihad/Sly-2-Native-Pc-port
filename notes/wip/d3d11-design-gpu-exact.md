# Design: GPU-exact GS on Direct3D 11 compute ("GSX")

Status: design complete (role: gpu-exact designer). Based on the renderer map (d22cef5), the census
(69feaeb) and the prior-art notes (230e2e4) on next/d3d11, plus a re-read of runtime/gs_draw.c
(draw_sprite/triangle/line, vertex queue), gs_draw_pixel.c (sampling and blend arithmetic),
gs_draw_swz.c (16-bit word pairing) and notes/performance.md.

## 0. Thesis

The GS's local memory moves to the GPU in its swizzled layout and stays the single source of truth.
Primitives are rasterized and shaded by D3D11 compute shaders with the GS's exact integer
rules, in GS order per pixel. The CPU decodes registers, assembles primitives, tracks
memory hazards at 256-byte block granularity and builds command batches; it never touches a
pixel.

Resolution scaling is done with *sample planes*. Plane s is a complete 4 MB GS memory image of the
same frame, rendered with the drawing offset shifted by a sub-pixel amount (a multiple of 1/16 px,
the GS's own coordinate precision). Plane 0 has offset 0, so at every internal resolution plane 0
is byte-identical to the 1x software renderer. Every Sly 2 trick works per plane with no
game-specific code, because every plane is a real GS:

- the green-channel shadow stencil with 8-bit wraparound;
- the CT16 channel shuffles;
- the T8H palette darkening of the frame into itself;
- the motion-blur feedback;
- Z-region aliasing;
- AA1 outlines.

This is the strongest property of the design. It is also why the alternatives lose on Sly 2:

- **D3D11 draws with a render-target cache** need reinterpretation shaders and Sly-specific
  heuristics (PCSX2 still has two GameDB entries for this game).
- **ROV pixel shaders into a VRAM UAV** order fragments only within a draw. D3D11 puts an implicit
  UAV barrier between draws that share a UAV, so Sly's 2-11k overlapping draws per frame would
  serialize.

A compute tile rasterizer orders work per pixel inside one thread, so it needs no barriers inside a
batch.

## 1. Exactness contract

"GPU equals software" is only achievable if both run the *same arithmetic*. Today's software
renderer uses three things a GPU cannot reproduce bit for bit:

- double-precision attribute planes rounded to fixed point (`rowstep_init`);
- float32 S/T/Q accumulated per pixel (`s += ds` in `sample_batch`);
- IEEE division `1.0f / q`. D3D11 allows 2.5 ULP for division and 1 ULP for rcp.

**Reference arithmetic v2.** M1 replaces these in the software renderer with a GPU-implementable
definition, written once in a polyglot header `runtime/gs_arith.hlsli`. That header compiles as C
(clang, `typedef struct { uint32_t x, y; } uint2;` shims, native int64 under `#ifdef __clang__`)
and as HLSL (int64 emulated with uint2 add-with-carry and 32x32->64 multiplies built from 16-bit
halves). Both renderers include the same source, so they cannot drift. The definitions:

1. **Edges.** Edge functions are unchanged: exact integers with the top-left bias of `edge_setup`.
   The GPU evaluates them per tile in 64-bit (emulated, once per primitive and tile, uniform) and
   then per pixel as a 32-bit delta (|A|·(16·31+15) < 2^27). A tile-origin value beyond ±2^30
   saturates, because the whole tile is then on one side of the edge.
2. **Linear attributes (R, G, B, A, fog, U/V with FST=1, Z).** The plane gradient is
   g = round_half_away(num · 2^16 / det), where num and det are exact integers. It is computed by
   an exact 64-bit (Z: 96-bit limb) long division in setup. The value at pixel (px, py) is
   base + floor((gx·(16px − x0) + gy·(16py − y0)) / 16), evaluated exactly (64-bit; Z uses
   3 limbs), then clamped (colours) or truncated (Z) at the pixel. This is the "exact plane, clamp
   at the pixel" semantics. It replaces both the double setup and the `sat32` at span start.
3. **S/T/Q (FST=0).** These are float32 planes built in setup with a fixed operation order. Only
   float add and mul are used: the D3D11 spec requires them correctly rounded, with denormals
   flushed. The division in setup and the per-pixel 1/q are an integer-exact routine (mantissa long
   division, round to nearest even) in gs_arith. The HLSL is compiled with
   `D3DCOMPILE_IEEE_STRICTNESS` and `precise`, so mad is never fused. The C side uses
   `-ffp-contract=off` and sets FTZ/DAZ in MXCSR on the threads that draw. U = f2i(s·iq·tw16), as
   today.
4. **Sprites.** U(px) = floor((U0·Δx + (U1−U0)·(16px − X0)) · 4096 / Δx), computed as an exact
   rational. FST=0 sprites take U0 and U1 from rule 3.
5. **Lines.** The DDA is defined in integers. The minor coordinate at major pixel p is
   Y0 + (16p − a0)·dy / len, as an exact floor division. The AA1 coverage fraction is the remainder
   rounded to 1/8, so it is the same as today's `lround(... * 8)` up to tie-breaking.
6. Texel fetch, bilinear with 4-bit weights, TFX, fog, ATE/AFAIL, DATE/DATM, ZTST, the blend
   (A−B)·C>>7+D, COLCLAMP or wrap, PABE, FBA, DTHE (16-bit only), FBMSK, and format packing are
   already integer-only in gs_draw_pixel.c. They are ported verbatim into gs_arith.hlsli and the
   C side calls the shared functions.

**The contract.** For any .gsrec, after every VBlank, plane 0 of GPU VRAM equals the software
renderer's `gs_vram` byte for byte. That covers all 4 MB, Z and off-screen buffers included, not
only the displayed picture. The tolerance is 0. The v1 to v2 change in the software renderer is
checked once, visually and with compare_frames.py tolerances (§9).

## 2. GPU resources (D3D11 feature level 11_0, cs_5_0 only)

Nothing optional is used: no ROV, no typed UAV loads, no doubles, no wave intrinsics. This runs on
any D3D11 GPU and on WARP, which matters for headless agent tests.

| resource | type | size |
|---|---|---|
| `vram` | RWByteAddressBuffer (R32_TYPELESS raw), P planes × 4 MiB, plane-major | 4 MiB (1 plane) to 256 MiB (64) |
| `shadow` | same layout; only block ranges copied for feedback are valid | same as vram |
| `swz` | ByteAddressBuffer: per-PSM page tables (u16) from gs_draw_swz.c / gs_mem probes, for CT32/24/16/16S, Z32/24/16/16S, T8, T4 | ~100 KB |
| `clut_ring` | ByteAddressBuffer, 4096 slots × 1 KB (CLUT memory snapshots) | 4 MiB |
| `vtx_ring` | 16 MiB dynamic buffer of `gsx_vtx` (32 B) | ≥3 frames |
| `prim_ring` | 4 MiB of `gsx_prim` (16 B) | |
| `state_ring` | 4 MiB of `gsx_state` (160 B) | |
| `setup` | RW structured, 64k × 192 B per pass (edges, planes, bbox, line record offset) | 12 MiB |
| `linepix` | RW buffer of expanded line pixels {q, frac, attr t} | 2 MiB |
| `bins` | RW raw: per tile a 64k-bit primitive bitset + 1k-bit summary | tiles × 8.1 KB |
| `xfer_ring` | 8 MiB: host-transfer patches (block data + bit mask) | |
| `picture[3]` | RGBA8 textures, UAV + SRV, internal picture size | ≤ 4096² |

- **Limits.** The D3D11 buffer size limit is max(128 MB, ¼ of video memory), which is 2 GB on the
  RTX 4060 Ti. On small GPUs P is capped so that vram + shadow fit.
- **Sample planes.** Factors kx, ky ∈ {1, 2, 4, 8, 16}, P = kx·ky ≤ 64. Plane (i, j) has offset
  (16i/kx, 16j/ky) in 1/16 px, which is exact on the GS grid.

## 3. CPU side (the GS thread)

### 3.1 What changes in the code

- **gs_draw.c is split.**
  - `gs_regs.c` keeps the register file, PRIM/PRMODE handling, `vertex()`, strip and fan assembly,
    and XYZ3/ADC. It calls a backend hook `draw(type, a, b, c)`.
  - The rest (`env_update` to `gs_prim`, `draw_*`, `tex_prepare`) becomes the software backend,
    which is unchanged apart from v2.
- **New `gs_gpu*.c` (backend "gsx")** and `runtime/shaders/*.hlsl` + `gs_arith.hlsli`.
- **Backend vtable** selected at start by `PS2_GS_BACKEND=gpu|sw` (gpu by default once M5 is in):
  `{exec_write_reg, exec_image_write, exec_image_read, reset, vblank(snapshot), kick, sync}`.
  - gs_thread.c calls it instead of `gs_exec_*`, `gs_draw_kick` and `vblank_picture`.
  - gs.c's `gs_display` (synchronous path) and `CSR.RESET` go through it.
- **Frozen headers stay unchanged.**
  - hw.h: SIGNAL/FINISH/LABEL keep executing on the EE thread at queue time, and CSR/IMR are
    untouched.
  - gs_internal.h: `gs_vram` remains as the *CPU mirror* (below), and `gs_mem_xfer_*` and
    `gs_clut_*` are reused unchanged.
- **window.c** gets an internal API in a new `present.h` (not hw.h):
  `present_gpu(picture_index, w, h, aspect)`. `window_present` stays for the software backend.
- **gs_replay.c** gets `-Backend`, `-Planes kx,ky`, `-DumpVram` and `-Lockstep` (§9).

### 3.2 Per-register and per-primitive work

- **State.** An environment change marks the state dirty. At the next primitive the backend builds
  a `gsx_state` holding the active context's raw registers (PRIM attributes, FRAME, ZBUF, TEST,
  ALPHA, TEX0, TEX1, CLAMP, MIPTBP1/2, TEXA, FOGCOL, SCISSOR, XYOFFSET, FBA, plus packed PABE,
  DTHE, COLCLAMP, SCANMSK and DIMX), the CLUT slot, and a `texsrc` field (VRAM, shadow, plane
  mode). The shader decodes the bits itself, so CPU work per state is a hash and a memcmp. States
  are deduplicated per pass in an open-addressing table keyed by a 64-bit hash of the 160 bytes.
  The census has 2-11k changes and 0.8-6.7k distinct states per frame.
- **Vertices.** Each is appended as a 32-byte `gsx_vtx` (x, y as int16 already minus XYOFFSET;
  z u32; rgba u32; s, t, q f32 with ST's low 8 mantissa bits already dropped; u, v u16; fog u8).
- **Primitives.** A `gsx_prim` (16 B) holds the first vertex index, two u8 deltas (strips and fans
  share vertices), type, AA1 and the state index.
- **Bounding box.** It is computed from the vertices, clipped to the scissor, and marked in the
  pass's written-cell bitmap (8×8 px cells; one bitmap for the frame, one for Z) only when the pass
  needs it (§3.3).
- **Texel bounding box (tex_prepare equivalent).** It is needed only when the texture's page range
  intersects the pass's target page range or the pages written in this pass. That is a 512-bit
  page-set AND, done once per state. Otherwise there is no per-primitive texture work.

### 3.3 Passes and hazards (the sync points)

A **pass** is a run of primitives with one *target key*: (FBP, FBW, FPSM, ZBP, ZPSM, Z used),
scissor-bounded. All texture and CLUT reads in a pass see VRAM as it was when the pass started:

- **(H1) RAW.** A primitive whose read blocks intersect blocks written by *earlier* primitives of
  the pass starts a new pass. This is the existing `register_prim` rule, at block instead of page
  granularity. Read and write sets are kept per byte lane (low 24 bits / top byte), so a CT24
  target and a T8H, 4HL or 4HH read of the same blocks do not conflict (paraLLEl-GS's separate
  masks).
- **(H2) WAR and self-feedback.** At pass close, reads ∩ writes are computed. The affected blocks
  are copied `vram` → `shadow` before the pass (CopySubresourceRegion per plane, or one copy
  kernel for many blocks). The `texsrc` of the primitives that read them is patched to *shadow*.
  This covers in-place filters such as the darkening pass, the CT16 shuffle that moves G into
  alpha in place, and the backdrop reading upload blocks that a later Z clear in the same pass
  overwrites. That case looks harmless, but without H2 another tile's write-back could race the
  read.
- **(H3) Target change.** FRAME, ZBUF, FBMSK-free Z/format changes start a new pass. The census
  says these are 0.1-0.2% of changes, giving about 10-30 passes per frame.
- **(H4) Host→local transfer.** If the transfer's blocks intersect the pending pass's read or write
  set, the pass is closed and the transfer follows it. Otherwise the transfer is *hoisted* into the
  pass's pre-op list, which runs before the pass. Primitives recorded after it then see it, and none
  recorded before it touched those blocks.
- **(H5) CLUT load** (TEX0 with a load per `gs_clut_on_tex0`):
  - If its blocks intersect earlier writes of the pass, H1 applies (new pass).
  - Otherwise it is a pre-op or done on the CPU (§3.4).
- **(H6) Limits.** 65,536 primitives per pass, or ring space.
- **(H7) K_VSYNC.** Close the pass, dispatch the CRTC kernel, signal a fence.
- **(H8) `gs_image_read`** (TRXDIR=1 via `gs_sync`). Close the pass, copy the source blocks of
  plane 0 to staging, Map(READ) (blocking), write them into the CPU mirror, then run the existing
  `gs_mem_xfer_read`. The result is exact by reuse.
- **(H9) Idle kick.** When the GS thread is idle (today's `gs_draw_kick` point) and the GPU has
  finished its previous submission (fence), close and submit the pass early to cut latency.
- **(H10) Throttle.** More than 2 VBlank frames in flight on the GPU means waiting on the fence
  (ID3D11Fence + SetEventOnCompletion, D3D11.4).

**Fallbacks (correct and slow; Sly never hits them):**

- A frame and a Z buffer that alias each other inside a pass.
- A scissor wider than FBW·64, where pixels alias across tiles.
- Overlapping TRXDIR=2.

In each case every primitive or copy becomes its own serial pass.

### 3.4 Transfers and CLUT: exact by reusing the CPU code

- **Host→local.** The CPU runs the existing `gs_mem_xfer_write` into its `gs_vram` mirror. In
  parallel it computes the transfer's bit-exact footprint per touched 256 B block, using the
  gs_mem_swizzle.h address functions over the rectangle (about 1 ns/texel; 0-270 KB per frame).
  The patch is {block id, 256 B data, 256 B bit mask}, or a "full block" flag. The GPU `xfer_patch`
  kernel applies `word = (old & ~m) | (new & m)` in *every* plane. No HLSL port of the transfer
  formats is needed, and blocks that are partly rendered keep their per-plane bytes.
- **Local→local (TRXDIR=2).** The `xfer_local` kernel is an HLSL port of the per-pixel
  `gs_px_read`/`gs_px_write` path. It runs per plane, from a snapshot of the source, or with a
  single thread in TRXPOS.DIR order when source and destination overlap. The census never sees it.
- **CLUT.** The CPU keeps a `gpu_dirty` bit per block, set at pass close for every block a pass
  wrote (conservatively: the union of written cells) and cleared by full-block host writes.
  - A load from clean blocks (every load in the census) runs the existing `gs_clut_on_tex0`
    against the mirror. It produces the 1 KB CLUT image, which is hashed (content-addressed) and
    uploaded into a `clut_ring` slot only if new. The 90-350 distinct CLUTs per frame become
    long-lived slots, and 0.5-7k reloads per frame cost a hash each.
  - A load from GPU-dirty blocks runs the `clut_load` kernel (an HLSL port, CSM1/CSM2/CSA) as a
    pre-op, from plane 0, into a fresh slot.
- **Mirror.** The CPU mirror is therefore exact for every host-written byte and never assumed valid
  for rendered bytes.

## 4. GPU pipeline per pass

1. **Pre-ops**, in recorded order: `xfer_patch`, `clut_load`, `xfer_local`, shadow copies.
2. **`setup_bin`** (one thread per primitive, 64 per group):
   - v2 setup: edges, gradients, 64-bit and limb divisions, bbox ∩ scissor ∩ pass tile rectangle.
   - Lines are expanded into `linepix` (one entry per major-axis pixel, exact DDA).
   - It then sets the primitive's bit in each overlapped 32×32 tile with `InterlockedOr`, plus the
     tile summary bit. For large triangles a tile is skipped if all three edges reject its corners.
   - Bitsets are cleared beforehand with ClearUnorderedAccessViewUint on the pass's tile range.
3. **`raster`**:
   - Dispatch(tilesX, tilesY, P / planes_per_group). A group is 256 threads covering a 32×32
     tile. Each thread owns 4 pixels × SPT samples (SPT = planes per group: 1, 2 or 4).
   - **Load:** each thread reads its pixels' colour and Z from `vram` (plane p) in the target
     formats, using the swizzle tables, into registers.
   - **Loop:** walk the tile's bitset in primitive order (firstbitlow over the summary words), so
     primitive order is preserved per pixel without sorting.
   - **Per primitive:** load its setup and state (uniform). Each wave tests its two 8×8 sub-blocks
     against the edges (about 20 ops) and skips the primitive if neither is touched. Each thread
     evaluates coverage at its sample positions and runs the full GS pixel pipeline from
     gs_arith.hlsli on its registers:
     - texel fetch from `vram`/`shadow` (plane per the rules below), CLUT from `clut_ring`;
     - TFX, fog, tests, DATE (destination alpha in registers), Z, blend, FBMSK.
   - **Store:** at the end each thread writes its words back. 32-bit formats use plain stores. For
     16-bit and 24-bit formats the 16-bit word partners (x and x+8 inside a 16-px column, per
     gs_draw_swz.c `build16`) lie in the same 16-aligned tile but may be in another thread. They
     are merged with InterlockedAnd(~own) + InterlockedOr(own), which is order-independent because
     the bits are disjoint. At init the swz tables are enumerated to assert that all word partners
     fall inside one 32×32 tile; if not, the fallback is 64-wide tiles.
4. Timestamp queries around the pass (PS2_GSX_STATS=1).

**Plane rules, which define the scaling semantics:**

- **Triangles and lines** are rasterized with vertices shifted by the plane offset. Plane s is the
  GS picture rendered at offset o_s, so lines stay 1 native pixel wide with AA1 coverage evaluated
  per plane, and outlines come out smooth when upscaled.
- **Sprites** have their coverage at the native position for all planes (single rate). Sprite edges
  are pixel-aligned, so text and UI do not bleed.
- **Textures that were never rendered** (all blocks of the read set uniform across planes) are read
  from plane 0 at the UV evaluated at the *shifted* position. Uploaded textures therefore gain real
  detail.
- **Textures whose blocks were rendered** (the per-block `divergent` bit, set at pass close when
  P > 1) are read from *the same plane* at the UV evaluated at the *native* position. If the
  rendered texture T_s(u) = T(u + o_s), a 1:1 pass gives out_s(x) = O(x + o_s), exactly aligned.
  This covers every post pass, the shuffles, the T8H darkening and the motion blur (with its 1-row
  scissor), with no half-pixel hacks. Scaled reads of rendered textures (the Binocucom RTT
  composite) are off by at most (M − I)·o_s, under one source texel. Plane 0 is still exact.

## 5. Display and presentation

- **CRTC kernels.**
  - `crtc_exact` is an HLSL port of `gs_compose`, including the weave, for 1x and P = 1. In tests
    the exact picture can also be produced by downloading plane 0 and running the CPU `gs_compose`.
  - `crtc_pc` is the default, for PC output:
    - It recognises the anti-flicker setup: EN1+EN2 reading the same FBP/FBW/PSM with |DBY1−DBY2|
      ≤ 1, MMOD=1 and ALP=0x80. It then shows only circuit 2, because the filter is TV-specific.
      Other PMODE combinations are composed exactly per circuit.
    - **Field mode** (SMODE2 INT=FFMD=1, 224-line buffer): the plane grid builds the progressive
      picture. Output row = ky·y + j + parity·ky/2, where parity comes from the FRAME's
      XYOFFSET.Y fraction (0 or 8; the sign is taken from today's `gs_fb_yfrac` logic). With
      ky = 2·kx a single field buffer yields a full-height progressive image with no weave and no
      flicker. The 2 missing edge rows are clamped.
    - **Output:** the picture is (512·kx/r) × (448·kx/r), where r×r planes are box-resolved for
      SSAA. Default kx = 1, ky = 2 (2 planes): progressive 512×448.
- **Picture handoff.** The picture goes to one of three `picture[]` textures, written by the GS
  thread through a UAV. The index is published under the existing SRWLOCK triple-buffer protocol.
- **Device and threading.**
  - One device, created at start-up by a new `d3d_device.c`, before the window and GS threads, with
    `ID3D11Multithread::SetMultithreadProtected(TRUE)`.
  - The GS thread records all compute work on the immediate context. That is about 30-60
    dispatches/copies per frame, so deferred contexts gain nothing.
  - The window thread first waits on the swap chain's frame-latency waitable object
    (`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, latency 1) *outside* the lock. It then
    takes the context briefly to draw the newest picture into the back buffer and calls Present(1),
    which returns promptly because the wait already happened. GPU ordering between the GS thread's
    CRTC dispatch and the window thread's draw follows from submission order on the same context.
  - Device-lost: recreate the device, re-upload VRAM plane 0 from the last good download (or
    reset), and fall back to the software backend after three failures.
- **Scaling and aspect.** The window thread's pixel shader does the final scale with area/bicubic
  filtering into the letterbox (§7 aspect).
- **Pacing.** The 59.94 Hz host timer stays the game clock. If tearing is supported
  (`DXGI_PRESENT_ALLOW_TEARING`) and the display has VRR, Present(0, ALLOW_TEARING) shows each
  picture exactly once. Otherwise behaviour is as today: one repeat about every 17 s, with an
  optional "VBlank from display vsync" mode as later work.

## 6. EE-visible behaviour

Unchanged:

- SIGNAL/FINISH/LABEL and CSR are handled on the EE thread at queue time.
- The EE waits only at the same points:
  - a full queue;
  - `gs_sync` (readback or CSR.RESET);
  - a VBlank while the previous picture has not been *submitted*. Today it is "composed"; it is
    now "the GS thread processed K_VSYNC". GPU completion is bounded separately by H10.
- A readback stalls the EE for about 0.5-2 ms, roughly what the software path's `gs_sync` costs
  today.
- CSR.RESET drops the vertex queue and cancels transfers. Recorded passes are submitted, not
  dropped, matching `gs_draw_reset` + `gs_sync`.

## 7. Resolution scaling, anti-aliasing and widescreen

- **Settings** (`sly2.ini` / env; default first):
  - `scale = 1..8`: kx = scale, ky = 2·scale, capped at P ≤ 64, so 4x = 4×8 = 32 planes;
  - `ssaa = 1 | 2`: r×r extra planes per output pixel, box-resolved;
  - `aspect = 4:3 | 16:9 | stretch`;
  - `present = vsync | vrr`.
  The offset grid must divide 16, so scales 3, 5, 6 and 7 use the next power of two internally and
  the final pass filters down.
- **MSAA** is not applicable. SSAA planes give the anti-aliasing, and AA1 lines stay exact.
- **Widescreen** needs from the renderer:
  1. Anamorphic output. The game keeps drawing 512-wide buffers. The EE projection patch (the
     constants at 0x14D750/54 in f_0014D6D8 and the f_0014F308 hook; separate EE work) puts a
     16:9 field of view into them, and `aspect = 16:9` stretches the picture to 16:9. Higher kx is
     recommended, because horizontal detail per degree drops by 4/3.
  2. HUD correctness belongs to the game patches (f_001BE6F0 portraits and the HUD sprite X scale
     pinned to a 4:3 safe area). The renderer adds nothing game-specific.
  3. The motion blur, shadows and post passes are full-screen 512-wide column passes in buffer
     space, so they stay correct under anamorphic widescreen with no change.
  True wider buffers (FBW 11) would need the game's post-pass column loops and VRAM map rewritten
  and are out of scope.

## 8. Shaders: compilation, permutations, cache keys

- **Kernels:**
  - `setup_bin`;
  - `raster`, with permutation key {FCLS: C32 | C24 | C16; ZCLS: none | Z32 | Z24 | Z16;
    SPT: 1 | 2 | 4}, giving 36 variants. CT16 vs CT16S and Z16 vs Z16S are a uniform table
    offset;
  - `xfer_patch`, `xfer_local`, `clut_load`, `shadow_copy`;
  - `crtc_exact`, `crtc_pc`;
  - `present_vs`/`present_ps`;
  - `conformance` (test only).
  Everything else (texture PSM, filter, TFX, tests, blend selectors, AFAIL, DATE) is decoded from
  the state at run time with uniform branches, which are coherent per group because the state is
  per primitive.
- **Build time.** A 150-line `tools-src/hlslc.c`, compiled with llvm-mingw, LoadLibrary-s
  System32's `d3dcompiler_47.dll` and calls D3DCompile with OPTIMIZATION_LEVEL3 and
  IEEE_STRICTNESS. The DXBC blobs become C arrays linked into the exe. No new tools are downloaded.
- **Run time (dev).** `PS2_GSX_SHADERS=<dir>` recompiles from source at start. Results are cached
  in `%LOCALAPPDATA%\sly2\shadercache\<fnv64(source + defines + compiler version)>.dxbc`.
- **Other cache keys:**
  - state dedupe: 64-bit hash plus memcmp of `gsx_state`, per pass;
  - CLUT slot: hash of the 1 KB content (plus CPSM, for 16-bit CLUT conversion done in the
    shader); slots are LRU, never reused while a fence older than their last use is pending;
  - the `divergent` and `gpu_dirty` bits are per block, CPU side.

## 9. Differential testing

1. **Arithmetic conformance (M0).** A kernel computes 10^8 random float add/mul cases (incl. FTZ
   edges), the gs_arith 64/96-bit helpers, the exact division and 1/q, and compares them with the C
   build of the same header. It runs on the RTX 4060 Ti and on WARP and must be bit-identical.
2. **Unit scenes.** `tests/unit/gs-draw` gets `-DTEST_GPU`. Every existing scene (fill rule,
   gouraud, CLUT, bilinear, blend, scissor, alpha tests, fog, 16-bit, perspective, mipmap, lines,
   CSR, display, deferred hazards, direct-sampling order, region clamp, formats/contexts, negative
   STQ, random texture cache) runs on both backends, with VRAM compared byte for byte (tolerance 0).
   New scenes cover the Sly-specific patterns: G-channel stencil with COLCLAMP=0 and FBMSK
   FFFF80FF/FFFFFEFF, CT16 shuffles, T8H self-darken, Z16S on Z32 memory, an upload into Z blocks
   sampled before a Z clear (H2), and the H1/H4 hoisting orders.
3. **Corpus.**
   - `gs_replay.ps1 -Backend gpu -Planes 1,1 -DumpVram` writes a VRAM hash per VBlank (full dumps
     on mismatch) and exact pictures. `compare_frames.py --tol 0` against `-Backend sw` on all 13
     recordings. The VRAM hashes must be equal at every VBlank.
   - `-Lockstep` runs the software backend in the same process. At every pass close it downloads
     plane 0 and diffs it, and on a mismatch it bisects the pass to the first differing primitive
     and dumps its state and vertices (the same format as PS2_GS_TRACE). An autonomous agent can
     fix mismatches with that alone.
4. **Scaling checks** (P > 1):
   - Plane 0 must still match exactly.
   - Each other plane is checked against the 1x picture box-filtered:
     - mean |Δ| ≤ 3;
     - 99.5% of pixels within 32;
     - the cross-correlation peak at a (0, 0) shift ± 1/kx px, which catches post-pass
       misalignment or a "chromatic" shift.
   - Screenshots at 2x/4x go to `build/gsx/shots/` for the owner.
5. **In game.**
   - Run the scripted path (copyright, intro, title, briefing) with `PS2_GS_BACKEND=gpu`,
     `PS2_GS_RECORD` and `PS2_PERF=1`.
   - Replay that recording through `sw` and diff (the in-game run is not deterministic, so the
     recording is the comparison).
   - Check GS-thread ms, GPU ms per pass class, and flips/s at 60.
   - Repeat in the existing ep3/5/6/7/8 worktree saves.
   - For ep2 (photo mission, readbacks), make a new recording when a save is available.

## 10. Cost estimates (per game frame, heavy scenes: ep3 hub, ep5, ep8 boss)

**CPU, GS thread:**

| work | estimate |
|---|---|
| register slots (~400k: RGBAQ/ST/XYZF2 per vertex), consumed and decoded as today | 2-3 ms |
| vertex/primitive append (120k × 32 B, 40k × 16 B) | 0.5-1 ms |
| state build and dedupe (2-11k) | 0.2-0.6 ms |
| hazard bitmaps | 0.3-1 ms |
| D3D11 calls (~200), ring Maps (NO_OVERWRITE) | ~0.2 ms |
| **total** | **3.5-6 ms** (today: 11-16.4 ms plus 8 pixel workers at about 7.5 ms critical path) |

- The 8 pixel workers disappear, which is the main win on a loaded machine.
- The EE thread is unchanged at ~7.7 ms.
- Upload is about 5 MB per frame (≈ 300 MB/s on PCIe 4.0 x8).

**GPU (RTX 4060 Ti, ~340 G wave-instructions/s peak, assuming 40-50% efficiency):**

- About 53k primitive-tile pairs per plane. Each costs roughly 100 instructions of uniform/edge
  work for the 1-2 touched waves and 6 skipped waves at about 5 instructions each, plus about 400
  instructions of shading per touched wave.
- That is 40-60 M wave-instructions, or **0.25-0.4 ms per plane**, plus post passes (~10
  full-screen) and per-pass overhead (20-30 passes × 10-20 µs).

| setting | planes | GPU time |
|---|---|---|
| 1x progressive | 2 | ~1-1.5 ms |
| 2x | 8 | ~3-4 ms |
| 4x | 32 | ~9-14 ms |

- 4x fits in 16.7 ms only at the lower end. **Plan for 2x-3x as the default on this GPU, and 4x
  after the M6 tuning.**
- Critical path: the densest tile, with 2-4k primitives around the character, runs serially, about
  0.5-1.5 ms per plane group. Planes run in parallel, so this tail does not grow with P.

## 11. Milestones

Each milestone is one worktree branch with notes/wip checkpoints and is shippable on its own.

- **M0 (2-3 days). Scaffold and conformance.** `d3d_device.c` (shared device, multithread
  protection), hlslc build step, the conformance kernel, the gs_arith.hlsli skeleton with int64/96
  helpers and the exact division.
  *Verify:* the conformance test is bit-identical on HW and WARP; window.c still presents (unit
  tests/window).
- **M1 (3-5 days). Reference v2 in the software renderer.** Rules 1-5 of §1 move into
  gs_arith.hlsli and gs_draw.c / gs_draw_pixel.c call it.
  *Verify:*
  - all unit tests pass (expected values regenerated only where the difference is ≤ 1 LSB of
    colour or 1/16 texel);
  - all 13 recordings v1 vs v2 with compare_frames, differences listed and inspected;
  - software-renderer speed ≤ 10% slower in `gs_replay -Bench`.
- **M2 (2-3 days). GPU memory, transfers, CLUT, readback, exact CRTC** (no drawing yet):
  `xfer_patch` / `xfer_local` / `clut_load` / readback via the mirror.
  *Verify:* `tests/unit/gs-mem` transfer cases run through the GPU and are byte-equal. A corpus
  replay with draws disabled on both backends gives equal VRAM hashes (uploads only).
- **M3 (5-8 days). Raster core:** `setup_bin` and `raster` for sprites, triangles and points;
  untextured pipeline (all tests, blend, COLCLAMP, FBMSK, DATE, FBA, Z formats, 16-bit write
  merge); passes H1-H10.
  *Verify:* unit scenes are VRAM-equal.
- **M4 (5-8 days). Textures** (all 8 kinds, CLUT slots, bilinear, TEXA, CLAMP/REGION, mipmaps
  incl. LCM 0), lines, AA1, shadow feedback (H2).
  *Verify:* all 13 recordings have equal VRAM hashes at every VBlank (planes 1×1), with
  `-Lockstep` clean.
- **M5 (3-4 days). In-game integration.** Backend vtable, `PS2_GS_BACKEND`, present path,
  fences, device-lost.
  *Verify:* the scripted path runs at 60 flips/s; the PS2_PERF GS thread is below 6 ms; in-game
  recordings replayed through sw are equal.
- **M6 (3-5 days). Performance.** Sub-block skipping, SPT, ring sizing, fewer passes.
  *Verify:* GPU ≤ 1.5 ms at 2 planes and ≤ 4 ms at 8 (timestamp queries) on ep8 boss and ep3 hub.
- **M7 (4-6 days). Planes and PC display.** `crtc_pc` (progressive, anti-flicker recognition,
  SSAA resolve), settings, final scaler.
  *Verify:* plane 0 is still exact; alignment metrics (§9.4) pass on all recordings; screenshots
  at 2x/4x.
- **M8 (2-3 days, plus the EE patches as a separate task). Aspect modes and widescreen
  presentation.** *Verify:* 16:9 screenshots with the EE patch, and the HUD reviewed.
- **M9 (2-3 days). Hardening.** WARP and software fallback, shader cache, P cap by memory, and
  the TRXDIR=2 / aliasing fallbacks exercised by unit tests.

**Total:** about 5-8 agent-weeks. The software renderer stays as the oracle and the fallback
throughout.

## 12. Risks

See the structured summary. The main ones:

1. The D3D11 float add/mul rounding guarantee, settled in M0. The fallback is an integer float
   emulation for S/T/Q, slower but exact.
2. Compute-raster performance on dense tiles at high P.
3. Long fxc compile times for the raster ubershader (precompile mitigates).
4. Visual regressions from the v1→v2 change.
5. The readback-dependent ep2 photo mission is untested.
6. Contention on the shared-context lock.
7. Field parity sign and edge rows in `crtc_pc`.
