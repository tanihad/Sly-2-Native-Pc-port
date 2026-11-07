// (ai) Reimagined textures: per-texture surface maps (normal + height + cavity) for the lighting (runtime/gpu/
// gpu_tex_normals.h, notes/reimagined.md). Include after etex.hlsli: the image is found exactly like et_sample's
// (et_tab by the decoded base), the normal class has the colour slice's index at half its resolution.
Texture2DArray<float4> etn_t0 : register(t40);  // normal classes (32 << class texels): r, g normal x, y (0.5 + 0.5 n),
Texture2DArray<float4> etn_t1 : register(t41);  // b height, a cavity
Texture2DArray<float4> etn_t2 : register(t42);
Texture2DArray<float4> etn_t3 : register(t43);
Texture2DArray<float4> etn_t4 : register(t44);
Texture2DArray<float4> etn_t5 : register(t45);
ByteAddressBuffer etn_ok : register(t46);       // class * 4096 + slice: 1 = the slice has a map

float4 etn_class(uint k, bool cl, float3 c, float2 dx, float2 dy) {
    float4 v = float4(0.5, 0.5, 0.5, 1.0);
    [branch] if (cl) {
        [branch] if (k == 0u) v = etn_t0.SampleGrad(et_sc, c, dx, dy);
        else if (k == 1u) v = etn_t1.SampleGrad(et_sc, c, dx, dy);
        else if (k == 2u) v = etn_t2.SampleGrad(et_sc, c, dx, dy);
        else if (k == 3u) v = etn_t3.SampleGrad(et_sc, c, dx, dy);
        else if (k == 4u) v = etn_t4.SampleGrad(et_sc, c, dx, dy);
        else v = etn_t5.SampleGrad(et_sc, c, dx, dy);
    } else {
        [branch] if (k == 0u) v = etn_t0.SampleGrad(et_sw, c, dx, dy);
        else if (k == 1u) v = etn_t1.SampleGrad(et_sw, c, dx, dy);
        else if (k == 2u) v = etn_t2.SampleGrad(et_sw, c, dx, dy);
        else if (k == 3u) v = etn_t3.SampleGrad(et_sw, c, dx, dy);
        else if (k == 4u) v = etn_t4.SampleGrad(et_sw, c, dx, dy);
        else v = etn_t5.SampleGrad(et_sw, c, dx, dy);
    }
    return v;
}

// the surface map at (S/Q, T/Q) uv (arguments as et_sample's). m.xy: tangent normal x, y (-1..1; z = sqrt(1 - x^2 - y^2)),
// m.z: height 0..1, m.w: cavity 0..1. false: none (use a flat normal, height 0.5, cavity 1)
bool etn_sample(uint dec, uint4 tc, uint tlev, float2 uv, float2 dx, float2 dy, out float4 m) {
    m = float4(0.0, 0.0, 0.5, 1.0);
    if (dec == 0u || (tlev & 3u) != 2u) return false;
    uint2 e = et_tab.Load2(((dec - 1u) >> 6) * 8u);
    if (e.x != dec) return false;
    uint k = e.y & 7u, slice = e.y >> 3;
    if (k > 5u || etn_ok.Load((k * 4096u + slice) * 4u) == 0u) return false;
    uint wms = tc.z & 3u, wmt = (tc.z >> 2) & 3u;
    if (wms > 1u || wmt > 1u) return false;
    bool cl = (wms | wmt) != 0u;
    if (cl) {
        if (wms == 0u) uv.x = frac(uv.x);
        if (wmt == 0u) uv.y = frac(uv.y);
    }
    float4 v = etn_class(k, cl, float3(uv, (float)slice), dx * e_ps.x, dy * e_ps.x);
    m = float4(v.xy * 2.0 - 1.0, v.z, v.w);
    return true;
}
