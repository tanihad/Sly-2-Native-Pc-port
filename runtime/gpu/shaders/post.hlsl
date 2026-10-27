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

uint np_clamp255(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint)v; }
int np_sel(uint s, int src, int dst) { return s == 0u ? src : s == 1u ? dst : 0; }

[numthreads(8, 8, 1)]
void cs_npost(uint3 id : SV_DispatchThreadID) {
    uint2 P = uint2(np_rect.x + id.x, np_rect.y + id.y);
    if (P.x >= np_rect.z || P.y >= np_rect.w) return;
    uint dst = np_dst[P];
    uint s = (np_mode & 2u) != 0u ? dst : np_src[P];
    uint t = s;
    if ((np_mode & 1u) != 0u) {          // T8H: index = the alpha byte, CT32 CLUT entry (ring: 2 KB per slot)
        uint p = ((np_clut >> 16) + (s >> 24)) & 511u;
        t = clut.Load((np_clut & 0xFFFFu) * 2048u + p * 4u);
    }
    // TFX (gs.hlsl tfx: MODULATE / DECAL)
    uint c = np_vcol, col;
    bool tcc = (np_mode & 32u) != 0u;
    if ((np_mode & 64u) != 0u) col = tcc ? t : (t & 0xFFFFFFu) | (c & 0xFF000000u);
    else {
        uint r = min(((c & 255u) * (t & 255u)) >> 7, 255u);
        uint g = min((((c >> 8) & 255u) * ((t >> 8) & 255u)) >> 7, 255u);
        uint b = min((((c >> 16) & 255u) * ((t >> 16) & 255u)) >> 7, 255u);
        uint a = tcc ? min(((c >> 24) * (t >> 24)) >> 7, 255u) : (c >> 24);
        col = r | (g << 8) | (b << 16) | (a << 24);
    }
    uint As = col >> 24;
    uint o = col;
    if ((np_mode & 4u) != 0u) {          // blend (32-bit frame, no dither)
        uint asel = np_alpha & 3u, bsel = (np_alpha >> 2) & 3u, csel = (np_alpha >> 4) & 3u, dsel = (np_alpha >> 6) & 3u;
        int cc = csel == 0u ? (int)As : csel == 1u ? (int)(dst >> 24) : (int)((np_alpha >> 8) & 255u);
        bool clampc = (np_mode & 8u) != 0u;
        o = 0u;
        [unroll] for (uint ch = 0u; ch < 24u; ch += 8u) {
            int cs = (int)((col >> ch) & 255u), cd = (int)((dst >> ch) & 255u);
            int v = (((np_sel(asel, cs, cd) - np_sel(bsel, cs, cd)) * cc) >> 7) + np_sel(dsel, cs, cd);
            o |= (clampc ? np_clamp255(v) : ((uint)v & 255u)) << ch;
        }
        o |= col & 0xFF000000u;
    }
    if ((np_mode & 16u) != 0u) o |= 0x80000000u;
    uint m = np_fmask | ((np_mode & 128u) != 0u ? 0xFF000000u : 0u);
    np_dst[P] = (dst & m) | (o & ~m);
}
