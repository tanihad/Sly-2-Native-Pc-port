// The modern renderer's Original materials (notes/modern-renderer.md 2.5; runtime/r2/r2_render.c): the VU1 main program's
// prelit vertex function (routines 0x06D0 / 0x0960 / 0x0BE0, notes/wip/r2-m1.md) in the vertex shader, and the GS's own
// texture / TFX / fog / alpha-test arithmetic in the pixel shader (shaders/gs.hlsl's functions, one definition of the GS
// formulas: tex_sample with the 4-bit bilinear, tfx, atst_pass; CLUTs straight from GS memory, R2_CLUT).
//
// Vertices: the VU's own arithmetic bit for bit (the design's "exactness lever", notes/modern-renderer.md 4: round toward
// zero emulated with error-free transforms, vu_mul / vu_add / vu_rcp), so X, Y (12.4), Z, F, S, T, Q equal what the VU
// XGKICKs (level-1 oracle: scripts/r2_mesh_check.c against runtime/r2/r2_vertex.c, the C twin). They are final screen
// positions (w = 1: entries reach R2 only inside the VU's guard band, r2_capture.c inside_guard, so nothing needs
// clipping) on the exact path's sample grid (d3d = (GS - XYOFFSET) / 16 * k + 0.5) through the REF viewport of mixed
// frames (16384 pixels centred on the origin); the rasterizer sees the GS's integer vertices, so coverage and the Z plane
// are the GS's. Depth: the GS Z / 2^20 (the exact path's P1 encoding), floored to the integer the GS would store, as
// SV_DepthLessEqual (early rejection stays on); pass A and pass B compute it identically. Colour, fog and S/T/Q are
// interpolated linearly on screen (as the GS), U = S/Q, V = T/Q per pixel.
#define FF 1
#define SPEC_NOVIEW 1
#define SPEC_NOLOD 1
#define R2_CLUT 1
#define TEXDEC 1                         // (r2-m7b) material word 15: the decoded texture (gpu_exec.c td_r2)
#include "gs.hlsl"
#include "r2_vu.hlsli"

#ifndef R2_DUAL
#define R2_DUAL 0
#endif
#ifndef R2_ZREC
#define R2_ZREC 1                        // 1: exact per-triangle Z records (geometry stage, PS2_R2_ZREC=1); 0: the
                                         // interpolated plane (the default pipelines, perms ps_r2_iz*)
#endif

// (r2-m2) constants of the device, written once (a per-segment constant buffer update made the driver stall the render
// thread for up to 13 ms when the GPU was the bottleneck): the segment's own values are a record in fdata (r2_seg)
cbuffer r2_static : register(b1) {
    float4 r2_misc;                      // dual-source bias (gs.hlsl ps_ff), Z bias (debugging), -, asuint: PS2_R2_DBG
    float4 r2_mft;                       // (r2-m8, R2_MFT) modern frame targets: sx, sy (target pixels per GS pixel), samples
#if R2_REIM
    float4 r2_reim;                      // (reimagine) the key light on screen (x, y: towards it, length = its share in the
                                         // picture plane), surface-map relief strength, cavity strength
    float4 r2_reim2;                     // normal-map strength on characters, -, -, -
#endif
};
#ifndef R2_MFT
#define R2_MFT 0                         // (r2-m8, R8) the modern frame's surfaces (gpu_exec_mft.h): d3d = (GS + 0.5) * s,
                                         // the rasterized Z (GS Z / 2^20 at the vertices; no SV_Depth: MSAA keeps per-sample depth)
#endif
#ifndef R2_MV
#define R2_MV 0                          // (r2-m8) the motion-vector pass (with R2_MFT)
#endif
static float2 g_prev_d3d;                // (R2_MV) the vertex's previous position, surface pixels
#ifndef R2_A2C
#define R2_A2C 0                         // (r2-m8) MSAA: pass-A cut-outs (alpha test GEQUAL / GREATER, failing fragments
                                         // discarded) as coverage instead of a discard (alpha-to-coverage through SV_Coverage)
#endif
#ifndef R2_ENH
#define R2_ENH 0                         // (light) Enhanced lighting (modern frames, runtime/gpu/gpu_light.h): lit sub-batches
                                         // lit per pixel with the same light block and formula (r2_lit_px), optional rim light
#endif
#ifndef R2_ETEX
#define R2_ETEX 0                        // (tex) Enhanced textures (modern frames, runtime/gpu/gpu_tex.h): materials with an
                                         // enhanced image sample it (trilinear + anisotropic, shaders/etex.hlsli)
#endif
#if R2_ETEX
#include "etex.hlsli"
#endif
#ifndef R2_REIM
#define R2_REIM 0                        // (reimagine) Look = Reimagined: the surface maps (relief, cavity, normal-mapped characters)
#endif
#if R2_REIM && R2_ETEX
#include "etn.hlsli"
static float4 g_nm;                      // the texel's surface map (normal x, y; height; cavity)
static float g_nmf;                      // its weight: 0 without a map, fading out where a map texel gets smaller than a pixel
static float2 g_duvx, g_duvy;            // the uv's screen derivatives
static float3 g_dpx, g_dpy;              // (lit meshes) the object-space position's screen derivatives
#endif
#if R2_ENH
static float4 g_lnrm;                    // r2_lit's (skinned) normal x 127 and its w
static uint g_lxb, g_lmw;                // its light block's fdata offset + 1 (0: not lit), the material's ambient add
#endif
// the segment record (gpu_front.h gpu_r2_seg, 32 bytes in fdata at the draw record's +28)
struct R2Seg {
    float4 map;                          // kx, ky, XYOFFSET x, y (12.4)
    uint4 off;                           // byte offsets in fdata: frame block, entries; flags (1 FBA); scissor y0
};
R2Seg r2_seg(uint o) {
    R2Seg s;
    s.map = asfloat(fdata.Load4(o));
    s.off = fdata.Load4(o + 16u);
    return s;
}

struct R2V {
    float3 pos : POSITION;
    uint wbits : WBITS;
    int2 uv : UV;
    uint cola : COLA;
    uint colb : COLB;
    uint batch : BATCH;                  // (r2-m2) the vertex's sub-batch in its mesh
    uint did : DRAWID;                   // the draw record (StartInstanceLocation through the iota buffer)
    uint nrm : NRM;                      // (r2-m3) lit: the source record's normal (s8 x 4)
    uint lcol : LCOL;                    //         its colour (u8 x 4)
    uint wts : WTS;                      //         skinned: its 4 bone weights (u8 x 4)
};

