// (r2-m7) Native post passes of modern frames (gpu_exec.c exec_npost; notes/modern-renderer.md "R7 as built"): a draw of
// sprites that read a target view one texel per pixel at the texel centres (Sly's shadow darken: T8H of the frame itself
// through the CLUT; the motion blur: CT32 of the other frame), with every other state the executor checked on the CPU,
// is one dispatch over its samples instead of an FF draw through the generic pixel shader: each sample reads its own
// sample of the source (the 1:1 rule of notes/d3d11-renderer.md 5.1), and the GS's own TFX / blend / mask arithmetic
// (gs_draw_pixel.c write_seg, gs.hlsl ps_gs) gives the word, written straight into the target.
cbuffer npost_cb : register(b2) {
    uint4 np_rect;                       // samples [x0, x1) x [y0, y1)
    uint np_mode;                        // 1 alpha-index view (T8H: the CLUT entry of the source's alpha byte), 2 same target
                                         // (the source is the destination), 4 ABE, 8 COLCLAMP, 16 FBA, 32 TCC, 64 DECAL
                                         // (else MODULATE), 128 write nothing of the alpha byte (RGB_ONLY)
    uint np_alpha;                       // asel | bsel << 2 | csel << 4 | dsel << 6 | fix << 8
    uint np_fmask;                       // frame write mask (1: kept)
    uint np_vcol;                        // the sprites' vertex colour (flat)
    uint np_clut;                        // CLUT ring slot, entry offset (CSA * 16) << 16
    uint np_pad[3];
};
ByteAddressBuffer clut : register(t5);
RWTexture2D<uint> np_dst : register(u0);
RWTexture2D<uint> np_src : register(u1);

// (r2-m7) shadow volumes (gpu_exec.c svol_resolve): the counter texture (u1) holds each sample's sum of G steps of the
// shadow-volume draws since the last resolve (ps_gs_tri_svol's atomic adds); G += sum modulo 256, the counter back to 0
[numthreads(8, 8, 1)]
void cs_svol_resolve(uint3 id : SV_DispatchThreadID) {
    uint2 P = uint2(np_rect.x + id.x, np_rect.y + id.y);
    if (P.x >= np_rect.z || P.y >= np_rect.w) return;
    uint n = np_src[P];
    if (n == 0u) return;
    np_src[P] = 0u;
    if ((n & 255u) == 0u) return;
    uint c = np_dst[P];
    np_dst[P] = (c & 0xFFFF00FFu) | ((((c >> 8) + n) & 255u) << 8);
}

uint np_clamp255(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint)v; }
int np_sel(uint s, int src, int dst) { return s == 0u ? src : s == 1u ? dst : 0; }

// the word a post-pass sprite leaves in a sample: dst the destination word, s the source word (the sample's own texel)
uint npost_word(uint dst, uint s, uint mode, uint alpha, uint fmask, uint vcol, uint clutw) {
    uint t = s;
    if ((mode & 1u) != 0u) {             // T8H: index = the alpha byte, CT32 CLUT entry (ring: 2 KB per slot)
        uint p = ((clutw >> 16) + (s >> 24)) & 511u;
        t = clut.Load((clutw & 0xFFFFu) * 2048u + p * 4u);
    }
    // TFX (gs.hlsl tfx: MODULATE / DECAL)
    uint c = vcol, col;
    bool tcc = (mode & 32u) != 0u;
    if ((mode & 64u) != 0u) col = tcc ? t : (t & 0xFFFFFFu) | (c & 0xFF000000u);
    else {
        uint r = min(((c & 255u) * (t & 255u)) >> 7, 255u);
        uint g = min((((c >> 8) & 255u) * ((t >> 8) & 255u)) >> 7, 255u);
        uint b = min((((c >> 16) & 255u) * ((t >> 16) & 255u)) >> 7, 255u);
        uint a = tcc ? min(((c >> 24) * (t >> 24)) >> 7, 255u) : (c >> 24);
        col = r | (g << 8) | (b << 16) | (a << 24);
    }
    uint As = col >> 24;
    uint o = col;
    if ((mode & 4u) != 0u) {             // blend (32-bit frame, no dither)
        uint asel = alpha & 3u, bsel = (alpha >> 2) & 3u, csel = (alpha >> 4) & 3u, dsel = (alpha >> 6) & 3u;
        int cc = csel == 0u ? (int)As : csel == 1u ? (int)(dst >> 24) : (int)((alpha >> 8) & 255u);
        bool clampc = (mode & 8u) != 0u;
        o = 0u;
        [unroll] for (uint ch = 0u; ch < 24u; ch += 8u) {
            int cs = (int)((col >> ch) & 255u), cd = (int)((dst >> ch) & 255u);
            int v = (((np_sel(asel, cs, cd) - np_sel(bsel, cs, cd)) * cc) >> 7) + np_sel(dsel, cs, cd);
            o |= (clampc ? np_clamp255(v) : ((uint)v & 255u)) << ch;
        }
        o |= col & 0xFF000000u;
    }
    if ((mode & 16u) != 0u) o |= 0x80000000u;
    uint m = fmask | ((mode & 128u) != 0u ? 0xFF000000u : 0u);
    return (dst & m) | (o & ~m);
}

