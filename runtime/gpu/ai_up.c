/* (ai) Real-ESRGAN x4 (RRDBNet: num_feat 64, num_grow_ch 32, num_block from the weights) on D3D11 compute shaders,
 * plus a plain C reference of the same network. Design, model, licence and results: notes/ai-textures.md.
 *
 * The network (basicsr RRDBNet, scale 4): conv_first; num_block x RRDB (3 residual dense blocks: conv1..4 with leaky
 * ReLU 0.2 append 32 channels each, conv5 back to 64, x5 * 0.2 + x; RRDB out * 0.2 + its input); conv_body + the
 * conv_first features; 2 x (nearest 2x, conv, lrelu); conv_hr + lrelu; conv_last (3 channels). One dispatch per conv
 * (ai_up.hlsl cs_conv) over a batch of same-sized images; feature buffers (float planes):
 *   0 IMG (3)  1 F (64)  2 R (64: the RRDB input)  3 D0, 4 D1 (192: dense blocks, ping-pong)  5 BA, 6 BB (64 at 4x)
 *   7 OUT (3 at 4x). */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_up.h"

typedef struct { char name[48]; int ndim, dims[4]; float* data; } ai_tensor;
struct ai_weights { int n, nf, gc, nb; ai_tensor* t; };

static void ai_err(char* err, size_t n, const char* fmt, const char* a) { if (err && n) snprintf(err, n, fmt, a); }

ai_weights* ai_weights_load(const char* path, char* err, size_t errn) {
    FILE* f = fopen(path, "rb");
    if (!f) { ai_err(err, errn, "cannot open %s", path); return NULL; }
    char magic[8];
    uint32_t cnt = 0;
    ai_weights* w = calloc(1, sizeof *w);
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SLYAIW01", 8) || fread(&cnt, 4, 1, f) != 1 || cnt > 4096) goto bad;
    w->t = calloc(cnt, sizeof *w->t);
    for (uint32_t i = 0; i < cnt; i++) {
        ai_tensor* t = &w->t[i];
        uint16_t nl = 0;
        uint32_t nd = 0, d[4];
        if (fread(&nl, 2, 1, f) != 1 || nl >= sizeof t->name || fread(t->name, 1, nl, f) != nl) goto bad;
        if (fread(&nd, 4, 1, f) != 1 || nd < 1 || nd > 4 || fread(d, 4, nd, f) != nd) goto bad;
        size_t e = 1;
        t->ndim = (int)nd;
        for (uint32_t k = 0; k < nd; k++) { if (d[k] == 0 || d[k] > 4096) goto bad; t->dims[k] = (int)d[k]; e *= d[k]; }
        t->data = malloc(e * 4);
        if (!t->data || fread(t->data, 4, e, f) != e) goto bad;
        w->n++;
    }
    fclose(f);
    /* geometry */
    for (int i = 0; i < w->n; i++) {
        if (!strcmp(w->t[i].name, "conv_first.weight")) w->nf = w->t[i].dims[0];
        if (!strcmp(w->t[i].name, "body.0.rdb1.conv1.weight")) w->gc = w->t[i].dims[0];
        int b;
        if (sscanf(w->t[i].name, "body.%d.", &b) == 1 && b + 1 > w->nb) w->nb = b + 1;
    }
    if (w->nf != 64 || w->gc != 32 || w->nb < 1) { ai_err(err, errn, "%s: not an RRDBNet 64/32", path); ai_weights_free(w); return NULL; }
    return w;
bad:
    fclose(f);
    ai_err(err, errn, "%s: bad weights file", path);
    ai_weights_free(w);
    return NULL;
}

void ai_weights_free(ai_weights* w) {
    if (!w) return;
    for (int i = 0; i < w->n; i++) free(w->t[i].data);
    free(w->t);
    free(w);
}

static const ai_tensor* ai_get(const ai_weights* w, const char* layer, const char* kind) {
    char n[64];
    snprintf(n, sizeof n, "%s.%s", layer, kind);
    for (int i = 0; i < w->n; i++) if (!strcmp(w->t[i].name, n)) return &w->t[i];
    return NULL;
}

