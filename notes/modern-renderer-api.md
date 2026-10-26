# Modern renderer (R2): prior art, graphics API, architecture

Status: **design input (2026-10-26, branch next/r2-design)**. This page covers prior art, the graphics API choice and the
layer architecture. How Sly 2 itself renders (render list, meshes, materials, the VU1 programs, effects) and where the
modern renderer takes over is the other half: notes/modern-renderer-re.md (checkpoint notes/wip/r2-re.md). Checkpoint
for this page: notes/wip/r2-api.md. Read notes/native-port-principles.md first.

**R2** = the modern high-level renderer: it draws the game's own models, levels and materials with a modern GPU
pipeline. **Exact path** = today's Direct3D 11 GS renderer (runtime/gpu, notes/gpu-renderer.md). It stays as the
fallback, as the reference for "Original" mode, and as the renderer for whatever R2 does not cover yet.

---

## 0. Decisions in one screen

1. **Prior art says: replace the console's geometry pipeline with known semantics, keep a generic exact path beside it,
   and never compile shaders on first use.** The closest precedent is OpenGOAL (Jak and Daxter, PS2): one renderer per
   bucket of the game's DMA list. High-level renderers draw geometry that was extracted offline. A generic GS-packet
   renderer draws everything else into the same framebuffer. RT64 and Unleashed Recompiled add ubershaders or
   pipelines compiled at boot or load, bindless textures, and texture packs.
2. **API: our own thin C rendering layer ("r2gpu", about 15 object types). The Direct3D 11 backend comes first. An SDL3
   GPU backend serves Linux and macOS.** D3D11 shares one device, one set of render targets and one depth buffer with
   the exact path. The hybrid frame (modern draws plus exact-path draws) then costs nothing, and the toolchain stays
   as it is. SDL3 GPU gives Vulkan on Linux and Metal on macOS (plus D3D12 for testing) through a C API under the zlib
   licence. The same SDL3 gives windowing, input and DualSense support on those platforms.
3. **Fallback: a direct Vulkan 1.3 backend** under the same layer, using Vulkan-Headers and volk. Linux uses it
   natively and macOS through KosmicKrisp or MoltenVK. Take it if SDL3 GPU proves too limiting: no bindless, no
   multi-draw indirect, no native handles for upscalers.
4. **Why not D3D12, Vulkan or plume as the main API?** The frame rate R2 must win comes from *data flow*. Static
   geometry stays resident on the GPU, transforms run on the GPU, there are a few hundred draws, and the VU1, GS-decode
   and frontend CPU stages are gone. Every bit of that is expressible in D3D11: compute shaders, structured buffers,
   `DrawIndexedInstancedIndirect`, texture arrays. The perf plan measured about 4000 D3D11 draws at 0.6-1.5 ms on the
   render thread, and R2 targets fewer than 1000. An explicit API would only pay off at tens of thousands of draws, or
   for features a 2005 game does not need on day one (ray tracing, mesh shaders). It would also put the main platform
   on cross-API interop with the exact path.
5. **No new tool on the player's PC.** Our shaders are our own code. The SPIR-V and MSL blobs for the SDL3 backend are
   compiled on the developer machine and committed as generated headers. The D3D11 backend keeps compiling HLSL with
   Windows' own d3dcompiler_47 (`scripts/hlslc.c`). Developer-only tools needing the owner's OK are listed in section 4.
