/* Pictures and presentation (notes/d3d11-renderer.md 8.2), M0 form: the software renderer's picture
 * is uploaded, composed into a GPU picture (picture[3], R8G8B8A8_UNORM at native size) and
 * presented, either through a flip-model swap chain on the window (FLIP_DISCARD, 3 buffers, frame
 * latency 1 with the waitable object, ALLOW_TEARING when supported and asked for) or, headless,
 * into an offscreen target of the window's size, so the same passes run in tests.
 *
 * Read-backs copy the composed picture into a ring of staging textures and complete
 * asynchronously (Map with DO_NOT_WAIT while other commands arrive). GPU timestamps around each
 * picture are resolved the same way. Producer-side calls are at the end of the file. */
#include "gpu_internal.h"

#include <stdatomic.h>
#include <string.h>

#include "gpu_shaders.h"

#define NPICT     3
#define NSTAGE    4
#define NTS       8
#define NSTATS    4096

static struct {
    ID3D11Texture2D* up;                 /* uploaded sw picture (raw BGRX bytes as R8G8B8A8) */
    ID3D11ShaderResourceView* up_srv;
    int up_w, up_h;
    ID3D11Texture2D* pict[NPICT];
    ID3D11RenderTargetView* pict_rtv[NPICT];
    ID3D11ShaderResourceView* pict_srv[NPICT];
    int pict_w, pict_h;                  /* allocated size */
    int cur, cur_w, cur_h, have;         /* last composed picture */
    uint32_t cur_vblank;
    ID3D11Texture2D* stage[NSTAGE];
    int stage_w, stage_h;
    struct { int slot, w, h; uint32_t vblank; gpu_readback* rb; } pend[NSTAGE];
    int npend, pend_head;
    ID3D11SamplerState* linear;
    ID3D11Buffer* cb;                    /* present: uv scale and clamp */
    /* present target */
    IDXGISwapChain1* sc;
    HANDLE waitable;
    UINT sc_flags;
    int bb_w, bb_h;
    ID3D11Texture2D* off;                /* headless */
    ID3D11RenderTargetView* rtv;         /* current back buffer / offscreen */
    /* timestamps */
    struct { ID3D11Query *dis, *t0, *t1; gpu_frame_stats st; int busy; } ts[NTS];
    int ts_head, ts_n;
    gpu_frame_stats st;                  /* picture being built */
    LARGE_INTEGER t_start;
    int ts_cur;                          /* ts slot of the picture being built (-1: none) */
} P;

static LARGE_INTEGER qpf;

/* completed stats, handed to the producer */
static SRWLOCK stats_lock = SRWLOCK_INIT;
static gpu_frame_stats stats_ring[NSTATS];
static int stats_head, stats_n;

static void stats_push(const gpu_frame_stats* s) {
    AcquireSRWLockExclusive(&stats_lock);
    if (stats_n == NSTATS) { stats_head = (stats_head + 1) % NSTATS; stats_n--; }
    stats_ring[(stats_head + stats_n) % NSTATS] = *s;
    stats_n++;
    ReleaseSRWLockExclusive(&stats_lock);
}

int gpu_take_frame_stats(gpu_frame_stats* out, int max) {
    AcquireSRWLockExclusive(&stats_lock);
    int n = stats_n < max ? stats_n : max;
    for (int i = 0; i < n; i++) out[i] = stats_ring[(stats_head + i) % NSTATS];
    stats_head = (stats_head + n) % NSTATS;
    stats_n -= n;
    ReleaseSRWLockExclusive(&stats_lock);
    return n;
}

/* ---- resources ------------------------------------------------------------------------------- */

static ID3D11Texture2D* tex_create(int w, int h, UINT bind, D3D11_USAGE usage, UINT cpu) {
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = usage;
    td.BindFlags = bind;
    td.CPUAccessFlags = cpu;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return NULL;
    return t;
}

static int ensure_upload(int w, int h) {
    if (P.up && P.up_w >= w && P.up_h >= h) return 1;
    GPU_RELEASE(P.up_srv);
    GPU_RELEASE(P.up);
    int nw = w > P.up_w ? w : P.up_w, nh = h > P.up_h ? h : P.up_h;
    P.up = tex_create(nw, nh, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
    if (!P.up || FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)P.up, NULL, &P.up_srv))) {
        GPU_RELEASE(P.up);
        return 0;
    }
    P.up_w = nw;
    P.up_h = nh;
    return 1;
}

static void pend_complete(int block);

