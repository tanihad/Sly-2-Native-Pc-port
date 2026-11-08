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
float3 l_bright(float3 c) {              // the part above the threshold (soft knee), squared: near-white lights glow, merely
    float l = max(max(c.r, c.g), c.b);   // bright surfaces (a lit floor, snow) hardly - an LDR picture has no HDR to tell them apart
    float k = l_bloom.y, t = l_bloom.x;
    float s = clamp(l - t + k, 0.0, 2.0 * k);
    s = s * s / (4.0 * k + 1e-5);
    float e = max(s, l - t);
    float f = e * e / (max(1.0 - t, 1e-3) * max(l, 1e-4));
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

#ifndef REIM
#define REIM 0
#endif
#if REIM
// ======================================================================================================================
// (reimagine) Look = Reimagined (runtime/gpu/gpu_reim.h, notes/reimagined-lighting.md): the post pass reimagined - screen-
// space shadows along the key light, a wider and stronger bloom with halos, light shafts from bright distant sources
// (the moon, the sky), depth + height fog in the scene's own sky colour, and a per-scene-mood colour grade (night: deep
// blues, warm lamps; day: warm, saturated), filmic contrast and a vignette. Composited from a copy of the picture (no
// blend): RGB written, the GS alpha byte kept. Only the REIM permutations contain this code (Faithful's are unchanged).
cbuffer reim_cb : register(b8) {
    float4 r_w0, r_w1, r_w2, r_w3;       // picture pixel + view depth -> world: the inverse world x clip matrix's rows
    float4 r_ndc;                        // ndc x = x * p.x + y, ndc y = z * p.y + w (picture pixel centres)
    float4 r_zab;                        // clip z = x * w + y; camera world height (z); -
    float4 r_key;                        // the key light's direction in view space (towards it), shadow strength
    float4 r_sss;                        // screen-space shadow: length (world units), thickness, bias, fade-out depth
    float4 r_grade;                      // saturation, contrast, vibrance, exposure
    float4 r_fog;                        // depth fog start, density, largest share, height fog density
    float4 r_hfog;                       // height fog: scale height, offset below the camera, -, -
    float4 r_vig;                        // vignette strength, inner radius, outer radius; clarity (local contrast power)
    float4 r_shaft;                      // shafts: intensity, decay, length (share of the way to the source), far depth
    float4 r_bloom;                      // bloom intensity, halo weight (up passes), -, -
    float4 r_misc;                       // stats blend (0..1), night override (<0: auto), debug view, frame
    float4 r_keyc;                       // key light colour (shadow tint), -
    float4 r_lm0, r_lm1, r_lm2, r_lm3;   // world -> the key light's shadow map clip (rows; gpu_reim.h reim_smap)
    float4 r_smp;                        // shadow map: texel (uv), texel (world units), fade start / end (view depth)
    float4 r_smp2;                       // fade end, strength (0: no map), the key light's world direction (x, y)
    float4 r_smp3;                       // its world direction z, the stable receiver bias (1: on), -, -
    float4 r_pv0, r_pv1, r_pv2, r_pv3;   // (fix) the previous frame's world x clip (rows): the key-light term's history
    float4 r_pndc;                       // the previous frame's ndc of a picture pixel (as r_ndc)
    float4 r_ptaa;                       // history weight (0: none), -, -, -
    float4 r_ksrc;                       // (2) the key light's picture position (uv, may be off the picture), its weight (0: not
                                         // in front: no shafts; < 0: 0.1.1214's bright-texel centroid), warm glow light (fires)
    float4 r_lc0, r_lc1, r_lc2, r_lc3;   // (2) world -> the static shadow cache's clip (rows; gpu_reim.h reim_smap)
    float4 r_scp;                        // (2) the cache: texel (uv), texel (world units), strength (0: none), -
};
Texture2D<float> r_smc : register(t27);      // (2) the static shadow cache (and ps_reim_sccopy's source)
Texture2D<float> r_smap : register(t24);     // the key light's shadow map (1 at the light, 0 far)
Texture2D<float4> r_stats : register(t18);   // 4 x 1: [0] average colour, mood; [1] fog colour; [2] shaft source; [3] -
Texture2D<float4> r_pic : register(t19);     // the picture (a copy)
Texture2D<float2> r_sh : register(t20);      // the key light (half res): visibility (shadows), N.L
Texture2D<float4> r_shafts : register(t21);  // light shafts (quarter res)
Texture2D<float4> r_scene : register(t22);   // half-res picture colour with its mips (the scene's averages)

// view position of a picture position (as l_pos) -> picture position
float2 r_pix(float3 S) { return float2((S.x / S.z - l_view.y) / l_view.x, (S.y / S.z - l_view.w) / l_view.z); }
// world position of picture position p at view depth w
float3 r_world(float2 p, float w) {
    float4 c = float4((r_ndc.x * p.x + r_ndc.y) * w, (r_ndc.z * p.y + r_ndc.w) * w, r_zab.x * w + r_zab.y, w);
    float4 h = c.x * r_w0 + c.y * r_w1 + c.z * r_w2 + c.w * r_w3;
    return h.xyz / h.w;
}
float r_noise(float2 q) { return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715)))); }

