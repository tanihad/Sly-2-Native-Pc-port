/* Render-thread execution of the frontend's command stream (notes/d3d11-renderer.md 2.1, milestone
 * M2 "submission go/no-go").
 *
 * The GS thread's frontend (gpu_front.c) closes a frame at every VBlank: commands (gpu_front.h
 * GPU_FOP_*), 32-byte vertices, u32 indices, records, StateRecs and a data arena. The sink below hands
 * the frame's buffers to the render thread without copying (gpu_front_detach); the render thread
 * uploads them into dynamic buffers and executes every command with real D3D11 objects: colour and
 * depth targets at the targets' scale, a vram_buf and a CLUT ring updated by the patch/CLUT ops,
 * state-object caches (blend, depth-stencil, rasterizer), the bindings of the design (records and
 * StateRecs as raw SRVs, per-draw constants through D3D11.1 constant-buffer offsets, ROV runs through
 * OMSetRenderTargetsAndUnorderedAccessViews with a read-only DSV, compute dispatches for loads and
 * ALIAS16), and trivial shaders (shaders/submit.hlsl). Nothing it draws is correct: M2 measures the
 * CPU cost of submitting Sly 2's frames (per frame and per D3D draw), M3 replaces the shaders.
 *
 * Timing per frame (gpu_frame_stats): render_ms = wall time of the whole frame on the render thread,
 * render_cpu_ms = its thread CPU time, upload_ms = the Map/copy part; GPU time from timestamps. */
#include "gpu_internal.h"
#include "gpu_front.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shaders.h"

/* frontend layouts (gpu_front.c / gpu_front_int.h) */
enum { XK_C32, XK_Z32, XK_Z24, XK_Z16, XK_Z16S };
enum { BL_OFF, BL_NOCOLOR, BL_ADD, BL_REVSUB, BL_DUAL, BL_N };
#define MAX_TARGETS 64
#define DRAW_CB_BYTES 256                /* per-draw constants: one 16-constant window (CB offsets) */

typedef struct {
    gpu_front_frame_data fd;
    void* handle;                        /* gpu_front_release */
} gpu_op_frame;

/* ---- render-thread state ---------------------------------------------------------------------- */
typedef struct {
    int used, depth;
    int w, h;                            /* allocated (pixels at scale) */
    int kx, ky, nat_w, nat_h;            /* requested native size and factors */
    ID3D11Texture2D* tex;
    ID3D11RenderTargetView* rtv;
    ID3D11ShaderResourceView* srv;
    ID3D11UnorderedAccessView* uav;
    ID3D11DepthStencilView *dsv, *dsv_ro;
    ID3D11Texture2D* shadow;             /* feedback copies (5.1) */
    ID3D11ShaderResourceView* shadow_srv;
    ID3D11Texture2D* stage;              /* downloads */
} xtarget;

typedef struct {
    ID3D11Buffer* buf;
    ID3D11ShaderResourceView* srv;
    size_t cap;
} dynbuf;

static struct {
    int init;
    xtarget T[MAX_TARGETS + 1];
    dynbuf vb, ib, rec, st, cb;
    ID3D11Buffer* vram;                  /* 4 MB raw (vram_buf) */
    ID3D11ShaderResourceView* vram_srv;
    ID3D11Buffer* clut;                  /* 4096 x 1 KB raw (CLUT ring) */
    ID3D11ShaderResourceView* clut_srv;
    ID3D11InputLayout* layout;
    ID3D11RasterizerState* rs;
    ID3D11BlendState* bs[BL_N][16];
    ID3D11DepthStencilState* ds[8];      /* depth class (ztst * 2 + zwrite) */
    ID3D11DepthStencilState* ds_off;
    ID3D11DepthStencilState* ds_write;   /* Z resolve / depth load: ALWAYS + write */
    /* bound state (redundant calls are filtered here, as a real backend does) */
    int mode;                            /* 0 none, 1 FF (RTV + DSV), 2 ROV (UAV + read-only DSV), 3 quad DSV */
    int b_ct, b_zt;
    void *b_vs, *b_ps, *b_bs, *b_ds, *b_t1, *b_t2;
    int b_topo;
    D3D11_RECT b_sc;
    int b_vp_w, b_vp_h;
    uint32_t state_calls;
} X;

static atomic_int inflight;             /* frames handed over and not finished */
#define MAX_INFLIGHT 3

static void count_state(void) { X.state_calls++; }

/* ---- resources ---------------------------------------------------------------------------------- */

static int dyn_ensure(dynbuf* d, size_t bytes, UINT bind, int raw) {
    if (d->buf && d->cap >= bytes) return 1;
    GPU_RELEASE(d->srv);
    GPU_RELEASE(d->buf);
    size_t cap = d->cap ? d->cap : 1 << 20;
    while (cap < bytes) cap *= 2;
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (UINT)cap;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (raw) bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &d->buf))) { gpu_log("cannot create a %zu-byte buffer", cap); return 0; }
    if (raw) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = (UINT)(cap / 4);
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)d->buf, &sd, &d->srv))) return 0;
    }
    d->cap = cap;
    return 1;
}

