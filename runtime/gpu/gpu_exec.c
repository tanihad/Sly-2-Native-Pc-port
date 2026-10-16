/* Render-thread execution of the frontend's command stream (notes/d3d11-renderer.md 2.1, 3, 4.3;
 * milestones M2 "submission go/no-go" and M3 "REF tier core").
 *
 * The GS thread's frontend (gpu_front.c) closes a frame at every VBlank (and, for lockstep, at
 * checkpoints): commands (gpu_front.h GPU_FOP_*), 32-byte vertices, u32 indices, records, StateRecs
 * and a data arena. The sink below hands the frame's buffers to the render thread without copying
 * (gpu_front_detach); the render thread uploads them into dynamic buffers and executes every command:
 *
 *   memory      vram_buf (4 MB raw buffer, SRV + UAV) holds GS memory as the GPU knows it: patches
 *               write the uploaded bits (cs_patch), downloads the GPU words of a writer target's
 *               blocks (cs_download, sample 0); loads fill target blocks from it (cs_load); cross copies
 *               move blocks between targets of one kind and scale, every sample (cs_xcopy, M4); swz_tab
 *               holds the swizzle tables (gs_mem_swizzle.h, built from the same arrays at start) and the
 *               inverse CT32 table (16-bit views and frames over C32 targets).
 *   targets     colour R8G8B8A8_TYPELESS (raw GS bytes; RTV UNORM, SRV UNORM + UINT, UAV UINT and
 *               R32_UINT); depth D32S8 (DSV, SRV) and zpatch R32_UINT (UAV: the stored Z word); shadow
 *               copies for feedback reads (5.1).
 *   ALIAS16     (M4) a run of draws into a 16-bit frame over a C32 target: gather (target halfwords ->
 *               R32_UINT staging texture in the frame's pixel space), REF draws on the staging texture,
 *               scatter (shaders/alias16.hlsl).
 *   REF frames  (GPU_FD_REF, M3): every draw is a ROV draw through shaders/gs.hlsl: no RTV/DSV, the
 *               colour UAV and zpatch UAV rasterizer-ordered, a 16384-pixel viewport (power of two:
 *               the GS 12.4 vertices map exactly onto D3D's 1/256 grid), records and StateRecs as
 *               raw SRVs, per-draw constants through D3D11.1 constant-buffer offsets. Depth lives in
 *               zpatch for the whole frame (Z resolves are not needed). Clears are cs_clear.
 *   other       (M2 submission benchmark, -Backend hwsubmit) FF draws through trivial shaders
 *               (shaders/submit.hlsl), real state objects and bindings.
 *
 * Timing per frame (gpu_frame_stats): render_ms = wall time of the whole frame on the render thread,
 * render_cpu_ms = its thread CPU time, upload_ms = the Map/copy part; GPU time from timestamps. */
#include "gpu_internal.h"
#include "gpu_front.h"
#include "gs_mem_swizzle.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shaders.h"

/* frontend layouts (gpu_front.c / gpu_front_int.h) */
enum { XK_C32, XK_Z32, XK_Z24, XK_Z16, XK_Z16S };
enum { BL_OFF, BL_NOCOLOR, BL_ADD, BL_REVSUB, BL_DUAL, BL_N };
enum { TK_NONE, TK_VRAM_IDX, TK_VRAM_DIRECT, TK_VIEW_C32, TK_VIEW_ALPHA, TK_ALIAS16 };
#define MAX_TARGETS 64
#define DRAW_CB_BYTES 256                /* per-draw constants: one 16-constant window (CB offsets) */
#define REF_VP 16384                     /* REF viewport (power of two: exact NDC) */

typedef struct {
    gpu_front_frame_data fd;
    void* handle;                        /* gpu_front_release */
} gpu_op_frame;

/* ---- render-thread state ---------------------------------------------------------------------- */
typedef struct {
    int used, depth, kind;
    int w, h;                            /* allocated (pixels at scale) */
    int kx, ky, nat_w, nat_h;            /* requested native size and factors */
    int base, fbw, bcols;                /* GS layout: base page, width (64 px), blocks per native row */
    ID3D11Texture2D* tex;
    ID3D11RenderTargetView* rtv;
    ID3D11ShaderResourceView* srv;       /* colour UNORM / depth R32 */
    ID3D11ShaderResourceView* srv_u;     /* colour UINT (texture views) */
    ID3D11UnorderedAccessView* uav;      /* colour UINT (R8G8B8A8: mem.hlsl) */
    ID3D11UnorderedAccessView* uav32;    /* colour as R32_UINT (the raw GS word: REF pixel shader, ALIAS16 atomics) */
    ID3D11DepthStencilView *dsv, *dsv_ro;
    ID3D11Texture2D* zp;                 /* depth: zpatch (R32_UINT, the stored Z word) */
    ID3D11UnorderedAccessView* zp_uav;
    ID3D11ShaderResourceView* zp_srv;    /* (ps_zresolve) */
    uint8_t* zts;                        /* mixed frames: per tile of 16x16 native pixels ZT_SYNC / ZT_DSV / ZT_ZP */
    int zts_w, zts_h;
    ID3D11Texture2D* shadow;             /* feedback copies (5.1) */
    ID3D11ShaderResourceView *shadow_srv, *shadow_srv_u;
} xtarget;

typedef struct {
    ID3D11Buffer* buf;
    ID3D11ShaderResourceView* srv;
    size_t cap;
} dynbuf;

typedef struct {                         /* shaders/mem.hlsl, alias16.hlsl mem_cb */
    uint32_t n, list_off, data_off, mask_off, tkind, tbase, tfbw, bcols, kx, ky, value, wmask, cx1, cy1, group_base, pad;
    uint32_t cx0, cy0, psm16, pad2;
} mem_consts;
_Static_assert(sizeof(mem_consts) == 80, "mem_cb");

static struct {
    int init;
    xtarget T[MAX_TARGETS + 1];
    dynbuf vb, ib, rec, st, cb, dat, aux;
    ID3D11Buffer* vram;                  /* 4 MB raw (vram_buf) */
    ID3D11ShaderResourceView* vram_srv;
    ID3D11UnorderedAccessView* vram_uav;
    ID3D11Buffer* scratch;               /* gpu_exec_read_memory, M6 compose: a copy of vram_buf + the GPU words */
    ID3D11UnorderedAccessView* scratch_uav;
    ID3D11ShaderResourceView* scratch_srv;
    ID3D11Buffer* stage;                 /* 4 MB read-back */
    ID3D11Buffer* swz;                   /* swz_tab */
    ID3D11ShaderResourceView* swz_srv;
    ID3D11Buffer* clut;                  /* 4096 x 1 KB raw (CLUT ring) */
    ID3D11ShaderResourceView* clut_srv;
    ID3D11Buffer* mcb;                   /* mem_consts */
    ID3D11InputLayout* layout;
    ID3D11RasterizerState* rs;
    ID3D11BlendState* bs[BL_N][16];
    ID3D11DepthStencilState* ds[8];      /* depth class (ztst * 2 + zwrite) */
    ID3D11DepthStencilState* ds_off;
    ID3D11DepthStencilState* ds_write;   /* Z resolve / depth load: ALWAYS + write */
    /* bound state (redundant calls are filtered here, as a real backend does) */
    int mode;                            /* 0 none, 1 FF (RTV + DSV), 2 ROV (UAV + read-only DSV), 3 quad DSV, 4 REF (2 UAVs),
                                            5 REF on the ALIAS16 staging texture */
    int b_ct, b_zt;
    void *b_vs, *b_ps, *b_bs, *b_ds, *b_t1, *b_t2;
    int b_topo;
    D3D11_RECT b_sc;
    int b_vp_w, b_vp_h;
    uint32_t state_calls;
    int ref;                             /* the frame being executed is a REF frame */
    int mixed;                           /* ... a mixed frame (M5: FF + ROV draws, depth synced per tile) */
    ID3D11Buffer* ztile;                 /* tile states of the depth target being synced (zsync.hlsl) */
    ID3D11ShaderResourceView* ztile_srv;
    ID3D11Buffer* zcb;                   /* zsync.hlsl constants */
    int probe, probe_x, probe_y;         /* PS2_GPU_PROBE=x,y: gs.hlsl records that pixel's pipeline */
    ID3D11Buffer *dbg, *dbg_stage;
    ID3D11UnorderedAccessView* dbg_uav;
    /* ALIAS16 runs: the staging texture (R32_UINT, a 16-bit frame's pixel space at the target's samples) */
    ID3D11Texture2D* a16;
    ID3D11UnorderedAccessView* a16_uav;
    int a16_w, a16_h;
} X;

static gpu_exec_counters XC;
static atomic_int inflight;             /* frames handed over and not finished */
#define MAX_INFLIGHT 3                   /* (replays; the game sets its own depth: gpu_submit_set_depth) */
static int inflight_max = MAX_INFLIGHT;
void gpu_submit_set_depth(int frames) { inflight_max = frames < 1 ? 1 : frames > 16 ? 16 : frames; }

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
    if (!bytes) return dyn_ensure(d, 16, bind, raw);
    if (!dyn_ensure(d, bytes, bind, raw)) return 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)d->buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, src, bytes);
    ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)d->buf, 0);
    gpu_rt.n_uploads++;
    gpu_rt.upload_bytes += (uint32_t)bytes;
    return 1;
}

/* a DEFAULT raw buffer with an SRV (and a UAV when uav != NULL) */
static ID3D11Buffer* raw_default(UINT bytes, const void* init, ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uav) {
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = (srv ? D3D11_BIND_SHADER_RESOURCE : 0) | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA sd0 = { init, 0, 0 };
    ID3D11Buffer* b = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, init ? &sd0 : NULL, &b))) return NULL;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = bytes / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)b, &sd, srv))) { GPU_RELEASE(b); return NULL; }
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = bytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)b, &ud, uav))) { GPU_RELEASE(b); return NULL; }
    }
    return b;
}

static void target_free(xtarget* t) {
    GPU_RELEASE(t->rtv); GPU_RELEASE(t->srv); GPU_RELEASE(t->srv_u); GPU_RELEASE(t->uav); GPU_RELEASE(t->uav32); GPU_RELEASE(t->dsv);
    GPU_RELEASE(t->dsv_ro); GPU_RELEASE(t->zp_uav); GPU_RELEASE(t->zp_srv); GPU_RELEASE(t->zp); GPU_RELEASE(t->shadow_srv);
    GPU_RELEASE(t->shadow_srv_u); GPU_RELEASE(t->shadow); GPU_RELEASE(t->tex);
}