// ---- screen-space shadows (half res): a short march from the surface towards the key light through the depth --------
float2 ps_reim_shadow(float4 pos : SV_Position) : SV_Target {
    if (r_key.w <= 0.0) return float2(1.0, 0.0);
    int2 q = int2(pos.xy);
    float w = l_wload(q);
    if (w >= l_z.z * 0.5 || w >= max(r_sss.w, r_smp2.x)) return float2(1.0, 0.0);
    float2 pp = pos.xy * l_map.zw;
    float3 P = l_pos(pp, w);
    float3 L = r_key.xyz;
    // the surface normal from the depth (the flatter neighbour, as the occlusion): surfaces turned away keep their light
    float wl = l_wload(q - int2(1, 0)), wr = l_wload(q + int2(1, 0)), wu = l_wload(q - int2(0, 1)), wd = l_wload(q + int2(0, 1));
    float3 Pl = l_pos(pp - float2(l_map.z, 0.0), wl), Pr = l_pos(pp + float2(l_map.z, 0.0), wr);
    float3 Pu = l_pos(pp - float2(0.0, l_map.w), wu), Pd = l_pos(pp + float2(0.0, l_map.w), wd);
    float3 dx = abs(wr - w) < abs(w - wl) ? Pr - P : P - Pl;
    float3 dy = abs(wd - w) < abs(w - wu) ? Pd - P : P - Pu;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0) N = -N;
    if (r_smp3.y > 0.0) {                // (fix) a wider baseline (2 texels each way): N.L no longer jumps with the depth's texels
        float wl2 = l_wload(q - int2(2, 0)), wr2 = l_wload(q + int2(2, 0)), wu2 = l_wload(q - int2(0, 2)), wd2 = l_wload(q + int2(0, 2));
        float3 Pl2 = l_pos(pp - float2(2.0 * l_map.z, 0.0), wl2), Pr2 = l_pos(pp + float2(2.0 * l_map.z, 0.0), wr2);
        float3 Pu2 = l_pos(pp - float2(0.0, 2.0 * l_map.w), wu2), Pd2 = l_pos(pp + float2(0.0, 2.0 * l_map.w), wd2);
        float3 dx2 = abs(wr2 - w) < abs(w - wl2) ? Pr2 - P : P - Pl2;
        float3 dy2 = abs(wd2 - w) < abs(w - wu2) ? Pd2 - P : P - Pu2;
        float3 N2 = cross(dx2, dy2);
        if (dot(N2, N2) > 1e-12) { N2 = normalize(N2); if (dot(N2, P) > 0.0) N2 = -N2; if (dot(N2, N) > 0.8) N = normalize(N + N2); }
    }
    float nl = dot(N, L);
    if (nl <= 0.02) return float2(1.0, 0.0);
    float len = r_sss.x * (1.0 + w / 3000.0);       // (a little longer far away: the same share of the screen)
    float noise = r_noise(pos.xy);
    float3 Po = P + N * (r_sss.z * (1.0 + w * 0.002));   // (off the surface: no self-shadowing)
    float occ = 0.0;
    const uint NS = 10u;
    [unroll] for (uint i = 0u; i < NS; i++) {
        if (w >= r_sss.w || r_sss.x <= 0.0) break;   // (beyond the contact shadows' reach / none: the map only)
        float t = ((float)i + noise) / (float)NS;
        t = t * t;                       // (denser near the surface: contact shadows stay crisp)
        float3 S = Po + L * (len * t + 2.0);
        if (S.z <= 1.0) break;
        float2 sp = r_pix(S) / l_map.zw;  // half-res texel
        if (any(sp < 0.0) || any(sp >= l_dst.xy)) break;
        float ws = l_wload(int2(sp));
        float dz = S.z - ws;             // > 0: something is in front of the ray point
        float th = r_sss.y * (1.0 + S.z / 2000.0);
        if (dz > 0.5 + S.z * 0.0015 && dz < th) occ = max(occ, 1.0 - t * 0.6);
    }
    float fade = saturate((r_sss.w - w) / (r_sss.w * 0.3));
    occ *= fade;
    // the key light's shadow map (real cast shadows of every solid R2 mesh): 4 x 4 PCF from four gathers, the receiver
    // pushed towards the light by a slope-scaled offset
    if (r_smp2.y > 0.0) {
        float3 Lw = float3(r_smp2.zw, r_smp3.x);
        float slope = sqrt(saturate(1.0 - nl * nl)) / max(nl, 0.2);
        float3 Pw = r_world(pp, w) + Lw * (r_smp.z * (1.5 + 2.0 * slope) + w * 0.002);
        if (r_smp3.y > 0.0) {
            // (fix) the receiver pushed out along its own world normal (normal-offset bias, ~2 map texels growing with the
            // distance): the depth-rebuilt positions jitter by a sub-texel as the camera moves, which flipped surfaces
            // turned partly away from the light between lit and shadowed every few frames (acne under motion)
            float3 W0 = r_world(pp, w);
            float3 Wx = r_world(pp + (abs(wr - w) < abs(w - wl) ? 1.0 : -1.0) * float2(l_map.z, 0.0), abs(wr - w) < abs(w - wl) ? wr : wl) - W0;
            float3 Wy = r_world(pp + (abs(wd - w) < abs(w - wu) ? 1.0 : -1.0) * float2(0.0, l_map.w), abs(wd - w) < abs(w - wu) ? wd : wu) - W0;
            float3 Nw = cross(Wx, Wy);
            float nn = dot(Nw, Nw);
            Nw = nn > 1e-12 ? Nw * rsqrt(nn) : Lw;
            if (dot(Nw, Lw) < 0.0) Nw = -Nw;
            Pw = W0 + Nw * (r_smp.z * (2.0 + w * 0.0008)) + Lw * (r_smp.z * (1.0 + 1.5 * slope));
        }
        float4 lc = Pw.x * r_lm0 + Pw.y * r_lm1 + Pw.z * r_lm2 + r_lm3;
        float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);
        if (all(uv > r_smp.xy) && all(uv < 1.0 - r_smp.xy) && lc.z > 0.0 && lc.z < 1.0) {
            float lit = 0.0;
            [unroll] for (int j = 0; j < 4; j++) {
                float2 o = float2((j & 1) ? 1.0 : -1.0, (j & 2) ? 1.0 : -1.0) * r_smp.xy;
                float4 g = r_smap.GatherRed(l_lin, uv + o);
                lit += dot(step(g, lc.z.xxxx + 1e-5), 0.25.xxxx);
            }
            lit *= 0.25;
            float2 e = min(uv, 1.0 - uv);
            float sf = saturate(min(e.x, e.y) * 12.0) * saturate((r_smp2.x - w) / max(r_smp2.x - r_smp.w, 1.0));
            if (r_smp3.y > 0.0) sf *= saturate(nl * 6.0);   // (fix: surfaces almost edge-on to the light fade out: no flip-flop)
            occ = max(occ, (1.0 - lit) * sf);
        }
        if (r_scp.z > 0.0) {             // (2) the static world's casters, kept from frame to frame (the static shadow cache)
            float4 lc2 = Pw.x * r_lc0 + Pw.y * r_lc1 + Pw.z * r_lc2 + r_lc3;
            float2 uv2 = float2(lc2.x * 0.5 + 0.5, 0.5 - lc2.y * 0.5);
            if (all(uv2 > r_scp.xx) && all(uv2 < 1.0 - r_scp.xx) && lc2.z > 0.0 && lc2.z < 1.0) {
                float lit2 = 0.0;
                [unroll] for (int j2 = 0; j2 < 4; j2++) {
                    float2 o2 = float2((j2 & 1) ? 1.0 : -1.0, (j2 & 2) ? 1.0 : -1.0) * r_scp.xx;
                    float4 g2 = r_smc.GatherRed(l_lin, uv2 + o2);
                    lit2 += dot(step(g2, lc2.z.xxxx + 1e-5), 0.25.xxxx);
                }
                float2 e2 = min(uv2, 1.0 - uv2);
                float sf2 = saturate(min(e2.x, e2.y) * 12.0) * saturate((r_smp2.x - w) / max(r_smp2.x - r_smp.w, 1.0)) * saturate(nl * 6.0);
                occ = max(occ, (1.0 - lit2 * 0.25) * sf2);
            }
        }
    }
    return float2(1.0 - saturate(occ), nl);
}