static int ensure_pictures(int w, int h) {
    if (P.pict[0] && P.pict_w >= w && P.pict_h >= h) return 1;
    while (P.npend) pend_complete(1);           /* staging copies read the old pictures */
    int nw = w > P.pict_w ? w : P.pict_w, nh = h > P.pict_h ? h : P.pict_h;
    for (int i = 0; i < NPICT; i++) {
        GPU_RELEASE(P.pict_srv[i]); GPU_RELEASE(P.pict_rtv[i]); GPU_RELEASE(P.pict[i]);
        P.pict[i] = tex_create(nw, nh, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        if (!P.pict[i] ||
            FAILED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)P.pict[i], NULL, &P.pict_rtv[i])) ||
            FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)P.pict[i], NULL, &P.pict_srv[i])))
            return 0;
    }
    for (int i = 0; i < NSTAGE; i++) {
        GPU_RELEASE(P.stage[i]);
        P.stage[i] = tex_create(nw, nh, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        if (!P.stage[i]) return 0;
    }
    P.pict_w = P.stage_w = nw;
    P.pict_h = P.stage_h = nh;
    P.have = 0;
    return 1;
}

static int make_rtv(void) {
    GPU_RELEASE(P.rtv);
    if (P.sc) {
        ID3D11Texture2D* bb = NULL;
        HRESULT hr = IDXGISwapChain1_GetBuffer(P.sc, 0, &gpu_IID_ID3D11Texture2D, (void**)&bb);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)bb, NULL, &P.rtv);
        GPU_RELEASE(bb);
        return SUCCEEDED(hr);
    }
    return SUCCEEDED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)P.off, NULL, &P.rtv));
}

static int swapchain_create(HWND hwnd) {
    IDXGIDevice* dxdev = NULL;
    IDXGIAdapter* ad = NULL;
    IDXGIFactory2* f2 = NULL;
    IDXGIFactory5* f5 = NULL;
    HRESULT hr = ID3D11Device_QueryInterface(gpu_rt.dev, &gpu_IID_IDXGIDevice, (void**)&dxdev);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxdev, &ad);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &gpu_IID_IDXGIFactory2, (void**)&f2);
    if (FAILED(hr)) goto out;
    BOOL tearing = FALSE;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(f2, &gpu_IID_IDXGIFactory5, (void**)&f5)) &&
        FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)))
        tearing = FALSE;
    gpu_rt.caps.tearing = tearing != FALSE;
    DXGI_SWAP_CHAIN_DESC1 d;
    memset(&d, 0, sizeof d);
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)gpu_rt.dev, hwnd, &d, NULL, NULL, &P.sc);
    if (FAILED(hr)) {
        gpu_log("swap chain: flip-discard with waitable object failed (hr=0x%08lX), trying plain", (unsigned long)hr);
        d.Flags = 0;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)gpu_rt.dev, hwnd, &d, NULL, NULL, &P.sc);
    }
    if (FAILED(hr)) goto out;
    P.sc_flags = d.Flags;
    IDXGIFactory2_MakeWindowAssociation(f2, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        IDXGISwapChain2* sc2 = NULL;
        if (SUCCEEDED(IDXGISwapChain1_QueryInterface(P.sc, &gpu_IID_IDXGISwapChain2, (void**)&sc2))) {
            IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
            P.waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
            GPU_RELEASE(sc2);
        }
    }
    gpu_rt.caps.swapchain = 1;
    gpu_rt.caps.waitable = P.waitable != NULL;
    gpu_rt.caps.tearing = gpu_rt.caps.tearing && (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
out:
    GPU_RELEASE(f5); GPU_RELEASE(f2); GPU_RELEASE(ad); GPU_RELEASE(dxdev);
    return P.sc != NULL;
}

int gpu_present_init(void) {
    QueryPerformanceFrequency(&qpf);
    memset(&P, 0, sizeof P);
    P.ts_cur = -1;
    AcquireSRWLockExclusive(&stats_lock);
    stats_head = stats_n = 0;
    ReleaseSRWLockExclusive(&stats_lock);
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(gpu_rt.dev, &sd, &P.linear))) return 0;
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &P.cb))) return 0;
    if (gpu_rt.cfg.hwnd) {
        if (!swapchain_create((HWND)gpu_rt.cfg.hwnd)) { gpu_log("cannot create the swap chain"); return 0; }
        P.bb_w = P.bb_h = -1;            /* sized at the first present */
    } else {
        int w = gpu_rt.cfg.present_w > 0 ? gpu_rt.cfg.present_w : 1280;
        int h = gpu_rt.cfg.present_h > 0 ? gpu_rt.cfg.present_h : 960;
        P.off = tex_create(w, h, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        if (!P.off || !make_rtv()) return 0;
        P.bb_w = w;
        P.bb_h = h;
    }
    if (gpu_rt.cfg.timestamps) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
        for (int i = 0; i < NTS; i++)
            if (FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qd, &P.ts[i].dis)) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &P.ts[i].t0)) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &P.ts[i].t1)))
                return 0;
    }
    return 1;
}

