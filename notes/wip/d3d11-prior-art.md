# WIP: d3d11 prior-art research (role: prior-art)

Task: research (web only) how other projects map the PS2 GS / console GPUs to modern GPU APIs;
dense sourced summary + lessons constraining our D3D11 design.

Status: 2026-10-11, pass 2 â€” DONE. Raw findings below; the deliverable (summary + lessons) is the
last section "## Summary (extended notes; the condensed deliverable is the next section)". wiki.pcsx2.net could not be read by any route (403 / bot
check); its content is taken from search-engine excerpts and is marked as such.

## Plan
1. [x] paraLLEl-GS (Themaister blog) â€” README fetch 404'd, blog post read in full
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
- ROVs: https://learn.microsoft.com/en-us/windows/win32/direct3d11/rasterizer-order-views â€” pixel
  shader only; RasterizerOrdered{Buffer,ByteAddressBuffer,StructuredBuffer,Texture2D,...}; order of
  UAV access guaranteed for overlapping invocations of the same draw (same pixel, or pixel+sample in
  sample-frequency mode) in submission order; query D3D11_FEATURE_D3D11_OPTIONS2.ROVsSupported.
- Typed UAV loads: https://learn.microsoft.com/en-us/windows/win32/direct3d11/typed-unordered-access-view-loads
  â€” D3D11.0 guarantees only R32_FLOAT/UINT/SINT; with OPTIONS2.TypedUAVLoadAdditionalFormats the
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

### PCSX2 hardware renderer (GS/HW) â€” sources
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
  a render target" (memoised, up to 65535 entries) â€” this is the "RT in RT" work that readonlymemo
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

