// The GS pixel pipeline on the ROV tier (notes/d3d11-renderer.md 4.3, milestone M3 "REF tier"): every
// fragment runs the software renderer's pipeline (runtime/gs_draw_pixel.c) in integers, in primitive
// order per pixel (rasterizer-ordered views):
//   attributes   from the frontend's per-primitive records (gpu_front.h gpu_rec_*): the software
//                renderer's own 16.16 row stepping (triangles: row + dy * rows + dx * x, modulo 2^32;
//                Z in 64 bits), sprites' U/V rule, line/point pixels as the DDA produced them;
//                perspective S/T/Q stepped in floats exactly as the software renderer steps them along
//                its span pieces (64-pixel columns, batch cuts from the frontend's table);
//   texture      direct sampling from vram_buf through the swizzle tables (all PSMs incl. T8H/T4HL/
//                T4HH), CLUT ring, TEXA, GS 4-bit bilinear, REPEAT/CLAMP/REGION modes, LOD and mipmaps;
//                or a target view (M4, 5.1): CT32/CT24, alpha index (T8H/T4HL/T4HH) and CT16/CT16S
//                (words mapped to the target's pixels) through the target's SRV, its shadow copy or, for
//                byte lanes the draw does not write, the bound target itself; 1:1 sprite reads take the
//                fragment's own sample of each texel, other reads sample 0;
//   TFX, fog, alpha test + AFAIL, DATE/DATM, Z test + write (zpatch holds the stored Z word),
//   blend ((A-B)*C>>7)+D with COLCLAMP or wrap, PABE, FBA, FBMSK per bit, DTHE (16-bit frames).
// Colour targets are bound as R32_UINT (the raw GS word); in an ALIAS16 run (16-bit frame over a C32
// target, shaders/alias16.hlsl) u0 is the staging texture holding each 16-bit pixel's value.
#include "int64.hlsli"
#include "gsmem.hlsli"

#ifndef PRIM
#define PRIM 0                           // 0 triangles, 1 sprites, 2 line / point pixels
#endif

// Per-draw constants (gpu_exec.c draw_consts, 256 bytes per draw slot). (M9) Read from the buffer drawc by the draw's
// slot, which every draw passes as its StartInstanceLocation (the vertex shader's per-instance input DRAWID, the
// executor's iota vertex buffer) and the vertex shader hands to the pixel shader: no constant buffer is rebound per
// draw (binding a constant-buffer window per draw cost the GPU ~0.2 us per draw at 3x).
ByteAddressBuffer drawc : register(t9);
static uint rec_first;                   // byte offset of the draw's first record
static uint draw_flags;                  // 1: colour target bound (u0), 2: depth target bound (u1)
static uint kx, ky;                      // samples per native pixel of the bound targets
static float2 ndc_scale, ndc_bias;       // NDC = d3d pixel * scale + bias (power-of-two viewport: exact)
static uint vkx, vky;                    // texture view target samples
static uint probe_x, probe_y;            // debug probe pixel (PS2_GPU_PROBE; ~0: off)
static float one;                        // 1.0: products multiplied by it are rounded before any add
                                         //   (drivers may fuse a*b+c into an FMA despite `precise`)
static uint vbase, vfbw;                 // texture view target: base page, width (CT16 views map words to its pixels)
static uint vflags;                      // 1: the view has the bound target's sample grid (1:1 reads use the fragment's sample)
static uint ff_blend;                    // FF draws (M5): blend class (gpu_front.c BL_*)
static uint atst_mode;                   // FF draws: 0, 1 the pass draw of an ATST split (discard failing fragments),
                                         //   2 its fail draw (discard passing fragments)
static float dual_bias;                  // FF dual-source blends: LSB the source is lowered by (ps_ff)

void load_draw(uint did) {
    uint b = did * 256u;
    uint4 a = drawc.Load4(b), c = drawc.Load4(b + 16u), d = drawc.Load4(b + 32u), e = drawc.Load4(b + 48u), f = drawc.Load4(b + 64u);
    rec_first = a.x; draw_flags = a.y; kx = a.z; ky = a.w;
    ndc_scale = asfloat(c.xy); ndc_bias = asfloat(c.zw);
    vkx = d.x; vky = d.y; probe_x = d.z; probe_y = d.w;
    one = asfloat(e.x); vbase = e.y; vfbw = e.z; vflags = e.w;
    ff_blend = f.x; atst_mode = f.y; dual_bias = asfloat(f.z);
}

// a * b rounded to the type's precision as the C reference rounds it (never fused with a later add)
double dmul_r(double a, double b) { precise double p = a * b; precise double q = p * (double)one; return q; }
float fmul_r(float a, float b) { precise float p = a * b; precise float q = p * one; return q; }

ByteAddressBuffer vram : register(t0);
Texture2D<uint4> view : register(t1);
ByteAddressBuffer recs : register(t3);
ByteAddressBuffer states : register(t4);
ByteAddressBuffer clut : register(t5);
ByteAddressBuffer fdata : register(t7);

#ifndef FF
#define FF 0                             // 1: the FF pixel shaders (no UAVs: the outputs are render targets)
#endif

#if !FF
// colour: the C32 target as R32_UINT (the raw GS word), or in a 16-bit frame run (ALIAS16) the
// staging texture in the frame's own pixel space holding each pixel's 16-bit value
#if NOROV
// (M11) GPUs without rasterizer-ordered views: plain UAVs. The frontend (gf_cfg.no_rov) then puts only primitives
// with disjoint rectangles into one draw, so no pixel is written twice within a draw, and D3D11 orders the UAV
// accesses of consecutive draws: the same per-pixel primitive order as the ROV draws.
RWTexture2D<uint> rov_c : register(u0);
RWTexture2D<uint> rov_z : register(u1);
#else
RasterizerOrderedTexture2D<uint> rov_c : register(u0);
RasterizerOrderedTexture2D<uint> rov_z : register(u1);
#endif
RWByteAddressBuffer dbg : register(u2);  // probe records (gpu_exec.c, PS2_GPU_PROBE)

// a probe record: 16 words (kind, record, state, U, V, texel, colour, Z, S, T, Q, xl, Z old, Cd, written, pixel)
void probe_rec(uint kind, uint rb, uint state, int U, int V, uint t, uint col, uint z, float s, float tt, float q, int xl,
               uint zold, uint dst, uint cw, int px, int py) {
    uint slot;
    dbg.InterlockedAdd(0, 1u, slot);
    if (slot >= 255u) return;
    uint o = 64u + slot * 64u;
    dbg.Store4(o, uint4(0xDB000000u | kind, rb, state, asuint(U)));
    dbg.Store4(o + 16u, uint4(asuint(V), t, col, z));
    dbg.Store4(o + 32u, uint4(asuint(s), asuint(tt), asuint(q), asuint(xl)));
    dbg.Store4(o + 48u, uint4(zold, dst, cw, (uint)px | ((uint)py << 16)));
}
#endif

// ---- vertex shaders --------------------------------------------------------------------------------
struct VR {
    float4 pos : SV_Position;
    nointerpolation uint rec : REC;      // record byte offset (sprites, line pixels)
    nointerpolation uint did : DID;      // (M9) the draw's slot (load_draw)
};

float4 clip_of(float2 d3d) {             // d3d pixel coordinates -> clip space
    float2 p = d3d * ndc_scale + ndc_bias;
    return float4(p.x, -p.y, 0.5, 1.0);
}

struct VIn {
    float2 xy : POSITION;                // GS coordinates (12.4 minus XYOFFSET, exact)
    uint rgba : COLOR0;
    uint fog_state : COLOR1;
    float3 stq : TEXCOORD0;
    uint did : DRAWID;                   // (M9) the draw's slot: per-instance element, StartInstanceLocation
};