static int dyn_fill(dynbuf* d, const void* src, size_t bytes, UINT bind, int raw) {
    if (!bytes) return 1;
    if (!dyn_ensure(d, bytes, bind, raw)) return 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)d->buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, src, bytes);
    ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)d->buf, 0);
    gpu_rt.n_uploads++;
    gpu_rt.upload_bytes += (uint32_t)bytes;
    return 1;
}

static ID3D11Buffer* raw_default(UINT bytes, ID3D11ShaderResourceView** srv) {
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ID3D11Buffer* b = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &b))) return NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = bytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)b, &sd, srv))) { GPU_RELEASE(b); return NULL; }
    return b;
}

static void target_free(xtarget* t) {
    GPU_RELEASE(t->rtv); GPU_RELEASE(t->srv); GPU_RELEASE(t->uav); GPU_RELEASE(t->dsv); GPU_RELEASE(t->dsv_ro);
    GPU_RELEASE(t->shadow_srv); GPU_RELEASE(t->shadow); GPU_RELEASE(t->stage); GPU_RELEASE(t->tex);
}

static void unbind_all(void);

/* (re)create t at w x h pixels; the old contents are copied over */
static int target_alloc(xtarget* t, int w, int h) {
    if (t->tex && t->w == w && t->h == h) return 1;
    unbind_all();
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (t->depth) {
        td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    } else {
        td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    }
    ID3D11Texture2D* nt = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &nt))) { gpu_log("cannot create a %dx%d target", w, h); return 0; }
    if (t->tex) {                        /* grow: keep what was drawn */
        D3D11_BOX box = { 0, 0, 0, (UINT)(t->w < w ? t->w : w), (UINT)(t->h < h ? t->h : h), 1 };
        ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)nt, 0, 0, 0, 0, (ID3D11Resource*)t->tex, 0, &box);
        gpu_rt.n_copies++;
    }
    target_free(t);
    t->tex = nt;
    t->w = w;
    t->h = h;
    HRESULT hr;
    if (t->depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)nt, &dd, &t->dsv);
        dd.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)nt, &dd, &t->dsv_ro);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)nt, &sd, &t->srv);
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)nt, &rd, &t->rtv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)nt, &sd, &t->srv);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)nt, &ud, &t->uav);
    }
    if (FAILED(hr)) { gpu_log("cannot create target views (hr=0x%08lX)", (unsigned long)hr); return 0; }
    return 1;
}

/* allocation granularity: 64 native rows, so growing targets are recreated rarely */
static int alloc_h(const xtarget* t) { return ((t->nat_h + 63) & ~63) * t->ky; }

static void exec_target(const gpu_fop_target* o) {
    if (o->id == 0 || o->id > MAX_TARGETS) return;
    xtarget* t = &X.T[o->id];
    t->used = 1;
    t->depth = o->kind != XK_C32;
    t->kx = o->kx ? o->kx : 1;
    t->ky = o->ky ? o->ky : 1;
    t->nat_w = o->fbw * 64;
    t->nat_h = o->rows;
    int w = t->nat_w * t->kx, h = alloc_h(t);
    if (t->tex && t->w >= w && t->h >= h && t->w == w) return;   /* (taller allocation kept) */
    if (t->tex && t->h > h) h = t->h;
    target_alloc(t, w, h);
}

static void exec_release(const gpu_fop_target* o) {
    if (o->id == 0 || o->id > MAX_TARGETS) return;
    unbind_all();
    target_free(&X.T[o->id]);
    memset(&X.T[o->id], 0, sizeof X.T[o->id]);
}

/* D3D11 needs equal dimensions for the bound RTV and DSV: the smaller one grows */
static void pair_match(xtarget* c, xtarget* z) {
    if (!c || !z || !c->tex || !z->tex) return;
    if (c->w == z->w && c->h == z->h) return;
    int w = c->w > z->w ? c->w : z->w, h = c->h > z->h ? c->h : z->h;
    target_alloc(c, w, h);
    target_alloc(z, w, h);
}

/* ---- state objects ------------------------------------------------------------------------------ */

static ID3D11BlendState* blend_state(int cls, int wmask) {
    if (cls < 0 || cls >= BL_N) cls = BL_OFF;
    wmask &= 15;
    if (X.bs[cls][wmask]) return X.bs[cls][wmask];
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    D3D11_RENDER_TARGET_BLEND_DESC* r = &bd.RenderTarget[0];
    r->RenderTargetWriteMask = (UINT8)wmask;
    r->SrcBlendAlpha = D3D11_BLEND_ONE;
    r->DestBlendAlpha = D3D11_BLEND_ZERO;
    r->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    switch (cls) {
    case BL_ADD: r->BlendEnable = TRUE; r->SrcBlend = D3D11_BLEND_ONE; r->DestBlend = D3D11_BLEND_ONE; r->BlendOp = D3D11_BLEND_OP_ADD; break;
    case BL_REVSUB: r->BlendEnable = TRUE; r->SrcBlend = D3D11_BLEND_ONE; r->DestBlend = D3D11_BLEND_ONE; r->BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
    case BL_DUAL: r->BlendEnable = TRUE; r->SrcBlend = D3D11_BLEND_SRC1_ALPHA; r->DestBlend = D3D11_BLEND_INV_SRC1_ALPHA; r->BlendOp = D3D11_BLEND_OP_ADD; break;
    default: r->BlendEnable = FALSE; r->SrcBlend = D3D11_BLEND_ONE; r->DestBlend = D3D11_BLEND_ZERO; r->BlendOp = D3D11_BLEND_OP_ADD; break;
    }
    ID3D11Device_CreateBlendState(gpu_rt.dev, &bd, &X.bs[cls][wmask]);
    return X.bs[cls][wmask];
}

