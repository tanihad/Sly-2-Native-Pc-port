// GS memory <-> targets on the GPU (runtime/gpu/gpu_exec.c, notes/d3d11-renderer.md 3.3):
//   cs_patch     vram_buf <- uploaded bits (per-word bit masks)
//   cs_load      target blocks <- vram_buf (stale blocks; every sample of a native pixel)
//   cs_download  vram_buf (or a snapshot copy) <- the GPU words of a writer target's blocks, sample 0
//   cs_clear     a recognised full fill (colour under a byte mask, or Z)
// Targets: colour = R8G8B8A8_UINT UAV (raw GS bytes R, G, B, A), depth = zpatch R32_UINT (the
// stored Z word: Z32/Z24 the whole 32-bit word, Z16 the 16-bit value).
#include "gsmem.hlsli"

cbuffer mem_cb : register(b0) {
    uint n;                              // entries
    uint list_off;                       // byte offset in data of the entry list
    uint data_off;                       // patch: 256-byte blocks
    uint mask_off;                       // patch: 256-byte masks
    uint tkind;                          // 0 C32, 1 Z32, 2 Z24, 3 Z16, 4 Z16S
    uint tbase;                          // base page
    uint tfbw;                           // width (64-pixel units)
    uint bcols;                          // blocks per row of the target
    uint kx, ky;                         // samples per native pixel
    uint value, wmask;                   // clear
    uint cx1, cy1;                       // clear rect [0, cx1) x [0, cy1)
    uint group_base;                     // first entry of this dispatch (dispatches of <= 65535 groups)
    uint mpad;
};

ByteAddressBuffer data : register(t7);
ByteAddressBuffer vram_src : register(t0);
RWByteAddressBuffer vram_dst : register(u0);
RWTexture2D<uint4> ctgt : register(u1);
RWTexture2D<uint> ztgt : register(u2);

uint tpsm() {
    switch (tkind) {
    case 1u: return PSMZ32;
    case 2u: return PSMZ24;
    case 3u: return PSMZ16;
    case 4u: return PSMZ16S;
    default: return PSMCT32;
    }
}

uint pack4(uint4 c) { return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }
uint4 unpack4(uint v) { return uint4(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24); }

uint list16(uint off, uint i) {
    uint w = data.Load(off + (i & ~1u) * 2u);
    return (i & 1u) ? w >> 16 : w & 0xFFFFu;
}

[numthreads(64, 1, 1)]
void cs_patch(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    uint i = group_base + g.x;
    if (i >= n) return;
    uint b = list16(list_off, i);
    uint m = data.Load(mask_off + i * 256u + t.x * 4u);
    if (m == 0u) return;
    uint d = data.Load(data_off + i * 256u + t.x * 4u);
    uint a = (b * 64u + t.x) * 4u;
    uint v = vram_dst.Load(a);
    vram_dst.Store(a, (v & ~m) | (d & m));
}

// the native pixel of thread p of local block k (blocks are 8x8 pixels, 16x8 for 16-bit formats)
uint2 block_pixel(uint k, uint p, bool is16) {
    uint bw = is16 ? 16u : 8u;
    return uint2((k % bcols) * bw + p % bw, (k / bcols) * 8u + p / bw);
}

void store_samples_c(uint2 px, uint v) {
    uint4 c = unpack4(v);
    for (uint j = 0u; j < ky; j++)
        for (uint i = 0u; i < kx; i++) ctgt[uint2(px.x * kx + i, px.y * ky + j)] = c;
}
void store_samples_z(uint2 px, uint v) {
    for (uint j = 0u; j < ky; j++)
        for (uint i = 0u; i < kx; i++) ztgt[uint2(px.x * kx + i, px.y * ky + j)] = v;
}

[numthreads(128, 1, 1)]
void cs_load(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    uint i = group_base + g.x;
    if (i >= n) return;
    uint psm = tpsm();
    bool is16 = psm_is16(psm);
    if (!is16 && t.x >= 64u) return;
    uint k = list16(list_off, i);
    uint2 px = block_pixel(k, t.x, is16);
    uint tab = swz_tab_of(psm), bp = tbase * 32u, v;
    if (is16) {
        uint h = gs_addr16(tab, bp, tfbw, px.x, px.y);
        v = (vram_src.Load((h >> 1) * 4u) >> ((h & 1u) * 16u)) & 0xFFFFu;
    } else {
        v = vram_src.Load(gs_addr32(tab, bp, tfbw, px.x, px.y) * 4u);
    }
    if (tkind == 0u) store_samples_c(px, v);
    else store_samples_z(px, v);
}