6. **Design in from day one:**
   - Geometry resident on the GPU, uploaded at level load.
   - Per-frame data limited to instances, transforms and bone palettes.
   - Skinning on the GPU.
   - Every pipeline created at boot from precompiled blobs, with no run-time shader generation.
   - "Original" materials (the PS2 look) and an "Enhanced" layer on the same material record.
   - Free resolution, with MSAA and motion vectors once a frame is fully modern.
   - Real high frame rates, with no interpolation (the game's dt is real time).
   - Layered asset overrides for mods and HD packs.

---

## 1. Prior art: what matters for Sly 2

### 1.1 Projects

| project | how graphics reach the GPU | backends / shaders | effects the console did with framebuffer tricks | high frame rate | mods / packs |
|---|---|---|---|---|---|
| **RT64** (N64 renderer of Zelda64Recomp; MIT) | Frames are deferred and logged, never sent to the GPU directly (an in-game debugger can inspect them). RSP vertex work runs in a GPU compute shader by HLE of the known microcodes. TMEM is decoded to RGBA by compute. Two renderers: native resolution, synced to RAM for CPU reads, and a high-resolution replay | D3D12 / Vulkan / Metal through plume. HLSL goes through DXC to DXIL (Windows) and SPIR-V. On macOS: SPIR-V -> SPIRV-Cross -> `xcrun metal` -> metallib. Ubershaders plus spec-constant variants. re-spirv (fast spec-constant folding) cut pipeline creation from 1.83 s to 0.25 s on NVIDIA and from 288 s to 0.43 s on Steam Deck | Framebuffer address tracking. A later texture read of a tracked region becomes a GPU copy, but only when the result would match a RAM read. Per-case compute shaders reinterpret formats. Upscaled framebuffers get their texture coordinates adjusted | Matches draw calls between frames and interpolates decomposed transforms. Game patches tag matrices through an extended GBI (NOOP-encoded commands) | DDS texture packs (Rice names), asynchronous streaming, bindless textures |
| **Zelda64Recomp** | RT64 | as RT64 | as RT64; "most effects modded for widescreen" | interpolation (game tick unchanged) | `.nrm` = zip + JSON manifest (+ code). A mod with `rt64.json` is a texture pack. In-game mod menu, drag-and-drop install |
| **Unleashed Recompiled** (Xbox 360) | A renderer "written from scratch to translate the game's draw calls". Hardware quirks a PC port does not need were skipped | D3D12 (SM6) / Vulkan 1.2 through plume. XenosRecomp turns the game's shaders into HLSL ahead of time. Pipelines compiled on the background loading workers while assets load, plus a fixed list at boot. Bindless, shader specialisation, parallel transfer queues | Unneeded texture copies are detected and skipped. Post effects re-done per resolution (depth-of-field taps 5x5..9x9) | Real higher tick with game patches. "Modern CPUs usually single-thread bound before the GPU" | File-level mod loader (Hedge Mod Manager). Windows + Linux; macOS was not planned at release |
| **libultraship / Ship of Harkinian** (Fast3D) | Display lists interpreted on the CPU into batched draws | Separate hand-written D3D11, OpenGL and Metal backends. **HLSL is generated per colour-combiner at run time and compiled with `D3DCompile`**. The cache is in memory only, so the first use of each combiner hitches | Framebuffers are textures. MSAA with resolve | matrix interpolation | `.o2r` (zip) / `.otr` (MPQ) archives. A patch archive "automatically replaces the data". SDL3 platform layer |
| **OpenGOAL** (Jak 1-3, PS2 decompilation; OpenGL) | The game still builds its per-bucket DMA lists. A `BucketRenderer` per bucket consumes the chain. High-level renderers (background `TFragment`, `Tie3`, `Shrub`, `Hfrag`; foreground `Merc2` skinned and `Generic2`; sprite, ocean, sky, shadow, eye) draw geometry the decompiler **extracted offline** into PC level files. `DirectRenderer` draws the remaining GS packets into the same framebuffer | OpenGL; the shaders are hand-written | Each effect is its own renderer (`EyeRenderer`, `SkyBlendGPU`/`CPU`, `Warp`, `SlowTimeEffect`, `DepthCue`, `Shadow2`). Effects that read the framebuffer get its layout from the shared render state | game made frame-rate independent | `texture_replacements/<page>/<name>.png` (an `_all` folder applies everywhere). `merc_replacements/<model>.glb` (visual only, same skeleton). Custom levels from glTF (Blender) |
| **PCSX2 hardware renderer** | GS emulation with a render-target cache and heuristics (notes/wip/d3d11-prior-art.md) | D3D11/12, Vulkan, Metal, OpenGL | Per-game CRC hacks, texture-shuffle detection, a barrier per primitive at maximum blend accuracy | none | texture replacement by hash |
| **PS2Recomp** (ran-j) | GS and VU are "basic stubs". The README says "performance is very bad for VU and GS" | - | - | - | - |

### 1.2 Lessons

1. **Replace geometry pipelines by their known semantics, not by translating microcode.** RT64 runs the RSP's
   transform and lighting in compute because a few known microcodes do all of it. OpenGOAL re-implemented each VU1
   renderer as a PC renderer fed by pre-extracted data. Sly 2 has only six real VU1 programs (notes/vu-microcode.md:
   the main renderer 376110651, the clipping renderer 188054619, the transform loop 188054331 and three small ones).
   That is the same situation, and R2 can treat each program's input format as a known API.
2. **Keep a generic exact renderer beside the high-level ones, in the same framebuffer, chosen per bucket.** OpenGOAL's
   `DirectRenderer` and RT64's native/high-resolution pair make it possible to port one draw kind at a time. Our exact
   path is far stronger than either (exact at sample 0), so R2 can start with one draw kind and grow.
3. **Build the exact renderer first, then the PC renderer.** The author of OpenGOAL's tfrag renderer first wrote "two
   different test renderers that were slow but did things exactly the same way as in the PS2 version" before tfrag3,
   and found that "you can often leave out huge chunks of code". We already have the exact renderer. It is R2's
   reference: R2's "Original" mode is diffed against it.
4. **Extract offline or at load, upload once.** tfrag3 adds "an offline preprocessing step that reads the level data and
   outputs data in a good format for PC". Transforms and clipping run on the GPU, culling runs on the CPU, and "keeping
   the number of OpenGL draw calls down is probably the best thing we can do for performance". Sly's VU1 input repeats
   91-97% byte for byte between frames (notes/perf-plan.md 1.5). Today that is wasted work; for R2 it is the
   opportunity.
5. **No shader compilation on first use, ever.**
   - libultraship generates and compiles HLSL per combiner at run time and hitches. RT64 needed ubershaders and then
     re-spirv to make specialisation fast. Unleashed compiles pipelines while assets load.
   - Our exact path already creates its 47 shader objects at start (M11). R2 keeps the rule: a closed, enumerable set
     of pipelines, compiled at build time and created at boot on worker threads.
6. **Skip hardware quirks a PC port does not need.** Unleashed does this. OpenGOAL recomputed the time-of-day palette in
   C++ instead of porting the PS2 code. Do it per effect, with the exact path as the oracle for what "the same look"
   means.
7. **Effects done with framebuffer tricks get native re-implementations, one renderer each** (OpenGOAL: eye, sky blend,
   warp, depth cue, shadow). RT64's generic tracking and reinterpretation is needed only because its port has no
   knowledge of the game. Ours can hook the game's own effect functions, as the motion-blur switch already does
   (patch `motion_blur_off`). An effect R2 has not re-implemented runs on the exact path inside the R2 frame.
8. **Interpolation is not needed here.** RT64 and Ship of Harkinian interpolate because their game logic is tied to
   20/30 Hz. Sly 2 steps its world by real-time dt (notes/frame-rate.md), so a higher frame rate is a real higher tick,
   as in Unleashed. pc-fps2 measured interpolation as a loss here. The RC chopper stays at 59.94 (frame_rate.c
   `keeps_60`).
9. **Mods are layers of archives, keyed by stable ids.** libultraship's patch `.o2r`, the zip + manifest `.nrm` of
   Zelda64Recomp, and OpenGOAL's per-name PNG/GLB folders are all the same idea. Texture packs are keyed by content
   hash (RT64/Rice, PCSX2) or by name (OpenGOAL). We can have both: a hash of the decoded texture, and asset names
   once r2-re maps the game's files.
10. **Cross-platform: pick a layer early and keep only what all backends share.**
    - Fast3D maintains three hand-written backends.
    - plume exists because RT64 and Unleashed wanted one renderer for D3D12, Vulkan and Metal. It is still described
      as "not production-ready / no stable API".
    - Unleashed shipped without macOS.
    - OpenGOAL's OpenGL is the portable route, but Apple froze OpenGL at 4.1.

---

## 2. Graphics API options

### 2.1 Facts checked on this machine (2026-10-26)

- **d3dcompiler_47** (Windows' own, 10.0.26100) compiles `ps_5_1` with register spaces and unbounded descriptor arrays
  (`D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES`): an 876-byte DXBC from `Texture2D tex[] : register(t0, space1)`
  indexed by `NonUniformResourceIndex`. It rejects `ps_6_0` ("unrecognized compiler target"). D3D12 accepts DXBC SM5.1
  (dxilconv.dll is part of Windows). So **D3D12 with descriptor-table bindless needs no new compiler. SM6 features
  (wave ops, mesh shaders, DXR, `ResourceDescriptorHeap`) need DXC.**
- **llvm-mingw clang 23.1.3** has only the x86-64 and AArch64 targets, with no DirectX or SPIR-V backend
  (`clang -print-targets`; `--driver-mode=dxc` fails). **Nothing in the toolchain makes SPIR-V or MSL today.**
- **llvm-mingw headers**: `d3d12.h`, `d3d12shader.h`, `dxgi1_6.h`, `d3d11_4.h`, `dxcapi.h` are present, with C
  interfaces. Missing: `d3dx12.h` (C++ helpers, not needed) and `vulkan/` (Vulkan-Headers would be needed).
- **System32**: `D3D12Core.dll`, `d3d11on12.dll`, `vulkan-1.dll` (the Vulkan loader ships with the GPU driver).

### 2.2 Comparison

MT = multithreaded command recording; MDI = multi-draw indirect. "Hybrid" = how R2 draws would share the colour and
depth targets of the D3D11 exact path.

| option | platforms | MT | bindless | indirect / GPU-driven | mesh / RT | shaders (toolchain today?) | hybrid with exact D3D11 path | C + llvm-mingw fit | licence | effort |
|---|---|---|---|---|---|---|---|---|---|---|
| **D3D11** (today) | Windows 10+ (x64, ARM64) | deferred contexts, real only on NVIDIA; effectively one submit thread | no (128 SRV slots; texture arrays) | `DrawIndexedInstancedIndirect`, compute-written args; no MDI (NVAPI extension on NVIDIA) | no | HLSL SM5.0 DXBC via inbox d3dcompiler_47: **have it** | **same device: free** | C COM interface, used today | OS | **lowest**: device, presenter, pacing, governor exist |
| **D3D12** | Windows 10+, Xbox | yes | yes (descriptor heaps; SM5.1 arrays work with d3dcompiler_47) | ExecuteIndirect with count | SM6.5 / DXR (need DXC) | SM5.1 DXBC: **have it**; SM6: DXC | D3D11On12 (inbox) or shared NT handles + shared fences; the exact path's early-depth, ROV and timing measurements would need re-validation | C interface in the headers; d3dx12 helpers are C++ | OS | high (memory, barriers, descriptors) |
| **Vulkan 1.3** | Windows, Linux, Android; macOS via KosmicKrisp (Vulkan 1.3 conformant on Apple Silicon, macOS 15+; 1.4 reported) or MoltenVK | yes | descriptor indexing (core 1.2) | `vkCmdDrawIndexedIndirectCount` (core 1.2) | `VK_EXT_mesh_shader`, KHR ray tracing | SPIR-V: **needs DXC / glslang / Slang** | `VK_KHR_external_memory_win32` + D3D11 shared handles + keyed mutex or shared fence per handoff: works, fiddly | native C API; needs Vulkan-Headers (+ volk) | Apache-2.0 / MIT headers | high (as D3D12) |
| **Metal** | macOS, iOS | yes (parallel encoders) | argument buffers | indirect command buffers | Metal 3 mesh, RT | MSL (SPIRV-Cross / Slang; the Metal framework compiles MSL source at run time) | n/a (no D3D) | Objective-C / metal-cpp, built with Apple's clang on a Mac only | OS | high, Mac only |
| **WebGPU** (Dawn / wgpu-native) | Win, Linux, macOS, Android, web | encoders on threads | not core (binding arrays are native extensions, in flux) | drawIndirect core; MDI only as a Dawn extension | no | WGSL (Tint / Naga); Dawn also takes SPIR-V | native interop extensions only | `webgpu.h` C API: good; Dawn is a large Chromium-style C++ build, wgpu-native a Rust binary | BSD-3 / MIT-Apache | medium; validation overhead, moving API |
| **SDL3 GPU** | D3D12, Vulkan, Metal: Windows 10+, Linux, macOS 10.14+, iOS, Xbox, Switch. **No D3D11 backend** | multiple command buffers recorded on threads | no | draw / dispatch indirect; no MDI | "not planned" | DXBC SM5.1 (d3dcompiler_47 works) or DXIL for D3D12, SPIR-V, MSL / metallib; SDL_shadercross (DXC + SPIRV-Cross) | **no documented native-handle or external-texture interop**: cannot share the exact path's targets | C API; builds with MinGW; also windowing, input (DualSense), audio | zlib | low-medium |
| **bgfx** | D3D11/12, Vulkan, Metal, GL, WebGPU; very wide | encoders per thread | no | yes | no | its own GLSL-like language + shaderc (our HLSL would have to be ported) | can adopt an existing D3D11 device | C99 API over a C++ library | BSD-2 | medium; its view/sort model fights a game-ordered hybrid stream |
| **Diligent** | D3D11/12, Vulkan, GL, WebGPU; Metal reportedly commercial-only | yes | yes | yes | yes | HLSL, converted at run time for GL/Vulkan | can attach to an existing D3D11 device | C interface over a large C++ codebase | Apache-2.0 | medium; heavy dependency |
| **NVRHI** (NVIDIA) | D3D11/12, Vulkan 1.3; Windows + Linux, x64 + ARM64; **no Metal** | yes | yes | yes | meshlets, RT | via ShaderMake (DXC / FXC / Slang) | the app creates the native device: can share ours | C++17 only | MIT | medium |
| **plume** (RT64, Unleashed) | D3D12, Vulkan, Metal | yes | yes (RT64 uses bindless) | yes | DXR in RT64 builds [unverified] | bring your own (RT64: DXC + SPIRV-Cross + xcrun) | no D3D11; exact path via D3D11On12 at best | C++17; "not production-ready / no stable API" | MIT | medium; proven by three shipped ports |
| **own thin C layer (r2gpu)** over D3D11 + SDL3 GPU (+ optional Vulkan) | union of the backends | per backend (data preparation is parallel anyway, 3.3) | no in the shared subset (texture arrays) | instancing + indirect | no (Vulkan backend later) | HLSL with register macros: d3dcompiler_47 for D3D11; committed SPIR-V / MSL for SDL3 | **free on Windows** (D3D11) | ours, C | ours | low-medium; we own the subset |

### 2.3 Recommendation

**Primary: r2gpu, a thin C layer tailored to R2.** Backends:

1. **Direct3D 11 first (Windows, the default).**
   - R2 creates its resources on the exact path's `ID3D11Device`, and its commands execute on the same render thread
     and immediate context, in stream order with the exact path's commands. The hybrid frame needs no copies, no
     cross-API fences and no second presenter.
   - The existing presenter, pacing, VRR handling, internal-scale governor, settings and fallback logic are reused.
   - No new tools: HLSL SM5.0 via `scripts/hlslc.c` and d3dcompiler_47, as today.
2. **SDL3 GPU second (Linux via Vulkan, macOS via Metal, ARM64; D3D12 on Windows as a test of the layer).**
   - It is a C API with no C++ dependency, and SDL maintains the three explicit backends.
   - The platform layer those ports need anyway comes with it: window, gamepads incl. DualSense, audio device.
   - The shared feature subset is about what D3D11 offers (no bindless, no MDI), so the SDL backend costs R2 nothing.
   - On these platforms there is no exact path: R2 must cover the whole frame there. The software GS renderer is the
     whole-frame fallback.
3. **Fallback: a direct Vulkan 1.3 backend** (Vulkan-Headers + volk, + VMA if allocation gets complex). It replaces
   SDL3 GPU on Linux, and on macOS through KosmicKrisp / MoltenVK, if SDL3 GPU proves too limiting:
   - descriptor indexing for large HD texture sets,
   - `DrawIndexedIndirectCount`,
   - native handles for FSR / XeSS / DLSS,
   - ray tracing for an "Enhanced+" tier.

   On Windows it could also run R2-only frames when the exact path is not needed.

Not chosen:
- **D3D12 as the main API**: Windows-only, interop with the exact path, no frame-rate gain at R2's draw counts. It stays
  reachable through SDL3 GPU's backend, or a D3D12 r2gpu backend later, if a feature such as DLSS frame generation
  (D3D12-only) is wanted.
- **plume**: C++, unstable API, no D3D11.
- **bgfx**: its own shader language and view model.
- **Diligent**: size, and Metal is commercial.
- **NVRHI**: no Metal.
- **WebGPU**: young native extensions, heavy build.

**Rules that keep r2gpu thin:**
- C99 header, opaque handles.
- Everything a pipeline needs is baked at creation: shaders, input layout, blend, depth, raster, target formats.
- Explicit passes (targets and load/store actions), explicit uploads (per-frame ring buffers + `Map NO_OVERWRITE` on
  D3D11).
- Compute + indirect draws.
- No D3D11 or SDL types above the layer.
- A conformance scene in `tests/unit/` renders the same images on every backend (WARP for D3D11 in CI-like runs).

---

## 3. Modern techniques to design in from day one

### 3.1 GPU-resident data and GPU-driven drawing

- **Static level geometry**: extracted at level load (section 5, layer 1) into one vertex/index arena per level. Mesh
  records give offset, count, bounds, material. Uploaded once, during the game's own loading screen.
- **Per frame only**: an instance table (mesh id, world matrix, previous-frame matrix for motion vectors, material
  overrides, light set), bone palettes, lights, fog, camera. Written by worker threads into a mapped ring and uploaded
  in one go.
- **Few draws**:
  - Sort instances by pipeline. One instanced draw per (pipeline, mesh) on D3D11.
  - When counts grow: "cluster instancing". Meshes are split into fixed-size triangle clusters, the vertex shader
    pulls vertices from structured buffers by `SV_InstanceID`, and a compute pass culls clusters and writes
    `DrawIndexedInstancedIndirect` args. That is one draw per pipeline, and it works on D3D11 without MDI.
  - Vulkan / D3D12 backends would use real MDI-with-count.
- **Culling**: frustum culling on the CPU (cheap at Sly's object counts), the game's own visibility data if r2-re finds
  it, GPU cluster culling later.
- **Skinning on the GPU**: the bone palette goes into a structured buffer (OpenGOAL Merc2: 7 vec4 per bone, batched per
  level). Skinning runs in the vertex shader, or in a compute pre-pass writing skinned vertices when several passes
  reuse them (shadow maps, motion vectors).
- **Translucency keeps the game's order**: alpha-blended draws are submitted in the order the game's render list gives
  them. GS-style blending is order-dependent; Sly sorts its own lists. Only opaque and alpha-tested work is reordered by
  pipeline.

### 3.2 Pipelines without stutter

- The PS2 material state space is small and closed:
  - GS blend (A, B, C, D selectors, FIX) and alpha test (ATST/AREF/AFAIL),
  - Z test / write, texture function (TFX/TCC), fog, CLAMP, filtering, CLUT or direct texture.
- "Original" shaders are specialised over a few bits (textured, TFX, fog, alpha-test class). Blend and depth stay
  D3D11 state objects, and anything rare uses a uniform-driven ubershader branch (Dolphin's hybrid lesson).
- "Enhanced" adds a fixed list of shaders. The whole set is enumerated at build time (`perms.txt`, as the exact path
  does) and created at boot on worker threads, like M11's warm-up and Unleashed's boot list.
- Never generate shader text at run time (the libultraship lesson).
- Vulkan backend: `VkPipelineCache` saved under `%LOCALAPPDATA%\Sly2` (and the platform equivalents), all pipelines
  created at boot.

### 3.3 Threads and frame rate

- The modern frame removes the worker (VIF/VU1), GS decode and frontend stages for everything R2 draws. Each of those
  costs 5-9 ms per frame today (perf-plan 1.1). What is left:
  - the EE game code (about 2 ms),
  - scene capture (hooks copying instance data, well under 1 ms),
  - the render thread (a few hundred draws, about 0.2-0.5 ms on D3D11),
  - the GPU.
- At 1440p with "Original" materials the GPU work is small. **300+ fps is a reasonable target for fully modern
  frames.** Hybrid frames are limited by the exact-path share they keep.
- Threads: EE -> scene packet ring (double-buffered, so EE frame n+1 overlaps render frame n) -> render thread. Worker
  jobs handle instance packing, bone palettes, culling, texture decode and streaming.
- Multithreaded command *recording* is not needed at this scale. Multithreaded *data preparation* is, and it works on
  every backend.
- **No interpolation**: real ticks (1.2 lesson 8).
- Keep the existing pacing, VRR, Reflex plans and frame limiter. They sit after R2.

### 3.4 Resolution independence and anti-aliasing

- **Fully modern frames**:
  - Any render resolution (percentage of the output), not the exact path's integer grid.
  - MSAA 2/4/8x (cheap for 2005 geometry) with alpha-to-coverage for alpha-tested foliage and fences (Unleashed does
    the same).
  - Optional SMAA / FXAA; supersampling through render scale.
  - Motion vectors from current and previous matrices. That opens TAA and FSR (FSR 2's HLSL compute source can be
    compiled as our own shaders on every backend), and DLSS SR on D3D11 / Vulkan. The exact path cannot offer any of
    these (it has no motion vectors).
- **Hybrid frames**: R2 renders into the exact path's targets, at its integer scale and with its sample convention
  (sample 0 = the native pixel), so later exact-path draws, downloads and the verifier still see one consistent target.
  Free resolution and MSAA arrive when a frame has no exact-path draws that read or test against R2's pixels (the
  HUD drawn on top after resolve is fine).
- **HUD and 2D**: drawn after resolve/upscale at output resolution. Widescreen HUD placement uses the existing hint
  mechanism (register 0x7F, notes/widescreen.md).

### 3.5 Materials: "Original" and "Enhanced" on one record

- **Material record** = the game's own parameters, read from its data:
  - texture + CLUT,
  - TFX / TCC,
  - vertex-colour scale (0x80 = 1.0, x2 modulate),
  - alpha test, blend selectors, fog,
  - LOD (K, L), filtering,
  - the light set / lighting model the VU1 program would have applied.

  On top of that sits an optional **Enhanced layer**: normal map, roughness / specular, emissive, an HD albedo
  override, a shadow-casting flag, an outline style.
- **"Original"**: reproduce the GS arithmetic in float (blend in 0-128 alpha units, `(A-B)*C>>7+D` with clamp/wrap
  per COLCLAMP, PS2 LOD formula, CLUT decoded at load, vertex lighting the way the VU1 program computes it).
  - Measured against the exact path with an image diff per corpus scene; the 10.2-style tolerance gates are reused.
  - It is "looks the same", not bit-exact. The exact path remains the bit-exact option.
- **"Enhanced"**, each switch on its own like the current filter toggles, with presets that extend today's
  Modern / Faithful:
  - per-pixel lighting from the same lights, normal maps,
  - shadow maps replacing or augmenting the game's projected and G-channel shadows,
  - SSAO, bloom, HD texture and model replacements,
  - outline styles (PS2-width, thin, inverted-hull or screen-space edges from depth / normals / ids).
- **Effects** the PS2 did with framebuffer tricks are listed by r2-re. Each gets either a native R2 pass (the default
  plan, one small module per effect as in OpenGOAL) or an exact-path segment inside the R2 frame until it is ported:
  - G-channel shadow counter, CT16 channel shuffles, T8H darken,
  - motion blur feedback, Binocucom render-to-texture, cel outlines, Z-region aliasing.

### 3.6 Asset override layers (mods, HD packs)

- **Virtual file system with ordered layers**:
  1. **base**: R2's cache extracted from the player's own disc, under `%LOCALAPPDATA%\Sly2\cache\r2`, keyed by disc
     hash + extractor version. Never distributed, like `dist/`.
  2. **built-in fixes**: our own patch data, if any.
  3. **user packs**: `mods\<name>.zip` (or a folder) with a `manifest.json` holding id, version, priority, targets,
     requires. Enabled and disabled in the Options menu.
- **Keys**:
  - Textures: by content hash of the *decoded* PS2 texture + CLUT (stable across levels, the Rice / RT64 / PCSX2
    convention), plus by asset name once r2-re maps the game's files (OpenGOAL style; names are friendlier for
    modders).
  - Models: by mesh / asset id.
- **Formats**:
  - DDS (BC7 albedo, BC5 normal maps, BC4 masks) loaded directly.
  - PNG accepted, converted once into the cache.
  - glTF 2.0 `.glb` for model replacements (OpenGOAL precedent), on the original skeleton.
- **Streaming**: HD textures load asynchronously on worker threads. The original texture is used until the
  replacement is resident, so there is never a hitch.
- **Tools**: an in-game "dump textures" switch writes the player's own textures with their hash names (for pack
  makers). Packs contain only their authors' work.

---

## 4. Tools that would need the owner's approval (developer machine only)

None of these are needed by the player's build. Shader binaries for non-D3D11 backends are generated on the developer
machine and committed (they are our code, not game data). A gate checks they are up to date: a hash of the HLSL
sources recorded in the generated header.

| tool | for | licence | notes |
|---|---|---|---|
| **DXC** (DirectXShaderCompiler, latest v1.9.2609, Sept 2026) | HLSL -> SPIR-V (SDL3 Vulkan, direct Vulkan); DXIL if a D3D12 SM6 feature is ever wanted | LLVM / NCSA | prebuilt Windows zip from GitHub releases |
| **SPIRV-Cross** | SPIR-V -> MSL for Metal (RT64's route) | Apache-2.0 | small C++ build or prebuilt; **or** use **SDL_shadercross** (zlib), which wraps DXC + SPIRV-Cross with SDL's binding conventions |
| *alternative to both*: **Slang** (v2026.20) | one compiler: HLSL-like source -> SPIR-V, MSL (experimental), HLSL, WGSL | Apache-2.0 with LLVM exception | prebuilt for Windows / Linux / macOS x64 + arm64; DXIL output still needs DXC |
| **SDL3** (source, pinned release) | the SDL3 GPU backend + platform layer on Linux / macOS (optional on Windows for DualSense) | zlib | built by our build on those platforms (or the system package); becomes a pinned download of the non-Windows installer |
| Vulkan-Headers + volk (+ VMA) | only for the fallback Vulkan backend | Apache-2.0 / MIT | headers / single files; Vulkan validation layers (Vulkan SDK) recommended for development |
| Xcode Command Line Tools (on a Mac) | building the macOS port at all (Apple clang + SDK) | Apple | MSL can be compiled at run time by Metal, so the full Xcode `metal` tool is optional |

The D3D11 backend (the first milestones) needs **none** of these.

---

## 5. Architecture sketch

```
 Layer 0  GAME           recompiled EE code + disc data (player's own image)
                         |  hooks at the game's render-list / loader functions (principle 4: library-level replacement)
                         v
 Layer 1  EXTRACTION     (a) load time: level / model / texture / CLUT / skeleton data -> GPU-ready resources,
          ("r2x")            cached per disc hash; HD / mod overrides resolved here (VFS layers, 3.6)
                         (b) per frame: scene capture -> scene packet (instances, matrices, bone palettes,
                             material refs, lights, fog, camera, effect requests), in game order
                         (c) not captured -> the game's DMA as today: VIF1 -> VU1 -> GIF -> GS -> exact path
                             (decided per draw kind / bucket, like OpenGOAL's BucketRenderer; switchable)
                         v
 Layer 2  SCENE          API-independent: mesh arenas, texture pool (arrays by size / format), material table
          ("r2scene")    (Original + Enhanced), instance ring (double-buffered EE -> render), skeletons, lights,
                         ordered phases: world opaque / alpha-tested / translucent (game order) / effects / HUD
                         v
 Layer 3  RENDERER       frame graph: culling -> (Enhanced: shadow maps, SSAO) -> main pass (Original | Enhanced)
          ("r2render")   -> translucent pass in game order -> effect passes (native re-implementations)
                         -> exact-path segments (hybrid) -> post (AA, bloom, upscale) -> HUD -> existing presenter
                         v
 Layer 4  RHI            r2gpu.h (C): buffers, textures, samplers, pipelines (baked, created at boot), passes,
                         command lists, compute, indirect, fences
                         v
 Layer 5  BACKENDS       D3D11 (Windows; shares ID3D11Device, targets and command stream with runtime/gpu)
                         SDL3 GPU (Linux: Vulkan, macOS: Metal, Windows: D3D12 for tests)
                         [fallback] Vulkan 1.3 direct
```

### 5.1 The hybrid frame (Windows, D3D11)

- **One command stream.** When the EE-side capture takes over a draw kind, it suppresses that part of the DMA (no VIF
  unpack, no VU1, no GS packets for it). It emits an `R2_SEGMENT` marker into the GS queue at the position the
  suppressed packets would have had. The frontend forwards the marker into its command chunks. The render thread
  executes R2 segments and exact draws in submission order on one immediate context. This ordering is what
  translucency, shadows and the game's own effects rely on.
- **Shared targets.** R2 draws into the exact path's target for the current GS FRAME/ZBUF (looked up in the frontend's
  target table at the marker), with the same integer scale and sample convention. The exact path's ownership table
  marks the covered pages GPU-owned and valid. Its later reads (feedback copies, CLUT-from-target, downloads, the
  verifier) then see R2's pixels as if the GS had drawn them.
- **Shared depth.** The exact path stores `z / 2^20` of the GS's integer Z (P1). R2 computes the same quantity from
  the game's projection (the VU1 program's Z formula, which r2-re documents) and writes it as `SV_Position.z`. GS Z and
  D3D depth both interpolate linearly in screen space, so later exact-path depth tests against R2 geometry agree up to
  float rounding.
  - Exact ties are not reproduced. Effects that depend on ties (the PS2 outlines' `ZTST GREATER`) are drawn by R2
    itself or kept on the exact path *together with* the geometry they tie against.
- **What stays exact for a long time**: HUD and 2D, FMV, loading screens, menus. They are cheap, and the frontend's cost
  scales with what it receives.
- **Verification**: an `R2=off|hybrid|full` switch.
  - Corpus scenes are rendered both ways: exact vs hybrid, with image metrics against the exact path, and stream
    hashes for the parts left exact.
  - In-game: F9-style toggling, as the GPU <-> software switch has.

### 5.2 Other platforms

No D3D11, so no exact path. R2 renders whole frames there (free resolution, MSAA). Anything not yet covered falls back
to the software GS renderer for the whole frame. Linux and macOS therefore come after R2 covers the full game on
Windows. The runtime's other Windows dependencies are separate port work (window.c, WASAPI, XInput, file paths,
threads); SDL3 covers most of them.

