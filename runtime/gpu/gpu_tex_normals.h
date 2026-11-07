/* (ai) Reimagined textures: per-texture height + normal maps for the lighting (the contract: notes/reimagined.md).
 * Included by gpu_tex.h (one translation unit: gpu_exec.c); implemented in gpu_tex_ai.h. Lighting code (gpu_light.h, the
 * R2 / MFT pixel shaders) reads them; nothing here changes Faithful or Retro.
 *
 * What exists: under Look = Reimagined every enhanced image (R2 materials, exact phase-A triangles from the decoded
 * cache: the textures et_sample finds) gets a surface map built with it - from the Enhanced image at first, again from the
 * AI image when that replaces it. Storage parallels the colour classes: class c (colour slices 64 << c) has a normal
 * class of (32 << c) texels per side (half the colour resolution: 2x the PS2 texture's density), same slice index, its
 * own full mip chain, R8G8B8A8_UNORM:
 *     r, g  tangent-space normal x, y as 0.5 + 0.5 n (+x = increasing u, +y = increasing v, i.e. down the image);
 *           z = sqrt(1 - x^2 - y^2) (>= 0, towards the viewer of the texture); flat = (0.5, 0.5)
 *     b     height 0..1 (0.5 = the texture's local mean; dark paint = low: grooves, grout, cracks)
 *     a     cavity 0..1 (1 = open surface, lower in creases: an occlusion term for ambient light)
 * The slope is moderate (PS2_TEX_NRM_K, default 4): scale x, y and renormalize for more or less relief. Mips flatten
 * naturally (each level comes from the colour's own lower level, slopes in level-0 units).
 *
 * Shader side (shaders/etn.hlsli): include it after etex.hlsli and call
 *     bool etn_sample(uint dec, uint4 tc, uint tlev, float2 uv, float2 dx, float2 dy, out float4 m)
 * with the same arguments as et_sample (the decoded base, TEX0 / CLAMP words, the state's filter bits, S/Q T/Q and their
 * screen derivatives): false = no map for this texel (no image, not built yet, Faithful) -> use the flat normal.
 * Registers: t40..t45 (normal classes 0..5), t46 (etn_ok: one word per class x 4096 slices, 1 = valid) - next to
 * Enhanced textures' t30..t36 / s4, s5 / b8, which etn_sample also uses (et_tab, the samplers, e_ps).
 *
 * C side (render thread, any frame after et_frame_begin): */
#ifndef GPU_TEX_NORMALS_H
#define GPU_TEX_NORMALS_H

#define ETN_SLOT0 40                     /* first PS register: t40..t45 normal classes, t46 etn_ok */

/* 1 while this frame's draws can use surface maps (Look = Reimagined, modern frames, the maps exist) */
static int etn_enabled(void);
/* binds t40..t46 for the pixel shader (et_bind_ps does it already whenever etn_enabled(); for other stages or slots use
 * etn_srv) */
static void etn_bind_ps(void);
/* the normal class array of class c (0..5; NULL when absent) and the validity buffer (ByteAddressBuffer, word
 * c * 4096 + slice) */
static ID3D11ShaderResourceView* etn_srv(int cls);
static ID3D11ShaderResourceView* etn_ok_srv(void);

#endif
