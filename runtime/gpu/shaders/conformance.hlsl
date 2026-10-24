// Conformance kernels (runtime/gpu/gpu_conf.c): the int64 helpers, the exact integer Z formula and
// the depth encoding, compared bit for bit against the C reference on every adapter we run on; (P1) the depth
// round trips of the early-depth encoding through a D32 depth buffer and the triangle bound on real planes.
#include "int64.hlsli"

struct conf_in {
    uint2 a, b, d;                       // int64 operands (Z: row, dy, dx)
    int c;                               // signed 32-bit multiplier
    uint s;                              // shift 0..63; s % 3 - 1 = tie delta for the encoding test
    uint k;                              // divisor 1..65535
    uint zmax;
    int by0, px, py;
    uint ijk;                            // i | j << 8 | kx << 16 | ky << 24
    uint2 pad;
};

struct conf_out {
    uint2 add, sub, mul32, mul, asr, shl, fdiv, z16;
    uint zc;                             // clamped Z
    uint flags;                          // lt | eq << 1 | enc_gt << 2 | enc_ge << 3 | enc_eq << 4
    uint enc;                            // asuint(encode(zc))
    uint dec;                            // decode(encode(zc))
    uint bvtx, bconst;                   // (P1) asuint of the triangle-vertex and constant bounds of zc
    uint2 pad;
};

cbuffer conf_cb : register(b0) {
    uint count;
    uint width;                          // depth tests: pixels per row (square)
    uint mode;                           // vs_conf_px: 0 rasterized depth 1.0, 1 the constant bound of zin (+ 1)
    uint kxy;                            // tri bound test: kx | ky << 16
};

StructuredBuffer<conf_in> cin : register(t0);
RWStructuredBuffer<conf_out> cout : register(u0);

[numthreads(64, 1, 1)]
void cs_conformance(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    conf_in v = cin[id.x];
    conf_out o;
    o.add = i64_add(v.a, v.b);
    o.sub = i64_sub(v.a, v.b);
    o.mul32 = i64_mul_i32(v.a, v.c);
    o.mul = i64_mul(v.a, v.b);
    o.asr = i64_asr(v.a, v.s);
    o.shl = i64_shl(v.a, v.s);
    o.fdiv = i64_floordiv_small(v.a, v.k);
    uint i = v.ijk & 0xFFu, j = (v.ijk >> 8) & 0xFFu, kx = (v.ijk >> 16) & 0xFFu, ky = v.ijk >> 24;
    o.z16 = gs_z16(v.a, v.b, v.d, v.by0, v.px, v.py, i, j, kx, ky);
    o.zc = gs_z_clamp(o.z16, v.zmax);
    uint z1 = o.zc;
    // z1 - 1, z1 or z1 + 1 within [0, 0xFFFFFFFF]. Not `(uint)clamp((int)z1 + delta, 0, K)`: fxc
    // compiles that as an unsigned clamp (-1 becomes K); found by this kernel, see notes/wip/gpu-M0.md.
    uint sel = v.s % 3u;
    uint z2 = sel == 0u ? (z1 > 0u ? z1 - 1u : 0u) : sel == 2u ? (z1 < 0xFFFFFFFFu ? z1 + 1u : z1) : z1;
    float e1 = gs_z_encode(z1), e2 = gs_z_encode(z2);
    o.flags = (i64_lt(v.a, v.b) ? 1u : 0u) | (i64_eq(v.a, v.b) ? 2u : 0u) | (e1 > e2 ? 4u : 0u) |
              (e1 >= e2 ? 8u : 0u) | (e1 == e2 ? 16u : 0u);
    o.enc = asuint(e1);
    o.dec = gs_z_decode(e1);
    o.bvtx = asuint(gs_z_bound_vtx(z1));
    o.bconst = asuint(gs_z_bound_const(z1));
    o.pad = uint2(0u, 0u);
    cout[id.x] = o;
}

// ---- (P1) depth round trips through a D32 depth buffer ------------------------------------------------
// One point per pixel (POINTLIST, SV_VertexID = pixel index); the pixel shaders write the production encoding
// as SV_DepthLessEqual (noperspective centroid position, as gs.hlsl ps_ff) under the rasterized depth of `mode`.
Buffer<uint> zin : register(t0);
Texture2D<float> zsrv : register(t1);
RWBuffer<uint> zout : register(u0);

struct VP {
    noperspective centroid float4 pos : SV_Position;
};

