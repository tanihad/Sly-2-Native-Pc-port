// Submission benchmark shaders (milestone M2, runtime/gpu/gpu_exec.c): the vertex/record fetch and
// the bindings of the real design (notes/d3d11-renderer.md 3.5, 4.5) with trivial pixel work. They
// exist so that every D3D draw of the frontend's command stream runs with realistic state: input
// layout, record SRVs, per-draw constants through constant-buffer offsets, SV_Depth, dual-source
// output, discard, ROV, texture SRV kinds. No result is correct; only the CPU cost of submitting is
// measured. Permutations: perms.txt (DUAL, ZOUT, DISCARD, TEX = 0 none / 1 vram_buf / 2 target view).

cbuffer draw_cb : register(b0) {
    uint rec_first;                      // byte offset of the draw's first record
    uint draw_flags;
    uint2 pad0;
    float2 ndc_scale;                    // 2 k / size
    float2 ndc_bias;                     // k / size - 1 (D3D pixel centre = GS position * k + 0.5)
};

ByteAddressBuffer vram_buf : register(t0);
Texture2D<float4> view : register(t1);
Texture2D<float> depth_srv : register(t2);
ByteAddressBuffer recs : register(t3);
ByteAddressBuffer states : register(t4);
ByteAddressBuffer clut : register(t5);

struct V {
    float4 pos : SV_Position;
    nointerpolation uint rgba : COLOR0;
    nointerpolation uint fog_state : COLOR1;
    float3 stq : TEXCOORD0;
};

float4 to_clip(float2 gs) {
    float2 p = gs * ndc_scale + ndc_bias;
    return float4(p.x, -p.y, 0.0, 1.0);
}

// ---- vertex shaders ------------------------------------------------------------------------------
struct VIn {
    float2 xy : POSITION;
    uint rgba : COLOR0;
    uint fog_state : COLOR1;
    float3 stq : TEXCOORD0;
};

V vs_tri(VIn i) {
    V o;
    o.pos = to_clip(i.xy);
    o.rgba = i.rgba;
    o.fog_state = i.fog_state;
    o.stq = i.stq;
    return o;
}

// sprite record (48 B): int16 x0, y0, x1, y1 | rgba | z | fog | state | ua va du dv | pad
V vs_sprite(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint base = rec_first + inst * 48;
    uint2 r = recs.Load2(base);
    float x0 = (float)(int)(r.x << 16 >> 16), y0 = (float)((int)r.x >> 16);
    float x1 = (float)(int)(r.y << 16 >> 16), y1 = (float)((int)r.y >> 16);
    float2 c = float2((vid & 1) ? x1 : x0, (vid & 2) ? y1 : y0);
    uint4 a = recs.Load4(base + 8);
    V o;
    o.pos = to_clip(c - 0.5);
    o.rgba = a.x;
    o.fog_state = a.z | a.w << 8;
    o.stq = float3(asfloat(recs.Load(base + 24)), 0.0, 1.0);
    return o;
}

// line pixel record (32 B): int16 px, py | z | rgba | fog_state | s t q | pad
V vs_linepix(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint base = rec_first + inst * 32;
    uint4 r = recs.Load4(base);
    float px = (float)(int)(r.x << 16 >> 16), py = (float)((int)r.x >> 16);
    float2 c = float2(px + ((vid & 1) ? 0.5 : -0.5), py + ((vid & 2) ? 0.5 : -0.5));
    V o;
    o.pos = to_clip(c);
    o.rgba = r.z;
    o.fog_state = r.w;
    o.stq = asfloat(recs.Load3(base + 16));
    return o;
}

// quad over the target (clears with a write mask, Z resolves, depth loads): vid 0..3
V vs_quad(uint vid : SV_VertexID) {
    V o;
    o.pos = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0, 0.0, 1.0);
    o.rgba = 0xFF808080u;
    o.fog_state = 0;
    o.stq = 0;
    return o;
}

// ---- pixel shaders ----------------------------------------------------------------------------------
#ifndef TEX
#define TEX 0
#endif

float4 unpack(uint c) { return float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0; }

float4 tex_fetch(V i) {
#if TEX == 1
    uint st = i.fog_state >> 8;
    uint tbp = states.Load(st * 96);                       // StateRec TEX0 low word
    uint a = ((tbp & 0x3FFF) * 256 + ((uint)i.stq.x & 0xFF) * 4) & 0x3FFFFC;
    uint t = vram_buf.Load(a);
    return unpack(clut.Load(((t & 0xFF) * 4) & 0x3FFFFC));
#elif TEX == 2
    return view.Load(int3((int2)i.pos.xy & 63, 0));
#else
    return float4(1, 1, 1, 1);
#endif
}

struct POut {
    float4 c0 : SV_Target0;
#ifdef DUAL
    float4 c1 : SV_Target1;
#endif
#ifdef ZOUT
    float z : SV_Depth;
#endif
};

POut ps_ff(V i, uint prim : SV_PrimitiveID) {
    POut o;
    float4 c = unpack(i.rgba) * tex_fetch(i);
#ifdef DISCARD
    if (c.a < 0.25) discard;
#endif
    o.c0 = c;
#ifdef DUAL
    o.c1 = float4(0, 0, 0, c.a);
#endif
#ifdef ZOUT
    uint zlo = recs.Load(rec_first + prim * 32);
    o.z = saturate(asfloat((zlo >> 9) | 0x3F000000u) - 0.5);
#endif
    return o;
}

// ROV tier: colour through a rasterizer-ordered UAV (read-modify-write in primitive order); Z read
// from the depth SRV (read-only DSV bound alongside)
RasterizerOrderedTexture2D<uint4> rov : register(u0);

void ps_rov(V i) {
    int2 p = (int2)i.pos.xy;
    uint4 d = rov[p];
    float zd = depth_srv.Load(int3(p, 0));
    float4 c = unpack(i.rgba) * tex_fetch(i);
    if ((draw_flags & 1) && zd > 0.75) return;
    rov[p] = (d + (uint4)(c * 8.0)) & 0xFF;
}

// Z resolve / depth load / masked clear quads
float ps_depth(V i) : SV_Depth { return 0.5; }
float4 ps_flat(V i) : SV_Target0 { return unpack(i.rgba); }

// ---- compute: loads into colour targets, ALIAS16 batches ----------------------------------------------
RWTexture2D<uint4> dst : register(u0);

[numthreads(8, 8, 1)]
void cs_touch(uint3 id : SV_DispatchThreadID) {
    uint2 p = uint2(id.x & 511, id.y + (id.x >> 9) * 8);
    dst[p] = dst[p] + uint4(1, 0, 0, 0);
}
