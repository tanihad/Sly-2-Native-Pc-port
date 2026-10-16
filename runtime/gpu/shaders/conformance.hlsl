// Conformance kernels (runtime/gpu/gpu_conf.c): the int64 helpers, the exact integer Z formula and
// the depth encoding, compared bit for bit against the C reference on every adapter we run on.
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
    uint enc;                            // asuint(encode(min(zc, 0x3F000000)))
    uint dec;                            // decode(encode(...))
};

cbuffer conf_cb : register(b0) {
    uint count;
    uint width;
    uint2 cb_pad;
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
    uint z1 = min(o.zc, 0x3F000000u);
    // z1 - 1, z1 or z1 + 1 within [0, 0x3F000000]. Not `(uint)clamp((int)z1 + delta, 0, K)`: fxc
    // compiles that as an unsigned clamp (-1 becomes K); found by this kernel, see notes/wip/gpu-M0.md.
    uint sel = v.s % 3u;
    uint z2 = sel == 0u ? (z1 > 0u ? z1 - 1u : 0u) : sel == 2u ? min(z1 + 1u, 0x3F000000u) : z1;
    float e1 = gs_z_encode(z1), e2 = gs_z_encode(z2);
    o.flags = (i64_lt(v.a, v.b) ? 1u : 0u) | (i64_eq(v.a, v.b) ? 2u : 0u) | (e1 > e2 ? 4u : 0u) |
              (e1 >= e2 ? 8u : 0u) | (e1 == e2 ? 16u : 0u);
    o.enc = asuint(e1);
    o.dec = gs_z_decode(e1);
    cout[id.x] = o;
}

// ---- SV_Depth round trip through a D32S8 depth buffer --------------------------------------------
Buffer<uint> zin : register(t0);
Texture2D<float> zsrv : register(t1);
RWBuffer<uint> zout : register(u0);

float ps_depth_write(float4 pos : SV_Position) : SV_Depth {
    uint idx = (uint)pos.y * width + (uint)pos.x;
    return gs_z_encode(zin[idx]);
}

struct depth_test_out {
    uint c : SV_Target;
    float d : SV_Depth;
};

// colour 1 where the hardware depth test (GREATER / GREATER_EQUAL against the first pass) passes
depth_test_out ps_depth_test(float4 pos : SV_Position) {
    depth_test_out o;
    uint idx = (uint)pos.y * width + (uint)pos.x;
    o.c = 1u;
    o.d = gs_z_encode(zin[idx]);
    return o;
}

// the ROV tier reads depth through an SRV of the depth target: decode it back to integers
[numthreads(8, 8, 1)]
void cs_depth_read(uint3 id : SV_DispatchThreadID) {
    zout[id.y * width + id.x] = gs_z_decode(zsrv.Load(int3(id.xy, 0)));
}