static ID3D11DepthStencilState* make_ds(int enable, D3D11_COMPARISON_FUNC f, int write) {
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = enable ? TRUE : FALSE;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = f;
    ID3D11DepthStencilState* s = NULL;
    ID3D11Device_CreateDepthStencilState(gpu_rt.dev, &dd, &s);
    return s;
}

static ID3D11DepthStencilState* depth_state(int cls) {
    cls &= 7;
    if (!cls) return X.ds_off;
    if (!X.ds[cls]) {
        static const D3D11_COMPARISON_FUNC f[4] = { D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS,
                                                    D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_GREATER };
        X.ds[cls] = make_ds(1, f[cls >> 1], cls & 1);
    }
    return X.ds[cls];
}

/* ---- shaders -------------------------------------------------------------------------------------- */
#define PSFF(D, Z, X) { GPU_SH_ps_ff_d##D##z##Z##x##X##t0, GPU_SH_ps_ff_d##D##z##Z##x##X##t1, GPU_SH_ps_ff_d##D##z##Z##x##X##t2 }
static const int ps_ff_id[2][2][2][3] = {
    { { PSFF(0, 0, 0), PSFF(0, 0, 1) }, { PSFF(0, 1, 0), PSFF(0, 1, 1) } },
    { { PSFF(1, 0, 0), PSFF(1, 0, 1) }, { PSFF(1, 1, 0), PSFF(1, 1, 1) } },
};
static const int ps_rov_id[3] = { GPU_SH_ps_rov_t0, GPU_SH_ps_rov_t1, GPU_SH_ps_rov_t2 };

static int tex_class(uint32_t ps_key) {
    int tk = (int)((ps_key >> 2) & 7);   /* TK_NONE, VRAM_IDX, VRAM_DIRECT, VIEW_C32, VIEW_ALPHA, ALIAS16 */
    return tk == 0 ? 0 : tk <= 2 ? 1 : 2;
}

/* ---- binding helpers ------------------------------------------------------------------------------ */

static void unbind_all(void) {
    if (!gpu_rt.ctx) return;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* none[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(c, 1, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    ID3D11UnorderedAccessView* nu = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &nu, NULL);
    X.mode = 0;
    X.b_ct = X.b_zt = -1;
    X.b_t1 = X.b_t2 = NULL;
    X.state_calls += 3;
}

static void set_viewport(int w, int h) {
    if (X.b_vp_w == w && X.b_vp_h == h) return;
    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(gpu_rt.ctx, 1, &vp);
    X.b_vp_w = w;
    X.b_vp_h = h;
    count_state();
}

static void set_scissor(LONG x0, LONG y0, LONG x1, LONG y1) {
    D3D11_RECT r = { x0, y0, x1, y1 };
    if (!memcmp(&r, &X.b_sc, sizeof r)) return;
    ID3D11DeviceContext_RSSetScissorRects(gpu_rt.ctx, 1, &r);
    X.b_sc = r;
    count_state();
}

static void set_vs(ID3D11VertexShader* s) {
    if (X.b_vs == s) return;
    ID3D11DeviceContext_VSSetShader(gpu_rt.ctx, s, NULL, 0);
    X.b_vs = s;
    count_state();
}

static void set_ps(ID3D11PixelShader* s) {
    if (X.b_ps == s) return;
    ID3D11DeviceContext_PSSetShader(gpu_rt.ctx, s, NULL, 0);
    X.b_ps = s;
    count_state();
}

static void set_blend(ID3D11BlendState* s) {
    if (X.b_bs == s) return;
    ID3D11DeviceContext_OMSetBlendState(gpu_rt.ctx, s, NULL, 0xFFFFFFFFu);
    X.b_bs = s;
    count_state();
}

static void set_depth(ID3D11DepthStencilState* s) {
    if (X.b_ds == s) return;
    ID3D11DeviceContext_OMSetDepthStencilState(gpu_rt.ctx, s, 0);
    X.b_ds = s;
    count_state();
}

static void set_topo(int strip) {
    if (X.b_topo == strip + 1) return;
    ID3D11DeviceContext_IASetPrimitiveTopology(gpu_rt.ctx, strip ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    X.b_topo = strip + 1;
    count_state();
}

static void set_t1(ID3D11ShaderResourceView* v) {
    if (X.b_t1 == v) return;
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 1, 1, &v);
    X.b_t1 = v;
    count_state();
}

static void set_t2(ID3D11ShaderResourceView* v) {
    if (X.b_t2 == v) return;
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 2, 1, &v);
    X.b_t2 = v;
    count_state();
}

