# WIP: d3d11 prior-art research (role: prior-art)

Task: research (web only) how other projects map the PS2 GS / console GPUs to modern GPU APIs;
dense sourced summary + lessons constraining our D3D11 design.

Status: 2026-10-11, pass 2 (first run was paused before any findings). Sections filled as research
progresses; final summary goes at the end.

## Plan
1. [x] paraLLEl-GS (Themaister blog) — README fetch 404'd, blog post read in full
2. [~] PCSX2 GS HW renderer (texture cache, RT cache, upscaling fixes, blending, DATE, Z, etc.)
3. [x] RT64 / Zelda64Recomp / Unleashed Recompiled renderer (READMEs)
4. [ ] Play! GS path (README has nothing; need GSH_Vulkan source)
5. [~] Sly 2/3 specifics: PCSX2 GameDB, wiki (403 via fetch; have search excerpts), widescreen pnach
6. [x] D3D11 capabilities: ROV (D3D11.3), typed UAV loads, SM5 limits (ROV + typed loads docs read)

## Findings

### paraLLEl-GS (Themaister, 2024-07-03)
Source: https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation
- Whole GS as Vulkan compute; VRAM is a 4 MiB SSBO. Hazard tracking: 8 KiB pages (pending FB writes /
  reads), plus 256-byte block masks (32 blocks/page in a u32) for copies, CLUT/image reads, clobbers.
  Separate FB-write vs texture-read masks so "render to upper 8 bits of 24-bit colour while sampling
  the lower bits" (the 8H/4HL/4HH palette-in-alpha trick) is not a false hazard. Timeline semaphore
  value per page for host readback.
- Hazard resolution by batching: copy-after-copy = barrier; copy over a sampled texture = flush copies
  + unswizzles but primitive batch continues; sampling/copying over an RT = full batch break.
  Power-of-two "red zone" false positives deliberately ignored. One case went from 500+ render-pass
  barriers to 18 after feedback analysis.
- Rasterizer: tile-based (32x32), u16 primitive index lists, <= 64k prims/pass; barycentric/Pineda
  edge functions in 32-bit int with 8 sub-pixel bits (GS needs 33 bits -> inputs downsampled
  slightly, error terms added back); custom fixed-point reciprocal (~24-bit) for reproducibility;
  D3D9-style top-left rule with pixel centres on integer coords; parallelograms for wide lines and
  sprites; soft-floor UV snap with epsilon for nearest filtering at texel corners.
- Texturing: compute unswizzle VRAM->VkImage (specialisation constants per PSM); REGION_REPEAT
  resolved at unswizzle time; CLUT updates batched through a ring of 1024 CLUT snapshots; bindless
  textures; mip level from log2(interpolated Q), not derivatives.
- Feedback (sampling the RT being drawn): per-primitive analysis; if UV - FB position is within a
  fixed range the sample reads the on-tile in-register colour; REGION_CLAMP textures get a "sliced"
  check against the clamp rect; non-clamped feedback is untracked ("questionable").
- Shading: ubershader keeps pixels in registers per tile; lazy shading (only if blend/ATE/mask
  depends on the existing pixel); early-Z unless a pending Z write could change the result;
  waterfall loops scalarise by state/texture index; programmable blending is "free" because the FB
  lives in SSBO/registers (no per-primitive barriers). DATE, PABE, alpha correction (FBA) and
  partial discard are "awkward on normal APIs but workable here".
- SSAA: for 8x keeps 10 VRAM copies (super-sampled, single-sampled reference, 8 sample copies);
  the reference copy detects a game overwriting an RT by copy -> discard super-samples and splat.
  Resolve on tile completion via clustered subgroup add. Sprites forced to single-rate so UI is not
  blurred / sampled outside its rect; flat primitives (same Q and Z) interpolate at single-rate
  pixel centre with a UV bounding box. Super-samples are extra samples, not a larger VRAM -> exact
  2D is preserved. This is the key difference from PCSX2 upscaling.
- CRTC: PMODE/DISPFB/DISPLAY scan-out of the single-sampled copy blended with BGCOLOR. Games that
  downsample progressive output into fields before the CRTC produce interlacing artifacts; per-game
  redirection of CRTC FB addresses suggested; field-rendering games (640x224) are hard.