// (2) the static shadow cache moved by whole texels (gpu_reim.h sc_move): the old map's depth at the offset texel
float ps_reim_sccopy(float4 pos : SV_Position) : SV_Depth {
    int2 q = int2(pos.xy) + int2(l_dir.xy);
    if (any(q < 0) || any(q >= int2(l_src.xy))) return 0.0;
    return r_smc.Load(int3(q, 0));
}

// the key light's (visibility, N.L) blurred depth-aware (separable, l_dir.xy), as the occlusion
Texture2D<float2> r_bsrc : register(t25);
float2 ps_reim_blur2(float4 pos : SV_Position) : SV_Target {
    int2 q = int2(pos.xy);
    float w = l_wload(q);
    float2 s = 0.0;
    float ws = 0.0;
    [unroll] for (int i = -4; i <= 4; i++) {
        int2 qi = q + int2(l_dir.xy * (float)i);
        float wi = l_wload(qi);
        float g = exp(-(float)(i * i) * (1.0 / 12.0)) * exp(-abs(wi - w) / max(w, 1e-3) * 40.0);
        s += r_bsrc.Load(int3(clamp(qi, int2(0, 0), int2(l_dst.xy) - 1), 0)) * g;
        ws += g;
    }
    return s / ws;
}

// (fix) the key-light term's temporal resolve (half res): this frame's (visibility, N.L) blended with the last frame's at
// the same world position (reprojected through the previous world x clip), the history clamped to this frame's 3 x 3
// neighbourhood (no ghosts): shadow edges that crawled or blinked between frames under camera motion settle
Texture2D<float2> r_shist : register(t26);
float2 ps_reim_shtaa(float4 pos : SV_Position) : SV_Target {
    int2 q = int2(pos.xy);
    float2 cur = r_bsrc.Load(int3(q, 0));
    float w = l_wload(q);
    if (r_ptaa.x <= 0.0 || w >= l_z.z * 0.5) return cur;
    float2 mn = cur, mx = cur;
    [unroll] for (int j = -1; j <= 1; j++)
        [unroll] for (int i = -1; i <= 1; i++) {
            float2 v = r_bsrc.Load(int3(clamp(q + int2(i, j), int2(0, 0), int2(l_dst.xy) - 1), 0));
            mn = min(mn, v); mx = max(mx, v);
        }
    float3 P = r_world(pos.xy * l_map.zw, w);
    float4 c = P.x * r_pv0 + P.y * r_pv1 + P.z * r_pv2 + r_pv3;
    if (c.w <= 1e-3) return cur;
    float2 nd = c.xy / c.w;
    float2 pp = float2((nd.x - r_pndc.y) / r_pndc.x, (nd.y - r_pndc.w) / r_pndc.z);   // previous picture pixel
    float2 uv = pp / l_map.zw * l_dst.zw;
    if (any(uv < 0.0) || any(uv > 1.0)) return cur;
    float2 h = clamp(r_shist.SampleLevel(l_lin, uv, 0.0), mn, mx);
    return lerp(cur, h, r_ptaa.x);
}

