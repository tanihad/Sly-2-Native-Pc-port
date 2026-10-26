# WIP: r2-api (modern high-level renderer: prior art + graphics API)

**Goal**: prior art of high-level renderers for recompiled/ported console games, a graphics API recommendation and an
architecture sketch for the Sly 2 modern renderer -> notes/modern-renderer-api.md. (RE of Sly's own rendering is
notes/wip/r2-re.md, another agent.)

## Done
- Read: native-port-principles, gpu-renderer, d3d11-renderer 0-2 + 13, perf-plan, pc-research-sources, frame-rate,
  install, agent-checkpoints, wip/d3d11-prior-art (RT64 / Unleashed sections).

- Prior-art research done (findings below): RT64, re-spirv, plume, Unleashed, libultraship Fast3D, Zelda64Recomp mods,
  PS2Recomp, OpenGOAL (the key PS2 precedent).

- API research done (SDL3 GPU, plume, NVRHI, bgfx, Diligent, WebGPU, KosmicKrisp, Slang, DXC); probes below.
- Decision: own thin C RHI; D3D11 backend first (same device as the exact path), SDL3 GPU backend for Linux/macOS;
  fallback: direct Vulkan backend (Vulkan-Headers + volk). Dev-side shader tools: DXC + SPIRV-Cross (SDL_shadercross)
  or Slang; blobs committed so player builds need nothing new.

- notes/modern-renderer-api.md written (prior art, API table, recommendation, tools for approval, architecture,
  hybrid contract, R0-R8 order).

**Status: complete** (2026-10-26).

## Next
- (for the synthesis) reconcile with notes/modern-renderer-re.md capture points; owner approval of the dev-side tools
  (DXC + SPIRV-Cross / SDL_shadercross or Slang; SDL3) before R7.

## Findings / traps
- git is not on PATH in the worktree: `. C:\Users\tanih\Projects\sly2-decomp\scripts\env.ps1` (worktree has no tools/).
- Context facts: game dt is real time (clamp 1/60 lifted by frame_rate patch) -> high fps = real ticks, no interpolation
  needed; CPU stages (worker/VU1, GS decode, frontend) 5-9 ms each limit fps; perf-plan option I = HLE renderer.
- RT64 (MIT): frames deferred/logged; RSP transforms in compute; TMEM decoded in compute; dual renderer (native for RAM
  sync + hi-res replay); ubershaders + spec-constant variants; shaders HLSL -> DXC -> DXIL (Win) / SPIR-V (always) /
  SPIR-V -> spirv-cross -> xcrun metal -> metallib (Mac); re-spirv (fast spec-constant folding) cut pipeline creation
  1.83 s -> 0.25 s (NVIDIA), 288 s -> 0.43 s (Steam Deck). DDS texture packs, async streaming, bindless. Win/Linux/Mac.
- plume (MIT, C++): thin LCD RHI over D3D12/Vulkan/Metal, "not production-ready / no stable API", bring your own shader
  compiler. Used by RT64 and Unleashed.
- Unleashed: renderer from scratch translating draw calls; D3D12 SM6 or Vulkan 1.2; XenosRecomp shaders -> HLSL AOT;
  pipelines compiled on background loader workers during asset load + fixed boot list; bindless; parallel transfer
  queues; skipped hardware quirks; MSAA > 2x, A2C, reverse Z; mod loader (file-level, Hedge Mod Manager); Win + Linux.
- libultraship Fast3D: D3D11 / OpenGL / Metal backends; HLSL generated per colour-combiner at RUN time + D3DCompile,
  memory-only cache (first-use hitches); one Draw per batch; MSAA; .o2r (zip) / .otr (MPQ) archives, patch archives
  replace data automatically; SDL3.
- Zelda64Recomp mods: .nrm = zip + JSON manifest (+ code); texture packs = mods with rt64.json; in-game mod menu.
- PS2Recomp: GS/VU only stubs ("performance is very bad for VU and GS") - no renderer precedent.
- OpenGOAL (Jak PS2 decomp, OpenGL): game still builds DMA per bucket; BucketRenderer per bucket consumes the chain;
  high-level renderers (TFragment/Tie3/Shrub/Hfrag background, Merc2 skinned foreground, Generic2, sprite, ocean, sky,
  shadow, eye) use geometry pre-extracted OFFLINE by the decompiler into fr3 level files; DirectRenderer (GS-packet path)
  handles the rest in the same framebuffer; per-bucket switchable; tfrag lesson: first slow exact renderers, then PC
  version; transforms/clipping on GPU, culling on CPU, draw count is the cost; Merc2 bones 7 vec4 each into a buffer,
  draws batched per level; texture_replacements/<page>/<tex>.png, merc_replacements/<model>.glb, custom levels (glb).
- PROBED here (2026-10-26): inbox d3dcompiler_47 10.0.26100 compiles ps_5_1 with register spaces and unbounded
  descriptor arrays (flag 1<<20 = D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES), 876-byte DXBC; ps_6_0 "unrecognized
  target". D3D12 accepts DXBC SM5.1 (dxilconv.dll is inbox). llvm-mingw clang 23.1.3 targets: x86-64 + AArch64 only, no
  DirectX/SPIR-V backends -> no HLSL->SPIR-V today. llvm-mingw include/ has d3d12.h, d3d12shader.h, dxgi1_6.h,
  d3d11_4.h, dxcapi.h; no vulkan/, no d3dx12.h. System32: D3D12Core.dll, d3d11on12.dll, vulkan-1.dll.
- SDL3 GPU: Vulkan / D3D12 (FL 11_0 + binding tier 2) / Metal; NO D3D11 backend; formats SPIRV, DXBC SM5_1 (D3D12),
  DXIL SM6_0, MSL, METALLIB; HLSL register spaces 0-3 per stage; MT command buffers; indirect yes; no bindless/mesh/RT;
  no documented native-handle / external-texture interop (forum question unanswered) -> cannot share D3D11 targets.
- NVRHI: D3D11/12/Vulkan, Win/Linux x64+ARM64, C++17, MIT, no Metal. bgfx: BSD-2, many backends incl. Metal, own shader
  language. Diligent: Apache-2.0, Metal backend reportedly commercial-only. KosmicKrisp (LunarG, MIT, Mesa): Vulkan 1.3
  conformant on Apple Silicon macOS 15+, 1.4 reported 2026. RADV has fragment shader interlock; AMD's own Vulkan driver
  long lacked it. DXC latest v1.9.2609 (2026-09); Slang v2026.20 (Apache-2.0+LLVM exc., prebuilt Win/Linux/Mac x64+arm64).
