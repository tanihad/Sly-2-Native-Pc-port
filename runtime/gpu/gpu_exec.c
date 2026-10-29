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
#include "r2/r2_render.h"
#include <psapi.h>

/* (M9) PS2_GS_MEM_MAP: the process's committed memory at steps of the render thread's set-up */
static void mem_note(const char* what) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS2_GS_MEM_MAP"); on = e && atoi(e) > 0; }
    if (!on) return;
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    uint64_t vl = 0, vn = 0;
    gpu_video_memory(&vl, &vn);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
        gpu_log("memory %s: committed %.0f MB (peak %.0f), video %.0f MB", what, pm.PrivateUsage / 1048576.0,
                pm.PeakPagefileUsage / 1048576.0, vl / 1048576.0);
}

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
    double t_queued;                     /* (M7) qpc_ms when gpu_submit_sink queued it (latency statistics) */
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
    size_t hi;                           /* (M9) the largest fill of the current window */
    uint32_t fills;                      /* (M9) fills in the current window */
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
    dynbuf dc;                           /* (M9) the per-draw constants as a raw buffer (gs.hlsl load_draw, t9) */
    ID3D11Buffer* iota;                  /* (M9) uint i at element i: the DRAWID per-instance element (vertex buffer slot 1) */
    uint32_t iota_n;
    int clearview;                       /* (M9) ID3D11DeviceContext1::ClearView supported (D3D11_FEATURE_D3D11_OPTIONS) */
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
    /* (perf-p2) the open ALIAS16 run is a copy run (cs_a16_copy per draw): its target, 16-bit PSM */
    int a16c_on, a16c_target, a16c_psm;
    ID3D11Buffer* a16cb;                 /* cs_a16_copy constants (a16c_consts) */
    ID3D11Buffer* npcb;                  /* (r2-m7) cs_npost constants (npost_consts) */
    ID3D11Texture2D* svcnt;              /* (r2-m7) shadow volumes: per sample sum of G steps (R32_UINT, zero between uses) */
    ID3D11UnorderedAccessView* svcnt_uav;
    int svcnt_w, svcnt_h;
    int sv_on, sv_ct, sv_bb[4];          /* counters pending for colour target sv_ct over native rect sv_bb (inclusive) */
    int sv_ez;                           /* the bound volume draw state has the read-only DSV */
    const gpu_front_frame_data* fd;      /* the frame being executed (its records, for the copy runs' tile tables) */
    uint32_t curpos;                     /* (r2-m7b) byte position of the op being executed */
} X;

static gpu_exec_counters XC;
static atomic_int inflight;             /* frames handed over and not finished */
#define MAX_INFLIGHT 3                   /* (replays; the game sets its own depth: gpu_submit_set_depth) */
static int inflight_max = MAX_INFLIGHT;
void gpu_submit_set_depth(int frames) { inflight_max = frames < 1 ? 1 : frames > 16 ? 16 : frames; }
/* (M9) adaptive queue depth (gpu_submit_sink): base frames normally, up to the depth above for ADAPT_MS after a slowdown */
#define ADAPT_MS 2000.0
static int inflight_base;                /* 0: the fixed depth */
static double q_short_since = -1, absorb_until;
static volatile LONG queue_absorbs;
void gpu_submit_set_adaptive(int base) { inflight_base = base < 0 ? 0 : base; q_short_since = -1; absorb_until = 0; }
uint32_t gpu_submit_absorbs(void) { return (uint32_t)queue_absorbs; }
static volatile double stall_ms;         /* (pc-pacing) producer waits for room in the queue (only the sink writes it) */
double gpu_submit_stall_ms(void) { return stall_ms; }
int gpu_submit_queued(void) { return atomic_load(&inflight); }

static void count_state(void) { X.state_calls++; }

/* ---- resources ---------------------------------------------------------------------------------- */

#define DYN_MIN (256u << 10)
static int dyn_ensure(dynbuf* d, size_t bytes, UINT bind, int raw) {
    if (d->buf && d->cap >= bytes) return 1;
    GPU_RELEASE(d->srv);
    GPU_RELEASE(d->buf);
    size_t cap = d->cap ? d->cap : DYN_MIN;
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

/* (M9) Every fill maps the buffer with DISCARD, and the driver renames it: a copy of the whole capacity per frame
 * the GPU has not finished, kept in its pool - non-local video memory, charged to the process (measured: ~300 MB
 * with the 8 MB start sizes of M2). The buffers start small and grow to the next power of two a frame needs; one
 * whose fills stayed under a quarter of its capacity for DYN_WINDOW fills (loading: 30 MB of texture patches in the
 * data arena, then a few hundred KB) is recreated at twice the window's largest fill, and the driver's copies of
 * the old one go with it. */
#define DYN_WINDOW 300
static int dyn_fill(dynbuf* d, const void* src, size_t bytes, UINT bind, int raw) {
    if (bytes > d->hi) d->hi = bytes;
    if (++d->fills >= DYN_WINDOW) {
        size_t want = DYN_MIN;
        while (want < 2 * d->hi) want *= 2;
        if (d->buf && d->cap > want && d->cap >= 4 * d->hi) {
            GPU_RELEASE(d->srv);
            GPU_RELEASE(d->buf);
            d->cap = 0;
            if (!dyn_ensure(d, want, bind, raw)) return 0;
            XC.dyn_shrinks++;
        }
        d->fills = 0;
        d->hi = 0;
    }
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
static void td_bind_ps(void);            /* (r2-m7b) */

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
    ID3D11Texture2D* nt = t->depth ? tex2d(w, h, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)
                                   : tex2d(w, h, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    ID3D11Texture2D* nz = t->depth ? tex2d(w, h, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE) : NULL;
    if (!nt || (t->depth && !nz)) { GPU_RELEASE(nt); GPU_RELEASE(nz); gpu_log("cannot create a %dx%d target", w, h); return 0; }
    if (getenv("PS2_GS_MEM_MAP")) { char wh[64]; snprintf(wh, sizeof wh, "target %dx%d %s created", w, h, t->depth ? "Z" : "C"); mem_note(wh); }
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
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)nt, &dd, &t->dsv);
        dd.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDepthStencilView(gpu_rt.dev, (ID3D11Resource*)nt, &dd, &t->dsv_ro);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_FLOAT;
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
static void bind_ref(int ct, int zt, int a16, int ez) {
    xtarget *c = tgt((uint32_t)ct), *z = tgt((uint32_t)zt);
    int mode = a16 ? 5 : ez && z ? 6 : 4;   /* (perf-p2) 6: + zt's read-only DSV for early depth rejection */
    if (X.mode == mode && X.b_ct == ct && X.b_zt == zt) return;
    if (X.b_t2) set_t2(NULL);
    /* (ALIAS16: the colour target is not bound, its views stay; the staging texture is u0) */
    if (!a16 && X.b_t1 && c && (X.b_t1 == c->srv || X.b_t1 == c->srv_u)) set_t1(NULL);
    if (mode == 6 && X.b_t1 && X.b_t1 == z->srv) set_t1(NULL);
    ID3D11UnorderedAccessView* uav[3] = { a16 ? X.a16_uav : c ? c->uav32 : NULL, z ? z->zp_uav : NULL, X.dbg_uav };
    ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(gpu_rt.ctx, 0, NULL, mode == 6 ? z->dsv_ro : NULL, 0,
                                                                  X.dbg_uav ? 3 : 2, uav, NULL);
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
static uint32_t bc_maxid;                /* (M9) build_constants: iota elements the frame's draws read */

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
    bc_maxid = 1;
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
        } else if (h->op == GPU_FOP_DRAW) {
            const gpu_fop_draw* d = (const gpu_fop_draw*)h;
            n += draw_slots(d, ref);
            /* (M9) the DRAWID element of the last instance: slot + instance (iota_ensure) */
            uint32_t top = n + (d->cls == GPU_PC_TRI ? 0u : d->count);
            if (top > bc_maxid) bc_maxid = top;
        }
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
                k->flags = (d->ct ? 1u : 0u) | (d->zt ? 2u : 0u) | (gpu_earlyz() ? 0u : 4u);
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
    UINT stride1 = 4;                    /* (M9) slot 1: the draw slot per instance (StartInstanceLocation) */
    ID3D11DeviceContext_IASetVertexBuffers(c, 1, 1, &X.iota, &stride1, &off);
    ID3D11DeviceContext_IASetIndexBuffer(c, X.ib.buf, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetInputLayout(c, X.layout);
    ID3D11ShaderResourceView* t0 = X.vram_srv;
    ID3D11ShaderResourceView* t34567[5] = { X.rec.srv, X.st.srv, X.clut_srv, X.swz_srv, X.dat.srv };
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &t0);
    ID3D11DeviceContext_PSSetShaderResources(c, 3, 5, t34567);
    ID3D11DeviceContext_VSSetShaderResources(c, 3, 1, &X.rec.srv);
    ID3D11DeviceContext_PSSetShaderResources(c, 9, 1, &X.dc.srv);   /* (M9) per-draw constants */
    ID3D11DeviceContext_VSSetShaderResources(c, 9, 1, &X.dc.srv);
    td_bind_ps();                        /* (r2-m7b) decoded textures (t13) */
    ID3D11DeviceContext_RSSetState(c, X.rs);
    X.state_calls += 11;
    X.b_topo = 0;
    X.b_vs = X.b_ps = X.b_bs = X.b_ds = NULL;
    X.b_vp_w = X.b_vp_h = -1;
    memset(&X.b_sc, 0xFF, sizeof X.b_sc);
}

/* (M9) the iota vertex buffer for at least n draw slots (element i holds i) */
static int iota_ensure(uint32_t n) {
    if (X.iota && X.iota_n >= n) return 1;
    uint32_t cap = 65536;
    while (cap < n) cap *= 2;
    uint32_t* v = malloc((size_t)cap * 4);
    if (!v) return 0;
    for (uint32_t i = 0; i < cap; i++) v[i] = i;
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = cap * 4;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { v, 0, 0 };
    GPU_RELEASE(X.iota);
    HRESULT hr = ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, &sd, &X.iota);
    free(v);
    if (FAILED(hr)) { X.iota_n = 0; gpu_log("cannot create the draw-slot buffer"); return 0; }
    X.iota_n = cap;
    return 1;
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
    /* (M9) hardware clears where they write exactly what cs_clear writes: a colour clear of every byte (ClearView on
     * the UNORM RTV: byte b as b / 255 converts back to b); a Z32 / Z16 depth clear of the target's whole drawn size
     * (the rows past nat_h are the allocation's padding, never drawn, shown or downloaded: a clear of the whole
     * allocation - zpatch through ClearUnorderedAccessViewUint, the DSV through ClearDepthStencilView - is the same;
     * Z24 keeps zpatch's top byte: cs_clear) */
    int fullrect = x1 * t->kx >= t->w && y1 * t->ky >= t->h, hw = 0;
    static int nohw = -1;                /* (PS2_GPU_NO_HWCLEAR=1: every clear through cs_clear, comparisons) */
    if (nohw < 0) { const char* e = getenv("PS2_GPU_NO_HWCLEAR"); nohw = e && atoi(e); }
    if (!nohw && t->depth && x1 >= t->nat_w && y1 >= t->nat_h) {
        fullrect = 1;
        if (o->zclear && X.mixed && t->zts) {        /* (every tile cleared in both copies) */
            int ra[4] = { 0, 0, t->zts_w, t->zts_h };
            zt_set(t, ra, -1, ZT_SYNC);
            full = 1;
        }
    }
    if (nohw) fullrect = 0;
    if (!t->depth && (o->mask & 15) == 15 && X.clearview && t->rtv && !nohw) {
        D3D11_RECT rc = { 0, 0, x1 * t->kx < t->w ? x1 * t->kx : t->w, y1 * t->ky < t->h ? y1 * t->ky : t->h };
        float col[4] = { (float)(o->value & 255) / 255.0f, (float)((o->value >> 8) & 255) / 255.0f,
                         (float)((o->value >> 16) & 255) / 255.0f, (float)(o->value >> 24) / 255.0f };
        ID3D11DeviceContext1_ClearView(gpu_rt.ctx1, (ID3D11View*)t->rtv, col, &rc, 1);
        hw = 1;
    } else if (t->depth && fullrect && t->kind != XK_Z24 && t->zp_uav) {
        UINT v[4] = { o->value, o->value, o->value, o->value };
        ID3D11DeviceContext_ClearUnorderedAccessViewUint(gpu_rt.ctx, t->zp_uav, v);
        hw = 1;
    }
    if (!hw) {
        mem_consts k;
        target_consts(t, &k);
        k.value = o->value;
        k.wmask = o->zclear ? 15 : o->mask & 15;
        k.cx1 = (uint32_t)x1;
        k.cy1 = (uint32_t)y1;
        mem_dispatch(GPU_SH_cs_clear, &k, (uint32_t)(x1 * t->kx + 7) / 8, (uint32_t)(y1 * t->ky + 7) / 8, NULL, X.dat.srv, NULL,
                     t->depth ? NULL : t->uav, t->depth ? t->zp_uav : NULL);   /* (M9: a thread per sample) */
    }
    if (o->zclear && X.mixed && t->zts) {
        uint32_t zv = (uint32_t)o->value & (zbits_of(t) == 1 ? 0xFFFFu : zbits_of(t) == 2 ? 0xFFFFFFu : 0xFFFFFFFFu);
        if (full) {                      /* (P1: enc(z), saturated at 1.0 - zpatch holds the value) */
            ID3D11DeviceContext_ClearDepthStencilView(gpu_rt.ctx, t->dsv, D3D11_CLEAR_DEPTH, gpu_z_encode(zv), 0);
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

int gpu_earlyz(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS2_GPU_EARLYZ"); on = !(e && *e == '0'); }
    return on;
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

/* ---- (M7) PS2_GPU_PROF=1: GPU time per kind of work --------------------------------------------------
 * A timestamp where the kind of work changes (ops, draw classes / tiers, depth resolves and syncs); the frame
 * waits for its timestamps (profiling only: it serialises CPU and GPU). Averages per frame in the log every
 * 60 frames. Attribution is approximate (the GPU overlaps neighbouring work). */
enum { PR_OTHER, PR_TARGET, PR_MEM, PR_CLEAR, PR_SHADOW, PR_FF_FAST, PR_FF_TRI, PR_FF_SPRITE, PR_FF_LINE,
       PR_REF_TRI, PR_REF_OTHER, PR_A16, PR_ZRESOLVE, PR_ZSYNC, PR_PRESENT, PR_A16_DRAW, PR_FF_FAST_Z, PR_R2, PR_PART, PR_N };
static const char* const pr_names[PR_N] = { "other", "target", "mem", "clear", "shadow", "ff-tri-fast", "ff-tri",
                                            "ff-sprite", "ff-line", "ref-tri", "ref-other", "alias16", "zresolve",
                                            "zsync", "present", "alias16-draw", "ff-tri-fast-z", "r2", "part" };
#define PR_MAXQ 8192
static struct {
    int on, n, cur;
    ID3D11Query* dis;
    ID3D11Query* q[PR_MAXQ];
    ID3D11Query *sq[PR_MAXQ], *oq[PR_MAXQ];   /* (M9, PS2_GPU_PROF=2) pipeline statistics + occlusion per segment */
    uint8_t cat[PR_MAXQ];
    double ms[PR_N], total;
    uint64_t marks[PR_N];
    uint64_t ps_inv[PR_N], occl[PR_N], prims[PR_N];
    uint32_t frames;
    /* (r2-m7) PS2_GPU_PROF_OPS=<frame>: that frame (counted from 1 by prof_end) gets a timestamp before every op, and
     * the GPU time per op signature is logged (which draws cost what) */
    uint32_t ops_frame, ops_on, curpos, opos[PR_MAXQ];
    /* (r2-m7b) PS2_GPU_AB=<frames>: A/B in one run - the experimental features are off in the A phases and on in the B
     * phases (alternating every <frames> frames), and the GPU time is accumulated per phase (prof-ab lines) */
    int ab, ab_b, ab_skip, ab_ee;
    double ab_ms[2][PR_N], ab_tot[2];
    uint32_t ab_n[2];
} PR = { -1 };
/* (r2-m7b) an experimental feature is off in this frame (an A phase of PS2_GPU_AB) */
static int ab_off(void) { return PR.ab > 0 && !PR.ab_b && !PR.ab_ee; }
/* (r2-m5b) the current phase for EE-side features (r2_capture.c: the VU clipper's entries with PS2_GPU_AB_EE=1, when only
 * they follow the phases); -1: no A/B */
volatile int gpu_ab_phase = -1;

static void prof_mark(int cat) {
    if (PR.on <= 0 || PR.n >= PR_MAXQ - 1 || (PR.n && cat == PR.cur && !PR.ops_on)) return;
    PR.opos[PR.n] = PR.curpos;
    if (!PR.q[PR.n]) {
        D3D11_QUERY_DESC qt = { D3D11_QUERY_TIMESTAMP, 0 };
        if (FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &PR.q[PR.n]))) { PR.on = 0; return; }
    }
    ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)PR.q[PR.n]);
    if (PR.on >= 2) {                    /* statistics of the segment that ends here, and of the one that begins */
        if (PR.n) {
            ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)PR.sq[PR.n - 1]);
            ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)PR.oq[PR.n - 1]);
        }
        if (!PR.sq[PR.n]) {
            D3D11_QUERY_DESC qs = { D3D11_QUERY_PIPELINE_STATISTICS, 0 }, qo = { D3D11_QUERY_OCCLUSION, 0 };
            if (FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qs, &PR.sq[PR.n])) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qo, &PR.oq[PR.n]))) { PR.on = 0; return; }
        }
        ID3D11DeviceContext_Begin(gpu_rt.ctx, (ID3D11Asynchronous*)PR.sq[PR.n]);
        ID3D11DeviceContext_Begin(gpu_rt.ctx, (ID3D11Asynchronous*)PR.oq[PR.n]);
    }
    PR.cat[PR.n++] = (uint8_t)cat;
    PR.cur = cat;
    if (cat < PR_N) PR.marks[cat]++;
}