void gpu_present_release(void) {
    if (!gpu_rt.dev) return;
    for (int i = 0; i < NTS; i++) { GPU_RELEASE(P.ts[i].dis); GPU_RELEASE(P.ts[i].t0); GPU_RELEASE(P.ts[i].t1); }
    for (int i = 0; i < NSTAGE; i++) GPU_RELEASE(P.stage[i]);
    for (int i = 0; i < NPICT; i++) { GPU_RELEASE(P.pict_srv[i]); GPU_RELEASE(P.pict_rtv[i]); GPU_RELEASE(P.pict[i]); }
    GPU_RELEASE(P.up_srv); GPU_RELEASE(P.up); GPU_RELEASE(P.linear); GPU_RELEASE(P.cb);
    GPU_RELEASE(P.rtv); GPU_RELEASE(P.off);
    if (P.waitable) CloseHandle(P.waitable);
    P.waitable = NULL;
    GPU_RELEASE(P.sc);
}

/* ---- timestamps and statistics ----------------------------------------------------------------- */

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart;
}

static int ts_complete(int block) {             /* oldest timestamp set; returns 1 if still pending */
    if (!P.ts_n) return 0;
    int i = P.ts_head;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT flags = block ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
    HRESULT hr;
    while ((hr = ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].dis, &dj, sizeof dj, flags)) == S_FALSE) {
        if (!block) return 1;
        SwitchToThread();
    }
    UINT64 t0 = 0, t1 = 0;
    if (hr == S_OK) {
        while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t0, &t0, sizeof t0, 0) == S_FALSE) SwitchToThread();
        while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t1, &t1, sizeof t1, 0) == S_FALSE) SwitchToThread();
    }
    gpu_frame_stats st = P.ts[i].st;
    st.gpu_ms = hr == S_OK && !dj.Disjoint && dj.Frequency ? (double)(t1 - t0) * 1000.0 / (double)dj.Frequency : -1.0;
    stats_push(&st);
    P.ts[i].busy = 0;
    P.ts_head = (P.ts_head + 1) % NTS;
    P.ts_n--;
    return 0;
}

static void frame_begin(uint32_t vblank, double front_ms) {
    QueryPerformanceCounter(&P.t_start);
    memset(&P.st, 0, sizeof P.st);
    P.st.vblank = vblank;
    P.st.front_ms = front_ms;
    P.st.gpu_ms = -1.0;
    gpu_rt.n_draws = gpu_rt.n_dispatches = gpu_rt.n_copies = gpu_rt.n_uploads = gpu_rt.upload_bytes = 0;
    P.ts_cur = -1;
    if (gpu_rt.cfg.timestamps) {
        if (P.ts_n == NTS) ts_complete(1);
        int i = (P.ts_head + P.ts_n) % NTS;
        ID3D11DeviceContext_Begin(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].dis);
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t0);
        P.ts_cur = i;
    }
}

static void frame_end(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    P.st.draws = gpu_rt.n_draws;
    P.st.dispatches = gpu_rt.n_dispatches;
    P.st.copies = gpu_rt.n_copies;
    P.st.uploads = gpu_rt.n_uploads;
    P.st.upload_bytes = gpu_rt.upload_bytes;
    P.st.render_ms = (double)(t.QuadPart - P.t_start.QuadPart) * 1000.0 / (double)qpf.QuadPart;
    if (P.ts_cur >= 0) {
        int i = P.ts_cur;
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t1);
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].dis);
        P.ts[i].st = P.st;
        P.ts[i].busy = 1;
        P.ts_n++;
        P.ts_cur = -1;
    } else {
        stats_push(&P.st);
    }
}

/* ---- picture: upload + compose ----------------------------------------------------------------- */

static void fullscreen_pass(ID3D11RenderTargetView* rtv, int vx, int vy, int vw, int vh, int ps,
                            ID3D11ShaderResourceView* srv) {
    D3D11_VIEWPORT vp = { (FLOAT)vx, (FLOAT)vy, (FLOAT)vw, (FLOAT)vh, 0.0f, 1.0f };
    ID3D11DeviceContext* c = gpu_rt.ctx;
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(c, NULL, 0);
    ID3D11DeviceContext_RSSetState(c, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, gpu_vs(GPU_SH_vs_fullscreen), NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, gpu_ps(ps), NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &P.linear);
    ID3D11DeviceContext_Draw(c, 3, 0);
    gpu_rt.n_draws++;
    ID3D11ShaderResourceView* none = NULL;      /* unbind: the source may be a render target next */
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &none);
}