uint target_value(uint2 px) {
    uint2 s = uint2(px.x * kx, px.y * ky);           // sample 0
    return tkind == 0u ? pack4(ctgt[s]) : ztgt[s];
}

// entries: gpu_dl_entry { u16 local, u16 abs; u32 pad; u64 words }
[numthreads(64, 1, 1)]
void cs_download(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    uint i = group_base + g.x;
    if (i >= n) return;
    uint4 e = data.Load4(list_off + i * 16u);
    uint k = e.x & 0xFFFFu;
    uint psm = tpsm(), tab = swz_tab_of(psm), bp = tbase * 32u;
    uint w, v;
    if (psm_is16(psm)) {
        // pixels x and x + 8 of a 16-wide block row share a word
        uint2 a = block_pixel(k, (t.x / 8u) * 16u + t.x % 8u, true), b = uint2(a.x + 8u, a.y);
        uint ha = gs_addr16(tab, bp, tfbw, a.x, a.y), hb = gs_addr16(tab, bp, tfbw, b.x, b.y);
        w = ha >> 1;
        v = ((target_value(a) & 0xFFFFu) << ((ha & 1u) * 16u)) | ((target_value(b) & 0xFFFFu) << ((hb & 1u) * 16u));
    } else {
        uint2 a = block_pixel(k, t.x, false);
        w = gs_addr32(tab, bp, tfbw, a.x, a.y);
        v = target_value(a);
    }
    uint bit = w & 63u;
    uint m = bit < 32u ? (e.z >> bit) & 1u : (e.w >> (bit - 32u)) & 1u;
    if (m != 0u) vram_dst.Store(w * 4u, v);
}

// (M9) a thread per sample of the rect [0, cx1 * kx) x [0, cy1 * ky) (coalesced; it was a thread per native pixel
// looping over its kx * ky samples)
[numthreads(8, 8, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID) {
    if (id.x >= cx1 * kx || id.y >= cy1 * ky) return;
    uint2 s = id.xy;
    if (tkind == 0u) {
        uint m = ((wmask & 1u) ? 0xFFu : 0u) | ((wmask & 2u) ? 0xFF00u : 0u) | ((wmask & 4u) ? 0xFF0000u : 0u) |
                 ((wmask & 8u) ? 0xFF000000u : 0u);
        ctgt[s] = unpack4((pack4(ctgt[s]) & ~m) | (value & m));
    } else {
        ztgt[s] = tkind == 2u ? (ztgt[s] & 0xFF000000u) | (value & 0xFFFFFFu) : value;
    }
}

// cross-target copy (M4): blocks of target t <- the same blocks of the target that wrote them (same
// kind and scale), every sample; entries u16 source local block | u16 destination local block << 16
RWTexture2D<uint4> csrc : register(u3);
RWTexture2D<uint> zsrc : register(u4);

[numthreads(128, 1, 1)]
void cs_xcopy(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    uint i = group_base + g.x;
    if (i >= n) return;
    bool is16 = psm_is16(tpsm());
    if (!is16 && t.x >= 64u) return;
    uint e = data.Load(list_off + i * 4u);
    uint bw = is16 ? 16u : 8u, ks = e & 0xFFFFu, kd = e >> 16, p = t.x;
    uint2 d = uint2((kd % bcols) * bw + p % bw, (kd / bcols) * 8u + p / bw);
    uint2 s = uint2((ks % value) * bw + p % bw, (ks / value) * 8u + p / bw);
    for (uint j = 0u; j < ky; j++)
        for (uint q = 0u; q < kx; q++) {
            uint2 a = uint2(d.x * kx + q, d.y * ky + j), b = uint2(s.x * kx + q, s.y * ky + j);
            if (tkind == 0u) ctgt[a] = csrc[b];
            else ztgt[a] = zsrc[b];
        }
}