/* ---- mixed frames: depth in the DSV (FF draws) and in zpatch (ROV / ALIAS16 draws, memory ops) ----------
 * One state per tile of 16x16 native pixels says which copy is current (shaders/zsync.hlsl): */
enum { ZT_SYNC, ZT_DSV, ZT_ZP };         /* both / the DSV is newer / zpatch is newer */
#define ZT_SHIFT 4

/* the tile grid of an allocation of w x h samples: states kept where the grid overlaps, new tiles hold
 * only what zpatch holds (loads, copies of the old contents) */
static void zts_resize(xtarget* t, int w, int h) {
    int tw = (w / t->kx + 15) >> ZT_SHIFT, th = (h / t->ky + 15) >> ZT_SHIFT;
    uint8_t* n = malloc((size_t)tw * th);
    if (!n) { gpu_log("out of memory"); exit(1); }
    memset(n, ZT_ZP, (size_t)tw * th);
    for (int y = 0; y < th && y < t->zts_h; y++)
        for (int x = 0; x < tw && x < t->zts_w; x++) n[y * tw + x] = t->zts[y * t->zts_w + x];
    free(t->zts);
    t->zts = n;
    t->zts_w = tw;
    t->zts_h = th;
}
static void ensure_zp(xtarget* t, int x0, int y0, int x1, int y1);
static void ensure_zp_all(xtarget* t);
static int zbits_of(const xtarget* t);
static void zt_set(xtarget* t, const int r[4], int from, int to);
static void zquad(xtarget* t, const int b[4], int mode, uint32_t value);

static void unbind_all(void);

static ID3D11Texture2D* tex2d(int w, int h, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return NULL;
    return t;
}

static void copy_old(ID3D11Texture2D* nt, ID3D11Texture2D* ot, int ow, int oh, int w, int h) {
    if (!nt || !ot) return;
    D3D11_BOX box = { 0, 0, 0, (UINT)(ow < w ? ow : w), (UINT)(oh < h ? oh : h), 1 };
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)nt, 0, 0, 0, 0, (ID3D11Resource*)ot, 0, &box);
    gpu_rt.n_copies++;
}

/* (M7) old contents at new sample factors: new sample (i, j) of a native pixel <- old sample (i * okx / kx,
 * j * oky / ky) of the same pixel (shaders/rescale.hlsl); colour words, or zpatch for depth targets */
typedef struct { uint32_t okx, oky, nkx, nky, w, h, ow, oh, depth, pad[3]; } rescale_consts;
static void rescale_copy(ID3D11ShaderResourceView* src, ID3D11UnorderedAccessView* dst, int depth, int okx, int oky, int ow, int oh,
                         int nkx, int nky, int w, int h) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    rescale_consts k = { (uint32_t)okx, (uint32_t)oky, (uint32_t)nkx, (uint32_t)nky, (uint32_t)w, (uint32_t)h, (uint32_t)ow,
                         (uint32_t)oh, (uint32_t)depth, { 0, 0, 0 } };
    _Static_assert(sizeof(rescale_consts) <= sizeof(mem_consts), "rescale_cb in mem_cb");
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)X.mcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memset(m.pData, 0, sizeof(mem_consts));
    memcpy(m.pData, &k, sizeof k);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)X.mcb, 0);
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_rescale), NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &X.mcb);
    ID3D11ShaderResourceView* s[2] = { depth ? NULL : src, depth ? src : NULL };
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, s);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &dst, NULL);
    ID3D11DeviceContext_Dispatch(c, (UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);
    gpu_rt.n_dispatches++;
    ID3D11ShaderResourceView* ns[2] = { NULL, NULL };
    ID3D11UnorderedAccessView* nu = NULL;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, ns);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &nu, NULL);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &X.vram_srv);   /* (as mem_dispatch leaves it) */
}

static int target_views(xtarget* t);

/* (re)create t at w x h samples. The old contents are copied over at the same samples (growing), or, okx > 0,
 * rescaled from okx x oky samples per native pixel to t->kx x t->ky (M7: depth from zpatch, which must be
 * current: every tile of the new depth texture is then zpatch-newer) */
static int target_realloc(xtarget* t, int w, int h, int okx, int oky) {
    if (t->tex && t->w == w && t->h == h && !okx) return 1;
    unbind_all();
    ID3D11Texture2D* nt = t->depth ? tex2d(w, h, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)
                                   : tex2d(w, h, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    ID3D11Texture2D* nz = t->depth ? tex2d(w, h, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE) : NULL;
    if (!nt || (t->depth && !nz)) { GPU_RELEASE(nt); GPU_RELEASE(nz); gpu_log("cannot create a %dx%d target", w, h); return 0; }
    xtarget n;
    memset(&n, 0, sizeof n);
    n.depth = t->depth;
    n.tex = nt;
    n.zp = nz;
    if (!target_views(&n)) { target_free(&n); gpu_log("cannot create target views"); return 0; }
    if (okx && t->tex) {                 /* rescale: sample 0 of every native pixel stays sample 0 */
        if (t->depth) rescale_copy(t->zp_srv, n.zp_uav, 1, okx, oky, t->w, t->h, t->kx, t->ky, w, h);
        else rescale_copy(t->srv_u, n.uav32, 0, okx, oky, t->w, t->h, t->kx, t->ky, w, h);
        XC.rescales++;
    } else {
        copy_old(nt, t->tex, t->w, t->h, w, h);  /* grow: keep what was drawn */
        copy_old(nz, t->zp, t->w, t->h, w, h);
    }
    target_free(t);
    t->tex = n.tex; t->zp = n.zp; t->rtv = n.rtv; t->srv = n.srv; t->srv_u = n.srv_u; t->uav = n.uav; t->uav32 = n.uav32;
    t->dsv = n.dsv; t->dsv_ro = n.dsv_ro; t->zp_uav = n.zp_uav; t->zp_srv = n.zp_srv;
    t->w = w;
    t->h = h;
    if (t->depth) {
        if (okx) { free(t->zts); t->zts = NULL; t->zts_w = t->zts_h = 0; }   /* (all tiles: zpatch newer) */
        zts_resize(t, w, h);
    }
    return 1;
}

static int target_alloc(xtarget* t, int w, int h) { return target_realloc(t, w, h, 0, 0); }

/* the views of a target's textures (tex, zp) */
static int target_views(xtarget* t) {
    ID3D11Texture2D* nt = t->tex;
    ID3D11Texture2D* nz = t->zp;
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
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)nz, &ud, &t->zp_uav);
        sd.Format = DXGI_FORMAT_R32_UINT;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)nz, &sd, &t->zp_srv);
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
        sd.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)nt, &sd, &t->srv_u);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)nt, &ud, &t->uav);
        ud.Format = DXGI_FORMAT_R32_UINT;    /* (a 32-bit typeless format may be viewed as R32_UINT) */
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)nt, &ud, &t->uav32);
    }
    if (FAILED(hr)) { gpu_log("cannot create target views (hr=0x%08lX)", (unsigned long)hr); return 0; }
    return 1;
}

static int shadow_ensure(xtarget* t) {
    if (t->shadow) return 1;
    t->shadow = tex2d(t->w, t->h, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE);
    if (!t->shadow) return 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t->shadow, &sd, &t->shadow_srv);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)t->shadow, &sd, &t->shadow_srv_u);
    return t->shadow_srv && t->shadow_srv_u;
}

/* allocation granularity: 64 native rows, so growing targets are recreated rarely */
static int alloc_h(const xtarget* t) { return ((t->nat_h + 63) & ~63) * t->ky; }

static void exec_target(const gpu_fop_target* o) {
    if (o->id == 0 || o->id > MAX_TARGETS) return;
    xtarget* t = &X.T[o->id];
    int nkx = o->kx ? o->kx : 1, nky = o->ky ? o->ky : 1;
    /* (M7) new sample factors of a live target (its family or the configuration changed): rescaled */
    int okx = t->tex ? t->kx : 0, oky = t->tex ? t->ky : 0, rescale = t->tex && (okx != nkx || oky != nky);
    if (rescale && t->depth) ensure_zp_all(t);   /* (zpatch holds the truth of every tile first) */
    t->used = 1;
    t->kind = o->kind;
    t->depth = o->kind != XK_C32;
    t->kx = nkx;
    t->ky = nky;
    t->nat_w = o->fbw * 64;
    t->nat_h = o->rows;
    t->base = o->base;
    t->fbw = o->fbw;
    t->bcols = o->fbw * 64 / (o->kind == XK_Z16 || o->kind == XK_Z16S ? 16 : 8);
    int w = t->nat_w * t->kx, h = alloc_h(t);
    if (rescale) { target_realloc(t, w, h, okx, oky); return; }
    if (t->tex && t->w >= w && t->h >= h && t->w == w) return;   /* (taller allocation kept) */
    if (t->tex && t->h > h) h = t->h;
    target_alloc(t, w, h);
}

