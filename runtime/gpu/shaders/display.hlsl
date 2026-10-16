// PCRTC on the GPU (M6, notes/d3d11-renderer.md 8.2): the picture of a VBlank composed from the
// displayed buffers exactly as runtime/gs.c gs_compose_from does it on the CPU - both circuits, DISPFB /
// DISPLAY (DBX/DBY, DX/DY, MAGH/MAGV, DW/DH), PMODE (EN1/EN2, MMOD/ALP, SLBG), BGCOLOR, SMODE2 field
// mode with the "DH + 1 over two" field rule - and then gs.c's field deinterlacer (cs_deint). Integer
// arithmetic throughout: at S=1 the picture equals gs_compose_from of the GPU's memory bit for bit.
//
// A circuit reads either a colour target (the frontend proved the displayed rect current there:
// texel (x * kx, y * ky), sample 0 of the native pixel) or vram_buf through the swizzle tables (the
// frontend downloaded the GPU words of the rect first).
//
// Pictures are R8G8B8A8 (R in the low byte, alpha 255), written through an R32_UINT UAV. Inside the
// shaders a pixel is gs.c's 0xAARRGGBB form.
#include "gsmem.hlsli"

ByteAddressBuffer vram : register(t0);
Texture2D<uint4> ctex0 : register(t1);   // circuit 1's target (UINT view), when src0 == 1
Texture2D<uint4> ctex1 : register(t2);   // circuit 2's
Texture2D<uint4> hist_same : register(t3);   // deint: the last field of this parity (full picture layout)
Texture2D<uint4> hist_oth : register(t4);    // deint: the last field of the other parity
RWTexture2D<uint> pict : register(u0);

cbuffer display_cb : register(b0) {
    uint W, H, ux0, uy0;
    uint hs, vs, flags, alp;             // flags: 1 MMOD, 2 SLBG, 4 field mode
    uint bg, par, deint_mode, pad0;      // deint: the field's parity, 0 average only, 1 still detection
    uint4 cA[2];                         // en, fbp, fbw, psm
    uint4 cB[2];                         // dbx, dby, dx, dy
    uint4 cC[2];                         // magh, magv, dw, dh
    uint4 cD[2];                         // source (0 vram_buf, 1 target), kx, ky, -
};

uint pack_rgba(uint c) { return ((c >> 16) & 0xFFu) | (c & 0xFF00u) | ((c & 0xFFu) << 16) | 0xFF000000u; }
uint unpack_rgb(uint4 t) { return (t.r << 16) | (t.g << 8) | t.b; }

// gs_fmt_of: the formats with a frame layout; anything else is read as PSMCT32 (gs.c decode_circuit)
uint fmt_psm(uint psm) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMCT16: case PSMCT16S: case PSMZ32: case PSMZ24: case PSMZ16: case PSMZ16S: return psm;
    default: return PSMCT32;
    }
}

// one displayed pixel of circuit i, buffer coordinates fx, fy (0..2047), as gs.c read_row
uint read_px(uint i, uint fx, uint fy) {
    uint psm = fmt_psm(cA[i].w);
    uint v;
    if (cD[i].x != 0u) {
        uint4 t = i == 0u ? ctex0.Load(int3(fx * cD[i].y, fy * cD[i].z, 0)) : ctex1.Load(int3(fx * cD[i].y, fy * cD[i].z, 0));
        v = t.r | (t.g << 8) | (t.b << 16) | (t.a << 24);
    } else if (psm_is16(psm)) {
        uint a = gs_addr16(swz_tab_of(psm), cA[i].y * 32u, cA[i].z, fx, fy);
        uint w = vram.Load((a >> 1) * 4u);
        v = (a & 1u) != 0u ? w >> 16 : w & 0xFFFFu;
    } else {
        v = vram.Load(gs_addr32(swz_tab_of(psm), cA[i].y * 32u, cA[i].z, fx, fy) * 4u);
    }
    if (psm_is16(psm))
        return ((v & 0x1Fu) << 19) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 3) | ((v & 0x8000u) != 0u ? 0x80000000u : 0u);
    uint top = (psm == PSMCT24 || psm == PSMZ24) ? 0x80000000u : (v & 0xFF000000u);
    return ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu) | top;
}

uint mix(uint a, uint b, uint al) {      // a * al + b * (255 - al), al 0..255 (gs.c mix)
    uint r = (((a >> 16) & 0xFFu) * al + ((b >> 16) & 0xFFu) * (255u - al) + 127u) / 255u;
    uint g = (((a >> 8) & 0xFFu) * al + ((b >> 8) & 0xFFu) * (255u - al) + 127u) / 255u;
    uint bl = ((a & 0xFFu) * al + (b & 0xFFu) * (255u - al) + 127u) / 255u;
    return (r << 16) | (g << 8) | bl;
}