// triangles: D3D pixel x * k + i has its centre at GS position x + i / k (sample 0 = the native pixel)
VR vs_ref_tri(VIn i) {
    load_draw(i.did);
    VR o;
    o.did = i.did;
    o.pos = clip_of(float2(i.xy.x * (float)kx + 0.5, i.xy.y * (float)ky + 0.5));
    o.rec = 0u;
    return o;
}

// Single-rate classes (sprites, line / point pixels): every sample of a covered native pixel takes the native
// pixel's value - the samples stored with it, x * k .. x * k + k - 1 (paraLLEl-GS rule, 4.4). (M7 tried the
// sample's nearest native pixel instead, centred like the triangles: it breaks passes that map every native
// pixel differently - the CT16 channel shuffles move 16-bit pixel x + 8 to x and x - 8 to x + 8 - so a storage
// pixel must only ever be written by the primitive covering its own sample 0.)
float2 single_rate_pos(float x, float y) { return float2(x * (float)kx, y * (float)ky); }

// sprites: the native rectangle [x0, x1) x [y0, y1), every sample of a native pixel covered (single rate)
VR vs_ref_sprite(VIn vin, uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint did = vin.did - inst;           // (M9: element StartInstanceLocation + instance of the iota buffer; VIn keeps the
                                         //  input registers of the shared input layout)
    load_draw(did);
    uint base = rec_first + inst * 64u;
    uint2 r = recs.Load2(base);
    float x0 = (float)((int)(r.x << 16) >> 16), y0 = (float)((int)r.x >> 16);
    float x1 = (float)((int)(r.y << 16) >> 16), y1 = (float)((int)r.y >> 16);
    VR o;
    o.pos = clip_of(single_rate_pos((vid & 1u) ? x1 : x0, (vid & 2u) ? y1 : y0));
    o.rec = base;
    o.did = did;
    return o;
}

// line / point pixels: one native pixel each
VR vs_ref_linepix(VIn vin, uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint did = vin.did - inst;           // (M9: element StartInstanceLocation + instance of the iota buffer; VIn keeps the
                                         //  input registers of the shared input layout)
    load_draw(did);
    uint base = rec_first + inst * 32u;
    uint r = recs.Load(base);
    float px = (float)((int)(r << 16) >> 16), py = (float)((int)r >> 16);
    VR o;
    o.pos = clip_of(single_rate_pos(px + ((vid & 1u) ? 1.0 : 0.0), py + ((vid & 2u) ? 1.0 : 0.0)));
    o.rec = base;
    o.did = did;
    return o;
}

// (M7) thin analytic lines (outlines = thin, gpu_rec_thin): a quad around the line, wide enough for its
// coverage plus a sample; the pixel shader decides every sample from its distance to the line
VR vs_ref_thin(VIn vin, uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint did = vin.did - inst;           // (M9: element StartInstanceLocation + instance of the iota buffer; VIn keeps the
                                         //  input registers of the shared input layout)
    load_draw(did);
    uint base = rec_first + inst * 128u;
    float4 g = asfloat(recs.Load4(base));            // lo, hi, a0, len (12.4)
    float2 o = asfloat(recs.Load2(base + 16u));      // o0, od (12.4)
    uint flags = recs.Load(base + 24u);
    float w = asfloat(recs.Load(base + 108u));
    bool xm = (flags & 1u) != 0u;
    float km = xm ? (float)kx : (float)ky, kn = xm ? (float)ky : (float)kx;
    float m = ((vid & 1u) ? g.y / 16.0 + 1.0 / km : g.x / 16.0 - 1.0 / km);
    float t = (m * 16.0 - g.z) / g.w;
    float h = w + 1.0 / kn;
    float n = (o.x + o.y * t) / 16.0 + ((vid & 2u) ? h : -h);
    float2 p = xm ? float2(m, n) : float2(n, m);
    VR r;
    r.pos = clip_of(float2(p.x * (float)kx + 0.5, p.y * (float)ky + 0.5));
    r.rec = base;
    r.did = did;
    return r;
}

// FF triangles (M5): the same position as vs_ref_tri (identical coverage), plus S/T/Q for the fast
// perspective path (interpolated linearly in screen space, as the GS steps them)
struct VF {
    float4 pos : SV_Position;
    nointerpolation uint rec : REC;
    noperspective float3 stq : TEXCOORD0;
    nointerpolation uint did : DID;      // (M9) the draw's slot
};

VF vs_ff_tri(VIn i) {
    load_draw(i.did);
    VF o;
    o.did = i.did;
    o.pos = clip_of(float2(i.xy.x * (float)kx + 0.5, i.xy.y * (float)ky + 0.5));
    o.rec = 0u;
    o.stq = i.stq;
    return o;
}

// ---- state ------------------------------------------------------------------------------------------
struct St {
    uint4 tex0clamp;                     // TEX0 lo, hi, CLAMP lo, hi
    uint2 tex1;
    uint texa, clut_slot, texsrc, fogcol, test, fmask, flags, alpha, zinfo, tlev;
    uint mip[6];
    uint2 dimx;                          // DIMX register (dither matrix, 16-bit frames)
};

#define STATE_BYTES 128u

St load_state(uint idx) {
    uint b = idx * STATE_BYTES;
    St s;
    s.tex0clamp = states.Load4(b);
    s.tex1 = states.Load2(b + 16u);
    uint4 a = states.Load4(b + 24u), c = states.Load4(b + 40u), d = states.Load4(b + 56u);
    s.texa = a.x; s.clut_slot = a.y; s.texsrc = a.z; s.fogcol = a.w;
    s.test = c.x; s.fmask = c.y; s.flags = c.z; s.alpha = c.w;
    s.zinfo = d.x; s.tlev = d.y;
    s.mip[0] = d.z; s.mip[1] = d.w;
    uint4 e = states.Load4(b + 72u);
    s.mip[2] = e.x; s.mip[3] = e.y; s.mip[4] = e.z; s.mip[5] = e.w;
    s.dimx = states.Load2(b + 96u);
    return s;
}

#define SF_FBA 1u
#define SF_COLCLAMP 2u
#define SF_TCC 16u
#define SF_FGE 32u
#define SF_FST 128u
#define SF_ABE 256u
#define SF_AA1 512u
#define SF_PABE 1024u
#define SF_TME 4096u
#define SF_FB24 8192u
#define SF_FB16 0x20000u
#define SF_DTHE 2048u
#define TS_SELF 0x10000u                 // StateRec texsrc: the view is the bound colour target (disjoint byte lanes)

// ---- numeric helpers (gs_draw_pixel.c) -----------------------------------------------------------
uint clamp255i(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint)v; }

int f2i(float x) {                       // floor, saturating, NaN -> large negative
    if (!(x > -1e8)) return -100000000;
    if (x > 1e8) return 100000000;
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}

// 1.0f / q correctly rounded (the C division): Newton in double, then the nearest float among the
// neighbours by the exact residual r * q - 1 (exact in double)
float rcp_exact(float q) {
    precise float r0 = 1.0 / q;
    if (!(abs(q) > 0.0) || !(abs(r0) < 3.0e38) || !(abs(r0) > 1.0e-37)) return r0;
    double dq = (double)q, d0 = (double)r0;
    precise double qd = dmul_r(dq, d0);
    precise double c2 = 2.0L - qd;
    precise double d1 = dmul_r(d0, c2);
    float r = (float)d1;
    float best = r;
    precise double rq = (double)r * dq;
    precise double e = rq - 1.0L;
    double be = e < 0.0 ? -e : e;
    [unroll] for (int k = 0; k < 2; k++) {
        float c = asfloat(k == 0 ? asuint(r) + 1u : asuint(r) - 1u);
        precise double cq = (double)c * dq;
        precise double ec = cq - 1.0L;
        ec = ec < 0.0 ? -ec : ec;
        if (ec < be) { be = ec; best = c; }
    }
    return best;
}

double i64_to_double(i64 v) { return (double)asint(v.y) * 4294967296.0 + (double)v.x; }