/* ---- the layer list (both implementations follow it) ------------------------------------------------------------ */
typedef struct { char name[32]; int cin, cout, act, up; } ai_layer;

static int ai_layers(const ai_weights* w, ai_layer* L) {   /* returns the count (<= 256) */
    int n = 0;
#define ADD(nm, ci, co, a, u) do { snprintf(L[n].name, 32, "%s", nm); L[n].cin = ci; L[n].cout = co; L[n].act = a; L[n].up = u; n++; } while (0)
    ADD("conv_first", 3, 64, 0, 0);
    for (int b = 0; b < w->nb; b++)
        for (int r = 1; r <= 3; r++)
            for (int c = 1; c <= 5; c++) {
                char nm[32];
                snprintf(nm, 32, "body.%d.rdb%d.conv%d", b, r, c);
                ADD(nm, 64 + (c - 1) * 32, c == 5 ? 64 : 32, c < 5, 0);
            }
    ADD("conv_body", 64, 64, 0, 0);
    ADD("conv_up1", 64, 64, 1, 1);
    ADD("conv_up2", 64, 64, 1, 1);
    ADD("conv_hr", 64, 64, 1, 0);
    ADD("conv_last", 64, 3, 0, 0);
#undef ADD
    return n;
}

/* ---- CPU reference ------------------------------------------------------------------------------------------------ */
static void ref_conv(const ai_weights* wt, const ai_layer* l, const float* in, int w, int h, float* out) {
    const ai_tensor* W = ai_get(wt, l->name, "weight");
    const ai_tensor* B = ai_get(wt, l->name, "bias");
    int iw = l->up ? w / 2 : w;
    size_t ipl = (size_t)iw * (l->up ? h / 2 : h), pl = (size_t)w * h;
    for (int o = 0; o < l->cout; o++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                double s = B->data[o];
                for (int c = 0; c < l->cin; c++) {
                    const float* k = W->data + ((size_t)o * l->cin + c) * 9;
                    for (int dy = -1; dy <= 1; dy++) {
                        int yy = y + dy;
                        if (yy < 0 || yy >= h) continue;
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = x + dx;
                            if (xx < 0 || xx >= w) continue;
                            size_t i = l->up ? (size_t)(yy >> 1) * iw + (xx >> 1) : (size_t)yy * w + xx;
                            s += (double)k[(dy + 1) * 3 + dx + 1] * in[c * ipl + i];
                        }
                    }
                }
                float v = (float)s;
                if (l->act && v < 0) v *= 0.2f;
                out[o * pl + (size_t)y * w + x] = v;
            }
}

int ai_cpu_forward(const ai_weights* wt, const float* in, int w, int h, float* out) {
    ai_layer L[256];
    ai_layers(wt, L);
    size_t P = (size_t)w * h;
    float* F = malloc(64 * P * 4), *R = malloc(64 * P * 4), *D = malloc(192 * P * 4), *T = malloc(64 * P * 4);
    float* A = malloc(64 * 16 * P * 4), *Bb = malloc(64 * 16 * P * 4);
    if (!F || !R || !D || !T || !A || !Bb) return -1;
    int li = 0;
    ref_conv(wt, &L[li++], in, w, h, F);
    memcpy(R, F, 64 * P * 4);
    memcpy(D, F, 64 * P * 4);                   /* D[0:64] = the RDB's input */
    for (int b = 0; b < wt->nb; b++) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) { ref_conv(wt, &L[li], D, w, h, D + (64 + 32 * c) * P); li++; }
            ref_conv(wt, &L[li++], D, w, h, T);
            for (size_t i = 0; i < 64 * P; i++) D[i] = T[i] * 0.2f + D[i];
        }
        for (size_t i = 0; i < 64 * P; i++) { D[i] = D[i] * 0.2f + R[i]; R[i] = D[i]; }
    }
    ref_conv(wt, &L[li++], D, w, h, T);
    for (size_t i = 0; i < 64 * P; i++) F[i] += T[i];
    ref_conv(wt, &L[li++], F, 2 * w, 2 * h, A);
    ref_conv(wt, &L[li++], A, 4 * w, 4 * h, Bb);
    ref_conv(wt, &L[li++], Bb, 4 * w, 4 * h, A);
    ref_conv(wt, &L[li++], A, 4 * w, 4 * h, out);
    free(F); free(R); free(D); free(T); free(A); free(Bb);
    return 0;
}

