// (tex) Enhanced textures in the pixel shaders (R2_ETEX in r2.hlsl, ETEX in mft.hlsl; runtime/gpu/gpu_tex.h): a texture
// with an enhanced image (upscaled, mip-mapped: shaders/tex.hlsl) is sampled from it with trilinear + anisotropic
// filtering instead of the GS's 4-bit bilinear of level 0. The image is found through et_tab by the decoded base the
// material / StateRec already carries (gs.hlsl St.dec): entry (dec - 1) / 64 holds (dec, class | slice << 3), valid only
// when its first word equals dec. Everything after the texel (TFX, fog, alpha test, blend) is unchanged. Only these
// permutations contain this code: the Original pipelines are untouched.
ByteAddressBuffer et_tab : register(t30);
Texture2DArray<float4> et_t0 : register(t31);   // classes: square slices of 64 << class texels, RGBA8 GS bytes
Texture2DArray<float4> et_t1 : register(t32);
Texture2DArray<float4> et_t2 : register(t33);
Texture2DArray<float4> et_t3 : register(t34);
Texture2DArray<float4> et_t4 : register(t35);
Texture2DArray<float4> et_t5 : register(t36);
SamplerState et_sw : register(s4);               // REPEAT: wrap, trilinear, anisotropic
SamplerState et_sc : register(s5);               // CLAMP
cbuffer et_ps_cb : register(b8) {
    float4 e_ps;                                 // gradient scale (2^lod bias), split x (pictures: Enhanced right of it), -, -
};

float4 et_class(uint k, bool cl, float3 c, float2 dx, float2 dy) {
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    [branch] if (cl) {
        [branch] if (k == 0u) v = et_t0.SampleGrad(et_sc, c, dx, dy);
        else if (k == 1u) v = et_t1.SampleGrad(et_sc, c, dx, dy);
        else if (k == 2u) v = et_t2.SampleGrad(et_sc, c, dx, dy);
        else if (k == 3u) v = et_t3.SampleGrad(et_sc, c, dx, dy);
        else if (k == 4u) v = et_t4.SampleGrad(et_sc, c, dx, dy);
        else v = et_t5.SampleGrad(et_sc, c, dx, dy);
    } else {
        [branch] if (k == 0u) v = et_t0.SampleGrad(et_sw, c, dx, dy);
        else if (k == 1u) v = et_t1.SampleGrad(et_sw, c, dx, dy);
        else if (k == 2u) v = et_t2.SampleGrad(et_sw, c, dx, dy);
        else if (k == 3u) v = et_t3.SampleGrad(et_sw, c, dx, dy);
        else if (k == 4u) v = et_t4.SampleGrad(et_sw, c, dx, dy);
        else v = et_t5.SampleGrad(et_sw, c, dx, dy);
    }
    return v;
}

// the enhanced texel (0xAABBGGRR, GS alpha) at normalized (S/Q, T/Q) uv with its screen derivatives; false: none (the
// caller samples the original way). tc: TEX0 lo, hi, CLAMP lo, hi; tlev: the state's texm (bits 0..1: 2 = bilinear)
bool et_sample(uint dec, uint4 tc, uint tlev, float2 uv, float2 dx, float2 dy, float px, out uint t) {
    t = 0u;
    if (dec == 0u || (tlev & 3u) != 2u || px < e_ps.y) return false;
    uint2 e = et_tab.Load2(((dec - 1u) >> 6) * 8u);
    if (e.x != dec) return false;
    uint wms = tc.z & 3u, wmt = (tc.z >> 2) & 3u;
    if (wms > 1u || wmt > 1u) return false;      // (REGION_CLAMP / REGION_REPEAT: the original way)
    bool cl = (wms | wmt) != 0u;
    if (cl) {                                    // (a REPEAT axis under the clamp sampler: wrapped here; the derivatives
        if (wms == 0u) uv.x = frac(uv.x);        //  stay those of the unwrapped coordinate)
        if (wmt == 0u) uv.y = frac(uv.y);
    }
    float4 v = et_class(e.y & 7u, cl, float3(uv, (float)(e.y >> 3)), dx * e_ps.x, dy * e_ps.x);
    uint4 b = (uint4)(saturate(v) * 255.0 + 0.5);
    t = b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
    if (e_ps.z == 1.0) t = (t & 0xFF000000u) | 0x000000FFu;   // (PS2_TEX_DBG=1: enhanced texels red)
    else if (e_ps.z == 2.0) t = (t & 0xFF000000u) | (0x10101u * (8u << min((uint)(e.y & 7u), 4u)));   // (2: the class)
    return true;
}
