# WIP: Enhanced lighting (branch next/light, worktree C:\Users\tanih\Projects\sly2-worktrees\light)

Task: notes/enhanced-lighting.md design; SSAO + bloom; per-pixel lighting for lit meshes (+ rim); softer shadows;
anisotropic. Setting "Lighting: Original / Enhanced" (graphics.lighting), live; env PS2_LIGHT=0/1 + per-feature switches.

Owner decision (via main, 2026-11-05): Enhanced ON by default. Presets: Quality + Balanced = Enhanced, Performance +
Original = Original. Fresh installs (Balanced) get Enhanced. Explicit preset in ini -> follows preset; custom without the
key -> Enhanced. Original must cost nothing (skip all passes) and stay bit-identical.

## Status
- [ ] 1 design note
- [ ] 2 SSAO + bloom
- [ ] 3 per-pixel lighting
- [ ] 4 shadows
- [ ] 5 aniso

## Log
- worktree created from main 7018070.
