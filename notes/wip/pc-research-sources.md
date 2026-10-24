# pc-research: web research takeaways (2026-10-23, two research agents; sources per bullet)

Unverified items marked [unverified]. dolphin-emu.org and rpcs3.net blocked the fetcher (403).

## Recompilation / emulator renderers
- RT64: ubershaders "to guarantee no stutters"; RSP vertex work (transform, lighting, texgen) on GPU compute by HLE of
  known display-list semantics (not microcode translation); TMEM decoded by compute; per-frame draw structure replayed.
  Interpolation: draw calls matched across frames, transforms decomposed / interpolated / re-run through the GPU RSP step;
  game patches tag transforms (gEXMatrixGroup ids) to avoid heuristic matching. D3D12 / Vulkan / Metal.
  https://github.com/rt64/rt64 , https://raw.githubusercontent.com/rt64/rt64/main/include/rt64_extended_gbi.h
- Zelda64Recomp: high fps = RT64 interpolation, game tick unchanged. https://github.com/Zelda64Recomp/Zelda64Recomp
  Cheap because transforms are deferred to the GPU: an extra frame only re-patches matrices.
- Unleashed Recompiled: renderer translates the game's draw calls (no Xenos emulation), plume (D3D12/Vulkan), bindless,
  pipelines compiled during asset load + fixed list at boot; XenosRecomp shaders AOT; high fps is a real higher tick with
  many patches; "modern CPUs usually single-thread bound before the GPU".
  https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md , https://cdn.jsdelivr.net/gh/hedge-dev/XenosRecomp@main/README.md
- PCSX2 MTVU: VIF1 commands into an ordered ring, VU1 on its own thread, GIF gets a placeholder PATH1 packet; weakness:
  SIGNAL/FINISH/LABEL from PATH1 (stays a speedhack). https://pcsx2.net/blog/2011/threading-vu1 ; MTGS diagnosis
  https://pcsx2.net/blog/2010/benchmarking-multithreaded-pcsx2 ; texture-cache targets https://wiki.pcsx2.net/GSDX
- Themaister (paraLLEl-GS): PCSX2 at max blend accuracy needs a barrier per primitive (MGS2 at 4x "murders" the GPU);
  paraLLEl-GS does setup, binning, raster, shading, unswizzle, CLUT on GPU compute (32x32 bins, <= 64k prims per pass,
  games 10-30k in the main pass); CPU keeps geometry + hazard tracking (8 KiB pages, block bitmasks) and is the
  bottleneck (SotC at 8x SSAA CPU-bound); MGS2 16x SSAA ~17% of an RX 7600.
  https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation
- paraLLEl-RDP: native pass first unblocks the CPU, upscaled passes async; RSP stays on a CPU dynarec.
  https://www.libretro.com/index.php/parallel-rdp-how-the-upscaled-rendering-works/
- Dolphin: hybrid ubershaders (specialised shaders compiled in background); exclusive ubershaders "incredibly
  demanding" at high res. https://forums.dolphin-emu.org/Thread-exclusive-vs-hybrid-ubershaders ; SSE vertex loader
  35 -> 100 fps in one game; most "GPU-bound" games limited by EFB copies.
  https://forums.dolphin-emu.org/Thread-attention-developers?pid=351404 ; Fast Depth [unverified]: GPU depth instead of
  GC-exact depth in the pixel shader.
- Xenia: ROV path loses early-Z / Hi-Z; GTX 1070 GPU ms old RT / new RT / ROV: Halo 3 34.1 / 13.6 / 23.8, Banjo 31.6 /
  17.5 / 59.4; shader depth writes "severe" cost; conservative SV_DepthGreaterEqual keeps coarse early testing.
  https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html
- RPCS3: 2017 vertex rewrite moved vertex decode/unpack to GPU shaders (CPU prep up to 200 ms/frame; CPU threads for it
  starved other threads) https://blog.rpcs3.net/2017/08/ (snippet); MT RSX offloader 5-10% in some games, off by
  default https://github.com/RPCS3/rpcs3/pull/6112
- VU microcode on the GPU: no precedent found. RT64 = HLE of semantics; paraLLEl-RDP keeps RSP on CPU; paraLLEl-GS keeps
  geometry on CPU; PCSX2 threads VU1 on CPU. PS4's PS2 emulator reportedly swaps known VU1 programs by hash for optimised
  CPU replacements https://www.psdevwiki.com/ps4/Talk:PS2_Emulation [unverified].

## APIs, NVIDIA, Windows
- D3D11: DrawIndexedInstancedIndirect exists; no core multi-draw-indirect; NVAPI NvAPI_D3D11_MultiDrawIndexedInstancedIndirect
  exists https://docs.nvidia.com/nvapi/group__dx.html . 3DMark API overhead (Titan X): ~1.3M draws/s D3D11 vs ~6.6M D3D12.
- SV_Depth disables early-Z; conservative depth keeps it when direction matches (GTX 600 anecdote; Xenia). Our M9 measured
  NO early rejection with SV_DepthLessEqual on the 4060 Ti (d3d11-renderer.md appendix L) - direction/mode to re-check.
- Reflex via public NVAPI (MIT, https://github.com/NVIDIA/nvapi): NvAPI_D3D_SetSleepMode / Sleep / SetLatencyMarker take
  IUnknown* device (D3D11 OK); latency, not fps; minimumIntervalUs = driver frame cap.
  https://docs.nvidia.com/nvapi/struct__NV__SET__SLEEP__MODE__PARAMS.html
- DLSS SR on D3D11 via NGX; needs motion vectors + jitter + depth; DLSS FG requires DX12 (Streamline page:
  "DirectX 12 is required for DLSS Frame Generation") https://developer.nvidia.com/rtx/streamline/get-started ; license
  proprietary (notify NVIDIA, attribution) https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt
- NVIDIA Smooth Motion: driver frame generation, RTX 40 supported now, DX11/DX12/Vulkan, per-program in the NVIDIA app,
  adds latency. https://www.nvidia.com/en-us/geforce/news/nvidia-app-global-dlss-overrides-rtx-40-series-smooth-motion/
- Low Latency Mode (DX11): driver queue control; Reflex supersedes. G-SYNC 101: G-SYNC + V-SYNC + cap >= 3 below refresh.
  https://www.blurbusters.com/gsync/gsync101-input-lag-tests-and-settings/14/
- NVAPI DRS app profiles (Godot sets Threaded Optimization this way) - acceptable for the game's own exe, opt-in.
- Flip model + waitable object + ALLOW_TEARING https://devblogs.microsoft.com/directx/dxgi-flip-model/ ; timeBeginPeriod
  per-process since 2004; high-resolution waitable timer.
- Hybrid CPUs: Intel guide - CPU sets (SetThreadSelectedCpuSets, EfficiencyClass) not hard affinity; HighQoS opt-out of
  EcoQoS; SMT siblings last. https://www.intel.com/content/www/us/en/developer/articles/guide/12th-gen-intel-core-processor-gamedev-guide.html