/* ---- GPU ------------------------------------------------------------------------------------------------------------ */
typedef struct {
    uint32_t in_buf, in_cap, cin, up_in, out_buf, out_c0, out_cap, cout, W, H, act, n_ocb;
    uint32_t r1_buf, r1_c0, r1_cap, r2_buf, r2_c0, r2_cap, o2_buf, o2_c0, o2_cap, w_off, b_off, n_cchunk;
    float a0, a1, a2, pad0;
} ai_cb;

enum { B_IMG, B_F, B_R, B_D0, B_D1, B_BA, B_BB, B_OUT, B_N, B_NONE = 8 };
static const uint32_t ai_cap[B_N] = { 3, 64, 64, 192, 192, 64, 64, 3 };
static const uint32_t ai_res[B_N] = { 1, 1, 1, 1, 1, 16, 16, 16 };   /* pixels per input pixel */

struct ai_gpu {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    void* compile;
    char* src;
    size_t nsrc;
    char path[260];
    struct { uint32_t key; ID3D11ComputeShader* cs; } perm[32];
    int nperm;
    ID3D11Buffer *wbuf, *cbuf, *buf[B_N], *stage;
    ID3D11ShaderResourceView* wsrv;
    ID3D11UnorderedAccessView* uav[B_N];
    ai_layer L[256];
    uint32_t woff[256], boff[256];
    int nl;
    size_t maxp, bytes;
};

static ID3D11Buffer* ai_buffer(ai_gpu* g, size_t bytes, UINT bind, D3D11_USAGE use, UINT cpu, UINT stride, const void* init) {
    D3D11_BUFFER_DESC d;
    memset(&d, 0, sizeof d);
    d.ByteWidth = (UINT)bytes; d.Usage = use; d.BindFlags = bind; d.CPUAccessFlags = cpu;
    if (stride) { d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride; }
    D3D11_SUBRESOURCE_DATA s = { init, 0, 0 };
    ID3D11Buffer* b = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(g->dev, &d, init ? &s : NULL, &b))) return NULL;
    g->bytes += bytes;
    return b;
}

typedef HRESULT(WINAPI* ai_compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);

