/* Conformance of the GPU integer arithmetic and of the depth path against C (notes/d3d11-renderer.md 3.6, 9 and
 * appendix P).
 *
 * 1. cs_conformance runs the int64 helpers (shaders/int64.hlsli), the exact integer Z formula
 *    (row + dy*(py-by0) + dx*px + floor(dy*j/ky) + floor(dx*i/kx), >> 16, clamp), the depth encoding
 *    enc(z) = min(z, 2^20) / 2^20 and the early-depth bounds on edge-case and random vectors; every output word must
 *    equal the C reference here.
 * 2. (P1) Depth round trip through a D32 depth buffer (the targets' format), every pixel its own point: a pixel shader
 *    writes enc(z) as SV_DepthLessEqual (any 32-bit Z: values >= 2^20 saturate); the raw depth bits (staging copy)
 *    must be enc(z) and a compute shader reading the depth SRV must decode min(z, 2^20); then FF-like fragments
 *    (z < 2^20, rasterized depth = the production bound z + 1) with GREATER, GREATER_EQUAL and GREATER_EQUAL + write
 *    must pass exactly where the integer comparison with the TRUE stored value does (GS ties decide outline
 *    visibility; saturated values are exact against FF fragments) and leave exactly the expected depth.
 * 3. (P1) The DSV <-> zpatch sync (shaders/zsync.hlsl) for Z16, Z24 and Z32: zpatch words (Z24 with garbage in the top
 *    byte, values >= 2^20, the format limits) resolved into the DSV (enc of the masked word), FF-like depth writes over
 *    half the pixels, then cs_zsync back: zpatch must hold the written Z (Z24: under the old top byte) where written and
 *    the old word, bit for bit, elsewhere (including the saturated ones); a constant resolve (partial depth clears).
 * 4. (P1) The triangle bound: random, sliver, tiny and steep triangles set up exactly as the frontend does (gs_setup.c),
 *    drawn at several sample factors through the REF viewport with the frontend's bound decision (gpu_front.h); every
 *    covered sample's exact Z (the FF pixel shader's formula) must be <= the rasterized depth (else SV_DepthLessEqual
 *    would clamp it). The minimum slack is reported; triangles beyond the slope limit are also drawn with the vertex
 *    bound (not counted as failures) to show what the limit guards against.
 * Runs on the render thread through gpu_run_sync. */
#include "gpu_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shaders.h"
#include "gs_setup.h"

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
    uint32_t zc, flags, enc, dec, bvtx, bconst, pad[2];
} conf_out;                              /* 96 bytes */

_Static_assert(sizeof(conf_in) == 64, "conf_in layout");
_Static_assert(sizeof(conf_out) == 96, "conf_out layout");

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

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t zsat(uint32_t z) { return z < GPU_ZE_ONE ? z : GPU_ZE_ONE; }
/* the bounds of int64.hlsli (gs_z_bound_vtx / gs_z_bound_const), exact in C */
static float bound_vtx(uint32_t z) {
    return (float)((z < GPU_ZE_ONE - GPU_ZB_MARGIN ? z : GPU_ZE_ONE - GPU_ZB_MARGIN) + GPU_ZB_MARGIN) * (1.0f / 1048576.0f);
}
static float bound_const(uint32_t z) { return (float)((z < GPU_ZE_ONE - 1u ? z : GPU_ZE_ONE - 1u) + 1u) * (1.0f / 1048576.0f); }

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
    uint32_t z1 = o->zc;
    int delta = (int)(v->s % 3) - 1;
    uint32_t z2 = delta < 0 ? (z1 ? z1 - 1 : 0) : delta > 0 ? (z1 < 0xFFFFFFFFu ? z1 + 1 : z1) : z1;
    /* the encoding orders exactly like the saturated integers min(z, 2^20) */
    uint32_t s1 = zsat(z1), s2 = zsat(z2);
    o->flags = ((int64_t)v->a < (int64_t)v->b ? 1u : 0u) | (v->a == v->b ? 2u : 0u) | (s1 > s2 ? 4u : 0u) |
               (s1 >= s2 ? 8u : 0u) | (s1 == s2 ? 16u : 0u);
    o->enc = f2u(gpu_z_encode(z1));
    o->dec = s1;
    o->bvtx = f2u(bound_vtx(z1));
    o->bconst = f2u(bound_const(z1));
}

/* ---- vectors ---- */

static uint64_t rng;
static uint64_t rnd64(void) {                                         /* xorshift64* */
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1DULL;
}
static double rndu(void) { return (double)(rnd64() >> 11) * (1.0 / 9007199254740992.0); }   /* [0, 1) */

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

/* (P1) Z values around the encoding's limits */
static uint32_t pick_z_edge(void) {
    static const uint32_t e[] = { 0, 1, 2, 0x7F5C3, 0xFFFF, 0x10000, GPU_Z_FF_MAX, GPU_Z_FF_MAX + 1, GPU_ZE_ONE - GPU_ZB_MARGIN,
                                  GPU_ZE_ONE - 2, GPU_ZE_ONE - 1, GPU_ZE_ONE, GPU_ZE_ONE + 1, 0xFFFFFF, 0x1000000, 0x3F000000,
                                  0x7FFFFF, 0x800000, 0xFFFFFFFE, 0xFFFFFFFF };
    return e[rnd64() % (sizeof e / sizeof e[0])];
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
    /* (P1) an eighth land on the encoding's limits: a constant Z record of an edge value (or one next to it) */
    if (rnd64() % 8 == 0) {
        uint32_t z = pick_z_edge();
        v->a = (uint64_t)z << 16 | (rnd64() % 65536);
        v->b = v->d = 0;
        v->zmax = 0xFFFFFFFFu;
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

static ID3D11Texture2D* tex_create(UINT w, UINT h, DXGI_FORMAT f, UINT bind, D3D11_USAGE usage, UINT cpu) {
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = f; td.Usage = usage; td.BindFlags = bind; td.CPUAccessFlags = cpu;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return NULL;
    return t;
}

static ID3D11DepthStencilState* ds_create(D3D11_COMPARISON_FUNC f, int write) {
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = f;
    ID3D11DepthStencilState* s = NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilState(gpu_rt.dev, &dd, &s))) return NULL;
    return s;
}

