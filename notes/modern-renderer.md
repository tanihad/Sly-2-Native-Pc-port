# The modern renderer "R2": final design

Status: **design for the owner's review (2026-10-26, branch next/r2-design)**. Sources: notes/modern-renderer-re.md (how Sly
2 renders, evidence in notes/wip/r2-re.md) and notes/modern-renderer-api.md (prior art, graphics API). This page supersedes
the order of work sketched in section 5.3 of the API page. Read notes/native-port-principles.md first.

Terms. **R2** = the modern renderer: it draws the game's own meshes with their own materials through a modern GPU
pipeline. **Exact path** = today's Direct3D 11 GS renderer (runtime/gpu, notes/gpu-renderer.md), which stays as the
fallback, as the reference for "Original" and as the renderer of whatever R2 does not cover yet. **Hybrid frame** = one
picture drawn partly by R2 and partly by the exact path, in the game's own order, into the same targets.

---

## 0. The design in one screen

1. **Take over at the render list, one draw kind at a time.** Sly 2 sorts every visible piece of the world into one
   render list (16-byte entries at 0x3B3980) and emits it in f_00157FA0. A hook just before 0x158378 (where the widescreen
   hook already sits) sees each entry with its priority, its per-frame header packet (matrix, fade, lights, bones) and its
   static mesh DMA. If R2 handles the entry's kind, the hook copies the entry's per-frame data into a **scene packet**,
   writes a `NATIVE_DRAW` hint into the frame's chain (the same channel as the widescreen hints) and drops the entry's
   mesh CALL. The game sees no difference. Everything else is drawn as today.
2. **Meshes are decoded once, not every frame.** The meshes are prebuilt VIF DMA in level RAM that never change. R2
   decodes each one when it first sees it, or while the level loads, into GPU vertex and index buffers. Per frame it
   sends only matrices, fades, light sets and bone palettes. That removes the VIF unpack, VU1, GS decode and frontend
   work for those draws. Those are the four CPU stages that cap the frame rate today, at 5-9 ms each.
3. **One device, one command stream, one set of targets.** R2's first backend is Direct3D 11 on the exact path's own
   device and render thread. The `NATIVE_DRAW` hint travels through the worker, the GS thread and the frontend like any GS
   write. The render thread executes R2's draws at that point of the stream, into the exact path's colour and depth
   targets. They use the exact path's depth encoding (`z / 2^20`) and alpha convention. Shadows, 2D and every effect
   that reads the picture work unchanged.
4. **"Original" is defined by measured tolerances against the exact path.** It is not bit-exact: GPU floats are not the
   VU's truncating arithmetic. Four verification levels (section 5) gate every milestone. The exact path stays available
   as a setting for anyone who wants the bit-exact picture.
5. **Thin own API, D3D11 first.** `r2gpu.h` is a C layer of about 15 object types. The D3D11 backend comes first with no
   new tools. An SDL3 GPU backend for Linux and macOS (Vulkan / Metal) comes once R2 covers whole frames, and a direct
   Vulkan backend is the fallback.
