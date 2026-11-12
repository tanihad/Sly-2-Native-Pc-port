// (ai) Real-ESRGAN RRDBNet inference, one 3x3 convolution layer per dispatch (notes/ai-textures.md; runtime/gpu/ai_up.c).
// Feature maps are float planes [image][channel][y][x] in up to 8 buffers (u0..u7, chosen per layer by index: the
// dense blocks append their growth channels to the buffer they read). Weights (t0) are packed per layer by ai_up.c:
// [oc block of 16][ci chunk of 8][ci 8][tap 9][oc 16], then the biases [oc block][16].
// A group computes a 32 x 8 tile of output pixels x 16 output channels; each thread 4 pixels (columns tx + 8k) x 16.
// The epilogue: v = conv + bias; lrelu(0.2) if act; v = v * a0 + res1 * a1 + res2 * a2; stored to out (and out2).
// A layer may be dispatched in bands of group rows (row0: the first; the game spreads the net over frames). No layer
// reads what it writes at another pixel, so bands are independent.
// (tex-opt) AI_HALF: the feature buffers u1..u6 hold half floats, two channels per word ([image][channel / 2][y][x],
// channel 2k in the low half): half the memory and traffic, the arithmetic stays float. u0 (the input) and u7 (the
// output) stay float (cs_ai_res / cs_ai_post read them). AI_HALF_W: the weights as halves too (4 per uint2). The
// buffer-role macros BIN_H / BOUT_H / BR1_H / BR2_H / BO2_H say which buffers of a permutation are half.

// (U1_H .. U6_H: which buffers are half - ai_up.c's mask)
#if U1_H
RWStructuredBuffer<uint> U1 : register(u1);
#else
RWStructuredBuffer<float> U1 : register(u1);
#endif
#if U2_H
RWStructuredBuffer<uint> U2 : register(u2);
#else
RWStructuredBuffer<float> U2 : register(u2);
#endif
#if U3_H
RWStructuredBuffer<uint> U3 : register(u3);
#else
RWStructuredBuffer<float> U3 : register(u3);
#endif
#if U4_H
RWStructuredBuffer<uint> U4 : register(u4);
#else
RWStructuredBuffer<float> U4 : register(u4);
#endif
#if U5_H
RWStructuredBuffer<uint> U5 : register(u5);
#else
RWStructuredBuffer<float> U5 : register(u5);
#endif
#if U6_H
RWStructuredBuffer<uint> U6 : register(u6);
#else
RWStructuredBuffer<float> U6 : register(u6);
#endif
RWStructuredBuffer<float> U0 : register(u0);
RWStructuredBuffer<float> U7 : register(u7);
#ifdef AI_HALF_W
StructuredBuffer<uint2> Wt : register(t0);
float4 wload(uint i) { uint2 w = Wt[i]; return float4(f16tof32(w.x), f16tof32(w.x >> 16), f16tof32(w.y), f16tof32(w.y >> 16)); }
#else
StructuredBuffer<float4> Wt : register(t0);
float4 wload(uint i) { return Wt[i]; }
#endif

cbuffer AiLayer : register(b0) {
    uint in_buf, in_cap, cin, up_in;        // input buffer, its channel capacity, input channels (from 0), nearest 2x
    uint out_buf, out_c0, out_cap, cout;    // output buffer, first channel written, capacity, output channels
    uint W, H, act, n_ocb;                  // output size (the input is W/2 x H/2 with up_in), lrelu, oc blocks
    uint r1_buf, r1_c0, r1_cap, r2_buf;     // residuals (buffer 8 = none): channel r1_c0 + oc
    uint r2_c0, r2_cap, o2_buf, o2_c0;      // second output (8 = none)
    uint o2_cap, w_off, b_off, n_cchunk;    // weights / biases (float4 units), input channel chunks of 8
    float a0, a1, a2;
    uint row0;                              // (time slicing) first group row of this dispatch: rows split over frames
};