/* a D32 depth target as the executor creates them (gpu_exec.c target_views): R32_TYPELESS, DSV D32_FLOAT, SRV R32_FLOAT */
typedef struct { ID3D11Texture2D *tex, *st; ID3D11DepthStencilView* dsv; ID3D11ShaderResourceView* srv; } dtarget;

static int dtarget_create(dtarget* t, UINT w, UINT h) {
    memset(t, 0, sizeof *t);
    t->tex = tex_create(w, h, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
    t->st = tex_create(w, h, DXGI_FORMAT_R32_TYPELESS, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!t->tex || !t->st) return 0;
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd;
    memset(&dvd, 0, sizeof dvd);
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)t->tex, &dvd, &t->dsv))) return 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    memset(&svd, 0, sizeof svd);
    svd.Format = DXGI_FORMAT_R32_FLOAT;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    return SUCCEEDED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t->tex, &svd, &t->srv));
}

static void dtarget_free(dtarget* t) { GPU_RELEASE(t->srv); GPU_RELEASE(t->dsv); GPU_RELEASE(t->st); GPU_RELEASE(t->tex); }

/* the raw 32-bit words of a w x h R32 texture (through its staging copy) -> out */
static int read_words(ID3D11Texture2D* tex, ID3D11Texture2D* st, int w, int h, uint32_t* out) {
    ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)st, (ID3D11Resource*)tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (!map_read((ID3D11Resource*)st, &m)) return 0;
    for (int y = 0; y < h; y++) memcpy(out + (size_t)y * w, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)st, 0);
    return 1;
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
    uint64_t field_bad[14] = { 0 };
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
        static const char* n32[] = { "zclamp", "flags", "enc", "dec", "bound_vtx", "bound_const" };
        for (int f = 0; f < 6; f++)
            if (g32[f] != w32[f]) {
                if (field_bad[8 + f]++ < 3 && a->verbose)
                    gpu_log("  %s mismatch #%d: gpu %08X c %08X (zmax %08X zc %08X delta %d)", n32[f], i, g32[f], w32[f],
                            in[i].zmax, want[i].zc, (int)(in[i].s % 3) - 1);
                bad++;
            }
    }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)bst, 0);
    if (a->verbose)
        gpu_log("cs_conformance: %d vectors x 14 results, %d mismatches", n, bad);
out:
    GPU_RELEASE(uav); GPU_RELEASE(srv); GPU_RELEASE(cb); GPU_RELEASE(bst); GPU_RELEASE(bout); GPU_RELEASE(bin);
    free(in);
    free(want);
    return bad;
}

#define DW 256
#define DH 256

/* the point-per-pixel pass: vs_conf_px (rasterized depth 1.0, or mode 1: the constant bound of zin) + ps */
static void px_pass(ID3D11ShaderResourceView* zsrv, ID3D11Buffer* cb, ID3D11PixelShader* ps, ID3D11DepthStencilState* ds,
                    ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    D3D11_VIEWPORT vp = { 0, 0, DW, DH, 0.0f, 1.0f };
    ID3D11DeviceContext_OMSetRenderTargets(c, rtv ? 1 : 0, rtv ? &rtv : NULL, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(c, ds, 0);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(c, gpu_vs(GPU_SH_vs_conf_px), NULL, 0);
    ID3D11DeviceContext_VSSetShaderResources(c, 0, 1, &zsrv);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &zsrv);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &cb);
    ID3D11DeviceContext_Draw(c, DW * DH, 0);
    ID3D11DeviceContext_ClearState(c);
}

static ID3D11Buffer* zbuf_create(const uint32_t* z, ID3D11ShaderResourceView** srv) {
    ID3D11Buffer* b = buf_create(4 * DW * DH, D3D11_BIND_SHADER_RESOURCE, 0, 0, D3D11_USAGE_DEFAULT, 0, z);
    if (!b) return NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    memset(&svd, 0, sizeof svd);
    svd.Format = DXGI_FORMAT_R32_UINT;
    svd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    svd.Buffer.NumElements = DW * DH;
    if (FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)b, &svd, srv))) { GPU_RELEASE(b); return NULL; }
    return b;
}

static ID3D11Buffer* cb4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t v[4] = { a, b, c, d };
    return buf_create(16, D3D11_BIND_CONSTANT_BUFFER, 0, 0, D3D11_USAGE_DEFAULT, 0, v);
}

static uint32_t pick_z_any(void) {
    switch (rnd64() % 6) {
    case 0: return pick_z_edge();
    case 1: return (uint32_t)(rnd64() % 0x10000);                     /* Z16 range */
    case 2: return (uint32_t)(rnd64() % 0x80000);                     /* Sly's Z (< 2^19) */
    case 3: return (uint32_t)(rnd64() % (GPU_ZE_ONE + 4));             /* the encoding's whole exact range */
    case 4: return (uint32_t)(rnd64() % 0x1000000);                   /* Z24 */
    default: return (uint32_t)rnd64();                                /* Z32 */
    }
}