// ---- (r2-m3) lit / skinned sub-batches: the main program's 0x1940 (4-bone skinning of position and normal, the normal
// renormalised x 127 and truncated) and 0x1050 (the light block's 4 lights: f = min(d + d^3, threshold), colour = sum of
// the lights' colours x f + ambient; colour A = the clamped light x the normal's w, colour B = the vertex colour x the
// "highlight" share; runtime/r2/r2_vertex.c r2_lit_eval is the VU-exact twin). Positions use the VU's arithmetic (Z ties
// with exact draws), the colours plain floats (the level-1 gate: within +-1). The entry's extra qwords (fdata): the light
// block (R2_LIGHT_QW = 11: directions transposed [0..2], colours x z x 2 [4..7], ambient [8], thresholds [9], VU 10
// [10]), then the header's qwords from 0x15 (bones: 4 rows each; UV offsets).
float4 r2_xq(uint xb, uint q) { return asfloat(fdata.Load4(xb + q * 16u)); }
float r2_sb(uint w, uint c) { return (float)((int)(w << (24u - 8u * c)) >> 24); }
float r2_ub(uint w, uint c) { return (float)((w >> (8u * c)) & 255u); }
void r2_lit(R2V i, uint4 lm, uint xb, inout float3 p, inout uint cola, inout uint colb, inout int2 uv) {
    uint lit = lm.x & 1u, skin = (lm.x >> 1) & 3u, tanim = lm.x >> 16;
    float4 n = float4(r2_sb(i.nrm, 0u), r2_sb(i.nrm, 1u), r2_sb(i.nrm, 2u), r2_sb(i.nrm, 3u));
    if (skin != 0u) {
        float w[4];
        [unroll] for (uint c = 0u; c < 4u; c++) w[c] = vu_mul(r2_ub(i.wts, c), asfloat(0x3B808081u));
        float3 P = float3(0.0, 0.0, 0.0), N = float3(0.0, 0.0, 0.0);
        [unroll] for (uint k = 0u; k < 4u; k++) {
            uint bo = ((k < 2u ? lm.y : lm.z) >> (16u * (k & 1u))) & 0xFFFFu;
            uint q = 11u + bo - 0x15u;
            float4 M0 = r2_xq(xb, q), M1 = r2_xq(xb, q + 1u), M2 = r2_xq(xb, q + 2u), M3 = r2_xq(xb, q + 3u);
            [unroll] for (uint j = 0u; j < 3u; j++) {
                float t = vu_add(vu_add(vu_add(vu_mul(M0[j], p.x), vu_mul(M1[j], p.y)), vu_mul(M2[j], p.z)), M3[j]);
                P[j] = k == 0u ? vu_mul(t, w[0]) : vu_add(P[j], vu_mul(t, w[k]));
                N[j] += (M0[j] * n.x + M1[j] * n.y + M2[j] * n.z) * w[k];
            }
        }
        p = P;
        if (skin == 2u) n.xyz = (float3)(int3)(N * (rsqrt(dot(N, N)) * 127.0));
    }
    if (lit != 0u) {
        float4 T0 = r2_xq(xb, 0u), T1 = r2_xq(xb, 1u), T2 = r2_xq(xb, 2u), amb = r2_xq(xb, 8u), thr = r2_xq(xb, 9u),
               v10 = r2_xq(xb, 10u);
        float4 d = T0 * n.x + T1 * n.y + T2 * n.z;
        float4 f = min(d + d * (d * d), thr);
        amb.w = amb.w + asfloat(lm.w) + v10.w;
        float4 C = r2_xq(xb, 4u) * f.x + r2_xq(xb, 5u) * f.y + r2_xq(xb, 6u) * f.z + r2_xq(xb, 7u) * f.w + amb;
        float mx = max(max(C.x, C.y), C.z);
        float lim = max(2.0 - mx, 0.0);
        float4 cl = float4(min(C.xyz, 2.0), min(C.w, lim));
        float k = lim - cl.w;
        float den = (2.0 + asfloat(0x3C810247u)) * asfloat(0x427DFF7Du) - k * asfloat(0x427DFF7Du);
        float4 cf = float4(r2_ub(i.lcol, 0u), r2_ub(i.lcol, 1u), r2_ub(i.lcol, 2u), r2_ub(i.lcol, 3u));
#if R2_ENH
        g_lnrm = n; g_lxb = xb + 1u; g_lmw = lm.w;
#endif
        int3 a = (int3)((cl.xyz + v10.xyz * k) * n.w);
        int3 b = (int3)(cf.xyz * cl.w * (cf.w / den));
        cola = ((uint)a.x & 255u) | (((uint)a.y & 255u) << 8) | (((uint)a.z & 255u) << 16) | (((uint)(int)cf.w & 255u) << 24);
        colb = ((uint)b.x & 255u) | (((uint)b.y & 255u) << 8) | (((uint)b.z & 255u) << 16) | (((uint)(int)den & 255u) << 24);
    }
    if (tanim != 0u) uv += asint(fdata.Load2(xb + (11u + tanim - 0x15u) * 16u));
}

#ifndef R2_CLIP
#define R2_CLIP 0                        // (r2-m2) 1: the pipeline of meshes crossing the VU's guard band (r2_gs_clip)
#endif
#ifndef R2_NODISCARD
#define R2_NODISCARD 0                   // (r2-m2) 1: no alpha-test discard (draws whose fragments all pass or all fail)
#endif
#if R2_CLIP
#define R2I                              // perspective-correct: the GPU-clipped triangles' attributes (w != 1)
#else
#define R2I noperspective
#endif

struct R2O {
    noperspective centroid float4 pos : SV_Position;   // (centroid: required with conservative depth output)
    R2I float4 col : COLOR0;             // 0..255
    R2I float fog : FOG;
    R2I float3 stq : TEXCOORD0;          // the VU's S, T, Q
    nointerpolation uint3 mr : MATRULE;  // the material record, the discard rule, the segment record
    nointerpolation int3 gv : GV;        // the GS vertex: X, Y (12.4, XYOFFSET subtracted), Z
#if R2_ENH
    nointerpolation uint4 lx : LITX;     // (light) light block offset + 1 (0: not lit), the ambient add, -, the fade (asuint)
    R2I float4 ln : LNRM;                // the object-space normal x 127, its w
    R2I float4 lc : LCOLV;               // the source record's colour (0..255)
    R2I float3 lv : LVIEW;               // object-space vector to the camera (rim light)
#endif
};

struct R2C {                             // (r2-m2) the clip pipeline's vertex: R2O + the clip-space position and the
    noperspective centroid float4 pos : SV_Position;   // attributes of a GPU-clipped triangle
    R2I float4 col : COLOR0;
    R2I float fog : FOG;
    R2I float3 stq : TEXCOORD0;
    nointerpolation uint3 mr : MATRULE;
    nointerpolation int3 gv : GV;
#if R2_ENH
    nointerpolation uint4 lx : LITX;
    R2I float4 ln : LNRM;
    R2I float4 lc : LCOLV;
    R2I float3 lv : LVIEW;
#endif
    float4 clip : CLIPPOS;               // M x position (the VU's clip space: |x|, |y|, |z| <= w inside its guard band)
    float4 alt : ALT;                    // u, v, 1, F (clipped triangles: perspective-correct U, V; fog without Q)
};

struct R2K {                             // the clip pipeline's rasterized vertex
    noperspective centroid float4 pos : SV_Position;
    R2I float4 col : COLOR0;
    R2I float fog : FOG;
    R2I float3 stq : TEXCOORD0;
    nointerpolation uint3 mr : MATRULE;
    nointerpolation int3 gv : GV;
#if R2_ENH                               // (light: R2O's fields in R2O's order, before the clip distances)
    nointerpolation uint4 lx : LITX;
    R2I float4 ln : LNRM;
    R2I float4 lc : LCOLV;
    R2I float3 lv : LVIEW;
#endif
    float4 cd0 : SV_ClipDistance0;       // the VU's six clip planes: w - x, w + x, w - y, w + y
    float2 cd1 : SV_ClipDistance1;       //                            w - z, w + z
};

struct R2P {                             // the geometry stage's output: R2O + the triangle's exact Z record
    noperspective centroid float4 pos : SV_Position;   // (centroid: required with conservative depth output)
    noperspective float4 col : COLOR0;
    noperspective float fog : FOG;
    noperspective float3 stq : TEXCOORD0;
    nointerpolation uint3 mr : MATRULE;
    nointerpolation uint4 zr0 : ZREC0;   // row (lo, hi), dy (lo, hi): 16.16 at x = 0 of row by0, per row
    nointerpolation uint3 zr1 : ZREC1;   // dx (lo, hi), by0
};

// (r2-m2) draw record (gpu_front.h gpu_r2_draw, 32 bytes; the shaders read the first 16): entry, mode (pass | rule << 2 |
// first batch << 8), mask word offset (~0: every batch), mesh handle. Material records (r2_render.c mat_records, 48
// bytes, t10, 3 per sub-batch from the mesh's base in the mesh table t11): +0 TEX0 lo, hi, CLAMP lo, hi; +16 TEX1, TEST,
// ALPHA (asel..dsel | fix << 8), FOGCOL; +32 fog scale, fog w max, flags (GPU_R2D_* | blend class << 12)
ByteAddressBuffer r2_mats : register(t10);
ByteAddressBuffer r2_mtab : register(t11);
#define R2D_CSEL 1u
#define R2D_ABE 8u
#define R2D_FGE 16u
#define R2D_TCC 32u