static xtarget* tgt(uint32_t id) { return id && id <= MAX_TARGETS && X.T[id].used && X.T[id].tex ? &X.T[id] : NULL; }

/* FF binding: RTV of ct + DSV of zt */
static void bind_ff(int ct, int zt) {
    xtarget *c = tgt((uint32_t)ct), *z = tgt((uint32_t)zt);
    if (c && z && (c->w != z->w || c->h != z->h)) pair_match(c, z);
    if (X.mode == 1 && X.b_ct == ct && X.b_zt == zt) return;
    /* the targets may be bound as textures (t1 view, t2 depth SRV): unbind those first */
    if (X.b_t2) set_t2(NULL);
    if (X.b_t1 && ((c && X.b_t1 == c->srv) || (z && X.b_t1 == z->srv))) set_t1(NULL);
    ID3D11RenderTargetView* rtv = c ? c->rtv : NULL;
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, c ? 1 : 0, c ? &rtv : NULL, z ? z->dsv : NULL);
    count_state();
    X.mode = 1;
    X.b_ct = ct;
    X.b_zt = zt;
    xtarget* v = c ? c : z;
    if (v) set_viewport(v->w, v->h);
}

/* ROV binding: colour UAV (rasterizer ordered in the shader) + read-only DSV and depth SRV of zt */
static void bind_rov(int ct, int zt) {
    xtarget *c = tgt((uint32_t)ct), *z = tgt((uint32_t)zt);
    if (c && z && (c->w != z->w || c->h != z->h)) pair_match(c, z);
    if (X.mode == 2 && X.b_ct == ct && X.b_zt == zt) return;
    if (X.b_t1 && c && X.b_t1 == c->srv) set_t1(NULL);
    ID3D11UnorderedAccessView* uav = c ? c->uav : NULL;
    ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(gpu_rt.ctx, 0, NULL, z ? z->dsv_ro : NULL, 0, 1, &uav, NULL);
    count_state();
    X.mode = 2;
    X.b_ct = ct;
    X.b_zt = zt;
    set_t2(z ? z->srv : NULL);
    xtarget* v = c ? c : z;
    if (v) set_viewport(v->w, v->h);
}

/* ---- per-frame upload and constants ------------------------------------------------------------- */

typedef struct {
    uint32_t rec_first, flags, pad[2];
    float ndc_scale[2], ndc_bias[2];
    uint32_t fill[56];
} draw_consts;
_Static_assert(sizeof(draw_consts) == DRAW_CB_BYTES, "per-draw constants are one 256-byte window");

static uint8_t* cb_cpu;
static size_t cb_cpu_cap;

/* the constants of every draw of the frame, in order (slot 0: quads); target sizes as they will be
 * when the draw runs (a light pass over the target ops) */
static uint32_t build_constants(const gpu_front_frame_data* fd) {
    struct { int w, h, kx, ky; } dim[MAX_TARGETS + 1];
    for (int i = 0; i <= MAX_TARGETS; i++) {
        dim[i].w = X.T[i].tex ? X.T[i].w : 0; dim[i].h = X.T[i].tex ? X.T[i].h : 0;
        dim[i].kx = X.T[i].kx ? X.T[i].kx : 1; dim[i].ky = X.T[i].ky ? X.T[i].ky : 1;
    }
    uint32_t n = 1;
    for (size_t pos = 0; pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        if (h->op == GPU_FOP_TARGET) {
            const gpu_fop_target* o = (const gpu_fop_target*)h;
            if (o->id && o->id <= MAX_TARGETS) {
                int kx = o->kx ? o->kx : 1, ky = o->ky ? o->ky : 1;
                dim[o->id].kx = kx; dim[o->id].ky = ky;
                dim[o->id].w = o->fbw * 64 * kx;
                int hh = ((o->rows + 63) & ~63) * ky;
                if (hh > dim[o->id].h) dim[o->id].h = hh;
            }
        } else if (h->op == GPU_FOP_DRAW) n++;
        pos += h->size;
    }
    size_t bytes = (size_t)n * DRAW_CB_BYTES;
    if (bytes > cb_cpu_cap) {
        free(cb_cpu);
        cb_cpu_cap = bytes * 2;
        cb_cpu = malloc(cb_cpu_cap);
        if (!cb_cpu) { cb_cpu_cap = 0; return 0; }
    }
    memset(cb_cpu, 0, DRAW_CB_BYTES);
    uint32_t i = 1;
    for (size_t pos = 0; pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        if (h->op == GPU_FOP_DRAW) {
            const gpu_fop_draw* d = (const gpu_fop_draw*)h;
            draw_consts* k = (draw_consts*)(cb_cpu + (size_t)i * DRAW_CB_BYTES);
            memset(k, 0, 32);
            int id = d->ct ? d->ct : d->zt;
            int w = id <= MAX_TARGETS && dim[id].w ? dim[id].w : 640, hh = id <= MAX_TARGETS && dim[id].h ? dim[id].h : 448;
            int kx = id <= MAX_TARGETS ? dim[id].kx : 1, ky = id <= MAX_TARGETS ? dim[id].ky : 1;
            k->rec_first = d->rec_first;
            k->flags = d->zt != 0;
            k->ndc_scale[0] = 2.0f * (float)kx / (float)w;
            k->ndc_scale[1] = 2.0f * (float)ky / (float)hh;
            k->ndc_bias[0] = (float)kx / (float)w - 1.0f;
            k->ndc_bias[1] = (float)ky / (float)hh - 1.0f;
            i++;
        }
        pos += h->size;
    }
    return n;
}

