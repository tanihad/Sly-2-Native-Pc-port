/* (r2-m1) Conformance of the modern renderer's exact shader arithmetic (shaders/r2_vu.hlsli through shaders/r2_conf.hlsl)
 * on the hardware adapter and on WARP, drawn through r2gpu (runtime/r2/r2gpu.h, the D3D11 backend):
 *   vu    the VU's FMAC operations (round toward zero, flush to zero): mul, add, the DIV reciprocal and a MUL + ADD chain,
 *         bit for bit against runtime/r2/r2_vertex.c r2_vu_* (16384 cases per run: random operands over the ranges the
 *         prelit transform sees, plus cancellations, wide exponent gaps, powers of two)
 *   div   the exact rational division of the per-triangle Z records (i64_divmod_floor, i64_round_scaled) against 128-bit
 *         integer arithmetic (16384 cases)
 *   test_r2 [--adapter hw|warp|both] */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include "r2/r2.h"
#include "r2/r2gpu.h"
#include "gpu_shaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

#define W 256
#define H 64
#define N (W * H)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static float rndf(int emin, int emax) {   /* random sign, exponent in [emin, emax], random mantissa */
    int e = emin + (int)(rnd() % (uint32_t)(emax - emin + 1));
    uint32_t u = ((uint32_t)(e + 127) << 23) | (rnd() & 0x7FFFFF) | (rnd() & 1u) << 31;
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

typedef struct {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ID3D11Texture2D *tex, *stage;
    ID3D11RenderTargetView* rtv;
    r2g_dev* d;
} env;

static int env_open(env* e, int warp) {
    memset(e, 0, sizeof *e);
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 2,
                                   D3D11_SDK_VERSION, &e->dev, NULL, &e->ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(NULL, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl + 1, 1,
                                           D3D11_SDK_VERSION, &e->dev, NULL, &e->ctx);
    if (FAILED(hr)) return 0;
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(e->dev, &td, NULL, &e->tex))) return 0;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(e->dev, &td, NULL, &e->stage))) return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(e->dev, (ID3D11Resource*)e->tex, NULL, &e->rtv))) return 0;
    e->d = r2g_d3d11_create(e->dev, e->ctx);
    return e->d != NULL;
}

static void env_close(env* e) {
    r2g_destroy(e->d);
    if (e->rtv) ID3D11RenderTargetView_Release(e->rtv);
    if (e->stage) ID3D11Texture2D_Release(e->stage);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    if (e->ctx) ID3D11DeviceContext_Release(e->ctx);
    if (e->dev) ID3D11Device_Release(e->dev);
}

