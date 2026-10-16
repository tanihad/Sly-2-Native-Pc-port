// Picture compose and present passes (runtime/gpu/gpu_present.c).

Texture2D<float4> src : register(t0);
SamplerState smp : register(s0);

cbuffer present_cb : register(b0) {
    float2 uv_scale;                     // picture size / texture size
    float2 uv_max;                       // last texel centre, so bilinear never reads past the picture
};

struct V {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// one triangle covering the viewport, no vertex buffer
V vs_fullscreen(uint id : SV_VertexID) {
    V o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    o.uv = t;
    return o;
}

// M0 compose: the software picture holds BGRX bytes (uploaded as R8G8B8A8): swap to RGBA, 1:1
float4 ps_compose(V i) : SV_Target {
    float4 c = src.Load(int3(i.pos.xy, 0));
    return float4(c.b, c.g, c.r, 1.0);
}

// present: bilinear scale of the picture into the letterboxed viewport
float4 ps_present(V i) : SV_Target {
    float2 uv = min(i.uv * uv_scale, uv_max);
    return float4(src.SampleLevel(smp, uv, 0).rgb, 1.0);
}

// (M7, notes/d3d11-renderer.md 8.1) separable resample of the picture into the viewport, one axis per pass:
// horizontal (picture -> intermediate lw x picture rows), then vertical (intermediate -> viewport). Output
// pixel d (centre d + 0.5) samples source position c = (d + 0.5) * ratio - shift in texel units (texel k is
// centred on k + 0.5); shift removes the sample-grid offset of scaled pictures. Filters: 0 point, 1 box
// (area of the footprint), 2 Lanczos-2, 3 Catmull-Rom, 4 linear; shrinking widens the kernel by the ratio
// (supersampling: the sharp downscale). Edge texels repeat. Values are gamma-space RGB, as the PS2's.
cbuffer resample_cb : register(b1) {
    float src_n;                         // source texels along the axis
    float dst_n;                         // destination pixels along the axis
    float shift;                         // sample-grid shift (texels)
    float ratio;                         // src_n / dst_n
    uint mode, axis, other_n, rpad0;     // filter; 0 columns / 1 rows; source size along the other axis
    float2 origin;                       // viewport origin of the destination (pixels)
    float2 rpad1;
};

static const float PI = 3.14159265358979;

float sinc(float x) { return abs(x) < 1e-4 ? 1.0 : sin(PI * x) / (PI * x); }

float kern(float x) {                    // x in kernel units
    float a = abs(x);
    if (mode == 2u) return a < 2.0 ? sinc(x) * sinc(x * 0.5) : 0.0;
    if (mode == 3u) {                    // Catmull-Rom (B = 0, C = 0.5)
        if (a < 1.0) return 1.5 * a * a * a - 2.5 * a * a + 1.0;
        if (a < 2.0) return -0.5 * a * a * a + 2.5 * a * a - 4.0 * a + 2.0;
        return 0.0;
    }
    return max(1.0 - a, 0.0);            // linear
}

float3 tap(int k, int o) {
    k = clamp(k, 0, (int)src_n - 1);
    return axis == 0u ? src.Load(int3(k, o, 0)).rgb : src.Load(int3(o, k, 0)).rgb;
}

float4 ps_resample(V i) : SV_Target {
    float2 p = i.pos.xy - origin;
    float d = axis == 0u ? p.x : p.y;    // (pixel centre: d = index + 0.5)
    int o = (int)(axis == 0u ? p.y : p.x);
    float c = d * ratio - shift;
    float3 acc = 0.0;
    float wsum = 0.0;
    if (mode == 0u) return float4(tap((int)floor(c), o), 1.0);
    if (mode == 1u) {                    // box: the footprint [c - ratio / 2, c + ratio / 2) by overlap
        float h = max(ratio, 1e-3) * 0.5, a0 = c - h, a1 = c + h;
        int k0 = (int)floor(a0), k1 = (int)ceil(a1);
        [loop] for (int k = k0; k < k1 && k < k0 + 64; k++) {
            float w = min((float)k + 1.0, a1) - max((float)k, a0);
            if (w <= 0.0) continue;
            acc += w * tap(k, o);
            wsum += w;
        }
    } else {
        float s = max(ratio, 1.0);       // kernel scale: wider when shrinking
        float r = (mode == 4u ? 1.0 : 2.0) * s;
        int k0 = (int)floor(c - r - 0.5), k1 = (int)ceil(c + r - 0.5);
        [loop] for (int k = k0; k <= k1 && k <= k0 + 128; k++) {
            float w = kern(((float)k + 0.5 - c) / s);
            if (w == 0.0) continue;
            acc += w * tap(k, o);
            wsum += w;
        }
    }
    float3 v = wsum != 0.0 ? acc / wsum : tap((int)floor(c), o);
    return float4(saturate(v), 1.0);
}