// ---- texture ---------------------------------------------------------------------------------------
struct Lvl { uint bp, bw, w, h; int ulo, uhi, umsk, ufix, vlo, vhi, vmsk, vfix; };

void axis_params(uint mode, int mn, int mx, int size, uint level, out int lo, out int hi, out int msk, out int fix) {
    fix = 0;
    if (mode == 0u) { lo = -536870912; hi = 536870911; msk = size - 1; }                 // REPEAT
    else if (mode == 1u) { lo = 0; hi = size - 1; msk = -1; }                           // CLAMP
    else if (mode == 2u) {                                                              // REGION_CLAMP
        mn >>= level; mx >>= level;
        hi = mx < size - 1 ? mx : size - 1;
        lo = mn < hi ? mn : hi;
        msk = -1;
    } else { lo = -536870912; hi = 536870911; msk = mn & (size - 1); fix = mx & (size - 1); }   // REGION_REPEAT
}

Lvl level_of(St st, uint l) {
    Lvl L;
    uint t0 = st.tex0clamp.x, t1 = st.tex0clamp.y;
    uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
    tw = tw > 10u ? 10u : tw;
    th = th > 10u ? 10u : th;
    if (l == 0u) { L.bp = t0 & 0x3FFFu; L.bw = (t0 >> 14) & 0x3Fu; }
    else { uint m = st.mip[l - 1u]; L.bp = m & 0x3FFFu; L.bw = (m >> 14) & 0x3Fu; }
    uint lw = tw > l ? tw - l : 0u, lh = th > l ? th - l : 0u;
    L.w = 1u << lw; L.h = 1u << lh;
    uint c0 = st.tex0clamp.z, c1 = st.tex0clamp.w;
    uint wms = c0 & 3u, wmt = (c0 >> 2) & 3u;
    int minu = (int)((c0 >> 4) & 0x3FFu), maxu = (int)((c0 >> 14) & 0x3FFu);
    int minv = (int)((c0 >> 24) | ((c1 & 3u) << 8)), maxv = (int)((c1 >> 2) & 0x3FFu);
    axis_params(wms, minu, maxu, (int)L.w, l, L.ulo, L.uhi, L.umsk, L.ufix);
    axis_params(wmt, minv, maxv, (int)L.h, l, L.vlo, L.vhi, L.vmsk, L.vfix);
    return L;
}

int wrapc(int c, int lo, int hi, int msk, int fix) {
    c = c < lo ? lo : c;
    c = c > hi ? hi : c;
    return (c & msk) | fix;
}

uint vw(uint w) { return vram.Load(w * 4u); }

uint expand16(uint c, uint texa) {
    uint rgb = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9);
    uint ta0 = texa & 0xFFu, aem = (texa >> 15) & 1u, ta1 = (texa >> 16) & 0xFFu;
    uint a = (c & 0x8000u) ? ta1 : (aem != 0u && (c & 0x7FFFu) == 0u) ? 0u : ta0;
    return rgb | (a << 24);
}

uint clut_half(uint slot, uint p) {
    uint w = clut.Load(slot * 1024u + (p & ~1u) * 2u);
    return (p & 1u) ? w >> 16 : w & 0xFFFFu;
}

uint clut_lookup(St st, uint idx) {
    uint t1 = st.tex0clamp.y;
    uint cpsm = (t1 >> 19) & 15u, csa = (t1 >> 24) & 31u;
    uint p = (csa * 16u + idx) & 511u;
    if (cpsm == PSMCT16 || cpsm == PSMCT16S) return expand16(clut_half(st.clut_slot, p), st.texa);
    return clut_half(st.clut_slot, p) | (clut_half(st.clut_slot, p ^ 256u) << 16);
}

// target views (notes/d3d11-renderer.md 5.1): texel (x, y) is the view target's native pixel (x, y);
// its sample is g_vsample (the fragment's own sample for 1:1 reads, else sample 0)
static uint2 g_vsample = uint2(0u, 0u);
static bool g_vself = false;             // read the bound colour target itself (byte lanes it does not write)
static int2 g_voff = int2(0, 0);         // (M7) 1:1 reads: the fragment's sample offset from its pixel's sample 0 (signed)

int floordiv_u(int a, uint k) { return a >= 0 ? (int)((uint)a / k) : -(int)(((uint)(-a) + k - 1u) / k); }

uint view_word(uint x, uint y) {
    int2 s = int2((int)(x * vkx + g_vsample.x), (int)(y * vky + g_vsample.y));
#if !FF
    if (g_vself) return rov_c[s];        // (FF draws never read their own target: a shadow copy)
#endif
    uint4 c = view.Load(int3(s, 0));
    return c.x | (c.y << 8) | (c.z << 16) | (c.w << 24);
}

// the view's word at GS word address w (CT16/CT16S views of a C32 target with the same base and width:
// 16-bit page n is 32-bit page n, so the word's page and its CT32 position in the page give the pixel)
uint view_word_at(uint w) {
    uint fbw = vfbw > 0u ? vfbw : 1u;
    uint p = ((w >> 11) - vbase) & 511u;
    uint ixy = swz_at(SWZ_ICT32, w & 2047u);
    return view_word((p % fbw) * 64u + (ixy & 63u), (p / fbw) * 32u + (ixy >> 8));
}

// one texel (x, y wrapped, 0..2047) of level L as 0xAABBGGRR (gs_tex_fetch)
uint texel(St st, Lvl L, uint x, uint y) {
    uint psm = (st.tex0clamp.x >> 20) & 0x3Fu;
    bool isview = (st.texsrc & 0xFFu) >= 3u;
    if (isview && (g_voff.x != 0 || g_voff.y != 0)) {
        // (M7) a 1:1 read at a signed sample offset: the sample at that offset from the texel's sample 0, which
        // for a negative offset is a sample of the texel before it (in the view's own texel space: CT16 views too)
        int ox = floordiv_u(g_voff.x, vkx), oy = floordiv_u(g_voff.y, vky);
        x = (uint)((int)x + ox) & 2047u;
        y = (uint)((int)y + oy) & 2047u;
        g_vsample = uint2((uint)(g_voff.x - ox * (int)vkx), (uint)(g_voff.y - oy * (int)vky));
    }
    uint res;
    if (psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S) {
        uint h = gs_addr16(swz_tab_of(psm), L.bp, L.bw, x, y);
        uint w16 = isview ? view_word_at(h >> 1) : vw(h >> 1);
        res = expand16((w16 >> ((h & 1u) * 16u)) & 0xFFFFu, st.texa);
    } else if (psm == PSMT8) {
        uint a = gs_addr8(L.bp, L.bw, x, y);
        res = clut_lookup(st, (vw(a >> 2) >> ((a & 3u) * 8u)) & 0xFFu);
    } else if (psm == PSMT4) {
        uint n4 = gs_addr4(L.bp, L.bw, x, y);
        res = clut_lookup(st, (vw(n4 >> 3) >> ((n4 & 7u) * 4u)) & 15u);
    } else {
        uint v = isview ? view_word(x, y) : vw(gs_addr32(swz_tab_of(psm), L.bp, L.bw, x, y));
        if (psm == PSMT8H) res = clut_lookup(st, v >> 24);
        else if (psm == PSMT4HL) res = clut_lookup(st, (v >> 24) & 15u);
        else if (psm == PSMT4HH) res = clut_lookup(st, v >> 28);
        else if (psm == PSMCT24 || psm == PSMZ24) {
            v &= 0xFFFFFFu;
            uint ta0 = st.texa & 0xFFu, aem = (st.texa >> 15) & 1u;
            res = v | (((aem != 0u && v == 0u) ? 0u : ta0) << 24);
        } else res = v;                  // CT32, Z32 (unknown PSMs read as CT32, as gs_tex_fetch)
    }
    return res;
}
// GS bilinear: 4-bit weights, sum(texel * wu * wv) >> 8 per channel (gs_draw_pixel.c bilerp)
uint bilerp(uint a, uint b, uint c, uint d, uint fu, uint fv) {
    uint r = 0u;
    [unroll] for (uint ch = 0u; ch < 32u; ch += 8u) {
        uint vl = ((a >> ch) & 255u) * (16u - fv) + ((c >> ch) & 255u) * fv;
        uint vr = ((b >> ch) & 255u) * (16u - fv) + ((d >> ch) & 255u) * fv;
        r |= ((vl * (16u - fu) + vr * fu) >> 8) << ch;
    }
    return r;
}