static void exec_release(const gpu_fop_target* o) {
    if (o->id == 0 || o->id > MAX_TARGETS) return;
    unbind_all();
    target_free(&X.T[o->id]);
    free(X.T[o->id].zts);
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

static int tex_kind(uint32_t ps_key) { return (int)((ps_key >> 2) & 7); }   /* TK_* */

static int tex_class(uint32_t ps_key) {
    int tk = tex_kind(ps_key);
    return tk == 0 ? 0 : tk <= 2 ? 1 : 2;
}

/* ---- binding helpers ------------------------------------------------------------------------------ */

static void unbind_all(void) {
    if (!gpu_rt.ctx) return;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* none[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(c, 1, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    ID3D11UnorderedAccessView* nu[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, nu, NULL);
    X.mode = 0;
    X.b_ct = X.b_zt = -1;
    X.b_t1 = X.b_t2 = NULL;
    X.state_calls += 3;
}

/* (w < 0: the REF viewport, -w pixels centred on the origin, so vertices at negative GS coordinates
 * are inside it too and nothing is clipped) */
static void set_viewport(int w, int h) {
    if (X.b_vp_w == w && X.b_vp_h == h) return;
    D3D11_VIEWPORT vp = { 0, 0, (FLOAT)w, (FLOAT)h, 0.0f, 1.0f };
    if (w < 0) { vp.TopLeftX = (FLOAT)(w / 2); vp.TopLeftY = (FLOAT)(h / 2); vp.Width = (FLOAT)-w; vp.Height = (FLOAT)-h; }
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
    if (X.b_t1 && ((c && (X.b_t1 == c->srv || X.b_t1 == c->srv_u)) || (z && X.b_t1 == z->srv))) set_t1(NULL);
    ID3D11RenderTargetView* rtv = c ? c->rtv : NULL;
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, c ? 1 : 0, c ? &rtv : NULL, z ? z->dsv : NULL);
    count_state();
    X.mode = 1;
    X.b_ct = ct;
    X.b_zt = zt;
    xtarget* v = c ? c : z;
    if (X.mixed) set_viewport(-REF_VP, -REF_VP);   /* (mixed frames: the REF vertex shaders, identical coverage) */
    else if (v) set_viewport(v->w, v->h);
}

/* ROV binding (M2 benchmark): colour UAV (rasterizer ordered in the shader) + read-only DSV and depth SRV of zt */
static void bind_rov(int ct, int zt) {
    xtarget *c = tgt((uint32_t)ct), *z = tgt((uint32_t)zt);
    if (c && z && (c->w != z->w || c->h != z->h)) pair_match(c, z);
    if (X.mode == 2 && X.b_ct == ct && X.b_zt == zt) return;
    if (X.b_t1 && c && (X.b_t1 == c->srv || X.b_t1 == c->srv_u)) set_t1(NULL);
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

/* REF binding: colour UAV (u0) and zpatch UAV (u1), both rasterizer ordered in gs.hlsl, no RTV/DSV */
static void bind_ref(int ct, int zt, int a16) {
    xtarget *c = tgt((uint32_t)ct), *z = tgt((uint32_t)zt);
    int mode = a16 ? 5 : 4;
    if (X.mode == mode && X.b_ct == ct && X.b_zt == zt) return;
    if (X.b_t2) set_t2(NULL);
    /* (ALIAS16: the colour target is not bound, its views stay; the staging texture is u0) */
    if (!a16 && X.b_t1 && c && (X.b_t1 == c->srv || X.b_t1 == c->srv_u)) set_t1(NULL);
    ID3D11UnorderedAccessView* uav[3] = { a16 ? X.a16_uav : c ? c->uav32 : NULL, z ? z->zp_uav : NULL, X.dbg_uav };
    ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(gpu_rt.ctx, 0, NULL, NULL, 0, X.dbg_uav ? 3 : 2, uav, NULL);
    count_state();
    X.mode = mode;
    X.b_ct = ct;
    X.b_zt = zt;
    set_viewport(-REF_VP, -REF_VP);
}

/* ---- per-frame upload and constants ------------------------------------------------------------- */

typedef struct {
    uint32_t rec_first, flags, kx, ky;
    float ndc_scale[2], ndc_bias[2];
    uint32_t vkx, vky, probe_x, probe_y;
    float one;
    uint32_t vbase, vfbw, vflags;        /* texture view target: base page, width; 1: its sample grid is the target's */
    uint32_t ff_blend, atst_mode;        /* FF draws (M5): blend class; ATST split half (1 pass draw, 2 fail draw) */
    float dual_bias;                     /* FF dual-source blends: source lowered by dual_bias / a LSB */
    uint32_t fill[45];
} draw_consts;

/* constant-buffer slots a draw uses: the two halves of an FF ATST split differ in atst_mode */
static uint32_t draw_slots(const gpu_fop_draw* d, int real) {
    return real && d->tier == GPU_TIER_FF && d->split == 3 ? 2u : 1u;
}
_Static_assert(sizeof(draw_consts) == DRAW_CB_BYTES, "per-draw constants are one 256-byte window");

static uint8_t* cb_cpu;
static size_t cb_cpu_cap;

/* the constants of every draw of the frame, in order (slot 0: quads); target sizes as they will be
 * when the draw runs (a light pass over the target ops). ref: REF or mixed frame (real drawing) */
static uint32_t build_constants(const gpu_front_frame_data* fd, int ref) {
    struct { int w, h, kx, ky, base, fbw; } dim[MAX_TARGETS + 1];
    for (int i = 0; i <= MAX_TARGETS; i++) {
        dim[i].w = X.T[i].tex ? X.T[i].w : 0; dim[i].h = X.T[i].tex ? X.T[i].h : 0;
        dim[i].kx = X.T[i].kx ? X.T[i].kx : 1; dim[i].ky = X.T[i].ky ? X.T[i].ky : 1;
        dim[i].base = X.T[i].base; dim[i].fbw = X.T[i].fbw;
    }
    uint32_t n = 1;
    for (size_t pos = 0; pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        if (h->op == GPU_FOP_TARGET) {
            const gpu_fop_target* o = (const gpu_fop_target*)h;
            if (o->id && o->id <= MAX_TARGETS) {
                int kx = o->kx ? o->kx : 1, ky = o->ky ? o->ky : 1;
                /* (exec_target: new factors reallocate at the new size; otherwise the height only grows) */
                int refactor = dim[o->id].w && (dim[o->id].kx != kx || dim[o->id].ky != ky);
                dim[o->id].kx = kx; dim[o->id].ky = ky;
                dim[o->id].base = o->base; dim[o->id].fbw = o->fbw;
                dim[o->id].w = o->fbw * 64 * kx;
                int hh = ((o->rows + 63) & ~63) * ky;
                if (refactor || hh > dim[o->id].h) dim[o->id].h = hh;
            }
        } else if (h->op == GPU_FOP_RELEASE) {
            const gpu_fop_target* o = (const gpu_fop_target*)h;
            if (o->id && o->id <= MAX_TARGETS) { dim[o->id].w = dim[o->id].h = 0; dim[o->id].kx = dim[o->id].ky = 1; }
        } else if (h->op == GPU_FOP_DRAW) n += draw_slots((const gpu_fop_draw*)h, ref);
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
            memset(k, 0, 64);
            int id = d->ct ? d->ct : d->zt;
            int w = id <= MAX_TARGETS && dim[id].w ? dim[id].w : 640, hh = id <= MAX_TARGETS && dim[id].h ? dim[id].h : 448;
            int kx = id <= MAX_TARGETS ? dim[id].kx : 1, ky = id <= MAX_TARGETS ? dim[id].ky : 1;
            k->rec_first = d->rec_first;
            k->kx = (uint32_t)kx;
            k->ky = (uint32_t)ky;
            if (ref || d->tier == GPU_TIER_ALIAS16) {   /* gs.hlsl: d3d pixel -> NDC through the power-of-two viewport */
                k->flags = (d->ct ? 1u : 0u) | (d->zt ? 2u : 0u);
                k->ndc_scale[0] = k->ndc_scale[1] = 2.0f / (float)REF_VP;
                k->ndc_bias[0] = k->ndc_bias[1] = 0.0f;   /* (viewport centred on the origin) */
                int vt = d->tex <= MAX_TARGETS ? d->tex : 0;
                k->vkx = (uint32_t)(vt ? dim[vt].kx : 1);
                k->vky = (uint32_t)(vt ? dim[vt].ky : 1);
                k->vbase = (uint32_t)(vt ? dim[vt].base : 0);
                k->vfbw = (uint32_t)(vt ? dim[vt].fbw : 0);
                k->vflags = vt && dim[vt].kx == kx && dim[vt].ky == ky ? 1u : 0u;
                k->probe_x = X.probe ? (uint32_t)X.probe_x : 0xFFFFFFFFu;
                k->probe_y = X.probe ? (uint32_t)X.probe_y : 0xFFFFFFFFu;
                k->one = 1.0f;
                k->ff_blend = d->blend;
                k->atst_mode = d->tier == GPU_TIER_FF ? (d->split == 2 ? 2u : d->split ? 1u : 0u) : 0u;
                /* dual-source source bias (gs.hlsl ps_ff): 0.496 is exact with an fp32 blender (WARP); the
                 * RTX 4060 Ti's ~fp16 blender is +-1 whatever the bias, 0.42 balances the errors */
                k->dual_bias = gpu_rt.caps.warp ? 0.49609375f : 0.42f;
                if (draw_slots(d, ref) == 2) {   /* the fail draw's copy */
                    draw_consts* k2 = (draw_consts*)(cb_cpu + (size_t)(i + 1) * DRAW_CB_BYTES);
                    memcpy(k2, k, 64 + 12);
                    k2->atst_mode = 2;
                }
            } else {
                k->flags = d->zt != 0;
                k->ndc_scale[0] = 2.0f * (float)kx / (float)w;
                k->ndc_scale[1] = 2.0f * (float)ky / (float)hh;
                k->ndc_bias[0] = (float)kx / (float)w - 1.0f;
                k->ndc_bias[1] = (float)ky / (float)hh - 1.0f;
            }
            i += draw_slots(d, ref);
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
    ID3D11ShaderResourceView* t34567[5] = { X.rec.srv, X.st.srv, X.clut_srv, X.swz_srv, X.dat.srv };
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &t0);
    ID3D11DeviceContext_PSSetShaderResources(c, 3, 5, t34567);
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

/* ---- compute: GS memory <-> targets (shaders/mem.hlsl) ----------------------------------------------- */

/* a cs over n groups (in dispatches of at most 65535 groups) with the given views; the pixel pipeline's
 * bindings are dropped first (a resource may not be an OM UAV and a CS UAV at once) */
static ID3D11UnorderedAccessView *mem_u3, *mem_u4;   /* (cs_xcopy's source target: set around its dispatch) */
static void mem_dispatch(int cs, mem_consts* k, uint32_t n, uint32_t gy, ID3D11ShaderResourceView* t0,
                         ID3D11ShaderResourceView* t7, ID3D11UnorderedAccessView* u0, ID3D11UnorderedAccessView* u1,
                         ID3D11UnorderedAccessView* u2) {
    if (!n) return;
    if (X.mode) unbind_all();
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* none2[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(c, 1, 2, none2);   /* (a target viewed as t1 may be a UAV here) */
    X.b_t1 = X.b_t2 = NULL;
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(cs), NULL, 0);
    ID3D11ShaderResourceView* srv0 = t0;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &srv0);
    ID3D11ShaderResourceView* t67[2] = { X.swz_srv, t7 };
    ID3D11DeviceContext_CSSetShaderResources(c, 6, 2, t67);
    ID3D11UnorderedAccessView* u[5] = { u0, u1, u2, mem_u3, mem_u4 };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, u, NULL);
    for (uint32_t done = 0; done < n;) {
        uint32_t g = n - done > 65535 ? 65535 : n - done;
        k->group_base = gy > 1 ? 0 : done;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)X.mcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, k, sizeof *k);
            ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)X.mcb, 0);
        }
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &X.mcb);
        ID3D11DeviceContext_Dispatch(c, g, gy, 1);
        gpu_rt.n_dispatches++;
        done += g;
        if (gy > 1) break;               /* (2D dispatches: one) */
    }
    ID3D11UnorderedAccessView* nu[5] = { NULL, NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, nu, NULL);
    ID3D11ShaderResourceView* ns[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, ns);
    /* vram_buf as a CS UAV unbinds its SRV everywhere: the pixel pipeline's t0 again */
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &X.vram_srv);
    X.state_calls += 10;
}