// The buffers of a layer are compile-time choices (ai_up.c compiles one permutation per combination, ~12):
// BIN (input), BOUT (output), BR1 / BR2 (residuals, optional), BO2 (second output, optional).
#ifndef BIN
#define BIN U0
#define BOUT U1
#endif
#ifndef U1_H
#define U1_H 0
#endif
#ifndef U2_H
#define U2_H 0
#endif
#ifndef U3_H
#define U3_H 0
#endif
#ifndef U4_H
#define U4_H 0
#endif
#ifndef U5_H
#define U5_H 0
#endif
#ifndef U6_H
#define U6_H 0
#endif
#ifndef BIN_H
#define BIN_H 0
#endif
#ifndef BOUT_H
#define BOUT_H 0
#endif
#ifndef BR1_H
#define BR1_H 0
#endif
#ifndef BR2_H
#define BR2_H 0
#endif
#ifndef BO2_H
#define BO2_H 0
#endif
float2 h2(uint w) { return float2(f16tof32(w), f16tof32(w >> 16)); }
uint ph2(float a, float b) { return f32tof16(a) | (f32tof16(b) << 16); }

#define TW 32
#define TH 8
#define CI 8
#define TE ((TH + 2) * (TW + 2))   // the input tile's pixels (an apron of 1)
#define TP ((TE + 63) / 64)        // per thread
groupshared float s_in[CI * TE];
groupshared float4 s_w[CI * 9 * 4];

