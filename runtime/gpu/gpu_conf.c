/* Conformance of the GPU integer arithmetic against C (notes/d3d11-renderer.md 3.6 and 9).
 *
 * 1. cs_conformance runs the int64 helpers (shaders/int64.hlsli), the exact integer Z formula
 *    (row + dy*(py-by0) + dx*px + floor(dy*j/ky) + floor(dx*i/kx), >> 16, clamp) and the depth
 *    encoding on edge-case and random vectors; every output word must equal the C reference here.
 * 2. SV_Depth round trip: a pixel shader writes encode(z) into a D32S8 depth buffer; the raw depth
 *    bits (staging copy) must be z + 0x00800000 and a compute shader reading the depth SRV must
 *    decode z; a second pass with GREATER and then GREATER_EQUAL against z +- 1 must pass exactly
 *    where the integer comparison does (GS ties decide outline visibility).
 * Runs on the render thread through gpu_run_sync. */
#include "gpu_internal.h"

#include <stdlib.h>
#include <string.h>

#include "gpu_shaders.h"

typedef struct {
    uint64_t a, b, d;
    int32_t c;
    uint32_t s, k, zmax;
    int32_t by0, px, py;
    uint32_t ijk;
    uint32_t pad[2];
} conf_in;                               /* 64 bytes, = conformance.hlsl conf_in */

typedef struct {
    uint64_t add, sub, mul32, mul, asr, shl, fdiv, z16;
    uint32_t zc, flags, enc, dec;
} conf_out;                              /* 80 bytes */

_Static_assert(sizeof(conf_in) == 64, "conf_in layout");
_Static_assert(sizeof(conf_out) == 80, "conf_out layout");

/* ---- C reference (two's complement, wrapping like the GPU) ---- */

static int64_t asr64(int64_t a, unsigned s) { return s ? a >> s : a; }   /* clang: arithmetic */

static int64_t floordiv64(int64_t a, uint32_t k) {
    int64_t q = a / (int64_t)k, r = a % (int64_t)k;
    return (r != 0 && a < 0) ? q - 1 : q;
}

static uint64_t ref_z16(const conf_in* v, unsigned i, unsigned j, unsigned kx, unsigned ky) {
    uint64_t z = v->a + v->b * (uint64_t)(int64_t)(v->py - v->by0) + v->d * (uint64_t)(int64_t)v->px;
    if (j) z += (uint64_t)floordiv64((int64_t)(v->b * (uint64_t)j), ky);
    if (i) z += (uint64_t)floordiv64((int64_t)(v->d * (uint64_t)i), kx);
    return z;
}

