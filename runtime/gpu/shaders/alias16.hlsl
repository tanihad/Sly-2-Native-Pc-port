// ALIAS16 runs (notes/d3d11-renderer.md 5.2, milestone M4): primitives drawn into a 16-bit frame
// (PSMCT16/CT16S) that lies over a C32 target with the same base and width. 16-bit page n is 32-bit
// page n, so every 16-bit pixel is one halfword of one of the target's 32-bit pixels:
//   cs_a16_gather   staging[x, y] = that halfword (every sample), for the run's rectangle of the frame
//   (the run's draws: shaders/gs.hlsl in 16-bit mode on the staging texture, ROV ordered)
//   cs_a16_scatter  the halfwords back into the target (the two halfwords of a word are 16-bit pixels x and
//                   x + 8 of a 16-wide block row: one thread writes both)
// (M9) One thread per sample (the threads of a group cover consecutive samples: coalesced), the target through
// its R32_UINT view (one 32-bit access instead of four channels).
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
RWTexture2D<uint> tgt : register(u1);   // the C32 target as R32_UINT (the raw GS word)

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

// id: the sample (x * kx + i, y * ky + j) of the rect, relative to its start
[numthreads(8, 8, 1)]
void cs_a16_gather(uint3 id : SV_DispatchThreadID) {
    uint x = cx0 + id.x / kx, y = cy0 + id.y / ky, i = id.x % kx, j = id.y % ky;
    if (x >= cx1 || y >= cy1) return;
    uint hw;
    uint2 c = target_pixel(x, y, hw);
    stage[uint2(x * kx + i, y * ky + j)] = (tgt[uint2(c.x * kx + i, c.y * ky + j)] >> (hw * 16u)) & 0xFFFFu;
}

// id.x: the sample column of the pair (16-bit pixels x and x + 8 of a 16-wide block row share a 32-bit word; the
// rect is whole 16x8 blocks): pair p = id.x / kx (8 per block row), sample i = id.x % kx
[numthreads(8, 8, 1)]
void cs_a16_scatter(uint3 id : SV_DispatchThreadID) {
    uint p = id.x / kx, i = id.x % kx, j = id.y % ky;
    uint x = cx0 + (p / 8u) * 16u + (p & 7u), y = cy0 + id.y / ky;
    if (x >= cx1 || y >= cy1) return;
    uint ha, hb;
    uint2 c = target_pixel(x, y, ha), cb = target_pixel(x + 8u, y, hb);
    uint2 s = uint2(c.x * kx + i, c.y * ky + j);
    uint va = stage[uint2(x * kx + i, y * ky + j)] & 0xFFFFu, vb = stage[uint2((x + 8u) * kx + i, y * ky + j)] & 0xFFFFu;
    if (c.x == cb.x && c.y == cb.y && ha != hb) {
        tgt[s] = (va << (ha * 16u)) | (vb << (hb * 16u));
    } else {                             // (never: x and x + 8 always share a word)
        tgt[s] = (tgt[s] & ~(0xFFFFu << (ha * 16u))) | (va << (ha * 16u));
        uint2 sb = uint2(cb.x * kx + i, cb.y * ky + j);
        tgt[sb] = (tgt[sb] & ~(0xFFFFu << (hb * 16u))) | (vb << (hb * 16u));
    }
}
