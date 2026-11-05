// (tex) Enhanced textures: the compute passes that build an enhanced image of a decoded texture (runtime/gpu/gpu_tex.h,
// notes/enhanced-textures.md). Source: the decoded texture cache (gs.hlsl texdec: level 0 as 0xAABBGGRR words, CLUT
// applied, alpha the GS byte, 0x80 = 1.0) or an HD-pack image uploaded in the same form. Output: one slice of a square
// RGBA8 Texture2DArray class (the GS bytes, unorm) with its whole mip chain.
//   cs_et_up     edge-directed upscale to the slice size S, plus an unsharp mask at the source scale (level 0)
//   cs_et_mip    one mip level from the previous one (alpha-weighted 2 x 2 average), alpha histogram
//   cs_et_cov    per level, the alpha scale that keeps the alpha test's coverage of level 0 (alpha-tested images)
//   cs_et_store  the scaled level into the texture
//   cs_et_hash   the content hash of a decoded image (64 bits, order-dependent, deterministic)

cbuffer et_cb : register(b3) {
    uint4 e_a;                           // source base (words), log2 w, log2 h, flags (1 clamp u, 2 clamp v, 4 no steering)
    uint4 e_b;                           // S (slice size), slice, scratch source offset, scratch destination offset (words)
    uint4 e_c;                           // source size, destination size, level, alpha ref (0: no coverage scaling)
    float4 e_p;                          // sharpen, sigma across, sigma along, minimum edge strength
    float4 e_q;                          // flat below this local range (0..255: smoothed), detail above this (sharpened)
};
ByteAddressBuffer e_src : register(t0);                   // decoded words (texdec) / the uploaded HD image
RWByteAddressBuffer e_lv : register(u0);                  // scratch: level 0 .. n of the image as words
RWTexture2DArray<unorm float4> e_tex : register(u1);      // the class array, one mip level (the slice is e_b.y)
RWByteAddressBuffer e_hist : register(u2);                // alpha histograms: level l at word l * 256; scales at 4096 + l
RWByteAddressBuffer e_out : register(u3);                 // hashes: 4 words per request

#define HIST_SCALES 4096u

float4 et_unpack(uint w) { return float4(w & 255u, (w >> 8) & 255u, (w >> 16) & 255u, w >> 24); }
uint et_pack(float4 c) {
    uint4 b = (uint4)clamp(c + 0.5, 0.0, 255.0);
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}
// the texel (x, y) of the source with the image's addressing (REPEAT wraps, CLAMP clamps)
float4 et_fetch(int x, int y, uint W, uint H) {
    x = (e_a.w & 1u) ? clamp(x, 0, (int)W - 1) : (x & (int)(W - 1u));
    y = (e_a.w & 2u) ? clamp(y, 0, (int)H - 1) : (y & (int)(H - 1u));
    return et_unpack(e_src.Load((e_a.x + (uint)y * W + (uint)x) * 4u));
}
float et_cr(float t, uint k) {           // Catmull-Rom weight of tap k (offsets -1, 0, 1, 2) at fraction t
    float t2 = t * t, t3 = t2 * t;
    if (k == 0u) return -0.5 * t3 + t2 - 0.5 * t;
    if (k == 1u) return 1.5 * t3 - 2.5 * t2 + 1.0;
    if (k == 2u) return -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    return 0.5 * t3 - 0.5 * t2;
}

groupshared uint gs_hist[256];
void hist_begin(uint li) {
    gs_hist[li] = 0u;
    if (li < 192u) gs_hist[li + 64u] = 0u;   // (64 threads: 4 bins each)
    if (li < 128u) gs_hist[li + 128u] = 0u;
    if (li < 64u) gs_hist[li + 192u] = 0u;
    GroupMemoryBarrierWithGroupSync();
}
void hist_end(uint li, uint level) {
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 0u; k < 4u; k++) {
        uint b = li + 64u * k;
        uint n = gs_hist[b];
        if (n != 0u) e_hist.InterlockedAdd((level * 256u + b) * 4u, n);
    }
}

