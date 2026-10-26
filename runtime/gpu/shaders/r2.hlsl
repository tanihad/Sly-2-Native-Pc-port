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
};
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
};

#ifndef R2_CLIP
#define R2_CLIP 0                        // (r2-m2) 1: the pipeline of meshes crossing the VU's guard band (r2_gs_clip)
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
};

struct R2C {                             // (r2-m2) the clip pipeline's vertex: R2O + the clip-space position and the
    noperspective centroid float4 pos : SV_Position;   // attributes of a GPU-clipped triangle
    R2I float4 col : COLOR0;
    R2I float fog : FOG;
    R2I float3 stq : TEXCOORD0;
    nointerpolation uint3 mr : MATRULE;
    nointerpolation int3 gv : GV;
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
    uint mat = r2_mtab.Load((dr.w & 0xFFFFFu) * 4u) + i.batch * 3u + rp;
    uint4 d3 = r2_mats.Load4(mat * 48u + 32u);   // fog scale, fog w max, flags
    uint eb = sg.off.y + dr.x * 80u;
    float4 m[4] = { asfloat(fdata.Load4(eb)), asfloat(fdata.Load4(eb + 16u)), asfloat(fdata.Load4(eb + 32u)),
                    asfloat(fdata.Load4(eb + 48u)) };
    float fade = asfloat(fdata.Load(eb + 64u));
    float4 c0 = asfloat(fdata.Load4(sg.off.x)), c1 = asfloat(fdata.Load4(sg.off.x + 16u));   // VU 0, VU 1
    // 0x07F8: MULAx / MADDAy / MADDAz / MADDw (row 3 x 1.0 exact), M = world x clip (gpu_r2front.c, VU-exact too)
    float c[4];
    float q;
    uint bx, by, bz;
    if ((asuint(r2_misc.w) & 4u) != 0u) {                   // (PS2_R2_DBG 4: plain float arithmetic, cost experiments)
        [unroll] for (uint j2 = 0u; j2 < 4u; j2++) c[j2] = m[0][j2] * i.pos.x + m[1][j2] * i.pos.y + m[2][j2] * i.pos.z + m[3][j2];
        q = 1.0 / c[3];
        bx = asuint(c1.x + c0.x * c[0] * q);
        by = asuint(c1.y + c0.y * c[1] * q);
        bz = asuint(c1.z + c0.z * c[2] * q);
    } else {
    [unroll] for (uint j = 0u; j < 4u; j++) {
        float acc = vu_mul(m[0][j], i.pos.x);
        acc = vu_add(acc, vu_mul(m[1][j], i.pos.y));
        acc = vu_add(acc, vu_mul(m[2][j], i.pos.z));
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
    precise float dx = (gx - sg.map.z) * (sg.map.x * (1.0 / 16.0)) + 0.5;
    precise float dy = (gy - sg.map.w) * (sg.map.y * (1.0 / 16.0)) + 0.5;
    // depth: the rasterized value only bounds the exact Z the pixel shader writes (SV_DepthLessEqual): half a unit above
    // the plane through the integer Z values, so it stays >= the GS's value at every sample
    o.pos = float4(dx * (1.0 / 8192.0), -dy * (1.0 / 8192.0), (gz + 0.5) * (1.0 / 1048576.0), 1.0);
    o.gv = int3((int)(bx & 0xFFFFu) - (int)sg.map.z, (int)(by & 0xFFFFu) - (int)sg.map.w, (int)((bz >> 4) & 0xFFFFFFu));
    // colour: A (pass A) or B, VU 8.x fade on colour A's alpha / colour B's RGB (0x0BE0, truncated)
    bool csel = (d3.z & R2D_CSEL) != 0u;   // (material flags)
    uint col = csel ? i.colb : i.cola;
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
    o.stq = float3(vu_mul((float)i.uv.x * (1.0 / 4096.0), q), vu_mul((float)i.uv.y * (1.0 / 4096.0), q), q);
    o.mr = uint3(mat, (dr.y >> 2) & 1u, so);
    if (skip) o.pos = float4(2.0, 2.0, 0.5, 1.0);   // (outside the clip volume: the batch's triangles are rejected)
    clip = float4(c[0], c[1], c[2], w);
    if (skip) clip = float4(2.0, 0.0, 0.0, 1.0);    // (outside x <= w: rejected by the clip pipeline too)
    // a clipped triangle's U, V (perspective-correct) and fog (MINI(w, VU 9.z) x Q is 1 while w < VU 9.z, and behind
    // the camera; the VU clipper's new vertices get theirs from the same formula)
    float wq = w < asfloat(d3.y) ? 1.0 : asfloat(d3.y) / w;
    alt = float4((float)i.uv.x * (1.0 / 4096.0), (float)i.uv.y * (1.0 / 4096.0), 1.0,
                 (float)((asuint(vu_add(asfloat(i.wbits), vu_mul(asfloat(d3.x), wq))) >> 4) & 255u));
}

R2O r2_vs(R2V i) {
    R2O o;
    float4 clip, alt;
    r2_vertex(i, o, clip, alt);
    return o;
}

R2C r2_vs_clip(R2V i) {
    R2O o;
    float4 clip, alt;
    r2_vertex(i, o, clip, alt);
    R2C r;
    r.pos = o.pos; r.col = o.col; r.fog = o.fog; r.stq = o.stq; r.mr = o.mr; r.gv = o.gv;
    r.clip = clip;
    r.alt = alt;
    return r;
}

// (r2-m2) Meshes crossing the VU's guard band (r2_capture.c inside_guard; notes/modern-renderer.md "R2 as built"): per
// triangle as the VU decides it (0x0C60: each vertex's CLIPW flags against |x|, |y|, |z| <= w): all three inside, the
// VU-exact screen vertices; all three outside one plane, nothing; else the VU's clipper (0x2540) would draw it as
// clipped fans - here the GPU clips the clip-space triangle against the same six planes (SV_ClipDistance), with
// perspective-correct attributes (not bit-exact: the clipper's own arithmetic is not reproduced; such triangles are
// near the camera and few).
uint r2_outcode(float4 c) {
    return (c.x > c.w ? 1u : 0u) | (c.x < -c.w ? 2u : 0u) | (c.y > c.w ? 4u : 0u) | (c.y < -c.w ? 8u : 0u) |
           (c.z > c.w ? 16u : 0u) | (c.z < -c.w ? 32u : 0u) | (c.w > 0.0 ? 0u : 64u);
}

[maxvertexcount(3)]
void r2_gs_clip(triangle R2C v[3], inout TriangleStream<R2K> ts) {
    uint o0 = r2_outcode(v[0].clip), o1 = r2_outcode(v[1].clip), o2 = r2_outcode(v[2].clip);
    if ((o0 & o1 & o2 & 63u) != 0u) return;
    bool inside = (o0 | o1 | o2) == 0u;
    R2Seg sg = r2_seg(v[0].mr.z);
    float4 fr = asfloat(fdata.Load4(sg.off.x + 32u));       // screen model: ax, bx, ay, by (gpu_r2_frame)
    float2 fz = asfloat(fdata.Load2(sg.off.x + 48u));       //               az, bz
    float kx = sg.map.x * (1.0 / 16.0), ky = sg.map.y * (1.0 / 16.0);
    [unroll] for (uint k = 0u; k < 3u; k++) {
        R2K o;
        o.col = v[k].col; o.mr = v[k].mr; o.gv = v[k].gv;
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
    float z : SV_DepthLessEqual;
};

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
        d.z = i.pos.z;
        return d;
    }
    uint mb = i.mr.x * 48u;
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
    uint t0 = d1.x, t1 = d1.y;
    uint tw = (t0 >> 26) & 15u, th = ((t0 >> 30) | (t1 << 2)) & 15u;
    float tw16 = (float)(16u << (tw > 10u ? 10u : tw)), th16 = (float)(16u << (th > 10u ? 10u : th));
    float iq = 1.0 / i.stq.z;              // (as the exact path's FF fast path, gs.hlsl STQ_FAST)
    precise float su = i.stq.x * iq;
    precise float tv = i.stq.y * iq;
    precise float su16 = su * tw16;
    precise float tv16 = tv * th16;
    uint t = (asuint(r2_misc.w) & 1u) != 0u ? 0x80808080u : tex_sample(st, f2i(su16), f2i(tv16), 1.0);   // (PS2_R2_DBG 1)
    uint4 vc = (uint4)max(i.col, 0.0);                     // the GS's Gouraud stepping truncates
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
    bool apass = (test & 1u) == 0u || atst_pass((test >> 1) & 7u, (test >> 4) & 0xFFu, As);
    if (apass == (i.mr.y != 0u)) discard;  // rule 0: failing fragments discarded, rule 1: passing ones
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
    R2Seg sg = r2_seg(i.mr.z);
    uint ao = As | ((sg.off.z & 1u) != 0u ? 0x80u : 0u);
    R2F o;
    o.c0 = float4(rgb, b2f(min(ao, 255u)));
#if R2_DUAL
    o.c1 = float4(0.0, 0.0, 0.0, a1);
#endif
#if R2_ZREC
    // the exact path's Z at this sample (gs.hlsl gs_frag): native pixel (px, py), sample (si, sj) of kx x ky
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
}