### 5.3 Order of work (for the synthesis; capture points come from r2-re)

| step | content | done when |
|---|---|---|
| R0 | r2gpu.h + D3D11 backend + conformance scene; pipeline warm-up at boot | the scene renders identically on hardware and WARP |
| R1 | extraction + capture of static level geometry in one hub, Original materials, hybrid segments | the hub renders hybrid; image diff vs the exact path within the gates; fps measured |
| R2 | skinned characters (GPU skinning), lighting as the VU1 program computes it | characters hybrid; diff gates |
| R3 | translucency in game order, the main effects (native or exact segments) | all 8 episodes hybrid |
| R4 | fully modern frames: free resolution, MSAA, motion vectors, HUD after resolve | frames without exact draws except HUD; 300+ fps target scenes |
| R5 | Enhanced layer: lighting, shadow maps, normal maps, outline styles; presets | each switch measurable, Original unchanged |
| R6 | VFS and mods: texture dump, hash / name keys, DDS / PNG / glb, Options menu | a sample pack loads without hitches |
| R7 | SDL3 GPU backend (approved tools, committed blobs) on Windows D3D12 for tests, then Linux | conformance scene identical; a hub on Linux |
| R8 | macOS (Metal via SDL3) | on a Mac |

---

## 6. Sources (fetched 2026-10-26)

