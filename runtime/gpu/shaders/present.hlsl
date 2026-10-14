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
