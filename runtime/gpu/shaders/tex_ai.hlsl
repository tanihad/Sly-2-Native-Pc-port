// (ai) Reimagined textures (runtime/gpu/gpu_tex_ai.h, notes/reimagined.md): the passes around the Real-ESRGAN net
// (ai_up.hlsl) and the surface maps of the lighting (gpu_tex_normals.h). Used only under Look = Reimagined.
//   cs_ai_post   the net's 4x output of a batch of padded tiles -> RGBA words (the GS's bytes) of each tile's place in its
//                texture's upload region: the padding cropped, the source's fine grain added back (optional), alpha
//                Catmull-Rom 4x from the source (cut-outs: a steep ramp around 50 %: smooth contours, the same coverage),
//                faint lines the net erased put back (below)
//   cs_ai_res    the restore's residual per source texel: the source minus the 4 x 4 box of the net's output. cs_ai_post
//                adds it back (bilinear 4x) where it continues along a direction (grout, grain, cracks, strokes: lines)
//                and is faint (the net keeps strong edges) - never isolated dots (dither: the net rightly removed it)
//   cs_etn       one level of a texture's surface map (normal x, y, height, cavity) from the colour levels the Enhanced
//                build left in its scratch (tex.hlsl e_lv): level k of the map (half the colour's size) from colour level
//                k + 1, height = luminance minus its coarser levels (a band-pass: shading gradients do not tilt
//                surfaces, painted detail does)

// ---- cs_ai_post ---------------------------------------------------------------------------------------------------------
cbuffer ai_post_cb : register(b4) {
    uint4 p_b;                           // first job, padding (input texels), -, -
    float4 p_f;                          // grain (0: off), restore strength (0: off), clarity (0: off), -
};
RWStructuredBuffer<float> p_out : register(u0);   // the net's output: per image 3 planes of 4 wp x 4 hp
RWStructuredBuffer<float> p_img : register(u1);   // the net's input: per image 3 planes of wp x hp (0..1)
RWByteAddressBuffer p_up : register(u2);          // upload regions (words: 0xAABBGGRR, alpha 0x80 = 1)
ByteAddressBuffer p_al : register(t0);            // per image the padded source alpha (floats 0..255)
ByteAddressBuffer p_jobs : register(t1);          // 16 words per image: below
RWByteAddressBuffer p_res : register(u3);         // per image 3 planes of wp x hp residuals (floats, 0..1 units)

// cs_ai_res: one thread per padded source texel (dispatch wp / 8, hp / 8, n)
[numthreads(8, 8, 1)]
void cs_ai_res(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID) {
    uint o = (p_b.x + gid.z) * 64u;
    uint4 b = p_jobs.Load4(o + 16u);
    uint wp = b.z, hp = b.w, P = wp * hp, W4 = 4u * wp, P16 = 16u * P, z = gid.z;
    if (id.x >= wp || id.y >= hp) return;
    uint q = id.y * wp + id.x;
    [unroll] for (uint c = 0u; c < 3u; c++) {
        float s = 0.0;
        [unroll] for (uint j = 0u; j < 4u; j++)
            [unroll] for (uint i = 0u; i < 4u; i++) s += p_out[(z * 3u + c) * P16 + (4u * id.y + j) * W4 + 4u * id.x + i];
        p_res.Store(((z * 3u + c) * P + q) * 4u, asuint(p_img[(z * 3u + c) * P + q] - s * (1.0 / 16.0)));
    }
}
float3 res_at(uint z, uint P, uint wp, int x, int y) {
    uint q = (uint)y * wp + (uint)x;
    return float3(asfloat(p_res.Load(((z * 3u) * P + q) * 4u)), asfloat(p_res.Load(((z * 3u + 1u) * P + q) * 4u)),
                  asfloat(p_res.Load(((z * 3u + 2u) * P + q) * 4u)));
}
// the residual of texel (x, y) kept: its luminance continues on both sides along some direction, faint (fades out from
// 10 to 28 of 255)
float3 res_kept(uint z, uint P, uint wp, uint hp, int x, int y) {
    float3 r = res_at(z, P, wp, x, y);
    const float3 lw = float3(0.299, 0.587, 0.114);
    float l = dot(r, lw), al = abs(l);
    if (al < 1.5 / 255.0) return float3(0.0, 0.0, 0.0);
    float best = 0.0;
    int2 d4[4] = { int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1) };
    [unroll] for (uint k = 0u; k < 4u; k++) {
        int2 a = clamp(int2(x, y) + d4[k], int2(0, 0), int2((int)wp - 1, (int)hp - 1));
        int2 c = clamp(int2(x, y) - d4[k], int2(0, 0), int2((int)wp - 1, (int)hp - 1));
        float l1 = dot(res_at(z, P, wp, a.x, a.y), lw), l2 = dot(res_at(z, P, wp, c.x, c.y), lw);
        if (l1 * l > 0.0 && l2 * l > 0.0) best = max(best, min(min(abs(l1), abs(l2)), al) / al);
    }
    float fade = saturate((28.0 / 255.0 - al) / (18.0 / 255.0));
    return r * (best * fade);
}

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
    [branch] if (p_f.z > 0.0) {                         // clarity: the luminance's mid band (a Gaussian of 5 x 5 taps 2
        const float3 lw = float3(0.299, 0.587, 0.114);  // pixels apart: ~1 source texel) boosted, at most +-24 levels
        float gk[5] = { 1.0, 4.0, 6.0, 4.0, 1.0 };
        float bl = 0.0;
        [unroll] for (uint gy = 0u; gy < 5u; gy++) {
            uint yy2 = (uint)clamp((int)Y + 2 * (int)gy - 4, 0, (int)(4u * hp) - 1);
            [unroll] for (uint gx = 0u; gx < 5u; gx++) {
                uint xx2 = (uint)clamp((int)X + 2 * (int)gx - 4, 0, (int)W4 - 1);
                uint pq = yy2 * W4 + xx2;
                bl += gk[gy] * gk[gx] * dot(float3(p_out[(z * 3u) * P16 + pq], p_out[(z * 3u + 1u) * P16 + pq], p_out[(z * 3u + 2u) * P16 + pq]), lw);
            }
        }
        float lc = dot(float3(p_out[(z * 3u) * P16 + Y * W4 + X], p_out[(z * 3u + 1u) * P16 + Y * W4 + X], p_out[(z * 3u + 2u) * P16 + Y * W4 + X]), lw);
        rgb += clamp(p_f.z * (lc - bl * (1.0 / 256.0)), -24.0 / 255.0, 24.0 / 255.0);
    }
    [branch] if (p_f.y > 0.0) {                         // the restore: bilinear 4x of the kept residuals
        float2 t = s - 0.0;
        float2 tb = floor(t);
        int2 t0 = (int2)tb;
        float2 tf = t - tb;
        float3 rr = float3(0.0, 0.0, 0.0);
        [unroll] for (uint jj = 0u; jj < 2u; jj++)
            [unroll] for (uint ii = 0u; ii < 2u; ii++) {
                int2 q = clamp(t0 + int2((int)ii, (int)jj), int2(0, 0), int2((int)wp - 1, (int)hp - 1));
                rr += (ii ? tf.x : 1.0 - tf.x) * (jj ? tf.y : 1.0 - tf.y) * res_kept(z, P, wp, hp, q.x, q.y);
            }
        rgb += p_f.y * rr;
    }
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