6. **Playable and faster early.** The first visible milestone (R1) draws the static world of one hub natively behind a
   setting. All prelit meshes (R2, about 85% of the 3D vertices in the hubs) should roughly double the hub frame rate
   (about 100 to 190-200 fps at 3x on the owner's PC). Every milestone falls back per entry to the exact path, can be
   switched off while playing, and merges only after a full in-game sweep.

---

## 1. Goals and non-goals

**Goals**
- **Frame rate**: 200+ fps in the heaviest hub at the default 3x on the owner's PC (i7-14700F, RTX 4060 Ti, 1440p 165 Hz)
  once prelit meshes are native, and 300+ for fully modern frames. 1% lows above the monitor's refresh. Section 6 has the
  targets per milestone.
- **Use the hardware properly**: static data resident on the GPU, transforms and skinning on the GPU, a few hundred
  draws per frame, and data preparation on worker threads. No CPU stage should be busy in proportion to the triangle
  count.
- **A base for upgrades and mods**: a material record that carries both the game's own parameters and an Enhanced layer;
  free resolution and MSAA; motion vectors; asset overrides keyed by stable ids.
- **Portability**: one renderer core over a thin API layer: Windows (x64, ARM64), then Linux and macOS.
- **Always playable**: every milestone ships as a setting with per-entry fallback, switchable at run time. The exact path
  stays the reference and fallback, and stays bit-exact when R2 is off.

**Non-goals**
- Bit-exact equality with the PS2 in R2. The exact path already provides it.
- Running VU microcode on the GPU (perf-plan section 3.2: no precedent, no exactness, wrong stage). R2 re-implements the
  *known semantics* of each VU1 program, the way OpenGOAL and RT64 do.
- Frame interpolation. The game's dt is real time, so a higher frame rate is a real higher tick (frame-rate.md).
- A new graphics API for its own sake. D3D12 or Vulkan on Windows gains nothing at R2's draw counts.
- Replacing the exact path for 2D, menus, FMV and Binocucom tricks before R2 covers 3D. Those stay exact until R7, and
  on Windows possibly for good.

---

## 2. Architecture

```
 EE thread (game)                     r2 job threads              worker -> GS thread -> frontend -> render thread (D3D11)
 ----------------                     --------------              --------------------------------------------------------
 RenderSw builds + sorts render list
 0x157FA0: frame globals  --(hook)--> scene slot n: camera, VU 0..0xA constants
 per entry (hook before 0x158378):
   classify kind --exact--> CALLs emitted as today ---------------> VIF1 -> VU1 -> GIF -> GS decode -> FF/ROV records -> draws
              \--native--> copy header data into slot n
                           first sight of mesh DMA? --> decode job: mesh -> vertex/index arena (upload on the device)
                           write NATIVE_DRAW(n, first, count) hint into the chain (VIF DIRECT, A+D reg 0x7F)
                           drop the mesh CALL (zero entry +0xC; +8 too for whole static packets)
                           at the end of a native run: state epilogue (A+D of the last skipped material registers)
 frame end (0x1583FC): publish slot n --> pack job: instance table, bone palettes -> GPU-ready blob
                                                                  ... hint reaches the frontend in stream order:
                                                                  flush merges, ownership "R2 wrote target T in scissor",
                                                                  resolve textures in stream order, OP_R2_SEGMENT -> render
                                                                  render thread: R2 draws on the same RTV/DSV, then continue
```

### 2.1 Layers (from notes/modern-renderer-api.md section 5, now bound to Sly's data)

| layer | name | Sly 2 content | code (new, `runtime/r2/`) |
|---|---|---|---|
| 0 | game | recompiled EE; render list, globs, static world tree 0x1D5808, header packets | patches in recomp/patches.py |
| 1 | extraction "r2x" | (a) mesh decode (prebuilt VIF DMA to vertex/index streams + material runs), texture + CLUT decode, cache by key; (b) per-frame capture into scene slots; (c) classifier (native / exact per entry) | r2_capture.c (EE side), r2_mesh.c, r2_tex.c, r2_class.c |
| 2 | scene "r2scene" | mesh arena, texture pool (Texture2DArray per size class), material table (Original + Enhanced), instance table, bone palettes, lights, fog, camera; scene slots (ring of 8) | r2_scene.c |
| 3 | renderer "r2render" | segment executor (hybrid), later the whole-frame graph: opaque / alpha-tested / translucent in game order / effects / post / HUD | r2_render.c, shaders/r2_*.hlsl |
| 4 | RHI `r2gpu.h` | buffers, textures, samplers, baked pipelines, passes, command lists, compute, indirect, queries | r2gpu.h |
| 5 | backends | D3D11 (shares the exact path's device, targets and render thread); later SDL3 GPU; fallback Vulkan 1.3 | r2gpu_d3d11.c, r2gpu_sdl.c, r2gpu_vk.c |

### 2.2 Interception: what the hook does per render-list entry

Three EE patches, all "before" hooks at addresses already used by the widescreen code (no control-flow change):

- **Frame begin** (in f_00157FA0 after the frame globals are uploaded): open scene slot n. Copy the camera (CM 0x2DDF40:
  world->clip +0x140, camera->world +0x60, near/far +0x4C/+0x50) and the VU1 constants 0..0xA that every VU1 program
  reads: depth and screen constants, matrices, fog. R2's vertex shaders evaluate the same formulas from the same numbers,
  so widescreen, the field offset of pc-smooth and the game's own FOV changes come along without extra work.
- **Per entry** (before 0x158378; `$s2` = entry, `$s0` = priority): the classifier decides from the priority, the VU
  routine set of the mesh DMA (as `kind_of` in scripts/r2_frame.py) and the level / kind allowlist. Unknown -> exact.
  - For a native entry, the hook copies its per-frame inputs into the slot. These are the world matrix, the fade
    parameters (VU 8), the light set (REFS to VU 0x15-0x20), the bone palette (VU 0x21+), the warp matrices and the
    near-clip flag. They are copied *now*, because effect pools are rewritten while a chain runs (frame-rate.md: 5-14% of
    async jobs).
  - Consecutive native entries form one **run**. At the start of a run the hook writes one hint `NATIVE_DRAW(slot, first
    draw)` at the DMAS write position, using the `aspect_hint` mechanism (VIF NOP x3 + DIRECT, PACKED GIF tag, A+D to
    register 0x7F). At the end of the run, it writes `NATIVE_END(count)`.
  - **Suppression**: the hook zeroes entry +0xC, the static mesh CALL. The header packet at +8 is kept: it is a few
    quadwords of VU constant uploads with no MSCAL, so VU1 memory stays exactly as in an all-exact frame. Static world
    leaves whose +8 is a whole prebuilt packet (with MSCALs) get +8 zeroed too. The game's own `beq v0, zero` skips the
    zeroed CALLs. The list is discarded after the frame, so nothing else reads them.
  - **State epilogue**: after a run, the hook writes the GS registers that the last skipped material packet would have
    left behind (TEX0, TEX1, CLAMP, TEST, ALPHA, FOGCOL; known from the extraction) as one A+D packet. Exact draws after
    a run then decode with the same GS context as in an all-exact frame. This makes "the remaining exact draws are
    unchanged" true by construction and checkable (section 5, level 3).
- **Frame end** (0x1583FC, where `ws_render_done` sits): close any open run and publish the slot to the render side. A
  pack job turns it into GPU-ready instance data.

**Bracket mode** (verification, `PS2_R2=bracket`): the hook writes `NATIVE_BEGIN(i)` / `NATIVE_END(i)` around entry i
*without* dropping its CALLs. The stream then holds both versions of every native entry. The frontend draws either one
(exact, native or both into scratch targets), and `.gsrec` recordings made in this mode replay hybrid and exact frames
offline from one stream. Bracket mode costs the exact path's full price; it exists only for A/B checks and recordings.

### 2.3 Extraction (layer 1)

- **Meshes.** The key is the DMA address plus a 64-bit hash of the DMA bytes, computed once per new address. The whole
  cache is invalidated on a level change (hook in the level loader), because the level heap is reused.
  - The decoder walks the batch structure: REFS/CNT tags, the GIF template at VU 0x009, the 11 strip descriptors, the two
    9-qw material packets, and the vertex streams. Prelit vertices: UV V2-16 (4.12), colour A and colour B as
    RGBA5551 + S-8 alpha, position V4-32 with ADC flags in w. Lit/skinned vertices add V3-16 UV, a V4-8 normal, a V4-8
    colour and V4-8 weights.
  - Output: a vertex stream per mesh (position, UV, colour A, colour B, normal, weights, as present) and **triangle
    lists** with ADC strip restarts resolved. Triangles are grouped into **material runs** in the game's batch order,
    each with the pass A and pass B states (texture, two CLUTs, TEST, ALPHA, FOGCOL, CLAMP, TEX1).
  - The decode runs on r2 job threads. Until a mesh is resident, its entries stay exact for that frame, so there is
    never a stall or a missing object.
  - R0 adds a debug check that re-hashes decoded DMA every frame during sweeps. A mesh the game modifies at run time
    (UV scrolling, water, if any) is marked exact or re-decoded per change.
- **Pre-warm**: when a level finishes loading, a job walks the static world tree and the object list's glob sets and
  decodes everything in advance, during the game's own loading screen.
- **Textures** are resolved **in stream order on the frontend thread**, where `gs_vram` and the ownership table are
  consistent with the GS stream. The EE runs ahead of the GS thread, so reading GS memory at capture time would race.
  - Key: (TBP, PSM, TW/TH, CBP, CPSM, CSA, the content hash of the source pages + CLUT). The ownership table's `cpu_gen`
    of the pages says when to re-hash.
  - Decode: T4/T8 + CT32 CLUT to RGBA8 (raw GS alpha 0..0x80 kept), one image per (texture, CLUT) pair. Most textures
    have two: the pass A base and the pass B highlight palette.
  - Storage: a slice in a Texture2DArray per size class (GS textures are powers of two).
  - Level textures are resident and never re-uploaded during gameplay, so this decode happens about once per texture per
    level (0.5-1.2 MB indexed, about 10-20 MB decoded per frame's working set). The sky's per-frame upload into the Z
    pages is the exception: it gets re-decoded when its pages change, or stays exact until R5.

### 2.4 Scene representation (layer 2)

- **Mesh arena** per level: one vertex buffer and one index buffer, plus mesh records {vertex offset, triangle ranges per
  material run, bounds, kind, outline edge list if any}. Uploaded once.
- **Material table**: per material run, the Original record is exactly the game's parameters: TEX0 / CLUT A / CLUT B,
  TFX/TCC, TEST (alpha test value and mode), ALPHA selectors + FIX, fog colour, CLAMP, TEX1 filter, pass A / pass B. The
  Enhanced record is optional: normal map, roughness, emissive, HD albedo, outline style, casts-shadow. Both are indices
  into GPU tables.
- **Instance table** per frame (scene slot): {mesh, world matrix, previous matrix (motion vectors, R8), fade a/b, light
  set index, bone palette offset, warp matrices offset, draw order index}. Size: about 900 entries x 128 B = 115 KB per
  frame in the ep1 hub.
- **Draw order**: R2 keeps the game's sorted order inside every run. Within one entry, R1 draws each material run's pass
  A then pass B, exactly as the VU emits them. From R2 on, draws are consolidated: one draw per (entry, pass, sampler
  class), with the texture array slice per triangle. This is allowed only after the per-entry A/B shows the reorder is
  invisible. Pass B is additive with Z GEQUAL and no Z write, and pass A nearly replaces (alpha test >= 126). So moving
  all of an entry's pass B after its pass A can change pixels only where batches of one entry overlap and pass A blends
  partially. R2's A/B measures this rather than assuming it. Instancing of identical meshes comes on top.

### 2.5 The D3D11 backend and the hybrid frame (layers 4-5)

- **Device and thread.** `r2gpu_d3d11.c` uses the exact path's `ID3D11Device` and immediate context, on the render thread.
  Resource creation (mesh arenas, texture slices) uses the free-threaded device from job threads. Per-frame data goes
  through `Map(NO_OVERWRITE)` rings like the exact path's.
- **Stream order.** The frontend turns a `NATIVE_DRAW` hint into `OP_R2_SEGMENT{slot, first, count}`:
  - First it flushes its pending draw merges.
  - It treats the segment like an exact draw that reads and writes colour and depth over the scissor of the current
    FRAME/ZBUF: stale blocks are loaded first (`OP_UPLOAD_TARGET`), and afterwards the blocks are GPU-owned and valid.
  - Later downloads, CLUT-from-target loads, the shadow pass's reads and the motion-blur feedback therefore see R2's
    pixels as if the GS had drawn them.
- **Targets and sample grid.** R2 renders into the exact target found for the current FRAME/ZBUF, with its scale factors
  (kx, ky), including the anamorphic widescreen kx. The VS maps GS coordinates exactly as the exact path does: `d3d = (gs
  - XYOFFSET) * k + 0.5`. Sample 0 of every pixel is the native pixel position, so downloads, the verifier and later
  exact draws see one consistent target.
- **Depth.** R2 computes the GS's integer Z from the VU program's formula (VU 0/1 constants), in float, and outputs
  `SV_Position.z = z / 2^20` (P1 encoding, appendix P of d3d11-renderer.md), written as true screen-linear depth: z is
  multiplied by w before the divide. The comparisons are the GS's: Z GREATER / GEQUAL map to D3D GREATER / GREATER_EQUAL.
  Pass A and pass B of one mesh run the same `precise` vertex code, so their equal-Z ties hold exactly.
  - Ties against **exact** draws are not guaranteed. Hence the rule: a mesh and everything that ties against it move
    together. That means its pass B, its outline, and decals drawn onto it.
- **Colour and alpha.** Targets hold raw GS bytes (alpha 0x80 = 1.0). R2's Original pixel shader reproduces the GS
  arithmetic: vertex colour x2 modulate (`(Cv * Ct) >> 7`), the alpha test, fog `(F*C + (255-F)*FOGCOL) >> 8`. It writes
  the alpha byte the GS would write (As, with FBA if set), because the shadow pipeline's DATE tests bit 7 of
  destination alpha and the T8H darken reads it.
  - Blending uses the exact path's dual-source mapping: SV_Target1 carries As/128 as the blend factor. Pass A
  `(Cs-Cd)*As+Cd` and pass B `(Cs-0)*As+Cd` are fixed-function blends. No ROV is needed for anything R2 draws.
  - These helpers are shared with the exact path's HLSL includes, so there is one definition of the GS formulas.
- **Pipelines.** A closed set, enumerated in `runtime/r2/shaders/perms.txt` and compiled at build time by
  `scripts/hlslc.c` with d3dcompiler_47. All are created at boot on the exact path's warm-up. No shader compilation ever
  happens while playing.
- **Exact path with R2 off.** The hooks only run when `renderer = modern`. With R2 off, the GS stream is byte-identical
  to main (checked by m9_exact and the stream hashes).

### 2.6 Threads and frame rate

| stage | today (ep1 hub, 3x) | with R2 covering a draw kind |
|---|---|---|
| EE (game + list building) | ~2 ms | + capture ~0.1-0.3 ms (copies, hints) |
| worker (VIF unpack, VU1, GIF) | ~7 ms | only the exact remainder: header packets, outlines/shadows/particles/2D until ported |
| GS thread (decode) | ~7.8 ms | the exact remainder |
| frontend | ~8 ms | the exact remainder + one op per run + texture resolves (cached) |
| r2 jobs (new) | - | decode at first sight (load screens), per-frame packing ~0.1-0.2 ms |
| render thread | ~0.7 ms | exact draws + R2 draws (R1: ~2-4k draws, R2+: a few hundred) |
| GPU | ~7.8 ms per game frame in game | simple shaders instead of the exact ubershader on most fragments |

The pipeline's frame rate is 1000 / the slowest stage. R2 makes the three middle CPU stages shrink in proportion to the
share of the frame it draws, so the gains follow the measured shares: prelit = 77-84% of entries and 83-87% of source
vertices in the hubs (modern-renderer-re.md section 3).

---

## 3. Feature classes: now and later

| feature (render prio / VU1 program) | share (hubs) | first native in | until then | native method | notes |
|---|---|---|---|---|---|
| static world tree leaves (0x1D5808, world-space prebuilt DMA) | ~60% of entries | **R1** (one hub), R2 (all) | exact | decode once, draw with identity matrix + fade | biggest single block |
| prelit props / objects (glob, matrix + fade) | ~20% of entries | **R2** | exact | same decoder + world matrix | |
| two-pass materials (pass A alpha-test, pass B additive, second CLUT) | all meshes | R1 | - | two pipelines per run; two decoded images | |
| per-vertex fog, LOD fade (RGB_ONLY pass A variants) | all meshes | R1 / R2 | - | VU formula in VS; pipeline variant per fade state | fade TEST variants 0x53001 / 0x5000D / 0x317E1 to be read in R0 |
| lit meshes (light set, up to 3 lights + ambient) | ~10% of vertices | **R3** | exact | lighting in VS, VU formula | colour scale hypothesis 0..248, 128 = 1.0 |
| skinned characters (EE bone palette, V4-8 weights) | (in the above) | R3 | exact | bone palette from the header packet into a structured buffer; skinning in VS | OpenGOAL Merc2 precedent |
| cel outlines (188054331, winged-edge silhouette lines) | 11-18 entries | **R4** (with their meshes) | the outlined object stays exact entirely | silhouette test in a compute pass over the edge lists, lines as quads; "PS2" width and "Thin" | Z ties: must move with the mesh |
| procedural warps (0x22C028, up to 4 extra matrices) | few | **R5** | exact | matrices from the header packet; blend rule from the VU | |
| sky (prio 1, class 3, textures in Z pages, before the Z clear) | 1-3 entries | R5 | exact | per-frame texture from the frame's upload; drawn before the clear | ordering-sensitive |
| prio 0 attachments (752221467), prio 13 Z/stencil volumes | rare | R5 (identify) / R7 | exact | after identification | unmeasured semantics |
| particles (376109499, VU-simulated "blips") | 2-10 entries, heavy fill in fire/rain | **R6** | exact | port the VU physics to a compute pass; instanced quads | fire / rain scenes are GPU-bound today |
| shadow volumes + G-channel stencil + CT16 shuffles + T8H darken | 6-41 entries | **R7** | exact (reads R2's depth and alpha) | real stencil volumes + a darken pass; no channel shuffles | removes ALIAS16 work |
| motion blur (16 sprites, FIX 48) | full screen | R7 | exact | one full-screen blend | |
| 2D, HUD, text, menus (prio 21) | 12-34 entries | R7 (gameplay HUD), Windows: may stay exact | exact | widescreen anchors from the existing hints | cheap on the exact path |
| Binocucom RTT, ep2 photo copy, portraits (class 8/9) | special scenes | R7 or never (Windows) | exact | render-to-texture passes | required before Linux/macOS |
| FMV | - | already native | - | - | |

---

## 4. "Original" and "Enhanced"

**Original** reproduces the PS2 look: the same vertex colours, textures with their CLUTs, alpha test, blend, fog, fade,
lighting formula and outlines, through float GPU arithmetic. It is the default, and it is defined by the tolerances in
section 5. R2 never approximates when the exact formula is cheap. For example it applies the GS's own `>> 7` modulate and
alpha-scale conventions in integer-equivalent float math.

- *Why not pixel-exact.* The VU truncates (round toward zero, flush to zero) and the GS rasterises with integer records
  computed on the CPU. A GPU vertex shader rounds to nearest. So vertex positions differ by up to 1/16 pixel, coverage
  differs on some edge pixels, and perspective texture coordinates can pick the neighbouring texel. Being bit-exact
  would require the exact path's whole machinery, which is exactly the cost R2 removes.
- *An exactness lever kept in reserve:* the vertex transform alone (a matrix multiply, the divide and FTOI4) can be
  emulated VU-exactly in a compute pre-pass, with integer soft-float and round toward zero. That costs about 0.1-0.2 ms
  of GPU time for 160k vertices per frame. Positions and Z would then match the VU bit for bit, so silhouettes and Z ties
  against exact draws match. It is used only if section 8's tie risk shows up in practice.

**Enhanced** is a layer on the same material record. Each part is a separate switch, grouped into presets that extend
today's Modern / Faithful filter presets, and Original stays one click away. Planned switches:
- per-pixel lighting from the game's own lights,
- normal maps,
- real shadow maps beside or instead of the projected shadows,
- SSAO and bloom,
- anisotropic filtering with generated mipmaps (the PS2 data has none),
- HD texture and model replacement,
- outline styles: PS2 width, thin, screen-space edges.

Enhanced has no fidelity gate. It has two hard rules:
1. With every Enhanced switch off, the R2 output is bit-identical to Original.
2. Every switch has a measured cost.

---

## 5. Verification

Four levels, all automated. The gates are written to `tests/r2/gates.json` in R0, after one calibration pass on the
oracle data, and frozen afterwards. A gate is only loosened with the owner's agreement.

### Level 1: vertex oracle (offline, per decoded mesh)

`scripts/r2_mesh_check` takes a `PS2_VU_RECORD` recording and the frame's RAM dump (PS2_R2_TRACE). It decodes the meshes
with R2's decoder and evaluates R2's vertex function: a C twin of the HLSL, checked against the GPU on WARP and the
hardware by a conformance kernel. It compares the result per vertex with what the VU program XGKICKed (r2_vu_kicks).

| quantity | gate |
|---|---|
| primitive set (which triangles are drawn: ADC, culling, guard band) | identical; <= 0.1% differ, only at clip boundaries |
| X, Y (GS 12.4) | \|d\| <= 1 unit (1/16 px) for >= 99.9% of vertices, never > 4 |
| Z (GS integer) | \|d\| <= 2 for >= 99.9%, never > 16 |
| S, T, Q | relative error <= 2^-16 |
| RGBA (prelit) | exact |
| RGBA (lit) | within +-1 for >= 99%, never > +-2 |
| fog F | within +-1 |

### Level 2: per-entry A/B (bracket mode, offline and in game)

A native entry is drawn both ways into scratch copies of the same target state, at S = 1 and at S = 3 (sample 0).

| quantity | gate |
|---|---|
| coverage | pixel presence differs on <= 1% of the entry's pixels, and only on edge pixels |
| colour where both cover | >= 99% within +-2 per channel; >= 99.9% within +-8; <= 0.1% above 16 (texel picks) |
| alpha byte | bit 7 identical on >= 99.9% (DATE); the byte within +-2 on >= 99% |
| depth | \|dZ\| <= 4 units on >= 99.9% of shared pixels |

### Level 3: whole frame, hybrid vs exact (offline replay of bracket recordings, and in game)

| check | gate |
|---|---|
| remainder invariance | the exact draws' GS primitive stream in the hybrid frame hashes identical to the same draws in the all-exact frame. This proves that suppression, epilogues and VU state carry-over change nothing else |
| S = 1 reference compose | PSNR >= 38 dB; <= 0.5% of pixels differ by > 16; mean signed error < 0.5 per channel |
| S = 3 box downsample | PSNR >= 34 dB; mean signed error < 0.5 |
| temporal | over 60 frames, the hybrid picture adds <= 0.1% pixels that toggle between frames when the exact one does not (catches Z-fighting / tie flicker) |

For comparison, the exact path's own S = 1 gate is 40 dB and <= 0.2%. R2's looser numbers account for edge coverage.

### Level 4: in game

- **Chaos benchmark** (`scripts\chaos_bench.ps1`, the scenes of tests/gpu/chaos.txt, release, unlimited frame rate, 3x
  16:9, quiet machine, builds interleaved): median fps, 1% and 0.1% lows, per-stage ms. `-Exes` compares `renderer =
  exact` and `modern` in one session.
- **Sweep**: warp_run through the prologue, every hub and one job per episode (the M8 list), with R2 on and the
  in-game A/B (`PS2_R2_AB=60`) every 60 VBlanks. Pictures are dumped sparsely and judged on contact sheets. A
  `m11_long`-style 22-minute run checks memory and cache growth.
- **Coverage census**: per scene, the share of entries, vertices and GS primitives still exact, and every fallback
  reason (unknown kind, mesh not resident, modified DMA). This is the milestone's progress number.
- **Owner check**: a dist build with the setting, before each milestone becomes the default.
- Disk hygiene (memory: disk-hygiene): dumps sparse, evidence frames only in build\screenshots\, recordings and RAM dumps
  deleted after use, stop below 50 GB free on C:.

Every existing exact-path gate still runs and must stay identical with R2 off: m9_exact, gpu_diff_all, stream hashes,
gs-hw. The in-game verifier (`PS2_GS_VERIFY`) runs in bracket mode or excludes R2-written pages, because the software
renderer never sees native draws.

---

## 6. Performance targets

Measured with the chaos benchmark on the **owner's PC** (i7-14700F, RTX 4060 Ti, 1440p): quiet machine, 3x 16:9 (render
scale 75%) unless noted, frame rate unlimited, medians of interleaved repeats. Baseline: main after perf P1-P3 (chaos
table in perf-plan.md, P3 results). CPU ms are per game frame and per stage; GPU ms are per game frame in game. The
targets after R0 are estimates scaled from the measured shares. Each milestone measures its own numbers and records them
here.

| after | ep1 hub fps (1% low) | ep3 / ep5 hub fps | m2 boardroom (fire) fps | slowest CPU stage, ep1 (ms) | GPU ep1 (ms) |
|---|---|---|---|---|---|
| baseline (main) | ~102 (64) | ~120 / ~125 | ~90 | frontend 8.1, GS 7.8, worker 7.2 | 7.8 |
| R0 | unchanged (+-3%) | unchanged | unchanged | + EE capture <= 0.1 | unchanged |
| R1 (static world, ep3 hub) | unchanged (exact) | ep3 >= 165 (>= 95) | - | ep3: each of worker / GS / frontend -45% | ep3 -35% |
| R2 (all prelit, all levels) | **>= 190 (>= 120)** | >= 220 / >= 210 | ~95 | <= 2.5 | <= 4.0 |
| R3 (lit / skinned, non-outlined) | >= 195 | >= 225 | ~95 | <= 2.3 | <= 3.8 |
| R4 (outlines + outlined objects) | >= 230 (>= 150) | >= 260 | ~100 | <= 1.8 | <= 3.5 |
| R5 (sky, warps, the rest of 3D) | >= 240 | >= 270 | ~100 | <= 1.6 | <= 3.3 |
| R6 (particles) | >= 240 | >= 270 | **>= 160 (>= 100)** | <= 1.6 | <= 3.0 (m2: 11 -> <= 5) |
| R7 (shadows, blur, HUD: exact path idle in gameplay) | **>= 280 (>= 180)** | >= 300 | >= 220 | EE ~2.5 (GS / frontend <= 0.8) | <= 2.5 |
| R8 (fully modern, 1440p native + MSAA 4x) | **>= 300 (>= 200)** | >= 320 | >= 250 | EE ~2.5 | <= 2.0 (4K: <= 4.0) |
| R9 (Enhanced "High") | >= 165 at 1440p | >= 165 | >= 165 | - | <= 5.5 |
| R10 (Linux, same PC) | >= 90% of Windows R8 | | | | |

From R7 on, the game's own EE thread (~2-2.5 ms: game logic plus building its render list) is the limit, about 400 fps.
Raising it further is perf-plan P6 work (PGO/LTO, in-game overhead), independent of R2.

**Weaker reference PC.** A 4-core / 8-thread desktop CPU of ~2017 (i5-7600 / Ryzen 5 1600 class), a GTX 1060 6 GB /
RX 580 class GPU, 1080p 60-144 Hz, internal scale 2x.

| after | ep1 hub fps at 2x (est.) | limit |
|---|---|---|
| baseline | 45-60 | CPU: five busy threads on four cores, each stage 1.5-2x slower |
| R2 | >= 100 | GPU / EE |
| R7 | >= 144 | EE |
| R8 (1080p native, MSAA 2x) | >= 144, GPU <= 4 ms | EE |

Until a real machine is available these numbers are checked on a **proxy**: the process restricted to 4 E-cores of the
i7-14700F through CPU sets, for the CPU stages, plus the 4060 Ti's GPU ms scaled by 2.5x. (Owner decision D5.)

---

## 7. Milestones

Each milestone is one autonomous agent task in its own worktree, with a checkpoint file `notes/wip/r2-<n>.md`. It
merges only when its gates pass. "Default" means the milestone may switch `renderer = modern` on by default for what it
covers; the owner confirms after testing the dist build. Effort is in agent-days of focused work, including gates.

### R0. Foundations (no visible change) - 5 agent-days
- **Deliverable**:
  - `runtime/r2/` skeleton; `r2gpu.h` + D3D11 backend on the shared device, with a conformance scene in
    `tests/unit/r2` (hardware and WARP).
  - The three EE hooks in observe mode: scene slots, classifier, `NATIVE_*` hints in bracket mode, with the frontend
    and render thread passing them through as no-ops. `.gsrec` carries the slot payloads.
  - The mesh decoder for prelit batches; `scripts/r2_mesh_check` (level 1 oracle); the DMA-modification check.
  - Read routines 0x2E20 / 0x2E68 / 0x2EB0 / 0x2EE8 of 376110651 and write down the exact prelit transform, Z, fog,
    fade and clip / ADC rules (notes/vu-microcode.md).
  - `tests/r2/gates.json` calibrated.
  - Settings: `[graphics] renderer = exact | modern` (Options: "Rendering: Modern / Reference"); env `PS2_R2=off |
    bracket | hybrid`, `PS2_R2_KINDS`, `PS2_R2_AB`.
- **Verification**: level 1 gates on the ep3 hub VU recording for every prelit routine. With R2 off, m9_exact and the
  stream hashes are identical. With bracket mode, the exact pictures are unchanged (the hints are ignored).
- **Gain**: none (EE capture <= 0.1 ms measured).

### R1. Static world natively in one hub - 7 agent-days
- **Deliverable**:
  - Static world tree leaves (entries written by 0x1D5808, both CALL forms: whole packet, or REFS + fade) drawn
    natively in the ep3 hub, chosen
    because its traces and VU recording already exist.
  - Hybrid execution: `OP_R2_SEGMENT`, ownership marking, epilogues, and the stream-order texture cache.
  - Original shaders for pass A / pass B / fog / fade; the game's batch order (no consolidation yet).
  - Allowlist of levels; per-entry fallback reasons counted.
- **Verification**: levels 1-3 on new bracket recordings of the ep3 hub (open view, at a wall, rain). Chaos ep3 scene.
  The ep3 part of the sweep plus 10 minutes of free play with `PS2_R2_AB=60`.
- **Gain**: ep3 hub about 120 -> >= 165 fps at 3x.
- **Default**: off; the owner can try it with the setting.

### R0 + R1 as built (next/r2-m1, 2026-10-26)
- **Code**: `runtime/r2/` (r2.h overview; r2_capture.c EE hooks + classification; r2_mesh.c decoder + mesh store;
  r2_vertex.c the VU-exact vertex function; r2gpu.h / r2gpu_d3d11.c; r2_render.c), `runtime/gpu/gpu_r2front.c`
  (GPU_FOP_R2 segments), shaders `r2.hlsl`, `r2_vu.hlsli`, `r2_conf.hlsl`; patches `r2m_alloc / r2m_entry / r2m_done`
  (recomp/patches.py); setting `[graphics] renderer_mode = reference | modern` (`PS2_R2=off|hybrid|bracket`).
- **Classification** (r2_cap_entry, per render-list entry before 0x158378): native when the entry is not from the
  allocator 0x1CD998 (static world, 0x1D5808), its level is allowlisted, the frame's screen constants have the
  float-bits form, class 0 (main program), the mesh decodes (cached per DMA address with a 32-byte fingerprint, decode
  budget 3 ms per frame), form 1's header parses, VU 8.w == 0, and the mesh's bounding box lies inside the VU's guard
  band in clip space (`|x|, |y|, |z| <= 0.999 w`, w > 0; else the VU's clipper would draw fans: the entry stays exact,
  reason "guard"). Hybrid drops the entry's mesh CALL; runs of native entries are preceded by `MSCAL 0x3AB0` +
  `MSCAL 0` (the previous exact entry's pending kicks, DBF parity kept) and followed by a state epilogue (the last
  pass's A+D registers).
- **Exactness (beyond the design)**: the reserve lever of section 4 is in, in a stronger form, because the gates needed
  it (Z ties at steep surfaces: Z exact only 71% with the float transform):
  - the vertex shader evaluates the VU's own operation sequence (0x07F8 MULA/MADDA chain, DIV, MULQ, ADDA + MADD,
    0x0070's W x C on the frontend) with round-toward-zero emulated through error-free transforms (Dekker / Knuth),
    so X, Y (12.4), Z, F, S, T, Q are bit-exact (level 1: 100% on the ep3 hub). Positions are final screen positions
    (w = 1, nothing to clip inside the guard band), S/T/Q interpolated linearly and divided per pixel like the GS.
  - a geometry stage computes each triangle's Z record (row, dy, dx at by0) exactly as the frontend's gs_tri_setup /
    gs_rowstep_attr would, in exact 64-bit rational arithmetic (r2_vu.hlsli), and the pixel shader evaluates it with the
    exact path's own gs_z16_sample: every sample of every pixel gets the exact path's Z, at any scale.
  - `tests/unit/r2` checks the arithmetic bit for bit on hardware and WARP (16384 random cases each).
  - cost (PS2_GPU_PROF=1 on a 10-VBlank bracket replay of the ep3 hub, 3x 16:9, RTX 4060 Ti; GPU clocks vary between
    runs, so compare within a run): the world drawn by the exact path costs ~0.37 ms (its share of ff-tri-fast-z);
    R2 draws the same pixels (PS invocations equal) in ~1.06 ms with the interpolated Z, ~2.44 ms with the geometry
    stage's Z records. Hence the default is the interpolated plane through the exact vertices (Z exact on 99.95% of
    pixels, the rest +-1; A/B gates pass), and `PS2_R2_ZREC=1` selects the exact records (100%). Stubbing the CLUT
    lookup changed nothing: R2's cost is its ~3000 small draws per frame (one per pass per sub-batch, state changes
    between pass A and B), where the exact path draws the world in a few long FF draws - draw consolidation (R2
    milestone) is what makes R2 cheaper than the exact path on the GPU, not the shaders.
- **Verification**: level 1 (scripts/r2_mesh_check.c, ep3 hub VU recording): 374 / 378 world entries decoded (2 mscal,
  2 vertex fallbacks), X / Y / Z / fog / S / T / Q / RGBA all exact, materials identical, no VU-drawn triangle missing.
  Levels 2-3 (scripts/r2_ab.ps1: a bracket recording replayed with `--r2-ab 0 / 1`, GS memory compared): S = 1 Z 100%
  exact; colour PSNR 61 dB, 0.11% of pixels > 2, 0.002% > 16; alpha bit 7 identical; at 3x 16:9 the pictures agree
  (4798 of 2.75 M samples differ by > 8, texel picks at edges). Note: the ep3 hub's brazier light flickers, so
  Sly's shadow darkness varies frame to frame - compare shadows only on the same frame (A/B), never across runs.
- **Performance** (chaos ep3_hub, 3 interleaved repeats each, release, 3x 16:9, unlimited, owner's PC; base = main
  630a4c7, new = hybrid with interpolated Z; tag r2m1_final): fps median 193.8 -> 208.3 (+7%); 1% low 84.1 -> 70.8,
  0.1% low 44.1 -> 46.5; per-stage medians worker 4.06 -> 3.18, GS 3.59 -> 2.71, frontend 3.68 -> 2.78 (p90 8.12 ->
  4.53), render 0.57 -> 0.61, GPU 5.01 -> 4.70 ms; GS primitives per frame 22.0k -> 16.3k; EE capture 0.16-0.27 ms
  per frame (hints 45-85 KB per frame: the form-1 entries' world matrices are not the identity). The scene's load
  varies a lot between runs (rain, camera: 15-25k primitives), so +-5% is noise. The 1% low is lower: frontend
  spikes up to 12.4 ms in hybrid runs (8.7-11.2 in base) - first-sight mesh decoding is not it (decoded 0-1 per
  second while playing); to be found in R2. Far short of the design's ep3 >= 165 fps target only because the
  baseline already moved (~120 at design time -> ~194 now); the GPU (~5 ms) now bounds the frame and R1 adds GPU
  work (see cost above), so the CPU stages' -24% shows mostly in the p90 / smoothness, not the median fps.
- **Exact-path finding (not changed here)**: `int64.hlsli i64_from_float_floor` is off by up to 128 for negative v
  above -2^32 (its low word 2^32 - k rounds to a multiple of 256 as a float; floor(-2.5) gives -1). In gs_z16_sample's
  sub-sample step (16.16 units) that is <= 2^-9 Z units, harmless; R2 calls the same function, so ties hold. A general
  use needs r2_vu.hlsli's r2_floor_i64.

### R2. All prelit meshes, all levels, draw consolidation - 8 agent-days
- **Deliverable**:
  - Glob prelit props and objects (world matrix + fade from the header packet; the light-prefix and fixed-sort-key
    variants).
  - Every level; the allowlist becomes a blocklist.
  - Pre-warm at level load.
  - Draw consolidation: texture arrays per size class, one draw per (entry, pass, sampler class), instancing of
    repeated meshes.
  - Objects that have outlines stay exact (rule: an entry whose glob set has an outline subglob is exact until R4).
- **Verification**: all four levels on bracket recordings of every hub plus three jobs. The consolidated vs
  unconsolidated A/B is within level 2. Full sweep; 22-minute run.
- **Gain**: **the big one: hub frame rates roughly double (ep1 about 102 -> >= 190 at 3x)**. Render thread draws from
  about 4k to a few hundred.
- **Default**: on after the sweep and the owner's check.
- **Decision**: D3 (stop perf P4/P5) should be settled before this milestone.

### R2 as built (next/r2-m2, 2026-10-26)
- **Coverage**: every level (the allowlist became a blocklist: `PS2_R2_BLOCK`, empty). Native: static world leaves (R1)
  and **glob props** (allocator entries of render class 0 with a header packet + mesh DMA; the header's matrix and VU 8
  as for form-1 world entries, light-prefix headers parse unchanged). **Outlined objects stay exact**: at the frame's
  first entry r2_capture.c parses every outline entry's (render class 1) header and a prop whose world matrix one of them
  uploads is exact (ep1 hub: ~1 prop / frame). **Guard-band meshes**: built, opt-in (`PS2_R2_CLIP=1`, `R2E_CLIP`): the
  clip pipeline (`vs/gs/ps_r2_clip`) decides per triangle like the VU (0x0C60's CLIPW flags): all inside, the VU-exact
  screen vertices; all outside one plane, nothing; else the GPU clips the clip-space triangle against the VU's six planes
  (SV_ClipDistance) with perspective-correct attributes. **Off by default**: the VU clipper's own fans are not on the
  triangle's plane (their Z differs from the plane by up to ~0.1%, e.g. -73 units at Z 92k on the ground in front of the
  camera), so the A/B Z gate fails where the near ground is clipped (ep5 hub: 21% of the pixels; the GPU-clipped picture
  itself looks the same). Reproducing 0x2150 / 0x1DC8 (the clipper and its projection) in the geometry stage is the way to
  make them native (R5). ep1 hub per frame: ~590 world + ~90 props native; exact: ~20-30 guard-band meshes, lit/skinned
  props and characters (decode "mscal"), ~1 outlined prop.
- **Hints -> scene slots**: hybrid mode writes each native entry's W x C (computed once on the EE with `r2_matmul_fast`,
  plain SSE under the VU's MXCSR rounding, unit-tested bit-exact against `r2_matmul`), fade and mesh into the frame's
  slot (`r2_slot`, a ring of 16, published at the frame's end); the chain carries one `R2H_RUN` per run. Hints 155 ->
  6-11 KB/frame, EE capture 0.52 -> 0.16-0.28 ms (with props). Bracket mode keeps the inline words (recordings).
- **Draw records / consolidation**: static material records per (mesh, sub-batch, pass) uploaded with the mesh (t10,
  base per mesh in a mesh table t11), the vertex carries its sub-batch, a draw record is 32 bytes (was 64 with the
  material). One pipeline (the PS always writes the dual-source factor: no shader switch between pass A and B).
  Consolidation built and measured, **opt-in** (`PS2_R2_CONSOLIDATE=1`): per entry one draw per (pass, state, discard
  rule) over all its sub-batches (a batch mask where some belong to another draw), ep1 4271 -> ~1500 draws/frame.
  **The draw count was not R2's GPU cost** (fixed-stream replay, ep1 hub in a searchlight, 3x: unconsolidated 0.46 ms,
  consolidated 0.44, the exact path's FF for the same entries 0.44): R2 costs the same per pixel as the exact path; knobs
  (`PS2_R2_DBG`): no texture 0.44, rasterized Z 0.43, float VS 0.45, constant PS 0.25 - fill / raster bound. And drawing
  an entry's pass A before its pass B is not invisible (the design's condition): foliage edges where a later batch's
  translucent pass-A pixels cover an earlier batch's pass B change (ep2 hub: 0.19% of the pixels > 8, 0.06% > 16; in the
  VU's order 0.085% / 0%). So the default keeps the VU's order; the render thread's cost of ~4k draws is ~0.4 ms.
- **Hitches when GPU-bound** (the cause of r2-m1's 1% low): with the CPU stages at ~2 ms the game becomes GPU-bound at
  3x, and the driver then blocked the render thread for 10-50 ms in whichever call hit its queue limit (R2's per-segment
  constant buffer update 2-13 ms, the exact path's data-arena Map 12 ms): 30-50 ms frames every ~0.2 s. Fixes: segment
  constants are a record in fdata (`gpu_r2_seg`; draw ids = the draw record's fdata offset / 32 through
  StartInstanceLocation; the static cbuffer is written once) and a **GPU frame fence** in gpu_exec.c (an event query
  per frame; frame N is recorded once the GPU finished frame N-2; `PS2_GPU_FENCE=n`, 0 off): ep1 hub modern 1% low
  27.5 -> 115.5 fps (median 233 -> 205: the shallower queue).
- **Options**: Graphics > **Rendering**: Modern / Reference (exact) (live); **F10** switches while playing. Default stays
  Reference until the owner's check.
- **Verification**: `tests/r2/gates.json` (calibrated, frozen; `scripts/r2_ab_compare.py --gates`, `r2_ab.ps1` exits
  with it). `scripts/r2_sweep.ps1`: the A/B on every hub + 3 jobs - **11 / 11 pass** (ep1-ep8 hubs, m2 boardroom, m3
  boss, m8 boss; S = 1 frame buffers PSNR 59-999 dB, > 16: <= 0.001%, Z within 4 >= 99.998%). Harness fixes on the way
  (the hybrid path was right): exact primitives are skipped only for entries R2 took (a recording starting after its
  frame's R2H_FRAME), and the last native entry before a render-class switch is not bracketed (the switch's packets,
  e.g. the shadow pipeline's stencil init, fell inside the bracket). `scripts/r2_prof_ab.ps1`: GPU cost exact vs
  hybrid on a fixed stream.
- **No-discard PS**: draws whose alpha test cannot split the fragments (no test, all pass, all fail: most of them) use
  a variant without `discard` (`ps_r2_nd`, state bit 14 marks the others); the PS reads no segment record (FBA travels
  with the material index).
- **Performance** (chaos benchmark, all 8 scenes, reference vs modern from one exe, interleaved, release, 3x 16:9
  unlimited, owner's PC; tags r2m2_final (1 repeat) and r2m2_final2 (m3 / m8 boss, 2 repeats, with the no-discard PS);
  fps median / 1% low / 0.1% low; CPU stage ms worker / GS / frontend; GPU ms):

  | scene | reference | modern | modern CPU stages | GPU ref -> mod |
  |---|---|---|---|---|
  | ep1_hub | 113.8 / 77.1 / 42.4 | **203.3** / 114.3 / 53.1 | 2.2 / 1.7 / 2.1 (r2m2_c9: 205.1, 2 repeats) | 5.2 -> 4.8 |
  | ep3_hub | 191.9 / 88.8 / 47.6 | 192.7 / 93.8 / 54.6 | 2.7 / 2.3 / 2.3 | 5.0 -> 5.1 (GPU-bound) |
  | ep5_hub | 154.3 / 116.0 / 48.3 | **216.0** / 142.2 / 59.4 | 1.7 / 1.5 / 2.1 | 4.8 -> 4.5 |
  | m1_brawl | 159.2 / 80.1 / 43.3 | 204.1 / 118.0 / 57.7 | 2.2 / 1.7 / 2.1 | 5.3 -> 4.9 |
  | m2_boardroom (fire) | 132.6 / 81.7 / 57.9 | 132.6 / 79.3 / 59.0 | 1.7 / 1.1 / 1.5 | 7.4 -> 7.4 (GPU-bound) |
  | m3_boss | 227.6 / 139.9 / 53.4 | 218.1 / 124.5 / 55.7 | 2.2 / 1.8 / 1.8 | 4.3 -> 4.5 (GPU-bound) |
  | m5_boss | 159.7 / 91.6 / 48.2 | 193.1 / 109.3 / 59.1 | 2.0 / 1.7 / 2.4 | 5.0 -> 5.1 |
  | m8_boss | 269.6 / 119.9 / 56.4 | 269.5 / 163.5 / 60.2 | 1.0 / 1.0 / 1.0 | 3.7 -> 3.6 |

  The CPU stages fall to ~2 ms (design: <= 2.5; ep3's worker 2.7), so most scenes become **GPU-bound at ~4.5-5 ms**:
  R2 costs the GPU what the exact path did for the same pixels, so it cannot lower it, and the design's GPU <= 4.0 ms /
  ep3 >= 220 fps need the remaining exact GPU work (shadows, motion-blur sprites, the ROV tier: ~1.5-2 ms in ep1) moved
  or optimised (R6 / R7). Where GPU-bound, modern equals reference (m2, m8, ep3) except m3_boss (-4% median, 1% low
  140 -> 125: GPU +0.2 ms in that fight; a fixed-stream replay of the boss intro shows parity). 22-minute m11_long with
  modern on (every episode): private bytes 696 -> 1009-1107 MB, flat (reference: 1138-1144), no dropped / stale /
  unsupported R2 work, the only slow captures the first frame of a level (decode budget).
- **Not done**: pre-warm at level load (first-sight decoding costs ~15 us a mesh under the 3 ms budget; entries stay
  exact for a frame or two), texture arrays (the GS memory sampling is not R2's GPU cost), instancing of repeated meshes.

### R3. Lit and skinned meshes - 7 agent-days
- **Deliverable**:
  - The lit / skinned routines (0x2EF8 / 0x2F80 / 0x2FC8 / 0x3358) read and documented: light set layout, colour
    scale, blend rule of the V4-8 weights.
  - Bone palettes from the header packet into a structured buffer; skinning and lighting in the VS.
  - Non-outlined lit objects native.
- **Verification**: level 1 with the lit RGBA gates (needs a VU recording with characters: museum, ep1 hub);
  levels 2-4 as before.
- **Gain**: small (most characters are outlined): +3-5%. Its value is that R4 builds on it.

### R4. Cel outlines with their meshes - 7 agent-days
- **Deliverable**:
  - Outline edge lists (V3-32 positions + V4-8 winged edges) extracted with the mesh.
  - Silhouette test in a compute pass (front / back facing of the two adjacent triangles), lines expanded to quads.
  - The PS2 style (one native pixel wide, Z-only pass then AA1 pass, ties against its own mesh by construction) and the
    Thin style (the existing setting).
  - Outlined objects native as a whole: mesh, pass B and outline.
- **Verification**: per-entry A/B with the outline pixels gated by coverage (level 2); temporal flicker gate on
  characters walking. The owner's eye on Sly's outline at 3x in both styles.
- **Gain**: hubs >= 230 fps; all characters off the exact path.

### R3 + R4 as built (next/r2-m3, 2026-10-27)
- **Lit / skinned meshes (R3)**: the main program's lit routines read (notes/wip/r2-m3.md "Findings"): 0x2EF8 /
  0x2F80 / 0x2FC8 light and skin both sub-batches of a buffer, 0x3358 (the next MSCAL) transforms them with the prelit
  transform. r2_mesh.c decodes them: the strips as prelit, plus each strip vertex's source record through the routines'
  output lists (qw0.z links; both lists of a buffer may write either strip, the later wins), its V4-8 normal, colour,
  rest position and weights; per sub-batch the lit flag, skin kind (0x20: 4 bones, positions + normals renormalised x 127;
  0x10 and 0x40 bone streams stay exact), 4 bone offsets, brightness [16].x and texture-animation UV offsets (0x0D28;
  TEX0 swaps stay exact). 0x0298 (light set into object space through 0x3BA8's inverse, attenuation of point lights,
  transpose / 127, colour x z x 2, ambient x 2, thresholds -y / z) runs on the EE per entry (r2_lights_prep); 0x1050
  (f = min(d + d^3, threshold) per light; colour A = clamped light x the normal's w, colour B = vertex colour x the
  highlight share) and the skinning run in the vertex shader (r2.hlsl r2_lit: positions with the VU's rounding, colours
  in floats). Level 1 (scripts/r2_mesh_check.c, every recorded lit program against the C twin r2_lit_eval): ep1 hub
  6464 lit vertices + 4939 skinned positions bit-exact, light blocks bit-exact; m3 boss 9832 lit (7 colour B +-1), 7184
  skinned positions exact. Render class 11 (class 0 inside FBA_1 = 1: characters) is native too.
- **Per-entry data**: the light block (11 qw) + the header's qwords 0x15.. (bones, UV offsets) as "extra" qwords: hybrid
  in the scene slot's pool (r2_slot.xq), bracket as R2H_XQ blocks deduplicated per frame (an object's meshes share one
  palette). The game's frame builder (DMAS, 360 KB) is the limit of bracket mode: ep3's hub left 8 KB free at the
  frame's end, and running it full hung the game; bracketed entries and blocks now keep 32 / 64 KB free (stat "builder
  free").
- **Outlines (R4), not as the design's compute pass**: the outline program 188054331 is reimplemented in C
  (r2_outline.c: set-up 0x0738 with W x C and the colour, batches 0x07F0 = skinning 0x01E0, projection 0x03C0, edges
  0x0500 with the clip / silhouette tests, the 0.00024 push-out and the screen transform), operating on a VU memory
  image and emitting the packets the VU would XGKICK, bit for bit (level 1: ep1 44 programs / 501 kicks, m3 boss 192 /
  2018, all identical packets and memory). An outline entry's mesh chain becomes a script (VU writes + MSCALs,
  r2_outline_decode, checked once to read only written memory); per frame the EE runs it with the entry's header and
  puts the packets in the slot, an R2H_OUTLINE hint replaces the mesh CALL and the GS thread decodes the packets in
  place. So the lines are the exact path's own (ps2 and thin outline settings unchanged, AA1, drawn twice) and their Z
  ties with their meshes are an all-exact frame's (R2's mesh Z equals the exact Z on >= 99.95% of pixels). An object is
  native with its meshes when its outline script decodes (else its meshes stay exact, recognised by the world matrix
  or, for skinned ones, a bone's translation row). The VU1 / VIF work of outlines leaves the worker; the EE pays
  ~0.1 ms per frame for outlines and light blocks (capture 0.24 -> 0.34 ms in the ep1 hub).
- **Performance** (chaos benchmark, all 8 scenes, reference vs modern from one exe, interleaved, 1 repeat, release, 3x
  16:9 unlimited, owner's PC; tag r2m3_final; fps median / 1% low / 0.1% low, modern CPU stages worker / GS / frontend,
  GPU ms ref -> mod):

  | scene | reference | modern | modern CPU stages | GPU |
  |---|---|---|---|---|
  | ep1_hub | 112.5 / 77.1 / 42.1 | **198.0** / 114.9 / 56.8 | 2.0 / 1.9 / 2.2 | 5.0 -> 5.0 |
  | ep3_hub | 211.4 / 82.0 / 46.8 | 207.5 / 98.4 / 59.1 | 1.7 / 1.5 / 1.4 | 4.6 -> 4.7 |
  | ep5_hub | 153.1 / 117.1 / 48.9 | **214.6** / 134.8 / 63.3 | 1.3 / 1.2 / 1.7 | 4.9 -> 4.6 |
  | m1_brawl | 137.4 / 77.6 / 43.6 | 198.4 / 113.1 / 56.6 | 1.5 / 1.4 / 1.7 | 5.4 -> 5.0 |
  | m2_boardroom | 126.3 / 76.9 / 53.1 | 135.3 / 82.8 / 60.3 | 1.2 / 0.9 / 1.3 | 7.8 -> 7.2 |
  | m3_boss | 225.2 / 136.5 / 51.1 | 228.3 / 144.9 / 63.4 | 1.3 / 1.4 / 1.3 | 4.4 -> 4.3 |
  | m5_boss | 159.5 / 86.2 / 45.1 | 207.0 / 114.4 / 61.4 | 1.3 / 1.3 / 1.9 | 4.8 -> 4.7 |
  | m8_boss | 281.7 / 113.6 / 52.6 | 262.5 / 159.7 / 56.9 | 0.6 / 0.7 / 0.9 | 3.5 -> 3.8 |

  The CPU stages fall again (R2: ep1 2.2 / 1.7 / 2.1, m1 2.2 / 1.7 / 2.1, m5 2.0 / 1.7 / 2.4; GS primitives ep1 12k, m1
  9.8k), but every scene is GPU-bound at 4-5 ms as after R2, so the median fps stays where R2 left it (ep1 203 -> 198,
  m3 218 -> 228, m5 193 -> 207: single runs, +-5% noise) and the design's hubs >= 230 fps is not reached: R3 / R4 remove
  CPU work, not GPU work (the meshes cost the same per pixel either way). The 1% / 0.1% lows are at or above
  reference everywhere. The GPU time left is the exact remainder (shadows, motion-blur sprites, ROV tier) and R2's own
  fill: R6 / R7 territory.
- **Found, not fixed (pre-existing on main)**: ep1 hub in hybrid mode, a large tree in front of the theatre sign is
  missing and shard polygons with the sign's texture appear (main's R2 too; bracket A/B passes): spawned as its own task.

### R5. Sky, warps and the rest of 3D - 6 agent-days
- **Deliverable**:
  - Sky (class 3: per-frame texture source, drawn before the Z clear in the game's order).
  - Procedural warps.
  - Fade-dependent TEST variants completed.
  - Identification of prio 0 attachments and prio 13 volumes; native where they are plain meshes, otherwise documented
    and kept exact.
  - Coverage census over the whole sweep.
- **Verification**: levels 2-4; census: >= 98% of 3D mesh vertices native in every swept scene.
- **Gain**: +3-5%, plus coverage; it is the precondition for R7 / R8.

### R6. Particles - 7 agent-days
- **Deliverable**: the 376109499 "blip" program's simulation (emitter parameters, per-particle 6-qw records, gravity,
  dt) ported to a compute pass; instanced textured quads; translucent draws in the game's order.
- **Verification**: per-particle positions vs VU output on recordings (level 1 style: XY <= 2 px, colour +-2); fire
  and rain scenes with levels 2-4.
- **Gain**: fire / rain / fight scenes, which are GPU-bound today: m2 boardroom about 90 -> >= 160 fps, with much
  better 1% lows.

### R7. Shadows, effects and the gameplay HUD: the exact path goes idle in gameplay - 10 agent-days
- **Deliverable**:
  - Shadow volumes (188054619) as real stencil volumes plus a darken pass with the game's darkening amount. The
    G-channel counter, CT16 shuffles and T8H darken are dropped.
  - Prio 13 volumes, if identified as shadow-like.
  - Motion blur as a full-screen blend.
  - The gameplay HUD and text (prio 21) natively with the widescreen anchors.
  - The exact path is still used for menus, loading screens, Binocucom RTTs, the ep2 photo and portraits.
- **Verification**:
  - Shadows: level 2 against the exact shadow pixels (darkened set identical on >= 99%, colour +-2).
  - HUD: pixel A/B at S = 1.
  - Sweep; the census shows gameplay frames with 0 exact draws.
- **Gain**: hubs >= 280 fps; the worker, GS and frontend threads are nearly idle in gameplay (lower power, more headroom
  on weak CPUs).

### R8. Fully modern frames: free resolution, MSAA, motion vectors - 8 agent-days
- **Deliverable**:
  - Frames with no exact draws render at any resolution, defaulting to the output resolution. "Render scale" becomes a
    percentage in modern mode.
  - MSAA 2/4/8x with alpha-to-coverage for pass A cut-outs.
  - HUD after resolve.
  - Previous-frame matrices and motion vectors (the base for TAA, FSR or DLSS later).
  - Hybrid frames (menus etc.) keep the exact grid. Switching between the two is seamless.
- **Verification**: Original gates at "render scale = 3x grid" (equal to R7 within level 3); sweep of pictures at
  1440p MSAA 4x; frame-time pacing checks (p99 / median <= 1.3).
- **Gain**: >= 300 fps in hubs at native 1440p with MSAA; sharper and cheaper than 3x supersampling.

### R9. Enhanced and mods - 12 agent-days (two tasks)
- **R9a Enhanced, 7 days**: per-pixel lighting, shadow maps, SSAO, bloom, anisotropic + mipmaps, outline styles, and
  presets ("Original", "Enhanced", "Performance") in the Options menu.
- **R9b Mods, 5 days**: VFS layers (cache / built-in / `mods\<name>`), texture dump with hash names, DDS / PNG
  replacement with asynchronous streaming, glTF model replacement on the original skeleton, and the Options mod list.
- **Verification**: with Enhanced off, the output is bit-identical to Original; the cost of each switch is measured; a
  sample pack loads with no hitch (frame-time p99 unchanged).
- **Gain**: features, not speed. Enhanced High targets >= 165 fps at 1440p.

### R10. SDL3 GPU backend; Linux - 10 agent-days. R11. macOS - 8 agent-days
- **Precondition**: R7 coverage. There is no exact path off Windows: remaining exact content (menus, RTTs) must be
  native, or the frame falls back to the software GS renderer. That is acceptable only for rare frames, so the census
  must show them rare.
- **Deliverable**:
  - `r2gpu_sdl.c`; shaders compiled on the developer machine to SPIR-V / MSL and committed as generated headers, with
    a staleness gate.
  - The platform layer (window, input incl. DualSense, audio) on SDL3 for the non-Windows build.
  - Conformance scene identical on D3D11, SDL3 (D3D12 / Vulkan on Windows) and the Linux build.
- **Fallback**: a direct Vulkan 1.3 backend (Vulkan-Headers + volk) if SDL3 GPU lacks something essential.
- **Decisions**: D4 (tools) and D5 (test hardware) before starting.

**Total**: about 100 agent-days for R0-R11. **R0-R2 (20 agent-days) deliver most of the frame-rate goal.**

---

## 8. Risks and mitigations

| risk | impact | mitigation |
|---|---|---|
| Unread VU formulas (lit colour scale, fog, fade TEST variants, clip / ADC rules) | wrong colours or missing triangles | read the routines first (R0 / R3 deliverables); the level 1 oracle decides, not a guess |
| Z ties between native and exact draws (outlines, decals, shadow volumes vs scene) | flicker or missing outline pixels | move tie partners together (the outline rule in R2); shadow volumes (exact until R7) are checked against native depth by the level 2 / temporal gates from R1 on; reserve lever: the VU-exact transform pre-pass (section 4) |
| Skipped entries leave different VU1 memory / registers / GS state for later exact entries | wrong exact draws after a native run | keep header packets (only meshes dropped); state epilogue; remainder-invariance hash (level 3); the 1.1% register-dependent programs (outline 0x07F0 after 0x0738) belong to outline entries, which move as a whole in R4 |
| Mesh DMA with EE-visible effects (VIF IRQ, MARK, VU1 results read by the EE handler 0x167F78) | game logic changes | the decoder rejects any DMA with IRQ bits, MARK or unknown VIF codes -> exact |
| Mesh data modified at run time / level heap reuse | stale geometry | content hash at first sight, cache flushed at level load, debug re-hash in sweeps |
| Texture source race (EE ahead of the GS thread) | wrong texture for a frame | textures resolved on the frontend thread in stream order with `cpu_gen` |
| Coverage outside the traced scenes (water, RC chopper, hacking / turret minigames, Binocucom views) | visual bugs in rare scenes | unknown kinds default to exact; the census lists every fallback; the sweep includes those jobs |
| Alpha / destination-alpha conventions (DATE in the shadow pipeline, T8H darken) | broken shadows | R2 writes the GS alpha byte by the GS rule; level 2 bit-7 gate |
| Two renderers to maintain | slower later work | shared HLSL includes for GS formulas; one frontend; the exact path's role shrinks to menus / special scenes after R7 |
| Frame-rate regressions when a frame is mostly exact (menus) | none expected | per-run segments only; measured in the chaos bench |
| Device loss | R2 resources lost | recreate from the CPU-side caches (decoded meshes / images kept), as the exact path reloads GS memory |
| SDL3 GPU limits (no bindless, no native-handle interop) | Linux / macOS work stalls | the shared feature subset is about D3D11's; the Vulkan fallback backend is designed in |
| "Original" not faithful enough for the owner | rework | gates set with the owner after R1's pictures; the reference (exact) setting stays |

---

## 9. Owner decisions

| # | decision | recommendation | needed before |
|---|---|---|---|
| **D1** | Must "Original" be **pixel-exact**, or **visually faithful within the section 5 gates**? | visually faithful: pixel-exact is only possible through the exact path, which stays available as "Rendering: Reference" | R1 |
| **D2** | When does Modern become the default? | per milestone after the sweep and your check, from R2 on (R1 opt-in) | R2 |
| **D3** | Stop perf-plan **P4 (GS decode fast path) and P5 (VU1 pool)**: R2 removes most of their work in gameplay. P6 (in-game overhead, PGO / LTO) and P7 (Reflex) stay useful because they speed up the EE thread and presentation | yes: P4 / P5 cancelled, P6 / P7 kept | R2 |
| **D4** | Approval to **download developer-only shader tools** for the cross-platform phase: DXC + SPIRV-Cross (or SDL_shadercross), or Slang; SDL3 source (pinned); Vulkan-Headers + volk for the fallback. Nothing new on the player's PC. R0-R9 need none of them | approve at R10 | R10 |
| **D5** | **Test hardware**: a weaker PC (or the E-core proxy of section 6), a Linux install or Steam Deck, an Apple Silicon Mac with Xcode Command Line Tools | the proxy until R8; a real machine for R10 / R11 | R8 / R10 |
| **D6** | Modern mode's resolution model from R8: a free render scale (%) with MSAA instead of the integer 3x supersampling grid | yes, with the integer grid kept for Reference | R8 |
| **D7** | Enhanced direction and order: which upgrades first (lighting / shadows / AO / HD textures), and whether Enhanced may change the art style (e.g. outline width) | Original stays the default preset; start with lighting + shadows + anisotropic | R9 |
| **D8** | Mods: in scope for a public release, and the policy that packs contain only their authors' own work (the texture dump stays a local tool) | yes, as in Ship of Harkinian / OpenGOAL | R9b |
