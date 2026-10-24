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

// ---- (perf-p2) copy runs (gpu_front.h gpu_fop_alias16 mode 1, gpu_front.c a16_copy_prim) ----------------------------
// One thread per sample of a draw's sprites: the fragment exactly as gs.hlsl ps_gs evaluates it for this class (sprite
// U / V rule, nearest texel of level 0 with its wrap mode, the CT16 view's word through the inverse CT32 table, the
// fragment's own sample of a 1:1 read, TFX, FBA, the 5551 packing), written into the halfword the 16-bit frame pixel
// occupies in the C32 target: the bits FBMSK lets through, by atomic AND / OR (two sprites of one draw may write the
// two halves of a word). The frontend guarantees no other sprite of the draw writes or reads what this one writes.
// 8x8-sample tiles: group g belongs to sprite s with prefix[s] <= g < prefix[s + 1].
cbuffer a16c_cb : register(b1) {
    uint c_rec_first, c_count, c_kx, c_ky;   // sprite records (bytes), sprites, target samples per pixel
    uint c_tbase, c_tfbw, c_psm16, c_same;   // target base page, width, the 16-bit frame's PSM, source = target
    uint c_vkx, c_vky, c_vbase, c_vfbw;      // source (view) target: samples per pixel, base page, width
    uint c_gx, c_ngroups;                    // groups per dispatch row, groups
    float c_one;                             // 1.0 (products not fused with a later add, as gs.hlsl dmul_r)
    uint c_pad;
    uint4 c_prefix[11];                      // first group of sprite i (i <= c_count)
    uint4 c_vint;                            // .xy bit s: sprite s's V rows are the integers a + b * py (no doubles)
    int4 c_vab[20];                          // per sprite (a, b), exact (gpu_exec.c exec_a16_copy)
};
ByteAddressBuffer a_recs : register(t3);
ByteAddressBuffer a_states : register(t4);
RWTexture2D<uint> a_src : register(u2);     // the source target as R32_UINT (c_same: u1)

uint a_prefix(uint i) { uint4 v = c_prefix[i >> 2]; uint k = i & 3u; return k == 0u ? v.x : k == 1u ? v.y : k == 2u ? v.z : v.w; }
double a_dmul(double a, double b) { precise double p = a * b; precise double q = p * (double)c_one; return q; }

// gs.hlsl axis_params / wrapc at level 0
void a_axis(uint mode, int mn, int mx, int size, out int lo, out int hi, out int msk, out int fix) {
    fix = 0;
    if (mode == 0u) { lo = -536870912; hi = 536870911; msk = size - 1; }
    else if (mode == 1u) { lo = 0; hi = size - 1; msk = -1; }
    else if (mode == 2u) { hi = mx < size - 1 ? mx : size - 1; lo = mn < hi ? mn : hi; msk = -1; }
    else { lo = -536870912; hi = 536870911; msk = mn & (size - 1); fix = mx & (size - 1); }
}
int a_wrap(int c, int lo, int hi, int msk, int fix) { c = c < lo ? lo : c; c = c > hi ? hi : c; return (c & msk) | fix; }

// gs.hlsl expand16 / tfx
uint a_expand16(uint c, uint texa) {
    uint rgb = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9);
    uint ta0 = texa & 0xFFu, aem = (texa >> 15) & 1u, ta1 = (texa >> 16) & 0xFFu;
    uint a = (c & 0x8000u) ? ta1 : (aem != 0u && (c & 0x7FFFu) == 0u) ? 0u : ta0;
    return rgb | (a << 24);
}
uint a_tfx(uint tf, bool tcc, uint c, uint t) {
    uint cr = c & 255u, cg = (c >> 8) & 255u, cb = (c >> 16) & 255u, ca = c >> 24;
    uint tr = t & 255u, tg = (t >> 8) & 255u, tb = (t >> 16) & 255u, ta = t >> 24;
    if (tf == 0u) {
        uint r = min((cr * tr) >> 7, 255u), g = min((cg * tg) >> 7, 255u), b = min((cb * tb) >> 7, 255u);
        uint a = tcc ? min((ca * ta) >> 7, 255u) : ca;
        return r | (g << 8) | (b << 16) | (a << 24);
    }
    if (tf == 1u) return tcc ? t : (t & 0xFFFFFFu) | (c & 0xFF000000u);
    uint r = min(((cr * tr) >> 7) + ca, 255u), g = min(((cg * tg) >> 7) + ca, 255u), b = min(((cb * tb) >> 7) + ca, 255u);
    uint a = !tcc ? ca : tf == 2u ? min(ta + ca, 255u) : ta;
    return r | (g << 8) | (b << 16) | (a << 24);
}

// word w (GS word address) of a C32 target at base page `base`, width fbw: its pixel
uint2 a_word_pixel(uint w, uint base, uint fbw) {
    uint f = fbw > 0u ? fbw : 1u;
    uint p = ((w >> 11) - base) & 511u;
    uint ixy = swz_at(SWZ_ICT32, w & 2047u);
    return uint2((p % f) * 64u + (ixy & 63u), (p / f) * 32u + (ixy >> 8));
}