static void target_consts(const xtarget* t, mem_consts* k) {
    memset(k, 0, sizeof *k);
    k->tkind = (uint32_t)t->kind;
    k->tbase = (uint32_t)t->base;
    k->tfbw = (uint32_t)t->fbw;
    k->bcols = (uint32_t)t->bcols;
    k->kx = (uint32_t)t->kx;
    k->ky = (uint32_t)t->ky;
}

/* vram_buf <- uploaded bits: data = n x u16 blocks (padded to 16), n x 256 B data, n x 256 B masks */
static void exec_patch(const gpu_fop_blocks* o) {
    if (!o->n) return;
    mem_consts k;
    memset(&k, 0, sizeof k);
    k.n = o->n;
    k.list_off = o->data_off;
    k.data_off = o->data_off + ((o->n * 2 + 15) & ~15u);
    k.mask_off = k.data_off + o->n * 256;
    mem_dispatch(GPU_SH_cs_patch, &k, o->n, 1, NULL, X.dat.srv, X.vram_uav, NULL, NULL);
    XC.patches++;
}

/* mixed frames: a memory op writes zpatch (loads, copies): zpatch is made current first, then newer */
static void zp_written(xtarget* t) {
    if (!X.mixed || !t || !t->depth || !t->zts) return;
    int r[4] = { 0, 0, t->zts_w, t->zts_h };
    zt_set(t, r, -1, ZT_ZP);
}

static void exec_load(const gpu_fop_blocks* o) {
    xtarget* t = tgt(o->target);
    if (!t || !o->n) return;
    if (X.mixed) ensure_zp_all(t);
    zp_written(t);
    mem_consts k;
    target_consts(t, &k);
    k.n = o->n;
    k.list_off = o->data_off;
    mem_dispatch(GPU_SH_cs_load, &k, o->n, 1, X.vram_srv, X.dat.srv, NULL, t->depth ? NULL : t->uav, t->depth ? t->zp_uav : NULL);
    XC.loads++;
}

/* target blocks <- blocks of the target that wrote them (same kind and scale), every sample */
static void exec_xcopy(const gpu_fop_xcopy* o) {
    xtarget *t = tgt(o->target), *s = tgt(o->src);
    if (!t || !s || !o->n || t->depth != s->depth) return;
    if (X.mixed) { ensure_zp_all(s); ensure_zp_all(t); }
    zp_written(t);
    mem_consts k;
    target_consts(t, &k);
    k.n = o->n;
    k.list_off = o->data_off;
    k.value = (uint32_t)s->bcols;        /* (the source's blocks per row) */
    mem_u3 = t->depth ? NULL : s->uav;
    mem_u4 = t->depth ? s->zp_uav : NULL;
    mem_dispatch(GPU_SH_cs_xcopy, &k, o->n, 1, NULL, X.dat.srv, NULL, t->depth ? NULL : t->uav, t->depth ? t->zp_uav : NULL);
    mem_u3 = mem_u4 = NULL;
    XC.xcopies++;
}

/* the GPU words of writer t's blocks -> dst (vram_buf, or the read-back scratch copy) */
static void download_into(xtarget* t, uint32_t n, uint32_t list_off, ID3D11ShaderResourceView* list, ID3D11UnorderedAccessView* dst) {
    if (!t || !n) return;
    mem_consts k;
    target_consts(t, &k);
    k.n = n;
    k.list_off = list_off;
    mem_dispatch(GPU_SH_cs_download, &k, n, 1, NULL, list, dst, t->depth ? NULL : t->uav, t->depth ? t->zp_uav : NULL);
}

static void exec_download(const gpu_fop_blocks* o) {
    if (X.mixed) ensure_zp_all(tgt(o->target));
    download_into(tgt(o->target), o->n, o->data_off, X.dat.srv, X.vram_uav);
    XC.downloads++;
}

static void exec_clear(const gpu_fop_clear* o) {
    xtarget* t = tgt(o->target);
    if (!t || (o->zclear != 0) != (t->depth != 0)) return;
    int x1 = o->x1 > 0 ? o->x1 : t->nat_w, y1 = o->y1 > 0 ? o->y1 : t->nat_h;
    int r[4], full = 0;
    if (o->zclear && X.mixed && t->zts) {
        /* mixed frame: tiles wholly inside the rect are cleared in both copies; the others first get their
         * DSV-newer data into zpatch, so after the clear zpatch is current there (DSV-newer -> in sync) */
        r[0] = r[1] = 0;
        r[2] = x1 >> ZT_SHIFT; r[3] = y1 >> ZT_SHIFT;
        if (r[2] > t->zts_w) r[2] = t->zts_w;
        if (r[3] > t->zts_h) r[3] = t->zts_h;
        zt_set(t, r, -1, ZT_SYNC);                       /* (no conversion needed inside) */
        ensure_zp(t, 0, 0, x1 - 1, y1 - 1);
        full = x1 * t->kx >= t->w && y1 * t->ky >= t->h;
    }
    mem_consts k;
    target_consts(t, &k);
    k.value = o->value;
    k.wmask = o->zclear ? 15 : o->mask & 15;
    k.cx1 = (uint32_t)x1;
    k.cy1 = (uint32_t)y1;
    mem_dispatch(GPU_SH_cs_clear, &k, (uint32_t)(x1 + 7) / 8, (uint32_t)(y1 + 7) / 8, NULL, X.dat.srv, NULL,
                 t->depth ? NULL : t->uav, t->depth ? t->zp_uav : NULL);
    if (o->zclear && X.mixed && t->zts) {
        uint32_t zv = (uint32_t)o->value & (zbits_of(t) == 1 ? 0xFFFFu : zbits_of(t) == 2 ? 0xFFFFFFu : 0xFFFFFFFFu);
        if (full) {
            float zf;
            uint32_t enc = zv + 0x00800000u;
            memcpy(&zf, &enc, 4);
            ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, t->dsv, D3D11_CLEAR_DEPTH, zf, 0);
        } else {
            int b[4] = { 0, 0, x1 - 1, y1 - 1 };
            zquad(t, b, 1, zv);
        }
    } else if (o->zclear && !X.ref) {    /* (M2 FF path: the DSV too) */
        float z = (float)(o->value & 0xFFFFFF) / 16777216.0f;
        ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, t->dsv, D3D11_CLEAR_DEPTH, z, 0);
    }
    XC.clears++;
}

/* a quad over the whole bound target (M2: Z resolves) */
static void draw_quad(void) {
    set_topo(1);
    set_vs(gpu_vs(GPU_SH_vs_quad));
    set_consts(0);
    ID3D11DeviceContext_Draw(gpu_rt.ctx, 4, 0);
    gpu_rt.n_draws++;
}

static ID3D11ShaderResourceView* view_srv(const gpu_fop_draw* d, int uint_view) {
    xtarget* t = tgt(d->tex);
    if (!t || t->depth) return NULL;
    if (d->tier == GPU_TIER_ALIAS16)     /* (the run's draws write the staging texture: its target is free) */
        return uint_view ? t->srv_u : t->srv;
    if ((d->ps_key >> 11) & 1) return NULL;   /* the bound target itself (byte lanes it does not write): read in the shader */
    if (d->tex == d->ct) {               /* feedback: its shadow copy (5.1) */
        if (!shadow_ensure(t)) return NULL;
        return uint_view ? t->shadow_srv_u : t->shadow_srv;
    }
    if (d->tex == d->zt) return NULL;
    return uint_view ? t->srv_u : t->srv;
}

static uint64_t dbg_nosize, dbg_nocount, dbg_cls[4], dbg_tier[4];

/* ---- mixed frames: depth sync (shaders/zsync.hlsl) -----------------------------------------------------
 * A tile rect [tx0, tx1) x [ty0, ty1) of depth target t holding tiles in state `want`: the bounding box of
 * those tiles (0: none). */
typedef struct { uint32_t kx, ky, tiles_x, mode, zbits, value, x0, y0, x1, y1, pad[2]; } zs_consts;
_Static_assert(sizeof(zs_consts) == 48, "zs_cb");

static int zbits_of(const xtarget* t) { return t->kind == XK_Z24 ? 2 : t->kind == XK_Z32 ? 3 : 1; }

/* native rect (inclusive) -> tile rect [x0, x1) x [y0, y1), clipped to the grid */
static int zt_rect(const xtarget* t, int x0, int y0, int x1, int y1, int r[4]) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    r[0] = x0 >> ZT_SHIFT; r[1] = y0 >> ZT_SHIFT;
    r[2] = (x1 >> ZT_SHIFT) + 1; r[3] = (y1 >> ZT_SHIFT) + 1;
    if (r[2] > t->zts_w) r[2] = t->zts_w;
    if (r[3] > t->zts_h) r[3] = t->zts_h;
    return r[0] < r[2] && r[1] < r[3];
}

static int zt_find(const xtarget* t, const int r[4], int want, int b[4]) {
    b[0] = b[1] = 1 << 30; b[2] = b[3] = -1;
    for (int y = r[1]; y < r[3]; y++)
        for (int x = r[0]; x < r[2]; x++)
            if (t->zts[y * t->zts_w + x] == want) {
                if (x < b[0]) b[0] = x;
                if (y < b[1]) b[1] = y;
                if (x + 1 > b[2]) b[2] = x + 1;
                if (y + 1 > b[3]) b[3] = y + 1;
            }
    return b[2] > 0;
}