/* one draw of kernel ps over the target with `in` bound at t7; out: N x uint4 */
static int run_kernel(env* e, const void* ps, size_t ps_size, const void* in, uint32_t in_bytes, uint32_t* out) {
    r2g_pipe_desc pd = { gpu_sh_vs_r2c, sizeof gpu_sh_vs_r2c, ps, ps_size, NULL, 0, NULL, 0 };
    r2g_pipe* p = r2g_pipe_create(e->d, &pd);
    r2g_buf* b = r2g_buf_create(e->d, R2G_BUF_RAW, R2G_USAGE_DEFAULT, in_bytes, in);
    if (!p || !b) { fprintf(stderr, "r2: pipeline / buffer creation failed\n"); r2g_pipe_free(p); r2g_buf_free(b); return 0; }
    r2g_invalidate(e->d);
    r2g_bind_targets_native(e->d, e->rtv, NULL);
    r2g_viewport(e->d, 0.0f, 0.0f, (float)W, (float)H);
    r2g_scissor(e->d, 0, 0, W, H);
    r2g_state st = { R2G_BLEND_OFF, 15, R2G_Z_OFF, 0, R2G_CULL_NONE };
    r2g_bind_pipe(e->d, p);
    r2g_bind_state(e->d, &st);
    r2g_bind_srv(e->d, 7, b);
    r2g_draw(e->d, 3, 0);
    r2g_end(e->d);
    r2g_bind_srv(e->d, 7, NULL);
    ID3D11DeviceContext_CopyResource(e->ctx, (ID3D11Resource*)e->stage, (ID3D11Resource*)e->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    int ok = SUCCEEDED(ID3D11DeviceContext_Map(e->ctx, (ID3D11Resource*)e->stage, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        for (int y = 0; y < H; y++) memcpy(out + (size_t)y * W * 4, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, W * 16);
        ID3D11DeviceContext_Unmap(e->ctx, (ID3D11Resource*)e->stage, 0);
    }
    r2g_pipe_free(p);
    r2g_buf_free(b);
    return ok;
}

static int test_vu(env* e, const char* name) {
    static float in[N * 4];
    static uint32_t out[N * 4];
    for (int i = 0; i < N; i++) {
        float a, b, w;
        switch (i % 8) {
        case 0: a = rndf(-20, 20); b = rndf(-20, 20); break;               /* matrix x position products */
        case 1: a = rndf(30, 34); b = rndf(-12, 12); break;                 /* VU 1 (2^33..) + VU 0 x q terms */
        case 2: a = rndf(-8, 8); b = -a * (1.0f + (float)(rnd() % 64) * 0x1p-23f); break;   /* cancellations */
        case 3: a = rndf(0, 10); b = rndf(-40, -25); break;                 /* wide exponent gaps (the add's sticky bits) */
        case 4: a = ldexpf(1.0f, (int)(rnd() % 40) - 20); b = rndf(-10, 10); break;          /* powers of two */
        default: a = rndf(-16, 16); b = rndf(-16, 16); break;
        }
        w = fabsf(rndf(-10, 20));                                           /* clip w */
        if (i % 16 == 5) w = ldexpf(1.0f, (int)(rnd() % 30) - 10);
        in[i * 4 + 0] = a; in[i * 4 + 1] = b; in[i * 4 + 2] = w; in[i * 4 + 3] = 0.0f;
    }
    if (!run_kernel(e, gpu_sh_ps_r2c_vu, sizeof gpu_sh_ps_r2c_vu, in, sizeof in, out)) return 0;
    int bad[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < N; i++) {
        float a = in[i * 4], b = in[i * 4 + 1], w = in[i * 4 + 2];
        uint32_t ref[4] = { fbits(r2_vu_mul(a, b)), fbits(r2_vu_add(a, b)), fbits(r2_vu_rcp(w)), fbits(r2_vu_add(r2_vu_mul(a, b), w)) };
        for (int k = 0; k < 4; k++)
            if (out[i * 4 + k] != ref[k]) {
                if (bad[k]++ < 4)
                    fprintf(stderr, "  %s vu op %d case %d: a %a b %a w %a: gpu %08X ref %08X\n", name, k, i, a, b, w, out[i * 4 + k], ref[k]);
            }
    }
    printf("r2 %s: vu mul %d, add %d, rcp %d, chain %d mismatches of %d\n", name, bad[0], bad[1], bad[2], bad[3], N);
    return !bad[0] && !bad[1] && !bad[2] && !bad[3];
}

static int64_t floordiv128(__int128 n, __int128 d) { __int128 q = n / d; if ((n % d) && ((n < 0) != (d < 0))) q--; return (int64_t)q; }
static int64_t round_ref(int64_t n, int64_t d, int s, int pos) {
    __int128 num = (__int128)n << s;
    __int128 q = num / d, r = num % d;
    if (r < 0) { q--; r += d; }                       /* floor, 0 <= r < d */
    int neg = !pos && q < 0;
    if (neg ? 2 * r > d : 2 * r >= d) q++;
    return (int64_t)q;
}

static int test_div(env* e, const char* name) {
    static uint32_t in[N * 8];
    static uint32_t out[N * 4];
    static int64_t nn[N], dd[N];
    static int ss[N], pp[N];
    for (int i = 0; i < N; i++) {
        int s = (i & 1) ? 20 : 16;
        int nb = 1 + (int)(rnd() % (uint32_t)(s == 20 ? 42 : 46));
        int64_t n = (int64_t)(((uint64_t)rnd() << 32 | rnd()) & ((1ull << nb) - 1));
        if (rnd() & 1) n = -n;
        int db = 1 + (int)(rnd() % 36);
        int64_t d = (int64_t)(((uint64_t)rnd() << 32 | rnd()) & ((1ull << db) - 1));
        if (d == 0) d = 1 + (rnd() % 7);
        if (i % 32 == 3) n = d * (int64_t)(rnd() % 1000) / 2;           /* exact halves */
        nn[i] = n; dd[i] = d; ss[i] = s; pp[i] = (i % 4) == 1;
        uint64_t un = (uint64_t)n, ud = (uint64_t)d;
        in[i * 8 + 0] = (uint32_t)un; in[i * 8 + 1] = (uint32_t)(un >> 32);
        in[i * 8 + 2] = (uint32_t)ud; in[i * 8 + 3] = (uint32_t)(ud >> 32);
        in[i * 8 + 4] = (uint32_t)s; in[i * 8 + 5] = (uint32_t)pp[i]; in[i * 8 + 6] = in[i * 8 + 7] = 0;
    }
    if (!run_kernel(e, gpu_sh_ps_r2c_div, sizeof gpu_sh_ps_r2c_div, in, sizeof in, out)) return 0;
    int bad = 0, badt = 0, badq = 0;
    for (int i = 0; i < N; i++) {
        int64_t t = (int64_t)((uint64_t)out[i * 4] | (uint64_t)out[i * 4 + 1] << 32);
        int64_t q = (int64_t)((uint64_t)out[i * 4 + 2] | (uint64_t)out[i * 4 + 3] << 32);
        int64_t rt = round_ref(nn[i], dd[i], ss[i], pp[i]), rq = floordiv128(nn[i], dd[i]);
        badt += t != rt;
        badq += q != rq;
        if (t != rt || q != rq) {
            if (bad++ < 4)
                fprintf(stderr, "  %s div case %d: n %lld d %lld s %d: gpu %lld / %lld ref %lld / %lld\n", name, i, (long long)nn[i],
                        (long long)dd[i], ss[i], (long long)t, (long long)q, (long long)rt, (long long)rq);
        }
    }
    printf("r2 %s: div %d mismatches of %d (rounded %d, floor %d)\n", name, bad, N, badt, badq);
    return !bad;
}

/* (r2-m2) r2_matmul_fast (the EE capture's W x C under the VU's MXCSR rounding) against the reference r2_matmul */
static int test_matmul(void) {
    int bad = 0, n = 20000;
    for (int t = 0; t < n; t++) {
        float w[16], c[16], a[16], b[16];
        for (int i = 0; i < 16; i++) {
            w[i] = (t % 4 == 0 && (i % 5)) ? 0.0f : rndf(-12, 12);
            if (t % 4 == 0 && !(i % 5)) w[i] = 1.0f;
            c[i] = rndf(-20, 20);
        }
        if (t % 3 == 1) { w[3] = w[7] = w[11] = 0.0f; w[15] = 1.0f; }
        r2_matmul(w, c, a);
        r2_matmul_fast(w, c, b);
        if (memcmp(a, b, sizeof a) && bad++ < 4) {
            for (int i = 0; i < 16; i++)
                if (fbits(a[i]) != fbits(b[i])) { fprintf(stderr, "  matmul case %d [%d]: fast %08X ref %08X\n", t, i, fbits(b[i]), fbits(a[i])); break; }
        }
    }
    printf("r2 cpu: matmul_fast %d mismatches of %d\n", bad, n);
    return !bad;
}

int main(int argc, char** argv) {
    const char* adapter = "both";
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--adapter") && i + 1 < argc) adapter = argv[++i];
    int ok = test_matmul();
    for (int warp = 0; warp < 2; warp++) {
        if (!strcmp(adapter, warp ? "hw" : "warp")) continue;
        const char* name = warp ? "warp" : "hw";
        env e;
        if (!env_open(&e, warp)) { printf("r2 %s: no device\n", name); ok = 0; env_close(&e); continue; }
        rng = 0x9E3779B97F4A7C15ull;
        ok &= test_vu(&e, name);
        ok &= test_div(&e, name);
        env_close(&e);
    }
    printf(ok ? "r2 unit tests passed\n" : "r2 unit tests FAILED\n");
    return ok ? 0 : 1;
}