static void prof_begin(void) {
    if (PR.on < 0) { const char* e = getenv("PS2_GPU_PROF"); PR.on = e && atoi(e) > 0 ? atoi(e) : 0; }
    if (!PR.on) return;
    if (!PR.dis) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        if (FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qd, &PR.dis))) { PR.on = 0; return; }
    }
    ID3D11DeviceContext_Begin(gpu_rt.ctx, (ID3D11Asynchronous*)PR.dis);
    PR.n = 0;
    if (!PR.ops_frame) { const char* e = getenv("PS2_GPU_PROF_OPS"); PR.ops_frame = e ? (uint32_t)atoi(e) : 0xFFFFFFFFu; }
    PR.ops_on = PR.frames + 1 == PR.ops_frame;
    PR.curpos = 0xFFFFFFFFu;
    prof_mark(PR_OTHER);
}

/* (r2-m7) PS2_GPU_PROF_OPS: the op at byte pos of the frame's commands, as a short signature */
static void prof_op_sig(const gpu_front_frame_data* fd, uint32_t pos, int cat, char* s, size_t n) {
    if (!fd || pos == 0xFFFFFFFFu || pos + sizeof(gpu_fop) > fd->cmd_bytes) { snprintf(s, n, "(%s)", cat < PR_N ? pr_names[cat] : "end"); return; }
    const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
    if (h->op == GPU_FOP_DRAW) {
        const gpu_fop_draw* d = (const gpu_fop_draw*)h;
        uint32_t st = 0xFFFFFFFFu;       /* the first record's state: its frame mask / alpha / test (what the draw is) */
        const gpu_staterec* sr = NULL;
        if (d->cls == GPU_PC_SPRITE && d->rec_first + 24u <= fd->rec_bytes) st = *(const uint32_t*)(fd->rec + d->rec_first + 20u);
        if (d->cls == GPU_PC_TRI && (d->ps_key & GPU_PSK_FF_FAST) && d->rec_first + 116u <= fd->rec_bytes)
            st = *(const uint32_t*)(fd->rec + d->rec_first + 112u);
        if (st != 0xFFFFFFFFu && st < fd->state_count) sr = (const gpu_staterec*)(fd->states + (size_t)st * GPU_STATE_BYTES);
        snprintf(s, n, "draw %s cls %u ct %u zt %u tex %u blend %u depth %u wm %X feat %X%s", pr_names[cat], d->cls, d->ct, d->zt,
                 d->tex, d->blend, d->depth, d->wmask, d->feat, "");
        if (sr) { size_t l = strlen(s); snprintf(s + l, n - l, " fmask %08X alpha %X tex0 %llX", sr->fmask, sr->alpha, (unsigned long long)sr->tex0); }
    } else {
        snprintf(s, n, "op %u target %u (%s)", h->op, ((const uint16_t*)h)[2], cat < PR_N ? pr_names[cat] : "?");
    }
}

static void prof_ops_report(const UINT64* ts, double freq) {
    typedef struct { char sig[200]; double ms; uint32_t n; } agg;
    static agg A[512];
    int na = 0;
    double tot = 0;
    for (int i = 0; i + 1 < PR.n; i++) {
        double ms = (double)(ts[i + 1] - ts[i]) * 1000.0 / freq;
        char s[200];
        prof_op_sig(X.fd, PR.opos[i], PR.cat[i], s, sizeof s);
        if (getenv("PS2_GPU_PROF_SEQ")) gpu_log("prof-seq %4d %7.3f ms @%u %s", i, ms, PR.opos[i], s);   /* (r2-m7b: in order) */
        int k;
        for (k = 0; k < na && strcmp(A[k].sig, s); k++) {}
        if (k == na) { if (na == 512) continue; snprintf(A[na].sig, sizeof A[na].sig, "%s", s); A[na].ms = 0; A[na].n = 0; na++; }
        A[k].ms += ms;
        A[k].n++;
        tot += ms;
    }
    for (int i = 0; i < na; i++)          /* (largest first) */
        for (int j = i + 1; j < na; j++)
            if (A[j].ms > A[i].ms) { agg t = A[i]; A[i] = A[j]; A[j] = t; }
    gpu_log("prof-ops frame %u: %.3f ms in %d intervals, %d signatures", PR.frames + 1, tot, PR.n - 1, na);
    for (int i = 0; i < na && i < 48; i++) gpu_log("prof-ops %7.3f ms %4u x %s", A[i].ms, A[i].n, A[i].sig);
}

static void prof_end(void) {
    if (PR.on <= 0) return;
    PR.cur = -1;
    prof_mark(PR_N);
    if (PR.on >= 2 && PR.n) {
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)PR.sq[PR.n - 1]);
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)PR.oq[PR.n - 1]);
    }
    ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)PR.dis);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)PR.dis, &dj, sizeof dj, 0) == S_FALSE) SwitchToThread();
    if (dj.Disjoint || !dj.Frequency) return;
    UINT64 prev = 0;
    double f[PR_N] = { 0 }, ft = 0;
    uint64_t fi[PR_N] = { 0 }, fo[PR_N] = { 0 }, fp[PR_N] = { 0 };
    static UINT64 tsv[PR_MAXQ];
    for (int i = 0; i < PR.n; i++) {
        UINT64 t = 0;
        while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)PR.q[i], &t, sizeof t, 0) == S_FALSE) SwitchToThread();
        tsv[i] = t;
        if (i) {
            double ms = (double)(t - prev) * 1000.0 / (double)dj.Frequency;
            f[PR.cat[i - 1]] += ms;
            ft += ms;
        }
        prev = t;
        if (PR.on >= 2 && i + 1 < PR.n) {
            D3D11_QUERY_DATA_PIPELINE_STATISTICS ps;
            UINT64 oc = 0;
            while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)PR.sq[i], &ps, sizeof ps, 0) == S_FALSE) SwitchToThread();
            while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)PR.oq[i], &oc, sizeof oc, 0) == S_FALSE) SwitchToThread();
            fi[PR.cat[i]] += ps.PSInvocations;
            fp[PR.cat[i]] += ps.CPrimitives;
            fo[PR.cat[i]] += oc;
        }
    }
    {                                    /* (M9) the frame's phases for the stats line (gpu_hw.c, PS2_GS_HW_STATS) */
        gpu_frame_stats* s = gpu_stats_cur();
        s->phases = 1;
        s->phase_ms[0] = (float)(f[PR_FF_FAST] + f[PR_FF_TRI] + f[PR_FF_FAST_Z] + f[PR_R2] + f[PR_PART]);
        s->phase_ms[1] = (float)(f[PR_FF_SPRITE] + f[PR_FF_LINE]);
        s->phase_ms[2] = (float)(f[PR_REF_TRI] + f[PR_REF_OTHER]);
        s->phase_ms[3] = (float)(f[PR_A16] + f[PR_A16_DRAW]);
        s->phase_ms[4] = (float)(f[PR_ZRESOLVE] + f[PR_ZSYNC]);
        s->phase_ms[5] = (float)(f[PR_OTHER] + f[PR_TARGET] + f[PR_MEM] + f[PR_CLEAR] + f[PR_SHADOW] + f[PR_PRESENT]);
    }
    if (PR.ops_on) prof_ops_report(tsv, (double)dj.Frequency);
    if (ft > 50.0) return;               /* (loading frames: left out) */
    for (int c = 0; c < PR_N; c++) { PR.ms[c] += f[c]; PR.ps_inv[c] += fi[c]; PR.occl[c] += fo[c]; PR.prims[c] += fp[c]; }
    PR.total += ft;
    if (PR.ab > 0 && PR.ab_skip > 0) PR.ab_skip--;   /* (r2-m5b) the frames after a switch: in flight / the EE's lag */
    else if (PR.ab > 0) {                /* (r2-m7b) per A / B phase */
        for (int c = 0; c < PR_N; c++) PR.ab_ms[PR.ab_b][c] += f[c];
        PR.ab_tot[PR.ab_b] += ft;
        PR.ab_n[PR.ab_b]++;
    }
    static int every = 0;                /* (r2-m1) PS2_GPU_PROF_EVERY=n: the line every n frames (short replays) */
    if (!every) { const char* e = getenv("PS2_GPU_PROF_EVERY"); every = e && atoi(e) > 0 ? atoi(e) : 60; }
    if (++PR.frames % (uint32_t)every) return;
    char line[2048];
    int o = snprintf(line, sizeof line, "prof: %u frames, GPU ms per frame %.3f:", PR.frames, PR.total / PR.frames);
    for (int c = 0; c < PR_N && o < (int)sizeof line - 48; c++)
        if (PR.ms[c] > 0) o += snprintf(line + o, sizeof line - (size_t)o, " %s %.3f (%.0f)", pr_names[c], PR.ms[c] / PR.frames,
                                         (double)PR.marks[c] / PR.frames);
    gpu_log("%s", line);
    for (int b = 0; PR.ab > 0 && b < 2; b++) {
        if (!PR.ab_n[b]) continue;
        o = snprintf(line, sizeof line, "prof-ab %c: %u frames, GPU ms per frame %.3f:", 'A' + b, PR.ab_n[b], PR.ab_tot[b] / PR.ab_n[b]);
        for (int c = 0; c < PR_N && o < (int)sizeof line - 48; c++)
            if (PR.ab_ms[b][c] > 0) o += snprintf(line + o, sizeof line - (size_t)o, " %s %.3f", pr_names[c], PR.ab_ms[b][c] / PR.ab_n[b]);
        gpu_log("%s", line);
    }
    if (PR.on >= 2) {                    /* per frame: PS invocations (M), samples passing depth (M), primitives (k) */
        o = snprintf(line, sizeof line, "prof-stats per frame (PS invocations M / samples passed M / primitives k):");
        for (int c = 0; c < PR_N && o < (int)sizeof line - 64; c++)
            if (PR.ps_inv[c]) o += snprintf(line + o, sizeof line - (size_t)o, " %s %.2f/%.2f/%.1f", pr_names[c],
                                            (double)PR.ps_inv[c] / PR.frames / 1e6, (double)PR.occl[c] / PR.frames / 1e6,
                                            (double)PR.prims[c] / PR.frames / 1e3);
        gpu_log("%s", line);
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
    int pc = PR.cur;
    prof_mark(PR_ZRESOLVE);
    ID3D11DeviceContext_Draw(gpu_rt.ctx, 4, 0);
    prof_mark(pc);
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
    int pc = PR.cur;
    prof_mark(PR_ZSYNC);
    ID3D11DeviceContext_Dispatch(c, (k.x1 - k.x0 + 7) / 8, (k.y1 - k.y0 + 7) / 8, 1);
    prof_mark(pc);
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

/* (perf-p2) a draw of an ALIAS16 copy run (gpu_front.h gpu_fop_alias16 mode 1): one cs_a16_copy dispatch over the
 * samples of its sprites, 8x8-sample tiles, written straight into the target (shaders/alias16.hlsl) */
typedef struct {
    uint32_t rec_first, count, kx, ky, tbase, tfbw, psm16, same, vkx, vky, vbase, vfbw, gx, ngroups;
    float one;
    uint32_t pad;
    uint32_t prefix[44];
    uint32_t vint[2];                    /* bit s: sprite s's V rows are integers (vab), no doubles in the shader */
    uint32_t pad2[2];
    int32_t vab[80];                     /* per sprite: trunc((vc0 + vcy * py) * 4096) = a + b * py (exact, checked) */
} a16c_consts;
_Static_assert(sizeof(a16c_consts) == 576, "a16c_cb");

static void exec_a16_copy(const gpu_fop_draw* d) {
    xtarget* t = tgt(d->ct);
    xtarget* v = tgt(d->tex);
    const gpu_front_frame_data* fd = X.fd;
    if (!t || t->depth || !v || v->depth || !fd || d->cls != GPU_PC_SPRITE || !d->count || d->count > GPU_A16_COPY_MAX) {
        dbg_nocount++;
        return;
    }
    if ((size_t)d->rec_first + (size_t)d->count * sizeof(gpu_rec_sprite) > fd->rec_bytes) return;
    if (!X.a16cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(a16c_consts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &X.a16cb))) { gpu_log("cannot create the ALIAS16 copy constants"); return; }
    }
    a16c_consts k;
    memset(&k, 0, sizeof k);
    k.rec_first = d->rec_first; k.count = d->count; k.kx = (uint32_t)t->kx; k.ky = (uint32_t)t->ky;
    k.tbase = (uint32_t)t->base; k.tfbw = (uint32_t)t->fbw; k.psm16 = (uint32_t)X.a16c_psm; k.same = d->tex == d->ct;
    k.vkx = (uint32_t)v->kx; k.vky = (uint32_t)v->ky; k.vbase = (uint32_t)v->base; k.vfbw = (uint32_t)v->fbw;
    k.one = 1.0f;
    uint32_t n = 0;
    for (uint32_t s = 0; s < d->count; s++) {
        const gpu_rec_sprite* r = (const gpu_rec_sprite*)(fd->rec + d->rec_first + (size_t)s * sizeof(gpu_rec_sprite));
        k.prefix[s] = n;
        /* V of row py is trunc((vc0 + vcy * py) * 4096) in doubles (gs.hlsl); when vc0 * 4096 and vcy * 4096 are integers
         * and every row's value stays inside int32 (no clamp), each double operation is exact and the value is
         * a + b * py: the shader then needs no double arithmetic (1/64 rate on GeForce) */
        double a = r->vc0 * 4096.0, b = r->vcy * 4096.0;
        double e0 = a + b * (double)r->y0, e1 = a + b * (double)(r->y1 - 1);
        if (a == (double)(int64_t)a && b == (double)(int64_t)b && fabs(a) < 1e9 && fabs(b) < 1e6 && fabs(e0) < 2147483000.0 &&
            fabs(e1) < 2147483000.0 && r->y1 > r->y0 && fabs(r->vc0) < 1e9 && fabs(r->vcy) < 1e6) {
            k.vint[s >> 5] |= 1u << (s & 31);
            k.vab[2 * s] = (int32_t)a;
            k.vab[2 * s + 1] = (int32_t)b;
        }
        /* (8 sample columns x 8 pixel rows per group: each thread does the ky samples of its column) */
        uint32_t w = r->x1 > r->x0 ? (uint32_t)(r->x1 - r->x0) * k.kx : 0, h = r->y1 > r->y0 ? (uint32_t)(r->y1 - r->y0) : 0;
        n += ((w + 7) / 8) * ((h + 7) / 8);
    }
    k.prefix[d->count] = n;
    if (!n) return;
    k.gx = n < 1024 ? n : 1024;
    k.ngroups = n;
    if (X.mode) unbind_all();
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* none2[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(c, 1, 2, none2);   /* (the targets may be views t1 / t2) */
    X.b_t1 = X.b_t2 = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)X.a16cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &k, sizeof k);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)X.a16cb, 0);
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_a16_copy), NULL, 0);
    ID3D11ShaderResourceView* s34[2] = { X.rec.srv, X.st.srv };
    ID3D11DeviceContext_CSSetShaderResources(c, 3, 2, s34);
    ID3D11DeviceContext_CSSetShaderResources(c, 6, 1, &X.swz_srv);
    ID3D11UnorderedAccessView* u[3] = { NULL, t->uav32, k.same ? NULL : v->uav32 };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, u, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 1, 1, &X.a16cb);
    int pc = PR.cur;
    prof_mark(PR_A16_DRAW);
    ID3D11DeviceContext_Dispatch(c, k.gx, (n + k.gx - 1) / k.gx, 1);
    prof_mark(pc);
    ID3D11UnorderedAccessView* nu[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, nu, NULL);
    ID3D11ShaderResourceView* ns[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetShaderResources(c, 3, 2, ns);
    gpu_rt.n_dispatches++;
    X.state_calls += 12;
    XC.a16_copy_dispatches++;
}