static void zt_set(xtarget* t, const int r[4], int from, int to) {
    for (int y = r[1]; y < r[3]; y++)
        for (int x = r[0]; x < r[2]; x++)
            if (from < 0 || t->zts[y * t->zts_w + x] == from) t->zts[y * t->zts_w + x] = (uint8_t)to;
}

static void zt_upload(const xtarget* t, zs_consts* k) {
    size_t n = (size_t)t->zts_w * t->zts_h;
    D3D11_BOX box = { 0, 0, 0, (UINT)((n + 3) & ~(size_t)3), 1, 1 };
    static uint8_t tmp[16384 + 4];
    if (n > 16384) n = 16384;
    memcpy(tmp, t->zts, n);
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)X.ztile, 0, &box, tmp, 0, 0);
    memset(k, 0, sizeof *k);
    k->kx = (uint32_t)t->kx; k->ky = (uint32_t)t->ky;
    k->tiles_x = (uint32_t)t->zts_w;
    k->zbits = (uint32_t)zbits_of(t);
}

static void zcb_set(const zs_consts* k) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)X.zcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, k, sizeof *k);
        ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)X.zcb, 0);
    }
}

/* zpatch -> DSV over tiles in state ZT_ZP (mode 0), or the constant `value` over the native rect r (mode 1:
 * a depth clear of part of the target); the quad leaves the OM bound to the DSV alone */
static void zquad(xtarget* t, const int b[4], int mode, uint32_t value) {
    zs_consts k;
    zt_upload(t, &k);
    k.mode = (uint32_t)mode;
    k.value = value;
    zcb_set(&k);
    if (X.b_t2) set_t2(NULL);
    ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 0, NULL, t->dsv);
    count_state();
    X.mode = 3;
    X.b_ct = X.b_zt = -1;
    set_viewport(t->w, t->h);
    LONG x0 = mode ? b[0] * t->kx : (b[0] << ZT_SHIFT) * t->kx, y0 = mode ? b[1] * t->ky : (b[1] << ZT_SHIFT) * t->ky;
    LONG x1 = mode ? (b[2] + 1) * t->kx : (b[2] << ZT_SHIFT) * t->kx, y1 = mode ? (b[3] + 1) * t->ky : (b[3] << ZT_SHIFT) * t->ky;
    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    set_scissor(x0, y0, x1, y1);
    set_topo(1);
    set_vs(gpu_vs(GPU_SH_vs_zquad));
    set_ps(gpu_ps(GPU_SH_ps_zresolve));
    set_depth(X.ds_write);
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 0, 1, &X.zcb);
    ID3D11ShaderResourceView* v8 = X.ztile_srv;
    ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 8, 1, &v8);
    set_t2(t->zp_srv);
    ID3D11DeviceContext_Draw(gpu_rt.ctx, 4, 0);
    set_t2(NULL);
    gpu_rt.n_draws++;
    X.state_calls += 2;
    XC.zresolves++;
    XC.zresolve_px += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
}

/* FF use of depth target t over native rect (inclusive): zpatch-newer tiles resolved into the DSV first */
static void ensure_dsv(xtarget* t, int x0, int y0, int x1, int y1) {
    int r[4], b[4];
    if (!t || !t->zts || !zt_rect(t, x0, y0, x1, y1, r) || !zt_find(t, r, ZT_ZP, b)) return;
    zquad(t, b, 0, 0);
    zt_set(t, b, ZT_ZP, ZT_SYNC);
}

/* DSV -> zpatch over the tiles in state ZT_DSV of the tile rect r */
static void zsync_dispatch(xtarget* t, const int b[4]) {
    if (X.mode) unbind_all();
    zs_consts k;
    zt_upload(t, &k);
    k.x0 = (uint32_t)((b[0] << ZT_SHIFT) * t->kx); k.y0 = (uint32_t)((b[1] << ZT_SHIFT) * t->ky);
    k.x1 = (uint32_t)((b[2] << ZT_SHIFT) * t->kx); k.y1 = (uint32_t)((b[3] << ZT_SHIFT) * t->ky);
    if (k.x1 > (uint32_t)t->w) k.x1 = (uint32_t)t->w;
    if (k.y1 > (uint32_t)t->h) k.y1 = (uint32_t)t->h;
    zcb_set(&k);
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_zsync), NULL, 0);
    ID3D11ShaderResourceView* s0 = t->srv;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &s0);
    ID3D11ShaderResourceView* s8 = X.ztile_srv;
    ID3D11DeviceContext_CSSetShaderResources(c, 8, 1, &s8);
    ID3D11UnorderedAccessView* u = t->zp_uav;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &u, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &X.zcb);
    ID3D11DeviceContext_Dispatch(c, (k.x1 - k.x0 + 7) / 8, (k.y1 - k.y0 + 7) / 8, 1);
    ID3D11UnorderedAccessView* nu = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &nu, NULL);
    ID3D11ShaderResourceView* ns = NULL;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &ns);
    gpu_rt.n_dispatches++;
    X.state_calls += 8;
    XC.zsyncs++;
    XC.zsync_px += (uint64_t)(k.x1 - k.x0) * (uint64_t)(k.y1 - k.y0);
}

/* zpatch use of depth target t over native rect (inclusive): DSV-newer tiles copied into zpatch first */
static void ensure_zp(xtarget* t, int x0, int y0, int x1, int y1) {
    int r[4], b[4];
    if (!t || !t->zts || !zt_rect(t, x0, y0, x1, y1, r) || !zt_find(t, r, ZT_DSV, b)) return;
    zsync_dispatch(t, b);
    zt_set(t, b, ZT_DSV, ZT_SYNC);
}

static void ensure_zp_all(xtarget* t) { if (t && t->depth) ensure_zp(t, 0, 0, t->w / t->kx - 1, t->h / t->ky - 1); }

/* after a write of rect (native, inclusive) through one copy: its tiles are newer there */
static void zt_mark(xtarget* t, int x0, int y0, int x1, int y1, int state) {
    int r[4];
    if (t && t->zts && zt_rect(t, x0, y0, x1, y1, r)) zt_set(t, r, -1, state);
}

/* a draw's scissor in samples: the native rectangle times the factors */
static void draw_scissor(const gpu_fop_draw* d, int kx, int ky) {
    set_scissor(d->sc[0] * kx, d->sc[1] * ky, (d->sc[2] + 1) * kx, (d->sc[3] + 1) * ky);
}

static void exec_draw_ref(const gpu_fop_draw* d, uint32_t slot) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    xtarget* sizing = tgt(d->ct) ? tgt(d->ct) : tgt(d->zt);
    if (!sizing) { dbg_nosize++; return; }
    if (!d->count) { dbg_nocount++; return; }
    xtarget* zt = X.mixed ? tgt(d->zt) : NULL;
    static int dbg_skip = -1;            /* PS2_GPU_DBG_ROVSKIP (performance experiments, mixed frames; wrong pictures):
                                            1 no ROV draw call, 2 no ROV draw at all, 3 ROV draws with a trivial shader */
    if (dbg_skip < 0) { const char* e = getenv("PS2_GPU_DBG_ROVSKIP"); dbg_skip = e ? atoi(e) : 0; }
    if (X.mixed && dbg_skip == 2 && d->tier == GPU_TIER_ROV) return;
    if (zt) ensure_zp(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3]);
    bind_ref(d->ct, d->zt, d->tier == GPU_TIER_ALIAS16);
    if (X.mixed && dbg_skip == 1 && d->tier == GPU_TIER_ROV) return;
    int tk = tex_kind(d->ps_key);
    set_t1(d->tex && (tk == TK_VIEW_C32 || tk == TK_VIEW_ALPHA || tk == TK_ALIAS16) ? view_srv(d, 1) : NULL);
    int kx = sizing->kx, ky = sizing->ky;
    draw_scissor(d, kx, ky);
    set_depth(X.ds_off);
    set_consts(slot);
    if (d->cls == GPU_PC_TRI) {
        int fast = (d->ps_key & GPU_PSK_FF_FAST) != 0;   /* (mixed default mode: 128-byte records, hardware S/T/Q) */
        set_topo(0);
        set_vs(gpu_vs(fast ? GPU_SH_vs_ff_tri : GPU_SH_vs_ref_tri));
        set_ps(gpu_ps(X.mixed && dbg_skip == 3 && fast ? GPU_SH_ps_ff_t001 : fast ? GPU_SH_ps_gs_tri_fast : GPU_SH_ps_gs_tri));
        ID3D11DeviceContext_DrawIndexed(c, d->count, d->first, 0);
    } else {
        set_topo(1);
        int sprite = d->cls == GPU_PC_SPRITE, thin = d->cls == GPU_PC_THIN;
        set_vs(gpu_vs(sprite ? GPU_SH_vs_ref_sprite : thin ? GPU_SH_vs_ref_thin : GPU_SH_vs_ref_linepix));
        set_ps(gpu_ps(sprite ? GPU_SH_ps_gs_sprite : thin ? GPU_SH_ps_gs_thin : GPU_SH_ps_gs_linepix));
        ID3D11DeviceContext_DrawInstanced(c, 4, d->count, 0, 0);
    }
    gpu_rt.n_draws++;
    XC.ref_draws++;
    if (X.mixed) XC.rov_draws++;
    if (zt && d->zw) zt_mark(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3], ZT_ZP);
}

/* FF draw (M5, 4.2): RTV + DSV, the blend and depth tests of the output merger, shaders/gs.hlsl ps_ff.
 * An ATST split (split 3) is two draws: the passing fragments with the state's Z write and colour mask,
 * then the failing ones as AFAIL says (FB_ONLY: no Z write; ZB_ONLY: no colour; RGB_ONLY: neither Z nor
 * alpha), each discarding the other half (atst_mode in the draw's constants: slot, slot + 1). */