// (M9) One texel() in the code: the nearest read is one tap, the bilinear one four taps of a loop. Every call of
// texel() is inlined with all of its formats, and the unrolled taps of every level read of tex_sample made the
// pixel shaders ~500 KB of DXBC each (and ~8 MB of driver memory each); the reads and their order are unchanged.
uint tex_level(St st, uint l, int U, int V, bool bil) {
    Lvl L = level_of(st, l);
    U >>= l;
    V >>= l;
    uint fu = 0u, fv = 0u, xa, xb = 0u, ya, yb = 0u, n = 1u;
    if (!bil) {
        xa = (uint)wrapc(U >> 4, L.ulo, L.uhi, L.umsk, L.ufix) & 2047u;
        ya = (uint)wrapc(V >> 4, L.vlo, L.vhi, L.vmsk, L.vfix) & 2047u;
    } else {
        U -= 8;
        V -= 8;
        fu = (uint)U & 15u; fv = (uint)V & 15u;
        int u0 = U >> 4, v0 = V >> 4;
        xa = (uint)wrapc(u0, L.ulo, L.uhi, L.umsk, L.ufix) & 2047u; xb = (uint)wrapc(u0 + 1, L.ulo, L.uhi, L.umsk, L.ufix) & 2047u;
        ya = (uint)wrapc(v0, L.vlo, L.vhi, L.vmsk, L.vfix) & 2047u; yb = (uint)wrapc(v0 + 1, L.vlo, L.vhi, L.vmsk, L.vfix) & 2047u;
        n = 4u;
    }
    uint t0 = 0u, t1 = 0u, t2 = 0u, t3 = 0u;
    [loop] for (uint k = 0u; k < n; k++) {
        uint t = texel(st, L, (k & 1u) ? xb : xa, (k & 2u) ? yb : ya);
        if (k == 0u) t0 = t; else if (k == 1u) t1 = t; else if (k == 2u) t2 = t; else t3 = t;
    }
    return bil ? bilerp(t0, t1, t2, t3, fu, fv) : t0;
}

uint lerp_rgba(uint a, uint b, uint f) {   // f 0..16
    uint r = 0u;
    [unroll] for (uint ch = 0u; ch < 32u; ch += 8u) r |= ((((a >> ch) & 255u) * (16u - f) + ((b >> ch) & 255u) * f) >> 4) << ch;
    return r;
}

// LOD in 1/16 units: LCM 0: -(log2|Q| as 4-bit fraction) << L + K, LCM 1: K
int lod16_of(St st, float q) {
    int k = (int)(st.tlev << 4) >> 20;   // signed 12 bits at bit 16
    if ((st.tlev >> 12) & 1u) return k;
    uint u = asuint(q) & 0x7FFFFFFFu;
    if (u == 0u) return 0x7FFF;
    int e = (int)((u >> 23) & 0xFFu) - 127, m = (int)((u >> 19) & 15u);
    return -(e * 16 + m) * (1 << ((st.tlev >> 13) & 3u)) + k;
}

// (M7) a non-1:1 read of a scaled target view at a fragment sample other than sample 0: the view's own sample
// grid is sampled (texel x's sample i was drawn at texel-space position x + 0.5 + i / vkx), nearest or with the
// GS's 4-bit bilinear weights in sample units; clamps act on the native texel, an edge texel's outer samples
// stand for it. Sample 0 keeps the native read (exact).
uint view_texel_s(St st, Lvl L, int sx, int sy) {
    int k1 = (int)vkx, k2 = (int)vky;
    int x = floordiv_u(sx, vkx), y = floordiv_u(sy, vky);
    int i = sx - x * k1, j = sy - y * k2;
    if (x < L.ulo) i = 0; else if (x > L.uhi) i = k1 - 1;
    if (y < L.vlo) j = 0; else if (y > L.vhi) j = k2 - 1;
    uint xw = (uint)wrapc(x, L.ulo, L.uhi, L.umsk, L.ufix) & 2047u, yw = (uint)wrapc(y, L.vlo, L.vhi, L.vmsk, L.vfix) & 2047u;
    g_vsample = uint2((uint)i, (uint)j);
    uint t = texel(st, L, xw, yw);
    g_vsample = uint2(0u, 0u);
    return t;
}

uint view_hires(St st, float uf, float vf, bool bil) {
    Lvl L = level_of(st, 0u);
    float ps = (uf - 8.0) * (float)vkx / 16.0, qs = (vf - 8.0) * (float)vky / 16.0;   // sample units
    // (M9) one view_texel_s() in a loop of one or four taps (as tex_level)
    int sx, sy;
    uint fu = 0u, fv = 0u, n = 1u;
    if (!bil) { sx = (int)floor(ps + 0.5); sy = (int)floor(qs + 0.5); }
    else {
        float fx = floor(ps), fy = floor(qs);
        sx = (int)fx; sy = (int)fy;
        fu = (uint)((ps - fx) * 16.0); fv = (uint)((qs - fy) * 16.0);
        n = 4u;
    }
    uint t00 = 0u, t10 = 0u, t01 = 0u, t11 = 0u;
    [loop] for (uint k = 0u; k < n; k++) {
        uint t = view_texel_s(st, L, sx + (int)(k & 1u), sy + (int)(k >> 1));
        if (k == 0u) t00 = t; else if (k == 1u) t10 = t; else if (k == 2u) t01 = t; else t11 = t;
    }
    return bil ? bilerp(t00, t10, t01, t11, fu, fv) : t00;
}

// (M9) the level(s) and filter first, then one tex_level() in a loop of one or two levels (see tex_level)
uint tex_sample(St st, int U, int V, float lq) {
    uint texm = st.tlev & 3u, cur = (st.tlev >> 2) & 7u;
    uint l = cur, nl = 1u, lf = 0u;
    bool bl = texm == 2u;
    if (texm == 3u) {
        uint mxl = (st.tlev >> 5) & 7u, mmag = (st.tlev >> 8) & 1u, mmin = (st.tlev >> 9) & 7u;
        int lod = lod16_of(st, lq);
        if (lod <= 0) { l = 0u; bl = mmag != 0u; }
        else if (mmin == 0u) { l = 0u; bl = false; }
        else if (mmin == 1u) { l = 0u; bl = true; }
        else if (mmin == 2u || mmin == 4u) {                             // *_MIPMAP_NEAREST
            uint m = (uint)((lod + 8) >> 4);
            l = m > mxl ? mxl : m;
            bl = mmin == 4u;
        } else {                                                        // *_MIPMAP_LINEAR
            l = (uint)(lod >> 4);
            bl = mmin == 5u;
            if (l >= mxl) l = mxl;
            else { nl = 2u; lf = (uint)lod & 15u; }
        }
    }
    uint r0 = 0u, r1 = 0u;
    [loop] for (uint k = 0u; k < nl; k++) {
        uint t = tex_level(st, l + k, U, V, bl);
        if (k == 0u) r0 = t; else r1 = t;
    }
    return nl == 2u ? lerp_rgba(r0, r1, lf) : r0;
}
// texture function (tfx_batch): colour x texel, GS alpha 0x80 = 1.0
uint tfx(uint tf, bool tcc, uint c, uint t) {
    uint cr = c & 255u, cg = (c >> 8) & 255u, cb = (c >> 16) & 255u, ca = c >> 24;
    uint tr = t & 255u, tg = (t >> 8) & 255u, tb = (t >> 16) & 255u, ta = t >> 24;
    if (tf == 0u) {                      // MODULATE
        uint r = min((cr * tr) >> 7, 255u), g = min((cg * tg) >> 7, 255u), b = min((cb * tb) >> 7, 255u);
        uint a = tcc ? min((ca * ta) >> 7, 255u) : ca;
        return r | (g << 8) | (b << 16) | (a << 24);
    }
    if (tf == 1u) return tcc ? t : (t & 0xFFFFFFu) | (c & 0xFF000000u);   // DECAL
    uint r = min(((cr * tr) >> 7) + ca, 255u), g = min(((cg * tg) >> 7) + ca, 255u), b = min(((cb * tb) >> 7) + ca, 255u);
    uint a = !tcc ? ca : tf == 2u ? min(ta + ca, 255u) : ta;              // HIGHLIGHT, HIGHLIGHT2
    return r | (g << 8) | (b << 16) | (a << 24);
}