[numthreads(8, 8, 1)]
void cs_npost(uint3 id : SV_DispatchThreadID) {
    uint2 P = uint2(np_rect.x + id.x, np_rect.y + id.y);
    if (P.x >= np_rect.z || P.y >= np_rect.w) return;
    uint dst = np_dst[P];
    uint s = (np_mode & 2u) != 0u ? dst : np_src[P];
    np_dst[P] = npost_word(dst, s, np_mode, np_alpha, np_fmask, np_vcol, np_clut);
}

// (r2-m7b) the shadow pipeline's tail in one pass (gpu_exec.c shfuse): per sample of the frame B, in the game's order,
//   1. the shadow volumes' pending counter into G (cs_svol_resolve),
//   2. the copy draws of the ALIAS16 copy run that follows (Sly: B.alpha bits 0 / 7 <- B.G, B.G <- A.alpha), each a
//      function of one halfword of the same 32-bit sample of B or A (verified on the CPU: CT16 views of targets with B's
//      width, sprites 8 columns wide stepping one texel per pixel 8 columns away, so the source is the other half of the
//      same word) - alias16.hlsl cs_a16_copy's per-halfword arithmetic (5551 expand with TEXA, TFX, FBA, pack, FBMSK),
//   3. the post pass that reads B itself 1:1 (the darken: T8H through the CLUT, npost_word).
// Each step reads only what earlier steps of the same sample wrote, so one thread per sample gives the passes' result.
cbuffer shf_cb : register(b4) {
    uint4 sf_rect;                       // samples [x0, x1) x [y0, y1)
    uint4 sf_svbb;                       // counter samples [x0, x1) x [y0, y1) (x1 = 0: no counter)
    uint4 sf_c[2];                       // copy draw i: TEXA, state flags, vertex colour, written bits | hs << 16 |
                                         // hd << 17 | source A << 18 | on << 19
    uint4 sf_np;                         // post pass: mode, alpha, fmask, vertex colour
    uint4 sf_np2;                        // CLUT word, on
};
RWTexture2D<uint> sf_b : register(u0);
RWTexture2D<uint> sf_a : register(u1);
RWTexture2D<uint> sf_cnt : register(u2);

uint sf_expand16(uint c, uint texa) {    // (alias16.hlsl a_expand16)
    uint rgb = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9);
    uint ta0 = texa & 0xFFu, aem = (texa >> 15) & 1u, ta1 = (texa >> 16) & 0xFFu;
    uint a = (c & 0x8000u) ? ta1 : (aem != 0u && (c & 0x7FFFu) == 0u) ? 0u : ta0;
    return rgb | (a << 24);
}
uint sf_tfx(uint tf, bool tcc, uint c, uint t) {   // (alias16.hlsl a_tfx)
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

[numthreads(8, 8, 1)]
void cs_shfuse(uint3 id : SV_DispatchThreadID) {
    uint2 P = uint2(sf_rect.x + id.x, sf_rect.y + id.y);
    if (P.x >= sf_rect.z || P.y >= sf_rect.w) return;
    uint w = sf_b[P];
    if (P.x >= sf_svbb.x && P.x < sf_svbb.z && P.y >= sf_svbb.y && P.y < sf_svbb.w) {
        uint n = sf_cnt[P];
        if (n != 0u) {
            sf_cnt[P] = 0u;
            w = (w & 0xFFFF00FFu) | ((((w >> 8) + n) & 255u) << 8);
        }
    }
    [unroll] for (uint k = 0u; k < 2u; k++) {
        uint4 c = sf_c[k];
        if ((c.w & (1u << 19)) == 0u) continue;
        uint src = (c.w & (1u << 18)) != 0u ? sf_a[P] : w;
        uint hs = (c.w >> 16) & 1u, hd = (c.w >> 17) & 1u, wb = c.w & 0xFFFFu;
        uint tex = sf_expand16((src >> (hs * 16u)) & 0xFFFFu, c.x);
        uint col = sf_tfx((c.y >> 2) & 3u, (c.y & 16u) != 0u, c.z, tex);
        if (c.y & 1u) col |= 0x80000000u;  // FBA
        uint c16 = ((col >> 3) & 0x1Fu) | ((col >> 6) & 0x3E0u) | ((col >> 9) & 0x7C00u) | ((col >> 16) & 0x8000u);
        w = (w & ~(wb << (hd * 16u))) | ((c16 & wb) << (hd * 16u));
    }
    if (sf_np2.y != 0u) w = npost_word(w, w, sf_np.x, sf_np.y, sf_np.z, sf_np.w, sf_np2.x);
    sf_b[P] = w;
}