/* 2: the round trip and the ties */
static int run_depth(const conf_args* a) {
    const int n = DW * DH;
    uint32_t* z1 = malloc(sizeof(uint32_t) * n);
    uint32_t* z2 = malloc(sizeof(uint32_t) * n);
    uint32_t* got = malloc(sizeof(uint32_t) * n);
    if (!z1 || !z2 || !got) { free(z1); free(z2); free(got); return -1; }
    rng = 0xD1B54A32D192ED03ULL ^ a->seed;
    for (int i = 0; i < n; i++) {
        uint32_t z = pick_z_any();
        z1[i] = z;
        /* an FF fragment: always < 2^20 (gpu_front.h); ties and neighbours of the stored value where it can be one */
        uint32_t f;
        if (z < GPU_ZE_ONE) {
            int64_t t = (int64_t)z + (int64_t)(rnd64() % 3) - 1;
            f = t < 0 ? 0 : t > GPU_ZE_ONE - 1 ? GPU_ZE_ONE - 1 : (uint32_t)t;
        } else f = rnd64() % 2 ? GPU_ZE_ONE - 1 - (uint32_t)(rnd64() % 3) : (uint32_t)(rnd64() % GPU_ZE_ONE);
        z2[i] = f;
    }
    int bad = -1;
    dtarget dt;
    ID3D11Texture2D *ctex = NULL, *cst = NULL;
    ID3D11RenderTargetView* crtv = NULL;
    ID3D11ShaderResourceView *z1srv = NULL, *z2srv = NULL;
    ID3D11Buffer *bz1 = NULL, *bz2 = NULL, *bout = NULL, *bst = NULL, *cb0 = NULL, *cb1 = NULL;
    ID3D11UnorderedAccessView* ouav = NULL;
    ID3D11DepthStencilState *ds_always = NULL, *ds_gt = NULL, *ds_ge = NULL, *ds_gew = NULL;
    ID3D11Device* d = gpu_rt.dev;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    if (!dtarget_create(&dt, DW, DH)) goto out;
    ctex = tex_create(DW, DH, DXGI_FORMAT_R32_UINT, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
    cst = tex_create(DW, DH, DXGI_FORMAT_R32_UINT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!ctex || !cst || FAILED(ID3D11Device_CreateRenderTargetView(d, (ID3D11Resource*)ctex, NULL, &crtv))) goto out;
    bz1 = zbuf_create(z1, &z1srv);
    bz2 = zbuf_create(z2, &z2srv);
    bout = buf_create(4 * n, D3D11_BIND_UNORDERED_ACCESS, 0, 0, D3D11_USAGE_DEFAULT, 0, NULL);
    bst = buf_create(4 * n, 0, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, NULL);
    cb0 = cb4(0, DW, 0, 0);
    cb1 = cb4(0, DW, 1, 0);
    if (!bz1 || !bz2 || !bout || !bst || !cb0 || !cb1) goto out;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uvd;
    memset(&uvd, 0, sizeof uvd);
    uvd.Format = DXGI_FORMAT_R32_UINT;
    uvd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uvd.Buffer.NumElements = (UINT)n;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(d, (ID3D11Resource*)bout, &uvd, &ouav))) goto out;
    ds_always = ds_create(D3D11_COMPARISON_ALWAYS, 1);
    ds_gt = ds_create(D3D11_COMPARISON_GREATER, 0);
    ds_ge = ds_create(D3D11_COMPARISON_GREATER_EQUAL, 0);
    ds_gew = ds_create(D3D11_COMPARISON_GREATER_EQUAL, 1);
    ID3D11PixelShader* psw = gpu_ps(GPU_SH_ps_depth_write);
    ID3D11PixelShader* pst = gpu_ps(GPU_SH_ps_depth_test);
    ID3D11ComputeShader* csr = gpu_cs(GPU_SH_cs_depth_read);
    if (!ds_always || !ds_gt || !ds_ge || !ds_gew || !psw || !pst || !csr || !gpu_vs(GPU_SH_vs_conf_px)) goto out;

    /* pass 1: write enc(z1) (rasterized depth 1.0: any value) */
    ID3D11DeviceContext_ClearState(c);
    ID3D11DeviceContext_ClearDepthStencilView(c, dt.dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    px_pass(z1srv, cb0, psw, ds_always, NULL, dt.dsv);
    int bad_raw = 0, bad_srv = 0, bad_t[3] = { 0, 0, 0 }, bad_w = 0;
    if (!read_words(dt.tex, dt.st, DW, DH, got)) goto out;
    for (int i = 0; i < n; i++) {
        uint32_t want = f2u(gpu_z_encode(z1[i]));
        if (got[i] != want && bad_raw++ < 3 && a->verbose) gpu_log("  depth raw #%d: z %08X: %08X want %08X", i, z1[i], got[i], want);
    }
    /* compute read of the depth SRV */
    ID3D11DeviceContext_CSSetShader(c, csr, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 1, 1, &dt.srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &ouav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &cb0);
    ID3D11DeviceContext_Dispatch(c, DW / 8, DH / 8, 1);
    ID3D11DeviceContext_ClearState(c);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)bst, (ID3D11Resource*)bout);
    D3D11_MAPPED_SUBRESOURCE m;
    if (!map_read((ID3D11Resource*)bst, &m)) goto out;
    for (int i = 0; i < n; i++) {
        uint32_t g = ((const uint32_t*)m.pData)[i];
        if (g != zsat(z1[i]) && bad_srv++ < 3 && a->verbose) gpu_log("  depth SRV decode #%d: %08X want %08X", i, g, zsat(z1[i]));
    }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)bst, 0);

    /* passes 2-4: FF fragments enc(z2) under the production bound z2 + 1 against the stored z1: GREATER, GREATER_EQUAL,
     * then GREATER_EQUAL with the Z write (the depth after it: enc(z2) where it passed, else the stored value) */
    static const char* tn[3] = { "GREATER", "GEQUAL", "GEQUAL+write" };
    for (int pass = 0; pass < 3; pass++) {
        static const float clr[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(c, crtv, clr);
        px_pass(z2srv, cb1, pst, pass == 0 ? ds_gt : pass == 1 ? ds_ge : ds_gew, crtv, dt.dsv);
        if (!read_words(ctex, cst, DW, DH, got)) goto out;
        for (int i = 0; i < n; i++) {
            uint32_t want = pass ? z2[i] >= z1[i] : z2[i] > z1[i];   /* (the TRUE stored value: saturation is exact here) */
            if (got[i] != want && bad_t[pass]++ < 3 && a->verbose)
                gpu_log("  depth %s #%d: stored %08X fragment %08X: passed %u want %u", tn[pass], i, z1[i], z2[i], got[i], want);
        }
    }
    if (!read_words(dt.tex, dt.st, DW, DH, got)) goto out;
    for (int i = 0; i < n; i++) {
        uint32_t want = f2u(gpu_z_encode(z2[i] >= z1[i] ? z2[i] : z1[i]));
        if (got[i] != want && bad_w++ < 3 && a->verbose)
            gpu_log("  depth after GEQUAL+write #%d: stored %08X fragment %08X: %08X want %08X", i, z1[i], z2[i], got[i], want);
    }
    bad = bad_raw + bad_srv + bad_t[0] + bad_t[1] + bad_t[2] + bad_w;
    if (a->verbose)
        gpu_log("depth round trip (%dx%d, D32, SV_DepthLessEqual): raw bits %d, SRV decode %d, GREATER ties %d, GEQUAL ties %d, "
                "GEQUAL+write %d / depth after %d mismatches", DW, DH, bad_raw, bad_srv, bad_t[0], bad_t[1], bad_t[2], bad_w);
out:
    ID3D11DeviceContext_ClearState(c);
    GPU_RELEASE(ds_gew); GPU_RELEASE(ds_ge); GPU_RELEASE(ds_gt); GPU_RELEASE(ds_always); GPU_RELEASE(ouav); GPU_RELEASE(z2srv);
    GPU_RELEASE(z1srv); GPU_RELEASE(cb1); GPU_RELEASE(cb0); GPU_RELEASE(bst); GPU_RELEASE(bout); GPU_RELEASE(bz2); GPU_RELEASE(bz1);
    GPU_RELEASE(crtv); GPU_RELEASE(cst); GPU_RELEASE(ctex);
    dtarget_free(&dt);
    free(z1);
    free(z2);
    free(got);
    return bad;
}