// ---- the upscale: one thread per output pixel of the S x S slice ---------------------------------------------------------
// Output pixel (X, Y) sits at source position p = (X + 0.5) * size / S - 0.5 (texel centres at integers). Its 4 x 4
// source neighbourhood gives:
//   - transparent texels (alpha < 1/2) the colour of their opaque neighbours (no dark fringes along cut-outs; alpha kept)
//   - the colour structure tensor (Sobel gradients of RGB and alpha at the central 2 x 2, bilinearly weighted): the
//     edge's direction and coherence
//   - two interpolations: Catmull-Rom, clamped to the central 2 x 2's range (no ringing), and a Gaussian kernel steered
//     along the edge (sigma along > sigma across: smooth, anti-aliased edges and lines instead of the source's staircase).
//     Coherent, strong edges take the steered one; texture noise and corners stay Catmull-Rom.
//   - an unsharp mask at the source scale: + sharpen x (result - the neighbourhood's Gaussian), the increment limited to
//     the central 2 x 2's range, RGB only (the painted detail the PS2's bilinear filter smeared, without halos)
[numthreads(8, 8, 1)]
void cs_et_up(uint3 id : SV_DispatchThreadID, uint li : SV_GroupIndex) {
    uint S = e_b.x, ref = e_c.w;
    if (ref != 0u) hist_begin(li);
    bool ok = id.x < S && id.y < S;
    uint W = 1u << e_a.y, H = 1u << e_a.z;
    float2 p = (float2(id.xy) + 0.5) * float2((float)W, (float)H) / (float)S - 0.5;
    float2 fb = floor(p);
    int2 b = (int2)fb;
    float2 f = p - fb;
    float4 c[16];
    [unroll] for (uint j = 0u; j < 4u; j++)
        [unroll] for (uint i = 0u; i < 4u; i++)
            c[j * 4u + i] = et_fetch(b.x - 1 + (int)i, b.y - 1 + (int)j, W, H);
    // transparent texels: the colour of the opaque ones, weighted by alpha and closeness
    {
        float3 acc = float3(0.0, 0.0, 0.0);
        float wsum = 0.0;
        [unroll] for (uint k = 0u; k < 16u; k++) {
            float2 d = float2((float)(k & 3u) - 1.0, (float)(k >> 2) - 1.0) - f;
            float w = c[k].w * (1.0 / (1.0 + dot(d, d)));
            acc += c[k].rgb * w;
            wsum += w;
        }
        if (wsum > 0.0) {
            float3 avg = acc / wsum;
            [unroll] for (uint k2 = 0u; k2 < 16u; k2++)
                if (c[k2].w < 64.0) c[k2].rgb = lerp(avg, c[k2].rgb, c[k2].w * (1.0 / 64.0));
        }
    }
    // the structure tensor (channels scaled to 0..1, alpha 0x80 = 1)
    float txx = 0.0, tyy = 0.0, txy = 0.0;
    [unroll] for (uint jj = 1u; jj <= 2u; jj++)
        [unroll] for (uint ii = 1u; ii <= 2u; ii++) {
            float4 sc = float4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 1.0 / 128.0);
            float4 gx = (c[(jj - 1u) * 4u + ii + 1u] + 2.0 * c[jj * 4u + ii + 1u] + c[(jj + 1u) * 4u + ii + 1u] -
                         c[(jj - 1u) * 4u + ii - 1u] - 2.0 * c[jj * 4u + ii - 1u] - c[(jj + 1u) * 4u + ii - 1u]) * sc * 0.125;
            float4 gy = (c[(jj + 1u) * 4u + ii - 1u] + 2.0 * c[(jj + 1u) * 4u + ii] + c[(jj + 1u) * 4u + ii + 1u] -
                         c[(jj - 1u) * 4u + ii - 1u] - 2.0 * c[(jj - 1u) * 4u + ii] - c[(jj - 1u) * 4u + ii + 1u]) * sc * 0.125;
            float wb = (ii == 1u ? 1.0 - f.x : f.x) * (jj == 1u ? 1.0 - f.y : f.y);
            txx += wb * dot(gx, gx);
            tyy += wb * dot(gy, gy);
            txy += wb * dot(gx, gy);
        }
    float hd = 0.5 * (txx - tyy);
    float rt = sqrt(hd * hd + txy * txy);
    float l1 = 0.5 * (txx + tyy) + rt, l2 = 0.5 * (txx + tyy) - rt;
    float2 gd = abs(txy) > 1e-9 ? normalize(float2(txy, l1 - txx)) : (txx >= tyy ? float2(1.0, 0.0) : float2(0.0, 1.0));
    float2 ed = float2(-gd.y, gd.x);
    float coh = (l1 - l2) / (l1 + l2 + 1e-9);
    float steer = smoothstep(0.25, 0.75, coh) * saturate((sqrt(max(l1, 0.0)) - e_p.w) / max(e_p.w, 1e-4));
    if (e_a.w & 4u) steer = 0.0;
    // Catmull-Rom, steered Gaussian, isotropic Gaussian (the blur of the unsharp mask)
    float4 rc = float4(0.0, 0.0, 0.0, 0.0), rs = rc, rg = rc;
    float ws = 0.0, wg = 0.0;
    float isa = 0.5 / (e_p.z * e_p.z), isc = 0.5 / (e_p.y * e_p.y);
    [unroll] for (uint k3 = 0u; k3 < 16u; k3++) {
        uint i3 = k3 & 3u, j3 = k3 >> 2;
        float2 d = float2((float)i3 - 1.0, (float)j3 - 1.0) - f;
        rc += c[k3] * (et_cr(f.x, i3) * et_cr(f.y, j3));
        float da = dot(d, ed), dc = dot(d, gd);
        float w = exp(-(da * da * isa + dc * dc * isc));
        rs += c[k3] * w;
        ws += w;
        float w2 = exp(-dot(d, d) * (0.5 / 0.81));
        rg += c[k3] * w2;
        wg += w2;
    }
    rs /= ws;
    rg /= wg;
    float4 mn = min(min(c[5], c[6]), min(c[9], c[10])), mx = max(max(c[5], c[6]), max(c[9], c[10]));
    rc = clamp(rc, mn, mx);
    float4 r = lerp(rc, rs, steer);
    // the sharpening's limit: the 3 x 3 texels around the nearest one (never beyond what the texture has there: no halo)
    int2 nb = int2(f.x >= 0.5 ? 2 : 1, f.y >= 0.5 ? 2 : 1);
    float3 lo = r.rgb, hi = r.rgb;
    [unroll] for (int dy = -1; dy <= 1; dy++)
        [unroll] for (int dx = -1; dx <= 1; dx++) {
            float3 v = c[(nb.y + dy) * 4 + nb.x + dx].rgb;
            lo = min(lo, v); hi = max(hi, v);
        }
    // low-contrast neighbourhoods (a smooth gradient in 4-bit palette steps): smoothed, not sharpened (sharpening would
    // turn the steps into ridges); detail and edges: the full unsharp mask
    float rng = dot(hi - lo, float3(0.299, 0.587, 0.114));
    float k = (e_a.w & 4u) ? 1.0 : smoothstep(e_q.x, e_q.y, rng);   // (HD images / no upscale: as they are)
    float3 base = lerp(rg.rgb, r.rgb, k);
    r.rgb = clamp(base + (e_p.x * k) * (r.rgb - rg.rgb), lo, hi);
    uint w = et_pack(r);
    if (ok) {
        e_lv.Store((e_b.w + id.y * S + id.x) * 4u, w);
        e_tex[uint3(id.xy, e_b.y)] = et_unpack(w) * (1.0 / 255.0);
        if (ref != 0u) InterlockedAdd(gs_hist[w >> 24], 1u);
    }
    if (ref != 0u) hist_end(li, 0u);
}