static uint32_t ref_zclamp(uint64_t z16, uint32_t zmax) {           /* tests_batch, gs_draw_pixel.c */
    int64_t zi = (int64_t)z16 >> 16;
    return zi < 0 ? 0 : zi > (int64_t)zmax ? zmax : (uint32_t)zi;
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void ref(const conf_in* v, conf_out* o) {
    memset(o, 0, sizeof *o);
    o->add = v->a + v->b;
    o->sub = v->a - v->b;
    o->mul32 = v->a * (uint64_t)(int64_t)v->c;
    o->mul = v->a * v->b;
    o->asr = (uint64_t)asr64((int64_t)v->a, v->s & 63);
    o->shl = v->a << (v->s & 63);
    o->fdiv = (uint64_t)floordiv64((int64_t)v->a, v->k);
    unsigned i = v->ijk & 0xFF, j = (v->ijk >> 8) & 0xFF, kx = (v->ijk >> 16) & 0xFF, ky = v->ijk >> 24;
    o->z16 = ref_z16(v, i, j, kx, ky);
    o->zc = ref_zclamp(o->z16, v->zmax);
    uint32_t z1 = o->zc < 0x3F000000u ? o->zc : 0x3F000000u;
    int delta = (int)(v->s % 3) - 1;
    int64_t z2s = (int64_t)z1 + delta;
    uint32_t z2 = z2s < 0 ? 0 : z2s > 0x3F000000 ? 0x3F000000u : (uint32_t)z2s;
    /* the encoding must order exactly like the integers */
    o->flags = ((int64_t)v->a < (int64_t)v->b ? 1u : 0u) | (v->a == v->b ? 2u : 0u) | (z1 > z2 ? 4u : 0u) |
               (z1 >= z2 ? 8u : 0u) | (z1 == z2 ? 16u : 0u);
    o->enc = z1 + 0x00800000u;
    o->dec = z1;
    (void)u2f;
}

/* ---- vectors ---- */

static uint64_t rng;
static uint64_t rnd64(void) {                                         /* xorshift64* */
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1DULL;
}

static uint64_t pick64(void) {
    static const uint64_t edge[] = { 0, 1, 2, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000ull,
                                     0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
                                     0xFFFFFFFF00000000ull, 0x00000000FFFF0000ull, 0x3F0000000000ull,
                                     0xFFFFFFFFFFFF0000ull };
    switch (rnd64() % 6) {
    case 0: return edge[rnd64() % (sizeof edge / sizeof edge[0])];
    case 1: return edge[rnd64() % (sizeof edge / sizeof edge[0])] + (rnd64() % 5) - 2;
    case 2: return (uint64_t)(int64_t)(int32_t)rnd64();              /* small signed */
    case 3: return (uint64_t)((int64_t)(rnd64() >> 15) - (int64_t)(1ull << 48));   /* 16.16 Z-like */
    default: return rnd64();
    }
}

static void make_vector(conf_in* v, int n) {
    memset(v, 0, sizeof *v);
    v->a = pick64();
    v->b = pick64();
    v->d = pick64();
    switch (rnd64() % 4) {
    case 0: v->c = (int32_t)rnd64(); break;
    case 1: v->c = (int32_t)(rnd64() % 4097) - 2048; break;
    case 2: { static const int32_t e[] = { 0, 1, -1, INT32_MIN, INT32_MAX, 2047, -2047 }; v->c = e[rnd64() % 7]; break; }
    default: v->c = (int32_t)(rnd64() % 64) - 32; break;
    }
    v->s = n < 64 ? (uint32_t)n : (uint32_t)(rnd64() % 64);
    v->k = rnd64() % 3 ? (uint32_t)(rnd64() % 8) + 1 : (uint32_t)(rnd64() % 65535) + 1;
    static const uint32_t zmax[] = { 0xFFFFFFFFu, 0xFFFFFFu, 0xFFFFu };
    v->zmax = zmax[rnd64() % 3];
    v->by0 = (int32_t)(rnd64() % 2048);
    v->py = (int32_t)(rnd64() % 2048);
    v->px = (int32_t)(rnd64() % 2048);
    if (rnd64() % 8 == 0) { v->px -= 4096; v->py -= 4096; }        /* out-of-range coordinates */
    unsigned kx = (unsigned)(rnd64() % 8) + 1, ky = (unsigned)(rnd64() % 8) + 1;
    unsigned i = (unsigned)(rnd64() % kx), j = (unsigned)(rnd64() % ky);
    v->ijk = i | j << 8 | kx << 16 | ky << 24;
    /* a third of the vectors look like real Z records: 16.16 Z of up to 32 bits, modest steps */
    if (rnd64() % 3 == 0) {
        v->a = (uint64_t)(int64_t)(rnd64() % (1ull << 48)) - (rnd64() % 4 == 0 ? (1ull << 47) : 0);
        v->b = (uint64_t)((int64_t)(rnd64() % (1ull << 36)) - (int64_t)(1ull << 35));
        v->d = (uint64_t)((int64_t)(rnd64() % (1ull << 36)) - (int64_t)(1ull << 35));
    }
}

/* ---- D3D helpers ---- */

static ID3D11Buffer* buf_create(UINT bytes, UINT bind, UINT misc, UINT stride, D3D11_USAGE usage, UINT cpu,
                                const void* init) {
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes;
    bd.Usage = usage;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = cpu;
    bd.MiscFlags = misc;
    bd.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA sd = { init, 0, 0 };
    ID3D11Buffer* b = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, init ? &sd : NULL, &b))) return NULL;
    return b;
}

static void* map_read(ID3D11Resource* r, D3D11_MAPPED_SUBRESOURCE* m) {
    if (FAILED(ID3D11DeviceContext_Map(gpu_rt.ctx, r, 0, D3D11_MAP_READ, 0, m))) return NULL;
    return m->pData;
}

typedef struct { int vectors; uint32_t seed; int verbose; int result; } conf_args;