bool atst_pass(uint atst, uint aref, uint a) {
    switch (atst) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return a < aref;
    case 3u: return a <= aref;
    case 4u: return a == aref;
    case 5u: return a >= aref;
    case 6u: return a > aref;
    default: return a != aref;
    }
}

int sel3(uint s, int src, int dst) { return s == 0u ? src : s == 1u ? dst : 0; }

// ---- perspective S/T/Q of triangles (gs_draw.c draw_triangle, gs_draw_pixel.c gs_batch_span) ------
// the first pixel of row py inside the left edges (gs_tri_row's xl)
int edge_min_x(int a, int dc, i64 c0, int rows) {
    i64 c = i64_add(c0, i64_mul_i32(i64_make(dc), rows));
    float cf = (float)asint(c.y) * 4294967296.0 + (float)c.x;   // (an estimate: corrected exactly below)
    float est = ceil(-cf / (float)a);
    int x = est < -4096.0 ? -4096 : est > 4096.0 ? 4096 : (int)est;
    [loop] for (int i1 = 0; i1 < 64 && i64_is_neg(i64_add(i64_mul_i32(i64_make(a), x), c)); i1++) x++;
    [loop] for (int i2 = 0; i2 < 64 && !i64_is_neg(i64_add(i64_mul_i32(i64_make(a), x - 1), c)); i2++) x--;
    return x;
}

// S at pixel x of a span starting at xl with value s0: per pixel s += ds, and each new piece
// (64-pixel columns, the batch cut at cutx) restarts with s + ds * take
float step_span(float s0, float ds, int xl, int x, int cutx) {
    precise float s = s0;
    int xs = xl;
    [loop] for (int it = 0; it < 64; it++) {
        int nb = (xs & ~63) + 64;
        if (cutx > xs && cutx < nb) nb = cutx;
        if (nb > x) break;
        precise float p = fmul_r(ds, (float)(nb - xs));
        s = s + p;
        xs = nb;
    }
    [loop] for (int k = xs; k < x && k < xs + 64; k++) s = s + ds;     // (< 64: pieces end at columns)
    return s;
}

// ---- fragment evaluation (shared by the REF/ROV pixel shader and the FF pixel shaders) -----------------
#define TK_VIEW_C32 3u

#ifndef STQ_FAST
#define STQ_FAST 0                       // FF triangles: S/T/Q interpolated by the hardware (128-byte records)
#endif
#ifndef TRI_REC
#define TRI_REC 256u                     // triangle record stride (FF fast path: the 128-byte prefix)
#endif

struct Frag {
    uint col;                            // 0xAABBGGRR after texture, TFX and fog
    uint vcol;                           // the primitive's colour at the pixel (probe)
    uint fog;                        // 0..255
    uint z;                              // stored-format Z (clamped)
    int U, V;                            // 1/16 texel
    float uf, vf;                        // (M7) U, V at the fragment's sample, unrounded (scaled view reads)
    float lq;                            // Q for the LOD
    uint state;
    uint rb;                             // record byte offset
    int2 P;                              // D3D pixel (sample)
    int px, py;                          // native pixel
};

static float dbg_s = 0.0, dbg_t = 0.0, dbg_q = 0.0;
static int dbg_xl = 0;