void gpu_exec_picture(const void* payload) {
    const gpu_op_picture* p = payload;
    frame_begin(p->vblank, p->front_ms);
    int w = p->px ? p->w : 640, h = p->px ? p->h : 448;
    if (!ensure_pictures(w, h) || (p->px && !ensure_upload(w, h))) {
        gpu_log("cannot allocate %dx%d pictures", w, h);
        gpu_rt.lost = 1;
        return;
    }
    int next = (P.cur + 1) % NPICT;
    if (p->px) {
        D3D11_BOX box = { 0, 0, 0, (UINT)w, (UINT)h, 1 };
        ID3D11DeviceContext_UpdateSubresource(gpu_rt.ctx, (ID3D11Resource*)P.up, 0, &box, p->px, (UINT)p->stride * 4u, 0);
        gpu_rt.n_uploads++;
        gpu_rt.upload_bytes += (uint32_t)(w * h * 4);
        fullscreen_pass(P.pict_rtv[next], 0, 0, w, h, GPU_SH_ps_compose, P.up_srv);
    } else {                                    /* display off: black */
        static const float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, P.pict_rtv[next], black);
    }
    P.cur = next;
    P.cur_w = w;
    P.cur_h = h;
    P.cur_vblank = p->vblank;
    P.have = 1;
}

/* ---- present ------------------------------------------------------------------------------------ */

/* 4:3 picture inside a w x h target, centred (the window.c rule) */
static void letterbox(int w, int h, int* x, int* y, int* lw, int* lh) {
    if ((int64_t)w * 3 > (int64_t)h * 4) { *lh = h; *lw = h * 4 / 3; }
    else { *lw = w; *lh = w * 3 / 4; }
    *x = (w - *lw) / 2;
    *y = (h - *lh) / 2;
}

void gpu_exec_present(const void* payload) {
    (void)payload;
    if (P.sc) {
        RECT rc;
        GetClientRect((HWND)gpu_rt.cfg.hwnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) { frame_end(); return; }       /* minimised */
        if (rc.right != P.bb_w || rc.bottom != P.bb_h) {
            ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 0, NULL, NULL);
            GPU_RELEASE(P.rtv);
            HRESULT hr = IDXGISwapChain1_ResizeBuffers(P.sc, 0, (UINT)rc.right, (UINT)rc.bottom, DXGI_FORMAT_UNKNOWN, P.sc_flags);
            if (FAILED(hr) || !make_rtv()) { gpu_log("swap chain resize failed (hr=0x%08lX)", (unsigned long)hr); P.bb_w = -1; frame_end(); return; }
            P.bb_w = rc.right;
            P.bb_h = rc.bottom;
        }
        if (P.waitable) WaitForSingleObjectEx(P.waitable, 100, TRUE);
    }
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, P.rtv, black);
    if (P.have) {
        int x, y, lw, lh;
        letterbox(P.bb_w, P.bb_h, &x, &y, &lw, &lh);
        /* the picture occupies [0, cur_w) x [0, cur_h) of a possibly larger texture: uv scale and
           clamp (half a texel inside, so bilinear taps never reach past the picture) */
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)P.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* f = m.pData;
            f[0] = (float)P.cur_w / (float)P.pict_w;
            f[1] = (float)P.cur_h / (float)P.pict_h;
            f[2] = ((float)P.cur_w - 0.5f) / (float)P.pict_w;
            f[3] = ((float)P.cur_h - 0.5f) / (float)P.pict_h;
            ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)P.cb, 0);
        }
        ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 0, 1, &P.cb);
        fullscreen_pass(P.rtv, x, y, lw, lh, GPU_SH_ps_present, P.pict_srv[P.cur]);
    }
    if (P.sc) {
        int tear = gpu_rt.cfg.tearing && gpu_rt.caps.tearing;
        HRESULT hr = IDXGISwapChain1_Present(P.sc, tear ? 0 : 1, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            gpu_log("device lost (hr=0x%08lX, reason 0x%08lX)", (unsigned long)hr,
                    (unsigned long)ID3D11Device_GetDeviceRemovedReason(gpu_rt.dev));
            gpu_rt.lost = 1;
        }
    }
    frame_end();
}

/* ---- read-back ----------------------------------------------------------------------------------- */

