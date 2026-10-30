// (r2-m8) The post passes' per-word arithmetic, shared by post.hlsl (compute passes on the exact grid) and mft.hlsl (the
// modern frame's surfaces): npost_word (a post-pass sprite's word, gs_draw_pixel.c write_seg) and the shadow pipeline's
// copy draws (alias16.hlsl's per-halfword arithmetic). The includer declares `clut` (t5, the CLUT ring).
#ifndef POSTFN_HLSLI
#define POSTFN_HLSLI

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
    if ((mode & 256u) != 0u) col = c;    // (r2-m6) flat: an untextured sprite (its colour; the source is not read)
    else if ((mode & 64u) != 0u) col = tcc ? t : (t & 0xFFFFFFu) | (c & 0xFF000000u);
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
// copy draw c: the destination word after it (src: the source word)
uint sf_copy(uint4 c, uint src, uint dst) {
    uint hs = (c.w >> 16) & 1u, hd = (c.w >> 17) & 1u, wb = c.w & 0xFFFFu;
    uint tex = sf_expand16((src >> (hs * 16u)) & 0xFFFFu, c.x);
    uint col = sf_tfx((c.y >> 2) & 3u, (c.y & 16u) != 0u, c.z, tex);
    if (c.y & 1u) col |= 0x80000000u;    // FBA
    uint c16 = ((col >> 3) & 0x1Fu) | ((col >> 6) & 0x3E0u) | ((col >> 9) & 0x7C00u) | ((col >> 16) & 0x8000u);
    return (dst & ~(wb << (hd * 16u))) | ((c16 & wb) << (hd * 16u));
}

#endif