ai_gpu* ai_gpu_create(ID3D11Device* dev, ID3D11DeviceContext* ctx, const ai_weights* wt, const char* hlsl_path,
                      size_t max_pixels, char* err, size_t errn) {
    ai_gpu* g = calloc(1, sizeof *g);
    g->dev = dev; g->ctx = ctx;
    if (max_pixels > 131072) max_pixels = 131072;   /* (each 4x buffer stays under 2^27 floats: a view's limit) */
    g->maxp = max_pixels;
    /* shader source (permutations compiled on first use: ai_shader) */
    FILE* f = fopen(hlsl_path, "rb");
    if (!f) { ai_err(err, errn, "cannot open %s", hlsl_path); free(g); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g->src = calloc(1, (size_t)n + 1);
    g->nsrc = fread(g->src, 1, (size_t)n, f);
    fclose(f);
    snprintf(g->path, sizeof g->path, "%s", hlsl_path);
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    g->compile = dc ? (void*)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!g->compile) { ai_err(err, errn, "%s", "no d3dcompiler_47.dll"); free(g->src); free(g); return NULL; }
    /* packed weights: per layer [ocb][ci chunk][ci 8][tap 9][oc 16], then biases [ocb][16] */
    g->nl = ai_layers(wt, g->L);
    size_t tot = 0;
    for (int i = 0; i < g->nl; i++) {
        int nocb = (g->L[i].cout + 15) / 16, nch = (g->L[i].cin + 7) / 8;
        g->woff[i] = (uint32_t)(tot / 4); tot += (size_t)nocb * nch * 8 * 9 * 16;
        g->boff[i] = (uint32_t)(tot / 4); tot += (size_t)nocb * 16;
    }
    float* pk = calloc(tot, 4);
    for (int i = 0; i < g->nl; i++) {
        const ai_layer* l = &g->L[i];
        const ai_tensor* W = ai_get(wt, l->name, "weight");
        const ai_tensor* B = ai_get(wt, l->name, "bias");
        if (!W || !B || W->dims[0] != l->cout || W->dims[1] != l->cin) { ai_err(err, errn, "missing layer %s", l->name); free(pk); free(g); return NULL; }
        int nocb = (l->cout + 15) / 16, nch = (l->cin + 7) / 8;
        for (int ob = 0; ob < nocb; ob++)
            for (int ch = 0; ch < nch; ch++)
                for (int c = 0; c < 8; c++)
                    for (int t = 0; t < 9; t++)
                        for (int o = 0; o < 16; o++) {
                            int oc = ob * 16 + o, ic = ch * 8 + c;
                            if (oc >= l->cout || ic >= l->cin) continue;
                            pk[(size_t)g->woff[i] * 4 + ((((size_t)ob * nch + ch) * 8 + c) * 9 + t) * 16 + o] = W->data[((size_t)oc * l->cin + ic) * 9 + t];
                        }
        for (int o = 0; o < l->cout; o++) pk[(size_t)g->boff[i] * 4 + o] = B->data[o];
    }
    g->wbuf = ai_buffer(g, tot * 4, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_IMMUTABLE, 0, 16, pk);
    free(pk);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_UNKNOWN; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sd.Buffer.NumElements = (UINT)(tot / 4);
    if (!g->wbuf || FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)g->wbuf, &sd, &g->wsrv))) goto fail;
    g->cbuf = ai_buffer(g, sizeof(ai_cb), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, 0, NULL);
    for (int b = 0; b < B_N; b++) {
        size_t e = g->maxp * ai_cap[b] * ai_res[b];
        g->buf[b] = ai_buffer(g, e * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, 4, NULL);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = (UINT)e;
        if (!g->buf[b] || FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource*)g->buf[b], &ud, &g->uav[b]))) goto fail;
    }
    g->stage = ai_buffer(g, g->maxp * 48 * 4, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0, NULL);
    if (!g->cbuf || !g->stage) goto fail;
    return g;
fail:
    ai_err(err, errn, "%s", "out of video memory (buffers)");
    ai_gpu_destroy(g);
    return NULL;
}

size_t ai_gpu_bytes(const ai_gpu* g) { return g ? g->bytes : 0; }