/* ---- command execution ---------------------------------------------------------------------------- */

static void bind_frame_buffers(void) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    UINT stride = 32, off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &X.vb.buf, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(c, X.ib.buf, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetInputLayout(c, X.layout);
    ID3D11ShaderResourceView* t0 = X.vram_srv;
    ID3D11ShaderResourceView* t345[3] = { X.rec.srv, X.st.srv, X.clut_srv };
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &t0);
    ID3D11DeviceContext_PSSetShaderResources(c, 3, 3, t345);
    ID3D11DeviceContext_VSSetShaderResources(c, 3, 1, &X.rec.srv);
    ID3D11DeviceContext_RSSetState(c, X.rs);
    X.state_calls += 8;
    X.b_topo = 0;
    X.b_vs = X.b_ps = X.b_bs = X.b_ds = NULL;
    X.b_vp_w = X.b_vp_h = -1;
    memset(&X.b_sc, 0xFF, sizeof X.b_sc);
}

static void set_consts(uint32_t slot) {
    UINT first = slot * (DRAW_CB_BYTES / 16), num = DRAW_CB_BYTES / 16;
    ID3D11DeviceContext1_VSSetConstantBuffers1(gpu_rt.ctx1, 0, 1, &X.cb.buf, &first, &num);
    ID3D11DeviceContext1_PSSetConstantBuffers1(gpu_rt.ctx1, 0, 1, &X.cb.buf, &first, &num);
    X.state_calls += 2;
}

/* a quad over the whole bound target (masked clears, depth loads, Z resolves) */
static void draw_quad(void) {
    set_topo(1);
    set_vs(gpu_vs(GPU_SH_vs_quad));
    set_consts(0);
    ID3D11DeviceContext_Draw(gpu_rt.ctx, 4, 0);
    gpu_rt.n_draws++;
}

static uint64_t dbg_nosize, dbg_nocount, dbg_cls[4], dbg_tier[4];
static void exec_draw(const gpu_fop_draw* d, uint32_t slot) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    int rov = d->tier == GPU_TIER_ROV;
    xtarget* ct = tgt(d->ct);
    xtarget* sizing = ct ? ct : tgt(d->zt);
    if (!sizing) { dbg_nosize++; return; }
    if (!d->count) { dbg_nocount++; dbg_cls[d->cls & 3]++; dbg_tier[d->tier & 3]++; return; }
    if (rov) bind_rov(d->ct, d->zt);
    else bind_ff(d->ct, d->zt);
    /* texture: vram_buf (t0, bound for the frame) or a target view (t1); a view of the bound colour
     * target reads its shadow copy (feedback, 5.1) */
    int tc = tex_class(d->ps_key);
    if (tc == 2 && d->tex) {
        xtarget* t = tgt(d->tex);
        ID3D11ShaderResourceView* v = NULL;
        if (t && !t->depth) {
            if (d->tex == d->ct) {
                if (!t->shadow) {
                    D3D11_TEXTURE2D_DESC td;
                    ID3D11Texture2D_GetDesc(t->tex, &td);
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
                    memset(&sd, 0, sizeof sd);
                    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    sd.Texture2D.MipLevels = 1;
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t->shadow)))
                        ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t->shadow, &sd, &t->shadow_srv);
                }
                v = t->shadow_srv;
            } else if (d->tex != d->zt) {
                v = t->srv;
            }
        }
        set_t1(v);
    }
    int kx = sizing->kx, ky = sizing->ky;
    set_scissor(d->sc[0] * kx, d->sc[1] * ky, (d->sc[2] + 1) * kx, (d->sc[3] + 1) * ky);
    int strip = d->cls != GPU_PC_TRI;
    set_topo(strip);
    set_vs(gpu_vs(d->cls == GPU_PC_TRI ? GPU_SH_vs_tri : d->cls == GPU_PC_SPRITE ? GPU_SH_vs_sprite : GPU_SH_vs_linepix));
    set_consts(slot);
    int zout = ((d->ps_key >> 7) & 3) == 1 && !rov;
    int discard = (int)((d->ps_key >> 10) & 1) || d->split;
    if (rov) {
        set_ps(gpu_ps(ps_rov_id[tc]));
        set_depth(X.ds_off);
    } else {
        int dual = d->blend == BL_DUAL;
        set_ps(gpu_ps(ps_ff_id[dual][zout][discard][tc]));
        set_blend(blend_state(d->blend, d->wmask));
        set_depth(depth_state(d->depth));
    }
    for (int pass = 0; pass < 1 + (d->split && !rov); pass++) {
        if (pass) set_depth(depth_state(d->depth & ~1));          /* second half of the ATST split: no Z write */
        if (d->cls == GPU_PC_TRI) ID3D11DeviceContext_DrawIndexed(c, d->count, d->first, 0);
        else ID3D11DeviceContext_DrawInstanced(c, 4, d->count, 0, 0);
        gpu_rt.n_draws++;
    }
}

