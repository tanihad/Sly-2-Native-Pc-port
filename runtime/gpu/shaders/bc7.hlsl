// (tex-opt) BC7 encoder for the opaque Reimagined AI images (runtime/gpu/gpu_tex_ai.h, notes/reimagined.md "Lighter and
// faster"): one thread per 4 x 4 block of one level of an RGBA8 class slice (the GS's bytes), written as a
// R32G32B32A32_UINT texel and copied into the BC7 class (a block-compatible copy). Every block decodes with alpha 255;
// the sampler scales a BC7 class's alpha by 128 / 255 (etex.hlsli: the GS's 0x80). Images with alpha stay RGBA8.
// Modes per block: 6 (one subset, 7-bit endpoints + p-bit 1 for alpha 255, 4-bit indices), and on the two partitions with
// the smallest within-subset variance 1 (two subsets, 6-bit + a p-bit per subset, 3-bit indices) and 3 (7-bit + a p-bit
// per endpoint, 2-bit indices). Endpoints: the subset's principal axis -> its extremes, every p-bit choice, a
// least-squares refinement; indices by projection onto the decoded endpoints (+-1 checked); the block keeps the mode with
// the smallest squared RGB error. Partition / anchor tables read back from the hardware decoder (scripts/bc7_tool.c probe).
Texture2DArray<float4> b_src : register(t0);
RWTexture2D<uint4> b_out : register(u0);
cbuffer bc7_cb : register(b0) {
    uint4 b_a;                           // slice, level, blocks across, blocks down
    uint4 b_b;                           // the level's width, height, -, -
};

static const uint bc7_p2[64] = {         // 2 bits per pixel: the pixel's subset
    0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000,
    0x50400000, 0x55555450, 0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000,
    0x55150100, 0x00004054, 0x15010000, 0x00405054, 0x00004050, 0x15050100, 0x05010000, 0x40505054,
    0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500, 0x15014054, 0x05414150,
    0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
    0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400,
    0x00041504, 0x00105410, 0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150,
    0x41050514, 0x41505014, 0x40011554, 0x54150140, 0x50505500, 0x00555050, 0x15151010, 0x54540404 };
static const uint bc7_a2[64] = {        // the second subset's anchor pixel
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15 };
static const uint W2[4] = { 0, 21, 43, 64 };
static const uint W3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const uint W4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

#ifndef BC7_THRESH
#define BC7_THRESH 16u
#endif
#ifndef BC7_NPART
#define BC7_NPART 2u
#endif
#ifndef BC7_MODE1
#define BC7_MODE1 1
#endif
static int3 px[16];                      // the block's RGB (0..255)

uint wt(uint nb, uint i) { return nb == 2u ? W2[i & 3u] : nb == 3u ? W3[i & 7u] : W4[i & 15u]; }
int3 lerp6(int3 a, int3 b, uint w) { return (a * (64 - (int)w) + b * (int)w + 32) >> 6; }
uint sub_of(uint part, uint i) { return (part >> (2u * i)) & 3u; }

// endpoint quantizers: kind 0 = 6 bits + p (mode 1), 1 = 7 bits + p (modes 3, 6)
int unq(uint kind, int q, uint p) {
    if (kind == 0u) { int v = (q << 1) | (int)p; return (v << 1) | (v >> 6); }
    return (q << 1) | (int)p;
}
int quant(uint kind, float f, uint p) {  // the q whose decoded value is nearest f
    int q0 = kind == 0u ? (int)round((f - 2.0 * (float)p) / 4.0315) : (int)round((f - (float)p) * 0.5);
    int mx = kind == 0u ? 63 : 127, best = 0, be = 100000;
    [unroll] for (int d = -1; d <= 1; d++) {
        int q = clamp(q0 + d, 0, mx);
        int e = abs(unq(kind, q, p) - (int)round(f));
        if (e < be) { be = e; best = q; }
    }
    return best;
}
int3 quant3(uint kind, float3 f, uint p) { return int3(quant(kind, f.x, p), quant(kind, f.y, p), quant(kind, f.z, p)); }
int3 unq3(uint kind, int3 q, uint p) { return int3(unq(kind, q.x, p), unq(kind, q.y, p), unq(kind, q.z, p)); }