static int run_cs(const conf_args* a) {
    int n = a->vectors;
    conf_in* in = malloc(sizeof(conf_in) * (size_t)n);
    conf_out* want = malloc(sizeof(conf_out) * (size_t)n);
    if (!in || !want) { free(in); free(want); return -1; }
    rng = 0x9E3779B97F4A7C15ULL ^ a->seed;
    for (int i = 0; i < n; i++) { make_vector(&in[i], i); ref(&in[i], &want[i]); }

    int bad = -1;
    ID3D11Buffer *bin = NULL, *bout = NULL, *bst = NULL, *cb = NULL;
    ID3D11ShaderResourceView* srv = NULL;
    ID3D11UnorderedAccessView* uav = NULL;
    bin = buf_create(sizeof(conf_in) * (UINT)n, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                     sizeof(conf_in), D3D11_USAGE_DEFAULT, 0, in);
    bout = buf_create(sizeof(conf_out) * (UINT)n, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                      sizeof(conf_out), D3D11_USAGE_DEFAULT, 0, NULL);
    bst = buf_create(sizeof(conf_out) * (UINT)n, 0, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, NULL);
    uint32_t cbv[4] = { (uint32_t)n, 0, 0, 0 };
    cb = buf_create(16, D3D11_BIND_CONSTANT_BUFFER, 0, 0, D3D11_USAGE_DEFAULT, 0, cbv);
    ID3D11ComputeShader* cs = gpu_cs(GPU_SH_cs_conformance);
    if (!bin || !bout || !bst || !cb || !cs ||
        FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)bin, NULL, &srv)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)bout, NULL, &uav)))
        goto out;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11DeviceContext_CSSetShader(c, cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &cb);
    ID3D11DeviceContext_Dispatch(c, (UINT)(n + 63) / 64, 1, 1);
    ID3D11DeviceContext_ClearState(c);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)bst, (ID3D11Resource*)bout);
    D3D11_MAPPED_SUBRESOURCE m;
    const conf_out* got = map_read((ID3D11Resource*)bst, &m);
    if (!got) goto out;
    static const char* names[] = { "add", "sub", "mul32", "mul", "asr", "shl", "fdiv", "z16" };
    uint64_t field_bad[12] = { 0 };
    bad = 0;
    for (int i = 0; i < n; i++) {
        const uint64_t* g = (const uint64_t*)&got[i];
        const uint64_t* w = (const uint64_t*)&want[i];
        for (int f = 0; f < 8; f++)
            if (g[f] != w[f]) {
                if (field_bad[f]++ < 3 && a->verbose)
                    gpu_log("  %s mismatch #%d: a=%016llX b=%016llX d=%016llX c=%d s=%u k=%u ijk=%08X px=%d py=%d by0=%d: gpu %016llX c %016llX",
                            names[f], i, (unsigned long long)in[i].a, (unsigned long long)in[i].b,
                            (unsigned long long)in[i].d, in[i].c, in[i].s, in[i].k, in[i].ijk, in[i].px, in[i].py,
                            in[i].by0, (unsigned long long)g[f], (unsigned long long)w[f]);
                bad++;
            }
        const uint32_t* g32 = &got[i].zc;
        const uint32_t* w32 = &want[i].zc;
        static const char* n32[] = { "zclamp", "flags", "enc", "dec" };
        for (int f = 0; f < 4; f++)
            if (g32[f] != w32[f]) {
                if (field_bad[8 + f]++ < 3 && a->verbose)
                    gpu_log("  %s mismatch #%d: gpu %08X c %08X (zmax %08X zc %08X delta %d)", n32[f], i, g32[f], w32[f],
                            in[i].zmax, want[i].zc, (int)(in[i].s % 3) - 1);
                bad++;
            }
    }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)bst, 0);
    if (a->verbose)
        gpu_log("cs_conformance: %d vectors x 12 results, %d mismatches", n, bad);
out:
    GPU_RELEASE(uav); GPU_RELEASE(srv); GPU_RELEASE(cb); GPU_RELEASE(bst); GPU_RELEASE(bout); GPU_RELEASE(bin);
    free(in);
    free(want);
    return bad;
}

#define DW 256
#define DH 256