/* (perf-p2) the specialised permutation (shaders/gs.hlsl SPEC_*) a draw can use from the features its primitives use
 * (gpu_fop_draw.feat): 2 untextured (SPEC_NOTEX), 1 no target view and no general LOD (SPEC_NOVIEW, SPEC_NOLOD), 0 the
 * ubershader. PS2_GPU_SPEC=0: the ubershaders always */
static int spec_level(const gpu_fop_draw* d) {
    static int on = -1;                  /* (PS2_GPU_SPEC=<mask>: bit n allows level n; 0 none) */
    if (on < 0) { const char* e = getenv("PS2_GPU_SPEC"); on = e ? (int)strtol(e, NULL, 0) : 14; }
    if (!on || X.probe) return 0;        /* (PS2_GPU_PROBE: the ubershaders record the probe pixel) */
    /* thin analytic lines evaluate their planes in non-precise floats (gs.hlsl PRIM 3): another permutation may fuse
     * them into FMAs differently (measured: the untextured one moved Z / colour words of --thin replays) - they keep the
     * ubershaders, so outlines = thin draws exactly what it did */
    if (d->cls == GPU_PC_THIN) return 0;
    int l;
    if (!(d->feat & GPU_DF_TEX)) l = 2;
    else if (d->feat & GPU_DF_LOD) l = 0;
    else if (!(d->feat & GPU_DF_VIEW)) l = 1;
    /* 3: every texture a target view read 1:1 (SPEC_VIEWONLY, SPEC_NOHIRES): sprites only */
    else l = d->cls == GPU_PC_SPRITE && !(d->feat & (GPU_DF_VRAM | GPU_DF_HIRES)) ? 3 : 0;
    /* (WARP executes the level-1 permutations wrongly in one case: a 16-bit texel's alpha bit of a sprite differs in
     * tests/unit/gs-draw t_random_states (iteration 49), with the same inputs (probe records) and the same shader logic,
     * while the RTX 4060 Ti matches the software renderer everywhere - a WARP anomaly like the STQ-record one of appendix
     * E; on WARP those draws keep the ubershaders) */
    if (l == 1 && gpu_rt.caps.warp) return 0;
    return (on >> l) & 1 ? l : 0;
}

/* ---- (r2-m7) native passes of modern frames: shared constants (shaders/post.hlsl npost_cb) ---- */
typedef struct {
    uint32_t rect[4];
    uint32_t mode, alpha, fmask, vcol, clut, pad[3];
} npost_consts;
_Static_assert(sizeof(npost_consts) == 48, "npost_cb");

static int npcb_ensure(void) {
    if (X.npcb) return 1;
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(npost_consts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &X.npcb));
}

static int npcb_write(const npost_consts* k) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)X.npcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, k, sizeof *k);
    ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)X.npcb, 0);
    return 1;
}

/* (r2-m7) shadow volumes (gpu_front.h GPU_DF_SVOL): their draws add each fragment's G step atomically into the counter
 * texture (shaders/gs.hlsl SVOL) while the colour target is only read; before anything else runs on the render thread the
 * counters are added into G (cs_svol_resolve, which zeroes them again). */
static int svol_ensure(int w, int h) {
    if (X.svcnt && X.svcnt_w >= w && X.svcnt_h >= h) return 1;
    if (X.sv_on) return 0;
    GPU_RELEASE(X.svcnt_uav);
    GPU_RELEASE(X.svcnt);
    if (w < X.svcnt_w) w = X.svcnt_w;
    if (h < X.svcnt_h) h = X.svcnt_h;
    X.svcnt_w = X.svcnt_h = 0;
    X.svcnt = tex2d(w, h, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS);
    if (!X.svcnt) return 0;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)X.svcnt, &ud, &X.svcnt_uav))) {
        GPU_RELEASE(X.svcnt);
        return 0;
    }
    static const UINT zero[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(gpu_rt.ctx, X.svcnt_uav, zero);
    X.svcnt_w = w;
    X.svcnt_h = h;
    return 1;
}

static void svol_resolve(void) {
    if (!X.sv_on) return;
    X.sv_on = 0;
    xtarget* t = tgt((uint32_t)X.sv_ct);
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* n1 = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 12, 1, &n1);
    unbind_all();
    if (!t || !npcb_ensure()) return;
    npost_consts k;
    memset(&k, 0, sizeof k);
    int x0 = (X.sv_bb[0] - 1) * t->kx, y0 = (X.sv_bb[1] - 1) * t->ky, x1 = (X.sv_bb[2] + 2) * t->kx, y1 = (X.sv_bb[3] + 2) * t->ky;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->w) x1 = t->w;
    if (y1 > t->h) y1 = t->h;
    if (x1 > X.svcnt_w) x1 = X.svcnt_w;
    if (y1 > X.svcnt_h) y1 = X.svcnt_h;
    if (x0 >= x1 || y0 >= y1) return;
    k.rect[0] = (uint32_t)x0; k.rect[1] = (uint32_t)y0; k.rect[2] = (uint32_t)x1; k.rect[3] = (uint32_t)y1;
    if (!npcb_write(&k)) return;
    int pc = PR.cur;
    prof_mark(PR_REF_TRI);
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_svol_resolve), NULL, 0);
    ID3D11UnorderedAccessView* u[2] = { t->uav32, X.svcnt_uav };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, u, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 2, 1, &X.npcb);
    ID3D11DeviceContext_Dispatch(c, (UINT)(x1 - x0 + 7) / 8, (UINT)(y1 - y0 + 7) / 8, 1);
    ID3D11UnorderedAccessView* nu[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, nu, NULL);
    prof_mark(pc);
    gpu_rt.n_dispatches++;
    X.state_calls += 8;
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
    /* (perf-p2, P2 c) early depth rejection on the ROV tier: every primitive of the draw is depth-tested GEQUAL / GREATER
     * and rasterizes an upper bound of its exact Z (gpu_fop_draw.feat has no GPU_DF_ZNGE; triangles through vs_ff_tri):
     * zt's DSV, made current over the draw's tiles (the exact value or, where saturated, 1.0 - never above the stored Z),
     * is bound read-only with the test GEQUAL, so a fragment whose bound is below it - whose exact Z then fails the shader's
     * own test against zpatch - is rejected before the shader ([earlydepthstencil]). Z the draw writes only grows (GEQUAL /
     * GREATER), so the DSV stays a lower bound of zpatch inside the draw. PS2_GPU_ROV_EARLYZ=0: off. */
    static int rov_ez = -1;
    if (rov_ez < 0) { const char* e = getenv("PS2_GPU_ROV_EARLYZ"); rov_ez = !(e && *e == '0'); }
    int ez = rov_ez && X.mixed && zt && d->tier == GPU_TIER_ROV && !(d->feat & GPU_DF_ZNGE) &&
             (d->cls != GPU_PC_TRI || (d->ps_key & GPU_PSK_FF_FAST));
    if (zt) ensure_zp(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3]);
    if (ez) ensure_dsv(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3]);
    /* (r2-m7) shadow volumes of modern frames (GPU_DF_SVOL): G steps into the counter texture, colour read-only (t12) */
    xtarget* sct = tgt(d->ct);
    int svol = X.mixed && d->tier == GPU_TIER_ROV && d->cls == GPU_PC_TRI && (d->ps_key & GPU_PSK_FF_FAST) &&
               (d->feat & GPU_DF_SVOL) && !gpu_no_rov() && sct && !sct->depth && sct->srv_u && svol_ensure(sct->w, sct->h);
    if (X.sv_on && (!svol || X.sv_ct != d->ct)) svol_resolve();
    if (svol) {
        xtarget* z = tgt(d->zt);
        if (X.mode != 9 || X.b_ct != d->ct || X.b_zt != d->zt || X.sv_ez != ez) {
            if (X.b_t2) set_t2(NULL);
            if (X.b_t1 && (X.b_t1 == sct->srv || X.b_t1 == sct->srv_u)) set_t1(NULL);
            ID3D11UnorderedAccessView* uav[2] = { X.svcnt_uav, z ? z->zp_uav : NULL };
            ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(c, 0, NULL, ez && z ? z->dsv_ro : NULL, 0, 2, uav, NULL);
            ID3D11DeviceContext_PSSetShaderResources(c, 12, 1, &sct->srv_u);
            X.mode = 9;
            X.b_ct = d->ct;
            X.b_zt = d->zt;
            X.sv_ez = ez;
            X.state_calls += 2;
            set_viewport(-REF_VP, -REF_VP);
        }
        if (!X.sv_on) { X.sv_on = 1; X.sv_ct = d->ct; memcpy(X.sv_bb, (int[4]){ d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3] }, sizeof X.sv_bb); }
        else {
            if (d->bbox[0] < X.sv_bb[0]) X.sv_bb[0] = d->bbox[0];
            if (d->bbox[1] < X.sv_bb[1]) X.sv_bb[1] = d->bbox[1];
            if (d->bbox[2] > X.sv_bb[2]) X.sv_bb[2] = d->bbox[2];
            if (d->bbox[3] > X.sv_bb[3]) X.sv_bb[3] = d->bbox[3];
        }
        XC.svol_draws++;
    } else {
        bind_ref(d->ct, d->zt, d->tier == GPU_TIER_ALIAS16, ez);
    }
    if (X.mixed && dbg_skip == 1 && d->tier == GPU_TIER_ROV) return;
    int tk = tex_kind(d->ps_key);
    set_t1(d->tex && (tk == TK_VIEW_C32 || tk == TK_VIEW_ALPHA || tk == TK_ALIAS16) ? view_srv(d, 1) : NULL);
    int kx = sizing->kx, ky = sizing->ky;
    draw_scissor(d, kx, ky);
    set_depth(ez ? depth_state(4) : X.ds_off);   /* (perf-p2: GEQUAL, no write - the bound against the DSV) */
    if (ez) XC.rov_ez_draws++;
    /* (M9) the draw's constants by its slot: StartInstanceLocation -> DRAWID (gs.hlsl load_draw), no constant buffer */
    /* (M11) without rasterizer-ordered views: the same shaders with plain UAVs (the frontend made the draw's primitives
     * disjoint, gf_cfg.no_rov) */
    int nr = gpu_no_rov();
    int sp = spec_level(d);              /* (perf-p2) [level][no ROV] */
    if (d->cls == GPU_PC_TRI) {
        int fast = (d->ps_key & GPU_PSK_FF_FAST) != 0;   /* (mixed default mode: 128-byte records, hardware S/T/Q) */
        static const int tf[3][2] = { { GPU_SH_ps_gs_tri_fast, GPU_SH_ps_gs_tri_fast_nr }, { GPU_SH_ps_gs_tri_fast_s, GPU_SH_ps_gs_tri_fast_s_nr },
                                      { GPU_SH_ps_gs_tri_fast_n, GPU_SH_ps_gs_tri_fast_n_nr } };
        set_topo(0);
        set_vs(gpu_vs(fast ? GPU_SH_vs_ff_tri : GPU_SH_vs_ref_tri));
        set_ps(gpu_ps(X.mixed && dbg_skip == 3 && fast ? GPU_SH_ps_ff_t001
                      : svol ? GPU_SH_ps_gs_tri_svol : fast ? tf[sp][nr] : nr ? GPU_SH_ps_gs_tri_nr : GPU_SH_ps_gs_tri));
        ID3D11DeviceContext_DrawIndexedInstanced(c, d->count, 1, d->first, 0, slot);
    } else {
        set_topo(1);
        int sprite = d->cls == GPU_PC_SPRITE, thin = d->cls == GPU_PC_THIN;
        static const int ss[4][2] = { { GPU_SH_ps_gs_sprite, GPU_SH_ps_gs_sprite_nr }, { GPU_SH_ps_gs_sprite_s, GPU_SH_ps_gs_sprite_s_nr },
                                      { GPU_SH_ps_gs_sprite_n, GPU_SH_ps_gs_sprite_n_nr }, { GPU_SH_ps_gs_sprite_w, GPU_SH_ps_gs_sprite_w_nr } };
        static const int sl[3][2] = { { GPU_SH_ps_gs_linepix, GPU_SH_ps_gs_linepix_nr }, { GPU_SH_ps_gs_linepix_s, GPU_SH_ps_gs_linepix_s_nr },
                                      { GPU_SH_ps_gs_linepix_n, GPU_SH_ps_gs_linepix_n_nr } };
        set_vs(gpu_vs(sprite ? GPU_SH_vs_ref_sprite : thin ? GPU_SH_vs_ref_thin : GPU_SH_vs_ref_linepix));
        set_ps(gpu_ps(sprite ? ss[sp][nr] : thin ? (nr ? GPU_SH_ps_gs_thin_nr : GPU_SH_ps_gs_thin) : sl[sp][nr]));
        ID3D11DeviceContext_DrawInstanced(c, 4, d->count, 0, slot);
    }
    gpu_rt.n_draws++;
    XC.ref_draws++;
    if (X.mixed) XC.rov_draws++;
    if (zt && d->zw) zt_mark(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3], ZT_ZP);
}

/* ---- (r2-m7b) the decoded texture cache (modern frames) ------------------------------------------------------------
 * A vram texture's level 0 decoded once into words exactly as gs.hlsl texel() returns them (cs_texdec runs texel() over
 * every texel of level 0), read by the TEXDEC pixel shaders with four plain loads instead of four swizzled reads plus
 * CLUT lookups. Exact by construction: an entry is keyed by the texture's TEX0 (TBP, TBW, PSM, TW, TH), its CLUT
 * identity (a hash of the ring slot's 1 KB snapshot with CSA / CPSM; TEXA for 16 / 24-bit texels) and its content
 * version, the newest write serial of the 8 KB pages it occupies (PATCH and DOWNLOAD ops bump the serial of the pages
 * they write). td_prepass walks the frame's ops before the upload, keeps the page serials and CLUT hashes as the
 * execution will see them, decides per StateRec (gpu_staterec.pad2[0] = 1 + first word, 0: none; a StateRec whose
 * texture changes between two of its draws in one frame gets none) and schedules the decodes at the ops before the
 * draws that need them (the specialised FF triangle permutations, SPEC_NOVIEW / SPEC_NOLOD). Measured in game (ep1 hub,
 * PS2_GPU_AB): GPU 4.46 -> 4.07 ms per frame (exact triangles 1.27 -> 0.99, the sky 0.52 -> 0.38). PS2_R2_TEXDEC=0: off,
 * 3: only draws without a depth target (the sky). */