static void exec_draw_ff(const gpu_fop_draw* d, uint32_t slot) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    xtarget* ct = tgt(d->ct);
    xtarget* zt = tgt(d->zt);
    xtarget* sizing = ct ? ct : zt;
    if (!sizing) { dbg_nosize++; return; }
    if (!d->count) { dbg_nocount++; return; }
    int zuse = zt && (d->depth >> 1) != 0;
    if (zuse) ensure_dsv(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3]);
    bind_ff(d->ct, zuse ? d->zt : 0);
    int tk = tex_kind(d->ps_key);
    set_t1(d->tex && (tk == TK_VIEW_C32 || tk == TK_VIEW_ALPHA || tk == TK_ALIAS16) ? view_srv(d, 1) : NULL);
    int kx = sizing->kx, ky = sizing->ky;
    draw_scissor(d, kx, ky);
    int dual = d->blend == BL_DUAL, zout = zuse;
    int ps;
    if (d->cls == GPU_PC_TRI) {
        set_topo(0);
        set_vs(gpu_vs(GPU_SH_vs_ff_tri));
        static const int t[2][2][2] = { { { GPU_SH_ps_ff_t000, GPU_SH_ps_ff_t001 }, { GPU_SH_ps_ff_t010, GPU_SH_ps_ff_t011 } },
                                        { { GPU_SH_ps_ff_t100, GPU_SH_ps_ff_t101 }, { GPU_SH_ps_ff_t110, GPU_SH_ps_ff_t111 } } };
        ps = t[dual][zout][(d->ps_key & GPU_PSK_FF_FAST) != 0];
    } else {
        set_topo(1);
        int sprite = d->cls == GPU_PC_SPRITE, thin = d->cls == GPU_PC_THIN;
        set_vs(gpu_vs(sprite ? GPU_SH_vs_ref_sprite : thin ? GPU_SH_vs_ref_thin : GPU_SH_vs_ref_linepix));
        static const int s[2][2] = { { GPU_SH_ps_ff_s00, GPU_SH_ps_ff_s01 }, { GPU_SH_ps_ff_s10, GPU_SH_ps_ff_s11 } };
        static const int l[2][2] = { { GPU_SH_ps_ff_l00, GPU_SH_ps_ff_l01 }, { GPU_SH_ps_ff_l10, GPU_SH_ps_ff_l11 } };
        static const int n[2][2] = { { GPU_SH_ps_ff_n00, GPU_SH_ps_ff_n01 }, { GPU_SH_ps_ff_n10, GPU_SH_ps_ff_n11 } };
        ps = sprite ? s[dual][zout] : thin ? n[dual][zout] : l[dual][zout];
    }
    set_ps(gpu_ps(ps));
    int zwrote = 0;
    for (int half = 0; half < 2; half++) {
        int depth = zuse ? d->depth : 0, wm = d->wmask;
        if (half == 0) {
            if (d->split && !(d->split & 1)) continue;
        } else {
            if (!(d->split & 2)) break;
            if (d->afail == 2) wm = 0;                       /* ZB_ONLY */
            else { depth &= ~1; if (d->afail == 3) wm &= 7; } /* FB_ONLY, RGB_ONLY */
        }
        if (!wm && !(depth & 1)) continue;
        set_consts(slot + (uint32_t)(half && d->split == 3));
        set_blend(blend_state(d->blend, wm));
        set_depth(depth_state(depth));
        if (d->cls == GPU_PC_TRI) ID3D11DeviceContext_DrawIndexed(c, d->count, d->first, 0);
        else ID3D11DeviceContext_DrawInstanced(c, 4, d->count, 0, 0);
        gpu_rt.n_draws++;
        XC.ff_draws++;
        zwrote |= depth & 1;
    }
    if (zuse && zwrote) zt_mark(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3], ZT_DSV);
}

static void exec_draw(const gpu_fop_draw* d, uint32_t slot) {
    if (X.ref || d->tier == GPU_TIER_ALIAS16) { exec_draw_ref(d, slot); return; }
    if (X.mixed) {
        if (d->tier == GPU_TIER_FF) exec_draw_ff(d, slot);
        else exec_draw_ref(d, slot);
        return;
    }
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
    if (tc == 2 && d->tex) set_t1(view_srv(d, 0));
    int kx = sizing->kx, ky = sizing->ky;
    draw_scissor(d, kx, ky);
    int strip = d->cls != GPU_PC_TRI;
    set_topo(strip);
    set_vs(gpu_vs(d->cls == GPU_PC_TRI ? GPU_SH_vs_tri : d->cls == GPU_PC_SPRITE ? GPU_SH_vs_sprite : GPU_SH_vs_linepix));
    set_consts(slot);
    int zout = ((d->ps_key >> 7) & 3) == 1 && !rov;
    int discard = (int)((d->ps_key >> 10) & 1) || d->split == 3;
    if (rov) {
        set_ps(gpu_ps(ps_rov_id[tc]));
        set_depth(X.ds_off);
    } else {
        int dual = d->blend == BL_DUAL;
        set_ps(gpu_ps(ps_ff_id[dual][zout][discard][tc]));
        set_blend(blend_state(d->blend, d->wmask));
        set_depth(depth_state(d->depth));
    }
    for (int pass = 0; pass < 1 + (d->split == 3 && !rov); pass++) {
        if (pass) set_depth(depth_state(d->depth & ~1));          /* second half of the ATST split: no Z write */
        if (d->cls == GPU_PC_TRI) ID3D11DeviceContext_DrawIndexed(c, d->count, d->first, 0);
        else ID3D11DeviceContext_DrawInstanced(c, 4, d->count, 0, 0);
        gpu_rt.n_draws++;
    }
}

static void exec_shadow(const gpu_fop_shadow* o) {
    xtarget* t = tgt(o->target);
    if (!t || t->depth) return;
    if (!shadow_ensure(t)) return;
    int x0 = o->x0 * t->kx, y0 = o->y0 * t->ky, x1 = (o->x1 + 1) * t->kx, y1 = (o->y1 + 1) * t->ky;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (x0 >= x1 || y0 >= y1) return;
    /* the shadow may be bound as t1 by an earlier draw: it is the destination now */
    if (X.b_t1 == t->shadow_srv || X.b_t1 == t->shadow_srv_u) set_t1(NULL);
    D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)t->shadow, 0, (UINT)x0, (UINT)y0, 0,
                                              (ID3D11Resource*)t->tex, 0, &box);
    gpu_rt.n_copies++;
}

/* ALIAS16 run begin (gather: target halfwords -> staging) / end (scatter: back), shaders/alias16.hlsl */
static void exec_alias16(const gpu_fop_alias16* o, int end) {
    xtarget* t = tgt(o->target);
    if (!t || t->depth || o->x1 <= o->x0 || o->y1 <= o->y0) return;
    int w = t->w, h = 2 * t->h;          /* (16-bit rows: two per C32 row) */
    if (!end && (!X.a16 || X.a16_w < w || X.a16_h < h)) {
        unbind_all();
        GPU_RELEASE(X.a16_uav);
        GPU_RELEASE(X.a16);
        if (w < X.a16_w) w = X.a16_w;
        if (h < X.a16_h) h = X.a16_h;
        X.a16 = tex2d(w, h, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (!X.a16 || FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)X.a16, &ud, &X.a16_uav))) {
            gpu_log("cannot create the %dx%d ALIAS16 staging texture", w, h);
            GPU_RELEASE(X.a16);
            X.a16_w = X.a16_h = 0;
            return;
        }
        X.a16_w = w;
        X.a16_h = h;
    }
    if (!X.a16_uav) return;
    mem_consts k;
    target_consts(t, &k);
    k.cx0 = (uint32_t)o->x0; k.cy0 = (uint32_t)o->y0; k.cx1 = (uint32_t)o->x1; k.cy1 = (uint32_t)o->y1;
    k.psm16 = o->psm;
    uint32_t wx = (uint32_t)(o->x1 - o->x0);
    if (end) wx /= 2;                    /* (scatter: a thread per word, two 16-bit pixels) */
    mem_dispatch(end ? GPU_SH_cs_a16_scatter : GPU_SH_cs_a16_gather, &k, (wx + 7) / 8,
                 (uint32_t)(o->y1 - o->y0 + 7) / 8, NULL, X.dat.srv, X.a16_uav, t->uav, NULL);
    if (end) XC.alias16_runs++;
}

static void exec_clut(const gpu_front_frame_data* fd, const gpu_fop_clut* o) {
    uint32_t slot = o->slot & 4095;
    D3D11_BOX box = { slot * 1024u, 0, 0, slot * 1024u + 1024u, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)X.clut, 0, &box, fd->data + o->data_off, 0, 0);
    gpu_rt.n_uploads++;
    gpu_rt.upload_bytes += 1024;
}