/* the permutation for a layer's buffers (BIN, BOUT, BR1, BR2, BO2 macros; B_NONE = absent) */
static ID3D11ComputeShader* ai_shader(ai_gpu* g, uint32_t in, uint32_t out, uint32_t r1, uint32_t r2, uint32_t o2) {
    uint32_t key = in | out << 4 | r1 << 8 | r2 << 12 | o2 << 16;
    for (int i = 0; i < g->nperm; i++) if (g->perm[i].key == key) return g->perm[i].cs;
    if (g->nperm == 32) return NULL;
    static const char* nm[B_N] = { "U0", "U1", "U2", "U3", "U4", "U5", "U6", "U7" };
    D3D_SHADER_MACRO m[6];
    int k = 0;
    m[k].Name = "BIN"; m[k++].Definition = nm[in];
    m[k].Name = "BOUT"; m[k++].Definition = nm[out];
    if (r1 < B_N) { m[k].Name = "BR1"; m[k++].Definition = nm[r1]; }
    if (r2 < B_N) { m[k].Name = "BR2"; m[k++].Definition = nm[r2]; }
    if (o2 < B_N) { m[k].Name = "BO2"; m[k++].Definition = nm[o2]; }
    m[k].Name = NULL; m[k].Definition = NULL;
    ID3DBlob *code = NULL, *msg = NULL;
    ID3D11ComputeShader* cs = NULL;
    /* (a compiled-shader cache next to the tool's temp files: the unrolled kernel takes ~4 s per permutation to compile;
     * the game would embed the DXBC at build time through scripts/hlslc.c instead) */
    char cpath[400] = "";
    uint64_t hsh = 1469598103934665603ull;
    for (size_t i = 0; i < g->nsrc; i++) hsh = (hsh ^ (uint8_t)g->src[i]) * 1099511628211ull;
    const char* cdir = getenv("AI_UP_CACHE");
    if (cdir && *cdir) {
        snprintf(cpath, sizeof cpath, "%s\\%016llx_%05x.dxbc", cdir, (unsigned long long)hsh, key);
        FILE* f = fopen(cpath, "rb");
        if (f) {
            static char buf[1 << 20];
            size_t nb = fread(buf, 1, sizeof buf, f);
            fclose(f);
            if (nb > 0 && SUCCEEDED(ID3D11Device_CreateComputeShader(g->dev, buf, nb, NULL, &cs))) {
                g->perm[g->nperm].key = key; g->perm[g->nperm++].cs = cs;
                return cs;
            }
        }
    }
    if (FAILED(((ai_compile_fn)g->compile)(g->src, g->nsrc, g->path, m, NULL, "cs_conv", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &msg))) {
        fprintf(stderr, "ai_up.hlsl: %s\n", msg ? (const char*)ID3D10Blob_GetBufferPointer(msg) : "compile failed");
    } else {
        ID3D11Device_CreateComputeShader(g->dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &cs);
        FILE* f = cpath[0] ? fopen(cpath, "wb") : NULL;
        if (f) { fwrite(ID3D10Blob_GetBufferPointer(code), 1, ID3D10Blob_GetBufferSize(code), f); fclose(f); }
    }
    if (code) ID3D10Blob_Release(code);
    if (msg) ID3D10Blob_Release(msg);
    if (cs) { g->perm[g->nperm].key = key; g->perm[g->nperm++].cs = cs; }
    return cs;
}

static void ai_dispatch(ai_gpu* g, const ai_layer* l, int li, int n, uint32_t W, uint32_t H, uint32_t in, uint32_t out,
                        uint32_t out_c0, uint32_t r1, uint32_t r1_c0, uint32_t r2, uint32_t o2, float a0, float a1, float a2) {
    ai_cb c;
    memset(&c, 0, sizeof c);
    c.in_buf = in; c.in_cap = ai_cap[in]; c.cin = (uint32_t)l->cin; c.up_in = (uint32_t)l->up;
    c.out_buf = out; c.out_c0 = out_c0; c.out_cap = ai_cap[out]; c.cout = (uint32_t)l->cout;
    c.W = W; c.H = H; c.act = (uint32_t)l->act; c.n_ocb = (uint32_t)(l->cout + 15) / 16;
    c.r1_buf = r1; c.r1_c0 = r1_c0; c.r1_cap = r1 < B_N ? ai_cap[r1] : 0;
    c.r2_buf = r2; c.r2_c0 = 0; c.r2_cap = r2 < B_N ? ai_cap[r2] : 0;
    c.o2_buf = o2; c.o2_c0 = 0; c.o2_cap = o2 < B_N ? ai_cap[o2] : 0;
    c.w_off = g->woff[li]; c.b_off = g->boff[li]; c.n_cchunk = (uint32_t)(l->cin + 7) / 8;
    c.a0 = a0; c.a1 = a1; c.a2 = a2;
    ID3D11ComputeShader* cs = ai_shader(g, in, out, r1, r2, o2);
    if (!cs) return;
    ID3D11DeviceContext_CSSetShader(g->ctx, cs, NULL, 0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource*)g->cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource*)g->cbuf, 0);
    ID3D11DeviceContext_Dispatch(g->ctx, (W + 31) / 32, (H + 7) / 8, (UINT)n * c.n_ocb);
}