static void rb_finish(gpu_readback* rb, int status) {
    rb->status = status;
    InterlockedExchange(&rb->done, 1);
    WakeByAddressAll((PVOID)&rb->done);
}

static void pend_complete(int block) {
    if (!P.npend) return;
    int i = P.pend_head;
    ID3D11Texture2D* st = P.stage[P.pend[i].slot];
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ,
                                         block ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    gpu_readback* rb = P.pend[i].rb;
    int w = P.pend[i].w, h = P.pend[i].h, status = 0;
    if (FAILED(hr)) status = -1;
    else {
        if (rb->cap_pixels >= w * h) {
            for (int y = 0; y < h; y++) {
                const uint8_t* s = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                uint32_t* d = rb->dst + (size_t)y * (size_t)w;
                for (int x = 0; x < w; x++, s += 4) d[x] = (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
            }
        } else status = -2;
        ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)st, 0);
    }
    rb->w = w;
    rb->h = h;
    rb->vblank = P.pend[i].vblank;
    P.pend_head = (P.pend_head + 1) % NSTAGE;
    P.npend--;
    rb_finish(rb, status);
}

void gpu_exec_readback(const void* payload) {
    gpu_readback* rb = ((const gpu_op_readback*)payload)->rb;
    if (gpu_rt.lost || !P.have) { rb_finish(rb, -1); return; }
    if (P.npend == NSTAGE) {
        ID3D11DeviceContext_Flush(gpu_rt.ctx);
        pend_complete(1);
    }
    int k = (P.pend_head + P.npend) % NSTAGE;
    P.pend[k].slot = k;
    P.pend[k].w = P.cur_w;
    P.pend[k].h = P.cur_h;
    P.pend[k].vblank = P.cur_vblank;
    P.pend[k].rb = rb;
    D3D11_BOX box = { 0, 0, 0, (UINT)P.cur_w, (UINT)P.cur_h, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)P.stage[k], 0, 0, 0, 0,
                                              (ID3D11Resource*)P.pict[P.cur], 0, &box);
    gpu_rt.n_copies++;
    P.npend++;
}

int gpu_present_poll(int block) {
    while (P.npend) {
        int before = P.npend;
        pend_complete(block);
        if (P.npend == before) break;           /* oldest still in flight */
        if (block) break;                       /* one per blocking call */
    }
    while (P.ts_n && !ts_complete(block)) if (block) break;
    return P.npend != 0;
}

void gpu_present_drain(void) {
    if (!gpu_rt.ctx) return;
    ID3D11DeviceContext_Flush(gpu_rt.ctx);
    while (P.npend) pend_complete(1);
    while (P.ts_n) ts_complete(1);
}

/* ---- producer side --------------------------------------------------------------------------------- */

void gpu_picture(const uint32_t* bgrx, int ok, int w, int h, int stride, uint32_t vblank) {
    if (!gpu_running()) return;
    double t0 = now_ms();
    gpu_op_picture pic;
    memset(&pic, 0, sizeof pic);
    pic.vblank = vblank;
    if (ok && bgrx && w > 0 && h > 0 && w <= stride) {
        uint32_t* d = gpu_data_alloc((size_t)w * (size_t)h * 4);
        if (d) {
            for (int y = 0; y < h; y++) memcpy(d + (size_t)y * (size_t)w, bgrx + (size_t)y * (size_t)stride, (size_t)w * 4);
            pic.px = d;
            pic.w = w;
            pic.h = h;
            pic.stride = w;
        }
    }
    pic.front_ms = now_ms() - t0;
    gpu_op_picture* p = gpu_cmd_alloc(GPU_OP_PICTURE, sizeof *p);
    *p = pic;
    gpu_cmd_commit();
    gpu_op_present* q = gpu_cmd_alloc(GPU_OP_PRESENT, sizeof *q);
    q->vblank = vblank;
    gpu_cmd_commit();
    gpu_cmd_publish();                          /* a VBlank: show it now */
}

void gpu_read_picture(gpu_readback* rb) {
    rb->done = 0;
    rb->status = 0;
    if (!gpu_running()) { rb->status = -1; rb->done = 1; return; }
    gpu_op_readback* p = gpu_cmd_alloc(GPU_OP_READBACK, sizeof *p);
    p->rb = rb;
    gpu_cmd_commit();
    gpu_cmd_publish();
}

void gpu_wait_readback(gpu_readback* rb) {
    long zero = 0;
    while (!InterlockedCompareExchange(&rb->done, 0, 0)) WaitOnAddress((volatile VOID*)&rb->done, &zero, sizeof zero, 50);
}