// attributes from the record, texture, TFX, fog; false: the row is masked (SCANMSK)
bool gs_frag(float4 pos, uint irec, float3 istq, uint prim, out Frag f, out St st) {
    f.P = (int2)pos.xy;
    f.px = (int)((uint)f.P.x / kx);
    f.py = (int)((uint)f.P.y / ky);
    int2 soff = f.P - int2(f.px * (int)kx, f.py * (int)ky);   // the sample's offset from sample 0 of its pixel (0..k-1)
    int px = f.px, py = f.py;
    f.U = 0; f.V = 0; f.lq = 1.0;
    f.uf = 0.0; f.vf = 0.0;
    bool uvf = false;                    // (M7) uf, vf set by the primitive class (sprites), else from U, V
    f.col = 0u; f.vcol = 0u; f.fog = 0u; f.z = 0u;
    bool one_to_one = false;             // a sprite stepping one texel per pixel (1:1 rule)
#if PRIM == 0
    uint rb = rec_first + prim * TRI_REC;
    f.state = recs.Load(rb + 112u);
#elif PRIM == 1
    uint rb = irec;
    f.state = recs.Load(rb + 20u);
#elif PRIM == 3
    uint rb = irec;
    f.state = recs.Load(rb + 28u);
#else
    uint rb = irec;
    f.state = recs.Load(rb + 12u) >> 8;
#endif
    f.rb = rb;
    st = load_state(f.state);
    uint scanmsk = (st.flags >> 14) & 3u;
#if PRIM != 2
    if ((scanmsk == 2u && (py & 1) == 0) || (scanmsk == 3u && (py & 1) != 0)) return false;
#endif
    bool tme = (st.flags & SF_TME) != 0u, fst = (st.flags & SF_FST) != 0u;
    uint zbits = (st.zinfo >> 4) & 3u;
    uint zmax = zbits == 3u ? 0xFFFFFFFFu : zbits == 2u ? 0xFFFFFFu : 0xFFFFu;
    uint t0 = st.tex0clamp.x, t1 = st.tex0clamp.y;
    uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
    float tw16 = (float)(16u << (tw > 10u ? 10u : tw)), th16 = (float)(16u << (th > 10u ? 10u : th));

#if PRIM == 0
    // ---- triangle: the software renderer's fixed-point row stepping ----
    // (M7) sample (si, sj) of the native pixel sits at GS (px + si / kx, py + sj / ky): attributes are
    // evaluated there (Z exactly as 3.6, the others with float sub-steps); sample 0 is the native pixel's
    // value bit for bit
    uint si = (uint)f.P.x - (uint)px * kx, sj = (uint)f.P.y - (uint)py * ky;
    float fi = (float)si / (float)kx, fj = (float)sj / (float)ky;
    int by0 = (int)(recs.Load(rb + 108u) << 16) >> 16, bx0 = (int)recs.Load(rb + 108u) >> 16;
    int rows = py - by0;
    int a[7];
    [unroll] for (uint k = 0u; k < 7u; k++) {
        uint dyk = recs.Load(rb + 52u + 4u * k), dxk = recs.Load(rb + 80u + 4u * k);
        uint v = recs.Load(rb + 24u + 4u * k) + dyk * (uint)rows + dxk * (uint)px;
        if (sj != 0u) v += (uint)(int)floor((float)asint(dyk) * fj);
        if (si != 0u) v += (uint)(int)floor((float)asint(dxk) * fi);
        a[k] = asint(v);
    }
    f.col = clamp255i(a[0] >> 16) | (clamp255i(a[1] >> 16) << 8) | (clamp255i(a[2] >> 16) << 16) | (clamp255i(a[3] >> 16) << 24);
    f.fog = clamp255i(a[4] >> 16);
    i64 z16 = gs_z16_sample(recs.Load2(rb), recs.Load2(rb + 8u), recs.Load2(rb + 16u), by0, px, py, si, sj, kx, ky);
    f.z = gs_z_clamp(z16, zmax);
    f.lq = asfloat(recs.Load(rb + 116u));
    if (tme) {
        if (fst) { f.U = a[5] >> 12; f.V = a[6] >> 12; }
        else {
#if STQ_FAST
            // (FF fast path: S/T/Q as the hardware interpolates them; the design's tolerance class
            // "perspective-textured")
            float s = istq.x, t = istq.y, q = istq.z;
            dbg_s = s; dbg_t = t; dbg_q = q;
            float iq = 1.0 / q;
            precise float su = s * iq;
            precise float tv = t * iq;
            precise float su16 = su * tw16;
            precise float tv16 = tv * th16;
            f.U = f2i(su16);
            f.V = f2i(tv16);
            f.lq = q;
#else
            // span start: xl from the left edges, S/T/Q of the row (doubles, stepped per row), then
            // the float steps along the span's pieces
            int xl = bx0;
            uint ne = recs.Load(rb + 224u);
            [loop] for (uint e = 0u; e < ne; e++) {
                int m = edge_min_x(asint(recs.Load(rb + 192u + 4u * e)), asint(recs.Load(rb + 200u + 4u * e)),
                                   recs.Load2(rb + 208u + 8u * e), rows);
                xl = m > xl ? m : xl;
            }
            xl = xl > px ? px : xl;      // (the pixel is inside its span: bounded loops whatever the estimate)
            int cutx = -1;
            uint ncuts = recs.Load(rb + 232u);
            if (ncuts != 0u) {
                uint co = recs.Load(rb + 228u);
                [loop] for (uint c = 0u; c < ncuts; c++) {
                    uint e2 = fdata.Load(co + 4u * c);
                    if ((int)(e2 << 16) >> 16 == py) { cutx = (int)e2 >> 16; break; }
                }
            }
            // (16-byte loads of the record's S/T/Q block at 112..191)
            uint4 w0 = recs.Load4(rb + 112u), w1 = recs.Load4(rb + 128u), w2 = recs.Load4(rb + 144u), w3 = recs.Load4(rb + 160u),
                  w4 = recs.Load4(rb + 176u);
            double srow = asdouble(w0.z, w0.w), trow = asdouble(w1.x, w1.y), qrow = asdouble(w1.z, w1.w);
            double sdy = asdouble(w2.x, w2.y), tdy = asdouble(w2.z, w2.w), qdy = asdouble(w3.x, w3.y);
            double sdx = asdouble(w3.z, w3.w), tdx = asdouble(w4.x, w4.y), qdx = asdouble(w4.z, w4.w);
            [loop] for (int r = 0; r < rows && r < 2048; r++) { srow = srow + sdy; trow = trow + tdy; qrow = qrow + qdy; }
            double dxl = (double)xl;
            precise double ps = dmul_r(sdx, dxl), pt = dmul_r(tdx, dxl), pq = dmul_r(qdx, dxl);
            precise double ss = srow + ps, st2 = trow + pt, sq = qrow + pq;
            float s0 = (float)ss, tt0 = (float)st2, q0 = (float)sq;
            float s = step_span(s0, (float)sdx, xl, px, cutx);
            float t = step_span(tt0, (float)tdx, xl, px, cutx);
            float q = step_span(q0, (float)qdx, xl, px, cutx);
            if ((si | sj) != 0u) {
                // (M7) another sample of the pixel: the plane at its position. (Its native row can be the one above
                // the triangle's first, rows = -1: a sample below that row's centre is covered while the centre is
                // not - the row loop above never runs backwards, so the plane starts from row by0's values.)
                double dfx = (double)px + (double)fi, dfy = (double)rows + (double)fj;
                double s0r = asdouble(w0.z, w0.w), t0r = asdouble(w1.x, w1.y), q0r = asdouble(w1.z, w1.w);
                s = (float)(s0r + sdy * dfy + sdx * dfx);
                t = (float)(t0r + tdy * dfy + tdx * dfx);
                q = (float)(q0r + qdy * dfy + qdx * dfx);
            }
            float iq = rcp_exact(q);
            precise float su = s * iq;
            precise float tv = t * iq;
            precise float su16 = su * tw16;
            precise float tv16 = tv * th16;
            f.U = f2i(su16);
            f.V = f2i(tv16);
            f.lq = q;
            dbg_s = s; dbg_t = t; dbg_q = q; dbg_xl = xl;
#endif
        }
    }
#elif PRIM == 1
    // ---- sprite: flat colour, U per pixel, V per row (gs_span_start_at) ----
    uint4 r = recs.Load4(rb + 8u);
    f.col = r.x;
    f.z = r.y;
    f.fog = r.z & 255u;
    f.lq = asfloat(recs.Load(rb + 48u));
    one_to_one = (recs.Load(rb + 60u) & 1u) != 0u;
    if (tme) {
        int x0 = (int)(recs.Load(rb) << 16) >> 16;
        int ua = asint(recs.Load(rb + 24u)), du = asint(recs.Load(rb + 28u));
        f.U = asint((uint)ua + (uint)du * (uint)(px - x0)) >> 12;
        double vc0 = asdouble(recs.Load(rb + 32u), recs.Load(rb + 36u)), vcy = asdouble(recs.Load(rb + 40u), recs.Load(rb + 44u));
        precise double vp = dmul_r(vcy, (double)py);
        precise double vs = vc0 + vp;
        precise double v = vs * 4096.0L;
        int vi = v > 2147483000.0L ? 2147483000 : v < -2147483000.0L ? -2147483000 : (int)v;
        f.V = vi >> 12;
        // (M7) at the sample's position (scaled view reads; single-rate otherwise)
        float si = (float)soff.x / (float)kx, sjf = (float)soff.y / (float)ky;
        f.uf = ((float)asint((uint)ua + (uint)du * (uint)(px - x0)) + (float)du * si) / 4096.0;
        f.vf = (float)(vc0 + vcy * ((double)py + (double)sjf));
        uvf = true;
    }
#elif PRIM == 3
    // ---- (M7) thin analytic line: the sample's distance to the line decides its coverage ----
    {
        float4 g = asfloat(recs.Load4(rb));          // lo, hi, a0, len (12.4)
        float2 o = asfloat(recs.Load2(rb + 16u));    // o0, od
        uint lf = recs.Load(rb + 24u);
        bool xm = (lf & 1u) != 0u;
        float gx = (float)f.P.x / (float)kx, gy = (float)f.P.y / (float)ky;   // the sample's GS position
        float mj = (xm ? gx : gy) * 16.0, mn = (xm ? gy : gx) * 16.0;
        if (mj < g.x || mj >= g.y) return false;
        float t = (mj - g.z) / g.w;
        float d = (mn - (o.x + o.y * t)) / 16.0;    // native pixels across the minor axis
        float w = asfloat(recs.Load(rb + 108u));
        uint cov = 0x80u;
        if ((lf & 2u) != 0u) {                      // AA1: coverage 1 - |d| / w replaces the alpha
            float c = 1.0 - abs(d) / w;
            if (c <= 0.0) return false;
            cov = (uint)round(c * 128.0);
            if (cov == 0u) return false;
        } else if (abs(d) >= w * 0.5) return false;
        float2 zz = asfloat(recs.Load2(rb + 32u));
        float zf = floor(zz.x + zz.y * t);
        f.z = zf <= 0.0 ? 0u : zf >= (float)zmax ? zmax : min((uint)zf, zmax);
        float4 c0 = asfloat(recs.Load4(rb + 40u)), cx = asfloat(recs.Load4(rb + 56u));
        float4 cv = floor(c0 + cx * t);
        uint ca = (lf & 2u) != 0u ? cov : clamp255i((int)cv.w);
        f.col = clamp255i((int)cv.x) | (clamp255i((int)cv.y) << 8) | (clamp255i((int)cv.z) << 16) | (ca << 24);
        float2 ff = asfloat(recs.Load2(rb + 72u));
        f.fog = clamp255i((int)floor(ff.x + ff.y * t));
        f.lq = asfloat(recs.Load(rb + 104u));
        if (tme) {
            float4 s01 = asfloat(recs.Load4(rb + 80u));   // sc0[0..2], scx[0]
            float2 s23 = asfloat(recs.Load2(rb + 96u));   // scx[1..2]
            float s = s01.x + s01.w * t, tt = s01.y + s23.x * t;
            if (fst) { f.U = (int)floor(s); f.V = (int)floor(tt); }
            else {
                float q = s01.z + s23.y * t, iq = 1.0 / q;
                precise float su = s * iq;
                precise float tv = tt * iq;
                precise float su16 = su * tw16;
                precise float tv16 = tv * th16;
                f.U = f2i(su16);
                f.V = f2i(tv16);
                f.lq = q;
            }
        }
    }
#else
    // ---- line / point pixel: as the DDA produced it ----
    uint4 r = recs.Load4(rb);
    f.z = r.y;
    f.col = r.z;
    f.fog = r.w & 255u;
    uint3 stq = recs.Load3(rb + 16u);
    f.lq = asfloat(recs.Load(rb + 28u));
    if (tme) {
        if (fst) { f.U = asint(stq.x) >> 12; f.V = asint(stq.y) >> 12; }
        else {
            float q = asfloat(stq.z), iq = rcp_exact(q);
            precise float su = asfloat(stq.x) * iq;
            precise float tv = asfloat(stq.y) * iq;
            precise float su16 = su * tw16;
            precise float tv16 = tv * th16;
            f.U = f2i(su16);
            f.V = f2i(tv16);
            f.lq = q;
        }
    }
#endif
    f.vcol = f.col;

    // ---- texture, TFX, fog ----
    uint col = f.col;
    if (tme && (st.tlev & 3u) != 0u) {
        // target views: 1:1 reads take the fragment's own sample of each texel (5.1), others sample 0
        g_vself = (st.texsrc & TS_SELF) != 0u;
        // 1:1 reads: the source sample at the fragment's own offset from the texel (texel(); M7: signed for the
        // single-rate classes, across the texel's neighbour; a view with other factors maps it to its own grid -
        // the frontend pairs the families of 1:1 reads)
        if (one_to_one) g_voff = int2(floordiv_u(soff.x * (int)vkx, kx), floordiv_u(soff.y * (int)vky, ky));
        uint tk = st.texsrc & 0xFFu, vpsm = (st.tex0clamp.x >> 20) & 0x3Fu;
        bool s0 = soff.x == 0 && soff.y == 0;
        uint t;
        if ((tk == 3u || tk == 4u) && !one_to_one && !g_vself && !s0 && (vkx > 1u || vky > 1u) && (st.tlev & 3u) != 3u &&
            vpsm != PSMCT16 && vpsm != PSMCT16S) {
            // (M7) a scaled view read through its own samples (sample 0 stays the native read)
            if (!uvf) { f.uf = (float)f.U; f.vf = (float)f.V; }
            t = view_hires(st, f.uf, f.vf, (st.tlev & 3u) == 2u);
        } else t = tex_sample(st, f.U, f.V, f.lq);
        col = tfx((st.flags >> 2) & 3u, (st.flags & SF_TCC) != 0u, col, t);
    }
    if (st.flags & SF_FGE) {
        uint fv = f.fog, fc = st.fogcol;
        uint r = (fv * (col & 255u) + (255u - fv) * (fc & 255u)) >> 8;
        uint g = (fv * ((col >> 8) & 255u) + (255u - fv) * ((fc >> 8) & 255u)) >> 8;
        uint b = (fv * ((col >> 16) & 255u) + (255u - fv) * ((fc >> 16) & 255u)) >> 8;
        col = r | (g << 8) | (b << 16) | (col & 0xFF000000u);
    }
    f.col = col;
    return true;
}

