// ALIAS16 runs (notes/d3d11-renderer.md 5.2, milestone M4): primitives drawn into a 16-bit frame
// (PSMCT16/CT16S) that lies over a C32 target with the same base and width. 16-bit page n is 32-bit
// page n, so every 16-bit pixel is one halfword of one of the target's 32-bit pixels:
//   cs_a16_gather   staging[x, y] = that halfword (every sample), for the run's rectangle of the frame
//   (the run's draws: shaders/gs.hlsl in 16-bit mode on the staging texture, ROV ordered)
//   cs_a16_scatter  the halfwords back into the target (InterlockedAnd/Or: the two halfwords of a word
//                   belong to different 16-bit pixels, i.e. different threads)
#include "gsmem.hlsli"

cbuffer mem_cb : register(b0) {
    uint n, list_off, data_off, mask_off;
    uint tkind;
    uint tbase;                          // the target's base page
    uint tfbw;                           // its width (64-pixel units) = the frame's
    uint bcols;
    uint kx, ky;                         // samples per native pixel
    uint value, wmask;
    uint cx1, cy1;                       // rect end (exclusive), 16-bit frame pixels
    uint group_base, mpad;
    uint cx0, cy0;                       // rect start
    uint psm16;                          // PSMCT16 or PSMCT16S
    uint mpad2;
};

RWTexture2D<uint> stage : register(u0); // R32_UINT, the frame's pixel space x samples
RWTexture2D<uint4> tgt4 : register(u1);  // the C32 target (R8G8B8A8_UINT: raw GS bytes)

uint tget(uint2 s) { uint4 c = tgt4[s]; return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }
void tput(uint2 s, uint v) { tgt4[s] = uint4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24); }

// the target pixel holding 16-bit pixel (x, y): halfword address -> word -> page and CT32 position in it
uint2 target_pixel(uint x, uint y, out uint hw) {
    uint h = gs_addr16(swz_tab_of(psm16), tbase * 32u, tfbw, x, y);
    hw = h & 1u;
    uint w = h >> 1;
    uint p = ((w >> 11) - tbase) & 511u;
    uint ixy = swz_at(SWZ_ICT32, w & 2047u);
    uint fbw = tfbw > 0u ? tfbw : 1u;
    return uint2((p % fbw) * 64u + (ixy & 63u), (p / fbw) * 32u + (ixy >> 8));
}

[numthreads(8, 8, 1)]
void cs_a16_gather(uint3 id : SV_DispatchThreadID) {
    uint x = cx0 + id.x, y = cy0 + id.y;
    if (x >= cx1 || y >= cy1) return;
    uint hw;
    uint2 c = target_pixel(x, y, hw);
    for (uint j = 0u; j < ky; j++)
        for (uint i = 0u; i < kx; i++)
            stage[uint2(x * kx + i, y * ky + j)] = (tget(uint2(c.x * kx + i, c.y * ky + j)) >> (hw * 16u)) & 0xFFFFu;
}

// one thread per word: 16-bit pixels x and x + 8 of a 16-wide block row share a 32-bit word (the
// rect is whole 16x8 blocks); id.x counts those pairs
[numthreads(8, 8, 1)]
void cs_a16_scatter(uint3 id : SV_DispatchThreadID) {
    uint x = cx0 + (id.x / 8u) * 16u + (id.x & 7u), y = cy0 + id.y;
    if (x >= cx1 || y >= cy1) return;
    uint ha, hb;
    uint2 c = target_pixel(x, y, ha), cb = target_pixel(x + 8u, y, hb);
    for (uint j = 0u; j < ky; j++)
        for (uint i = 0u; i < kx; i++) {
            uint2 s = uint2(c.x * kx + i, c.y * ky + j);
            uint va = stage[uint2(x * kx + i, y * ky + j)] & 0xFFFFu, vb = stage[uint2((x + 8u) * kx + i, y * ky + j)] & 0xFFFFu;
            if (c.x == cb.x && c.y == cb.y && ha != hb) {
                tput(s, (va << (ha * 16u)) | (vb << (hb * 16u)));
            } else {                     // (never: x and x + 8 always share a word)
                tput(s, (tget(s) & ~(0xFFFFu << (ha * 16u))) | (va << (ha * 16u)));
                uint2 sb = uint2(cb.x * kx + i, cb.y * ky + j);
                tput(sb, (tget(sb) & ~(0xFFFFu << (hb * 16u))) | (vb << (hb * 16u)));
            }
        }
}