/* 3: the DSV <-> zpatch sync of shaders/zsync.hlsl (zs_cb = gpu_exec.c zs_consts) */
typedef struct { uint32_t kx, ky, tiles_x, mode, zbits, value, x0, y0, x1, y1, pad[2]; } zs_cb;
_Static_assert(sizeof(zs_cb) == 48, "zsync.hlsl zs_cb");

static int run_zsync(const conf_args* a) {
    const int n = DW * DH, tiles = (DW / 16) * (DH / 16);
    uint32_t* w = malloc(sizeof(uint32_t) * n);
    uint32_t* z3 = malloc(sizeof(uint32_t) * n);
    uint32_t* got = malloc(sizeof(uint32_t) * n);
    if (!w || !z3 || !got) { free(w); free(z3); free(got); return -1; }
    int bad = -1, bad_res = 0, bad_sync = 0, bad_const = 0;
    dtarget dt;
    ID3D11Texture2D *zp = NULL, *zpst = NULL;
    ID3D11ShaderResourceView *zp_srv = NULL, *tile_srv = NULL, *z3srv = NULL;
    ID3D11UnorderedAccessView* zp_uav = NULL;
    ID3D11Buffer *tile = NULL, *zcb = NULL, *bz3 = NULL, *cb1 = NULL;
    ID3D11DepthStencilState* ds_always = NULL;
    ID3D11Device* d = gpu_rt.dev;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    if (!dtarget_create(&dt, DW, DH)) goto out;
    zp = tex_create(DW, DH, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
    zpst = tex_create(DW, DH, DXGI_FORMAT_R32_UINT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!zp || !zpst || FAILED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource*)zp, NULL, &zp_srv)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(d, (ID3D11Resource*)zp, NULL, &zp_uav)))
        goto out;
    tile = buf_create((UINT)tiles, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, D3D11_USAGE_DEFAULT, 0, NULL);
    zcb = buf_create(sizeof(zs_cb), D3D11_BIND_CONSTANT_BUFFER, 0, 0, D3D11_USAGE_DEFAULT, 0, NULL);
    cb1 = cb4(0, DW, 1, 0);
    ds_always = ds_create(D3D11_COMPARISON_ALWAYS, 1);
    if (!tile || !zcb || !cb1 || !ds_always) goto out;
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = (UINT)tiles / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource*)tile, &sd, &tile_srv))) goto out;
    }
    ID3D11VertexShader* vsq = gpu_vs(GPU_SH_vs_zquad);
    ID3D11PixelShader* psr = gpu_ps(GPU_SH_ps_zresolve);
    ID3D11ComputeShader* css = gpu_cs(GPU_SH_cs_zsync);
    ID3D11PixelShader* psf = gpu_ps(GPU_SH_ps_depth_ff);
    if (!vsq || !psr || !css || !psf || !gpu_vs(GPU_SH_vs_conf_px)) goto out;
    uint8_t states[DW / 16 * DH / 16];
    D3D11_VIEWPORT vp = { 0, 0, DW, DH, 0.0f, 1.0f };
    rng = 0xA0761D6478BD642FULL ^ a->seed;
    for (uint32_t zbits = 1; zbits <= 3; zbits++) {
        uint32_t mask = zbits == 1 ? 0xFFFFu : zbits == 2 ? 0xFFFFFFu : 0xFFFFFFFFu;
        for (int i = 0; i < n; i++) {
            uint32_t v = pick_z_any();
            if (zbits == 1) v &= 0xFFFFu;                                 /* (Z16 words: 16 bits) */
            else if (zbits == 2) v = (v & 0xFFFFFFu) | (uint32_t)(rnd64() & 0xFF) << 24;   /* garbage in the top byte */
            w[i] = v;
            uint32_t f = 0xFFFFFFFFu;                                    /* half the pixels get an FF depth write */
            if (rnd64() % 2) {
                f = (uint32_t)(rnd64() % 3 == 0 ? pick_z_edge() : rnd64());
                f = f % (zbits == 1 ? 0x10000u : GPU_ZE_ONE);              /* (FF fragments: < 2^20, Z16 <= 0xFFFF) */
            }
            z3[i] = f;
        }
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)zp, 0, NULL, w, DW * 4, 0);
        GPU_RELEASE(z3srv); GPU_RELEASE(bz3);
        bz3 = zbuf_create(z3, &z3srv);
        if (!bz3) goto out;
        zs_cb k;
        memset(&k, 0, sizeof k);
        k.kx = k.ky = 1; k.tiles_x = DW / 16; k.zbits = zbits;
        k.x0 = 0; k.y0 = 0; k.x1 = DW; k.y1 = DH;
        /* a) every tile zpatch-newer: ps_zresolve -> the DSV holds enc(word & mask) */
        memset(states, 2, sizeof states);
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)tile, 0, NULL, states, 0, 0);
        k.mode = 0;
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)zcb, 0, NULL, &k, 0, 0);
        ID3D11DeviceContext_ClearDepthStencilView(c, dt.dsv, D3D11_CLEAR_DEPTH, 0.5f, 0);
        ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, dt.dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(c, ds_always, 0);
        ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11DeviceContext_VSSetShader(c, vsq, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, psr, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &zcb);
        ID3D11DeviceContext_PSSetShaderResources(c, 2, 1, &zp_srv);
        ID3D11DeviceContext_PSSetShaderResources(c, 8, 1, &tile_srv);
        ID3D11DeviceContext_Draw(c, 4, 0);
        ID3D11DeviceContext_ClearState(c);
        if (!read_words(dt.tex, dt.st, DW, DH, got)) goto out;
        for (int i = 0; i < n; i++) {
            uint32_t want = f2u(gpu_z_encode(w[i] & mask));
            if (got[i] != want && bad_res++ < 3 && a->verbose)
                gpu_log("  zresolve Z%u #%d: word %08X: DSV %08X want %08X", zbits == 1 ? 16 : zbits == 2 ? 24 : 32, i, w[i], got[i], want);
        }
        /* b) FF depth writes over half the pixels, the tiles DSV-newer: cs_zsync back into zpatch */
        px_pass(z3srv, cb1, psf, ds_always, NULL, dt.dsv);
        memset(states, 1, sizeof states);
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)tile, 0, NULL, states, 0, 0);
        ID3D11DeviceContext_CSSetShader(c, css, NULL, 0);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &dt.srv);
        ID3D11DeviceContext_CSSetShaderResources(c, 8, 1, &tile_srv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &zp_uav, NULL);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &zcb);
        ID3D11DeviceContext_Dispatch(c, DW / 8, DH / 8, 1);
        ID3D11DeviceContext_ClearState(c);
        if (!read_words(zp, zpst, DW, DH, got)) goto out;
        for (int i = 0; i < n; i++) {
            uint32_t want = z3[i] == 0xFFFFFFFFu ? w[i] : zbits == 2 ? (w[i] & 0xFF000000u) | z3[i] : z3[i];
            if (got[i] != want && bad_sync++ < 3 && a->verbose)
                gpu_log("  zsync Z%u #%d: word %08X, FF write %08X: zpatch %08X want %08X", zbits == 1 ? 16 : zbits == 2 ? 24 : 32, i,
                        w[i], z3[i], got[i], want);
        }
        /* c) a partial depth clear: the constant resolve (mode 1) of a masked value */
        static const uint32_t cv[] = { 0, 1, 0x7F5C3, 0xFFFF, GPU_ZE_ONE - 1, GPU_ZE_ONE, GPU_ZE_ONE + 1, 0xFFFFFF, 0xFFFFFFFF };
        for (size_t q = 0; q < sizeof cv / sizeof cv[0]; q++) {
            k.mode = 1;
            k.value = cv[q] & mask;
            ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)zcb, 0, NULL, &k, 0, 0);
            ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, dt.dsv);
            ID3D11DeviceContext_OMSetDepthStencilState(c, ds_always, 0);
            ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
            ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ID3D11DeviceContext_VSSetShader(c, vsq, NULL, 0);
            ID3D11DeviceContext_PSSetShader(c, psr, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &zcb);
            ID3D11DeviceContext_PSSetShaderResources(c, 2, 1, &zp_srv);
            ID3D11DeviceContext_PSSetShaderResources(c, 8, 1, &tile_srv);
            ID3D11DeviceContext_Draw(c, 4, 0);
            ID3D11DeviceContext_ClearState(c);
            if (!read_words(dt.tex, dt.st, DW, DH, got)) goto out;
            uint32_t want = f2u(gpu_z_encode(k.value));
            int wrong = 0;
            for (int i = 0; i < n; i++) wrong += got[i] != want;
            if (wrong && bad_const++ < 3 && a->verbose) gpu_log("  zresolve constant %08X: %d pixels differ (want %08X)", k.value, wrong, want);
        }
    }
    bad = bad_res + bad_sync + bad_const;
    if (a->verbose)
        gpu_log("depth sync (zsync.hlsl, Z16 / Z24 / Z32, %dx%d): zpatch -> DSV %d, FF writes + DSV -> zpatch %d, constant resolves %d mismatches",
                DW, DH, bad_res, bad_sync, bad_const);