// (the VU's arithmetic: vu_mul / vu_add / vu_rcp, r2_vu.hlsli)
void r2_vertex(R2V i, out R2O o, out float4 clip, out float4 alt) {
    uint db = i.did * 32u;               // (StartInstanceLocation: the draw record's fdata offset / 32)
    uint4 dr = fdata.Load4(db);
    uint so = fdata.Load(db + 28u);
    R2Seg sg = r2_seg(so);
    uint rp = dr.y & 3u, b0 = dr.y >> 8;   // (pass: an HLSL keyword)
    bool skip = false;
    if (dr.z != 0xFFFFFFFFu) {           // a batch mask: the batches of the range this draw leaves to another
        uint rel = i.batch - b0;
        skip = ((fdata.Load((dr.z + (rel >> 5)) * 4u) >> (rel & 31u)) & 1u) == 0u;
    }
    uint mat = r2_mtab.Load((dr.w & 0xFFFFFu) * 16u) + i.batch * 3u + rp;   // (r8-perf: mbase, vbase, ibase per mesh)
    uint4 d3 = r2_mats.Load4(mat * 64u + 32u);   // fog scale, fog w max, flags
    uint eb = sg.off.y + dr.x * 144u;     // (gpu_front.h gpu_r2_entry)
    float4 m[4] = { asfloat(fdata.Load4(eb)), asfloat(fdata.Load4(eb + 16u)), asfloat(fdata.Load4(eb + 32u)),
                    asfloat(fdata.Load4(eb + 48u)) };
    float fade = asfloat(fdata.Load(eb + 64u));
    // (r2-m3) lit / skinned sub-batches (r2_lit): position, colours A / B, UV offset from the entry's extra qwords
    uint4 lm = r2_mats.Load4(mat * 64u + 44u);
    float3 ipos = i.pos;
    uint icola = i.cola, icolb = i.colb;
    int2 iuv = i.uv;
    uint xq = fdata.Load(eb + 72u);
#if R2_ENH
    g_lxb = 0u; g_lmw = 0u; g_lnrm = float4(0.0, 0.0, 127.0, 0.0);
#endif
    if (lm.x != 0u && xq != 0u) {
        r2_lit(i, lm, (xq - 1u) * 16u, ipos, icola, icolb, iuv);
        if ((asuint(r2_misc.w) & 32u) != 0u) { icola = 0x800000FFu; icolb = 0u; }   // (PS2_R2_DBG 32: lit entries red)
    }
    float4 c0 = asfloat(fdata.Load4(sg.off.x)), c1 = asfloat(fdata.Load4(sg.off.x + 16u));   // VU 0, VU 1
    // 0x07F8: MULAx / MADDAy / MADDAz / MADDw (row 3 x 1.0 exact), M = world x clip (gpu_r2front.c, VU-exact too)
    float c[4];
    float q;
    uint bx, by, bz;
    if ((asuint(r2_misc.w) & 4u) != 0u) {                   // (PS2_R2_DBG 4: plain float arithmetic, cost experiments)
        [unroll] for (uint j2 = 0u; j2 < 4u; j2++) c[j2] = m[0][j2] * ipos.x + m[1][j2] * ipos.y + m[2][j2] * ipos.z + m[3][j2];
        q = 1.0 / c[3];
        bx = asuint(c1.x + c0.x * c[0] * q);
        by = asuint(c1.y + c0.y * c[1] * q);
        bz = asuint(c1.z + c0.z * c[2] * q);
    } else {
    [unroll] for (uint j = 0u; j < 4u; j++) {
        float acc = vu_mul(m[0][j], ipos.x);
        acc = vu_add(acc, vu_mul(m[1][j], ipos.y));
        acc = vu_add(acc, vu_mul(m[2][j], ipos.z));
        c[j] = vu_add(acc, m[3][j]);
    }
    q = vu_rcp(c[3]);                                       // (inside the guard band: w > 0)
    // XYZF2 from the float bits of VU 1 + VU 0 x (clip x Q): X, Y the low 16 bits, Z bits 4..27
    bx = asuint(vu_add(c1.x, vu_mul(c0.x, vu_mul(c[0], q))));
    by = asuint(vu_add(c1.y, vu_mul(c0.y, vu_mul(c[1], q))));
    bz = asuint(vu_add(c1.z, vu_mul(c0.z, vu_mul(c[2], q))));
    }
    float w = c[3];
    float gx = (float)(bx & 0xFFFFu), gy = (float)(by & 0xFFFFu), gz = (float)((bz >> 4) & 0xFFFFFFu);
    // the exact path's sample grid; positions are final (w = 1): the GS interpolates everything linearly on screen
#if R2_MFT
    // (r2-m8) the modern frame's surfaces: GS pixel centres at d3d (x + 0.5) s; the GS Z at the vertices, rasterized
    precise float dx = ((gx - sg.map.z) * (1.0 / 16.0) + 0.5) * r2_mft.x;
    precise float dy = ((gy - sg.map.w) * (1.0 / 16.0) + 0.5) * r2_mft.y;
    o.pos = float4(dx * (1.0 / 8192.0), -dy * (1.0 / 8192.0), gz * (1.0 / 1048576.0), 1.0);
#if R2_MV
    // (r2-m8) the motion vector: the same vertex through the object's previous world x clip (the entry's prev, +80) and
    // the frame's screen constants, in the surface's pixels (plain floats: no exactness needed); none when the previous
    // position was behind the camera or off the GS's coordinate range
    {
        float4 mp[4] = { asfloat(fdata.Load4(eb + 80u)), asfloat(fdata.Load4(eb + 96u)), asfloat(fdata.Load4(eb + 112u)),
                         asfloat(fdata.Load4(eb + 128u)) };
        float4 cp = mp[0] * ipos.x + mp[1] * ipos.y + mp[2] * ipos.z + mp[3];
        g_prev_d3d = float2(dx, dy);
        if (cp.w > 1e-6) {
            float nx = cp.x / cp.w, ny = cp.y / cp.w;
            if (abs(nx) < 4.0 && abs(ny) < 4.0) {
                float pgx = (float)(asuint(c1.x + c0.x * nx) & 0xFFFFu), pgy = (float)(asuint(c1.y + c0.y * ny) & 0xFFFFu);
                g_prev_d3d = float2(((pgx - sg.map.z) * (1.0 / 16.0) + 0.5) * r2_mft.x, ((pgy - sg.map.w) * (1.0 / 16.0) + 0.5) * r2_mft.y);
            }
        }
    }
#endif
#else
    precise float dx = (gx - sg.map.z) * (sg.map.x * (1.0 / 16.0)) + 0.5;
    precise float dy = (gy - sg.map.w) * (sg.map.y * (1.0 / 16.0)) + 0.5;
    // depth: the rasterized value only bounds the exact Z the pixel shader writes (SV_DepthLessEqual): half a unit above
    // the plane through the integer Z values, so it stays >= the GS's value at every sample
    o.pos = float4(dx * (1.0 / 8192.0), -dy * (1.0 / 8192.0), (gz + 0.5) * (1.0 / 1048576.0), 1.0);
#endif
    o.gv = int3((int)(bx & 0xFFFFu) - (int)sg.map.z, (int)(by & 0xFFFFu) - (int)sg.map.w, (int)((bz >> 4) & 0xFFFFFFu));
    // colour: A (pass A) or B, VU 8.x fade on colour A's alpha / colour B's RGB (0x0BE0, truncated)
    bool csel = (d3.z & R2D_CSEL) != 0u;   // (material flags)
    uint col = csel ? icolb : icola;
    uint4 cc = uint4(col & 255u, (col >> 8) & 255u, (col >> 16) & 255u, col >> 24);
    if (fade < 1.0) {
        if (csel) cc.xyz = (uint3)((float3)cc.xyz * fade);
        else cc.w = (uint)((float)cc.w * fade);
    }
    o.col = (float4)cc;
    // fog: the w word (2^23 + m) + VU 9.y x (MINI(w, VU 9.z) x Q), F = bits 4..11
    float fw = vu_add(asfloat(i.wbits), vu_mul(asfloat(d3.x), vu_mul(min(w, asfloat(d3.y)), q)));
    o.fog = (float)((asuint(fw) >> 4) & 255u);
    // STQ = (ITOF12 u, v, 1) x Q, interpolated linearly on screen and divided per pixel (the GS's S/Q, T/Q)
    o.stq = float3(vu_mul((float)iuv.x * (1.0 / 4096.0), q), vu_mul((float)iuv.y * (1.0 / 4096.0), q), q);
    o.mr = uint3(mat, ((dr.y >> 2) & 1u) | (sg.off.z & 1u) << 1, so);   // (rule | FBA << 1: the PS loads no segment record)
    if (skip) o.pos = float4(2.0, 2.0, 0.5, 1.0);   // (outside the clip volume: the batch's triangles are rejected)
    clip = float4(c[0], c[1], c[2], w);
    if (skip) clip = float4(2.0, 0.0, 0.0, 1.0);    // (outside x <= w: rejected by the clip pipeline too)
    // a clipped triangle's U, V (perspective-correct) and fog (MINI(w, VU 9.z) x Q is 1 while w < VU 9.z, and behind
    // the camera; the VU clipper's new vertices get theirs from the same formula)
    float wq = w < asfloat(d3.y) ? 1.0 : asfloat(d3.y) / w;
    alt = float4((float)iuv.x * (1.0 / 4096.0), (float)iuv.y * (1.0 / 4096.0), 1.0,
                 (float)((asuint(vu_add(asfloat(i.wbits), vu_mul(asfloat(d3.x), wq))) >> 4) & 255u));
#if R2_ENH
    // (light) what the pixel shader's per-pixel lighting needs; the camera in object space: the point whose clip x, y and
    // w are 0 (the world x clip matrix's x / y / w columns: three planes)
    o.lx = uint4(g_lxb, g_lmw, 0u, asuint(fade));
    o.ln = g_lnrm;
    o.lc = float4(r2_ub(i.lcol, 0u), r2_ub(i.lcol, 1u), r2_ub(i.lcol, 2u), r2_ub(i.lcol, 3u));
    {
        float3 r0 = float3(m[0].x, m[1].x, m[2].x), r1 = float3(m[0].y, m[1].y, m[2].y), r2 = float3(m[0].w, m[1].w, m[2].w);
        float3 b = -float3(m[3].x, m[3].y, m[3].w);
        float3 c12 = cross(r1, r2), c20 = cross(r2, r0), c01 = cross(r0, r1);
        float det = dot(r0, c12);
        float3 e = abs(det) > 1e-20 ? (c12 * b.x + c20 * b.y + c01 * b.z) / det : ipos + float3(0.0, 0.0, 1.0);
        o.lv = e - ipos;
    }
#endif
}