// ---- the REF / ROV pixel shader ------------------------------------------------------------------------
#if !FF
#if PRIM == 0 && STQ_FAST
void ps_gs(VF i, uint prim : SV_PrimitiveID) {   // (mixed frames, default mode: 128-byte records, hardware S/T/Q)
    float3 istq = i.stq;
#else
void ps_gs(VR i
#if PRIM == 0
           , uint prim : SV_PrimitiveID
#endif
           ) {
    float3 istq = float3(0.0, 0.0, 1.0);
#endif
#if PRIM != 0
    uint prim = 0u;
#endif
    load_draw(i.did);
    Frag f;
    St st;
    if (!gs_frag(i.pos, i.rec, istq, prim, f, st)) return;
    int2 P = f.P;
    int px = f.px, py = f.py;
    uint rb = f.rb;
    uint col = f.col;
    uint zbits = (st.zinfo >> 4) & 3u;

    bool probing = (uint)px == probe_x && (uint)py == probe_y;
    if (probing) probe_rec(1u, rb, f.state, f.U, f.V, col, f.vcol, f.z, dbg_s, dbg_t, dbg_q, dbg_xl, i.did, 0u, 0u, px, py);

    // ---- tests: alpha, destination alpha, Z (+ Z write) ----
    // 16-bit frames (ALIAS16 runs): rov_c is the staging texture holding the pixel's 16-bit value
    bool fb16 = (st.flags & SF_FB16) != 0u;
    uint As = col >> 24;
    uint test = st.test;
    int mode = 1;
    bool zw = true;
    if ((test & 1u) && !atst_pass((test >> 1) & 7u, (test >> 4) & 0xFFu, As)) {
        uint afail = (test >> 12) & 3u;
        if (afail == 0u) return;                 // KEEP
        if (afail == 1u) zw = false;             // FB_ONLY
        else if (afail == 2u) mode = 0;          // ZB_ONLY
        else { zw = false; mode = 2; }           // RGB_ONLY
    }
    bool hasc = (draw_flags & 1u) != 0u, hasz = (draw_flags & 2u) != 0u;
    uint dst = 0u;
    bool have = false;
    if ((test >> 14) & 1u) {                     // DATE: bit 31 (bit 15 of 16-bit frames)
        dst = hasc ? rov_c[P] : 0u;
        have = true;
        if (((dst >> (fb16 ? 15u : 31u)) & 1u) != ((test >> 15) & 1u)) return;
    }
    if (((st.zinfo >> 3) & 1u) != 0u && hasz) {
        uint ztst = st.zinfo & 3u, zmsk = (st.zinfo >> 2) & 1u;
        uint zold = rov_z[P];
        uint zb = zbits == 1u ? zold & 0xFFFFu : zbits == 2u ? zold & 0xFFFFFFu : zold;
        uint zv = f.z;
        if (ztst == 0u) return;                  // NEVER
        if (ztst == 2u && zv < zb) return;       // GEQUAL
        if (ztst == 3u && zv <= zb) return;      // GREATER
        if (zw && zmsk == 0u) rov_z[P] = zbits == 2u ? (zold & 0xFF000000u) | zv : zv;
        if (probing) probe_rec(3u, rb, f.state, f.U, f.V, col, (zw ? 1u : 0u) | (st.zinfo << 4) | (draw_flags << 16), zv, 0.0, 0.0, 0.0, mode, zold, rov_z[P], 0u, px, py);
    }
    if (mode == 0 || !hasc) return;

    // ---- blend, dither, FBA, write mask (gs_draw_pixel.c write_seg) ----
    uint m = st.fmask;                           // (16-bit frames: the 16-bit form)
    if (mode == 2) m |= fb16 ? 0x8000u : 0xFF000000u;
    uint c = col;
    bool fb24 = (st.flags & SF_FB24) != 0u;
    bool dthe = fb16 && (st.flags & SF_DTHE) != 0u;
    int dm = 0;
    if (dthe) {                                  // DIMX entry (row y & 3, column x & 3), 3-bit signed
        uint sh = ((uint)py & 3u) * 16u + ((uint)px & 3u) * 4u;
        uint e = ((sh < 32u ? st.dimx.x >> sh : st.dimx.y >> (sh - 32u))) & 7u;
        dm = e >= 4u ? (int)e - 8 : (int)e;
    }
    bool clampc = (st.flags & SF_COLCLAMP) != 0u;
    if ((st.flags & SF_ABE) && (!(st.flags & SF_PABE) || (As & 0x80u))) {
        if (!have) {
            dst = rov_c[P];
            have = true;
        }
        uint al = st.alpha;
        uint asel = al & 3u, bsel = (al >> 2) & 3u, csel = (al >> 4) & 3u, dsel = (al >> 6) & 3u, fix = (al >> 8) & 255u;
        uint dx = fb16 ? ((dst << 3) & 0xF8u) | ((dst << 6) & 0xF800u) | ((dst << 9) & 0xF80000u) : dst;
        int ea = fb16 ? ((dst & 0x8000u) != 0u ? 0x80 : 0) : fb24 ? 0x80 : (int)(dst >> 24);
        int cc = csel == 0u ? (int)As : csel == 1u ? ea : (int)fix;
        uint o = 0u;
        [unroll] for (uint ch = 0u; ch < 24u; ch += 8u) {
            int cs = (int)((col >> ch) & 255u), cd = (int)((dx >> ch) & 255u);
            int v = (((sel3(asel, cs, cd) - sel3(bsel, cs, cd)) * cc) >> 7) + sel3(dsel, cs, cd) + dm;
            uint u = clampc ? clamp255i(v) : ((uint)v & 255u);
            o |= u << ch;
        }
        c = o | (col & 0xFF000000u);
    } else if (dthe) {
        uint o = 0u;
        [unroll] for (uint ch = 0u; ch < 24u; ch += 8u) {
            int v = (int)((col >> ch) & 255u) + dm;
            o |= (clampc ? clamp255i(v) : ((uint)v & 255u)) << ch;
        }
        c = o | (col & 0xFF000000u);
    }
    if (st.flags & SF_FBA) c |= 0x80000000u;
    if (fb16) c = ((c >> 3) & 0x1Fu) | ((c >> 6) & 0x3E0u) | ((c >> 9) & 0x7C00u) | ((c >> 16) & 0x8000u);
    if (m != 0u) {
        if (!have) dst = rov_c[P];
        c = (dst & m) | (c & ~m);
    }
    rov_c[P] = c;
    if (probing) probe_rec(2u, rb, f.state, f.U, f.V, col, f.vcol, f.z, 0.0, 0.0, 0.0, mode, 0u, dst, c, P.x, P.y);   // (the sample position)
}
#endif

// ---- the FF pixel shaders (M5, notes/d3d11-renderer.md 4.2) -------------------------------------------
// The fragment is evaluated exactly as above; the output merger does what is left: the Z test and write
// (exact integer Z through SV_Depth, 3.6), the blend (table 4.2), the byte write mask. Colours are raw GS
// bytes in a UNORM target: a byte b is output as b / 255 (exact through the UNORM conversion).
#ifndef DUAL
#define DUAL 0
#endif
#ifndef ZOUT
#define ZOUT 0
#endif
#define BL_OFF 0u
#define BL_NOCOLOR 1u
#define BL_ADD 2u
#define BL_REVSUB 3u
#define BL_DUAL 4u

struct FOut {
    float4 c0 : SV_Target0;
#if DUAL
    float4 c1 : SV_Target1;
#endif
#if ZOUT
    float z : SV_Depth;
#endif
};

float b2f(uint b) { return (float)b * (1.0 / 255.0); }

#if PRIM == 0
FOut ps_ff(VF i, uint prim : SV_PrimitiveID) {
    float3 istq = i.stq;
#else
FOut ps_ff(VR i) {
    uint prim = 0u;
    float3 istq = float3(0.0, 0.0, 1.0);
#endif
    load_draw(i.did);
    Frag f;
    St st;
    if (!gs_frag(i.pos, i.rec, istq, prim, f, st)) discard;
    uint col = f.col;
    uint As = col >> 24;
    uint test = st.test;
    bool apass = (test & 1u) == 0u || atst_pass((test >> 1) & 7u, (test >> 4) & 0xFFu, As);
    if (!apass && ((test >> 12) & 3u) == 0u) discard;             // AFAIL KEEP
    if (atst_mode == 1u && !apass) discard;                       // ATST split: the pass draw
    if (atst_mode == 2u && apass) discard;                        //             the fail draw
    FOut o;
    uint cs[3] = { col & 255u, (col >> 8) & 255u, (col >> 16) & 255u };
    float3 rgb = float3(b2f(cs[0]), b2f(cs[1]), b2f(cs[2]));
    float a1 = 0.0;
    if ((st.flags & SF_ABE) != 0u) {
        uint al = st.alpha;
        uint asel = al & 3u, bsel = (al >> 2) & 3u, csel = (al >> 4) & 3u, dsel = (al >> 6) & 3u, fix = (al >> 8) & 255u;
        int cc = csel == 0u ? (int)As : (int)fix;                // (C = Ad is never FF)
        bool clampc = (st.flags & SF_COLCLAMP) != 0u;
        [unroll] for (uint ch = 0u; ch < 3u; ch++) {
            int s = (int)cs[ch];
            uint u;
            if (ff_blend == BL_ADD) u = min((uint)(s * cc) >> 7, 255u);                       // + Cd (ONE, ONE, ADD)
            else if (ff_blend == BL_REVSUB) u = min((uint)(s * cc + 127) >> 7, 255u);         // Cd - (REV_SUBTRACT)
            else if (ff_blend == BL_DUAL) u = 0u;
            else {                                               // BL_OFF: no Cd, computed here
                int v = (((sel3(asel, s, 0) - sel3(bsel, s, 0)) * cc) >> 7) + sel3(dsel, s, 0);
                u = clampc ? clamp255i(v) : ((uint)v & 255u);
            }
            rgb[ch] = b2f(u);
        }
        if (ff_blend == BL_DUAL) {
            // (Cs - Cd) * C >> 7 + Cd = floor(Cs a + Cd (1 - a)), a = C / 128 <= 1: SRC1_ALPHA = a,
            // INV_SRC1_ALPHA. The output merger rounds to nearest, so for 0 < C < 128 the source is lowered
            // by dual_bias / a LSB (gpu_exec.c). With an exact blender (WARP: fp32) a bias of 0.496 gives the
            // floor unless Cs * C < 64; the RTX 4060 Ti blends UNORM8 targets at about fp16 precision
            // (+-0.125 LSB), where no bias is exact: 0.42 makes 80% of (Cs, Cd, C) exact and the +1 and -1
            // errors equally frequent, so chains of translucent layers do not drift (the design's +-1 class
            // for dual-source blends, 10.2; measurement in notes/wip/gpu-M5.md).
            // C = 128 (a = 1, As 0x80: the common case) and C = 0 are exact unbiased.
            float a = (float)cc * (1.0 / 128.0);
            a1 = a;
            [unroll] for (uint c2 = 0u; c2 < 3u; c2++) {
                float s = (float)cs[c2];
                if (cc > 0 && cc < 128) s = max(s - dual_bias / a, 0.0);
                rgb[c2] = s * (1.0 / 255.0);
            }
        }
    }
    uint ao = As | (((st.flags & SF_FBA) != 0u) ? 0x80u : 0u);
    o.c0 = float4(rgb, b2f(ao));
#if DUAL
    o.c1 = float4(0.0, 0.0, 0.0, a1);
#endif
#if ZOUT
    o.z = gs_z_encode(f.z);
#endif
    return o;
}