- RT64: https://github.com/rt64/rt64 , its build: https://raw.githubusercontent.com/rt64/rt64/main/CMakeLists.txt ,
  re-spirv: https://github.com/rt64/re-spirv , plume: https://github.com/renderbag/plume
- Unleashed Recompiled: https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md
- Zelda64Recomp: https://github.com/Zelda64Recomp/Zelda64Recomp ; mods: https://hackmd.io/@Wiseguy/HyhUXwiV0 ,
  https://www.gamingonlinux.com/2025/05/zelda-64-recompiled-majoras-mask-adds-modding-support-texture-pack-support-optimizations-and-more/page=1/
- libultraship: https://github.com/Kenix3/libultraship , Fast3D D3D11 backend:
  https://raw.githubusercontent.com/Kenix3/libultraship/main/src/fast/backends/gfx_direct3d11.cpp , backend list
  https://github.com/Kenix3/libultraship/tree/main/src/fast/backends
- OpenGOAL: tfrag porting notes https://opengoal.dev/docs/porting-info/drawable_and_tfrag/porting_tfrag ; renderer
  tree https://github.com/open-goal/jak-project/tree/master/game/graphics/opengl_renderer (BucketRenderer.h,
  foreground/Merc2.h); texture replacement https://opengoal.dev/docs/usage/texture_replacement ; progress report
  https://opengoal.dev/blog/progress-report-july-2024 ; custom levels https://opengoal.dev/docs/developing/custom_levels/intro