static void exec_clear(const gpu_fop_clear* o) {
    xtarget* t = tgt(o->target);
    if (!t) return;
    if (o->zclear) {
        if (!t->depth) return;
        float z = (float)(o->value & 0xFFFFFF) / 16777216.0f;
        ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, t->dsv, D3D11_CLEAR_DEPTH, z, 0);
        gpu_rt.n_draws++;
        return;
    }
    if (t->depth) return;
    uint32_t v = o->value;
    if ((o->mask & 15) == 15 || (o->mask & 15) == 0) {
        float f[4] = { (float)(v & 0xFF) / 255.0f, (float)((v >> 8) & 0xFF) / 255.0f, (float)((v >> 16) & 0xFF) / 255.0f, (float)(v >> 24) / 255.0f };
        ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, t->rtv, f);
        gpu_rt.n_draws++;
        return;
    }
    /* write-masked fill (the alpha reset): a quad with the mask in the blend state */
    bind_ff(o->target, 0);
    set_scissor(0, 0, t->w, t->h);
    set_ps(gpu_ps(GPU_SH_ps_flat));
    set_blend(blend_state(BL_OFF, (int)o->mask));
    set_depth(X.ds_off);
    draw_quad();
}

static void exec_shadow(const gpu_fop_shadow* o) {
    xtarget* t = tgt(o->target);
    if (!t || t->depth) return;
    if (!t->shadow) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(t->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t->shadow))) return;
        ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t->shadow, &sd, &t->shadow_srv);
    }
    int x0 = o->x0 * t->kx, y0 = o->y0 * t->ky, x1 = (o->x1 + 1) * t->kx, y1 = (o->y1 + 1) * t->ky;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (x0 >= x1 || y0 >= y1) return;
    /* the shadow may be bound as t1 by an earlier draw: it is the destination now */
    if (X.b_t1 == t->shadow_srv) set_t1(NULL);
    D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t->shadow, 0, (UINT)x0, (UINT)y0, 0,
                                              (ID3D11Resource*)t->tex, 0, &box);
    gpu_rt.n_copies++;
}

static void dispatch_on(xtarget* t, uint32_t groups) {
    if (!t || t->depth || !groups) return;
    if (X.mode) unbind_all();
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_touch), NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &t->uav, NULL);
    ID3D11DeviceContext_Dispatch(c, groups > 65535 ? 65535 : groups, 1, 1);
    ID3D11UnorderedAccessView* nu = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &nu, NULL);
    X.state_calls += 3;
    gpu_rt.n_dispatches++;
}

static void exec_load(const gpu_fop_blocks* o) {
    xtarget* t = tgt(o->target);
    if (!t || !o->n) return;
    if (!t->depth) { dispatch_on(t, o->n); return; }
    /* depth blocks: SV_Depth quad (the real one unswizzles from vram_buf) */
    if (X.b_t2) set_t2(NULL);
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 0, NULL, t->dsv);
    count_state();
    X.mode = 3;
    X.b_ct = X.b_zt = -1;
    set_viewport(t->w, t->h);
    set_scissor(0, 0, t->w, t->h);
    set_ps(gpu_ps(GPU_SH_ps_depth));
    set_depth(X.ds_write);
    draw_quad();
}

static void exec_download(const gpu_fop_blocks* o) {
    xtarget* t = tgt(o->target);
    if (!t) return;
    if (!t->stage) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(t->tex, &td);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t->stage))) return;
    }
    ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)t->stage, (ID3D11Resource*)t->tex);
    gpu_rt.n_copies++;
    D3D11_MAPPED_SUBRESOURCE m;              /* the GS thread waits for downloads (7): so does this */
    if (SUCCEEDED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)t->stage, 0, D3D11_MAP_READ, 0, &m)))
        ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)t->stage, 0);
}

static void exec_patch(const gpu_front_frame_data* fd, const gpu_fop_blocks* o) {
    const uint16_t* list = (const uint16_t*)(fd->data + o->data_off);
    const uint8_t* bytes = fd->data + o->data_off + ((o->n * 2 + 15) & ~15u);
    for (uint32_t i = 0; i < o->n;) {
        uint32_t j = i + 1;
        while (j < o->n && list[j] == list[j - 1] + 1) j++;          /* runs of consecutive blocks */
        D3D11_BOX box = { list[i] * 256u, 0, 0, (list[j - 1] + 1u) * 256u, 1, 1 };
        ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)X.vram, 0, &box, bytes + (size_t)i * 256, 0, 0);
        gpu_rt.n_uploads++;
        gpu_rt.upload_bytes += (j - i) * 256;
        i = j;
    }
}