// ---- a mip level: alpha-weighted 2 x 2 average of the previous level (scratch), histogram; without coverage scaling
// it goes straight into the texture --------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void cs_et_mip(uint3 id : SV_DispatchThreadID, uint li : SV_GroupIndex) {
    uint ss = e_c.x, ds = e_c.y, ref = e_c.w;
    if (ref != 0u) hist_begin(li);
    bool ok = id.x < ds && id.y < ds;
    if (ok) {
        // RGB: the cubic (Catmull-Rom) value half-way between the 2 x 2 source texels, from their 4 x 4 neighbourhood,
        // clamped to the 2 x 2's range: crisper than a box (distant surfaces keep their detail), no ringing. Alpha: the box
        // average (the coverage scaling's basis). Transparent texels' colours were filled from their neighbours at level 0.
        static const float cw[4] = { -0.0625, 0.5625, 0.5625, -0.0625 };
        float3 acc = float3(0.0, 0.0, 0.0), lo = float3(255.0, 255.0, 255.0), hi = float3(0.0, 0.0, 0.0);
        float a = 0.0;
        int m = (int)ss - 1;
        [unroll] for (int j = 0; j < 4; j++) {
            int y = (int)id.y * 2 - 1 + j;
            y = (e_a.w & 2u) || ss == 1u ? clamp(y, 0, m) : (y & m);
            [unroll] for (int i = 0; i < 4; i++) {
                int x = (int)id.x * 2 - 1 + i;
                x = (e_a.w & 1u) || ss == 1u ? clamp(x, 0, m) : (x & m);
                float4 t = et_unpack(e_lv.Load((e_b.z + (uint)y * ss + (uint)x) * 4u));
                acc += t.rgb * (cw[i] * cw[j]);
                if (i == 1 || i == 2) if (j == 1 || j == 2) { lo = min(lo, t.rgb); hi = max(hi, t.rgb); a += t.w; }
            }
        }
        uint wd = et_pack(float4(clamp(acc, lo, hi), a * 0.25));
        e_lv.Store((e_b.w + id.y * ds + id.x) * 4u, wd);
        if (ref != 0u) InterlockedAdd(gs_hist[wd >> 24], 1u);
        else e_tex[uint3(id.xy, e_b.y)] = et_unpack(wd) * (1.0 / 255.0);
    }
    if (ref != 0u) hist_end(li, e_c.z);
}