out:
    ID3D11DeviceContext_ClearState(c);
    GPU_RELEASE(ds_always); GPU_RELEASE(cb1); GPU_RELEASE(z3srv); GPU_RELEASE(bz3); GPU_RELEASE(zcb); GPU_RELEASE(tile_srv);
    GPU_RELEASE(tile); GPU_RELEASE(zp_uav); GPU_RELEASE(zp_srv); GPU_RELEASE(zpst); GPU_RELEASE(zp);
    dtarget_free(&dt);
    free(w);
    free(z3);
    free(got);
    return bad;
}

/* 4: the triangle bound (conformance.hlsl vs_conf_tri / ps_conf_tri) */
typedef struct { int64_t row, dy, dx; int32_t by0; uint32_t zmax; } tri_rec;   /* = conformance.hlsl tri_rec */
typedef struct { float x, y; uint32_t z; } tri_vtx;
_Static_assert(sizeof(tri_rec) == 32, "tri_rec layout");

#define TB_RT 4096                       /* render target (samples): 640 x 448 native at up to 6 x 9 */

/* a GS vertex coordinate (12.4) in [lo, hi) native pixels */
static int32_t tb_coord(double lo, double hi) { return (int32_t)floor((lo + (hi - lo) * rndu()) * 16.0); }