static void exec_clut(const gpu_front_frame_data* fd, const gpu_fop_clut* o) {
    uint32_t slot = o->slot & 4095;
    D3D11_BOX box = { slot * 1024u, 0, 0, slot * 1024u + 1024u, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)X.clut, 0, &box, fd->data + o->data_off, 0, 0);
    gpu_rt.n_uploads++;
    gpu_rt.upload_bytes += 1024;
}

static void exec_rov_end(const gpu_fop_rov_end* o) {
    if (!o->zresolve) return;
    xtarget* z = tgt(o->zt);
    if (!z) return;
    /* zpatch -> DSV: a quad over the run's bbox writing SV_Depth into the writable DSV */
    if (X.b_t2) set_t2(NULL);
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 0, NULL, z->dsv);
    count_state();
    X.mode = 3;
    X.b_ct = X.b_zt = -1;
    set_viewport(z->w, z->h);
    set_scissor(o->bbox[0] * z->kx, o->bbox[1] * z->ky, (o->bbox[2] + 1) * z->kx, (o->bbox[3] + 1) * z->ky);
    set_ps(gpu_ps(GPU_SH_ps_depth));
    set_depth(X.ds_write);
    draw_quad();
}

static int exec_init(void) {
    if (X.init) return 1;
    memset(&X, 0, sizeof X);
    X.vram = raw_default(4u << 20, &X.vram_srv);
    X.clut = raw_default(4096u * 1024u, &X.clut_srv);
    if (!X.vram || !X.clut) return 0;
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    const gpu_shader_blob* vb = &gpu_shaders[GPU_SH_vs_tri];
    if (FAILED(ID3D11Device_CreateInputLayout(gpu_rt.dev, el, 4, vb->code, vb->size, &X.layout))) { gpu_log("input layout failed"); return 0; }
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(gpu_rt.dev, &rd, &X.rs))) return 0;
    X.ds_off = make_ds(0, D3D11_COMPARISON_ALWAYS, 0);
    X.ds_write = make_ds(1, D3D11_COMPARISON_ALWAYS, 1);
    /* the per-frame buffers start at a size that holds Sly's heaviest frames */
    if (!dyn_ensure(&X.vb, 8u << 20, D3D11_BIND_VERTEX_BUFFER, 0) || !dyn_ensure(&X.ib, 2u << 20, D3D11_BIND_INDEX_BUFFER, 0) ||
        !dyn_ensure(&X.rec, 8u << 20, D3D11_BIND_SHADER_RESOURCE, 1) || !dyn_ensure(&X.st, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1) ||
        !dyn_ensure(&X.cb, 4u << 20, D3D11_BIND_CONSTANT_BUFFER, 0))
        return 0;
    /* every shader object up front (no creation inside a measured frame) */
    for (int i = 0; i < GPU_SH_COUNT; i++) {
        char k = gpu_shaders[i].profile[0];
        if (k == 'v') gpu_vs(i); else if (k == 'p') gpu_ps(i); else if (k == 'c') gpu_cs(i);
    }
    X.b_ct = X.b_zt = -1;
    X.init = 1;
    return 1;
}

void gpu_exec_release(void) {
    if (!X.init) return;
    if (dbg_nosize || dbg_nocount) gpu_log("exec: %llu draws without a target, %llu empty draws (cls %llu %llu %llu %llu, tier %llu %llu)", (unsigned long long)dbg_nosize, (unsigned long long)dbg_nocount, (unsigned long long)dbg_cls[0], (unsigned long long)dbg_cls[1], (unsigned long long)dbg_cls[2], (unsigned long long)dbg_cls[3], (unsigned long long)dbg_tier[0], (unsigned long long)dbg_tier[1]);
    for (int i = 0; i <= MAX_TARGETS; i++) target_free(&X.T[i]);
    GPU_RELEASE(X.vb.buf); GPU_RELEASE(X.ib.buf); GPU_RELEASE(X.rec.srv); GPU_RELEASE(X.rec.buf);
    GPU_RELEASE(X.st.srv); GPU_RELEASE(X.st.buf); GPU_RELEASE(X.cb.buf);
    GPU_RELEASE(X.vram_srv); GPU_RELEASE(X.vram); GPU_RELEASE(X.clut_srv); GPU_RELEASE(X.clut);
    GPU_RELEASE(X.layout); GPU_RELEASE(X.rs); GPU_RELEASE(X.ds_off); GPU_RELEASE(X.ds_write);
    for (int i = 0; i < 8; i++) GPU_RELEASE(X.ds[i]);
    for (int a = 0; a < BL_N; a++) for (int b = 0; b < 16; b++) GPU_RELEASE(X.bs[a][b]);
    X.init = 0;
}