// buffer column of output column ox for circuit i, or -1 outside its DISPLAY rect
int circuit_x(uint i, uint ox) {
    int X = (int)(ux0 + ox * hs), dx = (int)cB[i].z, dw = (int)cC[i].z;
    if (X < dx || X > dx + dw) return -1;
    return (int)((cB[i].x + (uint)(X - dx) / (cC[i].x + 1u)) & 2047u);
}

// buffer row of output row oy for circuit i, or -1 (not shown on this row)
int circuit_y(uint i, uint oy) {
    if (cA[i].x == 0u) return -1;
    int L = (int)(uy0 + oy * vs), dy = (int)cB[i].w, dh = (int)cC[i].w;
    if (L < dy || L > dy + dh) return -1;
    uint rel = (uint)(L - dy) / (cC[i].y + 1u);
    if ((flags & 4u) != 0u) {            // field mode: (DH + 1) / (MAGV + 1) / 2 lines per field
        rel >>= 1;
        if (rel >= (((uint)dh + 1u) / (cC[i].y + 1u)) >> 1) return -1;
    }
    return (int)((cB[i].y + rel) & 2047u);
}

[numthreads(8, 8, 1)]
void cs_compose(uint3 id : SV_DispatchThreadID) {
    uint ox = id.x, oy = id.y;
    if (ox >= W || oy >= H) return;
    bool mmod = (flags & 1u) != 0u, slbg = (flags & 2u) != 0u;
    int fy0 = circuit_y(0u, oy), fy1 = circuit_y(1u, oy);
    int x0 = cA[0].x != 0u ? circuit_x(0u, ox) : -1, x1 = cA[1].x != 0u ? circuit_x(1u, ox) : -1;
    uint d;
    if (fy0 < 0) {                       // circuit 2 (or the background) only
        if (slbg) d = bg;
        else if (fy1 >= 0) d = x1 >= 0 ? read_px(1u, (uint)x1, (uint)fy1) & 0xFFFFFFu : 0u;
        else d = 0u;
    } else {
        uint lower = slbg ? bg : (fy1 >= 0 && x1 >= 0) ? read_px(1u, (uint)x1, (uint)fy1) & 0xFFFFFFu : 0u;
        if (x0 < 0) d = lower;
        else {
            uint top = read_px(0u, (uint)x0, (uint)fy0);
            if (cA[1].x != 0u || slbg) {
                // PMODE.MMOD: 1 = the fixed ALP (0xFF = 1.0), 0 = circuit 1's pixel alpha (0x80 = 1.0)
                uint pa = top >> 24, al = mmod ? alp : pa >= 0x80u ? 255u : pa * 2u;
                d = mix(top, lower, al);
            } else d = top & 0xFFFFFFu;
        }
    }
    pict[uint2(ox, oy)] = pack_rgba(d);
}

// gs.c px_close / px_avg
bool px_close(uint a, uint b) {
    int dr = (int)((a >> 16) & 0xFFu) - (int)((b >> 16) & 0xFFu), dg = (int)((a >> 8) & 0xFFu) - (int)((b >> 8) & 0xFFu);
    int db = (int)(a & 0xFFu) - (int)(b & 0xFFu);
    return dr >= -6 && dr <= 6 && dg >= -6 && dg <= 6 && db >= -6 && db <= 6;
}
uint px_avg(uint a, uint b) { return (((a & 0xFEFEFEu) >> 1) + ((b & 0xFEFEFEu) >> 1) + (a & b & 0x010101u)) & 0xFFFFFFu; }
uint unpack(uint w) { return ((w & 0xFFu) << 16) | (w & 0xFF00u) | ((w >> 16) & 0xFFu); }

// the lines between this field's lines (gs.c deinterlace): thread (x, k) writes output row oth + 2k
[numthreads(8, 8, 1)]
void cs_deint(uint3 id : SV_DispatchThreadID) {
    uint x = id.x, oth = 1u - par;
    int oy = (int)(oth + 2u * id.y), h = (int)H;
    if (x >= W || oy >= h) return;
    int ya = oy - 1 >= 0 ? oy - 1 : oy + 1, yb = oy + 1 < h ? oy + 1 : oy - 1;
    if (ya < 0 || ya >= h) return;
    uint a = unpack(pict[uint2(x, ya)]), b = unpack(pict[uint2(x, yb)]);
    uint d;
    if (deint_mode != 0u) {
        uint pa = unpack_rgb(hist_same.Load(int3(x, ya, 0))), pb = unpack_rgb(hist_same.Load(int3(x, yb, 0)));
        bool still = px_close(a, pa) && px_close(b, pb);
        d = still ? unpack_rgb(hist_oth.Load(int3(x, oy, 0))) : px_avg(a, b);
    } else d = px_avg(a, b);
    pict[uint2(x, oy)] = pack_rgba(d);
}