VP vs_conf_px(uint id : SV_VertexID) {
    uint x = id % width, y = id / width;
    VP o;
    float w = (float)width;
    o.pos = float4(((float)x + 0.5) / w * 2.0 - 1.0, 1.0 - ((float)y + 0.5) / w * 2.0, 1.0, 1.0);
    if (mode == 1u) o.pos.z = gs_z_bound_const(zin[id]);
    return o;
}

uint px_index(float4 pos) { return (uint)pos.y * width + (uint)pos.x; }

float ps_depth_write(VP i) : SV_DepthLessEqual {
    return gs_z_encode(zin[px_index(i.pos)]);
}

// an FF draw's depth write over some pixels (zin 0xFFFFFFFF: none - discarded)
float ps_depth_ff(VP i) : SV_DepthLessEqual {
    uint z = zin[px_index(i.pos)];
    if (z == 0xFFFFFFFFu) discard;
    return gs_z_encode(z);
}

struct depth_test_out {
    uint c : SV_Target;
    float d : SV_DepthLessEqual;
};

// colour 1 where the hardware depth test (GREATER / GREATER_EQUAL against the first pass) passes
depth_test_out ps_depth_test(VP i) {
    depth_test_out o;
    o.c = 1u;
    o.d = gs_z_encode(zin[px_index(i.pos)]);
    return o;
}

// the depth SRV decoded back to integers (cs_zsync's read)
[numthreads(8, 8, 1)]
void cs_depth_read(uint3 id : SV_DispatchThreadID) {
    zout[id.y * width + id.x] = gs_z_decode(zsrv.Load(int3(id.xy, 0)));
}

// ---- (P1) the triangle bound on real Z planes ----------------------------------------------------------
// Triangles set up by the C side exactly as the frontend does (gs_tri_setup, gs_tri_plane, gs_rowstep_attr: the FF Z
// record) and positioned as vs_ff_tri positions them (GS coordinates * k + 0.5 through the 16384-pixel REF viewport);
// the vertex word is the vertex Z or 0xFFFFFFFF (no bound) as the frontend chose. Every covered sample computes the
// FF pixel shader's exact Z (gs_z16_sample, gs_z_clamp) and checks it against the rasterized depth: a value above it
// would be clamped by SV_DepthLessEqual (wrong depth). bres: [0] violations, [1] samples, [2] min slack (bounded
// triangles, z units, as int), [3] samples of bounded triangles, [4..7] the first violation (prim, x | y << 16, z,
// asuint(pos.z)), [8 + prim] 1: the triangle had a violation.
struct tri_rec { uint2 row, dy, dx; int by0; uint zmax; };
StructuredBuffer<tri_rec> trec : register(t1);
RWByteAddressBuffer bres : register(u1);

struct VT {
    noperspective centroid float4 pos : SV_Position;
    nointerpolation uint nb : NB;
};

VT vs_conf_tri(float2 xy : POSITION, uint zv : TEXCOORD1) {
    uint kx = kxy & 0xFFFFu, ky = kxy >> 16;
    float2 d3d = float2(xy.x * (float)kx + 0.5, xy.y * (float)ky + 0.5);
    float2 p = d3d * (2.0 / 16384.0);
    VT o;
    o.pos = float4(p.x, -p.y, gs_z_bound_vtx(zv), 1.0);
    o.nb = zv == 0xFFFFFFFFu ? 1u : 0u;
    return o;
}

void ps_conf_tri(VT i, uint prim : SV_PrimitiveID) {
    uint kx = kxy & 0xFFFFu, ky = kxy >> 16;
    tri_rec r = trec[prim];
    int2 P = (int2)i.pos.xy;
    int px = (int)((uint)P.x / kx), py = (int)((uint)P.y / ky);
    uint si = (uint)P.x - (uint)px * kx, sj = (uint)P.y - (uint)py * ky;
    i64 z16 = gs_z16_sample(r.row, r.dy, r.dx, r.by0, px, py, si, sj, kx, ky);
    uint zc = gs_z_clamp(z16, r.zmax);
    uint old;
    bres.InterlockedAdd(4, 1u, old);
    if (gs_z_encode(zc) > i.pos.z) {
        bres.InterlockedAdd(0, 1u, old);
        if (old == 0u) bres.Store4(16, uint4(prim, (uint)P.x | ((uint)P.y << 16), zc, asuint(i.pos.z)));
        bres.Store(32u + prim * 4u, 1u);          // (the triangles with a violation)
    }
    if (i.nb == 0u) {
        float slack = i.pos.z * 1048576.0 - (float)zc;
        int smin = (int)floor(slack), sold;
        bres.InterlockedMin(8, smin, sold);       // (int operand: the signed minimum)
        bres.InterlockedAdd(12, 1u, old);
    }
}