static double qpc_ms(void) {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double tsc_per_ms;

static double thread_ms(void) {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    if (!tsc_per_ms) {                   /* QueryThreadCycleTime counts TSC ticks */
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&b); while (b.QuadPart - a.QuadPart < f.QuadPart / 50);
        tsc_per_ms = (double)(__rdtsc() - t0) / ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    }
    return (double)c / tsc_per_ms;
}

void gpu_exec_frame(const void* payload) {
    const gpu_op_frame* p = payload;
    const gpu_front_frame_data* fd = &p->fd;
    if (!exec_init()) { gpu_rt.lost = 1; return; }
    thread_ms();
    gpu_stats_begin(fd->vblank, 0);
    double t0 = qpc_ms(), c0 = thread_ms();
    X.state_calls = 0;
    /* upload: vertices, indices, records, StateRecs, per-draw constants (one Map each) */
    uint32_t ncb = build_constants(fd);
    int ok = dyn_fill(&X.vb, fd->vtx, fd->vtx_bytes, D3D11_BIND_VERTEX_BUFFER, 0) &&
             dyn_fill(&X.ib, fd->idx, fd->idx_count * 4, D3D11_BIND_INDEX_BUFFER, 0) &&
             dyn_fill(&X.rec, fd->rec, fd->rec_bytes, D3D11_BIND_SHADER_RESOURCE, 1) &&
             dyn_fill(&X.st, fd->states, fd->state_count * 96, D3D11_BIND_SHADER_RESOURCE, 1) &&
             dyn_fill(&X.cb, cb_cpu, (size_t)ncb * DRAW_CB_BYTES, D3D11_BIND_CONSTANT_BUFFER, 0);
    double t1 = qpc_ms();
    if (!ok) gpu_log("frame %u: upload failed", fd->vblank);
    bind_frame_buffers();
    uint32_t slot = 1;
    for (size_t pos = 0; ok && pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        switch (h->op) {
        case GPU_FOP_TARGET: exec_target((const gpu_fop_target*)h); break;
        case GPU_FOP_RELEASE: exec_release((const gpu_fop_target*)h); break;
        case GPU_FOP_PATCH: exec_patch(fd, (const gpu_fop_blocks*)h); break;
        case GPU_FOP_CLUT: exec_clut(fd, (const gpu_fop_clut*)h); break;
        case GPU_FOP_LOAD: exec_load((const gpu_fop_blocks*)h); break;
        case GPU_FOP_DOWNLOAD: exec_download((const gpu_fop_blocks*)h); break;
        case GPU_FOP_CLEAR: exec_clear((const gpu_fop_clear*)h); break;
        case GPU_FOP_COPY_SHADOW: exec_shadow((const gpu_fop_shadow*)h); break;
        case GPU_FOP_DRAW: exec_draw((const gpu_fop_draw*)h, slot++); break;
        case GPU_FOP_ROV_END: exec_rov_end((const gpu_fop_rov_end*)h); break;
        case GPU_FOP_ALIAS16: {
            const gpu_fop_alias16* a = (const gpu_fop_alias16*)h;
            dispatch_on(tgt(a->target), a->count);
            break;
        }
        case GPU_FOP_PRESENT: break;
        default: break;
        }
        pos += h->size;
    }
    unbind_all();                        /* (targets may be released or recreated before the next frame) */
    ID3D11DeviceContext_Flush(gpu_rt.ctx);
    double t2 = qpc_ms(), c1 = thread_ms();
    gpu_frame_stats* st = gpu_stats_cur();
    st->render_cpu_ms = c1 - c0;
    st->upload_ms = t1 - t0;
    st->state_calls = X.state_calls;
    (void)t2;
    gpu_stats_end();
}

void gpu_exec_frame_done(const void* payload) {
    const gpu_op_frame* p = payload;
    gpu_front_release(p->handle);
    atomic_fetch_sub(&inflight, 1);
    WakeByAddressAll((PVOID)&inflight);
}

/* ---- producer side (GS thread): the frontend's sink -------------------------------------------------- */

void gpu_submit_sink(const gpu_front_frame_data* fd, void* user) {
    (void)user;
    if (!gpu_running()) return;
    /* at most MAX_INFLIGHT frames queued (the render thread's pace bounds the GS thread's, as VBlank
     * pacing does in the game) */
    for (;;) {
        int n = atomic_load(&inflight);
        if (n < MAX_INFLIGHT) break;
        WaitOnAddress((volatile VOID*)&inflight, &n, sizeof n, 5);
    }
    void* h = gpu_front_detach();
    if (!h) return;
    atomic_fetch_add(&inflight, 1);
    gpu_op_frame* op = gpu_cmd_alloc(GPU_OP_FRAME, sizeof *op);
    op->fd = *fd;
    op->handle = h;
    gpu_cmd_commit();
    gpu_cmd_publish();
}

void gpu_submit_wait_idle(void) {
    for (;;) {
        int n = atomic_load(&inflight);
        if (!n) break;
        WaitOnAddress((volatile VOID*)&inflight, &n, sizeof n, 5);
    }
}
