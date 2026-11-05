// (light) Enhanced lighting of modern frames (runtime/gpu/gpu_light.h, notes/enhanced-lighting.md): screen-space ambient
// occlusion from the frame's depth and bloom of the picture's bright parts, applied once per game frame at the resolve
// point (before the HUD). The picture holds the GS bytes (RGB 0..255 as UNORM, alpha = the GS alpha byte, which the HUD's
// DATE reads): every pass here writes RGB only.
//
// Depth: the modern frame's surfaces hold the GS Z / 2^20 (R2_MFT). Sly's projection makes the GS Z affine in 1 / w, so
// the view depth is w = 1 / (l_z.x * d + l_z.y) (gpu_light.h light_proj derives both from the frame's screen model and an
// entry's world x clip matrix); view x / y = (l_view.xz * picture pixel + l_view.yw) * w. Units: the game's world units.
#ifndef MS
#define MS 0
#endif

cbuffer light_cb : register(b7) {
    float4 l_dst;                        // destination size w, h, 1 / w, 1 / h
    float4 l_src;                        // sampled source size w, h, 1 / w, 1 / h
    float4 l_map;                        // depth surface pixels per picture pixel x, y; half-res: picture pixels per texel x, y
    float4 l_view;                       // view x = (x * p + y) * w, view y = (z * p + w) * w (p: picture pixel centre)
    float4 l_z;                          // 1 / w = x * d + y; sky w (no depth); debug view (1 occlusion, 2 bloom)
    float4 l_ao;                         // radius (world units), strength, largest radius (texels), bias (x radius)
    float4 l_ao2;                        // fade start, fade end (w), darkest factor, power
    float4 l_bloom;                      // threshold, knee, intensity, saturation of the bloom colour
    float4 l_dir;                        // blur: direction (texels); picture size inverse (z, w)
};

SamplerState l_lin : register(s0);
Texture2D<float> l_w : register(t14);    // half-res view depth (w)
Texture2D<float> l_aot : register(t15);  // ambient occlusion (half res)
Texture2D<float4> l_tex : register(t16); // colour source (picture / bloom levels)
#if MS
Texture2DMS<float> l_zs : register(t17); // the frame's depth surface (sample 0)
float l_zload(int2 q) { return l_zs.Load(q, 0); }
#else
Texture2D<float> l_zs : register(t17);
float l_zload(int2 q) { return l_zs.Load(int3(q, 0)); }
#endif