/* one triangle as the frontend sets it up; 0: not drawn (zero area, outside the scissor) or not an FF depth triangle
 * (a Z24 / Z32 vertex above GPU_Z_FF_MAX goes to the ROV tier). *nb: the frontend's decision (gpu_front.c) */
static int tb_make(gs_vtx v[3], tri_rec* r, int* nb, int* steep, double* slope, int small) {
    memset(v, 0, sizeof(gs_vtx) * 3);
    int shape = (int)(rnd64() % 10);
    double cx = -32.0 + 704.0 * rndu(), cy = -32.0 + 512.0 * rndu();
    double size = shape < 6 ? 1.0 + 31.0 * rndu() : shape < 9 ? 32.0 + 96.0 * rndu() : 128.0 + 768.0 * rndu();
    if (shape == 9 && rnd64() % 4 == 0) size = 2048.0;            /* a few very large ones (vertices far outside) */
    if (shape >= 6 && small && rnd64() % 8) size = 4.0 + 28.0 * rndu();   /* (WARP: mostly small ones) */
    if (size > 128.0 && rnd64() % 4) shape = 3;                    /* (large ones: mostly slivers, fewer samples) */
    for (int i = 0; i < 3; i++) { v[i].x = tb_coord(cx - size, cx + size); v[i].y = tb_coord(cy - size, cy + size); }
    if (shape >= 3 && shape <= 5) {                                  /* slivers: the third vertex near the line of the others */
        double t = rndu();
        v[2].x = (int32_t)(v[0].x + t * (v[1].x - v[0].x)) + (int32_t)(rnd64() % 9) - 4;
        v[2].y = (int32_t)(v[0].y + t * (v[1].y - v[0].y)) + (int32_t)(rnd64() % 9) - 4;
    }
    static const uint32_t zmaxs[3] = { 0xFFFFFFFFu, 0xFFFFFFu, 0xFFFFu };
    int fmt = (int)(rnd64() % 3);
    uint32_t lim = fmt == 2 ? 0xFFFFu : GPU_Z_FF_MAX, top = fmt == 2 ? 0x10000u : GPU_Z_FF_MAX + 1;
    uint32_t z0 = (uint32_t)(rnd64() % top);
    int zspread = (int)(rnd64() % 8);
    for (int i = 0; i < 3; i++) {
        int64_t z;
        switch (zspread < 3 ? 0 : zspread < 6 ? 1 : zspread == 6 ? 2 : (int)(rnd64() % 4)) {
        case 0: z = (int64_t)z0 + (int64_t)(rnd64() % 2001) - 1000; break;
        case 1: z = (int64_t)z0 + (int64_t)(rnd64() % 100001) - 50000; break;
        case 2: z = (int64_t)(rnd64() % top); break;
        default: z = (int64_t)pick_z_edge(); break;
        }
        if (z < 0) z = 0;
        if (fmt != 2 && z > (int64_t)GPU_Z_FF_MAX) z = rnd64() % 2 ? GPU_Z_FF_MAX : (int64_t)(rnd64() % top);
        if (fmt == 2 && z > 0xFFFF && rnd64() % 4) z &= 0xFFFF;         /* (Z16: a few vertices above the format's maximum) */
        v[i].z = (uint32_t)z;
    }
    gs_tri T;
    if (!gs_tri_setup(&T, &v[0], &v[1], &v[2], 0, 0, 0, 639, 0, 447)) return 0;
    gs_plane zp;
    gs_tri_plane(&T, &zp, v[0].z, v[1].z, v[2].z);
    gs_rowstep_attr(&zp, T.by0, 65536.0, 1, &r->row, &r->dy, &r->dx);
    r->by0 = T.by0;
    r->zmax = zmaxs[fmt];
    *slope = fabs(zp.cx) + fabs(zp.cy);
    *steep = !(*slope <= GPU_ZB_SLOPE);
    *nb = v[0].z > lim || v[1].z > lim || v[2].z > lim || *steep;
    return 1;
}

