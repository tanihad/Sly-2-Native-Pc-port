# WIP: delivery-lens judgement of the three D3D11 designs

Status: done (verdict returned to the orchestrator).

Verified on this machine (scratch probe, not committed):
- d3dcompiler_47 (System32) compiles `RasterizerOrderedTexture2D<uint4>` in ps_5_0 and a cs_5_0
  kernel with groupshared/InterlockedOr/firstbitlow.
- RTX 4060 Ti D3D11 device: FL 11_1 requested -> 0xb100, OPTIONS2.ROVsSupported=1,
  TypedUAVLoadAdditionalFormats=1, doubles=1.
- scripts/gs_replay.c uses only public GS entry points; a backend switch fits there.

Scores (delivery lens): high-level 7, gpu-exact 5, sly-specialised 5.5.
Recommendation: design 1 (high-level) as backbone; sequence an exact ROV/ROV_DEPTH path first,
FF as an optimisation gated by lockstep; graft design 3's all-fallback M0 harness, vram_buf direct
sampling (atlases later), render-thread command chunks, tiled-fill->clear; graft design 2's
VRAM-hash lockstep with bisection to the first diverging draw and WARP runs. Keep design 2 as the
fallback architecture if the M2 submission gate fails. Do not change the software renderer's
arithmetic.