// ---- coverage: for each level l of e_c.z, the alpha t whose share of texels >= t equals level 0's share >= ref; the
// level's alpha is then scaled by ref / t (alpha-tested images keep their thickness in the distance) ------------------------
[numthreads(1, 1, 1)]
void cs_et_cov(uint3 id : SV_DispatchThreadID) {
    uint ref = e_c.w, nl = e_c.z;
    uint n0 = 0u, c0 = 0u;
    for (uint b = 0u; b < 256u; b++) {
        uint h = e_hist.Load(b * 4u);
        n0 += h;
        if (b >= ref) c0 += h;
    }
    float cov = n0 ? (float)c0 / (float)n0 : 0.0;
    for (uint l = 1u; l < nl; l++) {
        uint nl_ = 0u;
        for (uint b2 = 0u; b2 < 256u; b2++) nl_ += e_hist.Load((l * 256u + b2) * 4u);
        float s = 1.0;
        if (nl_ != 0u && cov > 0.0 && cov < 1.0) {
            uint cum = 0u, t = 255u;
            for (int b3 = 255; b3 >= 1; b3--) {
                cum += e_hist.Load((l * 256u + (uint)b3) * 4u);
                t = (uint)b3;
                if ((float)cum >= cov * (float)nl_) break;
            }
            s = clamp((float)ref / (float)t, 0.5, 8.0);
        }
        e_hist.Store((HIST_SCALES + l) * 4u, asuint(s));
    }
}

[numthreads(8, 8, 1)]
void cs_et_store(uint3 id : SV_DispatchThreadID) {
    uint ds = e_c.y;
    if (id.x >= ds || id.y >= ds) return;
    float4 t = et_unpack(e_lv.Load((e_b.z + id.y * ds + id.x) * 4u));
    float s = asfloat(e_hist.Load((HIST_SCALES + e_c.z) * 4u));
    if (t.w <= 128.0) t.w = min(floor(t.w * s + 0.5), 128.0);
    e_tex[uint3(id.xy, e_b.y)] = t * (1.0 / 255.0);
}

// ---- the content hash of a decoded image: 64 threads hash consecutive runs of words (two 32-bit lanes, murmur3 and
// xxhash style), thread 0 folds them in order. The image's size goes into the seed. Result: 4 words at request e_b.y ------
groupshared uint2 gs_part[64];
uint rotl(uint x, uint r) { return (x << r) | (x >> (32u - r)); }
uint fmix(uint h) { h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16; return h; }
[numthreads(64, 1, 1)]
void cs_et_hash(uint3 id : SV_DispatchThreadID, uint li : SV_GroupIndex) {
    uint n = 1u << (e_a.y + e_a.z);
    uint a = n * li / 64u, z = n * (li + 1u) / 64u;
    uint h1 = 0x9747B28Cu ^ li, h2 = 0x165667B1u + li;
    for (uint i = a; i < z; i++) {
        uint w = e_src.Load((e_a.x + i) * 4u);
        uint k = w * 0xCC9E2D51u;
        k = rotl(k, 15u) * 0x1B873593u;
        h1 = rotl(h1 ^ k, 13u) * 5u + 0xE6546B64u;
        h2 = rotl(h2 + w * 0x85EBCA77u, 13u) * 0x9E3779B1u;
    }
    gs_part[li] = uint2(h1, h2);
    GroupMemoryBarrierWithGroupSync();
    if (li == 0u) {
        uint H1 = 0x2545F491u ^ (e_a.y << 8) ^ e_a.z, H2 = 0x27D4EB2Fu + (e_a.y << 4) + e_a.z;
        for (uint t = 0u; t < 64u; t++) {
            H1 = rotl(H1 ^ fmix(gs_part[t].x + t), 13u) * 5u + 0xE6546B64u;
            H2 = rotl(H2 + gs_part[t].y * 0xC2B2AE3Du, 17u) * 0x27D4EB2Fu;
        }
        e_out.Store4(e_b.y * 16u, uint4(fmix(H1 ^ n), fmix(H2 + n), e_a.y | (e_a.z << 8), e_a.x));
    }
}