static uint64_t shf_fused_n, shf_heads_n;   /* (r2-m7b) shadow tails fused, heads deferred (shfuse_try) */
#define TD_WORDS (16u << 20)             /* 64 MB of decoded texels */
#define TD_HASH 8192                     /* entries (open addressing; reset when 3/4 full or out of words) */
#define TD_JOBS 1024
#define TD_FRAME_TEXELS (4u << 20)       /* decodes per frame at most (the rest stay undecoded until a later frame) */
typedef struct { uint64_t k1, k2; uint32_t ver, base, n, frame; int live; } td_ent;
typedef struct { uint32_t pos, tex0lo, tex0hi, texa, clut_slot, base, lw, n; } td_job;
typedef struct { uint32_t tex0lo, tex0hi, clamp0, clamp1, texa, clut_slot, base, lw, n, pad0, pad1, pad2; } td_consts;   /* (gs.hlsl texdec_cb) */
static struct {
    int on;                              /* -1 unknown */
    ID3D11Buffer *buf, *cb;
    ID3D11ShaderResourceView* srv;
    ID3D11UnorderedAccessView* uav;
    uint32_t pgen[512], serial, frame;
    uint64_t chash[4096];
    td_ent ent[TD_HASH];
    uint32_t nent, used, frame_texels;
    int reset;
    td_job job[TD_JOBS];
    uint32_t njob, jnext;
    uint32_t* dpos;                      /* op positions of the draws with decoded StateRecs (in order) */
    uint32_t ndpos, dcap, dnext;
    uint8_t* stcopy;                     /* the frame's StateRecs with pad2[0] set */
    size_t stcap;
    uint32_t* sdec;                      /* per StateRec: 1 + base, 0 none */
    uint8_t* sflag;                      /* 0 untouched, 1 set, 2 none for this frame */
    size_t scap;
    uint64_t decodes, decode_texels, draws;
} TD = { -1 };

static int td_ensure(void) {
    if (TD.on < 0) { const char* e = getenv("PS2_R2_TEXDEC"); TD.on = e ? atoi(e) : 1; }
    if (!TD.on) return 0;
    if (TD.buf) return 1;
    TD.buf = raw_default(TD_WORDS * 4u, NULL, &TD.srv, &TD.uav);
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(td_consts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!TD.buf || FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &TD.cb))) {
        GPU_RELEASE(TD.srv); GPU_RELEASE(TD.uav); GPU_RELEASE(TD.buf);
        TD.on = 0;
        return 0;
    }
    TD.nent = TD.used = 0;
    memset(TD.ent, 0, sizeof TD.ent);
    return 1;
}

static void td_release(void) {
    GPU_RELEASE(TD.srv); GPU_RELEASE(TD.uav); GPU_RELEASE(TD.buf); GPU_RELEASE(TD.cb);
    TD.nent = TD.used = 0;
    memset(TD.ent, 0, sizeof TD.ent);
    memset(TD.chash, 0, sizeof TD.chash);
}

/* the newest write serial of the pages level 0 of a texture occupies (GS page layout of its PSM; a TBP inside a page
 * spills into the next page of every row) */
static uint32_t td_ver(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t lw, uint32_t lh) {
    uint32_t pw = 64, ph = 32, ppr = tbw;
    if (psm == 0x13) { pw = 128; ph = 64; ppr = tbw > 1 ? tbw >> 1 : tbw; }
    else if (psm == 0x14) { pw = 128; ph = 128; ppr = tbw > 1 ? tbw >> 1 : tbw; }
    else if (psm == 0x02 || psm == 0x0A || psm == 0x32 || psm == 0x3A) ph = 64;
    uint32_t w = 1u << lw, h = 1u << lh, cols = (w + pw - 1) / pw + ((tbp & 31) ? 1 : 0), rows = (h + ph - 1) / ph, v = 0;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t g = TD.pgen[((tbp >> 5) + r * ppr + c) & 511];
            if (g > v) v = g;
        }
    return v;
}

static uint64_t td_hash64(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

/* the decoded base (1 + first word) for StateRec s at op position pos, scheduling its decode; 0: none */
static uint32_t td_state(const gpu_staterec* s, uint32_t pos, int64_t area) {
    if (!(s->flags & GPU_SF_TME)) return 0;
    uint32_t tk = s->texsrc & 0xFF;
    if (tk != TK_VRAM_IDX && tk != TK_VRAM_DIRECT) return 0;
    uint32_t texm = s->tlev & 3, cur = (s->tlev >> 2) & 7;
    if ((texm != 1 && texm != 2) || cur) return 0;
    uint64_t t0 = s->tex0;
    uint32_t tbp = (uint32_t)t0 & 0x3FFF, tbw = (uint32_t)(t0 >> 14) & 0x3F, psm = (uint32_t)(t0 >> 20) & 0x3F;
    uint32_t tw = (uint32_t)(t0 >> 26) & 15, th = (uint32_t)(t0 >> 30) & 15;
    if (tw > 10) tw = 10;
    if (th > 10) th = 10;
    uint32_t n = 1u << (tw + th);
    int idx = psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C;
    uint64_t k1 = t0 & 0x3FFFFFFFFull, k2 = 0;
    if (idx) {
        if (s->clut_slot == 0xFFFFFFFFu) return 0;
        uint32_t cpsm = (uint32_t)(t0 >> 51) & 15, csa = (uint32_t)(t0 >> 56) & 31;
        k2 = TD.chash[s->clut_slot & 4095] ^ ((uint64_t)(cpsm | csa << 4) << 52) ^ 1;
        if (cpsm == 0x02 || cpsm == 0x0A) k2 ^= (uint64_t)s->texa * 0x9E3779B97F4A7C15ull;
    } else if (psm != 0x00 && psm != 0x30) k2 = (uint64_t)s->texa * 0x9E3779B97F4A7C15ull + 2;
    uint32_t ver = td_ver(tbp, tbw, psm, tw, th);
    uint32_t h = (uint32_t)((k1 * 0x9E3779B97F4A7C15ull ^ k2) >> 40) & (TD_HASH - 1);
    td_ent* e = NULL;
    for (uint32_t i = 0; i < TD_HASH; i++) {
        td_ent* c = &TD.ent[(h + i) & (TD_HASH - 1)];
        if (!c->live) break;
        if (c->k1 == k1 && c->k2 == k2) { e = c; break; }
    }
    if (e && e->ver == ver) { e->frame = TD.frame; return e->base + 1; }
    if (e && e->frame == TD.frame) return 0;  /* (decoded at another version earlier in this frame) */
    /* a decode: worth it when the draw covers a fair share of the texels (cached entries need none) */
    if (area * 8 < (int64_t)n || TD.frame_texels + n > TD_FRAME_TEXELS || TD.njob >= TD_JOBS) return 0;
    uint32_t base;
    if (e) base = e->base;               /* (a newer version of the same texture: in place) */
    else {
        if (TD.nent >= TD_HASH * 3 / 4 || TD.used + n > TD_WORDS) { TD.reset = 1; return 0; }
        base = TD.used;
        TD.used += n;
        for (uint32_t i = 0;; i++) {
            td_ent* c = &TD.ent[(h + i) & (TD_HASH - 1)];
            if (!c->live) { e = c; break; }
        }
        e->live = 1; e->k1 = k1; e->k2 = k2; e->base = base; e->n = n;
        TD.nent++;
    }
    e->ver = ver;
    e->frame = TD.frame;
    td_job* j = &TD.job[TD.njob++];
    j->pos = pos; j->tex0lo = (uint32_t)t0; j->tex0hi = (uint32_t)(t0 >> 32); j->texa = s->texa;
    j->clut_slot = idx ? s->clut_slot & 4095 : 0; j->base = base; j->lw = tw; j->n = n;
    TD.frame_texels += n;
    return base + 1;
}

/* walk the frame's ops: page serials, CLUT hashes, per-StateRec decisions, decodes to run. Returns the StateRecs to
 * upload (a patched copy, or fd->states when nothing is decoded). */
static const uint8_t* td_prepass(const gpu_front_frame_data* fd) {
    TD.njob = TD.jnext = TD.ndpos = TD.dnext = 0;
    TD.frame++;
    static int opdump = -1;
    if (opdump < 0) { const char* e = getenv("PS2_GPU_OPDUMP"); opdump = e ? atoi(e) : 0; }
    static int stats = -1;
    if (stats < 0) { const char* e = getenv("PS2_R2_STATS"); stats = e && *e && *e != '0'; }
    if (stats && TD.frame % 60 == 0)
        gpu_log("texdec: %llu decodes (%.1f M texels), %llu draws, %u entries, %.1f MB; shadow tails fused %llu (heads %llu)",
                (unsigned long long)TD.decodes, TD.decode_texels / 1e6, (unsigned long long)TD.draws, TD.nent, TD.used * 4.0 / 1048576.0,
                (unsigned long long)shf_fused_n, (unsigned long long)shf_heads_n);
    TD.frame_texels = 0;
    int cand = X.mixed && !X.ref && td_ensure() && !ab_off();
    if (TD.reset && cand) {
        TD.reset = 0;
        TD.nent = TD.used = 0;
        memset(TD.ent, 0, sizeof TD.ent);
    }
    if (cand && TD.scap < fd->state_count) {
        size_t c = fd->state_count + 256;
        uint32_t* a = realloc(TD.sdec, c * 4);
        if (a) TD.sdec = a;
        uint8_t* b = realloc(TD.sflag, c);
        if (b) TD.sflag = b;
        if (a && b) TD.scap = c; else cand = 0;
    }
    if (cand) memset(TD.sflag, 0, fd->state_count);
    int any = 0;
    for (size_t pos = 0; pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        if (opdump == XC.frames + 1) {    /* (r2-m7b, PS2_GPU_OPDUMP=<frame>: every op of that frame) */
            char s[200];
            prof_op_sig(fd, (uint32_t)pos, PR_OTHER, s, sizeof s);
            if (h->op == GPU_FOP_DRAW) {
                const gpu_fop_draw* d = (const gpu_fop_draw*)h;
                size_t l = strlen(s);
                snprintf(s + l, sizeof s - l, " tier %u n %u bb %d,%d-%d,%d split %u", d->tier, d->count, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3], d->split);
            } else if (h->op == GPU_FOP_ALIAS16 || h->op == GPU_FOP_ALIAS16_END) {
                const gpu_fop_alias16* a = (const gpu_fop_alias16*)h;
                size_t l = strlen(s);
                snprintf(s + l, sizeof s - l, " psm %X rect %d,%d-%d,%d mode %u", a->psm, a->x0, a->y0, a->x1, a->y1, a->mode);
            }
            gpu_log("opdump @%u %s", (unsigned)pos, s);
        }
        if (h->op == GPU_FOP_PATCH || h->op == GPU_FOP_DOWNLOAD) {
            const gpu_fop_blocks* o = (const gpu_fop_blocks*)h;
            TD.serial++;
            for (uint32_t i = 0; i < o->n; i++) {
                uint32_t b;
                if (h->op == GPU_FOP_PATCH) {
                    if ((size_t)o->data_off + i * 2u + 2u > fd->data_bytes) break;
                    b = *(const uint16_t*)(fd->data + o->data_off + i * 2u);
                } else {
                    if ((size_t)o->data_off + i * 16u + 16u > fd->data_bytes) break;
                    b = ((const gpu_dl_entry*)(fd->data + o->data_off))[i].abs;
                }
                TD.pgen[(b >> 5) & 511] = TD.serial;
            }
        } else if (h->op == GPU_FOP_CLUT) {
            const gpu_fop_clut* o = (const gpu_fop_clut*)h;
            if ((size_t)o->data_off + 1024 <= fd->data_bytes) TD.chash[o->slot & 4095] = td_hash64(fd->data + o->data_off, 1024);
        } else if (cand && h->op == GPU_FOP_DRAW) {
            const gpu_fop_draw* d = (const gpu_fop_draw*)h;
            if ((d->feat & GPU_DF_TEXDEC) && d->tier == GPU_TIER_FF && d->cls == GPU_PC_TRI && (d->ps_key & GPU_PSK_FF_FAST) &&
                spec_level(d) == 1 && d->count >= 3 && !(TD.on == 3 && d->zt)) {
                int64_t area = (int64_t)(d->bbox[2] - d->bbox[0] + 1) * (d->bbox[3] - d->bbox[1] + 1);
                uint32_t np = d->count / 3, last = 0xFFFFFFFFu;
                int got = 0;
                for (uint32_t i = 0; i < np; i++) {
                    size_t rb = (size_t)d->rec_first + (size_t)i * 128u;
                    if (rb + 116u > fd->rec_bytes) break;
                    uint32_t si = *(const uint32_t*)(fd->rec + rb + 112u);
                    if (si == last || si >= fd->state_count) continue;
                    last = si;
                    uint8_t f = TD.sflag[si];
                    if (f == 2) continue;
                    uint32_t v = td_state((const gpu_staterec*)(fd->states + (size_t)si * GPU_STATE_BYTES), (uint32_t)pos, area);
                    if (f == 0) { TD.sflag[si] = v ? 1 : 2; TD.sdec[si] = v; }
                    else if (TD.sdec[si] != v) { TD.sflag[si] = 2; TD.sdec[si] = 0; }
                    got |= v != 0;
                }
                if (got) {
                    if (TD.ndpos == TD.dcap) {
                        uint32_t c = TD.dcap ? TD.dcap * 2 : 256;
                        uint32_t* a = realloc(TD.dpos, c * 4);
                        if (a) { TD.dpos = a; TD.dcap = c; }
                    }
                    if (TD.ndpos < TD.dcap) TD.dpos[TD.ndpos++] = (uint32_t)pos;
                    any = 1;
                }
            }
        }
        pos += h->size;
    }
    if (!any) return fd->states;
    size_t bytes = fd->state_count * GPU_STATE_BYTES;
    if (TD.stcap < bytes) {
        uint8_t* a = realloc(TD.stcopy, bytes + 65536);
        if (!a) { TD.ndpos = 0; TD.njob = 0; return fd->states; }
        TD.stcopy = a;
        TD.stcap = bytes + 65536;
    }
    memcpy(TD.stcopy, fd->states, bytes);
    for (size_t i = 0; i < fd->state_count; i++)
        ((gpu_staterec*)(TD.stcopy + i * GPU_STATE_BYTES))->pad2[0] = TD.sflag[i] == 1 ? TD.sdec[i] : 0;   /* (pad2 is not zeroed upstream) */
    return TD.stcopy;
}

/* the decodes scheduled before the op at pos */
static void td_run(uint32_t pos) {
    while (TD.jnext < TD.njob && TD.job[TD.jnext].pos < pos) TD.jnext++;   /* (never: decodes precede FF triangle draws) */
    if (TD.jnext >= TD.njob || TD.job[TD.jnext].pos != pos) return;
    if (X.mode) unbind_all();
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* n1 = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &n1);
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_texdec), NULL, 0);
    ID3D11ShaderResourceView* s0 = X.vram_srv;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &s0);
    ID3D11ShaderResourceView* s56[2] = { X.clut_srv, X.swz_srv };
    ID3D11DeviceContext_CSSetShaderResources(c, 5, 2, s56);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &TD.uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 3, 1, &TD.cb);
    int pc = PR.cur;
    prof_mark(PR_MEM);
    for (; TD.jnext < TD.njob && TD.job[TD.jnext].pos == pos; TD.jnext++) {
        const td_job* j = &TD.job[TD.jnext];
        td_consts k = { j->tex0lo, j->tex0hi, 0, 0, j->texa, j->clut_slot, j->base, j->lw, j->n, 0, 0, 0 };
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)TD.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        memcpy(m.pData, &k, sizeof k);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)TD.cb, 0);
        ID3D11DeviceContext_Dispatch(c, (j->n + 63) / 64, 1, 1);
        gpu_rt.n_dispatches++;
        TD.decodes++;
        TD.decode_texels += j->n;
    }
    prof_mark(pc);
    ID3D11UnorderedAccessView* nu = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &nu, NULL);
    ID3D11ShaderResourceView* ns = NULL;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &ns);
    ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &TD.srv);
    X.state_calls += 10;
}

static void td_bind_ps(void) {
    if (TD.srv) ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 13, 1, &TD.srv);
}

/* the draw at pos reads decoded StateRecs */
static int td_draw(uint32_t pos) {
    while (TD.dnext < TD.ndpos && TD.dpos[TD.dnext] < pos) TD.dnext++;
    return TD.dnext < TD.ndpos && TD.dpos[TD.dnext] == pos;
}

/* (r2-m7b) R2 materials (r2_render.c, through r2_exec_in.texdec at the start of a segment): the decoded base (1 + first
 * word) of a material's texture as R2's pixel shader reads it (no TEXA, the CLUT from GS memory at CBP: R2_CLUT), decoded
 * right away when needed (GS memory is current at the segment); 0: none. The CLUT's pages count in the version. */
