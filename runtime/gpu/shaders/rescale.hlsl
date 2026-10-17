// (M7) A live target's contents at new sample factors (runtime/gpu/gpu_exec.c target_realloc): the
// target's family or the configured scale changed. New sample (i, j) of native pixel (x, y) takes old sample
// (i * okx / nkx, j * oky / nky) of the same native pixel, so sample 0 stays sample 0 (exact at any scale)
// and the other samples take the nearest old sample at or before their position. Colour words (read through
// the R8G8B8A8_UINT view, written as R32_UINT) or zpatch words (depth: zpatch is made current first).
cbuffer rescale_cb : register(b0) {
    uint okx, oky, nkx, nky;
    uint w, h, ow, oh;                   // new size, old size (samples)
    uint depth, rpad0, rpad1, rpad2;
};

Texture2D<uint4> src_c : register(t0);
Texture2D<uint> src_z : register(t1);
RWTexture2D<uint> dst : register(u0);

[numthreads(8, 8, 1)]
void cs_rescale(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    uint x = id.x / nkx, i = id.x - x * nkx, y = id.y / nky, j = id.y - y * nky;
    uint2 s = uint2(x * okx + i * okx / nkx, y * oky + j * oky / nky);
    uint v = 0u;
    if (s.x < ow && s.y < oh) {
        if (depth != 0u) v = src_z[s];
        else {
            uint4 c = src_c[s];
            v = c.x | (c.y << 8) | (c.z << 16) | (c.w << 24);
        }
    }
    dst[id.xy] = v;
}
