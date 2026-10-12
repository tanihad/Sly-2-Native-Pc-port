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
- SM 5.1 (and therefore wave intrinsics, SM6) is D3D12-only:
  https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/shader-model-5-1 ("introduced with
  D3D12"). D3D11 tops out at *_5_0 profiles: no WaveActiveBallot / subgroup ops, so the
  paraLLEl-GS fine binning (subgroup ballots) and clustered-add SSAA resolve must be rewritten with
  groupshared memory + InterlockedOr / prefix sums.

### PCSX2 hardware renderer (GS/HW) — sources
- DX11 shader: https://raw.githubusercontent.com/PCSX2/pcsx2/master/bin/resources/shaders/dx11/tfx.fx
- Renderer: https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/GS/Renderers/HW/GSRendererHW.cpp
- Texture cache: https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/GS/Renderers/HW/GSTextureCache.h
- Conversions: https://raw.githubusercontent.com/PCSX2/pcsx2/master/bin/resources/shaders/dx11/convert.fx
- Blog: https://pcsx2.net/blog/2016/channel-shuffle-effect ;
  https://www.readonlymemo.com/pcsx2-render-target-hdr-performance-graphical-fixes

Texture cache (GSTextureCache.h):
- Two object kinds: Source (what TEX0 samples) and Target (RenderTarget | DepthStencil, `m_dst[2]`).
  Surface base: m_TEX0/m_TEXA, m_unscaled_size + m_scale (real texture = unscaled*scale), m_age,
  m_end_block ("hint of the surface area", UnwrappedEndBlock for overlap tests).
- Source validity: one u32 per page, bits = the 32 blocks of that page (same granularity as
  paraLLEl-GS); m_from_target + m_from_target_TEX0 (pointer "not durable"); m_region (CLAMP
  sub-region); palette object; hash-cache entry (HashCacheKey = TEX0 hash + CLUT hash + TEX0/TEXA +
  region, used for texture replacement and de-dup).
- Target: per-channel validity (m_valid_alpha_low/high, m_valid_rgb; "valid alpha means we have
  rendered to the alpha channel", else local memory is still up to date), m_alpha_min/max (lets
  blending decisions prove alpha==128 etc.), GSDirtyRectList m_dirty (CPU uploads into a live
  target are applied lazily via UpdateIfDirtyIntersects), m_drawn_since_read, m_is_frame,
  readbacks_since_draw, m_downscaled.
- Lookups: LookupSource/LookupDepthSource(TEX0,TEXA,CLAMP,rect,lod,possible_shuffle,linear,
  req_color/req_alpha); LookupDrawTarget(type, fbmask, preserve_rgb/alpha); LookupDisplayTarget;
  GetExactTarget (BP/BW exact); FindOverlappingTarget; GetTargetWithSharedBits;
  CombineAlignedInsideTargets; SurfaceOffsetKey/SurfaceOffset = "texture located at an offset inside
  a render target" (memoised, up to 65535 entries) — this is the "RT in RT" work that readonlymemo
  says fixed Hitman/Stolen/Tekken 5 half-screen effects after "a couple of months of whack-a-mole".
- Invalidation: InvalidateVideoMem (CPU/local write -> GPU copy stale), InvalidateLocalMem (GPU
  draw -> local memory stale -> readback on CPU access), InvalidateSourcesFromTarget; Read(Target)
  = download to RAM; DISABLE_HW_TEXTURE_CACHE debug mode reads back every target after every draw.
- GPU-side VRAM ops: Move (rect copy BP/BW/PSM -> BP/BW/PSM), ShuffleMove (channel
  reinterpretation), PageMove, CopyPages(ShaderConvert), ConvertColorToDepth / ConvertDepthToColor,
  CopyRGBFromDepthToColor. PaletteMap caches 64 B / 1 KiB palettes keyed by content (4096 each),
  LookupPaletteSource can use a CLUT already resident in GPU memory.
- TargetHeightElem height cache so a FB keeps its previous height; ScaleTargetForDisplay grows a
  target when DISPFB points inside it; m_temporary_source/m_temporary_z per-draw partial copies.

convert.fx (format reinterpretation on GPU) — the exact bit ops PCSX2 needs because targets live
in typed textures rather than one byte-addressable VRAM:
- rgba8<->uint pack (`uint4(c*255.5)&0xFF`), rgb5a1<->uint, depth<->uint `uint(d*exp2(32))`;
  ps_convert_depth32_rgba8 / rgba8_depth32/24/16 / rgb5a1_depth16 / depth16_rgb5a1 /
  depth32_depth24 (mask 0xFFFFFF); bilinear variants quantise with floor(x*2^32)*2^-32.
- ps_convert_rgba_8i / rgb5a1_8i: 32-bit (or 16-bit) target re-read as PSMT8 indices (the channel
  shuffle layout: selects .rb or .ga by (pos.y&2), .x/.y by (pos.x&8)).
- ps_convert_clut_4/8: CLUT read out of a target (CLUT-in-VRAM written by a draw), 8x2 / 8-wide
  subgroup layout of CSM1.
- ps_datm0/1: DATE stencil init (`clip(alpha - 127.5/255)`); _rta_correction variants at 254.5/255.
- ps_colclip_init/resolve: COLCLAMP=0 wrap emulation by rendering into a 16-bit-per-channel
  "HDR" target (`round(c*255)/65535`) and resolving with `uint3(c*65535.5)&255`.
- ps_rta_correction/decorrection: alpha stored as 0..255 vs GS 0..128 ("RTA").
- ps_primid_image_init_*: primitive-ID DATE init. ps_yuv for the CRTC YUV output.

tfx.fx (per-draw ubershader; the same file serves D3D11 and D3D12 devices):
- DATE: PS_DATE 1/2 write primid (or 0x7FFFFFFF) to a float target, then PS_DATE 3 discards
  `primid > PrimMinTexture`; PS_DATE>=5 reads RT alpha directly (needs RT read = barrier/ROV).
  Stencil DATE is set up by the device with ps_datm*.
- SW blend: `Color.rgb = trunc(((A-B)*C)+D)` with RtLoad of Cd; PS_BLEND_MIX adds 124/256 rounding
  bias; PS_BLEND_HW 1-6 rewrite colour/As so fixed-function blend approximates; PS_COLCLIP masks
  with 0xFF (or scales by 65535 in COLCLIP_HW/HDR); PS_PABE zeroes As_rgba when alpha<1 (<128).
- Dither: DitherMatrix[x&3][y&3] at RcpScaleFactor positions (PS_DITHER 1) or raw window position
  (2); PS_DITHER_ADJUST; PS_ROUND_INV.
- FBMASK: `C = (C & ~FbMask) | (RT & FbMask)` via RtLoad.
- Texture shuffle: PS_SHUFFLE unpacks 16-bit 5551 fields into 8-bit channels on read
  (`T.r = (b<<3)&0xF8` ...) and repacks on write; PS_SHUFFLE_SAME (32->16 writes, "The
  Godfather"), PS_READ16_SRC, PS_SHUFFLE_ACROSS; flags SHUFFLE_READ/WRITE/READWRITE.
- Channel shuffle PS_CHANNEL_FETCH 1..6 = red/green/blue/alpha/RGB/gXbY (+ depth byte fetch for
  PS_DEPTH_FMT 1/2), offset by ChannelShuffleOffset.
- ATST LEQUAL/GEQUAL/EQUAL/NOTEQUAL vs AREF (EQUAL uses +-0.5); AFAIL KEEP = discard; FB_ONLY ->
  DISCARD_DEPTH, ZB_ONLY -> DISCARD_COLOR, RGB_ONLY -> restore old alpha (+ optional depth
  discard), RGB_ONLY_DSB uses dual-source alpha blend factor to keep alpha.
- Depth: VS `z = min(z, MaxDepth) * exp2(-32)` (32-bit int Z mapped to float); PS_ZCLAMP,
  PS_ZFLOOR, SW depth test `if (z < DepthLoad) discard`; PS_DEPTH_FMT 1/2/3 sample a depth target as
  RGBA8 / RGB5A1 texture.
- Sampler: PS_WMS/WMT 0-3 region repeat/clamp (REGION_RECT separate); PS_LTF manual bilinear (4
  taps, `frac(uv*WH.zw)`), palette PS_PAL_FMT (8/4HL/4HH via sample_4p), AEM/TA0/TA1, LOD:
  PS_AUTOMATIC_LOD (hw Sample) / PS_MANUAL_LOD (LODParams, K/L), anisotropic path.
- Misc: fog `trunc(lerp(FogColor, c, f*255/256))`, TFX 0-4 with `trunc((C*T)/128)`, TCC,
  PS_DST_FMT 16-bit forces A=0x80 and `&0xF8`, PS_FBA, PS_NO_COLOR/NO_COLOR1, PS_SCANMSK
  (`int(y)&1`), PS_TEX_IS_FB (RtLoad instead of texture), PS_RTA_(SRC_)CORRECTION, PS_ROV_COLOR /
  PS_ROV_DEPTH variants (ROV, with DISCARD macros).

GSRendererHW.cpp (heuristics, in file order):
- DetectTextureShuffleImpl: only when FRAME is 16-bit; 32-bit source needs FBMSK==0xFFFF
  (SwizzleTex32); 16-bit sources classified TwoPixel/Copy/Offset/RegionRepeat8/16/Reverse/
  GappedSwizzle ("handled with a CRC hack for NFS Undercover")/Swizzle; GetValidSize halves
  height/width for possible shuffles; IsSplitTextureShuffle checks the next draw is page-aligned.
- IsPossibleChannelShuffle: texture mapped, PSMT8, sprites, FRAME block == TEX0 block; Draw() then
  skips the N tiny draws and issues one ("HW: Skipped %d channel shuffle draws"), EmulateChannelShuffle
  has HLE patterns: Urban Chaos depth->green, Tales of Abyss depth->alpha, MGS3/Killzone clamp-based
  blue/alpha, Terminator 3 gXbY, min-UV detection (Ape Escape 2, Everybody's Tennis/Golf, Okage,
  VP2), full-screen replacement (GitS, Tekken 5). 2016 blog: GT4's brightness pass was ~1500 of
  ~2250 draws/frame of 64x32 sprites -> HLE'd to 1-2 draws.
- Half pixel offset: RealignTargetTextureCoordinate, modes Normal/Native/SpecialAggressive;
  "Do not apply HPO on texture shuffles as it already aligns the coordinates"; wiki: adds a 0.5 px
  offset to all surfaces, fixes bloom/blur shifted to the upper-left at upscale, game-specific.
- RoundSpriteOffset<linear> rounds interpolated U/V for sprites ("32 is based on taisho-mononoke");
  MergeSprite replaces "paving" sprite grids by one full-screen sprite (Tekken 5).
- Clears: DetectStripedDoubleClear / DetectDoubleHalfClear ("Block when any bits are masked. Too many
  false positives") / DetectRedundantBufferClear; TryGSMemClear does invalid-sized clears on CPU
  ("we don't want to create huge targets"), skips target creation when clearing to zero ("Games tend
  to clear more than they use"); 16-bit clear disguised as 32-bit rewritten with TEXA+PSMCT16.
- Draw(): no_rt/no_ds skip; preserve_rt_rgb/alpha from IsDiscardingDstRGB/Alpha + tex-is-rt;
  "It is allowed to use the depth and rt at the same location"; pending colclip resolve before a
  draw that needs it; "When the format is 24bit (Z or C), DATE ceases to function"; tex_is_rt when
  TEX0 block within FRAME's end block; mipmaps: m_lod from K/L/MXL, trilinear "both layers are
  sampled, only take the big one", layers uploaded in "Round 2" when HWMipmap on.
- Pitfall comments: Ridge Racer V / DD Arena "large ST coordinates in reflection map draws cause
  speckled artifacts"; Snowblind "set TW/TH to 1024 and use UVs for smaller textures inside that";
  Urban Reign scissors a draw to a target at 0x0-0x117F; Pac-Man World Rally degenerate triangles.
- EmulateTextureShuffleAndFbmask: shuffle -> ConvertSpriteTextureShuffle, ps.shuffle, dst format
  forced 32-bit; with DATE "you need to test the green channel instead of the alpha channel";
  FBMSK needs RT read: one barrier if no overlap, else full barrier; "unsafe" path (no feedback
  loop support): "Behavior is clearly undefined however there is a high probability that it will
  work".
- EmulateBlending: A==B / C==0 simplifications; blend_ad_alpha_masked swaps Ad->As when alpha is
  masked; accumulation blend in HW only for "(Cs - 0)*Something + Cd, or with alpha == 1"; "free
  blend" = non-recursive or accumulation needs no barrier; blend_mix 1/2/3 HW+SW mix when clamping;
  accuracy levels: Minimum (non-recursive SW only), Basic (prefers SW + barrier-requiring + free),
  Medium (SW for sprite draws under 100 drawlist items when barriers supported), High (high-alpha
  cases), Full (A!=B with alpha>1), Maximum (always SW, accumulation only 32-bit / no-barrier);
  SW forced by FB fetch, depth feedback, debug. COLCLAMP==0 modes: DISABLED / Free / ACCU HW / SW /
  HW (colclip_mode); PABE: accumulation via ps.pabe + SRC1 factor, feedback -> pure SW;
  "DATE_PRIMID interact very badly with sw blending" -> DATE_BARRIER with full barriers;
  DetermineBarriers: full barrier -> one barrier when no overlap or shuffle; drawlist (per-sprite
  split with barriers between) only with full barriers + feedback loops; multi-pass BLEND_HW1..9
  two-render-pass approximations; "Remove second color output when unused. Works around bugs in some
  drivers (e.g. Intel)". CanUseTexIsFB disabled at Minimum ("Disabling tex-is-fb due to minimum
  blending"). ROV path: GetForcedROVUsage / DetermineROVUsage / ConfigureROV (SW blend, PABE,
  colclip replacement, SW alpha test under ROV) — PCSX2 2.8.0 added ROV support (news:
  https://www.generationamiga.com/2026/08/29/pcsx2-new-update-brings-big-changes-to-the-playstation-2-emulator/
  "new Direct3D 12 features, support for Rasterizer Ordered Views, hardware AA1 support and an
  optional draw-buffering system").
- EmulateZbuffer: ZTST -> depth func; writes off for ZMSK or AA1 lines; max_z from format bits
  ("On the real GS we appear to do clamping on the max z value the format allows"); flat Z clamped
  in VS, else ps.zclamp; ps.zfloor when non-flat Z is written or GREATER.
- EmulateTextureSampler: "Only enable clamping in CLAMP mode. REGION_CLAMP will be done manually in
  the shader"; complex_wms_wmt / shader_emulated_sampler -> ps.ltf manual bilinear; TriFiltering
  Forced/PS2/Automatic/Off -> manual_lod/automatic_lod; point_sampler depends on
  GPUPaletteConversion ("Allow 8-bit textures": send 4/8-bit indices + palette to GPU vs CPU
  conversion to 32-bit); TA/AEM; HandleTextureHazards sets tex_is_fb, scale from
  src_target->GetScale(); "Targets aren't currently offset, so STScale takes care of it";
  m_downscale_source + FilteredDownsampleTexture for "native scaling" of upscaled targets ("Can't
  use box filtering on depth (yet), or fractional scales").
- Misc: OI_BlitFMV (FMV blit past the RT "to avoid various upscaling mess"); IsScalingDraw
  ("stupid draws like Manhunt and Syphon Filter where they sample a single pixel"); CanUseSwSpriteRender
  (small even-aligned 32-bit sprites, no Z/ATE/dither/mip -> software); IsCoverageAlphaSupported
  (AA1 via coverage alpha); BeginHLEHardwareDraw "no other way to ensure there's nothing of the
  last draw left over".
- readonlymemo: PS2 blending has "9 bits of accuracy"; PCSX2 kept converting to Float16 and back
  per draw for games whose colours are "masked/looped instead of clamped to 0-255" — the named games
  are Sly Cooper 2 and 3, Soul Calibur III, Echo Night: Beyond, Big Mutha Truckers; the fix keeps
  the HDR target alive across draws. => Sly 2 uses COLCLAMP=0 wrap-around blending.

### Play! (jpd002) Vulkan GS
Source: https://raw.githubusercontent.com/jpd002/Play-/master/Source/gs/GSH_Vulkan/GSH_VulkanDrawDesktop.cpp
- GS memory is one coherent storage buffer; the fragment shader does the full GS pixel pipeline
  itself: GetPixelAddress via swizzle lookup images, reads dst colour/depth, depth test
  (ALWAYS/NEVER/GEQUAL/GREATER), alpha test, destination alpha test, `((A-B)*C >> 7) + D` blend,
  COLCLAMP clamp or &255, FBA |0x80, `(final & fbWriteMask) | (dst & ~fbWriteMask)`, writes with
  IVec4ToPSM32/16; the colour attachment is a dummy ("needed for MoltenVK/Metal which seem to
  discard pixels that don't write to any color attachment").
- Ordering: whole read-modify-write wrapped in BeginInvocationInterlock/EndInvocationInterlock
  (VK_EXT_fragment_shader_interlock = ROV equivalent); fragment->fragment pipeline barrier with
  BY_REGION between draws ("On Apple M1 GPU, this doesn't seem to be supported and needed").
- Texturing from the same buffer (texels fetched via swizzle tables + CLUT storage buffer with a
  per-draw offset; bilinear by 4 fetches; LOD = log2|1/q| * L + K, bias -0.03125, mips 1-6);
  `textureUseMemoryCopy` keeps a second copy of GS RAM for texture reads because "Some games use tiny
  sprites to do full screen effects that requires to keep a copy of RAM for texture sampling"
  (dirty-region tracked, render pass flushed on overlap). Vertex shader maps `(xy+0.5)*2/DRAW_AREA
  - 1`; no upscaling in this design (native-resolution only). Addresses masked `& (RAMSIZE-1)`.
- Caveats: GEQUAL/GREATER only; assert(false) defaults; it is accurate but native-res and
  interlock-serialised: essentially a GPU software renderer.

### Sly 2 specifics
- PCSX2 patch file https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/SCUS-97316_07652DD9.pnach
  (CRC 07652DD9):
  ```
  [Widescreen 16:9] gsaspectratio=16:9 author=nemesis2000 & PsxFan107
  patch=1,EE,2014D750,word,3C013FDD   // lui at,0x3FDD   -> float 0x3FDDA523 = 1.7316
  patch=1,EE,2014D754,word,3421A523   // ori at,0xA523      (projection X scale constant)
  // Widescreen Bug Fix: hook at 0x14F370 -> cave 0xC0000: lui at,0x3F40 (0.75); mtc1 at,f30;
  //   mul.s f4,f4,f30; j 0x14F378; div.s f3,f1,f3 (displaced instr)
  patch=1,EE,2014F370,word,08030000 / 200C0000,3C013F40 / 200C0004,4481F000 /
  200C0008,461E2102 / 200C000C,08053CDE / 200C0010,460308C3
  // Character Portraits Fix: jal from 0x1BE808 -> cave 0xC0014: lui at,0x45AA (5440.0 = 4096*4/3);
  //   mtc1 at,f30; mul.s f20,f20,f30; jr ra
  patch=1,EE,201BE808,word,0C030005 / 200C0014,3C0145AA / 200C0018,4481F000 /
  200C001C,461EA502 / 200C0020,03E00008
  [No-Interlacing] gsinterlacemode=1 author=asasega   patch=1,EE,20240B4C,extended,00000000
  [No Motion Blur] author=Meos                        patch=1,EE,20160FA0,extended,00000000
  ```
  => widescreen is one projection constant (the game has no native 16:9); two call-site fixes
  (a 0.75 X factor in a culling/screen-space routine at 0x14F370 and a 4/3 X scale for 2D
  character portraits at 0x1BE808); the game has an interlace switch at 0x240B4C and a
  *motion blur* post effect (0x160FA0) that is a full-screen feedback draw.
- PCSX2 wiki (search excerpts; page itself 403s for fetch): HW mode shows "vertical blue lines"
  (fixed in SW mode); "because PCSX2 does not implement edge antialiasing in Hardware mode, many of
  the outlines are jagged" (Sly's cel-shade outlines use AA1); "screen shaking without Blend bff"
  (field-rendered output, needs field blending de-interlace); a tester used "TC offset X 280, Y 580"
  to fix ghosting at 3x with 4x MSAA.
- Forum (2022 dev build, https://forums.pcsx2.net/Thread-Some-problems-with-the-latest-dev-build?pid=624445):
  Sly 2/3 flicker under Vulkan and "gets worse as you increase Blending Unit... it does have to do
  with shadows"; Skipdraw 1 removes flicker and shadows; on D3D11 the games "need GPU Palette
  Conversion or else lots of the ground is missing and most objects become black and fragmented";
  GPU usage spikes at certain camera angles (30-50% speed).
- Episode 2 photo mission (https://forums.pcsx2.net/Thread-Zoom-issues-with-Sly-2?pid=571997):
  camera zoom/photo detection broke with D3D11 or widescreen patches (readback-dependent logic?).
- PCSX2 GameDB (GameIndex.yaml, raw slice 300000-400000):
  SCUS-97316 Sly 2: compat 5, gsHWFixes `halfPixelOffset: 5 # fixes chromatic effect`;
  SCUS-97464 Sly 3: `halfPixelOffset: 5 # fixes chromatic effect`, vuRoundMode 0, vu0ClampMode 3;
  SCUS-97198 Sly 1: `halfPixelOffset: 5 # fixes post alignment`, `autoFlush: 1 # fixes Sly's
  shadow definition`. Config.h enum GSHalfPixelOffset = Off, Normal, Special, SpecialAggressive,
  Native, NativeWTexOffset(=5) -> Sly needs "Align to Native (with texture offset)": the
  post-process ("chromatic" colour-separation / motion-blur) samples the frame RT and only lines up
  when the texture coordinates are aligned to native pixels. AutoFlush = flush the draw list when a
  draw samples the target it writes (feedback) -> Sly's shadows read the RT they draw into.
- PR https://github.com/PCSX2/pcsx2/pull/14945 (GL fallback): "Smoke test Sly 2 on GL, make sure
  shadows work on newer hardware... For older hardware Sly 2 has precision issues which is
  expected" — the shadows depend on hardware blending into an RGBA16_UNORM colclip ("HDR") target
  (fallback RGBA16F loses precision). Together with readonlymemo: Sly 2's shadows are drawn with
  COLCLAMP=0 (wrapping) blending into the colour buffer; exact 8-bit wrap arithmetic is required.
- Issue https://github.com/PCSX2/pcsx2/issues/2964: Sly trilogy cel-shading "black lines" break
  when upscaled (GL worse than DX11; Round Sprite Full helps); gregory38: "I miss some barriers
  because I was careless for point and line"; "unscaling (no safe hack) might overlap pixels that
  wouldn't in the first place". => outlines are GS line primitives (AA1) whose width must stay 1
  native pixel (scaled) and whose coverage must not be double-blended.
- PCSX2 2.8 blog (https://pcsx2.net/blog/2026/pcsx2-2.8): hardware AA1 "adds a semi-transparent
  border around triangles... doesn't write depth", emulated with the new software depth path,
  "Due to the high cost of the software depth emulation, this is currently disabled by default";
  Sly 2 is one of the before/after showcase games. ROV on D3D12/D3D11/Vulkan "allows higher
  blending accuracy levels to incur less of a performance hit", "works well in cases where the
  number of barriers would be excessively high (e.g. with AA1 enabled)", enabled by heuristic
  (DetermineROVUsage: only when it saves >= 2 barriers; colour ROV forces depth ROV when discard /
  depth output / DATE / alpha test; ConfigureROV: SW blend, PABE, SW colclip, shader DATE "Don't
  use stencil with ROV", alpha test FEEDBACK; "Remove regular barriers"). Depth feedback: old
  approach "rendered every draw call twice"; now SW depth test in shader. D3D11 is now "the least
  preferred renderer", D3D12 default; Enhanced Barriers "removes the need for aliased resources in
  feedback reads"; tristrip merging "massive performance gain on some games".
- D3D11 device (GSDevice11.cpp): feature level 11_1->11_0->10_0; features: primitive_id,
  stencil_buffer, vs_expand (points/lines expanded in VS from a structured buffer), rov = typed UAV
  loads + ROVsSupported + UAV support for every write format; no texture_barrier / FB fetch, so RT
  reads use FeedbackCopyAndBind: CopyRect of draw-area (+sample-area) into draw_rt_clone bound as
  SRV; full_barrier loops over the draw list copying each entry's bbox then drawing it ("DX11 can't
  do partial depth copies"); DATE: stencil (SetupDATE: ps_datm* into stencil, EQUAL test) or PrimID
  (primid_init + MIN blend + ps.date=3); colclip target R16G16B16A16_UNORM if blendable else
  FP16; "OM targets and UAVs share the same namespace in DX11" (OMSetRenderTargetsAndUnorderedAccessViews).

### Xenia ROV (Xbox 360 EDRAM on D3D12) — closest analogue of "VRAM as UAV"
Source: https://github.com/xenia-project/xenia/wiki/ROV
- ROV path renders "directly to the 10 MB buffer" doing "blending and depth/stencil testing
  manually in pixel shaders"; "much higher performance since there's no expensive data copying";
  RT path = conventional render targets + copies between formats, "a very heavy performance
  impact", and "unfixable transparency issues" / depth problems from format approximation.
  Minimum HW: NVIDIA Maxwell 2 (GTX 950), AMD Vega, Intel HD 4200. Feature level 12_1 requires ROV
  (https://en.wikipedia.org/wiki/Feature_levels_in_Direct3D: RTX 40 = 12_2, CR tier 3, typed UAV
  loads required at 12_0).

### pnach addresses mapped to our recompiled functions (build/sly2/recomp, read-only)
- 0x14D750 `lui $at,0x3FA6` (orig float 0x3FA6xxxx ~1.30; patch 1.7316 = x4/3) in f_0014D6D8
  (funcs_004.c:2598) -> projection / viewport X-scale setup; widescreen = scale X by 3/4 here.
- 0x14F370 `div.s $f3,$f1,$f3` in f_0014F308 (funcs_004.c:6150) -> hook multiplies $f4 by 0.75
  (screen-space / culling width fix).
- 0x1BE808 `mul.s $f20,$f20,$f1` in f_001BE6F0 (funcs_007.c:37695) -> 2D portraits X scale x4/3.
- 0x240B4C in f_00240AE0 (funcs_012.c:25847) -> interlace/SMODE2 setup (No-Interlacing writes 0).
- 0x160FA0 `lw $v1,-0x14DC($v0)` in f_00160F98 (funcs_004.c:41289) -> motion blur post effect
  entry (No Motion Blur nops the load).