static uint32_t td_r2(void* user, uint64_t t0) {
    (void)user;
    if (!X.mixed || X.ref || TD.on <= 0 || TD.on == 3 || ab_off() || !td_ensure()) return 0;
    uint32_t tbp = (uint32_t)t0 & 0x3FFF, tbw = (uint32_t)(t0 >> 14) & 0x3F, psm = (uint32_t)(t0 >> 20) & 0x3F;
    uint32_t tw = (uint32_t)(t0 >> 26) & 15, th = (uint32_t)(t0 >> 30) & 15;
    if (tw > 10) tw = 10;
    if (th > 10) th = 10;
    uint32_t n = 1u << (tw + th);
    int idx = psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C;
    uint32_t cbp = (uint32_t)(t0 >> 37) & 0x3FFF;
    uint64_t k1 = t0 & 0x3FFFFFFFFull, k2 = 0x8000000000000000ull | (idx ? (uint64_t)cbp << 1 | 1 : 0);
    uint32_t ver = td_ver(tbp, tbw, psm, tw, th);
    if (idx) {                           /* (a 16x16 CT32 CLUT at CBP: its page and the next, for a CBP inside a page) */
        uint32_t g0 = TD.pgen[(cbp >> 5) & 511], g1 = TD.pgen[((cbp >> 5) + 1) & 511];
        if (g0 > ver) ver = g0;
        if (g1 > ver) ver = g1;
    }
    uint32_t h = (uint32_t)((k1 * 0x9E3779B97F4A7C15ull ^ k2) >> 40) & (TD_HASH - 1);
    td_ent* e = NULL;
    for (uint32_t i = 0; i < TD_HASH; i++) {
        td_ent* c = &TD.ent[(h + i) & (TD_HASH - 1)];
        if (!c->live) break;
        if (c->k1 == k1 && c->k2 == k2) { e = c; break; }
    }
    if (e && e->ver == ver) { e->frame = TD.frame; return e->base + 1; }
    if (e && e->frame == TD.frame) return 0;
    if (TD.frame_texels + n > TD_FRAME_TEXELS) return 0;
    uint32_t base;
    if (e) base = e->base;
    else {
        if (TD.nent >= TD_HASH * 3 / 4 || TD.used + n > TD_WORDS) { TD.reset = 1; return 0; }
        base = TD.used;
        TD.used += n;
        for (uint32_t i = 0;; i++) {
            td_ent* c = &TD.ent[(h + i) & (TD_HASH - 1)];
            if (!c->live) { e = c; break; }
        }
        e->live = 1; e->k1 = k1; e->k2 = k2; e->base = base; e->n = n;
        TD.nent++;
    }
    e->ver = ver;
    e->frame = TD.frame;
    TD.frame_texels += n;
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* n1 = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 13, 1, &n1);
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_texdec_r2), NULL, 0);
    ID3D11ShaderResourceView* s0 = X.vram_srv;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &s0);
    ID3D11DeviceContext_CSSetShaderResources(c, 6, 1, &X.swz_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &TD.uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 3, 1, &TD.cb);
    td_consts k = { (uint32_t)t0, (uint32_t)(t0 >> 32), 0, 0, 0, 0, base, tw, n, 0, 0, 0 };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)TD.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &k, sizeof k);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)TD.cb, 0);
        ID3D11DeviceContext_Dispatch(c, (n + 63) / 64, 1, 1);
        gpu_rt.n_dispatches++;
        TD.decodes++;
        TD.decode_texels += n;
    } else e->ver = 0xFFFFFFFFu;         /* (not decoded: the next use tries again) */
    ID3D11UnorderedAccessView* nu = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &nu, NULL);
    ID3D11ShaderResourceView* ns = NULL;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &ns);
    X.state_calls += 9;
    return e->ver == 0xFFFFFFFFu ? 0 : base + 1;
}

/* (r2-m6, R6) particle draws (GPU_DF_PART) run the native particle pipeline: modern frames only (the frontend marks them
 * only there), not in the A phases of PS2_GPU_AB; PS2_GPU_PART=0: off here (the exact FF shaders draw them) */
static int part_exec(void) {
    static int env = -1;
    if (env < 0) { const char* e = getenv("PS2_GPU_PART"); env = !(e && *e == '0'); }
    return env && X.mixed && !X.ref && !ab_off();
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
    int sp = spec_level(d);
    if (d->cls == GPU_PC_TRI) {
        set_topo(0);
        set_vs(gpu_vs(GPU_SH_vs_ff_tri));
        static const int t[2][2][2] = { { { GPU_SH_ps_ff_t000, GPU_SH_ps_ff_t001 }, { GPU_SH_ps_ff_t010, GPU_SH_ps_ff_t011 } },
                                        { { GPU_SH_ps_ff_t100, GPU_SH_ps_ff_t101 }, { GPU_SH_ps_ff_t110, GPU_SH_ps_ff_t111 } } };
        static const int ts[3][2][2] = { { { 0 } },
                                         { { GPU_SH_ps_ff_t001s, GPU_SH_ps_ff_t011s }, { GPU_SH_ps_ff_t101s, GPU_SH_ps_ff_t111s } },
                                         { { GPU_SH_ps_ff_t001n, GPU_SH_ps_ff_t011n }, { GPU_SH_ps_ff_t101n, GPU_SH_ps_ff_t111n } } };
        int fast = (d->ps_key & GPU_PSK_FF_FAST) != 0;
        ps = sp && fast ? ts[sp][dual][zout] : t[dual][zout][fast];
        static const int td[2][2] = { { GPU_SH_ps_ff_t001d, GPU_SH_ps_ff_t011d }, { GPU_SH_ps_ff_t101d, GPU_SH_ps_ff_t111d } };
        if (sp == 1 && fast && (d->feat & GPU_DF_PART) && part_exec()) {
            /* (r2-m6, R6) particle triangles: the native particle pipeline (hardware-interpolated attributes and Z, no
             * conservative depth output; shaders/gs.hlsl ps_ff_part) */
            static const int pp[2][2] = { { GPU_SH_ps_ff_p0, GPU_SH_ps_ff_p0d }, { GPU_SH_ps_ff_p1, GPU_SH_ps_ff_p1d } };
            int tdd = TD.ndpos && td_draw(X.curpos);
            if (tdd) TD.draws++;
            set_vs(gpu_vs(GPU_SH_vs_ff_part));
            ps = pp[dual][tdd];
            XC.part_draws++;
        } else
        if (sp == 1 && fast && TD.ndpos && td_draw(X.curpos)) { ps = td[dual][zout]; TD.draws++; }   /* (r2-m7b) */
    } else {
        set_topo(1);
        int sprite = d->cls == GPU_PC_SPRITE, thin = d->cls == GPU_PC_THIN;
        set_vs(gpu_vs(sprite ? GPU_SH_vs_ref_sprite : thin ? GPU_SH_vs_ref_thin : GPU_SH_vs_ref_linepix));
        static const int s[4][2][2] = { { { GPU_SH_ps_ff_s00, GPU_SH_ps_ff_s01 }, { GPU_SH_ps_ff_s10, GPU_SH_ps_ff_s11 } },
                                        { { GPU_SH_ps_ff_s00s, GPU_SH_ps_ff_s01s }, { GPU_SH_ps_ff_s10s, GPU_SH_ps_ff_s11s } },
                                        { { GPU_SH_ps_ff_s00n, GPU_SH_ps_ff_s01n }, { GPU_SH_ps_ff_s10n, GPU_SH_ps_ff_s11n } },
                                        { { GPU_SH_ps_ff_s00w, GPU_SH_ps_ff_s01w }, { GPU_SH_ps_ff_s10w, GPU_SH_ps_ff_s11w } } };
        static const int l[3][2][2] = { { { GPU_SH_ps_ff_l00, GPU_SH_ps_ff_l01 }, { GPU_SH_ps_ff_l10, GPU_SH_ps_ff_l11 } },
                                        { { GPU_SH_ps_ff_l00s, GPU_SH_ps_ff_l01s }, { GPU_SH_ps_ff_l10s, GPU_SH_ps_ff_l11s } },
                                        { { GPU_SH_ps_ff_l00n, GPU_SH_ps_ff_l01n }, { GPU_SH_ps_ff_l10n, GPU_SH_ps_ff_l11n } } };
        static const int n[2][2] = { { GPU_SH_ps_ff_n00, GPU_SH_ps_ff_n01 }, { GPU_SH_ps_ff_n10, GPU_SH_ps_ff_n11 } };   /* (thin: no spec, spec_level) */
        ps = sprite ? s[sp][dual][zout] : thin ? n[dual][zout] : l[sp][dual][zout];
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
        static int dbgd = -1;                /* (M9 experiments, wrong pictures: PS2_GPU_DBG_DRAW bits: 1 keep the first blend /
                                                depth states, 4 skip the draw, 8 one primitive) */
        if (dbgd < 0) { const char* e = getenv("PS2_GPU_DBG_DRAW"); dbgd = e ? atoi(e) : 0; }
        if (dbgd & 4) continue;
        if (!(dbgd & 1) || !X.b_bs) { set_blend(blend_state(d->blend, wm)); set_depth(depth_state(depth)); }
        /* (M9) the constants by the draw's slot (StartInstanceLocation -> DRAWID); the fail half of a split is the next slot */
        UINT s = slot + (uint32_t)(half && d->split == 3);
        if (d->cls == GPU_PC_TRI) ID3D11DeviceContext_DrawIndexedInstanced(c, (dbgd & 8) ? 3 : d->count, 1, d->first, 0, s);
        else ID3D11DeviceContext_DrawInstanced(c, 4, (dbgd & 8) ? 1 : d->count, 0, s);
        gpu_rt.n_draws++;
        XC.ff_draws++;
        zwrote |= depth & 1;
    }
    if (zuse && zwrote) zt_mark(zt, d->bbox[0], d->bbox[1], d->bbox[2], d->bbox[3], ZT_DSV);
}

/* (r2-m7) native post passes of modern frames (gpu_front.h GPU_DF_NPOST; shaders/post.hlsl): an FF draw of sprites that
 * each read a target view one texel per pixel at the texel centres - texel (x, y) is the sprite's own pixel - is one
 * cs_npost dispatch over its samples, each reading its own sample of the source (the 1:1 rule) and writing the GS word.
 * Every condition the compute pass assumes is checked here from the records and the StateRec; anything else stays the
 * FF draw (returns 0). Sly: the shadow darken (T8H of the frame through the CLUT 0x1800) and the motion blur. */
/* (r2-m7b) the checks and constants of a post pass (exec_npost, shfuse): 0 not one; rects rc[nr] (native, disjoint),
 * their bounding box bb, whole: the union is the box */
typedef struct { npost_consts k; int rc[64][4], nr, bb[4], whole; } npost_plan;
/* (r2-m6) flat post passes (untextured sprites, npost_prep): PS2_R2_NPOST_FLAT=0 off; off in PS2_GPU_AB's A phases */
static int flat_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS2_R2_NPOST_FLAT"); on = !(e && *e == '0'); }
    return on;
}
static int npost_prep(const gpu_fop_draw* d, npost_plan* P) {
    static int on = -1;                  /* PS2_R2_NPOST=0: the FF draws (A/B) */
    if (on < 0) { const char* e = getenv("PS2_R2_NPOST"); on = !(e && *e == '0'); }
    const gpu_front_frame_data* fd = X.fd;
    /* (r2-m6) flat: untextured sprites of one colour (a masked fill or a blend with the destination: m2's shadow passes
     * write G bit planes under FBMSK FFFF80FF / FFFFFEFF, which only the ROV tier draws) - every sample's word is a
     * function of its own destination word, as for the textured passes; FF or ROV tier */
    int flat = d->tex == 0 && !(d->feat & GPU_DF_TEX) && flat_on() && !ab_off();
    if (!on || !fd || !X.mixed || (d->tier != GPU_TIER_FF && !(flat && d->tier == GPU_TIER_ROV)) || d->cls != GPU_PC_SPRITE ||
        !d->count || d->count > 64 || (d->depth & 1) || (d->depth >> 1) > 1 || d->split)
        return 0;
    xtarget *t = tgt(d->ct), *v = flat ? t : tgt(d->tex);
    if (!t || t->depth || !v || v->depth || t->kx != v->kx || t->ky != v->ky || !t->uav32 || !v->uav32) return 0;
    if ((size_t)d->rec_first + (size_t)d->count * sizeof(gpu_rec_sprite) > fd->rec_bytes) return 0;
    const gpu_rec_sprite* r0 = (const gpu_rec_sprite*)(fd->rec + d->rec_first);
    uint32_t si = r0->state;
    if (si >= fd->state_count) return 0;
    const gpu_staterec* s = (const gpu_staterec*)(fd->states + (size_t)si * GPU_STATE_BYTES);
    uint32_t tk = s->texsrc & 0xFF, psm = (uint32_t)(s->tex0 >> 20) & 0x3F;
    uint32_t tbp = (uint32_t)s->tex0 & 0x3FFF, tbw = (uint32_t)(s->tex0 >> 14) & 0x3F;
    uint32_t f = s->flags, tfx = (f >> GPU_SF_TFX_SHIFT) & 3;
    uint32_t clut = 0;
    if (flat) {
        if ((f & (GPU_SF_TME | GPU_SF_FGE | GPU_SF_AA1 | GPU_SF_PABE | GPU_SF_DTHE | GPU_SF_FB24 | GPU_SF_FB16)) ||
            ((f >> GPU_SF_SCANMSK_SHIFT) & 3))
            return 0;
    } else {
        if (!(f & GPU_SF_TME) || !(f & GPU_SF_FST) || (f & (GPU_SF_FGE | GPU_SF_AA1 | GPU_SF_PABE | GPU_SF_DTHE | GPU_SF_FB24 | GPU_SF_FB16)) ||
            ((f >> GPU_SF_SCANMSK_SHIFT) & 3) || tfx > 1 || (s->texsrc & GPU_TS_SELF) || ((s->texsrc >> 8) & 0xFF) != d->tex)
            return 0;
        if (!((tk == TK_VIEW_C32 && psm == 0x00) || (tk == TK_VIEW_ALPHA && psm == 0x1B))) return 0;
        if (tbp != (uint32_t)v->base * 32u || tbw != (uint32_t)v->fbw) return 0;   /* (texel (x, y) = the view's pixel (x, y)) */
        uint32_t texm = s->tlev & 3, mxl = (s->tlev >> 5) & 7;
        if ((texm != 1 && texm != 2) || mxl) return 0;
        if (tk == TK_VIEW_ALPHA) {
            uint32_t cpsm = (uint32_t)(s->tex0 >> 51) & 15, csa = (uint32_t)(s->tex0 >> 56) & 31;
            if (cpsm != 0 || s->clut_slot == 0xFFFFFFFFu) return 0;
            clut = (s->clut_slot & 4095) | (csa * 16u) << 16;
        }
    }
    uint32_t ate = s->test & 1, atst = (s->test >> 1) & 7, afail = (s->test >> 12) & 3, date = (s->test >> 14) & 1;
    int rgb_only = 0;
    if (date) return 0;
    if (ate && atst == 0) {              /* NEVER: what AFAIL writes */
        if (afail == 3) rgb_only = 1;
        else if (afail != 1) return 0;
    } else if (ate && atst != 1) return 0;
    /* the sprites: one state and colour, one texel per pixel and row at the texel centres, inside the scissor, disjoint */
    int sx0 = d->sc[0], sy0 = d->sc[1], sx1 = d->sc[2] + 1, sy1 = d->sc[3] + 1;
    int bx0 = 1 << 20, by0 = 1 << 20, bx1 = -(1 << 20), by1 = -(1 << 20);
    int64_t area = 0;
    int (*rc)[4] = P->rc, nr = 0;
    for (uint32_t i = 0; i < d->count; i++) {
        const gpu_rec_sprite* r = r0 + i;
        if (r->state != si || r->rgba != r0->rgba) return 0;
        if (!flat && (!(r->flags & 1) || r->du != (16 << 12) || r->vcy != 16.0 || r->vc0 != 8.0 ||
                      r->ua != ((16 * (int32_t)r->x0 + 8) << 12)))
            return 0;
        int x0 = r->x0 > sx0 ? r->x0 : sx0, y0 = r->y0 > sy0 ? r->y0 : sy0;
        int x1 = r->x1 < sx1 ? r->x1 : sx1, y1 = r->y1 < sy1 ? r->y1 : sy1;
        if (x0 >= x1 || y0 >= y1) continue;
        for (int j = 0; j < nr; j++)
            if (x0 < rc[j][2] && rc[j][0] < x1 && y0 < rc[j][3] && rc[j][1] < y1) return 0;
        rc[nr][0] = x0; rc[nr][1] = y0; rc[nr][2] = x1; rc[nr][3] = y1; nr++;
        area += (int64_t)(x1 - x0) * (y1 - y0);
        if (x0 < bx0) bx0 = x0;
        if (y0 < by0) by0 = y0;
        if (x1 > bx1) bx1 = x1;
        if (y1 > by1) by1 = y1;
    }
    P->nr = nr;
    P->whole = nr && area == (int64_t)(bx1 - bx0) * (by1 - by0);
    P->bb[0] = bx0; P->bb[1] = by0; P->bb[2] = bx1; P->bb[3] = by1;
    npost_consts k;
    memset(&k, 0, sizeof k);
    int same = flat || d->tex == d->ct;
    k.mode = (!flat && tk == TK_VIEW_ALPHA ? 1u : 0u) | (same ? 2u : 0u) | ((f & GPU_SF_ABE) ? 4u : 0u) | ((f & GPU_SF_COLCLAMP) ? 8u : 0u) |
             ((f & GPU_SF_FBA) ? 16u : 0u) | ((f & GPU_SF_TCC) ? 32u : 0u) | (tfx == 1 ? 64u : 0u) | (rgb_only ? 128u : 0u) |
             (flat ? 256u : 0u);
    k.alpha = s->alpha;
    k.fmask = s->fmask;
    k.vcol = r0->rgba;
    k.clut = clut;
    P->k = k;
    return 1;
}