// ---- bloom: a softer, lower threshold and the half-res scene colour for the averages (MRT) --------------------------
struct RBOut { float4 bright : SV_Target0; float4 scene : SV_Target1; };
RBOut ps_reim_bright(float4 pos : SV_Position) {
    float2 p = pos.xy * l_map.zw;
    float3 s = 0.0, sc = 0.0;
    float ws = 0.0;
    // a bright scene raises the threshold (its own brightness, last frame's statistics): only real lights glow
    float rb_k = 1.0 / (1.0 + 0.9 * saturate(r_stats.Load(int3(0, 0, 0)).w - 0.12));
    [unroll] for (int j = 0; j < 4; j++) {
        float2 o = float2((j & 1) ? 1.0 : -1.0, (j & 2) ? 1.0 : -1.0);
        float3 c0 = l_tex.SampleLevel(l_lin, (p + o) * l_dir.zw, 0.0).rgb;
        float3 c = l_bright(c0 * rb_k) * (1.0 / rb_k);
        float wt = 1.0 / (1.0 + l_luma(c));
        s += c * wt;
        ws += wt;
        sc += c0;
    }
    RBOut o;
    o.bright = float4(s / ws, 1.0);
    o.scene = float4(sc * 0.25, 1.0);
    return o;
}
// the dual-filter upsample with the wider level weighted (halos around lamps, windows and the moon)
float4 ps_reim_up(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * l_dst.zw, h = l_src.zw;
    float3 s = l_tex.SampleLevel(l_lin, uv + float2(-2.0 * h.x, 0.0), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(2.0 * h.x, 0.0), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(0.0, -2.0 * h.y), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(0.0, 2.0 * h.y), 0.0).rgb;
    s += l_tex.SampleLevel(l_lin, uv + float2(-h.x, h.y), 0.0).rgb * 2.0;
    s += l_tex.SampleLevel(l_lin, uv + float2(h.x, h.y), 0.0).rgb * 2.0;
    s += l_tex.SampleLevel(l_lin, uv + float2(h.x, -h.y), 0.0).rgb * 2.0;
    s += l_tex.SampleLevel(l_lin, uv + float2(-h.x, -h.y), 0.0).rgb * 2.0;
    return float4(s * (r_bloom.y / 12.0), 1.0);
}