// the principal axis of subset s: its extremes
void axis_ends(uint part, uint s, out float3 e0, out float3 e1) {
    float3 mean = 0.0;
    float n = 0.0;
    [unroll] for (uint i = 0u; i < 16u; i++) if (sub_of(part, i) == s) { mean += (float3)px[i]; n += 1.0; }
    mean /= max(n, 1.0);
    float3 c0 = 0.0, c1 = 0.0, c2 = 0.0;
    [unroll] for (uint j = 0u; j < 16u; j++) if (sub_of(part, j) == s) {
        float3 d = (float3)px[j] - mean;
        c0 += d * d.x; c1 += d * d.y; c2 += d * d.z;
    }
    float3 v = float3(0.577, 0.577, 0.577);
    [unroll] for (uint k = 0u; k < 5u; k++) { float3 nv = c0 * v.x + c1 * v.y + c2 * v.z; float l = length(nv); v = l > 1e-6 ? nv / l : v; }
    float tmin = 0.0, tmax = 0.0;
    [unroll] for (uint m = 0u; m < 16u; m++) if (sub_of(part, m) == s) {
        float t = dot((float3)px[m] - mean, v);
        tmin = min(tmin, t); tmax = max(tmax, t);
    }
    e0 = clamp(mean + v * tmin, 0.0, 255.0);
    e1 = clamp(mean + v * tmax, 0.0, 255.0);
}

// indices of subset s for decoded endpoints a, b (nb bits): projection, then the nearest of three; returns the error
uint assign(uint part, uint s, int3 a, int3 b, uint nb, inout uint idx[16]) {
    uint err = 0u, mx = (1u << nb) - 1u;
    float3 d = (float3)(b - a);
    float dd = dot(d, d), sc = dd > 0.0 ? (float)mx / dd : 0.0;
    [unroll] for (uint i = 0u; i < 16u; i++) {
        if (sub_of(part, i) != s) continue;
        int k0 = (int)round(dot((float3)(px[i] - a), d) * sc);
        uint bi = 0u, be = 0xFFFFFFFFu;
        [unroll] for (int o = -1; o <= 1; o++) {
            uint k = (uint)clamp(k0 + o, 0, (int)mx);
            int3 e = lerp6(a, b, wt(nb, k)) - px[i];
            uint ee = (uint)dot(e, e);
            if (ee < be) { be = ee; bi = k; }
        }
        idx[i] = bi;
        err += be;
    }
    return err;
}
// least squares: the endpoints that fit subset s best for its indices
void refine(uint part, uint s, uint nb, uint idx[16], inout float3 e0, inout float3 e1) {
    float a = 0.0, b = 0.0, c = 0.0;
    float3 r0 = 0.0, r1 = 0.0;
    [unroll] for (uint i = 0u; i < 16u; i++) {
        if (sub_of(part, i) != s) continue;
        float w = (float)wt(nb, idx[i]) * (1.0 / 64.0), u = 1.0 - w;
        a += u * u; b += u * w; c += w * w;
        r0 += u * (float3)px[i]; r1 += w * (float3)px[i];
    }
    float det = a * c - b * b;
    if (abs(det) < 1e-4) return;
    e0 = clamp((c * r0 - b * r1) / det, 0.0, 255.0);
    e1 = clamp((a * r1 - b * r0) / det, 0.0, 255.0);
}