static int exec_npost(const gpu_fop_draw* d) {
    static npost_plan P;
    if (!npost_prep(d, &P)) return 0;
    if (!P.nr) return 1;                 /* (nothing inside the scissor) */
    if (!npcb_ensure()) return 0;
    npost_consts k = P.k;
    int nr = P.nr, whole = P.whole, same = (k.mode & 2u) != 0;   /* (r2-m6: flat passes read only the destination) */
    int bx0 = P.bb[0], by0 = P.bb[1], bx1 = P.bb[2], by1 = P.bb[3];
    xtarget *t = tgt(d->ct), *v = same ? t : tgt(d->tex);
    if (X.mode) unbind_all();
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11ShaderResourceView* none2[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(c, 1, 2, none2);   /* (the targets may be views t1 / t2) */
    X.b_t1 = X.b_t2 = NULL;
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_npost), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 5, 1, &X.clut_srv);
    ID3D11UnorderedAccessView* u[2] = { t->uav32, same ? NULL : v->uav32 };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, u, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 2, 1, &X.npcb);
    int kx = t->kx, ky = t->ky;
    for (int i = 0; i < (whole ? 1 : nr); i++) {
        const int* q = whole ? (int[4]){ bx0, by0, bx1, by1 } : P.rc[i];
        k.rect[0] = (uint32_t)(q[0] * kx); k.rect[1] = (uint32_t)(q[1] * ky);
        k.rect[2] = (uint32_t)(q[2] * kx); k.rect[3] = (uint32_t)(q[3] * ky);
        if (k.rect[2] > (uint32_t)t->w) k.rect[2] = (uint32_t)t->w;
        if (k.rect[3] > (uint32_t)t->h) k.rect[3] = (uint32_t)t->h;
        if (k.rect[0] >= k.rect[2] || k.rect[1] >= k.rect[3]) continue;
        if (!npcb_write(&k)) break;
        ID3D11DeviceContext_Dispatch(c, (k.rect[2] - k.rect[0] + 7) / 8, (k.rect[3] - k.rect[1] + 7) / 8, 1);
        gpu_rt.n_dispatches++;
    }
    ID3D11UnorderedAccessView* nu[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, nu, NULL);
    X.state_calls += 10;
    XC.npost_draws++;
    return 1;
}

/* ---- (r2-m7b) the shadow pipeline's tail in one pass (shaders/post.hlsl cs_shfuse) ---------------------------------
 * In a modern frame Sly ends its shadow pipeline with: the volumes (their G steps pending in the counter texture), an
 * ALIAS16 copy run on the frame B (shuffle 2: B.alpha bits 0 / 7 <- B.G; shuffle 3: B.G <- A.alpha, A the other frame),
 * a feedback copy of B and the darken (a post pass reading B itself 1:1). Four full-screen passes (counter resolve, two
 * copy dispatches, the darken) become one when every copy draw is provably a function of the other half of the same
 * 32-bit sample (shf_copy: CT16 views with B's width, sprites of 8 columns stepping one texel per pixel 8 columns away,
 * covering whole pages; the CT16 swizzle puts columns x and x + 8 of a 16-wide block into the two halves of one word,
 * checked once from the tables) and the darken covers the same rectangle. The feedback copy is dropped: the darken
 * rewrites every pixel it covers, so the frontend copies again before any later feedback read. PS2_R2_SHFUSE=0: off. */
typedef struct { uint32_t rect[4], svbb[4], c[3][4], clr[4], np[4], np2[4]; } shf_consts;
_Static_assert(sizeof(shf_consts) == 128, "shf_cb");
static struct {
    ID3D11Buffer* cb;
    int lay, hlo, on;
    uint64_t fused, heads;
    /* a deferred head (shuffle 1 + the G clear) waiting for the tail at op pend_tail */
    int pend, pend_b, pend_a, pend_w, pend_h;
    size_t pend_tail;
    uint32_t pc[4], pclr[4];
} SF = { NULL, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, { 0 }, { 0 } };

/* the CT16 page layout: halfwords of columns x (x % 16 < 8) and x + 8 of a row share a word, x's half is SF.hlo */
static int shf_layout(void) {
    if (SF.lay) return SF.lay > 0;
    SF.lay = 1;
    SF.hlo = gs_swz_ct16[0] & 1;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            if ((x & 15) >= 8) continue;
            uint32_t a = gs_swz_ct16[(y << 6) | x], b = gs_swz_ct16[(y << 6) | (x + 8)];
            if ((int)(a & 1) != SF.hlo || b != (a ^ 1)) SF.lay = -1;
        }
    return SF.lay > 0;
}

/* a copy draw (of an ALIAS16 copy run, 16-bit frame psm16 over its colour target) that is a function of the other half
 * of the same 32-bit sample, B the fused pass's frame: its constants (gs post.hlsl sf_c) and the covered native rect */
static int shf_copy(const gpu_fop_draw* d, int B, uint32_t psm16, uint32_t c[4], int* W, int* H) {
    const gpu_front_frame_data* fd = X.fd;
    xtarget *t = tgt(d->ct), *v = tgt(d->tex);
    if (d->tier != GPU_TIER_ALIAS16 || d->cls != GPU_PC_SPRITE || !d->count || d->count > 64 || !t || !v || t->depth ||
        v->depth || v->kx != t->kx || v->ky != t->ky || v->fbw != t->fbw || !v->uav32 || !t->uav32 || psm16 != 0x02)
        return 0;
    if ((size_t)d->rec_first + (size_t)d->count * sizeof(gpu_rec_sprite) > fd->rec_bytes) return 0;
    const gpu_rec_sprite* r0 = (const gpu_rec_sprite*)(fd->rec + d->rec_first);
    if (r0->state >= fd->state_count) return 0;
    const gpu_staterec* s = (const gpu_staterec*)(fd->states + (size_t)r0->state * GPU_STATE_BYTES);
    uint32_t psm = (uint32_t)(s->tex0 >> 20) & 0x3F, tbp = (uint32_t)s->tex0 & 0x3FFF, tbw = (uint32_t)(s->tex0 >> 14) & 0x3F;
    uint32_t tw = (uint32_t)(s->tex0 >> 26) & 15, th = (uint32_t)(s->tex0 >> 30) & 15;
    if (!(s->flags & GPU_SF_TME) || psm != 0x02 || tbp != (uint32_t)v->base * 32u || tbw != (uint32_t)v->fbw) return 0;
    if ((s->clamp & 3) > 1 || ((s->clamp >> 2) & 3) > 1) return 0;   /* (REPEAT / CLAMP: identity inside the texture) */
    int w16 = t->fbw * 64, h16 = r0->y1;
    if (tw > 10) tw = 10;
    if (th > 10) th = 10;
    if (w16 <= 0 || w16 > 1024 || h16 <= 0 || (h16 & 63) || (1 << tw) < w16 || (1 << th) < h16 || h16 / 2 > t->nat_h) return 0;
    if (r0->ua & 0xFFF) return 0;
    int u0 = r0->ua >> 12;               /* (texel centre of the first column, 1/16) */
    if ((u0 - 8) & 15) return 0;
    int dx = (u0 - 8) / 16 - r0->x0;
    if (dx != 8 && dx != -8) return 0;
    if (d->sc[0] > 0 || d->sc[1] > 0 || d->sc[2] < w16 - 1 || d->sc[3] < h16 - 1) return 0;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < d->count; i++) {
        const gpu_rec_sprite* r = r0 + i;
        if (r->state != r0->state || r->rgba != r0->rgba || !(r->flags & 1) || r->du != (16 << 12) || r->vcy != 16.0 ||
            r->vc0 != 8.0 || r->y0 != 0 || r->y1 != h16 || r->x1 - r->x0 != 8 || (r->x0 & 15) != (dx > 0 ? 0 : 8) ||
            r->x0 < 0 || r->x0 >= w16 || r->ua != ((16 * (r->x0 + dx) + 8) << 12) || ((seen >> (r->x0 >> 4)) & 1))
            return 0;
        seen |= 1ull << (r->x0 >> 4);
    }
    if ((int)d->count != w16 / 16) return 0;
    uint32_t hd = dx > 0 ? (uint32_t)SF.hlo : (uint32_t)SF.hlo ^ 1, hs = hd ^ 1;
    c[0] = s->texa;
    c[1] = s->flags;
    c[2] = r0->rgba;
    c[3] = (~s->fmask & 0xFFFFu) | hs << 16 | hd << 17 | (uint32_t)(d->tex != B) << 18 | 1u << 19 | (uint32_t)(d->ct != B) << 20;
    *W = w16;
    *H = h16 / 2;
    return 1;
}

/* the tail at ALIAS16 op pos (a copy run on B, its feedback copy, the darken): constants (no head, no counter) */
typedef struct { shf_consts k; int B, A, W, H; size_t next; uint32_t slots; } shf_plan;
static int shf_tail(const gpu_front_frame_data* fd, size_t pos, shf_plan* L) {
    const gpu_fop_alias16* a = (const gpu_fop_alias16*)(fd->cmd + pos);
    if (pos + sizeof *a > fd->cmd_bytes || a->h.op != GPU_FOP_ALIAS16 || a->h.size < sizeof *a || a->mode != 1) return 0;
    int bt = a->target;
    xtarget* t = tgt((uint32_t)bt);
    if (!t || t->depth || !t->uav32) return 0;
    memset(L, 0, sizeof *L);
    int ncopy = 0, W = 0, H = 0, at = 0;
    uint32_t sl = 0;
    size_t p = pos + a->h.size;
    const gpu_fop_draw* np = NULL;
    for (int step = 0; step < 8 && p + sizeof(gpu_fop) <= fd->cmd_bytes; step++) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + p);
        if (!h->size) return 0;
        if (h->op == GPU_FOP_DRAW && ncopy >= 0 && ((const gpu_fop_draw*)h)->tier == GPU_TIER_ALIAS16) {
            const gpu_fop_draw* d = (const gpu_fop_draw*)h;
            int w, hh;
            if (ncopy == 2 || d->ct != bt || !shf_copy(d, bt, a->psm, L->k.c[1 + ncopy], &w, &hh)) return 0;
            if (ncopy && (w != W || hh != H)) return 0;
            if (d->tex != bt) { if (at && at != d->tex) return 0; at = d->tex; }
            W = w; H = hh;
            ncopy++;
            sl += draw_slots(d, 1);
        } else if (h->op == GPU_FOP_ALIAS16_END && ncopy > 0) {
            if (((const gpu_fop_alias16*)h)->target != bt) return 0;
            ncopy = -ncopy;              /* (the run is closed) */
        } else if (h->op == GPU_FOP_COPY_SHADOW && ncopy < 0) {
            const gpu_fop_shadow* o = (const gpu_fop_shadow*)h;
            if (o->target != bt || o->x0 < 0 || o->y0 < 0 || o->x1 >= W || o->y1 >= H) return 0;
        } else if (h->op == GPU_FOP_DRAW && ncopy < 0) {
            np = (const gpu_fop_draw*)h;
            p += h->size;
            break;
        } else return 0;
        p += h->size;
    }
    if (!np || !(np->feat & GPU_DF_NPOST) || np->ct != bt || np->tex != bt) return 0;
    static npost_plan P;
    if (!npost_prep(np, &P) || !P.whole || P.bb[0] != 0 || P.bb[1] != 0 || P.bb[2] != W || P.bb[3] != H) return 0;
    if (at && !tgt((uint32_t)at)) return 0;
    L->k.np[0] = P.k.mode; L->k.np[1] = P.k.alpha; L->k.np[2] = P.k.fmask; L->k.np[3] = P.k.vcol;
    L->k.np2[0] = P.k.clut; L->k.np2[1] = 1;
    L->B = bt; L->A = at; L->W = W; L->H = H;
    L->next = p;
    L->slots = sl + draw_slots(np, 1);
    return 1;
}

static int shf_enabled(void) {
    if (SF.on < 0) { const char* e = getenv("PS2_R2_SHFUSE"); SF.on = !(e && *e == '0'); }
    return SF.on && X.mixed && !X.ref && !ab_off() && shf_layout();
}

/* at ALIAS16 op pos: shuffle 1 (a copy run on A reading B) and the G clear of B, followed by shadow-volume draws on B
 * only and then a tail that fuses: both are deferred into the tail's pass (*next: the op after the clear) */
static int shf_head(const gpu_front_frame_data* fd, size_t pos, size_t* next, uint32_t* slots) {
    if (SF.pend || !shf_enabled()) return 0;
    const gpu_fop_alias16* a = (const gpu_fop_alias16*)(fd->cmd + pos);
    if (a->h.size < sizeof *a || a->mode != 1) return 0;
    size_t p = pos + a->h.size;
    const gpu_fop_draw* d = (const gpu_fop_draw*)(fd->cmd + p);
    if (p + sizeof *d > fd->cmd_bytes || d->h.op != GPU_FOP_DRAW || d->ct != a->target || d->tex == a->target) return 0;
    int bt = d->tex, w, h;
    uint32_t c[4];
    if (!shf_copy(d, bt, a->psm, c, &w, &h)) return 0;
    p += d->h.size;
    const gpu_fop_alias16* e = (const gpu_fop_alias16*)(fd->cmd + p);
    if (p + sizeof(gpu_fop) > fd->cmd_bytes || e->h.op != GPU_FOP_ALIAS16_END || e->target != a->target) return 0;
    p += e->h.size;
    const gpu_fop_clear* cl = (const gpu_fop_clear*)(fd->cmd + p);
    if (p + sizeof *cl > fd->cmd_bytes || cl->h.op != GPU_FOP_CLEAR || cl->target != bt || cl->zclear || cl->mask != 2) return 0;
    xtarget* tb = tgt((uint32_t)bt);
    int cx1 = cl->x1 > 0 ? cl->x1 : tb ? tb->nat_w : 0, cy1 = cl->y1 > 0 ? cl->y1 : tb ? tb->nat_h : 0;
    if (cx1 != w || cy1 != h) return 0;
    size_t after_clear = p + cl->h.size;
    int nvol = 0;
    for (p = after_clear; p + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* o = (const gpu_fop*)(fd->cmd + p);
        if (!o->size) return 0;
        if (o->op == GPU_FOP_DRAW) {
            const gpu_fop_draw* v = (const gpu_fop_draw*)o;
            if (!(v->feat & GPU_DF_SVOL) || v->ct != bt) return 0;
            nvol++;
        } else if (o->op != GPU_FOP_ROV_END) break;
        p += o->size;
    }
    if (!nvol) return 0;
    shf_plan L;
    if (!shf_tail(fd, p, &L) || L.B != bt || L.A != a->target || L.W != w || L.H != h) return 0;
    SF.pend = 1; SF.pend_b = bt; SF.pend_a = a->target; SF.pend_tail = p; SF.pend_w = w; SF.pend_h = h;
    memcpy(SF.pc, c, sizeof c);
    SF.pclr[0] = 1; SF.pclr[1] = cl->value; SF.pclr[2] = 0x0000FF00u; SF.pclr[3] = 0;
    SF.heads++;
    shf_heads_n++;
    *next = after_clear;
    *slots = draw_slots(d, 1);
    return 1;
}