float4 vs_light(uint vid : SV_VertexID) : SV_Position {   // one triangle over the target
    float2 t = float2((vid << 1) & 2u, vid & 2u);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float l_wof(float d) {                   // view depth of a depth value (no depth / behind: the sky's)
    float iw = l_z.x * d + l_z.y;
    return iw > 1e-7 ? min(1.0 / iw, l_z.z) : l_z.z;
}
// view-space position of picture position p at depth w
float3 l_pos(float2 p, float w) { return float3((l_view.x * p.x + l_view.y) * w, (l_view.z * p.y + l_view.w) * w, w); }
float l_depth_at(float2 p) {             // the depth surface's sample 0 under picture position p
    int2 q = int2(p * l_map.xy);
    return l_zload(q);
}

// ---- half-res view depth: the nearest of the 2 x 2 picture pixels' depths (thin foreground keeps its occlusion) ---------
float ps_light_lin(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy * l_map.zw;        // the texel's picture-space centre (a corner of four picture pixels)
    float w0 = l_wof(l_depth_at(p + float2(-0.5, -0.5))), w1 = l_wof(l_depth_at(p + float2(0.5, -0.5)));
    float w2 = l_wof(l_depth_at(p + float2(-0.5, 0.5))), w3 = l_wof(l_depth_at(p + float2(0.5, 0.5)));
    return min(min(w0, w1), min(w2, w3));
}

float l_wload(int2 q) { return l_w.Load(int3(clamp(q, int2(0, 0), int2(l_dst.xy) - 1), 0)); }

// ---- ambient occlusion (half res): a disc of samples around the pixel, the occlusion of each as in "Alchemy" AO (the
// share of the sample's offset along the normal over its length, fading out at the radius); normals from the depth ------
float ps_light_ao(float4 pos : SV_Position) : SV_Target {
    int2 q = int2(pos.xy);
    float w = l_wload(q);
    if (w >= l_z.z * 0.5 || w >= l_ao2.y) return 1.0;
    float2 pp = pos.xy * l_map.zw;       // picture position of the texel centre
    float3 P = l_pos(pp, w);
    // the normal from the neighbour on the flatter side in x and in y (no smearing across silhouettes)
    float wl = l_wload(q - int2(1, 0)), wr = l_wload(q + int2(1, 0)), wu = l_wload(q - int2(0, 1)), wd = l_wload(q + int2(0, 1));
    float3 Pl = l_pos(pp - float2(l_map.z, 0.0), wl), Pr = l_pos(pp + float2(l_map.z, 0.0), wr);
    float3 Pu = l_pos(pp - float2(0.0, l_map.w), wu), Pd = l_pos(pp + float2(0.0, l_map.w), wd);
    float3 dx = abs(wr - w) < abs(w - wl) ? Pr - P : P - Pl;
    float3 dy = abs(wd - w) < abs(w - wu) ? Pd - P : P - Pu;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0) N = -N;         // (towards the camera)
    float R = l_ao.x;
    float rpx = R / (w * abs(l_view.x) * l_map.z);   // the radius in texels at this depth
    if (rpx < 1.0) return 1.0;
    rpx = min(rpx, l_ao.z);
    float Rw = rpx * w * abs(l_view.x) * l_map.z;    // (clamped) radius in world units
    float noise = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));   // interleaved gradient noise
    float occ = 0.0;
    const uint NS = 12u;
    [unroll] for (uint i = 0u; i < NS; i++) {
        float a = ((float)i + noise) * 2.39996323;
        float r = rpx * sqrt(((float)i + 0.5 + 0.5 * noise) / (float)NS);
        float2 o = r * float2(cos(a), sin(a));
        float2 sp = pos.xy + o;
        float ws = l_wload(int2(sp));
        float3 S = l_pos(sp * l_map.zw, ws);
        float3 v = S - P;
        float vv = dot(v, v), vn = dot(v, N);
        float fall = saturate(1.0 - vv / (Rw * Rw));
        occ += saturate((vn - l_ao.w * Rw) * Rw / (vv + 0.01 * Rw * Rw)) * fall;
    }
    occ /= (float)NS;
    float ao = saturate(1.0 - l_ao.y * occ);
    ao = pow(ao, l_ao2.w);
    float fade = saturate((w - l_ao2.x) / max(l_ao2.y - l_ao2.x, 1e-3));   // (distant: none - fog covers it anyway)
    return lerp(max(ao, l_ao2.z), 1.0, fade);
}

// ---- depth-aware blur of the occlusion (separable, l_dir.xy) --------------------------------------------------------
float ps_light_blur(float4 pos : SV_Position) : SV_Target {
    int2 q = int2(pos.xy);
    float w = l_wload(q);
    float s = 0.0, ws = 0.0;
    [unroll] for (int i = -4; i <= 4; i++) {
        int2 qi = q + int2(l_dir.xy * (float)i);
        float wi = l_wload(qi);
        float g = exp(-(float)(i * i) * (1.0 / 12.0)) * exp(-abs(wi - w) / max(w, 1e-3) * 40.0);
        s += l_aot.Load(int3(clamp(qi, int2(0, 0), int2(l_dst.xy) - 1), 0)) * g;
        ws += g;
    }
    return s / ws;
}

