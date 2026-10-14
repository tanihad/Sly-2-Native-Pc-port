# WIP: native 16:9 widescreen (branch next/widescreen)

**Goal**: runtime aspect setting (16:9 default, 4:3 bit-identical), EE patches for projection /
culling / 2D, HUD hints via GS reg 0x7F, sw-renderer HUD transform, 16:9 presentation, settings file.

## Done
- (nothing yet) checkpoint file created.

## In progress
- Reading design (gpu-integ notes/d3d11-renderer.md 8.3, 13; prior-art 5), recomp overrides/hooks,
  runtime gs/window.

## Next
1. Locate projection (0x14D750), culling (0x14F370), portraits (0x1BE808) in disasm/recomp output.
2. Find other aspect-dependent code (frustum, binocucom, shadows, particles, HUD).
3. runtime/aspect.c + settings file + PS2_ASPECT.
4. EE hooks/overrides.
5. HUD hints (0x7F) + sw transform + window presentation.
6. Tests, verification frames.

## Findings / traps
