# WIP: synthesis of the final D3D11 renderer design

Output: notes/d3d11-renderer.md (on next/d3d11).

## Status
- [x] Inputs read (map, census, prior art, 3 designs, 3 judgements, native-port-principles.md,
      gs_draw.c Z/attribute arithmetic: rowstep_init/rowstep_start, draw_line).
- [ ] notes/d3d11-renderer.md written and committed.

## Decision (fixed; do not re-litigate on resume)
- Backbone: design 1 (draws -> D3D11, GPU render targets at scale, generic per-draw exact paths
  ROV / ALIAS16 / target views). Design 2 rejected as the architecture: it is a GS emulator on
  compute (contradicts native-port-principles "GS primitives -> D3D11 draw calls", "no pixel-exact
  GS emulation"), rewrites the shipped software renderer's arithmetic, nothing usable before its M4,
  9-14 ms GPU at 4x. Kept as the documented fallback architecture if the submission gate fails.
  Design 3's recognisers (virtual G stencil) are not a correctness mechanism; optional perf only.
- Fatal issue found by the fidelity judge (Z precision: Sly Z is 13-17 bits, GS truncates per-pixel
  Z, outlines decided by GREATER ties) is fixed generically: exact integer Z per pixel from a
  per-primitive integer record (same rowstep arithmetic as gs_draw.c, int64 emulated), written as
  SV_Depth with the order-preserving encoding asfloat(z + 0x00800000) into D32 (exact up to
  z = 0x3F000000; larger vertex Z -> reference tier, counted).
- Sample grid: sample (i,j) of native pixel n at GS position n + (i/kx, j/ky) (sample 0 = native
  pixel centre exactly). Downloads/readbacks/lockstep at any scale use sample 0, never averages.
  Presenter compensates the (k-1)/2k px shift.
- Integer kx, ky; display-family targets ky = 2*kx; widescreen kx = round(N*W).
- Tiers per draw: FF (fixed function) | ROV (colour as RasterizerOrdered UAV, Z test in shader via
  read-only DSV + SRV) | GROUPED exact tier (conditional Z write + Cd-dependent non-FF blend: disjoint
  primitive groups) | ALIAS16 (CS) | PUNT (sw at sample 0). REFERENCE mode: every draw ROV with Z
  in an R32_UINT ROV UAV, exact, for bring-up and as an oracle at any scale.
- Sequencing (delivery judge): M0 all-fallback harness with 0-diff gate; hwdry classifier; submission
  benchmark early; reference tier first; FF classes moved one at a time behind lockstep.
- Side-band hints: GS register 0x7F (recorded by gs_record automatically).

## Next
- Write the document; commit; return summary.