int ai_gpu_run(ai_gpu* g, int n, int w, int h, const float* in, float* out) {
    size_t P = (size_t)w * h;
    if (n < 1 || (size_t)n * P > g->maxp) return -1;
    D3D11_BOX bx = { 0, 0, 0, (UINT)((size_t)n * 3 * P * 4), 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource*)g->buf[B_IMG], 0, &bx, in, 0, 0);
    ID3D11DeviceContext_CSSetShaderResources(g->ctx, 0, 1, &g->wsrv);
    ID3D11DeviceContext_CSSetConstantBuffers(g->ctx, 0, 1, &g->cbuf);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g->ctx, 0, B_N, g->uav, NULL);
    uint32_t W = (uint32_t)w, H = (uint32_t)h;
    int li = 0;
    ai_dispatch(g, &g->L[li], li, n, W, H, B_IMG, B_F, 0, B_NONE, 0, B_NONE, B_D0, 1, 0, 0); li++;
    uint32_t cur = B_D0, oth = B_D1;
    for (int b = 0; b < (g->nl - 6) / 15; b++) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) { ai_dispatch(g, &g->L[li], li, n, W, H, cur, cur, 64 + 32 * c, B_NONE, 0, B_NONE, B_NONE, 1, 0, 0); li++; }
            if (r < 2)      /* x5 * 0.2 + x -> the next RDB's input */
                ai_dispatch(g, &g->L[li], li, n, W, H, cur, oth, 0, cur, 0, B_NONE, B_NONE, 0.2f, 1, 0);
            else            /* ((x5 * 0.2 + x) * 0.2 + RRDB input) -> R and the next RRDB's dense buffer */
                ai_dispatch(g, &g->L[li], li, n, W, H, cur, oth, 0, cur, 0, b ? B_R : B_F, B_R, 0.04f, 0.2f, 1);
            li++;
            uint32_t t = cur; cur = oth; oth = t;
        }
    }
    ai_dispatch(g, &g->L[li], li, n, W, H, cur, B_F, 0, B_F, 0, B_NONE, B_NONE, 1, 1, 0); li++;      /* + conv_first */
    ai_dispatch(g, &g->L[li], li, n, 2 * W, 2 * H, B_F, B_BA, 0, B_NONE, 0, B_NONE, B_NONE, 1, 0, 0); li++;
    ai_dispatch(g, &g->L[li], li, n, 4 * W, 4 * H, B_BA, B_BB, 0, B_NONE, 0, B_NONE, B_NONE, 1, 0, 0); li++;
    ai_dispatch(g, &g->L[li], li, n, 4 * W, 4 * H, B_BB, B_BA, 0, B_NONE, 0, B_NONE, B_NONE, 1, 0, 0); li++;
    ai_dispatch(g, &g->L[li], li, n, 4 * W, 4 * H, B_BA, B_OUT, 0, B_NONE, 0, B_NONE, B_NONE, 1, 0, 0); li++;
    ID3D11UnorderedAccessView* nul[B_N] = { 0 };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g->ctx, 0, B_N, nul, NULL);
    size_t ob = (size_t)n * 48 * P * 4;
    D3D11_BOX ox = { 0, 0, 0, (UINT)ob, 1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(g->ctx, (ID3D11Resource*)g->stage, 0, 0, 0, 0, (ID3D11Resource*)g->buf[B_OUT], 0, &ox);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource*)g->stage, 0, D3D11_MAP_READ, 0, &m))) return -2;
    memcpy(out, m.pData, ob);
    ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource*)g->stage, 0);
    return 0;
}

void ai_gpu_destroy(ai_gpu* g) {
    if (!g) return;
    for (int b = 0; b < B_N; b++) {
        if (g->uav[b]) ID3D11UnorderedAccessView_Release(g->uav[b]);
        if (g->buf[b]) ID3D11Buffer_Release(g->buf[b]);
    }
    if (g->stage) ID3D11Buffer_Release(g->stage);
    if (g->wsrv) ID3D11ShaderResourceView_Release(g->wsrv);
    if (g->wbuf) ID3D11Buffer_Release(g->wbuf);
    if (g->cbuf) ID3D11Buffer_Release(g->cbuf);
    for (int i = 0; i < g->nperm; i++) ID3D11ComputeShader_Release(g->perm[i].cs);
    free(g->src);
    free(g);
}
