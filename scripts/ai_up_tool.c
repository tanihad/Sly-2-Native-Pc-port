/* (ai) Standalone Real-ESRGAN x4 tool for the AI-texture prototype (notes/ai-textures.md): runtime/gpu/ai_up.c on a
 * headless D3D11 device (no window). Built by scripts/ai_textures.py (build\tools\ai_up.exe).
 *
 *   ai_up cpu   <weights.bin> <in.f32> <out.f32>                       C reference (slow)
 *   ai_up gpu   <weights.bin> <ai_up.hlsl> <in.f32> <out.f32>          one image on the GPU
 *   ai_up batch <weights.bin> <ai_up.hlsl> <in.bin> <out.bin> [max_pixels]
 *
 * f32: u32 w, u32 h, float RGB planes. in.bin: "AIB1", u32 count, per image u32 w, h + RGB bytes (interleaved);
 * out.bin: "AIO1", u32 count, per image u32 4w, 4h + RGB bytes. batch groups same-sized images into one dispatch
 * chain each and prints the time and the video memory (the process's DXGI usage, sampled after every batch). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/gpu/ai_up.h"

static double now(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static float* read_f32(const char* p, int* w, int* h) {
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    uint32_t d[2];
    float* px = NULL;
    if (fread(d, 4, 2, f) == 2 && d[0] && d[1] && d[0] <= 4096 && d[1] <= 4096) {
        px = malloc((size_t)d[0] * d[1] * 12);
        if (px && fread(px, 4, (size_t)d[0] * d[1] * 3, f) != (size_t)d[0] * d[1] * 3) { free(px); px = NULL; }
        *w = (int)d[0]; *h = (int)d[1];
    }
    fclose(f);
    return px;
}
static int write_f32(const char* p, const float* px, int w, int h) {
    FILE* f = fopen(p, "wb");
    if (!f) return -1;
    uint32_t d[2] = { (uint32_t)w, (uint32_t)h };
    fwrite(d, 4, 2, f);
    fwrite(px, 4, (size_t)w * h * 3, f);
    fclose(f);
    return 0;
}

static IDXGIAdapter3* g_ad3;
static size_t g_peak;
static void vram_sample(void) {
    DXGI_QUERY_VIDEO_MEMORY_INFO mi;
    if (g_ad3 && SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(g_ad3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)) &&
        mi.CurrentUsage > g_peak) g_peak = (size_t)mi.CurrentUsage;
}

static int make_device(ID3D11Device** dev, ID3D11DeviceContext** ctx) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, dev, &got, ctx))) {
        fprintf(stderr, "no D3D11 device\n");
        return -1;
    }
    IDXGIDevice* dd = NULL;
    IDXGIAdapter* ad = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(*dev, &IID_IDXGIDevice, (void**)&dd)) && SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))) {
        DXGI_ADAPTER_DESC de;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &de))) fprintf(stderr, "adapter: %ls (%llu MB)\n", de.Description, (unsigned long long)de.DedicatedVideoMemory >> 20);
        IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter3, (void**)&g_ad3);
        IDXGIAdapter_Release(ad);
    }
    if (dd) IDXGIDevice_Release(dd);
    return 0;
}

typedef struct { uint32_t w, h; uint8_t* px; uint8_t* out; } img_t;
static int cmp_size(const void* a, const void* b) {
    const img_t* x = *(img_t* const*)a; const img_t* y = *(img_t* const*)b;
    if (x->w != y->w) return x->w < y->w ? -1 : 1;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
    char err[512] = "";
    if (argc < 5) { fprintf(stderr, "usage: see scripts/ai_up_tool.c\n"); return 2; }
    ai_weights* wt = ai_weights_load(argv[2], err, sizeof err);
    if (!wt) { fprintf(stderr, "%s\n", err); return 1; }
    if (!strcmp(argv[1], "cpu")) {
        int w, h;
        float* in = read_f32(argv[3], &w, &h);
        if (!in) { fprintf(stderr, "bad input\n"); return 1; }
        float* out = malloc((size_t)w * h * 48 * 4);
        double t = now();
        ai_cpu_forward(wt, in, w, h, out);
        fprintf(stderr, "cpu: %dx%d in %.2f s\n", w, h, now() - t);
        return write_f32(argv[4], out, 4 * w, 4 * h) ? 1 : 0;
    }
    if (argc < 6) { fprintf(stderr, "usage: see scripts/ai_up_tool.c\n"); return 2; }
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    if (make_device(&dev, &ctx)) return 1;
    vram_sample();
    size_t base = g_peak;
    if (!strcmp(argv[1], "gpu")) {
        int w, h;
        float* in = read_f32(argv[4], &w, &h);
        if (!in) { fprintf(stderr, "bad input\n"); return 1; }
        ai_gpu* g = ai_gpu_create(dev, ctx, wt, argv[3], (size_t)w * h, err, sizeof err);
        if (!g) { fprintf(stderr, "%s\n", err); return 1; }
        float* out = malloc((size_t)w * h * 48 * 4);
        double t = now();
        int r = ai_gpu_run(g, 1, w, h, in, out);
        fprintf(stderr, "gpu: %dx%d in %.3f s (%d)\n", w, h, now() - t, r);
        return r || write_f32(argv[5], out, 4 * w, 4 * h) ? 1 : 0;
    }
    if (strcmp(argv[1], "batch")) { fprintf(stderr, "unknown mode %s\n", argv[1]); return 2; }
    size_t maxp = argc > 6 ? (size_t)_strtoui64(argv[6], NULL, 10) : 131072;
    FILE* f = fopen(argv[4], "rb");
    uint32_t hd[2];
    if (!f || fread(hd, 4, 2, f) != 2 || memcmp(hd, "AIB1", 4)) { fprintf(stderr, "bad batch file\n"); return 1; }
    uint32_t n = hd[1];
    img_t* im = calloc(n, sizeof *im);
    img_t** ord = calloc(n, sizeof *ord);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d[2];
        if (fread(d, 4, 2, f) != 2 || !d[0] || !d[1] || (size_t)d[0] * d[1] > maxp) { fprintf(stderr, "image %u: bad size\n", i); return 1; }
        im[i].w = d[0]; im[i].h = d[1];
        im[i].px = malloc((size_t)d[0] * d[1] * 3);
        if (fread(im[i].px, 1, (size_t)d[0] * d[1] * 3, f) != (size_t)d[0] * d[1] * 3) { fprintf(stderr, "short batch file\n"); return 1; }
        ord[i] = &im[i];
    }
    fclose(f);
    qsort(ord, n, sizeof *ord, cmp_size);
    double t0 = now();
    ai_gpu* g = ai_gpu_create(dev, ctx, wt, argv[3], maxp, err, sizeof err);
    if (!g) { fprintf(stderr, "%s\n", err); return 1; }
    double t1 = now(), tgpu = 0;
    float* fin = malloc(maxp * 3 * 4);
    float* fout = malloc(maxp * 48 * 4);
    uint64_t px_in = 0;
    int batches = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t w = ord[i]->w, h = ord[i]->h;
        size_t P = (size_t)w * h;
        uint32_t k = 0;
        while (i + k < n && ord[i + k]->w == w && ord[i + k]->h == h && (k + 1) * P <= maxp) k++;
        for (uint32_t j = 0; j < k; j++) {
            const uint8_t* s = ord[i + j]->px;
            float* d = fin + (size_t)j * 3 * P;
            for (size_t p = 0; p < P; p++)
                for (int c = 0; c < 3; c++) d[c * P + p] = s[p * 3 + c] * (1.0f / 255.0f);
        }
        double tb = now();
        if (ai_gpu_run(g, (int)k, (int)w, (int)h, fin, fout)) { fprintf(stderr, "run failed\n"); return 1; }
        tgpu += now() - tb;
        vram_sample();
        size_t Q = 16 * P;
        for (uint32_t j = 0; j < k; j++) {
            uint8_t* o = ord[i + j]->out = malloc(Q * 3);
            const float* s = fout + (size_t)j * 3 * Q;
            for (size_t p = 0; p < Q; p++)
                for (int c = 0; c < 3; c++) {
                    float v = s[c * Q + p];
                    v = v < 0 ? 0 : v > 1 ? 1 : v;
                    o[p * 3 + c] = (uint8_t)(v * 255.0f + 0.5f);
                }
        }
        px_in += (uint64_t)k * P;
        batches++;
        i += k;
    }
    double t2 = now();
    f = fopen(argv[5], "wb");
    if (!f) return 1;
    uint32_t oh[2] = { 0x314F4941u, n };     /* "AIO1" */
    fwrite(oh, 4, 2, f);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d[2] = { im[i].w * 4, im[i].h * 4 };
        fwrite(d, 4, 2, f);
        fwrite(im[i].out, 1, (size_t)d[0] * d[1] * 3, f);
    }
    fclose(f);
    printf("ai_up: %u images, %llu input pixels (padded), %d batches; setup %.2f s, GPU runs %.2f s (%.1f Mpx/s in), total %.2f s\n",
           n, (unsigned long long)px_in, batches, t1 - t0, tgpu, px_in / tgpu / 1e6, t2 - t0);
    printf("ai_up: buffers %.0f MB; process video memory peak %.0f MB (%.0f MB before the net)\n", ai_gpu_bytes(g) / 1048576.0,
           g_peak / 1048576.0, base / 1048576.0);
    ai_gpu_destroy(g);
    return 0;
}