R2O r2_vs(R2V i) {
    R2O o;
    float4 clip, alt;
    r2_vertex(i, o, clip, alt);
    return o;
}

#ifndef R2_PULL
#define R2_PULL 0
#endif
#if R2_PULL
// (r8-perf) a merged draw of the segment's plan (gpu_front.h gpu_r2_chunk, gpu_r2front.c r2_plan): instance k is the draw
// record whose absolute index is the k-th word of the chunk's member list (fdata, 32-byte aligned: StartInstanceLocation =
// its offset / 32, so DRAWID - SV_InstanceID locates it); vertex v is index v of that draw's range, pulled from the arena
// (r2_render.c: indices t21, vertices t20 as the r2_vtx records; the mesh table holds the mesh's vertex and index bases).
// Vertices past the draw's count make triangles outside the clip volume (rejected).
ByteAddressBuffer r2_vb : register(t20);
ByteAddressBuffer r2_ib : register(t21);
struct R2Pull {
    uint did : DRAWID;
    uint vid : SV_VertexID;
    uint inst : SV_InstanceID;
};
R2O r2_vs_pull(R2Pull p) {
    uint rid = fdata.Load((p.did - p.inst) * 32u + p.inst * 4u);
    uint db = rid * 32u;
    uint mesh = fdata.Load(db + 12u) & 0xFFFFFu;
    uint2 fc = fdata.Load2(db + 16u);                       // first, count (indices)
    uint3 mt = r2_mtab.Load3(mesh * 16u);                   // material, vertex, index bases
    bool pad = p.vid >= fc.y;
    uint vi = r2_ib.Load((mt.z + fc.x + (pad ? 0u : p.vid)) * 4u);
    uint vb = (mt.y + vi) * 48u;
    uint4 a = r2_vb.Load4(vb), b = r2_vb.Load4(vb + 16u), c = r2_vb.Load4(vb + 32u);
    R2V i;
    i.pos = asfloat(a.xyz);
    i.wbits = a.w;
    i.uv = int2((int)(b.x << 16) >> 16, (int)b.x >> 16);
    i.cola = b.y; i.colb = b.z; i.batch = b.w;
    i.nrm = c.x; i.lcol = c.y; i.wts = c.z;
    i.did = rid;
    R2O o;
    float4 clip, alt;
    r2_vertex(i, o, clip, alt);
    if (pad) o.pos = float4(2.0, 2.0, 0.5, 1.0);
    return o;
}
#endif

#if R2_MV
// (r2-m8, R8) the motion-vector pass of a modern frame (gpu_exec_mft.h mft_mv): pass A of every native entry again, depth
// GEQUAL half a GS Z unit closer (its own surface passes, nothing behind it), writing current - previous position
struct R2M {
    float4 pos : SV_Position;
    noperspective float2 mv : MOTION;
};
R2M r2_vs_mv(R2V i) {
    R2O o;
    float4 clip, alt;
    r2_vertex(i, o, clip, alt);
    R2M r;
    r.pos = o.pos;
    r.pos.z += 0.5 * (1.0 / 1048576.0);
    r.mv = float2(o.pos.x * 8192.0, -o.pos.y * 8192.0) - g_prev_d3d;
    return r;
}
float2 r2_ps_mv(R2M i) : SV_Target { return i.mv; }
#endif