/* the fused pass: the plan's tail (or none: tail == 0) with the pending head and B's pending volume counters */
static int shf_exec(shf_plan* L, int tail) {
    xtarget* t = tgt((uint32_t)L->B);
    xtarget* av = L->A ? tgt((uint32_t)L->A) : NULL;
    if (!t) return 0;
    shf_consts* k = &L->k;
    if (!tail) memset(k, 0, sizeof *k);
    if (SF.pend) { memcpy(k->c[0], SF.pc, sizeof SF.pc); memcpy(k->clr, SF.pclr, sizeof SF.pclr); k->np2[2] = 1; }
    if (X.sv_on && X.sv_ct != L->B) svol_resolve();
    int kx = t->kx, ky = t->ky, rest = 0;
    k->rect[0] = 0; k->rect[1] = 0;
    k->rect[2] = (uint32_t)(L->W * kx < t->w ? L->W * kx : t->w);
    k->rect[3] = (uint32_t)(L->H * ky < t->h ? L->H * ky : t->h);
    if (X.sv_on) {
        ID3D11ShaderResourceView* n1 = NULL;
        ID3D11DeviceContext_PSSetShaderResources(gpu_rt.ctx, 12, 1, &n1);
        int x0 = (X.sv_bb[0] - 1) * kx, y0 = (X.sv_bb[1] - 1) * ky, x1 = (X.sv_bb[2] + 2) * kx, y1 = (X.sv_bb[3] + 2) * ky;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > X.svcnt_w) x1 = X.svcnt_w;
        if (y1 > X.svcnt_h) y1 = X.svcnt_h;
        rest = x1 > (int)k->rect[2] || y1 > (int)k->rect[3];   /* (counters outside: resolved after, those inside are 0 then) */
        if (x1 > (int)k->rect[2]) x1 = (int)k->rect[2];
        if (y1 > (int)k->rect[3]) y1 = (int)k->rect[3];
        if (x0 < x1 && y0 < y1) { k->svbb[0] = (uint32_t)x0; k->svbb[1] = (uint32_t)y0; k->svbb[2] = (uint32_t)x1; k->svbb[3] = (uint32_t)y1; }
        X.sv_on = rest;
    }
    if ((k->c[1][3] & (1u << 18)) || (k->c[2][3] & (1u << 18)) || k->np2[2]) { if (!av) return 0; }
    ID3D11DeviceContext* c = gpu_rt.ctx;
    if (!SF.cb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(shf_consts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &SF.cb))) return 0;
    }
    if (X.mode) unbind_all();
    ID3D11ShaderResourceView* none2[2] = { NULL, NULL };
    ID3D11DeviceContext_PSSetShaderResources(c, 1, 2, none2);   /* (the targets may be views t1 / t2) */
    X.b_t1 = X.b_t2 = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)SF.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, k, sizeof *k);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)SF.cb, 0);
    int pc = PR.cur;
    prof_mark(PR_A16_DRAW);
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_shfuse), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 5, 1, &X.clut_srv);
    ID3D11UnorderedAccessView* u[3] = { t->uav32, av ? av->uav32 : NULL, k->svbb[2] ? X.svcnt_uav : NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, u, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 4, 1, &SF.cb);
    ID3D11DeviceContext_Dispatch(c, (k->rect[2] + 7) / 8, (k->rect[3] + 7) / 8, 1);
    ID3D11UnorderedAccessView* nu[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, nu, NULL);
    prof_mark(pc);
    gpu_rt.n_dispatches++;
    X.state_calls += 10;
    SF.pend = 0;
    if (rest) svol_resolve();
    return 1;
}

/* at ALIAS16 op pos (the main loop): a deferred head, the head itself or a tail. 1: ops up to *next are done */
static int shfuse_try(const gpu_front_frame_data* fd, size_t pos, size_t* next, uint32_t* slots) {
    if (SF.pend && pos == SF.pend_tail) {
        shf_plan L;
        if (shf_tail(fd, pos, &L) && L.B == SF.pend_b && L.A == SF.pend_a && shf_exec(&L, 1)) {
            SF.fused++;
            shf_fused_n++;
            XC.npost_draws++;
            *next = L.next;
            *slots = L.slots;
            return 1;
        }
        /* (never expected: the head was checked with this tail) the head alone, before the counters are resolved */
        memset(&L, 0, sizeof L);
        L.B = SF.pend_b; L.A = SF.pend_a;
        L.W = SF.pend_w; L.H = SF.pend_h;
        int sv = X.sv_on;
        X.sv_on = 0;                     /* (the counters stay pending: resolved by the normal path) */
        shf_exec(&L, 0);
        X.sv_on = sv;
        SF.pend = 0;
        return 0;
    }
    if (shf_head(fd, pos, next, slots)) return 1;
    shf_plan L;
    if (!shf_enabled() || !shf_tail(fd, pos, &L) || !shf_exec(&L, 1)) return 0;
    SF.fused++;
    shf_fused_n++;
    XC.npost_draws++;
    *next = L.next;
    *slots = L.slots;
    return 1;
}
static void exec_draw(const gpu_fop_draw* d, uint32_t slot) {
    if (PR.on > 0) {
        int ff = X.mixed && d->tier == GPU_TIER_FF && !X.ref;
        int zo = d->zt && (d->depth >> 1) != 0;
        prof_mark(d->tier == GPU_TIER_ALIAS16 ? PR_A16_DRAW
                  : d->cls == GPU_PC_TRI ? (!ff ? PR_REF_TRI : (d->feat & GPU_DF_PART) ? PR_PART : (d->ps_key & GPU_PSK_FF_FAST) ? (zo ? PR_FF_FAST_Z : PR_FF_FAST) : PR_FF_TRI)
                  : !ff ? PR_REF_OTHER : d->cls == GPU_PC_SPRITE ? PR_FF_SPRITE : PR_FF_LINE);
    }
    if (d->tier == GPU_TIER_ALIAS16 && X.a16c_on) { exec_a16_copy(d); return; }   /* (perf-p2) a copy run */
    if ((d->feat & GPU_DF_NPOST) && X.mixed && !X.ref) {          /* (r2-m7) a native post pass */
        if (exec_npost(d)) return;
        XC.npost_rejected++;
    }
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

/* (r2-m1) a modern-renderer segment (gpu_front.h gpu_fop_r2, runtime/r2/r2_render.c) on the targets' RTV + DSV, as an
 * FF draw would use them: zpatch-newer depth tiles resolved into the DSV first, the written tiles DSV-newer after */
static uint64_t r2_segs, r2_draws_total, r2_skipped;
static void exec_r2(const gpu_front_frame_data* fd, const gpu_fop_r2* o) {
    xtarget* ct = tgt(o->ct);
    xtarget* zt = tgt(o->zt);
    if (!X.mixed || !ct || !zt) { r2_skipped++; return; }   /* (REF / benchmark frames draw no R2 segment) */
    prof_mark(PR_R2);
    ensure_dsv(zt, o->bbox[0], o->bbox[1], o->bbox[2], o->bbox[3]);
    bind_ff(o->ct, o->zt);
    if (X.b_t1) set_t1(NULL);
    if (X.b_t2) set_t2(NULL);
    if (!iota_ensure(o->draw_off / 32u + o->ndraw + 1)) return;   /* (r2-m2: draw ids = the records' fdata offsets / 32) */
    if (TD.on != 0) td_ensure();          /* (r2-m7b: its buffer exists before the segment's materials need it) */
    r2_exec_in in = { gpu_rt.dev, gpu_rt.ctx, ct->rtv, zt->dsv, ct->kx, ct->ky, X.vram_srv, X.swz_srv, X.dat.srv, X.iota,
                      gpu_rt.caps.warp ? 0.49609375f : 0.42f, td_r2, NULL, NULL };
    in.texdec_srv = TD.srv;
    int n = r2_render_segment(o, fd->data, fd->data_bytes, &in);
    gpu_rt.n_draws += (uint32_t)n;
    r2_segs++;
    r2_draws_total += (uint64_t)n;
    if (!(o->flags & GPU_R2F_ZMSK)) zt_mark(zt, o->bbox[0], o->bbox[1], o->bbox[2], o->bbox[3], ZT_DSV);
    bind_frame_buffers();                /* the exact path's own IA, views, rasterizer state; cached bindings forgotten */
    X.mode = 0;
    X.b_ct = X.b_zt = -1;
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
    /* (perf-p2) a copy run: its draws write the target directly (exec_a16_copy), nothing to gather or scatter */
    int copy = o->h.size >= sizeof *o && o->mode == 1;
    if (!end) { X.a16c_on = copy; X.a16c_target = o->target; X.a16c_psm = o->psm; }
    else X.a16c_on = 0;
    if (copy) { if (end) { XC.alias16_runs++; XC.a16_copy_runs++; } return; }
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
    /* (M9) a thread per sample, the target as R32_UINT */
    mem_dispatch(end ? GPU_SH_cs_a16_scatter : GPU_SH_cs_a16_gather, &k, (wx * (uint32_t)t->kx + 7) / 8,
                 ((uint32_t)(o->y1 - o->y0) * (uint32_t)t->ky + 7) / 8, NULL, X.dat.srv, X.a16_uav, t->uav32, NULL);
    if (end) XC.alias16_runs++;
}

static void exec_clut(const gpu_front_frame_data* fd, const gpu_fop_clut* o) {
    uint32_t slot = o->slot & 4095;
    /* (perf-p2) 2 KB per slot: word p = halfword p | halfword (p ^ 256) << 16 (gs.hlsl clut_word: a CT32 entry in one load) */
    uint16_t h[512];
    uint32_t wd[512];
    memcpy(h, fd->data + o->data_off, sizeof h);
    for (int p = 0; p < 512; p++) wd[p] = (uint32_t)h[p] | (uint32_t)h[p ^ 256] << 16;
    D3D11_BOX box = { slot * 2048u, 0, 0, slot * 2048u + 2048u, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)X.clut, 0, &box, wd, 0, 0);
    gpu_rt.n_uploads++;
    gpu_rt.upload_bytes += 2048;
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
    mem_note("exec init start");
    memset(&X, 0, sizeof X);
    X.vram = raw_default(4u << 20, NULL, &X.vram_srv, &X.vram_uav);
    X.scratch = raw_default(4u << 20, NULL, &X.scratch_srv, &X.scratch_uav);
    X.clut = raw_default(4096u * 2048u, NULL, &X.clut_srv, NULL);   /* (perf-p2: 2 KB per slot, exec_clut) */
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
        { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },   /* (pc-research: vertex Z, early depth bound) */
        /* (M9) the draw's slot: element StartInstanceLocation + instance of the iota buffer (the vertex shaders of the
         * instanced classes - sprites, line pixels - subtract SV_InstanceID; a larger step rate was not divided the
         * documented way on the RTX 4060 Ti) */
        { "DRAWID", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    const gpu_shader_blob* vb = &gpu_shaders[GPU_SH_vs_ff_tri];   /* (the input signature with DRAWID) */
    if (FAILED(ID3D11Device_CreateInputLayout(gpu_rt.dev, el, 6, vb->code, vb->size, &X.layout))) { gpu_log("input layout failed"); return 0; }
    if (!iota_ensure(65536)) return 0;
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS fo;
        memset(&fo, 0, sizeof fo);
        X.clearview = gpu_rt.ctx1 && SUCCEEDED(ID3D11Device_CheckFeatureSupport(gpu_rt.dev, D3D11_FEATURE_D3D11_OPTIONS, &fo, sizeof fo)) &&
                      fo.ClearView;
    }
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
    /* the per-frame buffers start small (M9: they grow to what a frame needs and shrink after peaks, dyn_fill) */
    if (!dyn_ensure(&X.vb, 1u << 20, D3D11_BIND_VERTEX_BUFFER, 0) || !dyn_ensure(&X.ib, 1u << 20, D3D11_BIND_INDEX_BUFFER, 0) ||
        !dyn_ensure(&X.rec, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1) || !dyn_ensure(&X.st, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1) ||
        !dyn_ensure(&X.dc, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1) || !dyn_ensure(&X.dat, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1) ||
        !dyn_ensure(&X.aux, 1u << 20, D3D11_BIND_SHADER_RESOURCE, 1))
        return 0;
    mem_note("exec buffers");
    /* every shader object up front (no creation inside a measured frame) */
    for (int i = 0; i < GPU_SH_COUNT; i++) {
        char k = gpu_shaders[i].profile[0];
        const char* nm = gpu_shaders[i].name;
        size_t nl = strlen(nm);
        /* (pc-portable) without rasterizer-ordered views the ROV shaders cannot be created (E_INVALIDARG) and are never
         * used (gpu_no_rov: the _nr variants): skip them instead of logging a failure for each */
        if (!gpu_rt.caps.rovs && (!strncmp(nm, "ps_rov_t", 8) ||
                                  (!strncmp(nm, "ps_gs_", 6) && !(nl > 3 && !strcmp(nm + nl - 3, "_nr")))))
            continue;
        PROCESS_MEMORY_COUNTERS_EX pm0, pm1;
        int dbg = getenv("PS2_GS_MEM_MAP") != NULL;
        if (dbg) { memset(&pm0, 0, sizeof pm0); pm0.cb = sizeof pm0; K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm0, sizeof pm0); }
        if (k == 'v') gpu_vs(i); else if (k == 'p') gpu_ps(i); else if (k == 'c') gpu_cs(i);
        if (dbg) {
            memset(&pm1, 0, sizeof pm1); pm1.cb = sizeof pm1; K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm1, sizeof pm1);
            gpu_log("shader %s (%u bytes): +%.2f MB", gpu_shaders[i].name, gpu_shaders[i].size, ((double)pm1.PrivateUsage - (double)pm0.PrivateUsage) / 1048576.0);
        }
    }
    mem_note("exec shaders");
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
            gpu_log("probe v%u%s %s rec %u state %u: U %d V %d colour %08X (vertex/record %08X) z %08X s %.9g t %.9g q %.9g xl %d [s %08X t %08X q %08X] draw slot %u",
                    fd->vblank, fd->flags & GPU_FD_PARTIAL ? "p" : "", kinds[k], r[1], r[2], (int)r[3], (int)r[4], r[5], r[6], r[7],
                    s, t, q, (int)r[11], r[8], r[9], r[10], r[12]);
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

static void fence_release(void);
void gpu_exec_release(void) {
    if (!X.init) return;
    r2_render_release();                 /* (r2-m1: meshes upload again on the recreated device) */
    fence_release();                     /* (r2-m2: the GPU frame fence's queries) */
    if (dbg_nosize || dbg_nocount) gpu_log("exec: %llu draws without a target, %llu empty draws (cls %llu %llu %llu %llu, tier %llu %llu)", (unsigned long long)dbg_nosize, (unsigned long long)dbg_nocount, (unsigned long long)dbg_cls[0], (unsigned long long)dbg_cls[1], (unsigned long long)dbg_cls[2], (unsigned long long)dbg_cls[3], (unsigned long long)dbg_tier[0], (unsigned long long)dbg_tier[1]);
    for (int i = 0; i <= MAX_TARGETS; i++) { target_free(&X.T[i]); free(X.T[i].zts); X.T[i].zts = NULL; }
    GPU_RELEASE(X.ztile_srv); GPU_RELEASE(X.ztile); GPU_RELEASE(X.zcb);
    for (int i = 0; i < PR_MAXQ; i++) { GPU_RELEASE(PR.q[i]); GPU_RELEASE(PR.sq[i]); GPU_RELEASE(PR.oq[i]); }
    GPU_RELEASE(PR.dis);
    dynbuf* d[8] = { &X.vb, &X.ib, &X.rec, &X.st, &X.cb, &X.dat, &X.aux, &X.dc };
    for (int i = 0; i < 8; i++) { GPU_RELEASE(d[i]->srv); GPU_RELEASE(d[i]->buf); d[i]->cap = 0; }
    GPU_RELEASE(X.iota);
    X.iota_n = 0;
    GPU_RELEASE(X.vram_srv); GPU_RELEASE(X.vram_uav); GPU_RELEASE(X.vram); GPU_RELEASE(X.scratch_uav); GPU_RELEASE(X.scratch_srv);
    GPU_RELEASE(X.scratch);
    GPU_RELEASE(X.stage); GPU_RELEASE(X.swz_srv); GPU_RELEASE(X.swz); GPU_RELEASE(X.clut_srv); GPU_RELEASE(X.clut);
    GPU_RELEASE(X.dbg_uav); GPU_RELEASE(X.dbg); GPU_RELEASE(X.dbg_stage); GPU_RELEASE(X.a16_uav); GPU_RELEASE(X.a16);
    GPU_RELEASE(X.a16cb);
    GPU_RELEASE(X.npcb);
    td_release();
    GPU_RELEASE(X.svcnt_uav); GPU_RELEASE(X.svcnt);
    X.svcnt_w = X.svcnt_h = 0;
    X.sv_on = 0;
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
    in.scale_x = o->h.size >= sizeof *o && o->scale_x ? o->scale_x : in.scale;   /* (M10: anamorphic widescreen) */
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
    /* (pc-pacing) for the paced present and the internal scale governor: the picture's VBlank time, its scale, whether a
     * newer frame already waits behind this one */
    gpu_rt.cur_vblank_ms = di->vblank_ms;
    gpu_rt.cur_behind = atomic_load(&inflight) > 1 + gpu_present_pace_frames();   /* (more queued than the pacing holds back) */
    gpu_stats_cur()->scale = in.scale;
    /* (the picture was reported when its frame was queued: gpu_submit_sink, GPU_FD_SIGNAL) */
    if (di->flags & GPU_DI_SHOW) gpu_present_show(di->aspect_w, di->aspect_h, (int)((di->gfx >> GPU_GFX_SCALER_SHIFT) & 15));
    present_done = 1;
}

