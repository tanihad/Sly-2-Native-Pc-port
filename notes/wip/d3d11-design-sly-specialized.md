# D3D11 renderer design: Sly-2-specialised hybrid (designer checkpoint)

Status: measurements done (probe below), design written. Inputs: d3d11-map.md, d3d11-census.md,
d3d11-prior-art.md, census reports in build/census, plus two new probes run this session.

## New measurements (this run; probes in the session scratchpad, read .gsrec files in place)

1. Shadow counter ops (rope, fix-perf brief, ep6 rc_battle):
   - G init: FBMSK FFFF00FF, ABE=0, flat G = 126, 16 sprites (ZTST ALWAYS).
   - Volumes: FBMSK FFFF00FF, flat (IIP=0) tristrips, (0-Cs)*128+Cd with G = 2 or 254 (i.e. -2 / +2
     mod 256), ZTST GREATER (rope) and (Cs-0)*128+Cd with G = 2/254 under ZTST ALWAYS (Binocucom).
     DATE=1. Steps are always +-2: bit 0 of the counter never changes through the volumes.
   - Bit-plane steps (brief, rc_battle): FBMSK FFFF80FF ABE=0 G=62 (write G bits 0-6 := 62), then
     FBMSK FFFFFEFF (Cd-0)*FIX2>>7 (G bit 0 := G bit 6). 1312 kicks each in rc_battle.
2. Post-pass texel alignment (all FST sprites): UV - XY = (+0.5, +0.5) for the darken (T8H) and
   copies, (+0.5, +1.0) with bilinear for the motion blur (TEX1 0x61), (-7.5/+8.5, +0.5) for the
   CT16 shuffles. Sly samples texel centres: pixel x reads u = x + 0.5.
3. XYOFFSET in rope is constant (1792, 1936): no per-field half-line jitter in gameplay.
4. Blend alpha above 1.0 (probe_alpha: CLUT max alpha x vertex alpha, CLUT contents tracked
   through CT32 uploads), rope:
   - Pass A (Cs-Cd)*As+Cd, ATST GEQUAL 126: vertex A = 128 and CLUT max 128 in 94%; As can exceed
     128 on 0.4% (T4) / 5.4% (T8) of vertex kicks (CLUTs with alpha 245).
   - Pass B (Cs-0)*As+Cd, ATST NEVER: vertex alpha 0..127 (lighting weight); >128 possible on <1%.
     Additive forms are exact in fixed function when the shader premultiplies (see 4.2).

## Design

(the full design is in the StructuredOutput deliverable; summary of the decisions)
- Seam A, GS thread translates draws into an own command stream; a render thread owns the D3D11
  immediate context, swapchain and present.
- Fixed-function D3D11 for the common path (RGBA8 targets at kx*ky scale, D32S8 depth, hardware
  Z, FF blend with shader-side premultiply), exact copy-blend tier for As>128 Cd-dependent blends.
- Recognisers: tiled fills -> clears, CT16-on-CT32 "channel-move compiler" (derived from swizzle
  math, not hand-written), green-stencil virtualisation (G counter in the stencil buffer, rotated
  by n where the step is +-2^n; bit-plane ops as masked REPLACE/test), RT-as-texture for
  layout-compatible views (T8H darken, CT32 motion blur, Binocucom RTT).
- Everything else: exact fallback via native-resolution download -> software rasteriser -> stale
  page upload. Verify mode and gs_replay -Backend both for per-frame diffs.
- Coordinate convention d3d = (gs + 0.5) * k (centred sub-samples), chosen because Sly samples
  texel centres (measurement 2).
