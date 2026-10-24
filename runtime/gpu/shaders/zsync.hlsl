// Depth of mixed frames (M5, notes/d3d11-renderer.md 3.6, 4.3; runtime/gpu/gpu_exec.c): a depth target
// lives in two places, the DSV (FF draws: D32 holding min(z, 2^20) / 2^20, the order-preserving encoding of
// int64.hlsli, saturated at 1.0) and zpatch (R32_UINT, the stored Z word: ROV runs, ALIAS16 draws, loads, downloads). The
// render thread tracks per tile of 16x16 native pixels which one is newer (ztile: one byte per tile,
// 0 in sync, 1 the DSV is newer, 2 zpatch is newer) and converts only those tiles:
//   ps_zresolve  zpatch -> DSV (the design's ps_zresolve): SV_Depth of the zpatch word, or a constant
//   cs_zsync     DSV -> zpatch: the decoded Z merged into the word (Z24 keeps zpatch's top byte)
#include "int64.hlsli"

cbuffer zs_cb : register(b0) {
    uint kx, ky;                         // samples per native pixel
    uint tiles_x;                        // tiles per row of the target
    uint mode;                           // ps_zresolve: 0 the zpatch-newer tiles, 1 every pixel: the constant `value`
    uint zbits;                          // 1 Z16, 2 Z24, 3 Z32
    uint value;                          // mode 1: the stored Z value
    uint x0, y0, x1, y1;                 // cs_zsync: the rect in samples [x0, x1) x [y0, y1)
    uint2 pad;
};

ByteAddressBuffer ztile : register(t8);

uint tile_state(uint2 s) {
    uint idx = ((s.y / ky) >> 4) * tiles_x + ((s.x / kx) >> 4);
    return (ztile.Load((idx >> 2) * 4u) >> ((idx & 3u) * 8u)) & 255u;
}

uint zmask(uint z) { return zbits == 1u ? z & 0xFFFFu : zbits == 2u ? z & 0xFFFFFFu : z; }

// a quad over the bound depth target (the scissor is the rect)
float4 vs_zquad(uint vid : SV_VertexID) : SV_Position {
    return float4((vid & 1u) ? 1.0 : -1.0, (vid & 2u) ? -1.0 : 1.0, 0.5, 1.0);
}

Texture2D<uint> zp_src : register(t2);

float ps_zresolve(float4 pos : SV_Position) : SV_Depth {
    uint2 s = (uint2)pos.xy;
    if (mode == 1u) return gs_z_encode(value);
    if (tile_state(s) != 2u) discard;
    return gs_z_encode(zmask(zp_src.Load(int3(s, 0))));
}

Texture2D<float> depth_src : register(t0);
RWTexture2D<uint> zp_dst : register(u0);

[numthreads(8, 8, 1)]
void cs_zsync(uint3 id : SV_DispatchThreadID) {
    uint2 s = uint2(x0 + id.x, y0 + id.y);
    if (s.x >= x1 || s.y >= y1) return;
    if (tile_state(s) != 1u) return;
    uint z = gs_z_decode(depth_src.Load(int3(s, 0)));
    if (z >= ZE_ONE) return;             // (P1) saturated: a value >= 2^20 no FF draw wrote (they write < 2^20) - zpatch holds it
    zp_dst[s] = zbits == 2u ? (zp_dst[s] & 0xFF000000u) | (z & 0xFFFFFFu) : z;
}