void gpu_exec_reset(void) {
    if (!gpu_running()) return;
    gpu_run_sync((void (*)(void*))gpu_exec_release, NULL);   /* (exec_init recreates everything on the next frame) */
}

/* (r2-m2) GPU frame fence: before a frame's work is recorded, the GPU has finished the frame `fence_depth` frames back (an
 * event query per frame). Without it, a GPU-bound game (the modern renderer makes the CPU stages fast enough for that at 3x)
 * let the driver queue several frames, then block the render thread for 10-50 ms inside whichever call hit its limit
 * (the data arena's Map, a constant buffer update): 30-50 ms hitches every ~0.2 s, latency up to 65 ms. With the fence
 * the wait happens here, one GPU frame at a time. PS2_GPU_FENCE=n (frames, default 2; 0 off). */
#define FENCE_N 4
static ID3D11Query* fence_q[FENCE_N];
static uint32_t fence_i;
static int fence_depth = -1;
static uint64_t fence_waits;
static double fence_wait_ms;

static void fence_wait(void) {
    if (fence_depth < 0) {
        const char* e = getenv("PS2_GPU_FENCE");
        fence_depth = e && *e ? atoi(e) : 2;
        if (fence_depth < 0) fence_depth = 0;
        if (fence_depth > FENCE_N - 1) fence_depth = FENCE_N - 1;
    }
    if (!fence_depth || fence_i < (uint32_t)fence_depth) return;
    ID3D11Query* q = fence_q[(fence_i - (uint32_t)fence_depth) % FENCE_N];
    if (!q) return;
    BOOL done = FALSE;
    if (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)q, &done, sizeof done, 0) != S_FALSE) return;
    double w0 = qpc_ms();
    while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)q, &done, sizeof done, 0) == S_FALSE) {
        if (qpc_ms() - w0 > 100.0) break;        /* (a device problem: never hold the renderer longer) */
        SwitchToThread();
    }
    fence_waits++;
    fence_wait_ms += qpc_ms() - w0;
}

static void fence_signal(void) {
    if (!fence_depth) return;
    ID3D11Query** q = &fence_q[fence_i % FENCE_N];
    if (!*q) {
        D3D11_QUERY_DESC d = { D3D11_QUERY_EVENT, 0 };
        if (FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &d, q))) { *q = NULL; return; }
    }
    ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)*q);
    fence_i++;
}

static void fence_release(void) {
    for (int i = 0; i < FENCE_N; i++) GPU_RELEASE(fence_q[i]);
    fence_i = 0;
}

void gpu_exec_frame(const void* payload) {
    const gpu_op_frame* p = payload;
    const gpu_front_frame_data* fd = &p->fd;
    if (!exec_init()) { gpu_rt.lost = 1; return; }
    fence_wait();
    thread_ms();
    gpu_stats_begin(fd->vblank, 0);
    gpu_rt.cur_queued_ms = p->t_queued;  /* (M9: photon latency, gpu_present.c) */
    gpu_stats_cur()->queued = atomic_load(&inflight);
    double t0 = qpc_ms(), c0 = thread_ms();
    X.state_calls = 0;
    uint64_t ff0 = XC.ff_draws, rov0 = XC.rov_draws;
    X.ref = (fd->flags & GPU_FD_REF) != 0;
    X.mixed = !X.ref && (fd->flags & GPU_FD_MIXED) != 0;
    XC.frames++;
    if (fd->flags & GPU_FD_PARTIAL) XC.partial_frames++;
    /* upload: vertices, indices, records, StateRecs, the data arena, per-draw constants (one Map each) */
    uint32_t ncb = build_constants(fd, X.ref || X.mixed);
    static int ab_init;
    if (!ab_init) {
        ab_init = 1; const char* e = getenv("PS2_GPU_AB"); PR.ab = e && atoi(e) > 0 ? atoi(e) : 0;   /* (r2-m7b) */
        e = getenv("PS2_GPU_AB_EE"); PR.ab_ee = e && *e == '1';   /* (r2-m5b) */
    }
    if (PR.ab > 0) {
        int nb = (int)((XC.frames / (uint32_t)PR.ab) & 1);
        if (nb != PR.ab_b) PR.ab_skip = 8;
        PR.ab_b = nb;
        gpu_ab_phase = nb;
    }
    const uint8_t* states = td_prepass(fd);   /* (r2-m7b) the decoded texture cache's StateRec fields */
    int ok = dyn_fill(&X.vb, fd->vtx, fd->vtx_bytes, D3D11_BIND_VERTEX_BUFFER, 0) &&
             dyn_fill(&X.ib, fd->idx, fd->idx_count * 4, D3D11_BIND_INDEX_BUFFER, 0) &&
             dyn_fill(&X.rec, fd->rec, fd->rec_bytes, D3D11_BIND_SHADER_RESOURCE, 1) &&
             dyn_fill(&X.st, states, fd->state_count * GPU_STATE_BYTES, D3D11_BIND_SHADER_RESOURCE, 1) &&
             dyn_fill(&X.dat, fd->data, fd->data_bytes, D3D11_BIND_SHADER_RESOURCE, 1) &&
             (X.ref || X.mixed ? dyn_fill(&X.dc, cb_cpu, (size_t)ncb * DRAW_CB_BYTES, D3D11_BIND_SHADER_RESOURCE, 1) && iota_ensure(bc_maxid)
                               : dyn_fill(&X.cb, cb_cpu, (size_t)ncb * DRAW_CB_BYTES, D3D11_BIND_CONSTANT_BUFFER, 0));
    double t1 = qpc_ms();
    if (!ok) gpu_log("frame %u: upload failed", fd->vblank);
    bind_frame_buffers();
    prof_begin();
    X.fd = fd;
    X.a16c_on = 0;
    uint32_t slot = 1;
    for (size_t pos = 0; ok && pos + sizeof(gpu_fop) <= fd->cmd_bytes;) {
        const gpu_fop* h = (const gpu_fop*)(fd->cmd + pos);
        if (!h->size) break;
        PR.curpos = (uint32_t)pos;
        X.curpos = (uint32_t)pos;
        if (h->op == GPU_FOP_ALIAS16) {     /* (r2-m7b) the shadow pipeline's tail as one pass */
            size_t nx;
            uint32_t ns;
            if (shfuse_try(fd, pos, &nx, &ns)) { slot += ns; pos = nx; continue; }
        }
        /* (r2-m7) pending shadow-volume counters go into G before any other op (another volume draw on the same target,
         * the end of its ROV run, CLUT and memory patches do not touch the target) */
        if (X.sv_on && h->op != GPU_FOP_ROV_END && h->op != GPU_FOP_CLUT && h->op != GPU_FOP_PATCH &&
            !(h->op == GPU_FOP_DRAW && (((const gpu_fop_draw*)h)->feat & GPU_DF_SVOL) && ((const gpu_fop_draw*)h)->ct == X.sv_ct))
            svol_resolve();
        if (X.probe && h->op != GPU_FOP_DRAW && h->op != GPU_FOP_CLUT && h->op != GPU_FOP_PATCH)   /* (debugging: the memory ops) */
            gpu_log("exec v%u%s op %u target %u", fd->vblank, fd->flags & GPU_FD_PARTIAL ? "p" : "", h->op, ((const uint16_t*)h)[2]);
        if (PR.on > 0 && h->op != GPU_FOP_DRAW &&
            !(PR.ops_on && (h->op == GPU_FOP_CLUT || h->op == GPU_FOP_PATCH) && PR.cur == PR_MEM))
            prof_mark(h->op == GPU_FOP_TARGET || h->op == GPU_FOP_RELEASE ? PR_TARGET
                      : h->op == GPU_FOP_PATCH || h->op == GPU_FOP_LOAD || h->op == GPU_FOP_XCOPY || h->op == GPU_FOP_DOWNLOAD ||
                        h->op == GPU_FOP_CLUT ? PR_MEM
                      : h->op == GPU_FOP_CLEAR ? PR_CLEAR : h->op == GPU_FOP_COPY_SHADOW ? PR_SHADOW
                      : h->op == GPU_FOP_ALIAS16 || h->op == GPU_FOP_ALIAS16_END ? PR_A16
                      : h->op == GPU_FOP_PRESENT ? PR_PRESENT : PR_OTHER);
        if (TD.jnext < TD.njob) td_run((uint32_t)pos);   /* (r2-m7b) decodes before the draw that needs them */
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
        case GPU_FOP_R2: exec_r2(fd, (const gpu_fop_r2*)h); break;
        default: break;
        }
        pos += h->size;
    }
    svol_resolve();                      /* (r2-m7) */
    SF.pend = 0;                         /* (r2-m7b: a head waits only within its frame) */
    unbind_all();                        /* (targets may be released or recreated before the next frame) */
    prof_end();
    probe_report(fd);
    fence_signal();
    ID3D11DeviceContext_Flush(gpu_rt.ctx);
    double t2 = qpc_ms(), c1 = thread_ms();
    gpu_frame_stats* st = gpu_stats_cur();
    st->render_cpu_ms = c1 - c0;
    st->upload_ms = t1 - t0;
    st->state_calls = X.state_calls;
    st->partial = (fd->flags & GPU_FD_PARTIAL) != 0;
    st->ff_draws = (uint32_t)(XC.ff_draws - ff0);
    st->rov_draws = (uint32_t)(XC.rov_draws - rov0);
    st->latency_ms = st->presented && p->t_queued > 0 ? qpc_ms() - p->t_queued : -1.0;
    (void)t2;
    gpu_stats_end();
    if (XC.frames == 1 || XC.frames == 3 || XC.frames == 10 || XC.frames == 60 || XC.frames == 600) {
        char w[48];
        snprintf(w, sizeof w, "after frame %u", (unsigned)XC.frames);
        mem_note(w);
        if (getenv("PS2_GS_MEM_MAP"))
            gpu_log("upload buffers (MB): vb %.1f ib %.1f rec %.1f st %.1f dc %.1f dat %.1f; shrinks %llu", X.vb.cap / 1048576.0,
                    X.ib.cap / 1048576.0, X.rec.cap / 1048576.0, X.st.cap / 1048576.0, X.dc.cap / 1048576.0, X.dat.cap / 1048576.0,
                    (unsigned long long)XC.dyn_shrinks);
    }
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

/* (M7) every sample of a target as the render thread has it after everything submitted (tests: FF vs REF at
 * scale): colour words (R in the low byte) or zpatch words */
typedef struct { uint32_t id; uint32_t* out; size_t cap; int w, h, kx, ky, depth, ok; } readtgt_args;

static void readtgt_fn(void* arg) {
    readtgt_args* a = arg;
    a->ok = 0;
    if (!X.init) return;
    xtarget* t = tgt(a->id);
    if (!t || !t->tex) return;
    unbind_all();
    if (t->depth) ensure_zp_all(t);
    ID3D11Texture2D* src = t->depth ? t->zp : t->tex;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(src, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(gpu_rt.ctx, (ID3D11Resource*)st, (ID3D11Resource*)src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
        int w = (int)td.Width, h = (int)td.Height;
        if ((size_t)w * (size_t)h <= a->cap) {
            for (int y = 0; y < h; y++) memcpy(a->out + (size_t)y * (size_t)w, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
            a->w = w; a->h = h; a->kx = t->kx; a->ky = t->ky; a->depth = t->depth;
            a->ok = 1;
        }
        ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)st, 0);
    }
    GPU_RELEASE(st);
}

int gpu_exec_read_target(uint32_t id, uint32_t* out, size_t cap_words, int* w, int* h, int* kx, int* ky, int* depth) {
    readtgt_args a = { id, out, cap_words, 0, 0, 0, 0, 0, 0 };
    gpu_run_sync(readtgt_fn, &a);
    *w = a.w; *h = a.h; *kx = a.kx; *ky = a.ky; *depth = a.depth;
    return a.ok;
}

/* ---- producer side (GS thread): the frontend's sink -------------------------------------------------- */

extern int frame_rate_high(void) __attribute__((weak));   /* (pc-fps) runtime/frame_rate.c */

void gpu_submit_sink(const gpu_front_frame_data* fd, void* user) {
    (void)user;
    if (!gpu_running()) {                /* (no render thread: the picture is still reported) */
        if (fd->flags & GPU_FD_SIGNAL) gpu_present_signal(fd->vblank);
        return;
    }
    /* at most MAX_INFLIGHT frames queued (the render thread's pace bounds the GS thread's, as VBlank
     * pacing does in the game). (M9) Adaptive (gpu_submit_set_adaptive): inflight_base frames normally; a slowdown that
     * fills them after the queue had been short (<= 1 frame) for ADAPT_MS - NVIDIA lowering the clocks of a lightly
     * loaded GPU for a few frames - may queue up to inflight_max for ADAPT_MS. A GPU that is behind for good never
     * lets the queue drain, so it stays at inflight_base frames of latency (the EE then waits: the game slows down as
     * it would with any renderer too slow for it). */
    /* (pc-pacing) the paced present holds back up to `extra` frames on purpose (a picture waits for its time while the
     * next ones arrive): the queue is that much deeper, so the game never waits for the pacing */
    int extra = gpu_present_pace_frames();
    int limit = inflight_max + extra;
    /* (pc-fps) above 60 fps the frame rate is what the GPU can draw: two frames queued at most (low latency; the game's speed
     * does not depend on it), no absorbing - a GPU slower than the frame limiter is not a passing slowdown */
    int high = frame_rate_high && frame_rate_high();
    if (high) limit = 2 + extra;
    else if (inflight_base > 0 && inflight_base < inflight_max) {
        int n = atomic_load(&inflight) - extra;
        double now = qpc_ms();
        int calm = q_short_since >= 0 && now - q_short_since >= ADAPT_MS;
        if (n <= 1) { if (q_short_since < 0) q_short_since = now; }
        else q_short_since = -1;
        limit = inflight_base + extra;
        if (now < absorb_until) limit = inflight_max + extra;
        else if (n >= inflight_base && calm) { absorb_until = now + ADAPT_MS; limit = inflight_max + extra; queue_absorbs++; }
    }
    double w0 = -1;
    for (;;) {
        int n = atomic_load(&inflight);
        if (n < limit) break;
        if (w0 < 0) w0 = qpc_ms();
        WaitOnAddress((volatile VOID*)&inflight, &n, sizeof n, 5);
    }
    if (w0 >= 0) stall_ms += qpc_ms() - w0;   /* (pc-pacing: what the game lost to the GPU, PS2_GS_HW_STATS) */
    void* h = gpu_front_detach();
    if (!h) return;
    atomic_fetch_add(&inflight, 1);
    gpu_cmd_lock();
    gpu_op_frame* op = gpu_cmd_alloc(GPU_OP_FRAME, sizeof *op);
    op->fd = *fd;
    op->handle = h;
    op->t_queued = qpc_ms();
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