static void exec_rov_end(const gpu_fop_rov_end* o) {
    if (!o->zresolve || X.ref || X.mixed) return;   /* (REF: depth stays in zpatch; mixed: resolved per tile when
                                                       an FF draw needs it, ensure_dsv) */
    xtarget* z = tgt(o->zt);
    if (!z) return;
    /* M2: zpatch -> DSV, a quad over the run's bbox writing SV_Depth into the writable DSV */
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

/* swz_tab: the swizzle tables of gs_mem_swizzle.h as u32 entries (shaders/gsmem.hlsli SWZ_*) */
#define SWZ_ENTRIES 47104                /* shaders/gsmem.hlsli */
static uint32_t* swz_build(void) {
    uint32_t* t = malloc(SWZ_ENTRIES * 4);
    if (!t) return NULL;
    uint32_t o = 0;
    for (int i = 0; i < 2048; i++) t[o++] = gs_swz_ct32[i];
    for (int i = 0; i < 2048; i++) t[o++] = gs_swz_z32[i];
    for (int i = 0; i < 4096; i++) t[o++] = gs_swz_ct16[i];
    for (int i = 0; i < 4096; i++) t[o++] = gs_swz_ct16s[i];
    for (int i = 0; i < 4096; i++) t[o++] = gs_swz_z16[i];
    for (int i = 0; i < 4096; i++) t[o++] = gs_swz_z16s[i];
    for (int i = 0; i < 8192; i++) t[o++] = gs_swz_t8[i];
    for (int i = 0; i < 16384; i++) t[o++] = gs_swz_t4[i];
    for (int i = 0; i < 2048; i++) t[o + gs_swz_ct32[i]] = (uint32_t)(i & 63) | (uint32_t)(i >> 6) << 8;   /* SWZ_ICT32 */
    return t;
}

static int exec_init(void) {
    if (X.init) return 1;
    memset(&X, 0, sizeof X);
    X.vram = raw_default(4u << 20, NULL, &X.vram_srv, &X.vram_uav);
    X.scratch = raw_default(4u << 20, NULL, &X.scratch_srv, &X.scratch_uav);
    X.clut = raw_default(4096u * 1024u, NULL, &X.clut_srv, NULL);
    uint32_t* swz = swz_build();
    X.swz = swz ? raw_default(SWZ_ENTRIES * 4u, swz, &X.swz_srv, NULL) : NULL;
    free(swz);
    if (!X.vram || !X.scratch || !X.clut || !X.swz) { gpu_log("exec: cannot create the memory buffers"); return 0; }
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 4u << 20;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &X.stage))) return 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(mem_consts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &X.mcb))) return 0;
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
    X.ztile = raw_default(16384, NULL, &X.ztile_srv, NULL);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(zs_consts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!X.ztile || FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &X.zcb))) return 0;
    /* the per-frame buffers start at a size that holds Sly's heaviest frames */
    if (!dyn_ensure(&X.vb, 8u << 20, D3D11_BIND_VERTEX_BUFFER, 0) || !dyn_ensure(&X.ib, 2u << 20, D3D11_BIND_INDEX_BUFFER, 0) ||
        !dyn_ensure(&X.rec, 8u << 20, D3D11_BIND_SHADER_RESOURCE, 1) || !dyn_ensure(&X.st, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1) ||
        !dyn_ensure(&X.cb, 4u << 20, D3D11_BIND_CONSTANT_BUFFER, 0) || !dyn_ensure(&X.dat, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1) ||
        !dyn_ensure(&X.aux, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1))
        return 0;
    /* every shader object up front (no creation inside a measured frame) */
    for (int i = 0; i < GPU_SH_COUNT; i++) {
        char k = gpu_shaders[i].profile[0];
        if (k == 'v') gpu_vs(i); else if (k == 'p') gpu_ps(i); else if (k == 'c') gpu_cs(i);
    }
    X.b_ct = X.b_zt = -1;
    const char* pr = getenv("PS2_GPU_PROBE");
    if (pr && sscanf(pr, "%d,%d", &X.probe_x, &X.probe_y) == 2) {
        X.dbg = raw_default(64u * 256u, NULL, NULL, &X.dbg_uav);
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 64u * 256u;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (X.dbg && SUCCEEDED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &X.dbg_stage))) X.probe = 1;
        gpu_log("probe pixel %d,%d: %s", X.probe_x, X.probe_y, X.probe ? "on" : "unavailable");
    }
    X.init = 1;
    return 1;
}

/* PS2_GPU_PROBE: the records gs.hlsl wrote for the probe pixel during this frame */
static void probe_report(const gpu_front_frame_data* fd) {
    if (!X.probe) return;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    unbind_all();
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)X.dbg_stage, (ID3D11Resource*)X.dbg);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)X.dbg_stage, 0, D3D11_MAP_READ, 0, &m))) return;
    const uint32_t* d = m.pData;
    uint32_t n = d[0] < 255 ? d[0] : 255;
    static const char* kinds[4] = { "?", "texture", "write", "Z" };
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t* r = d + 16 + i * 16;
        float s, t, q;
        memcpy(&s, &r[8], 4); memcpy(&t, &r[9], 4); memcpy(&q, &r[10], 4);
        uint32_t k = r[0] & 3;
        if (k == 1)
            gpu_log("probe v%u%s %s rec %u state %u: U %d V %d colour %08X (vertex/record %08X) z %08X s %.9g t %.9g q %.9g xl %d [s %08X t %08X q %08X]",
                    fd->vblank, fd->flags & GPU_FD_PARTIAL ? "p" : "", kinds[k], r[1], r[2], (int)r[3], (int)r[4], r[5], r[6], r[7],
                    s, t, q, (int)r[11], r[8], r[9], r[10]);
        else if (k == 3)
            gpu_log("probe v%u%s %s rec %u: zv %08X zold %08X after %08X written/zinfo/flags %X mode %d", fd->vblank, fd->flags & GPU_FD_PARTIAL ? "p" : "",
                    kinds[k], r[1], r[7], r[12], r[13], r[6], (int)r[11]);
        else
            gpu_log("probe v%u%s %s rec %u: colour %08X Cd %08X written %08X mode %d at sample (%u,%u)", fd->vblank,
                    fd->flags & GPU_FD_PARTIAL ? "p" : "", kinds[k], r[1], r[5], r[13], r[14], (int)r[11], r[15] & 0xFFFF, r[15] >> 16);
    }
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)X.dbg_stage, 0);
    static const uint32_t zero[4] = { 0, 0, 0, 0 };
    D3D11_BOX box = { 0, 0, 0, 16, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)X.dbg, 0, &box, zero, 0, 0);
}

void gpu_exec_release(void) {
    if (!X.init) return;
    if (dbg_nosize || dbg_nocount) gpu_log("exec: %llu draws without a target, %llu empty draws (cls %llu %llu %llu %llu, tier %llu %llu)", (unsigned long long)dbg_nosize, (unsigned long long)dbg_nocount, (unsigned long long)dbg_cls[0], (unsigned long long)dbg_cls[1], (unsigned long long)dbg_cls[2], (unsigned long long)dbg_cls[3], (unsigned long long)dbg_tier[0], (unsigned long long)dbg_tier[1]);
    for (int i = 0; i <= MAX_TARGETS; i++) { target_free(&X.T[i]); free(X.T[i].zts); X.T[i].zts = NULL; }
    GPU_RELEASE(X.ztile_srv); GPU_RELEASE(X.ztile); GPU_RELEASE(X.zcb);
    dynbuf* d[7] = { &X.vb, &X.ib, &X.rec, &X.st, &X.cb, &X.dat, &X.aux };
    for (int i = 0; i < 7; i++) { GPU_RELEASE(d[i]->srv); GPU_RELEASE(d[i]->buf); d[i]->cap = 0; }
    GPU_RELEASE(X.vram_srv); GPU_RELEASE(X.vram_uav); GPU_RELEASE(X.vram); GPU_RELEASE(X.scratch_uav); GPU_RELEASE(X.scratch_srv);
    GPU_RELEASE(X.scratch);
    GPU_RELEASE(X.stage); GPU_RELEASE(X.swz_srv); GPU_RELEASE(X.swz); GPU_RELEASE(X.clut_srv); GPU_RELEASE(X.clut);
    GPU_RELEASE(X.dbg_uav); GPU_RELEASE(X.dbg); GPU_RELEASE(X.dbg_stage); GPU_RELEASE(X.a16_uav); GPU_RELEASE(X.a16);
    GPU_RELEASE(X.mcb); GPU_RELEASE(X.layout); GPU_RELEASE(X.rs); GPU_RELEASE(X.ds_off); GPU_RELEASE(X.ds_write);
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

/* M6: the VBlank's picture (GPU_FOP_PRESENT with a display info): compose from the targets / vram_buf,
 * read back if asked, present, report */
static int present_done;                 /* the frame's PRESENT was handled (gpu_exec_frame_done checks) */
static int present_bad_src;

static void exec_present(const gpu_front_frame_data* fd, const gpu_fop_present* o) {
    if (o->di_off == ~0u || o->di_off + sizeof(gpu_display_info) > fd->data_bytes) return;
    const gpu_display_info* di = (const gpu_display_info*)(fd->data + o->di_off);
    unbind_all();
    gpu_compose_in in;
    memset(&in, 0, sizeof in);
    in.priv = di->priv;
    in.vram = X.vram_srv;
    in.swz = X.swz_srv;
    in.field_code = di->field_code;
    in.scale = o->h.size >= sizeof *o && o->scale ? o->scale : 1;
    in.flicker_off = (di->gfx & GPU_GFX_FLICKER_OFF) != 0;
    in.fields = (di->gfx & GPU_GFX_FIELDS) != 0;
    if (o->mem_n && o->mem_off + o->mem_n * (sizeof(gpu_dl_entry) + 2) <= fd->data_bytes) {
        /* circuits shown from memory: vram_buf + the GPU words of their rects, in a scratch copy */
        const uint16_t* w = (const uint16_t*)(fd->data + o->mem_off + o->mem_n * sizeof(gpu_dl_entry));
        ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)X.scratch, (ID3D11Resource*)X.vram);
        gpu_rt.n_copies++;
        for (uint32_t i = 0; i < o->mem_n;) {
            uint32_t j = i;
            while (j < o->mem_n && w[j] == w[i]) j++;
            xtarget* t = tgt(w[i]);
            if (t && t->depth) ensure_zp_all(t);
            download_into(t, j - i, o->mem_off + i * (uint32_t)sizeof(gpu_dl_entry), X.dat.srv, X.scratch_uav);
            i = j;
        }
        unbind_all();
        in.vram = X.scratch_srv;
    }
    for (int i = 0; i < 2; i++) {
        if (!o->src[i]) continue;
        xtarget* t = tgt(o->src[i]);
        if (t && !t->depth && t->srv_u) {
            in.tex[i] = t->srv_u;
            in.kx[i] = t->kx;
            in.ky[i] = t->ky;
        } else if (!present_bad_src++) {
            gpu_log("present: display source target %u missing (composed from vram_buf)", o->src[i]);
        }
    }
    gpu_present_compose(&in);
    if (di->rb) gpu_present_readback((gpu_readback*)di->rb);
    /* (the picture was reported when its frame was queued: gpu_submit_sink, GPU_FD_SIGNAL) */
    if (di->flags & GPU_DI_SHOW) gpu_present_show(di->aspect_w, di->aspect_h, (int)((di->gfx >> GPU_GFX_SCALER_SHIFT) & 15));
    present_done = 1;
}

void gpu_exec_reset(void) {
    if (!gpu_running()) return;
    gpu_run_sync((void (*)(void*))gpu_exec_release, NULL);   /* (exec_init recreates everything on the next frame) */
}