#ifndef R2_SHADOW
#define R2_SHADOW 0
#endif
#if R2_SHADOW
// (reimagine) Look = Reimagined: the frame's R2 draws again as shadow casters, depth only, into the key light's shadow map
// (runtime/gpu/gpu_reim.h): the vertex's clip position (the entry's world x clip) back to the world through the frame's
// inverse world x clip, then the light's orthographic projection (depth: 1 at the light, GEQUAL as the frame's own)
cbuffer r2_shadow : register(b3) {
    float4 sh_ipv[4];                    // the frame's inverse world x clip (rows)
    float4 sh_lm[4];                     // world -> the light's clip (rows)
};
float4 r2_vs_shadow(R2V i) : SV_Position {
    // (r2_vertex's position alone, in plain floats: no colours, fog, UVs or the VU's rounding - a caster needs none)
    uint db = i.did * 32u;
    uint4 dr = fdata.Load4(db);
    R2Seg sg = r2_seg(fdata.Load(db + 28u));
    if (dr.z != 0xFFFFFFFFu) {
        uint rel = i.batch - (dr.y >> 8);
        if (((fdata.Load((dr.z + (rel >> 5)) * 4u) >> (rel & 31u)) & 1u) == 0u) return float4(2.0, 2.0, 0.5, 1.0);
    }
    uint mat = r2_mtab.Load((dr.w & 0xFFFFFu) * 16u) + i.batch * 3u + (dr.y & 3u);
    uint4 lm = r2_mats.Load4(mat * 64u + 44u);
    uint eb = sg.off.y + dr.x * 144u;
    float4 m[4] = { asfloat(fdata.Load4(eb)), asfloat(fdata.Load4(eb + 16u)), asfloat(fdata.Load4(eb + 32u)),
                    asfloat(fdata.Load4(eb + 48u)) };
    float3 p = i.pos;
    uint xq = fdata.Load(eb + 72u);
    uint skin = (lm.x >> 1) & 3u;
    if (lm.x != 0u && xq != 0u && skin != 0u) {   // (0x1940's 4-bone skinning of the position)
        uint xb = (xq - 1u) * 16u;
        float3 P = float3(0.0, 0.0, 0.0);
        [unroll] for (uint k = 0u; k < 4u; k++) {
            uint bo = ((k < 2u ? lm.y : lm.z) >> (16u * (k & 1u))) & 0xFFFFu;
            uint q = 11u + bo - 0x15u;
            float wk = r2_ub(i.wts, k) * (1.0 / 255.0);
            P += (r2_xq(xb, q).xyz * p.x + r2_xq(xb, q + 1u).xyz * p.y + r2_xq(xb, q + 2u).xyz * p.z + r2_xq(xb, q + 3u).xyz) * wk;
        }
        p = P;
    }
    float4 clip = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    float4 h = clip.x * sh_ipv[0] + clip.y * sh_ipv[1] + clip.z * sh_ipv[2] + clip.w * sh_ipv[3];
    float3 wp = h.xyz / h.w;
    return wp.x * sh_lm[0] + wp.y * sh_lm[1] + wp.z * sh_lm[2] + sh_lm[3];
}
void r2_ps_shadow() {}
#endif

#if R2_SHADOW
// (reimagine2) cut-out casters: the draws whose alpha test splits their fragments (leaves, fronds, grilles, fences) cast
// the shape of what the picture shows, not their whole cards - the full vertex (lit / skinned UVs and colours) and the
// material's texture and alpha test, as r2_ps does (the PS2 texels: the cut-out's shape is the game's own)
struct R2SA {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;               // U, V (texture units; affine in the light's orthographic view = exact)
    float va : COLOR0;                   // the vertex colour's alpha
    nointerpolation uint3 mr : MATRULE;
};
R2SA r2_vs_shadow_at(R2V i) {
    R2O o;
    float4 clip, alt;
    r2_vertex(i, o, clip, alt);
    R2SA r;
    float4 h = clip.x * sh_ipv[0] + clip.y * sh_ipv[1] + clip.z * sh_ipv[2] + clip.w * sh_ipv[3];
    float3 wp = h.xyz / h.w;
    r.pos = wp.x * sh_lm[0] + wp.y * sh_lm[1] + wp.z * sh_lm[2] + sh_lm[3];
    if (o.pos.x >= 2.0) r.pos = float4(2.0, 2.0, 0.5, 1.0);   // (a batch left to another draw)
    r.uv = alt.xy;
    r.va = o.col.w;
    r.mr = o.mr;
    return r;
}
void r2_ps_shadow_at(R2SA i) {
    vkx = 1u; vky = 1u; vbase = 0u; vfbw = 1u; vflags = 0u; one = 1.0; kx = 1u; ky = 1u;   // (gs.hlsl statics, as r2_ps)
    uint mb = i.mr.x * 64u;
    uint4 d1 = r2_mats.Load4(mb);
    uint4 d2 = r2_mats.Load4(mb + 16u);
    uint4 d3 = r2_mats.Load4(mb + 32u);
    uint test = d2.y;
    if ((test & 1u) == 0u) return;
    uint flags = d3.z;
    St st;
    st.tex0clamp = d1;
    st.tex1 = uint2(d2.x, 0u);
    st.texa = 0u; st.clut_slot = 0u; st.texsrc = 1u; st.fogcol = d2.w; st.test = d2.y; st.fmask = 0u; st.flags = 0u;
    st.alpha = d2.z; st.zinfo = 0u;
    st.tlev = (flags >> 8) & 3u;
    st.mip[0] = 0u; st.mip[1] = 0u; st.mip[2] = 0u; st.mip[3] = 0u; st.mip[4] = 0u; st.mip[5] = 0u;
    st.dimx = uint2(0u, 0u);
    st.dec = r2_mats.Load(mb + 60u);
    uint t0 = d1.x, t1 = d1.y;
    uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
    float tw16 = (float)(16u << (tw > 10u ? 10u : tw)), th16 = (float)(16u << (th > 10u ? 10u : th));
    uint t = tex_sample(st, f2i(i.uv.x * tw16), f2i(i.uv.y * th16), 1.0);
    uint va = (uint)clamp(i.va, 0.0, 255.0);
    uint col = tfx((t1 >> 3) & 3u, (flags & R2D_TCC) != 0u, 0x808080u | (va << 24), t);
    bool apass = atst_pass((test >> 1) & 7u, (test >> 4) & 0xFFu, col >> 24);
    if (apass == ((i.mr.y & 1u) != 0u)) discard;   // rule 0: failing fragments discarded, rule 1: passing ones
}
#endif

R2C r2_vs_clip(R2V i) {
    R2O o;
    float4 clip, alt;
    r2_vertex(i, o, clip, alt);
    R2C r;
    r.pos = o.pos; r.col = o.col; r.fog = o.fog; r.stq = o.stq; r.mr = o.mr; r.gv = o.gv;
#if R2_ENH
    r.lx = o.lx; r.ln = o.ln; r.lc = o.lc; r.lv = o.lv;
#endif
    r.clip = clip;
    r.alt = alt;
    return r;
}

// (r2-m2) Meshes crossing the VU's guard band (r2_capture.c inside_guard; notes/modern-renderer.md "R2 as built"): per
// triangle as the VU decides it (0x0C60: each vertex's CLIPW flags against |x|, |y|, |z| <= w): all three inside, the
// VU-exact screen vertices; all three outside one plane, nothing; else the VU's clipper (0x2540) draws it as clipped
// fans: (r2-m5b) those are the VU's own packets (runtime/r2/r2_clip.c, decoded after the entry), so the triangle is left
// out here. Hybrid entries carry run tables instead (the strip triangles through the plain pipeline); this stage remains
// for bracket A/B and the PS2_GPU_AB_EE A phases. PS2_R2_CLIP=1 (misc bit 64): the GPU clips the clip-space triangle
// against the same six planes (SV_ClipDistance), perspective-correct, not the VU's arithmetic.
uint r2_outcode(float4 c) {
    return (c.x > c.w ? 1u : 0u) | (c.x < -c.w ? 2u : 0u) | (c.y > c.w ? 4u : 0u) | (c.y < -c.w ? 8u : 0u) |
           (c.z > c.w ? 16u : 0u) | (c.z < -c.w ? 32u : 0u) | (c.w > 0.0 ? 0u : 64u);
}

