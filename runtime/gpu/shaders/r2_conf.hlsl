// (r2-m1) Conformance kernels of the modern renderer (tests/unit/r2/test_r2.c): the exact arithmetic of r2_vu.hlsli run
// on the adapter, one case per pixel of a 256-wide R32G32B32A32_UINT target, the inputs in a raw buffer (t7), compared
// bit for bit with the C reference (runtime/r2/r2_vertex.c r2_vu_*, exact 128-bit rationals).
#include "r2_vu.hlsli"

ByteAddressBuffer cin : register(t7);

float4 r2c_vs(uint id : SV_VertexID) : SV_Position {   // one triangle over the whole target
    float2 p = float2((id << 1) & 2u, id & 2u);
    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}

uint case_of(float4 pos) { return (uint)pos.x + (uint)pos.y * 256u; }

// 16 bytes per case: a, b, w, -  ->  vu_mul(a, b), vu_add(a, b), vu_rcp(w), ITOF-free FMAC chain a * b + w
uint4 r2c_vu_ps(float4 pos : SV_Position) : SV_Target0 {
    uint4 v = cin.Load4(case_of(pos) * 16u);
    float a = asfloat(v.x), b = asfloat(v.y), w = asfloat(v.z);
    return uint4(asuint(vu_mul(a, b)), asuint(vu_add(a, b)), asuint(vu_rcp(w)), asuint(vu_add(vu_mul(a, b), w)));
}

// 32 bytes per case: n (lo, hi), d (lo, hi), s, pos  ->  i64_round_scaled(n, d, s, pos) (lo, hi), floor(n / d) (lo, hi)
uint4 r2c_div_ps(float4 pos : SV_Position) : SV_Target0 {
    uint b = case_of(pos) * 32u;
    uint4 v = cin.Load4(b), u = cin.Load4(b + 16u);
    i64 t = i64_round_scaled(v.xy, v.zw, u.x, u.y != 0u);
    i64 r;
    i64 q = i64_divmod_floor(v.xy, v.zw, r);
    return uint4(t, q);
}