void gpu_exec_frame(const void* payload) {
    const gpu_op_frame* p = payload;
    const gpu_front_frame_data* fd = &p->fd;
    if (!exec_init()) { gpu_rt.lost = 1; return; }
    thread_ms();
    gpu_stats_begin(fd->vblank, 0);
    double t0 = qpc_ms(), c0 = thread_ms();
    X.state_calls = 0;
    uint64_t ff0 = XC.ff_draws, rov0 = XC.rov_draws;
    X.ref = (fd->flags & GPU_FD_REF) != 0;
    X.mixed = !X.ref && (fd->flags & GPU_FD_MIXED) != 0;
    XC.frames++;
    if (fd->flags & GPU_FD_PARTIAL) XC.partial_frames++;
    /* upload: vertices, indices, records, StateRecs, the data arena, per-draw constants (one Map each) */
    uint32_t ncb = build_constants(fd, X.ref || X.mixed);
    int ok = dyn_fill(&X.vb, fd->vtx, fd->vtx_bytes, D3D11_BIND_VERTEX_BUFFER, 0) &&
             dyn_fill(&X.ib, fd->idx, fd->idx_count * 4, D3D11_BIND_INDEX_BUFFER, 0) &&
             dyn_fill(&X.rec, fd->rec, fd->rec_bytes, D3D11_BIND_SHADER_RESOURCE, 1) &&
             dyn_fill(&X.st, fd->states, fd->state_count * GPU_STATE_BYTES, D3D11_BIND_SHADER_RESOURCE, 1) &&
             dyn_fill(&X.dat, fd->data, fd->data_bytes, D3D11_BIND_SHADER_RESOURCE, 1) &&
             dyn_fill(&X.cb, cb_cpu, (size_t)ncb * DRAW_CB_BYTES, D3D11_BIND_CONSTANT_BUFFER, 0);
    double t1 = qpc_ms();
    if (!ok) gpu_log("frame %u: upload failed", fd->vblank);
    bind_frame_buffers();
    uint32_t slot = 1;
    for (size_t pos = 0; ok && pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        if (X.probe && h->op != GPU_FOP_DRAW && h->op != GPU_FOP_CLUT && h->op != GPU_FOP_PATCH)   /* (debugging: the memory ops) */
            gpu_log("exec v%u%s op %u target %u", fd->vblank, fd->flags & GPU_FD_PARTIAL ? "p" : "", h->op, ((const uint16_t*)h)[2]);
        switch (h->op) {
        case GPU_FOP_TARGET: exec_target((const gpu_fop_target*)h); break;
        case GPU_FOP_RELEASE: exec_release((const gpu_fop_target*)h); break;
        case GPU_FOP_PATCH: exec_patch((const gpu_fop_blocks*)h); break;
        case GPU_FOP_CLUT: exec_clut(fd, (const gpu_fop_clut*)h); break;
        case GPU_FOP_LOAD: exec_load((const gpu_fop_blocks*)h); break;
        case GPU_FOP_XCOPY: exec_xcopy((const gpu_fop_xcopy*)h); break;
        case GPU_FOP_DOWNLOAD: exec_download((const gpu_fop_blocks*)h); break;
        case GPU_FOP_CLEAR: exec_clear((const gpu_fop_clear*)h); break;
        case GPU_FOP_COPY_SHADOW: exec_shadow((const gpu_fop_shadow*)h); break;
        case GPU_FOP_DRAW: exec_draw((const gpu_fop_draw*)h, slot); slot += draw_slots((const gpu_fop_draw*)h, X.ref || X.mixed); break;
        case GPU_FOP_ROV_END: exec_rov_end((const gpu_fop_rov_end*)h); break;
        case GPU_FOP_ALIAS16: exec_alias16((const gpu_fop_alias16*)h, 0); break;
        case GPU_FOP_ALIAS16_END: exec_alias16((const gpu_fop_alias16*)h, 1); break;
        case GPU_FOP_PRESENT: exec_present(fd, (const gpu_fop_present*)h); break;
        default: break;
        }
        pos += h->size;
    }
    unbind_all();                        /* (targets may be released or recreated before the next frame) */
    probe_report(fd);
    ID3D11DeviceContext_Flush(gpu_rt.ctx);
    double t2 = qpc_ms(), c1 = thread_ms();
    gpu_frame_stats* st = gpu_stats_cur();
    st->render_cpu_ms = c1 - c0;
    st->upload_ms = t1 - t0;
    st->state_calls = X.state_calls;
    st->partial = (fd->flags & GPU_FD_PARTIAL) != 0;
    st->ff_draws = (uint32_t)(XC.ff_draws - ff0);
    st->rov_draws = (uint32_t)(XC.rov_draws - rov0);
    (void)t2;
    gpu_stats_end();
}

void gpu_exec_frame_done(const void* payload) {
    const gpu_op_frame* p = payload;
    if (!present_done) {                 /* not executed (device lost): the VBlank is still reported, in order */
        const gpu_front_frame_data* fd = &p->fd;
        for (size_t pos = 0; pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
            const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
            if (!h->size) break;
            const gpu_fop_present* o = (const gpu_fop_present*)h;
            if (h->op == GPU_FOP_PRESENT && o->di_off != ~0u && o->di_off + sizeof(gpu_display_info) <= fd->data_bytes) {
                const gpu_display_info* di = (const gpu_display_info*)(fd->data + o->di_off);
                if (di->rb) gpu_present_readback((gpu_readback*)di->rb);   /* (fails: lost) */
            }
            pos += h->size;
        }
    }
    present_done = 0;
    gpu_front_release(p->handle);
    atomic_fetch_sub(&inflight, 1);
    WakeByAddressAll((PVOID)&inflight);
}

/* ---- GS memory read-back (lockstep, reference compose) ------------------------------------------------ */

typedef struct {
    const gpu_dl_entry* e;
    const uint16_t* w;
    int n;
    uint8_t* out;
    int ok;
} readmem_args;

static void readmem_fn(void* arg) {
    readmem_args* a = arg;
    a->ok = 0;
    if (!exec_init()) return;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    unbind_all();
    for (int i = 1; i <= MAX_TARGETS; i++) ensure_zp_all(tgt((uint32_t)i));   /* (mixed frames: DSV-newer depth) */
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)X.scratch, (ID3D11Resource*)X.vram);
    /* entries grouped by writer, uploaded once; one download per writer into the scratch copy */
    gpu_dl_entry* sorted = malloc((size_t)(a->n ? a->n : 1) * sizeof *sorted);
    if (!sorted) return;
    int ns = 0;
    struct { uint16_t w; int first, count; } grp[MAX_TARGETS + 1];
    int ng = 0;
    for (int w = 1; w <= MAX_TARGETS; w++) {
        int first = ns;
        for (int i = 0; i < a->n; i++) if (a->w[i] == w) sorted[ns++] = a->e[i];
        if (ns > first) { grp[ng].w = (uint16_t)w; grp[ng].first = first; grp[ng].count = ns - first; ng++; }
    }
    int ok = dyn_fill(&X.aux, sorted, (size_t)ns * sizeof *sorted, D3D11_BIND_SHADER_RESOURCE, 1);
    free(sorted);
    if (!ok) return;
    for (int g = 0; g < ng; g++)
        download_into(tgt(grp[g].w), (uint32_t)grp[g].count, (uint32_t)(grp[g].first * (int)sizeof(gpu_dl_entry)), X.aux.srv, X.scratch_uav);
    if (X.probe) {                       /* (debug: the probe pixel in every target) */
        for (int i = 1; i <= MAX_TARGETS; i++) {
            xtarget* t = tgt((uint32_t)i);
            if (!t || X.probe_x * t->kx >= t->w || X.probe_y * t->ky >= t->h) continue;
            D3D11_TEXTURE2D_DESC td;
            memset(&td, 0, sizeof td);
            td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.SampleDesc.Count = 1;
            td.Format = t->depth ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8G8B8A8_TYPELESS;
            td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D* st = NULL;
            if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &st))) continue;
            D3D11_BOX box = { (UINT)(X.probe_x * t->kx), (UINT)(X.probe_y * t->ky), 0, (UINT)(X.probe_x * t->kx + 1), (UINT)(X.probe_y * t->ky + 1), 1 };
            ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource*)st, 0, 0, 0, 0, (ID3D11Resource*)(t->depth ? t->zp : t->tex), 0, &box);
            D3D11_MAPPED_SUBRESOURCE pm;
            if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &pm))) {
                gpu_log("probe read-back: target %d (%s base %03X) at the probe pixel: %08X", i, t->depth ? "Z" : "C", t->base,
                        *(const uint32_t*)pm.pData);
                ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)st, 0);
            }
            GPU_RELEASE(st);
        }
    }
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)X.stage, (ID3D11Resource*)X.scratch);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)X.stage, 0, D3D11_MAP_READ, 0, &m))) return;
    memcpy(a->out, m.pData, 4u << 20);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)X.stage, 0);
    a->ok = 1;
}

int gpu_exec_read_memory(const gpu_dl_entry* entries, const uint16_t* writers, int n, uint8_t* out) {
    readmem_args a = { entries, writers, n, out, 0 };
    gpu_run_sync(readmem_fn, &a);
    return a.ok;
}

void gpu_exec_get_counters(gpu_exec_counters* out) { *out = XC; }

/* ---- producer side (GS thread): the frontend's sink -------------------------------------------------- */

void gpu_submit_sink(const gpu_front_frame_data* fd, void* user) {
    (void)user;
    if (!gpu_running()) {                /* (no render thread: the picture is still reported) */
        if (fd->flags & GPU_FD_SIGNAL) gpu_present_signal(fd->vblank);
        return;
    }
    /* at most MAX_INFLIGHT frames queued (the render thread's pace bounds the GS thread's, as VBlank
     * pacing does in the game) */
    for (;;) {
        int n = atomic_load(&inflight);
        if (n < inflight_max) break;
        WaitOnAddress((volatile VOID*)&inflight, &n, sizeof n, 5);
    }
    void* h = gpu_front_detach();
    if (!h) return;
    atomic_fetch_add(&inflight, 1);
    gpu_cmd_lock();
    gpu_op_frame* op = gpu_cmd_alloc(GPU_OP_FRAME, sizeof *op);
    op->fd = *fd;
    op->handle = h;
    gpu_cmd_commit();
    gpu_cmd_publish();
    gpu_cmd_unlock();
    /* M6: the VBlank's picture is reported once its frame is queued: the EE then waits only while the
     * frontend is behind or the queue is full (a slow GPU: up to gpu_submit_set_depth frames absorb it),
     * never for the GPU itself or for Present (measured: an EE waiting for the render thread missed VBlanks
     * whenever NVIDIA's power management slowed a lightly loaded GPU down) */
    if (fd->flags & GPU_FD_SIGNAL) gpu_present_signal(fd->vblank);
}

void gpu_submit_wait_idle(void) {
    for (;;) {
        int n = atomic_load(&inflight);
        if (!n) break;
        WaitOnAddress((volatile VOID*)&inflight, &n, sizeof n, 5);
    }
}