// (2) a texel that light shafts may come from: far (the sky, the distant background) and neutral in hue - moonlight, a
// pale sky, the sun's glow. Fires, torches, lamps, lasers and glows are saturated (orange, yellow, red, green): never.
// (Their sprites write no depth, so over the sky they are "far" too: the hue is what tells them apart.)
float r_skyw(float3 b, float w) {
    float mx = max(max(b.r, b.g), b.b), mn = min(min(b.r, b.g), b.b);
    float sat = (mx - mn) / max(mx, 1e-4);
    float warm = saturate((b.r - b.b) / max(mx, 1e-4) * 2.5);   // (any warm cast: firelight)
    return (w >= r_shaft.w ? 1.0 : 0.0) * saturate((0.45 - sat) / 0.2) * (1.0 - warm);
}

// ---- scene statistics (4 x 1, smoothed over frames): the mood, the fog colour, the shafts' source ----------------
Texture2D<float4> r_prev : register(t23);    // the previous statistics
float4 ps_reim_stats(float4 pos : SV_Position) : SV_Target {
    uint k = (uint)pos.x;
    float4 prev = r_prev.Load(int3(k, 0, 0));
    float4 v = 0.0;
    uint lw, lh, nl;
    r_scene.GetDimensions(0, lw, lh, nl);
    if (k == 0u) {                       // the average colour (mip 4: 80 x 45 at 1440p) and its log-average brightness
        float3 s = 0.0;
        float ll = 0.0;
        [loop] for (uint y = 0u; y < 12u; y++)
            [loop] for (uint x = 0u; x < 16u; x++) {
                float3 c = r_scene.SampleLevel(l_lin, (float2(x, y) + 0.5) / float2(16.0, 12.0), 3.0).rgb;
                s += c;
                ll += log(l_luma(c) + 0.02);
            }
        s /= 192.0;
        v = float4(s, exp(ll / 192.0));
    } else if (k == 1u) {                // the fog colour: the top third of the picture, far / sky texels weighted up
        float3 s = 0.0;
        float sw = 1e-4;
        [loop] for (uint y = 0u; y < 8u; y++)
            [loop] for (uint x = 0u; x < 16u; x++) {
                float2 uv = (float2(x, y) + 0.5) / float2(16.0, 24.0);
                float3 c = r_scene.SampleLevel(l_lin, uv, 2.0).rgb;
                float w = l_w.Load(int3(int2(uv * l_src.xy), 0));
                float g = w >= l_z.z * 0.5 ? 1.0 : saturate(w / 6000.0) * 0.6 + 0.05;
                s += c * g;
                sw += g;
            }
        v = float4(s / sw, 1.0);
    } else if (k == 2u && r_ksrc.z >= 0.0) {   // (2) the shafts' source: the key light (moon / sun) itself, and how much
        float sw = 0.0;                  // bright neutral sky lies towards it (fires, lamps, lasers and glows never count)
        [loop] for (uint y = 0u; y < 24u; y++)
            [loop] for (uint x = 0u; x < 40u; x++) {
                float2 uv = (float2(x, y) + 0.5) / float2(40.0, 24.0);
                float3 b = l_tex.SampleLevel(l_lin, uv, 0.0).rgb;   // (bloom level 4)
                float w = l_w.Load(int3(int2(uv * l_src.xy), 0));
                float e = l_luma(b) * r_skyw(b, w);
                float2 dv = (uv - r_ksrc.xy) * float2(l_src.x * l_src.w, 1.0);
                e *= exp(-dot(dv, dv) * 4.0);    // (near the light: a sky glow across the picture is no source)
                sw += e * e;
            }
        v = float4(r_ksrc.xy, sqrt(sw / 960.0) * 3.0 * r_ksrc.z, 1.0);
    } else if (k == 2u) {                // the shafts' source: the centroid of the bright far texels (moon, sky glow)
        float2 s = 0.0;
        float sw = 0.0;
        [loop] for (uint y = 0u; y < 24u; y++)
            [loop] for (uint x = 0u; x < 40u; x++) {
                float2 uv = (float2(x, y) + 0.5) / float2(40.0, 24.0);
                float3 b = l_tex.SampleLevel(l_lin, uv, 0.0).rgb;   // (bloom level 4)
                float w = l_w.Load(int3(int2(uv * l_src.xy), 0));
                float far = w >= r_shaft.w ? 1.0 : 0.0;
                float e = l_luma(b) * far;
                e *= e;
                s += uv * e;
                sw += e;
            }
        v = sw > 1e-6 ? float4(s / sw, sqrt(sw / 960.0), 1.0) : float4(prev.xy, 0.0, 1.0);
    }
    if (k == 3u) {                       // the light the relight / shadows / occlusion leave on average (a 32 x 18 grid):
        float s = 0.0;                   // the picture is brought back to the game's own brightness by its inverse
        [loop] for (uint y = 0u; y < 18u; y++)
            [loop] for (uint x = 0u; x < 32u; x++) {
                float2 uv = (float2(x, y) + 0.5) / float2(32.0, 18.0);
                float ao = l_ao.y > 0.0 ? l_aot.SampleLevel(l_lin, uv, 0.0) : 1.0;
                float2 sh = r_sh.SampleLevel(l_lin, uv, 0.0);
                float lit = smoothstep(0.0, 0.7, sh.y) * sh.x, dark = saturate(sh.y * 4.0) * (1.0 - sh.x);
                s += ao * (1.0 + r_keyc.w * lit * 0.8) * (1.0 - r_key.w * dark * 0.75);
            }
        v = float4(s / 576.0, 0.0, 0.0, 1.0);
    }
    float a = r_misc.x;
    if (prev.w <= 0.0) a = 1.0;          // (the first frame)
    if (k == 2u && (prev.z < 1e-3 || r_ksrc.z >= 0.0)) prev.xy = v.xy;   // (a source appearing: from where it is; (2) the key
    return lerp(prev, v, a);                                               //  light: always exactly where it is)
}