[numthreads(8, 8, 1)]
void cs_a16_copy(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint g = gid.y * c_gx + gid.x;
    if (g >= c_ngroups) return;
    uint lo = 0u, hi = c_count;              // prefix[lo] <= g < prefix[hi]
    [loop] while (hi - lo > 1u) { uint mid = (lo + hi) >> 1; if (a_prefix(mid) <= g) lo = mid; else hi = mid; }
    uint t = g - a_prefix(lo);
    uint rb = c_rec_first + lo * 64u;
    uint2 r = a_recs.Load2(rb);
    int x0 = (int)(r.x << 16) >> 16, y0 = (int)r.x >> 16, x1 = (int)(r.y << 16) >> 16, y1 = (int)r.y >> 16;
    // (a thread per sample column of a pixel row: the addresses once, the ky samples of the column in a loop)
    uint wsm = (uint)(x1 - x0) * c_kx, hpx = (uint)(y1 - y0);
    uint tpr = (wsm + 7u) >> 3;
    uint sx = (t % tpr) * 8u + tid.x, ry = (t / tpr) * 8u + tid.y;
    if (sx >= wsm || ry >= hpx) return;
    uint i = sx % c_kx;
    int px = x0 + (int)(sx / c_kx), py = y0 + (int)ry;
    // the record (gpu_rec_sprite) and the state (gpu_staterec)
    uint4 rr = a_recs.Load4(rb + 8u);        // rgba, z, fog, state
    int ua = asint(a_recs.Load(rb + 24u)), du = asint(a_recs.Load(rb + 28u));
    uint4 v4 = a_recs.Load4(rb + 32u);       // vc0, vcy
    bool one_to_one = (a_recs.Load(rb + 60u) & 1u) != 0u;
    uint sb = rr.w * 128u;
    uint4 tc = a_states.Load4(sb);           // TEX0 lo, hi, CLAMP lo, hi
    uint texa = a_states.Load(sb + 24u);
    uint fmask = a_states.Load(sb + 44u), flags = a_states.Load(sb + 48u);
    // U per pixel, V per row (gs.hlsl gs_frag, PRIM 1)
    int U = asint((uint)ua + (uint)du * (uint)(px - x0)) >> 12;
    int vi;
    if (((lo < 32u ? c_vint.x >> lo : c_vint.y >> (lo - 32u)) & 1u) != 0u) {
        int4 ab = c_vab[lo >> 1];
        vi = (lo & 1u) ? ab.z + ab.w * py : ab.x + ab.y * py;   // (the same value: every double operation is exact then)
    } else {
        double vc0 = asdouble(v4.x, v4.y), vcy = asdouble(v4.z, v4.w);
        precise double vp = a_dmul(vcy, (double)py);
        precise double vs = vc0 + vp;
        precise double vv = vs * 4096.0L;
        vi = vv > 2147483000.0L ? 2147483000 : vv < -2147483000.0L ? -2147483000 : (int)vv;
    }
    int V = vi >> 12;
    // nearest texel of level 0 (tex_level, level_of)
    uint t0 = tc.x, t1 = tc.y;
    uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
    tw = tw > 10u ? 10u : tw;
    th = th > 10u ? 10u : th;
    uint bp = t0 & 0x3FFFu, bw = (t0 >> 14) & 0x3Fu, psm = (t0 >> 20) & 0x3Fu;
    uint c0 = tc.z, c1 = tc.w;
    int ulo, uhi, umsk, ufix, vlo, vhi, vmsk, vfix;
    a_axis(c0 & 3u, (int)((c0 >> 4) & 0x3FFu), (int)((c0 >> 14) & 0x3FFu), 1 << tw, ulo, uhi, umsk, ufix);
    a_axis((c0 >> 2) & 3u, (int)((c0 >> 24) | ((c1 & 3u) << 8)), (int)((c1 >> 2) & 0x3FFu), 1 << th, vlo, vhi, vmsk, vfix);
    uint xa = (uint)a_wrap(U >> 4, ulo, uhi, umsk, ufix) & 2047u, ya = (uint)a_wrap(V >> 4, vlo, vhi, vmsk, vfix) & 2047u;
    uint h = gs_addr16(swz_tab_of(psm), bp, bw, xa, ya);
    uint2 vpx = a_word_pixel(h >> 1, c_vbase, c_vfbw);
    uint wb = ~fmask & 0xFFFFu;              // the bits FBMSK writes
    // the destination halfword (gather's target_pixel)
    uint hd = gs_addr16(swz_tab_of(c_psm16), c_tbase * 32u, c_tfbw, (uint)px, (uint)py);
    uint2 dpx = a_word_pixel(hd >> 1, c_tbase, c_tfbw);
    uint sh = (hd & 1u) * 16u;
    [loop] for (uint j = 0u; j < c_ky; j++) {
        // the view's sample: 1:1 reads take the fragment's own (signed offset in [0, k): the texel itself), others sample 0
        uint2 vsm = one_to_one ? uint2(i * c_vkx / c_kx, j * c_vky / c_ky) : uint2(0u, 0u);
        uint2 vsx = uint2(vpx.x * c_vkx + vsm.x, vpx.y * c_vky + vsm.y);
        uint word = c_same != 0u ? tgt[vsx] : a_src[vsx];
        uint tex = a_expand16((word >> ((h & 1u) * 16u)) & 0xFFFFu, texa);
        uint col = a_tfx((flags >> 2) & 3u, (flags & 16u) != 0u, rr.x, tex);
        if (flags & 1u) col |= 0x80000000u;  // FBA
        uint c16 = ((col >> 3) & 0x1Fu) | ((col >> 6) & 0x3E0u) | ((col >> 9) & 0x7C00u) | ((col >> 16) & 0x8000u);
        uint2 ds = uint2(dpx.x * c_kx + i, dpx.y * c_ky + j);
        uint ignored;
        InterlockedAnd(tgt[ds], ~(wb << sh), ignored);
        InterlockedOr(tgt[ds], (c16 & wb) << sh, ignored);
    }
}
