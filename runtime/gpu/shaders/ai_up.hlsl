// (ai) Real-ESRGAN RRDBNet inference, one 3x3 convolution layer per dispatch (notes/ai-textures.md; runtime/gpu/ai_up.c).
// Feature maps are float planes [image][channel][y][x] in up to 8 buffers (u0..u7, chosen per layer by index: the
// dense blocks append their growth channels to the buffer they read). Weights (t0) are packed per layer by ai_up.c:
// [oc block of 16][ci chunk of 8][ci 8][tap 9][oc 16], then the biases [oc block][16].
// A group computes a 32 x 8 tile of output pixels x 16 output channels; each thread 4 pixels (columns tx + 8k) x 16.
// The epilogue: v = conv + bias; lrelu(0.2) if act; v = v * a0 + res1 * a1 + res2 * a2; stored to out (and out2).
// A layer may be dispatched in bands of group rows (row0: the first; the game spreads the net over frames). No layer
// reads what it writes at another pixel, so bands are independent.

RWStructuredBuffer<float> U0 : register(u0);
RWStructuredBuffer<float> U1 : register(u1);
RWStructuredBuffer<float> U2 : register(u2);
RWStructuredBuffer<float> U3 : register(u3);
RWStructuredBuffer<float> U4 : register(u4);
RWStructuredBuffer<float> U5 : register(u5);
RWStructuredBuffer<float> U6 : register(u6);
RWStructuredBuffer<float> U7 : register(u7);
StructuredBuffer<float4> Wt : register(t0);

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

#define TW 32
#define TH 8
#define CI 8
groupshared float s_in[CI][TH + 2][TW + 2];
groupshared float4 s_w[CI * 9 * 4];

[numthreads(8, 8, 1)]
void cs_conv(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint li : SV_GroupIndex) {
    uint img = gid.z / n_ocb, ocb = gid.z - img * n_ocb;
    int x0 = (int)(gid.x * TW), y0 = (int)((gid.y + row0) * TH);
    uint iw = up_in ? W >> 1 : W, ih = up_in ? H >> 1 : H;
    uint plane = iw * ih;
    float4 acc[4][4];      // [pixel][oc / 4]
    [unroll] for (uint p = 0; p < 4; p++) [unroll] for (uint o = 0; o < 4; o++) acc[p][o] = 0;

    for (uint ch = 0; ch < n_cchunk; ch++) {
        GroupMemoryBarrierWithGroupSync();
        for (uint k = li; k < CI * (TH + 2) * (TW + 2); k += 64) {
            uint c = k / ((TH + 2) * (TW + 2)), r = k - c * ((TH + 2) * (TW + 2));
            uint ry = r / (TW + 2), rx = r - ry * (TW + 2);
            int yy = y0 + (int)ry - 1, xx = x0 + (int)rx - 1;
            uint cc = ch * CI + c;
            float v = 0;
            if (cc < cin && yy >= 0 && xx >= 0 && yy < (int)H && xx < (int)W) {
                uint sy = up_in ? (uint)yy >> 1 : (uint)yy, sx = up_in ? (uint)xx >> 1 : (uint)xx;
                v = BIN[(img * in_cap + cc) * plane + sy * iw + sx];
            }
            s_in[c][ry][rx] = v;
        }
        uint wb = w_off + (ocb * n_cchunk + ch) * (CI * 9 * 4);
        for (uint k2 = li; k2 < CI * 9 * 4; k2 += 64) s_w[k2] = Wt[wb + k2];
        GroupMemoryBarrierWithGroupSync();

        [loop] for (uint c = 0; c < CI; c++) {
            [unroll] for (uint ky = 0; ky < 3; ky++) {
                [unroll] for (uint kx = 0; kx < 3; kx++) {
                    float v0 = s_in[c][tid.y + ky][tid.x + kx];
                    float v1 = s_in[c][tid.y + ky][tid.x + 8 + kx];
                    float v2 = s_in[c][tid.y + ky][tid.x + 16 + kx];
                    float v3 = s_in[c][tid.y + ky][tid.x + 24 + kx];
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
            float4 v4 = acc[p2][q] + Wt[b_off + ocb * 4 + q];
            [unroll] for (uint j = 0; j < 4; j++) {
                uint oc = ocb * 16 + q * 4 + j;
                if (oc >= cout) continue;
                float v = j == 0 ? v4.x : j == 1 ? v4.y : j == 2 ? v4.z : v4.w;
                if (act) v = v >= 0 ? v : 0.2f * v;
                v *= a0;
                #ifdef BR1
                v += a1 * BR1[(img * r1_cap + r1_c0 + oc) * opl + pix];
#endif
                #ifdef BR2
                v += a2 * BR2[(img * r2_cap + r2_c0 + oc) * opl + pix];
#endif
                BOUT[(img * out_cap + out_c0 + oc) * opl + pix] = v;
                #ifdef BO2
                BO2[(img * o2_cap + o2_c0 + oc) * opl + pix] = v;
#endif
            }
        }
    }
}