- Perf: MGS2 at 16x SSAA (= 4x upscale) ~25 W / 17% GPU on an RX 7600; SotC intro: PCSX2 max
  blending ~24 fps at 2x on the author's GPU vs paraLLEl-GS "sweating" at 8x SSAA and CPU-bound on
  geometry; VP2 at 8x SSAA ~ PCSX2 speed.
- Needs: subgroup ballot/shuffle/broadcast/clustered add (sizes 16/32/64), 16-bit storage types,
  bindless, specialisation constants, timeline semaphores. 32-bit Z is fixed-point (no f64 on GPU).
- README fetch of https://github.com/Themaister/parallel-gs returned 404 (repo may have moved).

### RT64 (N64 recompiled ports)
Source: https://github.com/rt64/rt64
- Draw calls are recorded per frame, not sent immediately; RDP ops reading memory as textures are
  deferred until a full sync is needed. Tracks addresses used as framebuffers with dimensions and
  GPU-resident contents; a later sample of such a region becomes a direct GPU copy, but only when
  the result would match a RAM-sourced sample apart from precision. Format reinterpretation handled
  by per-case per-pixel compute shaders. Detection at RDP command level so also valid in LLE.
- Dual renderer: native-resolution renderer syncs to RAM immediately (for CPU readbacks); high-res
  renderer replays recorded draws (possibly at a different rate); native renderer can be disabled
  when the game does not depend on RAM contents.
- Ubershaders "to guarantee no stutters due to pipeline compilation"; RSP transforms in compute;
  texture decode to RGBA32 via compute else sample TMEM directly.
- Widescreen: arbitrary aspect incl. ultrawide; extended command set via NOOP (gEX*) lets ports /
  patches pass extra info; gEXVertexZTest for depth-tested effects (halos) in widescreen; "no
  tolerance for game-specific workarounds" (accuracy first).
- High frame rate by draw-call matching between frames (params, textures, position, orientation,
  velocity), decomposed transforms interpolated separately; "highly experimental".
- D3D12 / Vulkan / Metal only (SM6 required): https://github.com/Zelda64Recomp/Zelda64Recomp
  (ultrawide HUD may be pinned to 16:9; framerate independent of gameplay; most effects modded for
  widescreen).

### Unleashed Recompiled (hedge-dev)
Source: https://github.com/hedge-dev/UnleashedRecomp
- "A new renderer was written from scratch" translating draw calls to D3D12 (SM6) / Vulkan 1.2 via
  the plume abstraction; skips Xenos quirks not needed by a PC port. Bindless textures, shader
  specialisation, pipelines compiled during asset loading in background workers (no first-use
  stutter); a small set of special shaders (post, 2D) compiled at boot. XenosRecomp -> HLSL.
- Options: resolution scale, MSAA beyond original 2x, alpha-to-coverage, more precise reverse-Z
  (fewer Z-fights), 60 fps default with unlocked target; high-frame-rate glitches remain (uneven
  original timing); ultrawide with UI aligned to screen edges or to 16:9 safe area; cutscenes
  locked to original aspect by default (unlocking "causes visual oddities").

### D3D11.3 capability facts (Microsoft Learn)
- ROVs: https://learn.microsoft.com/en-us/windows/win32/direct3d11/rasterizer-order-views — pixel
  shader only; RasterizerOrdered{Buffer,ByteAddressBuffer,StructuredBuffer,Texture2D,...}; order of
  UAV access guaranteed for overlapping invocations of the same draw (same pixel, or pixel+sample in
  sample-frequency mode) in submission order; query D3D11_FEATURE_D3D11_OPTIONS2.ROVsSupported.
- Typed UAV loads: https://learn.microsoft.com/en-us/windows/win32/direct3d11/typed-unordered-access-view-loads
  — D3D11.0 guarantees only R32_FLOAT/UINT/SINT; with OPTIONS2.TypedUAVLoadAdditionalFormats the
  set {RGBA32, RGBA16, RGBA8 unorm/uint/sint, R16, R8} is all-or-nothing; B5G6R5/B5G5R5A1/R16_UNORM
  etc. are per-format (CheckFeatureSupport FORMAT_SUPPORT2 UAV_TYPED_LOAD). Shader flag
  D3D_SHADER_REQUIRES_TYPED_UAV_LOAD_ADDITIONAL_FORMATS.
- Implication: a D3D11 "VRAM as UAV" design should use R32_UINT raw/typed buffers (always loadable)
  and do its own 16/32-bit unpacking, exactly like paraLLEl-GS's SSBO.