static int run_depth(const conf_args* a) {
    const int n = DW * DH;
    uint32_t* z1 = malloc(sizeof(uint32_t) * n);
    uint32_t* z2 = malloc(sizeof(uint32_t) * n);
    if (!z1 || !z2) { free(z1); free(z2); return -1; }
    rng = 0xD1B54A32D192ED03ULL ^ a->seed;
    static const uint32_t edge[] = { 0, 1, 2, 0xFFFF, 0x10000, 0xFFFFFF, 0x1000000, 0x3EFFFFFF, 0x3F000000, 0x7FFFFF, 0x800000 };
    for (int i = 0; i < n; i++) {
        uint32_t z;
        switch (rnd64() % 4) {
        case 0: z = edge[rnd64() % (sizeof edge / sizeof edge[0])]; break;
        case 1: z = (uint32_t)(rnd64() % 0x10000); break;              /* Z16 range: Sly's 13-17-bit Z */
        case 2: z = (uint32_t)(rnd64() % 0x1000000); break;
        default: z = (uint32_t)(rnd64() % 0x3F000001u); break;
        }
        z1[i] = z;
        int64_t t = (int64_t)z + (int64_t)(rnd64() % 3) - 1;
        z2[i] = t < 0 ? 0 : t > 0x3F000000 ? 0x3F000000u : (uint32_t)t;
    }
    int bad = -1;
    ID3D11Texture2D *dtex = NULL, *dst = NULL, *ctex = NULL, *cst = NULL;
    ID3D11DepthStencilView* dsv = NULL;
    ID3D11ShaderResourceView *dsrv = NULL, *z1srv = NULL, *z2srv = NULL;
    ID3D11RenderTargetView* crtv = NULL;
    ID3D11Buffer *bz1 = NULL, *bz2 = NULL, *bout = NULL, *bst = NULL, *cb = NULL;
    ID3D11UnorderedAccessView* ouav = NULL;
    ID3D11DepthStencilState *ds_always = NULL, *ds_gt = NULL, *ds_ge = NULL;
    ID3D11Device* d = gpu_rt.dev;
    ID3D11DeviceContext* c = gpu_rt.ctx;

    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = DW; td.Height = DH; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(d, &td, NULL, &dtex))) goto out;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(d, &td, NULL, &dst))) goto out;
    td.Format = DXGI_FORMAT_R32_UINT; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.CPUAccessFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(d, &td, NULL, &ctex))) goto out;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(d, &td, NULL, &cst))) goto out;
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
    memset(&dvd, 0, sizeof dvd);
    dvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateDepthStencilView(d, (ID3D11Resource*)dtex, &dvd, &dsv))) goto out;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    memset(&svd, 0, sizeof svd);
    svd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource*)dtex, &svd, &dsrv))) goto out;
    if (FAILED(ID3D11Device_CreateRenderTargetView(d, (ID3D11Resource*)ctex, NULL, &crtv))) goto out;
    bz1 = buf_create(4 * n, D3D11_BIND_SHADER_RESOURCE, 0, 0, D3D11_USAGE_DEFAULT, 0, z1);
    bz2 = buf_create(4 * n, D3D11_BIND_SHADER_RESOURCE, 0, 0, D3D11_USAGE_DEFAULT, 0, z2);
    bout = buf_create(4 * n, D3D11_BIND_UNORDERED_ACCESS, 0, 0, D3D11_USAGE_DEFAULT, 0, NULL);
    bst = buf_create(4 * n, 0, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, NULL);
    uint32_t cbv[4] = { 0, DW, 0, 0 };
    cb = buf_create(16, D3D11_BIND_CONSTANT_BUFFER, 0, 0, D3D11_USAGE_DEFAULT, 0, cbv);
    if (!bz1 || !bz2 || !bout || !bst || !cb) goto out;
    memset(&svd, 0, sizeof svd);
    svd.Format = DXGI_FORMAT_R32_UINT;
    svd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    svd.Buffer.NumElements = (UINT)n;
    if (FAILED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource*)bz1, &svd, &z1srv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource*)bz2, &svd, &z2srv)))
        goto out;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uvd;
    memset(&uvd, 0, sizeof uvd);
    uvd.Format = DXGI_FORMAT_R32_UINT;
    uvd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uvd.Buffer.NumElements = (UINT)n;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(d, (ID3D11Resource*)bout, &uvd, &ouav))) goto out;
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(d, &dd, &ds_always))) goto out;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_GREATER;
    if (FAILED(ID3D11Device_CreateDepthStencilState(d, &dd, &ds_gt))) goto out;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    if (FAILED(ID3D11Device_CreateDepthStencilState(d, &dd, &ds_ge))) goto out;
    ID3D11VertexShader* vs = gpu_vs(GPU_SH_vs_fullscreen);
    ID3D11PixelShader* psw = gpu_ps(GPU_SH_ps_depth_write);
    ID3D11PixelShader* pst = gpu_ps(GPU_SH_ps_depth_test);
    ID3D11ComputeShader* csr = gpu_cs(GPU_SH_cs_depth_read);
    if (!vs || !psw || !pst || !csr) goto out;

    /* pass 1: write encode(z1) */
    D3D11_VIEWPORT vp = { 0, 0, DW, DH, 0.0f, 1.0f };
    ID3D11DeviceContext_ClearState(c);
    ID3D11DeviceContext_ClearDepthStencilView(c, dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(c, ds_always, 0);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, psw, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &z1srv);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &cb);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)dst, (ID3D11Resource*)dtex);
    /* compute read of the depth SRV */
    ID3D11DeviceContext_CSSetShader(c, csr, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 1, 1, &dsrv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &ouav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &cb);
    ID3D11DeviceContext_Dispatch(c, DW / 8, DH / 8, 1);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)bst, (ID3D11Resource*)bout);
    ID3D11DeviceContext_ClearState(c);

    int bad_raw = 0, bad_srv = 0, bad_gt = 0, bad_ge = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (!map_read((ID3D11Resource*)dst, &m)) goto out;
    for (int y = 0; y < DH; y++)
        for (int x = 0; x < DW; x++) {
            uint32_t raw;
            memcpy(&raw, (const uint8_t*)m.pData + (size_t)y * m.RowPitch + (size_t)x * 8, 4);
            uint32_t want = z1[y * DW + x] + 0x00800000u;
            if (raw != want) {
                if (bad_raw++ < 3 && a->verbose) gpu_log("  depth raw (%d,%d): %08X want %08X", x, y, raw, want);
            }
        }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)dst, 0);
    if (!map_read((ID3D11Resource*)bst, &m)) goto out;
    for (int i = 0; i < n; i++) {
        uint32_t g = ((const uint32_t*)m.pData)[i];
        if (g != z1[i] && bad_srv++ < 3 && a->verbose) gpu_log("  depth SRV decode #%d: %08X want %08X", i, g, z1[i]);
    }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)bst, 0);

    /* passes 2 and 3: encode(z2) against encode(z1) with GREATER, then GREATER_EQUAL */
    for (int pass = 0; pass < 2; pass++) {
        static const UINT zero[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearUnorderedAccessViewUint(c, ouav, zero);   /* (keeps the UAV path warm) */
        static const float clr[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(c, crtv, clr);
        ID3D11DeviceContext_OMSetRenderTargets(c, 1, &crtv, dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(c, pass ? ds_ge : ds_gt, 0);
        ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, pst, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &z2srv);
        ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &cb);
        ID3D11DeviceContext_Draw(c, 3, 0);
        ID3D11DeviceContext_ClearState(c);
        ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)cst, (ID3D11Resource*)ctex);
        if (!map_read((ID3D11Resource*)cst, &m)) goto out;
        for (int y = 0; y < DH; y++)
            for (int x = 0; x < DW; x++) {
                uint32_t g = ((const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch))[x];
                int i = y * DW + x;
                uint32_t want = pass ? z2[i] >= z1[i] : z2[i] > z1[i];
                if (g != want) {
                    int* b = pass ? &bad_ge : &bad_gt;
                    if ((*b)++ < 3 && a->verbose)
                        gpu_log("  depth %s (%d,%d): z1 %08X z2 %08X: passed %u want %u", pass ? "GEQUAL" : "GREATER", x, y, z1[i], z2[i], g, want);
                }
            }
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)cst, 0);
    }
    bad = bad_raw + bad_srv + bad_gt + bad_ge;
    if (a->verbose)
        gpu_log("depth round trip (%dx%d): raw D32 bits %d, SRV decode %d, GREATER ties %d, GEQUAL ties %d mismatches",
                DW, DH, bad_raw, bad_srv, bad_gt, bad_ge);
out:
    GPU_RELEASE(ds_ge); GPU_RELEASE(ds_gt); GPU_RELEASE(ds_always); GPU_RELEASE(ouav); GPU_RELEASE(z2srv);
    GPU_RELEASE(z1srv); GPU_RELEASE(cb); GPU_RELEASE(bst); GPU_RELEASE(bout); GPU_RELEASE(bz2); GPU_RELEASE(bz1);
    GPU_RELEASE(crtv); GPU_RELEASE(dsrv); GPU_RELEASE(dsv); GPU_RELEASE(cst); GPU_RELEASE(ctex); GPU_RELEASE(dst);
    GPU_RELEASE(dtex);
    free(z1);
    free(z2);
    return bad;
}

static void conf_fn(void* p) {
    conf_args* a = p;
    int r1 = run_cs(a);
    int r2 = run_depth(a);
    a->result = r1 < 0 || r2 < 0 ? -1 : r1 + r2;
}

int gpu_conformance(int vectors, uint32_t seed, int verbose) {
    if (!gpu_running()) return -1;
    conf_args a = { vectors > 0 ? vectors : 1 << 16, seed, verbose, -1 };
    gpu_run_sync(conf_fn, &a);
    return a.result;
}