[maxvertexcount(3)]
void r2_gs_clip(triangle R2C v[3], inout TriangleStream<R2K> ts) {
    uint o0 = r2_outcode(v[0].clip), o1 = r2_outcode(v[1].clip), o2 = r2_outcode(v[2].clip);
    if ((o0 & o1 & o2 & 63u) != 0u) return;
    bool inside = (o0 | o1 | o2) == 0u && (asuint(r2_misc.w) & 16u) == 0u;   // (PS2_R2_DBG 16: always the clip path)
    // (r2-m5b) a triangle the VU clips: its fans are the VU clipper's own (runtime/r2/r2_clip.c, decoded by the GS thread
    // after the entry), or the VU culls it; GPU clipping only with PS2_R2_CLIP=1 (misc bit 64)
    if ((o0 | o1 | o2) != 0u && (asuint(r2_misc.w) & 64u) == 0u) return;
#if R2_MFT
    if (!inside) return;                 // (mft-pop: the modern frame's surfaces - no GPU-clipped form, positions are final)
#endif
    R2Seg sg = r2_seg(v[0].mr.z);
    float4 fr = asfloat(fdata.Load4(sg.off.x + 32u));       // screen model: ax, bx, ay, by (gpu_r2_frame)
    float2 fz = asfloat(fdata.Load2(sg.off.x + 48u));       //               az, bz
    float kx = sg.map.x * (1.0 / 16.0), ky = sg.map.y * (1.0 / 16.0);
    [unroll] for (uint k = 0u; k < 3u; k++) {
        R2K o;
        o.col = v[k].col; o.mr = v[k].mr; o.gv = v[k].gv;
#if R2_ENH
        o.lx = v[k].lx; o.ln = v[k].ln; o.lc = v[k].lc; o.lv = v[k].lv;
#endif
        if (inside) {
            o.pos = v[k].pos; o.fog = v[k].fog; o.stq = v[k].stq;
            o.cd0 = float4(1.0, 1.0, 1.0, 1.0); o.cd1 = float2(1.0, 1.0);
        } else {
            float4 c = v[k].clip;
            // d3d = (GS - XYOFFSET) k / 16 + 0.5 with GS = b + a c / w, through the REF viewport (NDC x 8192), times w
            o.pos = float4((((fr.y - sg.map.z) * kx + 0.5) * c.w + fr.x * kx * c.x) * (1.0 / 8192.0),
                           -(((fr.w - sg.map.w) * ky + 0.5) * c.w + fr.z * ky * c.y) * (1.0 / 8192.0),
                           ((fz.y + 0.5) * c.w + fz.x * c.z) * (1.0 / 1048576.0), c.w);
            o.fog = v[k].alt.w;
            o.stq = v[k].alt.xyz;
            o.cd0 = float4(c.w - c.x, c.w + c.x, c.w - c.y, c.w + c.y);
            o.cd1 = float2(c.w - c.z, c.w + c.z);
        }
        ts.Append(o);
    }
}

// ---- the triangle's Z record exactly as the exact path's frontend builds it (runtime/gs_setup.c gs_tri_setup /
// gs_tri_plane, gs_setup.h gs_rowstep_attr: row = llround((c0 + cy * by0) * 65536), dy, dx = llround(cy / cx * 65536),
// computed there in doubles). Here in exact rational arithmetic on 64-bit integers (int64.hlsli): with the vertices'
// integer X, Y (12.4) and Z, cx * 65536 = 2^20 Nx / A, cy * 65536 = 2^20 Ny / A and (c0 + cy * by0) * 65536 =
// 65536 z0 + 2^16 M / A (A = twice the area in 12.4 units). The doubles round only far below the 2^-16 the records keep,
// so the two agree but for values within ~1e-12 of a rounding boundary. The pixel shader then evaluates the record with
// the exact path's own gs_z16_sample, so every sample of every pixel holds the Z the exact path would write there and
// Z ties with exact draws (decals, the shadow pipeline) hold at any scale. (i64_round_scaled: r2_vu.hlsli)
[maxvertexcount(3)]
void r2_gs(triangle R2O v[3], inout TriangleStream<R2P> ts) {
    int x0 = v[0].gv.x, y0 = v[0].gv.y, x1 = v[1].gv.x, y1 = v[1].gv.y, x2 = v[2].gv.x, y2 = v[2].gv.y;
    i64 A = i64_sub(i64_mul_i32(i64_make(x1 - x0), y2 - y0), i64_mul_i32(i64_make(y1 - y0), x2 - x0));
    if (all(A == uint2(0u, 0u))) return;                    // (the GS draws nothing)
    int miny = min(y0, min(y1, y2));
    int by0 = max((miny + 15) >> 4, (int)r2_seg(v[0].mr.z).off.w);
    int d1 = v[1].gv.z - v[0].gv.z, d2 = v[2].gv.z - v[0].gv.z;
    i64 nx = i64_sub(i64_mul_i32(i64_make(d1), y2 - y0), i64_mul_i32(i64_make(d2), y1 - y0));
    i64 ny = i64_sub(i64_mul_i32(i64_make(d2), x1 - x0), i64_mul_i32(i64_make(d1), x2 - x0));
    if (i64_is_neg(A)) { A = i64_neg(A); nx = i64_neg(nx); ny = i64_neg(ny); }
    i64 m = i64_add(i64_mul_i32(nx, -x0), i64_mul_i32(ny, 16 * by0 - y0));
    i64 dx = i64_round_scaled(nx, A, 20u, false);
    i64 dy = i64_round_scaled(ny, A, 20u, false);
    i64 row = i64_add(i64_shl(i64_make(v[0].gv.z), 16u), i64_round_scaled(m, A, 16u, true));   // (Z >= 0)
    [unroll] for (uint k = 0u; k < 3u; k++) {
        R2P o;
        o.pos = v[k].pos; o.col = v[k].col; o.fog = v[k].fog; o.stq = v[k].stq; o.mr = v[k].mr;
        o.zr0 = uint4(row, dy);
        o.zr1 = uint3(dx, asuint(by0));
        ts.Append(o);
    }
}

struct R2F {
    float4 c0 : SV_Target0;
#if R2_DUAL
    float4 c1 : SV_Target1;
#endif
#if R2_MFT
#if R2_A2C
    uint cov : SV_Coverage;              // (r2-m8) the cut-out's coverage (ANDed with the rasterizer's)
#endif
#else
    float z : SV_DepthLessEqual;
#endif
};

#if R2_ENH >= 2
// (reimagine) THE per-pixel normal hook of Look = Reimagined (R2_ENH=2): N is the interpolated (skinned) object-space unit
// normal of a lit sub-batch, V the unit direction to the camera, both object space. A normal map (the Reimagined textures'
// normal maps, notes/reimagined.md) perturbs N here - define R2_NMAP and provide this function (e.g. a tangent frame from
// the screen-space derivatives of the position and UV). Default: the flat, interpolated normal.
float3 r2_reim_normal(float3 N, float3 V) {
    N = normalize(N);
#if R2_REIM && R2_ETEX
    // the surface map's tangent normal through a cotangent frame from the screen derivatives of the position and uv
    // (meshes carry no tangents); faded with the map's footprint (no shimmer at a distance)
    if (g_nmf > 0.0) {
        float3 dp2p = cross(g_dpy, N), dp1p = cross(N, g_dpx);
        float3 T = dp2p * g_duvx.x + dp1p * g_duvy.x, B = dp2p * g_duvx.y + dp1p * g_duvy.y;
        float im = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-30));
        float2 nxy = g_nm.xy * (g_nmf * r2_reim2.x);
        float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
        float3 Np = T * (im * nxy.x) + B * (im * nxy.y) + N * nz;
        if (dot(Np, Np) > 1e-12) N = normalize(Np);
    }