[numthreads(8, 8, 1)]
void cs_conv(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint li : SV_GroupIndex) {
    uint img = gid.z / n_ocb, ocb = gid.z - img * n_ocb;
    int x0 = (int)(gid.x * TW), y0 = (int)((gid.y + row0) * TH);
    uint iw = up_in ? W >> 1 : W, ih = up_in ? H >> 1 : H;
    uint plane = iw * ih;
    float4 acc[4][4];      // [pixel][oc / 4]
    [unroll] for (uint p = 0; p < 4; p++) [unroll] for (uint o = 0; o < 4; o++) acc[p][o] = 0;
    // (tex-opt) the tile pixels this thread loads, once: their offset in the input plane (-1: outside, zero)
    int toff[TP];
    [unroll] for (uint t = 0; t < TP; t++) {
        uint e = li + 64u * t;
        uint ry = e / (TW + 2), rx = e - ry * (TW + 2);
        int yy = y0 + (int)ry - 1, xx = x0 + (int)rx - 1;
        bool in_ = e < TE && yy >= 0 && xx >= 0 && yy < (int)H && xx < (int)W;
        uint sy = up_in ? (uint)yy >> 1 : (uint)yy, sx = up_in ? (uint)xx >> 1 : (uint)xx;
        toff[t] = in_ ? (int)(sy * iw + sx) : -1;
    }

    for (uint ch = 0; ch < n_cchunk; ch++) {
        GroupMemoryBarrierWithGroupSync();
#if BIN_H
        [unroll] for (uint lc = 0; lc < CI / 2; lc++) {   // (two channels per word)
            uint cc = ch * CI + 2 * lc;
            uint pb = (img * (in_cap >> 1) + (cc >> 1)) * plane;
            [unroll] for (uint lt = 0; lt < TP; lt++) {
                uint e = li + 64u * lt;
                if (e < TE) {
                    float2 v = cc < cin && toff[lt] >= 0 ? h2(BIN[pb + (uint)toff[lt]]) : float2(0, 0);
                    s_in[2 * lc * TE + e] = v.x;
                    s_in[(2 * lc + 1) * TE + e] = v.y;
                }
            }
        }
#else
        [unroll] for (uint lc = 0; lc < CI; lc++) {
            uint cc = ch * CI + lc;
            uint pb = (img * in_cap + cc) * plane;
            [unroll] for (uint lt = 0; lt < TP; lt++) {
                uint e = li + 64u * lt;
                if (e < TE) s_in[lc * TE + e] = cc < cin && toff[lt] >= 0 ? BIN[pb + (uint)toff[lt]] : 0.0;
            }
        }
#endif
        uint wb = w_off + (ocb * n_cchunk + ch) * (CI * 9 * 4);
        for (uint k2 = li; k2 < CI * 9 * 4; k2 += 64) s_w[k2] = wload(wb + k2);
        GroupMemoryBarrierWithGroupSync();

        [loop] for (uint c = 0; c < CI; c++) {
            [unroll] for (uint ky = 0; ky < 3; ky++) {
                [unroll] for (uint kx = 0; kx < 3; kx++) {
                    uint sb = c * TE + (tid.y + ky) * (TW + 2) + tid.x + kx;
                    float v0 = s_in[sb], v1 = s_in[sb + 8], v2 = s_in[sb + 16], v3 = s_in[sb + 24];
                    uint wi = (c * 9 + ky * 3 + kx) * 4;
                    [unroll] for (uint q = 0; q < 4; q++) {
                        float4 w = s_w[wi + q];
                        acc[0][q] = mad(v0, w, acc[0][q]);
                        acc[1][q] = mad(v1, w, acc[1][q]);
                        acc[2][q] = mad(v2, w, acc[2][q]);
                        acc[3][q] = mad(v3, w, acc[3][q]);
                    }
                }
            }
        }
    }

    uint y = (uint)y0 + tid.y;
    if (y >= H) return;
    uint opl = W * H;
    [unroll] for (uint p2 = 0; p2 < 4; p2++) {
        uint x = (uint)x0 + tid.x + 8 * p2;
        if (x >= W) continue;
        uint pix = y * W + x;
        [unroll] for (uint q = 0; q < 4; q++) {
            float4 v4 = acc[p2][q] + wload(b_off + ocb * 4 + q);
            if (act) v4 = v4 >= 0 ? v4 : 0.2f * v4;
            v4 *= a0;
            uint oc0 = ocb * 16 + q * 4;
#if BR1_H
            { uint i1 = (img * (r1_cap >> 1) + ((r1_c0 + oc0) >> 1)) * opl + pix; v4 += a1 * float4(h2(BR1[i1]), h2(BR1[i1 + opl])); }
#elif defined(BR1)
            [unroll] for (uint j1 = 0; j1 < 4; j1++) if (oc0 + j1 < cout) v4[j1] += a1 * BR1[(img * r1_cap + r1_c0 + oc0 + j1) * opl + pix];
#endif
#if BR2_H
            { uint i2 = (img * (r2_cap >> 1) + ((r2_c0 + oc0) >> 1)) * opl + pix; v4 += a2 * float4(h2(BR2[i2]), h2(BR2[i2 + opl])); }
#elif defined(BR2)
            [unroll] for (uint j2 = 0; j2 < 4; j2++) if (oc0 + j2 < cout) v4[j2] += a2 * BR2[(img * r2_cap + r2_c0 + oc0 + j2) * opl + pix];
#endif
#if BOUT_H
            { uint io = (img * (out_cap >> 1) + ((out_c0 + oc0) >> 1)) * opl + pix; BOUT[io] = ph2(v4.x, v4.y); BOUT[io + opl] = ph2(v4.z, v4.w); }
#else
            [unroll] for (uint j = 0; j < 4; j++) if (oc0 + j < cout) BOUT[(img * out_cap + out_c0 + oc0 + j) * opl + pix] = v4[j];
#endif
#if BO2_H
            { uint i3 = (img * (o2_cap >> 1) + ((o2_c0 + oc0) >> 1)) * opl + pix; BO2[i3] = ph2(v4.x, v4.y); BO2[i3 + opl] = ph2(v4.z, v4.w); }
#elif defined(BO2)
            [unroll] for (uint j3 = 0; j3 < 4; j3++) if (oc0 + j3 < cout) BO2[(img * o2_cap + o2_c0 + oc0 + j3) * opl + pix] = v4[j3];
#endif
        }
    }
}
