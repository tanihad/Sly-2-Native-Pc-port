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
                                         // (else MODULATE), 128 write nothing of the alpha byte (RGB_ONLY), 256 (r2-m6) flat:
                                         // untextured, the colour is np_vcol
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

#include "postfn.hlsli"

[numthreads(8, 8, 1)]
void cs_npost(uint3 id : SV_DispatchThreadID) {
    uint2 P = uint2(np_rect.x + id.x, np_rect.y + id.y);
    if (P.x >= np_rect.z || P.y >= np_rect.w) return;
    uint dst = np_dst[P];
    uint s = (np_mode & 2u) != 0u ? dst : np_src[P];
    np_dst[P] = npost_word(dst, s, np_mode, np_alpha, np_fmask, np_vcol, np_clut);
}

// (r2-m7b) the shadow pipeline in one pass (gpu_exec.c shfuse): per sample of the frame B (A: the other frame), in the
// game's order,
//   0. the copy draw of shuffle 1 (A.alpha <- B.G), deferred past the shadow volumes (they only add to the counter and
//      read Z and B's alpha), and the G clear before the volumes (byte mask, value),
//   1. the shadow volumes' pending counter into G (cs_svol_resolve),
//   2. the copy draws of the ALIAS16 copy run that follows (shuffles 2 and 3: B.alpha bits 0 / 7 <- B.G, B.G <- A.alpha),
//   3. the post pass that reads B itself 1:1 (the darken: T8H through the CLUT, npost_word).
// Every copy draw is a function of the other half of the same 32-bit sample of B or A (verified on the CPU: CT16 views of
// targets of B's width, sprites 8 columns wide stepping one texel per pixel 8 columns away; the CT16 swizzle puts columns
// x and x + 8 of a 16-wide block into the two halves of one word) - alias16.hlsl cs_a16_copy's per-halfword arithmetic
// (5551 expand with TEXA, TFX, FBA, pack, FBMSK). Each step reads only what earlier steps of the same sample wrote, so
// one thread per sample gives the passes' result.
cbuffer shf_cb : register(b4) {
    uint4 sf_rect;                       // samples [x0, x1) x [y0, y1)
    uint4 sf_svbb;                       // counter samples [x0, x1) x [y0, y1) (x1 = 0: no counter)
    uint4 sf_c[3];                       // copy draw i (0: deferred shuffle 1): TEXA, state flags, vertex colour,
                                         // written bits | hs << 16 | hd << 17 | source A << 18 | on << 19 | dest A << 20
    uint4 sf_clr;                        // G clear of B after copy 0: on, value, byte mask (32-bit form)
    uint4 sf_np;                         // post pass: mode, alpha, fmask, vertex colour
    uint4 sf_np2;                        // CLUT word, on, A written
};
RWTexture2D<uint> sf_b : register(u0);
RWTexture2D<uint> sf_a : register(u1);
RWTexture2D<uint> sf_cnt : register(u2);

[numthreads(8, 8, 1)]
void cs_shfuse(uint3 id : SV_DispatchThreadID) {
    uint2 P = uint2(sf_rect.x + id.x, sf_rect.y + id.y);
    if (P.x >= sf_rect.z || P.y >= sf_rect.w) return;
    uint w = sf_b[P];
    bool ua = sf_np2.z != 0u || (sf_c[1].w & (1u << 18)) != 0u || (sf_c[2].w & (1u << 18)) != 0u;
    uint a = ua ? sf_a[P] : 0u;
    [unroll] for (uint k = 0u; k < 3u; k++) {
        uint4 c = sf_c[k];
        if ((c.w & (1u << 19)) != 0u) {
            uint src = (c.w & (1u << 18)) != 0u ? a : w;
            if ((c.w & (1u << 20)) != 0u) a = sf_copy(c, src, a);
            else w = sf_copy(c, src, w);
        }
        if (k == 0u) {
            if (sf_clr.x != 0u) w = (w & ~sf_clr.z) | (sf_clr.y & sf_clr.z);
            if (P.x >= sf_svbb.x && P.x < sf_svbb.z && P.y >= sf_svbb.y && P.y < sf_svbb.w) {
                uint n = sf_cnt[P];
                if (n != 0u) {
                    sf_cnt[P] = 0u;
                    w = (w & 0xFFFF00FFu) | ((((w >> 8) + n) & 255u) << 8);
                }
            }
        }
    }
    if (sf_np2.y != 0u) w = npost_word(w, w, sf_np.x, sf_np.y, sf_np.z, sf_np.w, sf_np2.x);
    sf_b[P] = w;
    if (sf_np2.z != 0u) sf_a[P] = a;
}