// ---- light shafts (quarter res): bright far texels smeared along the way towards the source -------------------------
float4 ps_reim_shafts(float4 pos : SV_Position) : SV_Target {
    float4 src = r_stats.Load(int3(2, 0, 0));
    if (src.z * r_shaft.x <= 1e-4) return 0.0;
    float2 uv = pos.xy * l_dst.zw;
    float2 d = (src.xy - uv) * (r_shaft.z / 24.0);
    float noise = r_noise(pos.xy);
    float2 t = uv + d * noise;
    float3 s = 0.0;
    float dec = 1.0;
    [unroll] for (uint i = 0u; i < 24u; i++) {
        float3 b = l_tex.SampleLevel(l_lin, t, 0.0).rgb;
        float w = l_w.SampleLevel(l_lin, t, 0.0);
        s += b * (r_ksrc.z >= 0.0 ? r_skyw(b, w) : (w >= r_shaft.w ? 1.0 : 0.0)) * dec;   // (2: sky light only)
        dec *= r_shaft.y;
        t += d;
    }
    return float4(s * (1.0 / 24.0), 1.0);
}

// ---- composite + grade (full res, no blend) ----------------------------------------------------------------------
float3 r_srgb2lin(float3 c) { return c * c; }   // (gamma 2: cheap, its exact inverse below)
float3 r_lin2srgb(float3 c) { return sqrt(max(c, 0.0)); }
float r_curve(float x, float k, float p) {   // filmic contrast around the pivot p (display values), a soft toe and shoulder
    x = saturate(x);
    return x < p ? p * pow(x / p, k) : 1.0 - (1.0 - p) * pow((1.0 - x) / (1.0 - p), k);
}
float4 ps_reim_comp(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    int2 ip = int2(p);
    float4 pic = r_pic.Load(int3(ip, 0));
    float3 c = pic.rgb;
    float d = l_depth_at(p);
    float w = l_wof(d);
    bool sky = w >= l_z.z * 0.5;
    if (r_ptaa.y > 0.0) {                // (2) contrast-adaptive sharpening (AMD CAS's cross form): the picture's own detail
        int2 mxp = int2(l_dst.xy) - 1;   // crisper - Sly, textures, edges - without halos (weaker where contrast is already high)
        float3 cb = r_pic.Load(int3(clamp(ip - int2(0, 1), 0, mxp), 0)).rgb, cd = r_pic.Load(int3(clamp(ip - int2(1, 0), 0, mxp), 0)).rgb;
        float3 cf = r_pic.Load(int3(clamp(ip + int2(1, 0), 0, mxp), 0)).rgb, ch = r_pic.Load(int3(clamp(ip + int2(0, 1), 0, mxp), 0)).rgb;
        float3 mn = min(c, min(min(cb, cd), min(cf, ch))), mx = max(c, max(max(cb, cd), max(cf, ch)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        float3 wk = amp * -lerp(0.125, 0.2, saturate(r_ptaa.y));
        c = saturate((c + (cb + cd + cf + ch) * wk) / (1.0 + 4.0 * wk));
    }
    // occlusion + screen-space shadow, depth-aware upsampled together
    float ao = 1.0;
    float2 sh = float2(1.0, 0.0);
    if (!sky) {
        float2 t = p / l_map.zw - 0.5, f0 = floor(t), f = t - f0;
        float s = 0.0, ws = 0.0;
        float2 s2 = 0.0;
        [unroll] for (int j = 0; j < 4; j++) {
            int2 qj = clamp(int2(f0) + int2(j & 1, j >> 1), int2(0, 0), int2(l_src.xy) - 1);
            float bw = ((j & 1) ? f.x : 1.0 - f.x) * ((j & 2) ? f.y : 1.0 - f.y);
            float wj = l_w.Load(int3(qj, 0));
            float g = (bw + 1e-3) / (1e-3 + abs(wj - w) / w * 20.0 + 0.02);
            s += (l_ao.y > 0.0 ? l_aot.Load(int3(qj, 0)) : 1.0) * g;
            s2 += r_sh.Load(int3(qj, 0)).xy * g;
            ws += g;
        }
        ao = s / ws;
        sh = s2 / ws;
    }
    float4 st0 = r_stats.Load(int3(0, 0, 0)), fogc = r_stats.Load(int3(1, 0, 0));
    // the mood: night (a dark picture) .. day
    float night = r_misc.y >= 0.0 ? r_misc.y : saturate((0.30 - st0.w) / 0.16);
    // shadows: in the key light's opposite hue a little (cool under a warm lamp), occlusion neutral
    float3 shc = lerp(float3(1.0, 1.0, 1.0), float3(0.95, 0.98, 1.02), 0.5 + 0.5 * night);
    c *= ao;
    // the key light relit: surfaces facing it brighten in its hue where it reaches them, darken (cool) where something casts
    // a shadow on them; surfaces turned away keep the game's own lighting
    {
        float nl = sh.y, vis = sh.x;
        float3 kc = abs(r_keyc.rgb);
        kc = kc / max(max(kc.r, kc.g), max(kc.b, 1e-3));
        float lit = smoothstep(0.0, 0.7, nl) * vis, dark = saturate(nl * 4.0) * (1.0 - vis);
        dark *= 1.0 - 0.6 * smoothstep(0.25, 0.65, l_luma(c));   // (a lamp-lit spot keeps its own light: no mud under a canopy by the fire)
        float head = saturate(1.15 - max(max(c.r, c.g), c.b));   // (bright surfaces: little headroom - no clipping to white)
        c *= 1.0 + r_keyc.w * lit * head * lerp(float3(1.0, 1.0, 1.0), kc, 0.05);   // (white light: the surfaces keep their own hue)
        c *= lerp(float3(1.0, 1.0, 1.0), shc * (1.0 - r_key.w), dark);
    }
    // exposure: what the occlusion, shadows and relight take away on average comes back (the picture at least as bright as
    // the game's own: depth from the contrast, not from darkening)
    c *= clamp(1.04 / max(r_stats.Load(int3(3, 0, 0)).x, 0.5), 1.0, 1.35);
    // light: bloom + shafts
    float3 bl = l_tex.SampleLevel(l_lin, p * l_dir.zw, 0.0).rgb * r_bloom.x;
    float3 sf = r_shafts.SampleLevel(l_lin, p * l_dir.zw, 0.0).rgb * r_shaft.x;
    float3 lin = r_srgb2lin(c) * r_grade.w;
    // clarity: local contrast against the picture's mean around (a ~16 pixel scale), on the brightness only
    {
        float Lc = l_luma(lin), Ll = l_luma(r_srgb2lin(r_scene.SampleLevel(l_lin, p * l_dir.zw, 3.0).rgb));
        lin *= clamp(pow(max((Lc + 0.004) / (Ll + 0.004), 1e-3), r_vig.w), 0.7, 1.45);
    }
    if (r_ksrc.w > 0.0 && !sky) {        // (2) warm glow lights its surroundings: fires and torches light the ground, walls and
        float3 g = min(bl, 2.0);         // characters near them in their own hue (the glow's own flicker: the flames' animation)
        float gm = max(max(g.r, g.g), g.b);
        float wg = saturate((g.r - g.b) / max(gm, 1e-4) * 1.6 - 0.2);
        lin += lin * g * (wg * r_ksrc.w);
    }
    lin += min(bl, 4.0) * (l_bloom.z * 0.35) + min(sf, 4.0);   // (the glow is light: added linearly)
    // atmosphere: depth fog + height fog towards the scene's sky colour (the sky itself has it already)
    if (!sky) {
        float3 P = r_world(p, w);
        float dist = w;
        float fd = max(dist - r_fog.x, 0.0) * r_fog.y;
        float h0 = r_zab.z - r_hfog.y;
        float dz = P.z - r_zab.z;
        float e0 = exp(-max(r_zab.z - h0, 0.0) / r_hfog.x), e1 = exp(-clamp(P.z - h0, 0.0, 50.0 * r_hfog.x) / r_hfog.x);   // (no thicker than at h0 below it: pits and pools stay clear)
        float hint = abs(dz) > 1.0 ? (e0 - e1) * r_hfog.x / -dz : e0;
        float fh = r_fog.w * dist * max(hint, 0.0);
        float fa = r_fog.z * (1.0 - exp(-(fd + fh)));
        float3 fcol = r_srgb2lin(fogc.rgb);
        fcol = lerp(l_luma(fcol).xxx, fcol, 0.5);   // (half the sky's hue: haze, not a tint)
        lin = lerp(lin, fcol, fa);
    }
    // grade: split toning by the mood (night: blue shadows and amber highlights; day: warm, a cool teal in the darks)
    float L = l_luma(lin);
    float3 cs = float3(0.99, 1.0, 1.01);                       // shadow tint (mild: the game's own hues stay)
    float3 ch = float3(1.05, 1.0, 0.93);                       // highlight tint
    float hw = smoothstep(0.02, 0.45, L);
    lin *= lerp(cs, ch, hw);
    // warm lamps: warm-hued light glows warmer and a little brighter against the night
    float warm = saturate((lin.r - lin.b) / max(lin.r, 1e-3)) * smoothstep(0.04, 0.35, L);
    lin *= 1.0 + warm * 0.3 * float3(1.0, 0.85, 0.6);
    // saturation and vibrance (luminance kept)
    L = l_luma(lin);
    float mx = max(max(lin.r, lin.g), lin.b), mn = min(min(lin.r, lin.g), lin.b);
    float sat = (mx - mn) / max(mx, 1e-4);
    // (vibrance: the dull colours gain most; skin-like warm tones and the sky are protected)
    float skin = (lin.r > lin.g && lin.g > lin.b) ? saturate(1.0 - abs(sat - 0.35) / 0.25) * saturate((lin.g - lin.b) / max(lin.r - lin.b, 1e-4) * 2.0) : 0.0;
    float prot = max(skin * 0.8, sky ? 0.8 : 0.0);
    float sk = 1.0 + (r_grade.x - 1.0 + r_grade.z * (1.0 - sat)) * (1.0 - prot);
    lin = max(lerp(L.xxx, lin, sk), 0.0);
    // filmic: the light added above white rolls off on a short shoulder, then the contrast curve
    float3 o = r_lin2srgb(lin);
    float3 sh2 = max(o - 1.0, 0.0);
    o = o - sh2 + (1.0 - exp(-sh2 * 2.0)) * 0.12;   // (above white: a short soft shoulder)
    float pv = clamp(sqrt(st0.w) * 0.9, 0.18, 0.5);            // (contrast around the scene's own mid-tone: no overall darkening)
    o = float3(r_curve(o.r, r_grade.y, pv), r_curve(o.g, r_grade.y, pv), r_curve(o.b, r_grade.y, pv));
    // vignette
    float2 v = (p * l_dir.zw - 0.5) * float2(l_dst.x * l_dst.w, 1.0);
    float vr = length(v) / length(float2(0.5 * l_dst.x * l_dst.w, 0.5));
    o *= 1.0 - r_vig.x * smoothstep(r_vig.y, r_vig.z, vr);
    if (r_misc.z == 1.0) o = sh.xxx;                       // (PS2_REIM_DBG=1: the screen-space shadow)
    else if (r_misc.z == 2.0) o = sf;                      // (2: the shafts)
    else if (r_misc.z == 3.0) { float3 P = r_world(p, w); o = sky ? float3(0, 0, 1) : frac(P / 500.0); }   // (3: world position bands)
    else if (r_misc.z == 4.0) o = fogc.rgb;                // (4: the fog colour)
    return float4(saturate(o), pic.a);
}
#endif