struct Fit { int3 q0, q1; uint p0, p1; uint err; };
// one subset: nb index bits, quantizer kind, p-bits pm (1: one per subset, 2: one per endpoint, 3: both forced to 1)
Fit fit(uint part, uint s, uint nb, uint kind, uint pm, inout uint idx[16]) {
    float3 e0, e1;
    axis_ends(part, s, e0, e1);
    Fit best;
    best.q0 = 0; best.q1 = 0; best.p0 = 0u; best.p1 = 0u; best.err = 0xFFFFFFFFu;
    uint bidx[16];
    [unroll] for (uint z = 0u; z < 16u; z++) bidx[z] = idx[z];
    [loop] for (uint it = 0u; it < 2u; it++) {
        uint ncomb = pm == 1u ? 2u : pm == 2u ? 4u : 1u;
        [loop] for (uint c = 0u; c < ncomb; c++) {
            uint p0 = pm == 3u ? 1u : pm == 1u ? c : (c & 1u), p1 = pm == 3u ? 1u : pm == 1u ? c : (c >> 1);
            int3 q0 = quant3(kind, e0, p0), q1 = quant3(kind, e1, p1);
            uint ti[16];
            [unroll] for (uint y = 0u; y < 16u; y++) ti[y] = bidx[y];
            uint e = assign(part, s, unq3(kind, q0, p0), unq3(kind, q1, p1), nb, ti);
            if (e < best.err) {
                best.err = e; best.q0 = q0; best.q1 = q1; best.p0 = p0; best.p1 = p1;
                [unroll] for (uint y2 = 0u; y2 < 16u; y2++) bidx[y2] = ti[y2];
            }
        }
        if (best.err == 0u) break;
        refine(part, s, nb, bidx, e0, e1);
    }
    [unroll] for (uint y3 = 0u; y3 < 16u; y3++) if (sub_of(part, y3) == s) idx[y3] = bidx[y3];
    return best;
}

// ---- bit packing --------------------------------------------------------------------------------------------------------
static uint ob[4];
static uint opos;
void put(uint v, uint n) {               // (n <= 8 here; a field never crosses more than one word boundary)
    uint w = opos >> 5, sh = opos & 31u;
    uint lo = v << sh, hi = sh ? v >> (32u - sh) : 0u;
    if (w == 0u) { ob[0] |= lo; ob[1] |= hi; }
    else if (w == 1u) { ob[1] |= lo; ob[2] |= hi; }
    else if (w == 2u) { ob[2] |= lo; ob[3] |= hi; }
    else ob[3] |= lo;
    opos += n;
}
void put_begin() { ob[0] = 0u; ob[1] = 0u; ob[2] = 0u; ob[3] = 0u; opos = 0u; }
void swap_ends(inout Fit f) { int3 t = f.q0; f.q0 = f.q1; f.q1 = t; uint p = f.p0; f.p0 = f.p1; f.p1 = p; }

uint4 pack6(Fit f, uint idx[16]) {       // mode 6, alpha 127 + p-bit 1 = 255
    if (idx[0] >= 8u) { swap_ends(f); [unroll] for (uint i = 0u; i < 16u; i++) idx[i] = 15u - idx[i]; }
    put_begin();
    put(64u, 7u);
    put((uint)f.q0.x, 7u); put((uint)f.q1.x, 7u); put((uint)f.q0.y, 7u); put((uint)f.q1.y, 7u);
    put((uint)f.q0.z, 7u); put((uint)f.q1.z, 7u); put(127u, 7u); put(127u, 7u);
    put(f.p0, 1u); put(f.p1, 1u);
    [unroll] for (uint j = 0u; j < 16u; j++) put(idx[j], j == 0u ? 3u : 4u);
    return uint4(ob[0], ob[1], ob[2], ob[3]);
}
uint4 pack13(uint mode, uint pi, Fit f0, Fit f1, uint idx[16]) {   // modes 1 and 3
    uint part = bc7_p2[pi], anc = bc7_a2[pi], nb = mode == 1u ? 3u : 2u, hi = 1u << (nb - 1u), mx = (1u << nb) - 1u;
    if (idx[0] >= hi) { swap_ends(f0); [unroll] for (uint i = 0u; i < 16u; i++) if (sub_of(part, i) == 0u) idx[i] = mx - idx[i]; }
    if (idx[anc] >= hi) { swap_ends(f1); [unroll] for (uint i2 = 0u; i2 < 16u; i2++) if (sub_of(part, i2) == 1u) idx[i2] = mx - idx[i2]; }
    put_begin();
    uint cb = mode == 1u ? 6u : 7u;
    put(mode == 1u ? 2u : 8u, mode == 1u ? 2u : 4u);
    put(pi, 6u);
    put((uint)f0.q0.x, cb); put((uint)f0.q1.x, cb); put((uint)f1.q0.x, cb); put((uint)f1.q1.x, cb);
    put((uint)f0.q0.y, cb); put((uint)f0.q1.y, cb); put((uint)f1.q0.y, cb); put((uint)f1.q1.y, cb);
    put((uint)f0.q0.z, cb); put((uint)f0.q1.z, cb); put((uint)f1.q0.z, cb); put((uint)f1.q1.z, cb);
    if (mode == 1u) { put(f0.p0, 1u); put(f1.p0, 1u); }
    else { put(f0.p0, 1u); put(f0.p1, 1u); put(f1.p0, 1u); put(f1.p1, 1u); }
    [unroll] for (uint j = 0u; j < 16u; j++) put(idx[j], j == 0u || j == anc ? nb - 1u : nb);
    return uint4(ob[0], ob[1], ob[2], ob[3]);
}

