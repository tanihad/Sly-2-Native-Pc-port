// (ai) Reimagined textures (runtime/gpu/gpu_tex_ai.h, notes/reimagined.md): the passes around the Real-ESRGAN net
// (ai_up.hlsl) and the surface maps of the lighting (gpu_tex_normals.h). Used only under Look = Reimagined.
//   cs_ai_post   the net's 4x output of a batch of padded tiles -> RGBA words (the GS's bytes) of each tile's place in its
//                texture's upload region: the padding cropped, the source's fine grain added back (optional), alpha
//                Catmull-Rom 4x from the source (cut-outs: a steep ramp around 50 %: smooth contours, the same coverage)
//   cs_etn       one level of a texture's surface map (normal x, y, height, cavity) from the colour levels the Enhanced
//                build left in its scratch (tex.hlsl e_lv): level k of the map (half the colour's size) from colour level
//                k + 1, height = luminance minus its coarser levels (a band-pass: shading gradients do not tilt
//                surfaces, painted detail does)

// ---- cs_ai_post ---------------------------------------------------------------------------------------------------------
cbuffer ai_post_cb : register(b4) {
    uint4 p_b;                           // first job, padding (input texels), -, -
    float4 p_f;                          // grain (0: off), -, -, -
};
RWStructuredBuffer<float> p_out : register(u0);   // the net's output: per image 3 planes of 4 wp x 4 hp
RWStructuredBuffer<float> p_img : register(u1);   // the net's input: per image 3 planes of wp x hp (0..1)
RWByteAddressBuffer p_up : register(u2);          // upload regions (words: 0xAABBGGRR, alpha 0x80 = 1)
ByteAddressBuffer p_al : register(t0);            // per image the padded source alpha (floats 0..255)
ByteAddressBuffer p_jobs : register(t1);          // 16 words per image: below

float cr_w(float t, uint k) {
    float t2 = t * t, t3 = t2 * t;
    if (k == 0u) return -0.5 * t3 + t2 - 0.5 * t;
    if (k == 1u) return 1.5 * t3 - 2.5 * t2 + 1.0;
    if (k == 2u) return -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    return 0.5 * t3 - 0.5 * t2;
}

// job: up base (words), pitch (words: 4 w), x, y of the tile in the texture (4x texels), width, height (4x texels),
// padded input width, height (texels), flags (1 cut-out alpha, 2 opaque)
[numthreads(8, 8, 1)]
void cs_ai_post(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID) {
    uint o = (p_b.x + gid.z) * 64u;
    uint4 a = p_jobs.Load4(o), b = p_jobs.Load4(o + 16u), c = p_jobs.Load4(o + 32u);
    uint base = a.x, pitch = a.y, tx = a.z, ty = a.w, cw = b.x, ch = b.y, wp = b.z, hp = b.w, flags = c.x;
    if (id.x >= cw || id.y >= ch) return;
    uint pad = p_b.y, W4 = 4u * wp, P = wp * hp, P16 = 16u * P;
    uint X = id.x + 4u * pad, Y = id.y + 4u * pad;           // (the padded output pixel)
    uint z = gid.z;
    float3 rgb = float3(p_out[(z * 3u + 0u) * P16 + Y * W4 + X], p_out[(z * 3u + 1u) * P16 + Y * W4 + X],
                        p_out[(z * 3u + 2u) * P16 + Y * W4 + X]);
    float2 s = (float2(X, Y) + 0.5) * 0.25 - 0.5;            // (the source position, padded texels)
    float2 fb = floor(s);
    int2 bi = (int2)fb;
    float2 f = s - fb;
    float al = 0.0;
    float3 gr = float3(0.0, 0.0, 0.0);
    [unroll] for (uint j = 0u; j < 4u; j++) {
        int yy = clamp(bi.y - 1 + (int)j, 0, (int)hp - 1);
        [unroll] for (uint i = 0u; i < 4u; i++) {
            int xx = clamp(bi.x - 1 + (int)i, 0, (int)wp - 1);
            float w = cr_w(f.x, i) * cr_w(f.y, j);
            uint q = (uint)yy * wp + (uint)xx;
            al += w * asfloat(p_al.Load((z * P + q) * 4u));
            [branch] if (p_f.x > 0.0) {                     // the source's grain: texel minus its 3 x 3 binomial
                float3 cs = float3(p_img[(z * 3u) * P + q], p_img[(z * 3u + 1u) * P + q], p_img[(z * 3u + 2u) * P + q]);
                float3 bl = float3(0.0, 0.0, 0.0);
                [unroll] for (int dy = -1; dy <= 1; dy++)
                    [unroll] for (int dx = -1; dx <= 1; dx++) {
                        uint q2 = (uint)clamp(yy + dy, 0, (int)hp - 1) * wp + (uint)clamp(xx + dx, 0, (int)wp - 1);
                        float k = (dy != 0 ? 0.5 : 1.0) * (dx != 0 ? 0.5 : 1.0) * 0.25;
                        bl += k * float3(p_img[(z * 3u) * P + q2], p_img[(z * 3u + 1u) * P + q2], p_img[(z * 3u + 2u) * P + q2]);
                    }
                gr += w * (cs - bl);
            }
        }
    }
    rgb += p_f.x * gr;
    if (flags & 1u) al = (al - 127.5) * 2.5 + 127.5;
    uint ga = (flags & 2u) ? 128u : (uint)(clamp(al, 0.0, 255.0) * (128.0 / 255.0) + 0.5);
    uint3 q8 = (uint3)(saturate(rgb) * 255.0 + 0.5);
    p_up.Store((base + (ty + id.y) * pitch + tx + id.x) * 4u, q8.x | (q8.y << 8) | (q8.z << 16) | (ga << 24));
}