convert.fx (format reinterpretation on GPU) â€” the exact bit ops PCSX2 needs because targets live
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
  colclip replacement, SW alpha test under ROV) â€” PCSX2 2.8.0 added ROV support (news:
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
  per draw for games whose colours are "masked/looped instead of clamped to 0-255" â€” the named games
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
  expected" â€” the shadows depend on hardware blending into an RGBA16_UNORM colclip ("HDR") target
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

### Xenia ROV (Xbox 360 EDRAM on D3D12) â€” closest analogue of "VRAM as UAV"
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

## Summary (extended notes; the condensed deliverable is the next section)

### 1. paraLLEl-GS: the GS as a GPU compute rasterizer (accuracy + SSAA)
Themaister's write-up (https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation, repo https://github.com/Arntzen-Software/parallel-gs) is the only project that reaches software-renderer accuracy on the GPU with anti-aliasing. Design: the 4 MiB VRAM is one storage buffer; hazards are tracked per 8 KiB page (pending FB writes/reads) and per 256-byte block (32-bit mask per page) for copies, CLUT/image reads and clobbers, with separate FB-write and texture-read masks so the "palette in the unused upper bits of 24-bit colour" (8H/4HL/4HH) trick is not a false hazard. Hazards are resolved by batching: copy-after-copy is a barrier, a copy over a sampled texture flushes copies/unswizzles but keeps the primitive batch, and sampling/copying the current RT breaks the batch (one scene dropped from 500+ barriers to 18 after per-primitive feedback analysis). Rasterization is tile-based (32x32 tiles, u16 primitive lists, <= 64k prims/pass), with barycentric edge functions in 32-bit integer math with 8 sub-pixel bits (GS needs 33 bits, so inputs are downsampled slightly and the error re-added), a ~24-bit fixed-point reciprocal for reproducibility, D3D9-style top-left rule with pixel centres on integer coordinates, parallelograms for wide lines/sprites, and a soft-floor UV snap for nearest sampling at texel corners. Texturing: compute unswizzle VRAM->image per PSM via specialization constants, REGION_REPEAT resolved at unswizzle, CLUT updates batched through a 1024-entry snapshot ring, bindless textures, mip level from log2 of interpolated Q (not derivatives). Shading: an ubershader keeps the tile's pixels in registers; shading is lazy (only when blend/ATE/mask needs the old pixel), early-Z unless a pending Z write could change the result, waterfall loops scalarise by state, and because the framebuffer lives in SSBO/registers "programmable blending is effectively free", DATE/PABE/FBA/partial-discard being "awkward on normal APIs but workable here". SSAA keeps N sample copies plus a single-sampled reference copy (8x => 10 VRAM copies); the reference detects a game overwriting an RT by copy so super-samples are discarded and splatted; resolve on tile completion via clustered subgroup add; sprites are forced single-rate so UI is never blurred or sampled outside its rect; flat primitives (same Q and Z) are interpolated at the single-rate pixel centre with a UV bounding box. CRTC scan-out reads the single-sampled copy; games that downsample progressive output into fields produce interlacing artifacts (per-game CRTC redirection suggested); field-rendering games (640x224) remain hard. Performance: MGS2 at 16x SSAA (= 4x upscale) ~25 W / 17% of an RX 7600; Shadow of the Colossus intro beats PCSX2 (24 fps at 2x with max blending on the author's GPU) while at 8x SSAA and CPU-bound on geometry. Requirements (README): descriptorIndexing, timelineSemaphore, 8/16-bit storage, shaderInt16, scalarBlockLayout, full subgroup support with sizes 16-64; not bit-exact in varying interpolation; AA1 "questionable" because its hardware details are unknown; CRTC field blending and a weave deinterlacer are built in; tested on RTX 4070/3060, RX 6800/7600, Arc A770/B580, Deck.

### 2. PCSX2 GS hardware renderer (GSdx lineage): what a render-target-cache design costs
Sources: tfx.fx (https://raw.githubusercontent.com/PCSX2/pcsx2/master/bin/resources/shaders/dx11/tfx.fx), convert.fx, GSRendererHW.cpp, GSTextureCache.h, GSDevice11.cpp (same tree), the 2016 channel-shuffle post (https://pcsx2.net/blog/2016/channel-shuffle-effect), the RT/HDR article (https://www.readonlymemo.com/pcsx2-render-target-hdr-performance-graphical-fixes) and the 2.8 blog (https://pcsx2.net/blog/2026/pcsx2-2.8).
- Texture cache. Two kinds of objects: Source (what TEX0 samples; validity = one u32 per page, bits = the 32 blocks; may be created from a Target with a recorded TEX0 and a "not durable" pointer; palette object; hash-cache entry keyed on TEX0 hash + CLUT hash) and Target (RenderTarget | DepthStencil; unscaled size + scale; per-channel validity "valid alpha means we have rendered to the alpha channel", alpha min/max; dirty-rect list for CPU uploads into a live target; m_drawn_since_read / readbacks_since_draw). Lookups by BP/BW/PSM with overlap search, "RT in RT" (SurfaceOffset: a texture at an offset inside a target, memoised) which took "a couple of months of whack-a-mole" and fixed Hitman/Stolen/Tekken 5 half-screen effects. Invalidation is two-way (InvalidateVideoMem on CPU writes, InvalidateLocalMem on GPU draws => readback on CPU access); GPU-side Move/ShuffleMove/PageMove, ConvertColorToDepth/DepthToColor, target height cache, display-target growth when DISPFB points inside a target. 2.8 still lists target mis-picks, single-pixel overlaps, zero-height targets and shrinking depth targets as fresh fixes: this heuristic layer never converges.
- Format reinterpretation (convert.fx): RGBA8<->uint pack, RGB5A1<->uint, depth<->uint(d*2^32), depth32->RGBA8 / RGB5A1, RGBA8->PSMT8 index layout for channel shuffles, CLUT read out of a target (CSM1 8x2 layout), DATM stencil init, "colclip" init/resolve (COLCLAMP=0 wrap emulated by rendering into an RGBA16_UNORM target and `& 255` on resolve), RTA correction (alpha 0..128 vs 0..255). Every one exists only because targets are typed textures rather than one byte-addressable VRAM.
- Per-draw ubershader (tfx.fx): DATE via stencil, primitive-ID (two-pass MIN blend) or RT-alpha read; SW blend `trunc(((A-B)*C)+D)` reading the RT, HW blend approximations BLEND_HW1-9 (two render passes), blend_mix with a 124/256 rounding bias, PABE, FBMASK `(C & ~mask) | (RT & mask)`, dithering with the 4x4 DIMX at native positions (RcpScaleFactor), texture shuffle (16-bit 5551 fields unpacked/repacked in 8-bit channels; with DATE "test the green channel instead of alpha"), channel shuffle fetches (red/green/blue/alpha/RGB/gXbY + depth bytes), ATST with EQUAL +-0.5, AFAIL as discard / DISCARD_DEPTH / DISCARD_COLOR / restore-old-alpha / dual-source alpha, Z = min(z, MaxDepth) * 2^-32 with ZCLAMP/ZFLOOR and a SW depth test, depth sampled as RGBA8/RGB5A1, manual bilinear (PS_LTF) for region clamp/repeat, palette lookup (8 / 4HL / 4HH), AEM/TA0/TA1, manual LOD from K/L, fog `trunc(lerp(fog,c,f*255/256))`, TFX `trunc(C*T/128)`, 16-bit destination `A=0x80, &0xF8`, SCANMSK, TEX_IS_FB, ROV variants.
- Blending accuracy ladder (EmulateBlending): Minimum (non-recursive SW only, free since 2.8), Basic, Medium (SW for sprite draws < 100 drawlist items when barriers exist), High, Full, Maximum (always SW). Accumulation blend is HW-safe only for "(Cs - 0)*Something + Cd, or with alpha == 1"; anything else needs a RT read, i.e. a barrier (GL/VK), a copy (D3D11: FeedbackCopyAndBind copies the draw area into a clone SRV; a "full barrier" loops over the draw list copying each entry's bbox then drawing it; "DX11 can't do partial depth copies"), a primitive-by-primitive drawlist ("Sw blending will force the draw to run primitive by primitive"), or ROV. "DATE_PRIMID interact very badly with sw blending". DATE "ceases to function" on 24-bit targets.
- ROV (2.8, D3D12/D3D11/Vulkan): DetermineROVUsage enables it only when it saves >= 2 barriers; colour ROV forces depth ROV whenever there is discard, depth output, DATE or alpha test; ConfigureROV switches to SW blend, PABE, SW colclip, shader DATE ("Don't use stencil with ROV"), alpha-test FEEDBACK, and "Remove regular barriers". The blog: ROV "allows higher blending accuracy levels to incur less of a performance hit" and "works well in cases where the number of barriers would be excessively high (e.g. with AA1 enabled)". Xenia's EDRAM-in-UAV path reports the same: ROV gives "much higher performance since there's no expensive data copying" versus the RT path's copies and "unfixable transparency issues" (https://github.com/xenia-project/xenia/wiki/ROV).
- AA1 (2.8): "adds a semi-transparent border around triangles... doesn't write depth", emulated through the new software-depth path and "disabled by default" for cost; Sly 2 is a showcase game. Lines/points are expanded in the vertex shader from a structured buffer (vs_expand) on D3D11.
- Upscaling hacks are all symptom patches of rendering at k x native in a pixel-centre-aligned rasterizer: Half Pixel Offset (Off/Normal/Special/SpecialAggressive/Native/NativeWTexOffset; "adds a 0.5 pixel offset to all surfaces", fixes bloom/blur shifted to the upper-left), Round Sprite (Half = flat sprites, Full = all; rounds interpolated U/V), Align Sprite, Merge Sprite ("paving" sprite grids -> one sprite, Tekken 5), Texture Offsets (last resort), Native Scaling (Off/Normal/Aggressive/NormalUpscaled/AggressiveUpscaled: detect "scaling draws" and downsample the source target so native-res post effects stay aligned), Wild Arms offset, bilinear-dirty hacks, "Limit 24-bit depth". GameDB pitfalls: speckles from large ST coordinates (Ridge Racer V), Snowblind's TW/TH=1024 with UV sub-rects, degenerate triangles, scissors into tiny targets, FMV blits past the RT "to avoid various upscaling mess".
- Channel/texture shuffles: GT4's brightness pass was ~1500 of ~2250 draws per frame of 64x32 sprites reading the 32-bit frame as PSMT8 through a CLUT; GSdx HLE'd it to 1-2 draws per game pattern (Urban Chaos depth->green, Tales of Abyss depth->alpha, MGS3/Killzone, Terminator 3 gXbY, Ape Escape 2, VP2, GitS, Tekken 5). Texture shuffle = 16-bit sprites re-reading a 32-bit target with FBMSK 0xFFFF to swap bytes; classified TwoPixel/Copy/Offset/RegionRepeat/Reverse/GappedSwizzle/Swizzle (one class still "handled with a CRC hack"). A VRAM-addressed renderer needs none of this detection.
- Readbacks: HWDownloadMode Enabled / ForceFull / NoReadbacks / Unsynchronized / Disabled; 2.8 added a consolidated single download because "Some games issue multiple small, partial GPU-to-CPU readbacks per frame"; a 30% speedup in Time Crisis 3 came from avoiding readbacks.
- D3D11 specifics (GSDevice11.cpp): FL 11_1 -> 11_0 -> 10_0; no texture barrier / FB fetch, so every RT read is a copy; `rov = typed UAV loads && ROVsSupported && UAV support for every write format`; stencil DATE and PrimID DATE; colclip target R16G16B16A16_UNORM when blendable; "OM targets and UAVs share the same namespace in DX11". PCSX2 2.8 now calls D3D11 "the least preferred renderer".

### 3. Play! (Vulkan GSH): a GPU "software renderer" via fragment shader interlock
https://raw.githubusercontent.com/jpd002/Play-/master/Source/gs/GSH_Vulkan/GSH_VulkanDrawDesktop.cpp: GS memory is one coherent storage buffer; the GPU rasterizer only produces fragments, and the fragment shader does the whole GS pixel pipeline itself (swizzled address via lookup images, dst colour/depth read, ZTST GEQUAL/GREATER, ATE, DATE, `((A-B)*C >> 7) + D`, COLCLAMP or `& 255`, FBA, write mask, PSM32/16 pack) between BeginInvocationInterlock/EndInvocationInterlock (VK_EXT_fragment_shader_interlock = ROV); a fragment->fragment pipeline barrier separates draws; textures are fetched from the same buffer with a CLUT storage buffer, bilinear by 4 fetches, LOD = log2|1/q|*L + K; a second copy of GS RAM serves texture reads because "some games use tiny sprites to do full screen effects". Native resolution only, assert(false) on unhandled modes. It proves a UAV-VRAM + ROV pixel pipeline is feasible on consumer GPUs but shows the cost of serialising every fragment.

### 4. Native-port renderers (RT64, Unleashed Recompiled)
RT64 (https://github.com/rt64/rt64): draw calls are recorded per frame, not issued immediately; framebuffer addresses, sizes and GPU contents are tracked and a later sample becomes a GPU copy only "when the result would match a RAM-sourced sample apart from precision"; per-case compute shaders reinterpret formats; a dual renderer keeps a native-resolution copy in sync with RAM for CPU readbacks while a high-resolution renderer replays the draws (can be disabled when the game never reads RAM); ubershaders "to guarantee no stutters"; RSP transforms in compute; widescreen is arbitrary aspect plus an extended command set (gEX*) the port injects via NOOPs (gEXVertexZTest for depth-tested halos in widescreen); "no tolerance for game-specific workarounds"; high-frame-rate by draw-call matching is "highly experimental". Zelda64Recomp pins the HUD to 16:9 on ultrawide and decouples frame rate from gameplay. Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp): "A new renderer was written from scratch", D3D12 (SM6)/Vulkan via plume, bindless + shader specialisation with pipelines compiled during asset loading, resolution scale, MSAA beyond the original 2x, alpha-to-coverage, more precise reverse-Z, ultrawide with UI pinned to 16:9 or screen edges, cutscenes "locked to their original aspect ratio" because unlocking "causes visual oddities", and residual high-frame-rate glitches from the original's uneven timing. Both treat widescreen/HUD as game-code patches, not renderer heuristics.

### 5. Sly 2 / Sly 3 specifics
- GameDB (GameIndex.yaml): Sly 2 (SCUS-97316) and Sly 3 `halfPixelOffset: 5 # fixes chromatic effect` (= Align to Native with texture offset); Sly 1 `halfPixelOffset: 5 # fixes post alignment`, `autoFlush: 1 # fixes Sly's shadow definition` (autoflush = flush when a draw samples the target it writes). The post-process that separates colour channels / motion-blurs samples the frame RT and is only aligned at native texel positions.
- Shadows: readonlymemo names Sly Cooper 2 and 3 among the few games whose colours are "masked/looped instead of clamped to 0-255" (COLCLAMP=0), and PR https://github.com/PCSX2/pcsx2/pull/14945 tests "Sly 2 on GL, make sure shadows work on newer hardware... For older hardware Sly 2 has precision issues" (RGBA16_UNORM blending vs FP16): the shadows depend on exact 8-bit wrap-around blend arithmetic. A 2022 forum report (https://forums.pcsx2.net/Thread-Some-problems-with-the-latest-dev-build?pid=624445): Sly 2/3 flicker on Vulkan and "gets worse as you increase Blending Unit... it does have to do with shadows", Skipdraw 1 removes both; on D3D11 they "need GPU Palette Conversion or else lots of the ground is missing and most objects become black and fragmented" (CPU-side palette expansion of frequently re-palettised textures went stale); GPU usage spikes at certain camera angles.
- Outlines: https://github.com/PCSX2/pcsx2/issues/2964 â€” the trilogy's cel-shading "black lines" look broken when upscaled on GL, Round Sprite Full helps; gregory38: "I miss some barriers because I was careless for point and line" and "unscaling (no safe hack) might overlap pixels that wouldn't in the first place". Wiki excerpts (page itself is bot-walled): "because PCSX2 does not implement edge antialiasing in Hardware mode, many of the outlines are jagged"; HW mode shows "vertical blue lines"; "screen shaking without Blend bff" (field-rendered output needs field blending); a tester needed "TC offset X 280, Y 580" against ghosting at 3x with 4x MSAA; software mode recommended. Episode-2 photo mission misdetects with D3D11 or widescreen (https://forums.pcsx2.net/Thread-Zoom-issues-with-Sly-2?pid=571997).
- Widescreen (https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/SCUS-97316_07652DD9.pnach, nemesis2000 & PsxFan107): one projection constant at 0x14D750/54 (`lui/ori $at` 0x3FA6xxxx -> 0x3FDDA523, i.e. x4/3; inside our f_0014D6D8), a hook at 0x14F370 (f_0014F308) multiplying $f4 by 0.75, a hook at 0x1BE808 (f_001BE6F0) scaling 2D character portraits by 5440/4096 = 4/3; plus "No-Interlacing" (0x240B4C in f_00240AE0 = 0) and "No Motion Blur" (0x160FA0 in f_00160F98 = 0). The game has no native 16:9; the HUD is unpatched (letterboxed 4:3 UI stretched to 16:9 is what PCSX2 users get) and the motion blur is a frame-feedback effect.

### 6. D3D11 capability facts that bound the design
ROVs are a D3D11.3 feature (query D3D11_FEATURE_D3D11_OPTIONS2.ROVsSupported; pixel shader only; ordering guaranteed only among overlapping invocations of the same draw) https://learn.microsoft.com/en-us/windows/win32/direct3d11/rasterizer-order-views. Typed UAV loads: D3D11.0 guarantees only R32_FLOAT/UINT/SINT; the RGBA32/RGBA16/RGBA8/R16/R8 set is all-or-nothing behind TypedUAVLoadAdditionalFormats; B5G5R5A1/B5G6R5/R16_UNORM are per-format https://learn.microsoft.com/en-us/windows/win32/direct3d11/typed-unordered-access-view-loads. RTX 40 is feature level 12_2 (ROV and typed loads required from 12_0/12_1, conservative raster tier 3) https://en.wikipedia.org/wiki/Feature_levels_in_Direct3D. D3D11 shaders are *_5_0 only; SM 5.1 and wave intrinsics are "introduced with D3D12" https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/shader-model-5-1. cs_5_0 has groupshared, interlocked ops and 64 KB TGSM but no subgroup ballot/shuffle/clustered add, no bindless (128 SRV slots), and no 16-bit storage types (uint16 must be packed into R32 buffers).

### Lessons that constrain our D3D11 design
1. Do not build a render-target cache. PCSX2's Source/Target/overlap/offset/shuffle heuristics are still receiving correctness fixes after 20 years, need per-game GameDB entries (Sly needs two of them), and every reinterpretation (colour<->depth, 32<->16, RT-as-PSMT8, CLUT-in-RT) is a separate shader. Keep the single swizzled 4 MB VRAM as the source of truth (as our software GS, paraLLEl-GS, Play! and Xenia-ROV do) and address it from shaders with the GS swizzle; R32_UINT raw/structured buffers are always UAV-loadable on D3D11, so pack 16-bit and 8-bit formats ourselves.
2. The GS pixel pipeline must run in the shader, in GS order, with 8-bit integer arithmetic: wrap-around COLCLAMP, PABE, FBA, FBMSK, DATE/DATM, ATST+AFAIL partial writes, DTHE 4x4 at native positions, 24/16-bit Z clamp. Sly 2's shadows (COLCLAMP=0, flicker with PCSX2's blend levels, RGBA16 precision dependence) and its outlines are exactly the cases PCSX2 still needs ROV/barriers/HDR for. Fixed-function blend + typed RTs cannot express them; ROV or compute gives them for free.
3. Choose between two sound D3D11 shapes: (a) compute tile rasterizer (paraLLEl-GS) rewritten for cs_5_0 â€” binning via groupshared atomics/prefix sums instead of subgroup ballots, no bindless (texture atlas/array or sampling VRAM directly), timeline semaphores replaced by event/fence queries; or (b) GPU-rasterized draws with ROV pixel shaders doing the GS pixel pipeline into the VRAM UAV (Play!/Xenia shape; RTX 40 supports ROV and PCSX2 2.8 now uses it on D3D11). (b) is simpler and keeps vertex processing on the GPU rasterizer, but ROV ordering is per draw only, so draw boundaries become the hazard unit and overlapping draws need UAV barriers (unbind/rebind) between them; (a) batches hundreds of draws per pass with explicit page-level hazard tracking and gets SSAA structurally.
4. Upscaling must be super-sampling or sample-exact scaling, not "render at k x native". Every PCSX2 upscale hack (half-pixel offset, round/align/merge sprite, texture offsets, native scaling, the Sly "chromatic effect" and TC-offset ghosting, broken cel lines) comes from moving pixel centres and texel footprints. paraLLEl-GS's rule set is the one to copy: rasterize at the native integer grid with extra samples per pixel, force sprites and flat (same Q, same Z) primitives to single-rate at the native pixel centre with a UV bounding box, keep a single-sampled reference copy so copies/readbacks/CLUT reads/post-effects see exactly native data, and resolve only for display. Sly 2's full-screen feedback effects (motion blur, chromatic separation, shadows reading the RT, CLUT-in-VRAM) all read the native copy and stay aligned.
5. Hazard tracking at page (8 KiB) + block (256 B) granularity with separate read/write masks and separate "FB write" vs "texture read" masks is the proven minimum; it is also what our software GS already does (page ownership, RAW/SNAP/HOLD batches), so the GPU backend can reuse those decisions and only has to add "copy over sampled texture" vs "sample the current RT" distinctions (the latter costs a batch break or ROV).
6. Feedback (texture == frame) must be handled per primitive: if UV minus FB position is within a fixed range, read the in-flight pixel (ROV/on-tile); REGION_CLAMP textures can be checked against their clamp rect; otherwise snapshot. PCSX2's autoflush/"tex is fb"/clone copies and Play!'s second RAM copy are the expensive alternatives. Sly 1's GameDB autoflush entry says Sly's shadows need this path.
7. Lines/AA1 are a first-class primitive: Sly's outlines are AA1 lines whose coverage replaces alpha and which do not write depth (PCSX2 2.8 blog). Width must stay one native pixel at any sample count, coverage must come from the GS line rule (our software DDA, two pixels across the minor axis), and coverage must not be double-blended when supersampled (paraLLEl-GS calls its AA1 "questionable"; PCSX2's cost the whole software-depth path). This argues for single-rate line rasterization with analytic coverage even inside an SSAA design.
8. Readbacks and CRTC: keep a native-resolution VRAM copy that is always correct for TRXDIR=1 reads, CLUT loads from VRAM and DISPFB scan-out (RT64 keeps a whole native renderer for this). Sly 2 is field-rendered ("Blend bff" needed in PCSX2, "No-Interlacing" patch exists): the display path must blend/weave fields from VRAM at native resolution and optionally run the game progressive via the 0x240B4C switch rather than deinterlacing a high-res frame.
9. Texture decode on the GPU, not the CPU: PCSX2's D3D11 "GPU Palette Conversion" being required for Sly 2 (missing ground, black fragmented objects) shows that Sly re-palettises textures too often for CPU-side CLUT expansion to track; sample indices from VRAM and apply the CLUT in-shader (CLUT snapshots batched in a ring as paraLLEl-GS does).
10. No per-draw state explosion: use one ubershader (RT64, paraLLEl-GS, PCSX2 all do) with uniform branching per batch; D3D11 has no pipeline-cache stutter problem but has a 128-SRV/8-UAV bind limit, so VRAM-as-buffer plus a small set of decoded texture caches is the practical binding model.
11. Widescreen/HUD belong in game code, not the renderer: the community patch is three code sites (projection X scale in f_0014D6D8 x3/4; a culling/width factor in f_0014F308; portrait scale in f_001BE6F0) and leaves the 4:3 HUD stretched. Our recompiler should patch those functions natively, keep the HUD at 4:3 inside a 16:9 frame (Zelda64Recomp/Unleashed pin UI to the safe area) and keep the 4-pixel-aligned GS scissor/FRAME sizes untouched; the renderer only needs to expose a wider framebuffer width (FBW/scissor are per-draw GS state, so a wider frame is an FBW+XYOFFSET change in the display and projection code, not a renderer hack).
12. Performance targets from prior art: paraLLEl-GS does 16x SSAA of MGS2 at 17% of an RX 7600; PCSX2 D3D11 at Maximum blending collapses to primitive-by-primitive copies; Play! serialises every fragment with interlock. For our 60 fps / 16.7 ms budget on an RTX 4060 Ti, a batched compute or ROV design with hazard-aware batching is the only route that is both exact for Sly 2's blend tricks and fast; the EE thread (7.7 ms) already produces the command stream, so the GPU path must accept the existing gs_write_reg/gs_image_write stream and page-ownership batches from gs_draw_pixel.c unchanged.

## Deliverable: prior-art summary and lessons (condensed, ~2800 words)

### 1. paraLLEl-GS: the GS as a compute rasterizer with exact SSAA
Themaister's post (https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation; repo https://github.com/Arntzen-Software/parallel-gs) is the only GPU GS that reaches software-renderer accuracy plus anti-aliasing. VRAM is one 4 MiB storage buffer. Hazards are tracked per 8 KiB page (pending FB writes/reads) and per 256-byte block (a u32 mask per page) for copies, CLUT/image reads and clobbers, with separate FB-write and texture-read masks so the "palette in the unused upper byte of 24-bit colour" trick (8H/4HL/4HH) is not a false hazard. Resolution is by batching: copy-after-copy is a barrier; a copy over a sampled texture flushes copies/unswizzles but keeps the primitive batch; sampling or copying the current RT breaks the batch (one scene went from 500+ barriers to 18 after per-primitive feedback analysis). Rasterization is tile-based (32x32 tiles, u16 primitive lists, 64k prims/pass), barycentric edge functions in 32-bit integers with 8 sub-pixel bits (the GS needs 33 bits, so inputs are downsampled and the error re-added), a ~24-bit fixed-point reciprocal for reproducibility, D3D9 top-left rule with pixel centres on integer coordinates, parallelograms for wide lines/sprites, and a soft-floor UV snap for nearest sampling. Textures are unswizzled VRAM->image by compute (specialization constant per PSM, REGION_REPEAT resolved there), CLUT updates batched through a 1024-snapshot ring, textures bindless, mip level from log2 of interpolated Q. Shading uses one ubershader keeping tile pixels in registers: lazy shading (only if blend/ATE/mask needs the old pixel), early-Z unless a pending Z write could change the result, waterfall loops to scalarise state; because the framebuffer is in SSBO/registers "programmable blending is effectively free" and DATE/PABE/FBA/partial discard are "awkward on normal APIs but workable here". SSAA keeps N sample copies plus a single-sampled reference (8x => 10 VRAM copies); the reference detects a game overwriting an RT by copy so super-samples are discarded and splatted; sprites are forced single-rate so UI is never blurred or sampled outside its rect; flat primitives (same Q and Z) interpolate at the single-rate pixel centre with a UV bounding box; resolve happens per tile with clustered subgroup adds. The CRTC scans out the single-sampled copy; games that downsample progressive output into fields produce interlacing artifacts, and 640x224 field-rendering games "are difficult". Performance: MGS2 at 16x SSAA (= 4x upscale) at ~17% of an RX 7600; Shadow of the Colossus beats PCSX2 (24 fps at 2x, max blending) while at 8x SSAA and CPU-bound on geometry. README requirements: descriptor indexing, timeline semaphores, 8/16-bit storage, shaderInt16, full subgroup support with sizes 16-64; not bit-exact in varying interpolation; its AA1 is "questionable"; weave deinterlacer and CRTC field blending built in; tested on RTX 4070/3060, RX 6800/7600, Arc, Deck.

### 2. PCSX2 GS/HW: what a render-target-cache design costs
Sources: tfx.fx and convert.fx (https://raw.githubusercontent.com/PCSX2/pcsx2/master/bin/resources/shaders/dx11/), GSRendererHW.cpp, GSTextureCache.h, GSDevice11.cpp (https://raw.githubusercontent.com/PCSX2/pcsx2/master/pcsx2/GS/Renderers/), the channel-shuffle post (https://pcsx2.net/blog/2016/channel-shuffle-effect), the RT/HDR article (https://www.readonlymemo.com/pcsx2-render-target-hdr-performance-graphical-fixes) and the 2.8 blog (https://pcsx2.net/blog/2026/pcsx2-2.8).
- Texture cache: Source objects (what TEX0 samples; validity one u32 per page, bits = the 32 blocks; may be derived from a Target with a "not durable" pointer; palette object; hash-cache key = TEX0 hash + CLUT hash) and Target objects (RenderTarget | DepthStencil; unscaled size + scale; per-channel validity "valid alpha means we have rendered to the alpha channel"; alpha min/max; dirty rects for CPU uploads; readbacks_since_draw). Lookups by BP/BW/PSM with overlap search and "RT in RT" (SurfaceOffset: a texture at an offset inside a target), which took "a couple of months of whack-a-mole" to land. Invalidation runs both ways (InvalidateVideoMem on CPU writes, InvalidateLocalMem on GPU draws => readback). The 2.8 changelog still lists wrong-target picks, single-pixel overlaps, zero-height targets and shrinking depth targets as fresh fixes: the heuristic layer never converges.
- Reinterpretation shaders (convert.fx): RGBA8<->uint, RGB5A1<->uint, depth<->uint(d*2^32), depth->RGBA8/RGB5A1, RGBA8->PSMT8 index layout, CLUT read out of a target, DATM stencil init, "colclip" init/resolve (COLCLAMP=0 wrap emulated in an RGBA16_UNORM target and `& 255` on resolve), RTA alpha correction. Each exists only because targets are typed textures rather than one byte-addressable VRAM.
- Ubershader features (tfx.fx): DATE by stencil, primitive-ID (two passes with MIN blend) or RT-alpha read; SW blend `trunc(((A-B)*C)+D)` reading the RT; HW approximations BLEND_HW1-9 with two render passes; blend_mix with a 124/256 bias; PABE; FBMASK `(C & ~mask) | (RT & mask)`; dither from the 4x4 DIMX at native positions; texture shuffle (16-bit 5551 fields unpacked/repacked in 8-bit channels; with DATE "test the green channel instead of the alpha channel"); channel-shuffle fetches; ATST with EQUAL +-0.5; AFAIL as discard/DISCARD_DEPTH/DISCARD_COLOR/restore-alpha/dual-source alpha; Z = min(z, MaxDepth)*2^-32 with ZCLAMP/ZFLOOR and a SW depth test; depth sampled as RGBA8/RGB5A1; manual bilinear for region clamp/repeat; 8/4HL/4HH palette lookups; AEM/TA0/TA1; manual LOD from K/L; fog `trunc(lerp(fog,c,f*255/256))`; TFX `trunc(C*T/128)`; 16-bit destination `A=0x80, &0xF8`; SCANMSK; ROV variants.
- Blending ladder: Minimum (non-recursive SW only, free since 2.8), Basic, Medium (SW for sprite draws under 100 drawlist items when barriers exist), High, Full, Maximum (always SW). Accumulation blend is HW-safe only for "(Cs - 0)*Something + Cd, or with alpha == 1"; anything else needs an RT read: a barrier (GL/VK), a copy on D3D11 (FeedbackCopyAndBind copies the draw area into a clone SRV; a "full barrier" loops over the draw list copying each entry's bbox then drawing it; "DX11 can't do partial depth copies"), a primitive-by-primitive drawlist ("Sw blending will force the draw to run primitive by primitive"), or ROV. "DATE_PRIMID interact very badly with sw blending"; DATE "ceases to function" on 24-bit targets.
- ROV (2.8; D3D12, D3D11, Vulkan): DetermineROVUsage enables it only when it saves at least two barriers; colour ROV forces depth ROV whenever there is discard, depth output, DATE or alpha test; ConfigureROV switches to SW blend, PABE, SW colclip, shader DATE ("Don't use stencil with ROV"), alpha-test FEEDBACK, and removes regular barriers. The blog: ROV "allows higher blending accuracy levels to incur less of a performance hit" and "works well in cases where the number of barriers would be excessively high (e.g. with AA1 enabled)". Xenia's EDRAM-in-UAV path (https://github.com/xenia-project/xenia/wiki/ROV) reports the same: "much higher performance since there's no expensive data copying" versus an RT path with "unfixable transparency issues".
- AA1 (2.8): "adds a semi-transparent border around triangles... doesn't write depth", emulated through a software-depth path, "disabled by default" for cost; Sly 2 is a showcase game. D3D11 expands lines/points in the vertex shader from a structured buffer (vs_expand).
- Upscaling hacks are symptom patches of rendering at k x native: Half Pixel Offset (Off/Normal/Special/SpecialAggressive/Native/NativeWTexOffset; "adds a 0.5 pixel offset to all surfaces"), Round Sprite (Half = flat sprites, Full = all), Align/Merge Sprite ("paving" sprite grids -> one sprite), Texture Offsets, Native Scaling (detect "scaling draws" and downsample the source target so native-res post effects stay aligned), bilinear-dirty hacks. Comment pitfalls: speckles from large ST coordinates (Ridge Racer V), Snowblind's TW/TH=1024 with UV sub-rects, degenerate triangles, scissors into tiny targets, FMV blits past the RT.
- Shuffles: GT4's brightness pass was ~1500 of ~2250 draws/frame of 64x32 sprites reading the 32-bit frame as PSMT8 through a CLUT; GSdx HLE'd it per game pattern (Urban Chaos, Tales of Abyss, MGS3, Terminator 3, Tekken 5...). Texture shuffle = 16-bit sprites re-reading a 32-bit target with FBMSK 0xFFFF; one class is still "handled with a CRC hack". A VRAM-addressed renderer needs none of this detection.
- Readbacks: modes Enabled/ForceFull/NoReadbacks/Unsynchronized/Disabled; 2.8 added one consolidated download because "some games issue multiple small, partial GPU-to-CPU readbacks per frame".
- D3D11 device: FL 11_1 -> 11_0 -> 10_0; no texture barrier or FB fetch, so every RT read is a copy; `rov = typed UAV loads && ROVsSupported && UAV support for every write format`; stencil and PrimID DATE; colclip target R16G16B16A16_UNORM when blendable; "OM targets and UAVs share the same namespace in DX11". 2.8 now calls D3D11 "the least preferred renderer".

### 3. Play! (Vulkan): a GPU "software renderer" through fragment-shader interlock
https://raw.githubusercontent.com/jpd002/Play-/master/Source/gs/GSH_Vulkan/GSH_VulkanDrawDesktop.cpp: GS memory is one coherent storage buffer; the GPU rasterizer only produces fragments and the fragment shader runs the whole GS pixel pipeline (swizzled address via lookup images, dst colour/depth read, ZTST, ATE, DATE, `((A-B)*C >> 7) + D`, COLCLAMP or `& 255`, FBA, write mask, PSM32/16 pack) between BeginInvocationInterlock/EndInvocationInterlock (VK_EXT_fragment_shader_interlock = ROV), with a fragment->fragment barrier between draws; texels and the CLUT come from the same buffer; a second copy of GS RAM serves texture reads because "some games use tiny sprites to do full screen effects". Native resolution only. It proves VRAM-as-UAV plus ROV is viable on consumer GPUs and shows the cost of serialising every fragment.

### 4. Native-port renderers: RT64 and Unleashed Recompiled
RT64 (https://github.com/rt64/rt64): draws are recorded per frame, not issued immediately; framebuffer addresses, sizes and GPU contents are tracked and a later sample becomes a GPU copy only "when the result would match a RAM-sourced sample apart from precision"; per-case compute shaders reinterpret formats; a dual renderer keeps a native-resolution copy in sync with RAM for CPU readbacks while a high-resolution renderer replays the draws; ubershaders "to guarantee no stutters"; widescreen is any aspect plus an extended command set (gEX*) injected by the port via NOOPs; "no tolerance for game-specific workarounds"; frame interpolation by draw matching is "highly experimental". Zelda64Recomp pins the HUD to 16:9 on ultrawide. Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp): "A new renderer was written from scratch" on D3D12 (SM6)/Vulkan, bindless and shader specialisation with pipelines compiled during asset loading, resolution scale, MSAA beyond the original 2x, alpha-to-coverage, precise reverse-Z, ultrawide with UI pinned to 16:9 or the edges, cutscenes "locked to their original aspect ratio" because unlocking "causes visual oddities". Both treat widescreen and HUD as game-code patches, not renderer heuristics.

### 5. Sly 2 / Sly 3 specifics
- GameDB (GameIndex.yaml): Sly 2 (SCUS-97316) and Sly 3 `halfPixelOffset: 5 # fixes chromatic effect` (Align to Native with texture offset); Sly 1 `halfPixelOffset: 5 # fixes post alignment` and `autoFlush: 1 # fixes Sly's shadow definition` (flush when a draw samples the target it writes). The colour-separation/motion-blur post effect samples the frame RT and is only aligned at native texel positions.
- Shadows: the RT/HDR article names Sly Cooper 2 and 3 among the few games whose colours are "masked/looped instead of clamped to 0-255" (COLCLAMP=0); PR https://github.com/PCSX2/pcsx2/pull/14945 tests "Sly 2 on GL, make sure shadows work on newer hardware... For older hardware Sly 2 has precision issues" (RGBA16_UNORM vs FP16 blending). So the shadows depend on exact 8-bit wrap-around blends. A 2022 report (https://forums.pcsx2.net/Thread-Some-problems-with-the-latest-dev-build?pid=624445): Sly 2/3 flicker on Vulkan and it "gets worse as you increase Blending Unit... it does have to do with shadows"; on D3D11 they "need GPU Palette Conversion or else lots of the ground is missing and most objects become black and fragmented"; GPU spikes at certain camera angles.
- Outlines: https://github.com/PCSX2/pcsx2/issues/2964 reports the trilogy's cel-shading "black lines" breaking when upscaled; gregory38: "I miss some barriers because I was careless for point and line" and "unscaling... might overlap pixels that wouldn't in the first place". Wiki excerpts (the page itself is bot-walled): "because PCSX2 does not implement edge antialiasing in Hardware mode, many of the outlines are jagged"; HW mode shows "vertical blue lines"; "screen shaking without Blend bff"; one tester needed "TC offset X 280, Y 580" against ghosting at 3x; software mode recommended. The episode-2 photo mission misdetects under D3D11 or widescreen (https://forums.pcsx2.net/Thread-Zoom-issues-with-Sly-2?pid=571997).
- Widescreen (https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/SCUS-97316_07652DD9.pnach): one projection constant at 0x14D750/54 (`lui/ori $at` 0x3FA6xxxx -> 0x3FDDA523, a x4/3 change, in our f_0014D6D8), a hook at 0x14F370 (f_0014F308) multiplying $f4 by 0.75, a hook at 0x1BE808 (f_001BE6F0) scaling 2D portraits by 5440/4096 = 4/3; plus "No-Interlacing" (0x240B4C in f_00240AE0 := 0) and "No Motion Blur" (0x160FA0 in f_00160F98 := 0). The game has no native 16:9, the HUD is left stretched, and the motion blur is a frame-feedback effect.

### 6. D3D11 capability facts
ROVs are a D3D11.3 feature (D3D11_FEATURE_D3D11_OPTIONS2.ROVsSupported; pixel shader only; ordering only among overlapping invocations of the same draw) https://learn.microsoft.com/en-us/windows/win32/direct3d11/rasterizer-order-views. Typed UAV loads: D3D11.0 guarantees only R32_FLOAT/UINT/SINT; RGBA32/RGBA16/RGBA8/R16/R8 are all-or-nothing behind TypedUAVLoadAdditionalFormats; B5G5R5A1/B5G6R5/R16_UNORM are per-format https://learn.microsoft.com/en-us/windows/win32/direct3d11/typed-unordered-access-view-loads. RTX 40 is feature level 12_2 (ROV and typed loads required from 12_0/12_1) https://en.wikipedia.org/wiki/Feature_levels_in_Direct3D. D3D11 shaders are *_5_0 only; SM 5.1 and wave intrinsics are "introduced with D3D12" https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/shader-model-5-1: cs_5_0 has groupshared and interlocked ops but no subgroup ballot/shuffle/clustered add, no bindless (128 SRV slots), no 16-bit storage types.

### Lessons that constrain our design
1. No render-target cache. PCSX2's Source/Target/offset/shuffle heuristics still receive correctness fixes after 20 years, need per-game entries (Sly needs two), and every reinterpretation is its own shader. Keep the single swizzled 4 MB VRAM as the source of truth (software GS, paraLLEl-GS, Play!, Xenia-ROV all do) and address it from shaders with the GS swizzle; R32_UINT buffers are always UAV-loadable on D3D11, so pack 16/8-bit formats ourselves.
2. The GS pixel pipeline runs in the shader, in GS order, in 8-bit integer arithmetic: wrapping COLCLAMP, PABE, FBA, FBMSK, DATE, ATST+AFAIL partial writes, DTHE at native positions, 24/16-bit Z clamp. Sly 2's shadows (COLCLAMP=0, flicker with PCSX2 blend levels, RGBA16 precision) and outlines are exactly what PCSX2 still needs ROV/barriers/HDR for; fixed-function blending cannot express them.
3. Two sound D3D11 shapes: (a) a compute tile rasterizer in the paraLLEl-GS mould, rewritten for cs_5_0 (groupshared atomics/prefix sums instead of subgroup ballots, no bindless, fences instead of timeline semaphores); or (b) GPU-rasterized draws with ROV pixel shaders running the GS pixel pipeline into the VRAM UAV (Play!/Xenia shape; RTX 40 supports ROV and PCSX2 2.8 uses it on D3D11). (b) is simpler, but ROV ordering is per draw, so draw boundaries become the hazard unit and overlapping draws need UAV barriers between them; (a) batches hundreds of draws per pass with page-level hazard tracking and gets SSAA structurally.
4. Upscale by super-sampling on the native integer grid, never by rendering at k x native. Every PCSX2 hack (half-pixel offset, round/align/merge sprite, texture offsets, native scaling, Sly's "chromatic effect" and TC-offset ghosting, broken cel lines) comes from moved pixel centres and texel footprints. Copy paraLLEl-GS's rules: extra samples per native pixel, sprites and flat primitives single-rate at the native centre with a UV bounding box, a single-sampled reference copy so copies/readbacks/CLUT reads/post effects see native data, resolve only for display. Sly 2's feedback effects (motion blur, chromatic separation, RT-reading shadows, CLUT-in-VRAM) then stay aligned by construction.
5. Hazard tracking at page + 256-byte block granularity with separate read/write and FB-write/texture-read masks is the proven minimum, and our software GS already computes page ownership and RAW/SNAP/HOLD batches; the GPU backend should reuse those decisions and add only the "copy over sampled texture" vs "sample the current RT" distinction.
6. Feedback (texture == frame) is per primitive: if UV minus FB position is within a fixed range, read the in-flight pixel (ROV/on-tile); REGION_CLAMP textures are checked against their clamp rect; otherwise snapshot. PCSX2's autoflush/clone copies and Play!'s second RAM copy are the expensive alternatives; Sly 1's autoflush entry says Sly's shadows take this path.
7. Lines/AA1 are first-class: Sly's outlines are AA1 lines whose coverage replaces alpha and which do not write depth. Width stays one native pixel at any sample count, coverage comes from the GS line rule (our DDA: two pixels across the minor axis) and must not be double-blended under SSAA. Rasterize lines single-rate with analytic coverage even inside an SSAA design.
8. Readbacks and CRTC: keep a native-resolution VRAM copy that is always correct for TRXDIR=1 reads, CLUT loads and DISPFB scan-out (RT64 keeps a whole native renderer for this). Sly 2 is field-rendered: blend/weave fields from native VRAM, and prefer running the game progressive through the 0x240B4C switch over deinterlacing a high-res frame.
9. Decode textures on the GPU: Sly 2 needing PCSX2's "GPU Palette Conversion" shows it re-palettises too often for CPU-side CLUT expansion; sample indices from VRAM and apply CLUT snapshots in-shader.
10. One ubershader with uniform per-batch branching (RT64, paraLLEl-GS, PCSX2 all do); D3D11 has no pipeline-stutter problem but has 128-SRV/8-UAV limits, so VRAM-as-buffer plus a small decoded-texture cache is the binding model.
11. Widescreen and HUD belong in game code: patch f_0014D6D8 (projection X), f_0014F308 and f_001BE6F0 natively, keep the HUD at 4:3 inside the 16:9 frame (Zelda64Recomp/Unleashed pin UI to the safe area), and leave GS scissor/FRAME state alone.
12. Budget: paraLLEl-GS does 16x SSAA at 17% of an RX 7600; PCSX2 D3D11 at Maximum blending collapses to per-primitive copies; Play! serialises every fragment. Only a hazard-batched compute or ROV design is both exact for Sly 2's blend tricks and fast enough for 16.7 ms on an RTX 4060 Ti, consuming the existing gs_write_reg/gs_image_write stream and gs_draw_pixel.c batches unchanged.