[numthreads(8, 8, 1)]
void cs_bc7(uint3 id : SV_DispatchThreadID) {
    if (id.x >= b_a.z || id.y >= b_a.w) return;
    float3 sum = 0.0;
    float sq = 0.0;
    float3 pv[16];
    float pq[16];
    [unroll] for (uint i = 0u; i < 16u; i++) {
        uint x = min(id.x * 4u + (i & 3u), b_b.x - 1u), y = min(id.y * 4u + (i >> 2), b_b.y - 1u);
        int3 c = (int3)(saturate(b_src.Load(int4(x, y, b_a.x, b_a.y)).rgb) * 255.0 + 0.5);
        px[i] = c;
        pv[i] = (float3)c;
        pq[i] = dot(pv[i], pv[i]);
        sum += pv[i]; sq += pq[i];
    }
    uint idx[16];
    [unroll] for (uint z = 0u; z < 16u; z++) idx[z] = 0u;
    Fit f6 = fit(0u, 0u, 4u, 1u, 3u, idx);
    uint4 best = pack6(f6, idx);
    uint be = f6.err;
#ifndef BC7_ONLY6
    if (be > BC7_THRESH) {               // (a block mode 6 fits closely enough: done)
        // the two partitions with the smallest within-subset variance (sums: subset 1 = the block - subset 0)
        uint p1 = 0u, p2 = 1u;
        float c1 = 1e30, c2 = 1e30;
        [loop] for (uint p = 0u; p < 64u; p++) {
            uint part = bc7_p2[p];
            float3 s0 = 0.0;
            float q0 = 0.0, n0 = 0.0;
            [unroll] for (uint k = 0u; k < 16u; k++) if (sub_of(part, k) == 0u) { s0 += pv[k]; q0 += pq[k]; n0 += 1.0; }
            float3 s1 = sum - s0;
            float n1 = 16.0 - n0;
            float c = q0 - dot(s0, s0) / n0 + (sq - q0) - dot(s1, s1) / n1;
            if (c < c1) { c2 = c1; p2 = p1; c1 = c; p1 = p; }
            else if (c < c2) { c2 = c; p2 = p; }
        }
        [loop] for (uint t = 0u; t < BC7_NPART; t++) {
            uint pi = t == 0u ? p1 : p2, part = bc7_p2[pi];
            Fit a, b;
#if BC7_MODE1
            a = fit(part, 0u, 3u, 0u, 1u, idx);
            b = fit(part, 1u, 3u, 0u, 1u, idx);
            if (a.err + b.err < be) { be = a.err + b.err; best = pack13(1u, pi, a, b, idx); }
#endif
            a = fit(part, 0u, 2u, 1u, 2u, idx);
            b = fit(part, 1u, 2u, 1u, 2u, idx);
            if (a.err + b.err < be) { be = a.err + b.err; best = pack13(3u, pi, a, b, idx); }
        }
    }
#endif
    b_out[id.xy] = best;
}