#endif
    return N;
}
static uint g_reim_skin;                 // (the sub-batch is skinned: a character - only they get the rim and the highlight)
#endif
#if R2_ENH
// (light) r2_lit's colour per pixel: the interpolated normal (renormalised to the VU's 127) through the entry's light block,
// the same formula (f = min(d + d^3, threshold), clamps, the highlight share), plus an optional rim light (r2_misc.z)
float4 r2_lit_px(uint4 lx, float4 ln, float4 lc, float3 lv, bool csel, float4 col) {
    uint xb = lx.x - 1u;
    float3 N = ln.xyz * (127.0 * rsqrt(max(dot(ln.xyz, ln.xyz), 1e-6)));
#if R2_ENH >= 2
    N = r2_reim_normal(N, lv) * 127.0;   // (reimagine: the surface map's normal, when there is one)
#endif
    float4 T0 = r2_xq(xb, 0u), T1 = r2_xq(xb, 1u), T2 = r2_xq(xb, 2u), amb = r2_xq(xb, 8u), thr = r2_xq(xb, 9u),
           v10 = r2_xq(xb, 10u), L0 = r2_xq(xb, 4u);
    float4 d = T0 * N.x + T1 * N.y + T2 * N.z;
    float4 f = min(d + d * (d * d), thr);
    amb.w = amb.w + asfloat(lx.y) + v10.w;
    float4 C = L0 * f.x + r2_xq(xb, 5u) * f.y + r2_xq(xb, 6u) * f.z + r2_xq(xb, 7u) * f.w + amb;
    float mx = max(max(C.x, C.y), C.z);
    float lim = max(2.0 - mx, 0.0);
    float4 cl = float4(min(C.xyz, 2.0), min(C.w, lim));
    float k = lim - cl.w;
    float den = (2.0 + asfloat(0x3C810247u)) * asfloat(0x427DFF7Du) - k * asfloat(0x427DFF7Du);
    float fade = asfloat(lx.w);
    if (csel) {
        float3 b = lc.xyz * cl.w * (lc.w / den);
        if (fade < 1.0) b *= fade;
        return float4(b, col.w);
    }
    float3 a = (cl.xyz + v10.xyz * k) * ln.w;
#if R2_ENH >= 2
    // (reimagine) Look = Reimagined: a specular highlight from the strongest light and a rim light in its hue (the lights'
    // colours may be negative - the light then comes from -direction, as in f's sign)
    {
        float3 V = normalize(lv), Nn = normalize(N);
        uint kk = 0u;
        float best = -1.0;
        [unroll] for (uint j = 0u; j < 4u; j++) {
            float4 Lj = r2_xq(xb, 4u + j);
            float m = abs(dot(Lj.xyz, float3(0.3, 0.59, 0.11)));
            if (m > best) { best = m; kk = j; }
        }
        float4 Lc = r2_xq(xb, 4u + kk);
        float sg = dot(Lc.xyz, float3(0.3, 0.59, 0.11)) < 0.0 ? -1.0 : 1.0;
        float3 Ld = float3(T0[kk], T1[kk], T2[kk]) * sg;
        float ll = length(Ld);
        float3 lc3 = abs(Lc.xyz);
        float3 hue = lc3 / max(max(lc3.x, lc3.y), max(lc3.z, 1e-3));
        if (ll > 1e-6 && r2_mft.w > 0.0 && g_reim_skin != 0u) {
            Ld /= ll;
            float3 H = normalize(Ld + V);
            float nh = saturate(dot(Nn, H)), nl = saturate(dot(Nn, Ld));
            float sp = pow(nh, 28.0) * nl;
            a += sp * r2_mft.w * (0.35 + 0.65 * hue) * ln.w;   // (ln.w: the colour's scale, 128 = the texture as it is)
        }
        if (r2_misc.z > 0.0 && g_reim_skin != 0u) {   // (world meshes seen at a grazing angle: no rim - a floor turned grey-blue)
            float r = 1.0 - saturate(abs(dot(Nn, V)));
            r = r * r * r;
            a += r * r2_misc.z * (0.4 + 0.6 * hue) * ln.w;
        }
    }
    return float4(a, col.w);
#endif
    if (r2_misc.z > 0.0) {               // rim: grazing angles towards the camera, in the key light's hue
        float r = 1.0 - saturate(abs(dot(N * (1.0 / 127.0), normalize(lv))));
        r = r * r * r;
        float3 rc = max(L0.xyz, 0.0);
        rc = rc / max(max(rc.x, rc.y), max(rc.z, 1e-3));
        a += r * r2_misc.z * rc * ln.w;
    }
    return float4(a, col.w);
}
#endif