static int run_tribound(const conf_args* a) {
    static const int kxy[][2] = { { 1, 1 }, { 2, 2 }, { 3, 3 }, { 3, 6 }, { 4, 6 }, { 5, 8 }, { 6, 9 } };
    const int nk = (int)(sizeof kxy / sizeof kxy[0]);
    int ntri = a->vectors / 32;
    if (gpu_rt.caps.warp) ntri /= 8;                                 /* (WARP rasterizes on the CPU) */
    if (ntri < 256) ntri = 256;
    tri_rec* recs = malloc(sizeof(tri_rec) * (size_t)ntri);
    tri_vtx* vtx = malloc(sizeof(tri_vtx) * 3 * (size_t)ntri);
    uint8_t* steepf = malloc((size_t)ntri);
    uint32_t* vz = malloc(sizeof(uint32_t) * 3 * (size_t)ntri);
    double* slopes = malloc(sizeof(double) * (size_t)ntri);
    uint32_t* rr = calloc(8 + (size_t)ntri, 4);                   /* bres: header + per-triangle flags */
    if (!recs || !vtx || !steepf || !vz || !slopes || !rr) { free(recs); free(vtx); free(steepf); free(vz); free(slopes); free(rr); return -1; }
    const UINT rbytes = (UINT)(32 + 4 * ntri);
    int bad = -1;
    dtarget dt;
    ID3D11Buffer *vb = NULL, *rb = NULL, *res = NULL, *rst = NULL, *cb = NULL;
    ID3D11ShaderResourceView* rsrv = NULL;
    ID3D11UnorderedAccessView* ruav = NULL;
    ID3D11InputLayout* il = NULL;
    ID3D11RasterizerState* rs = NULL;
    ID3D11DepthStencilState* ds_off = NULL;
    ID3D11Device* d = gpu_rt.dev;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    memset(&dt, 0, sizeof dt);
    if (!dtarget_create(&dt, TB_RT, TB_RT)) goto out;
    rb = buf_create(sizeof(tri_rec) * (UINT)ntri, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(tri_rec),
                    D3D11_USAGE_DEFAULT, 0, NULL);
    vb = buf_create(sizeof(tri_vtx) * 3 * (UINT)ntri, D3D11_BIND_VERTEX_BUFFER, 0, 0, D3D11_USAGE_DEFAULT, 0, NULL);
    res = buf_create(rbytes, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, D3D11_USAGE_DEFAULT, 0, NULL);
    rst = buf_create(rbytes, 0, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, NULL);
    cb = buf_create(16, D3D11_BIND_CONSTANT_BUFFER, 0, 0, D3D11_USAGE_DEFAULT, 0, NULL);
    if (!rb || !vb || !res || !rst || !cb || FAILED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource*)rb, NULL, &rsrv)))
        goto out;
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = rbytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(d, (ID3D11Resource*)res, &ud, &ruav))) goto out;
        D3D11_INPUT_ELEMENT_DESC el[2] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        int vid = GPU_SH_vs_conf_tri;
        if (FAILED(ID3D11Device_CreateInputLayout(d, el, 2, gpu_shaders[vid].code, gpu_shaders[vid].size, &il))) goto out;
        D3D11_RASTERIZER_DESC rd;                                     /* as gpu_exec.c's */
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        if (FAILED(ID3D11Device_CreateRasterizerState(d, &rd, &rs))) goto out;
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        if (FAILED(ID3D11Device_CreateDepthStencilState(d, &dd, &ds_off))) goto out;
    }
    if (!gpu_vs(GPU_SH_vs_conf_tri) || !gpu_ps(GPU_SH_ps_conf_tri)) goto out;
    rng = 0x8EBC6AF09C88C6E3ULL ^ a->seed;
    uint64_t viol = 0, samples = 0, bsamples = 0, unsafe_viol = 0, unsafe_samples = 0, n_nb = 0, n_steep = 0, n_tri = 0;
    uint64_t unsafe_tris = 0, unsafe_n = 0;
    double unsafe_slope = 1e300;                                    /* the least steep triangle that broke its bound */
    int min_slack = 1 << 30;
    bad = 0;
    for (int ki = 0; ki < nk; ki++) {
        int kx = kxy[ki][0], ky = kxy[ki][1];
        if (gpu_rt.caps.warp && kx * ky > 24) continue;               /* (WARP: up to 4 x 6) */
        int nt = 0;
        while (nt < ntri) {
            gs_vtx v[3];
            int nb, steep;
            if (!tb_make(v, &recs[nt], &nb, &steep, &slopes[nt], gpu_rt.caps.warp)) continue;
            uint32_t lim = recs[nt].zmax == 0xFFFFu ? 0xFFFFu : GPU_Z_FF_MAX;
            /* 1: steep, every vertex within the limits (the slope limit is its one reason for no bound) */
            steepf[nt] = (uint8_t)(steep && v[0].z <= lim && v[1].z <= lim && v[2].z <= lim);
            for (int i = 0; i < 3; i++) {
                tri_vtx* o = &vtx[nt * 3 + i];
                o->x = (float)v[i].x * (1.0f / 16.0f);   /* (emit_vertex: GS 12.4 minus XYOFFSET, here 0) */
                o->y = (float)v[i].y * (1.0f / 16.0f);
                o->z = nb ? 0xFFFFFFFFu : v[i].z;
                vz[nt * 3 + i] = v[i].z;
            }
            n_nb += nb; n_steep += steep;
            nt++;
        }
        n_tri += (uint64_t)nt;
        for (int unsafe = 0; unsafe < 2; unsafe++) {
            /* unsafe: only the triangles whose one reason for no bound is the slope limit, drawn WITH the vertex bound
             * (what the limit guards against) */
            if (unsafe) {
                int any = 0;
                for (int t = 0; t < nt; t++) {
                    int keep = steepf[t] == 1;
                    for (int i = 0; i < 3; i++) vtx[t * 3 + i].z = keep ? vz[t * 3 + i] : 0xFFFFFFFFu;
                    any |= keep;
                }
                if (!any) break;
            }
            ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)rb, 0, NULL, recs, 0, 0);
            ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)vb, 0, NULL, vtx, 0, 0);
            memset(rr, 0, rbytes);
            rr[2] = 0x7FFFFFFF;
            ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)res, 0, NULL, rr, 0, 0);
            uint32_t cbv[4] = { 0, 0, 0, (uint32_t)kx | (uint32_t)ky << 16 };
            ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)cb, 0, NULL, cbv, 0, 0);
            D3D11_VIEWPORT vp = { -8192.0f, -8192.0f, 16384.0f, 16384.0f, 0.0f, 1.0f };   /* (gpu_exec.c REF viewport) */
            D3D11_RECT sc = { 0, 0, 640 * kx, 448 * ky };
            UINT stride = sizeof(tri_vtx), off = 0;
            ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(c, 0, NULL, dt.dsv, 1, 1, &ruav, NULL);
            ID3D11DeviceContext_OMSetDepthStencilState(c, ds_off, 0);
            ID3D11DeviceContext_RSSetState(c, rs);
            ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
            ID3D11DeviceContext_RSSetScissorRects(c, 1, &sc);
            ID3D11DeviceContext_IASetInputLayout(c, il);
            ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &vb, &stride, &off);
            ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(c, gpu_vs(GPU_SH_vs_conf_tri), NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &cb);
            ID3D11DeviceContext_PSSetShader(c, gpu_ps(GPU_SH_ps_conf_tri), NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &cb);
            ID3D11DeviceContext_PSSetShaderResources(c, 1, 1, &rsrv);
            ID3D11DeviceContext_Draw(c, (UINT)nt * 3, 0);
            ID3D11DeviceContext_ClearState(c);
            ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)rst, (ID3D11Resource*)res);
            D3D11_MAPPED_SUBRESOURCE m;
            if (!map_read((ID3D11Resource*)rst, &m)) { bad = -1; goto out; }
            memcpy(rr, m.pData, rbytes);
            ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)rst, 0);
            if (unsafe) {
                unsafe_viol += rr[0];
                unsafe_samples += rr[3];
                for (int t = 0; t < nt; t++)
                    if (rr[8 + t]) { unsafe_tris++; if (slopes[t] < unsafe_slope) unsafe_slope = slopes[t]; }
                for (int t = 0; t < nt; t++) unsafe_n += steepf[t];
                continue;
            }
            viol += rr[0];
            samples += rr[1];
            bsamples += rr[3];
            if ((int)rr[2] < min_slack) min_slack = (int)rr[2];
            if (rr[0]) {
                bad += (int)rr[0];
                if (a->verbose)
                    gpu_log("  triangle bound at %dx%d: %u samples above the rasterized depth; first: triangle %u sample (%u, %u) Z %08X depth %08X",
                            kx, ky, rr[0], rr[4], rr[5] & 0xFFFF, rr[5] >> 16, rr[6], rr[7]);
            }
        }
    }
    if (a->verbose) {
        gpu_log("triangle bound (%llu triangles at %d sample factors, %llu without a bound, %llu of them steeper than %.0f/pixel): "
                "%llu samples, %llu bounded, %llu above the rasterized depth, min slack %d (margin %u)",
                (unsigned long long)n_tri, nk, (unsigned long long)n_nb, (unsigned long long)n_steep, GPU_ZB_SLOPE,
                (unsigned long long)samples, (unsigned long long)bsamples, (unsigned long long)viol, min_slack, GPU_ZB_MARGIN);
        gpu_log("triangle bound without the slope limit (%llu steep triangles drawn with the vertex bound): %llu of %llu samples "
                "above in %llu triangles, the least steep %.0f/pixel", (unsigned long long)unsafe_n, (unsigned long long)unsafe_viol,
                (unsigned long long)unsafe_samples, (unsigned long long)unsafe_tris, unsafe_tris ? unsafe_slope : 0.0);
    }
    /* the bound must hold with room to spare: at least half the margin left on every bounded sample */
    if (bad == 0 && bsamples && min_slack < (int)(GPU_ZB_MARGIN / 2)) {
        if (a->verbose) gpu_log("  triangle bound: min slack %d below half the margin (%u)", min_slack, GPU_ZB_MARGIN / 2);
        bad = 1;
    }
out:
    ID3D11DeviceContext_ClearState(c);
    GPU_RELEASE(ds_off); GPU_RELEASE(rs); GPU_RELEASE(il); GPU_RELEASE(ruav); GPU_RELEASE(rsrv); GPU_RELEASE(cb); GPU_RELEASE(rst);
    GPU_RELEASE(res); GPU_RELEASE(vb); GPU_RELEASE(rb);
    dtarget_free(&dt);
    free(recs);
    free(vtx);
    free(steepf);
    free(vz);
    free(slopes);
    free(rr);
    return bad;
}

static void conf_fn(void* p) {
    conf_args* a = p;
    int r1 = run_cs(a);
    int r2 = run_depth(a);
    int r3 = run_zsync(a);
    int r4 = run_tribound(a);
    a->result = r1 < 0 || r2 < 0 || r3 < 0 || r4 < 0 ? -1 : r1 + r2 + r3 + r4;
}

int gpu_conformance(int vectors, uint32_t seed, int verbose) {
    if (!gpu_running()) return -1;
    conf_args a = { vectors > 0 ? vectors : 1 << 16, seed, verbose, -1 };
    gpu_run_sync(conf_fn, &a);
    return a.result;
}