// ---- bloom ----------------------------------------------------------------------------------------------------------
float l_luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 l_bright(float3 c) {              // the part above the threshold, with a soft knee
    float l = max(max(c.r, c.g), c.b);
    float k = l_bloom.y, t = l_bloom.x;
    float s = clamp(l - t + k, 0.0, 2.0 * k);
    s = s * s / (4.0 * k + 1e-5);
    float f = max(s, l - t) / max(l, 1e-4);
    float3 b = c * f;
    return lerp(l_luma(b).xxx, b, l_bloom.w);
}
// the picture's bright parts at half resolution: four 2 x 2 box taps, each weighted down by its brightness (single bright
// pixels do not flicker in and out)
float4 ps_light_bright(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy * l_map.zw;        // picture position
    float3 s = 0.0;
    float ws = 0.0;
    [unroll] for (int j = 0; j < 4; j++) {
        float2 o = float2((j & 1) ? 1.0 : -1.0, (j & 2) ? 1.0 : -1.0);
        float3 c = l_bright(l_tex.SampleLevel(l_lin, (p + o) * l_dir.zw, 0.0).rgb);
        float wt = 1.0 / (1.0 + l_luma(c));
        s += c * wt;
        ws += wt;
    }
    return float4(s / ws, 1.0);
}
// dual-filter downsample (a 2x smaller level)
float4 ps_light_down(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * l_dst.zw, h = 0.5 * l_src.zw;
    float3 s = l_tex.SampleLevel(l_lin, uv, 0.0).rgb * 4.0;
    s += l_tex.SampleLevel(l_lin, uv + float2(-h.x, -h.y), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(h.x, -h.y), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(-h.x, h.y), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(h.x, h.y), 0.0).rgb;
    return float4(s * 0.125, 1.0);
}
// dual-filter upsample of a 2x smaller level, added to this level (additive blend)
float4 ps_light_up(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * l_dst.zw, h = l_src.zw;
    float3 s = l_tex.SampleLevel(l_lin, uv + float2(-2.0 * h.x, 0.0), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(2.0 * h.x, 0.0), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(0.0, -2.0 * h.y), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(0.0, 2.0 * h.y), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(-h.x, h.y), 0.0).rgb * 2.0;
    s += l_tex.SampleLevel(l_lin, uv + float2(h.x, h.y), 0.0).rgb * 2.0;
    s += l_tex.SampleLevel(l_lin, uv + float2(h.x, -h.y), 0.0).rgb * 2.0;
    s += l_tex.SampleLevel(l_lin, uv + float2(-h.x, -h.y), 0.0).rgb * 2.0;
    return float4(s * (1.0 / 12.0), 1.0);
}

// ---- composite onto the picture (dual-source blend: picture x occlusion + bloom, RGB only) ---------------------------
struct LOut { float4 add : SV_Target0; float4 mul : SV_Target1; };
LOut ps_light_comp(float4 pos : SV_Position) {
    LOut o;
    float2 p = pos.xy;                   // picture pixel centre
    float ao = 1.0;
    if (l_ao.y > 0.0) {
        float w = l_wof(l_depth_at(p));
        if (w < l_z.z * 0.5) {
            // depth-aware upsample: the four nearest half-res texels, weighted by bilinear weight x depth similarity
            float2 t = p / l_map.zw - 0.5, f0 = floor(t), f = t - f0;
            float s = 0.0, ws = 0.0;
            [unroll] for (int j = 0; j < 4; j++) {
                int2 qj = int2(f0) + int2(j & 1, j >> 1);
                qj = clamp(qj, int2(0, 0), int2(l_src.xy) - 1);
                float bw = ((j & 1) ? f.x : 1.0 - f.x) * ((j & 2) ? f.y : 1.0 - f.y);
                float wj = l_w.Load(int3(qj, 0));
                float g = (bw + 1e-3) / (1e-3 + abs(wj - w) / w * 20.0 + 0.02);
                s += l_aot.Load(int3(qj, 0)) * g;
                ws += g;
            }
            ao = s / ws;
        }
    }
    float3 b = l_bloom.z > 0.0 ? l_tex.SampleLevel(l_lin, p * l_dir.zw, 0.0).rgb * l_bloom.z : 0.0;
    o.add = float4(b, 0.0);
    o.mul = float4(ao, ao, ao, 1.0);
    if (l_z.w == 1.0) { o.add = float4(ao.xxx * 0.8, 0.0); o.mul = 0.0; }        // (PS2_LIGHT_DBG=1: the occlusion)
    else if (l_z.w == 2.0) { o.add = float4(b, 0.0); o.mul = 0.0; }               // (2: the bloom)
    else if (l_z.w == 3.0) {                                                        // (3: view depth, bands of 500 units)
        float w = l_wof(l_depth_at(p));
        o.add = float4(saturate(w / 8000.0), frac(w / 500.0), w >= l_z.z * 0.5 ? 1.0 : 0.0, 0.0); o.mul = 0.0;
    }
    return o;
}