#if R2_ZREC
R2F r2_ps(R2P i) {
#else
R2F r2_ps(R2O i) {
#endif
    vkx = 1u; vky = 1u; vbase = 0u; vfbw = 1u; vflags = 0u; one = 1.0; kx = 1u; ky = 1u;   // (gs.hlsl statics: no view)
    if ((asuint(r2_misc.w) & 8u) != 0u) {                 // (PS2_R2_DBG 8: a constant colour, cost experiments)
        R2F d;
        d.c0 = float4(0.5, 0.5, 0.5, 0.5);
#if R2_DUAL
        d.c1 = float4(0.0, 0.0, 0.0, 0.0);
#endif
#if R2_MFT
#if R2_A2C
        d.cov = 0xFFFFFFFFu;
#endif
#else
        d.z = i.pos.z;
#endif
        return d;
    }
    uint mb = i.mr.x * 64u;
    uint4 d1 = r2_mats.Load4(mb);
    uint4 d2 = r2_mats.Load4(mb + 16u);
    uint4 d3 = r2_mats.Load4(mb + 32u);
    uint flags = d3.z;
    St st;
    st.tex0clamp = d1;
    st.tex1 = uint2(d2.x, 0u);
    st.texa = 0u; st.clut_slot = 0u; st.texsrc = 1u; st.fogcol = d2.w; st.test = d2.y; st.fmask = 0u; st.flags = 0u;
    st.alpha = d2.z; st.zinfo = 0u;
    st.tlev = (flags >> 8) & 3u;          // texm: 1 nearest, 2 bilinear (GPU_R2D_TEXM)
    st.mip[0] = 0u; st.mip[1] = 0u; st.mip[2] = 0u; st.mip[3] = 0u; st.mip[4] = 0u; st.mip[5] = 0u;
    st.dimx = uint2(0u, 0u);
    st.dec = r2_mats.Load(mb + 60u);
    uint t0 = d1.x, t1 = d1.y;
    uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
    float tw16 = (float)(16u << (tw > 10u ? 10u : tw)), th16 = (float)(16u << (th > 10u ? 10u : th));
    float iq = 1.0 / i.stq.z;              // (as the exact path's FF fast path, gs.hlsl STQ_FAST)
    precise float su = i.stq.x * iq;
    precise float tv = i.stq.y * iq;
    precise float su16 = su * tw16;
    precise float tv16 = tv * th16;
#if R2_ETEX
    uint t;
    {
        float2 euv = float2(su, tv);
        float2 edx = ddx(euv), edy = ddy(euv);
        if ((asuint(r2_misc.w) & 1u) != 0u) t = 0x80808080u;
        else if (!et_sample(st.dec, d1, st.tlev, euv, edx, edy, i.pos.x, t)) t = tex_sample(st, f2i(su16), f2i(tv16), 1.0);
#if R2_REIM
        g_nmf = 0.0; g_nm = float4(0.0, 0.0, 0.5, 1.0); g_duvx = edx; g_duvy = edy;
        float4 nm;
        if (etn_sample(st.dec, d1, st.tlev, euv, edx, edy, nm)) {
            uint2 ee = et_tab.Load2(((st.dec - 1u) >> 6) * 8u);
            float fp = max(length(edx), length(edy)) * (float)(32u << (ee.y & 7u)) * e_ps.x;   // map texels per pixel
            g_nmf = saturate((4.0 - fp) / 2.5);
            g_nm = nm;
        }
#if R2_ENH
        g_dpx = -ddx(i.lv); g_dpy = -ddy(i.lv);
#endif
#endif
    }
#else
    uint t = (asuint(r2_misc.w) & 1u) != 0u ? 0x80808080u : tex_sample(st, f2i(su16), f2i(tv16), 1.0);   // (PS2_R2_DBG 1)
#endif
#if R2_ENH >= 2 && !R2_ZREC
    g_reim_skin = (r2_mats.Load(mb + 44u) >> 1) & 3u;
#endif
#if R2_ENH && !R2_ZREC
    float4 icol = i.lx.x != 0u ? r2_lit_px(i.lx, i.ln, i.lc, i.lv, (flags & R2D_CSEL) != 0u, i.col) : i.col;
    uint4 vc = (uint4)max(icol, 0.0);
#else
    uint4 vc = (uint4)max(i.col, 0.0);                     // the GS's Gouraud stepping truncates
#endif
#if R2_REIM && R2_ETEX
    // (reimagine) the surface map on the world: relief lit from the key light's side (the slope's direction on screen against
    // the light's) and the cavity deepening creases; faded with the map's footprint
#if R2_ENH
    if (g_nmf > 0.0 && i.lx.x == 0u) {
#else
    if (g_nmf > 0.0) {
#endif
        float2 dux = g_duvx * rsqrt(max(dot(g_duvx, g_duvx), 1e-30)), duy = g_duvy * rsqrt(max(dot(g_duvy, g_duvy), 1e-30));
        float2 sd = float2(dot(g_nm.xy, dux), dot(g_nm.xy, duy));
        float rel = clamp(dot(sd, r2_reim.xy) * r2_reim.z, -0.45, 0.45);
        float f = (1.0 + rel * g_nmf) * lerp(1.0, g_nm.w, r2_reim.w * g_nmf);
        vc.xyz = (uint3)min((float3)vc.xyz * f, 255.0);
    }
#endif
    vc = min(vc, uint4(255u, 255u, 255u, 255u));
    uint col = tfx((t1 >> 3) & 3u, (flags & R2D_TCC) != 0u, vc.x | (vc.y << 8) | (vc.z << 16) | (vc.w << 24), t);
    if (flags & R2D_FGE) {
        uint fv = (uint)clamp(i.fog, 0.0, 255.0), fc = d2.w;
        uint r = (fv * (col & 255u) + (255u - fv) * (fc & 255u)) >> 8;
        uint g = (fv * ((col >> 8) & 255u) + (255u - fv) * ((fc >> 8) & 255u)) >> 8;
        uint b = (fv * ((col >> 16) & 255u) + (255u - fv) * ((fc >> 16) & 255u)) >> 8;
        col = r | (g << 8) | (b << 16) | (col & 0xFF000000u);
    }
    uint As = col >> 24;
    uint test = d2.y;
#if R2_A2C
    // (r2-m8, R8) alpha-to-coverage for the cut-outs of pass A (alpha test GEQUAL / GREATER, failing fragments discarded):
    // the share of the pixel's samples that pass grows linearly with the alpha's distance from the reference across the
    // alpha's screen-space gradient (the anti-aliased alpha test of modern engines); the GS alpha byte the target keeps is
    // unchanged (the destination-alpha tests of the shadow pipeline read it), the coverage goes through SV_Coverage
    uint a2c_cov = 0xFFFFFFFFu;
    float fa = (float)As;
    float wa = fwidth(fa);
    {
        bool rule1 = (i.mr.y & 1u) != 0u;
        uint atst = (test >> 1) & 7u, aref = (test >> 4) & 0xFFu;
        if ((test & 1u) != 0u && !rule1 && (atst == 5u || atst == 6u)) {
            float thr = (float)aref + (atst == 6u ? 0.5 : -0.5);
            float cv = saturate((fa - thr) / max(wa, 1.0) + 0.5);
            uint ns = (uint)r2_mft.z;
            uint n = (uint)(cv * (float)ns + 0.5);
            if (n == 0u) discard;
            uint all = ns >= 32u ? 0xFFFFFFFFu : (1u << ns) - 1u;
            uint m = n >= ns ? all : (1u << n) - 1u;
            uint rot = ((uint)i.pos.x ^ ((uint)i.pos.y << 1)) & (ns - 1u);   // (which samples: varied per pixel)
            a2c_cov = rot == 0u ? m : ((m << rot) | (m >> (ns - rot))) & all;
        } else {
            bool apass = (test & 1u) == 0u || atst_pass(atst, aref, As);
            if (apass == rule1) discard;
        }
    }
#elif !R2_NODISCARD
    // (r2-m2) only draws whose alpha test splits the fragments use this pipeline; the others (no alpha test, every
    // fragment passing or every one failing) the R2_NODISCARD variant, whose depth stays early (no discard)
    bool apass = (test & 1u) == 0u || atst_pass((test >> 1) & 7u, (test >> 4) & 0xFFu, As);
    if (apass == ((i.mr.y & 1u) != 0u)) discard;  // rule 0: failing fragments discarded, rule 1: passing ones
#endif
    // blend: the exact path's FF table (gs.hlsl ps_ff); the output merger does Cd
    uint bl = (flags >> 12) & 15u;
    uint cs[3] = { col & 255u, (col >> 8) & 255u, (col >> 16) & 255u };
    float3 rgb = float3(b2f(cs[0]), b2f(cs[1]), b2f(cs[2]));
    float a1 = 0.0;
    if ((flags & R2D_ABE) != 0u) {
        uint al = d2.z;
        uint asel = al & 3u, bsel = (al >> 2) & 3u, csel = (al >> 4) & 3u, dsel = (al >> 6) & 3u, fix = (al >> 8) & 255u;
        int ccv = csel == 0u ? (int)As : (int)fix;
        [unroll] for (uint ch = 0u; ch < 3u; ch++) {
            int s = (int)cs[ch];
            uint u;
            if (bl == BL_ADD) u = min((uint)(s * ccv) >> 7, 255u);
            else if (bl == BL_REVSUB) u = min((uint)(s * ccv + 127) >> 7, 255u);
            else if (bl == BL_DUAL) u = 0u;
            else {
                int v = (((sel3(asel, s, 0) - sel3(bsel, s, 0)) * ccv) >> 7) + sel3(dsel, s, 0);
                u = clamp255i(v);
            }
            rgb[ch] = b2f(u);
        }
        if (bl == BL_DUAL) {
            float a = (float)ccv * (1.0 / 128.0);
            a1 = min(a, 1.0);
            [unroll] for (uint c2 = 0u; c2 < 3u; c2++) {
                float s = (float)cs[c2];
                if (ccv > 0 && ccv < 128) s = max(s - r2_misc.x / a, 0.0);
                rgb[c2] = s * (1.0 / 255.0);
            }
        }
    }
    uint ao = As | ((i.mr.y & 2u) != 0u ? 0x80u : 0u);   // (FBA)
    R2F o;
    o.c0 = float4(rgb, b2f(min(ao, 255u)));
#if R2_DUAL
    o.c1 = float4(0.0, 0.0, 0.0, a1);
#endif
#if R2_MFT
#if R2_A2C
    o.cov = a2c_cov;
#endif
    return o;
#else
#if R2_ZREC
    // the exact path's Z at this sample (gs.hlsl gs_frag): native pixel (px, py), sample (si, sj) of kx x ky
    R2Seg sg = r2_seg(i.mr.z);
    uint kxu = (uint)sg.map.x, kyu = (uint)sg.map.y;
    uint2 tp = (uint2)i.pos.xy;
    uint pxu = tp.x / kxu, pyu = tp.y / kyu;
    i64 z16 = gs_z16_sample(i.zr0.xy, i.zr0.zw, i.zr1.xy, asint(i.zr1.z), (int)pxu, (int)pyu, tp.x - pxu * kxu,
                            tp.y - pyu * kyu, kxu, kyu);
    float zv = (float)gs_z_clamp(z16, 0xFFFFFFFFu);
#else
    // the rasterizer's plane through the exact vertices (half a unit up, r2_vs): the GS's Z but where the plane
    // comes within ~0.01 of an integer
    float zv = floor(i.pos.z * 1048576.0 - 0.5);
#endif
    o.z = max(zv + r2_misc.y, 0.0) * (1.0 / 1048576.0);   // (r2_misc.y: PS2_R2_ZBIAS, debugging)
    if ((asuint(r2_misc.w) & 2u) != 0u) o.z = i.pos.z;     // (PS2_R2_DBG 2: the rasterized depth)
    return o;
#endif
}