- PS2Recomp: https://github.com/ran-j/PS2Recomp ; PCSX2 / paraLLEl-GS / Xenia / Dolphin: notes/wip/d3d11-prior-art.md,
  notes/wip/pc-research-sources.md
- SDL3 GPU: https://wiki.libsdl.org/SDL3/CategoryGPU , https://wiki.libsdl.org/SDL3/SDL_GPUShaderFormat ,
  https://wiki.libsdl.org/SDL3/SDL_CreateGPUShader ; interop question:
  https://discourse.libsdl.org/t/sdl3-gpu-api-interoperability/55204
- NVRHI: https://github.com/NVIDIA-RTX/NVRHI ; bgfx: https://github.com/bkaradzic/bgfx ; Diligent:
  https://github.com/DiligentGraphics/DiligentEngine (Metal licensing per https://gitee.com/panxine/DiligentCore mirror
  README; to confirm) ; Slang: https://github.com/shader-slang/slang ; DXC:
  https://github.com/microsoft/DirectXShaderCompiler/releases/latest
- Dawn multi-draw indirect: https://dawn.googlesource.com/dawn/+/cc32bc406cca3397cbcdcd29b8ea8e4a93c995ad/docs/dawn/features/multi_draw_indirect.md
- KosmicKrisp: https://www.lunarg.com/lunarg-achieves-vulkan-1-3-conformance-with-kosmickrisp-on-apple-silicon/ ,
  https://www.phoronix.com/news/KosmicKrisp-Vulkan-1.4-2026
- Fragment shader interlock on AMD: https://www.phoronix.com/news/RADV-Adds-Frag-Shader-Interlock
- D3D11 vs D3D12 draw overhead (3DMark API overhead): notes/wip/pc-research-sources.md

Marked [unverified]: plume / RT64 ray-tracing build details. Not checked: SDL3 GPU internals beyond the wiki (a native
interop property may exist undocumented: check `SDL_gpu.h` before R7), and wgpu-native's binding-array status.