// ---- cs_etn --------------------------------------------------------------------------------------------------------------
cbuffer et_cb : register(b3) {           // (tex.hlsl's: level = the map's level, first job; e_r.y = the slope scale)
    uint4 e_b;
    float4 e_p;
    float4 e_q;
    float4 e_r;
};
ByteAddressBuffer e_jobs : register(t1);
RWByteAddressBuffer e_lv : register(u0);
RWTexture2DArray<unorm float4> n_tex : register(u5);     // the normal class, level e_b.x
RWByteAddressBuffer n_ok : register(u6);                 // class * 4096 + slice: 1 = has a map

static uint j_lw, j_lh, j_flags, j_S, j_slice, j_lv, j_nm;
void njob_load(uint z) {
    uint o = (e_b.y + z) * 64u;
    uint4 a = e_jobs.Load4(o), b = e_jobs.Load4(o + 16u), c = e_jobs.Load4(o + 32u);
    j_lw = a.y; j_lh = a.z; j_flags = a.w; j_S = b.x; j_slice = b.y; j_lv = b.z; j_nm = c.y;
}
uint nlv_off(uint k) { uint s = j_S >> k; return j_lv + (4u * j_S * j_S - 4u * s * s) / 3u; }
float nlum(uint lvl, int x, int y) {
    uint s = j_S >> lvl;
    int m = (int)s - 1;
    x = (j_flags & 1u) ? clamp(x, 0, m) : (x & m);
    y = (j_flags & 2u) ? clamp(y, 0, m) : (y & m);
    uint w = e_lv.Load((nlv_off(lvl) + (uint)y * s + (uint)x) * 4u);
    return dot(float3(w & 255u, (w >> 8) & 255u, (w >> 16) & 255u), float3(0.299, 0.587, 0.114)) * (1.0 / 255.0);
}
float nlum_bil(uint lvl, float2 p) {     // p: texel units of that level (centres at i + 0.5)
    float2 q = p - 0.5, fq = floor(q), f = q - fq;
    int2 b = (int2)fq;
    return lerp(lerp(nlum(lvl, b.x, b.y), nlum(lvl, b.x + 1, b.y), f.x), lerp(nlum(lvl, b.x, b.y + 1), nlum(lvl, b.x + 1, b.y + 1), f.x), f.y);
}
// height at map texel (x, y) of colour level L: the luminance minus the mean of its 4x and 8x coarser levels
float nheight(uint L, int x, int y) {
    uint l2 = min(L + 2u, j_nm - 1u), l3 = min(L + 3u, j_nm - 1u);
    float2 p = float2((float)x + 0.5, (float)y + 0.5);
    return nlum(L, x, y) - 0.5 * (nlum_bil(l2, p / (float)(1u << (l2 - L))) + nlum_bil(l3, p / (float)(1u << (l3 - L))));
}
[numthreads(8, 8, 1)]
void cs_etn(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID) {
    njob_load(gid.z);
    uint k = e_b.x, L = k + 1u, ns = j_S >> L;
    if (L >= j_nm || id.x >= ns || id.y >= ns) return;
    int x = (int)id.x, y = (int)id.y;
    float h = nheight(L, x, y);
    float hl = nheight(L, x - 1, y), hr = nheight(L, x + 1, y), hu = nheight(L, x, y - 1), hd = nheight(L, x, y + 1);
    float sl = e_r.y / (float)(1u << k);               // (slopes in level-0 map texels: coarser levels flatten)
    float3 n = normalize(float3(-(hr - hl) * 0.5 * sl, -(hd - hu) * 0.5 * sl, 1.0));
    float cav = saturate(1.0 - 2.5 * max(0.0, 0.25 * (hl + hr + hu + hd) - h) * 4.0);
    n_tex[uint3(id.xy, j_slice)] = float4(n.xy * 0.5 + 0.5, saturate(0.5 + h), cav);
    if (k == 0u && id.x == 0u && id.y == 0u) n_ok.Store(((firstbithigh(j_S) - 6u) * 4096u + j_slice) * 4u, 1u);
}
