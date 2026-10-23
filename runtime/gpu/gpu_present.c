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
#include "gpu_pace.h"                    /* (pc-pacing) present delay, VRR detection */
#include "gpu_vrr.h"                     /* (pc-fps) what the driver says about variable refresh */
#include "gs_draw.h"                     /* gs_display_layout_of (the PCRTC layout, shared with gs.c) */

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shaders.h"

#define NPICT     3
#define NSTAGE    4
#define NTS       8
#define NSTATS    4096
#define PICT_MIN  1024                   /* pictures are at least this size (gs_compose: at most 1024 x 1024) */

typedef struct {                         /* shaders/display.hlsl display_cb */
    uint32_t W, H, ux0, uy0, hs, vs, flags, alp, bg, par, deint_mode, so;
    uint32_t cA[2][4], cB[2][4], cC[2][4], cD[2][4];
    uint32_t sox, pad[3];                /* (M10) columns per output pixel (anamorphic widescreen) */
} display_consts;

typedef struct {                         /* shaders/present.hlsl resample_cb */
    float src_n, dst_n, shift, ratio;
    uint32_t mode, axis, other_n, pad0;
    float origin[2], pad1[2];
} resample_consts;

static struct {
    ID3D11Texture2D* up;                 /* uploaded sw picture (raw BGRX bytes as R8G8B8A8) */
    ID3D11ShaderResourceView* up_srv;
    int up_w, up_h;
    ID3D11Texture2D* pict[NPICT];        /* R8G8B8A8_TYPELESS: RTV / SRV UNORM, UAV R32_UINT (GPU compose) */
    ID3D11RenderTargetView* pict_rtv[NPICT];
    ID3D11ShaderResourceView* pict_srv[NPICT];
    ID3D11UnorderedAccessView* pict_uav[NPICT];
    ID3D11Texture2D* hist[2];            /* deinterlacer: the last field of each parity (whole picture layout) */
    ID3D11ShaderResourceView* hist_srv[2];
    uint32_t hist_when[2];               /* VBlank + 1 when it was shown, 0 = none (gs.c deinterlace) */
    int hist_dims[2][2];
    int hist_w, hist_h;                  /* allocated size (M7: on first use, the picture's size) */
    ID3D11Buffer* dcb;                   /* display_consts */
    int pict_w, pict_h;                  /* allocated size */
    int cur, cur_w, cur_h, have;         /* last composed picture */
    float cur_shift[2];                  /* (M7) its sample-grid shift per axis, in picture texels (present resample) */
    uint32_t cur_vblank;
    ID3D11Texture2D* stage[NSTAGE];
    int stage_w, stage_h;
    ID3D11Texture2D* mid;                /* (M7) present: the horizontally resampled picture (RGBA16F) */
    ID3D11RenderTargetView* mid_rtv;
    ID3D11ShaderResourceView* mid_srv;
    int mid_w, mid_h;
    ID3D11Buffer* rcb;                   /* resample_consts */
    struct { int slot, w, h; uint32_t vblank; gpu_readback* rb; float shift[2]; } pend[NSTAGE];
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
    /* (pc-pacing) t0..t1 the frame's drawing and compose, t1b..t2 the present (resample), ev: an event query at t1 (when
     * the picture is ready: the present delay's samples); t1 / t1b / t2 are ended by the present when there is one */
    struct { ID3D11Query *dis, *t0, *t1, *t1b, *t2, *ev; gpu_frame_stats st; int busy, t1_end, pres, ev_end, ready_done;
             double vb_ms, self_ms, q_ms, s_ms; } ts[NTS];
    int ts_head, ts_n;
    gpu_frame_stats st;                  /* picture being built */
    LARGE_INTEGER t_start;
    int ts_cur;                          /* ts slot of the picture being built (-1: none) */
    double pres_q[64];                   /* (M9) per present count (mod 64): when its frame was queued (qpc ms) */
    UINT last_pc;                        /* the last present count the frame statistics reported as shown */
    /* (pc-pacing) */
    pace_delay pd;                       /* the present delay D (ready latency of the pictures) */
    pace_disp disp;                      /* VRR detection from the shown presents */
    double last_sync_ms;                 /* SyncQPCTime of present count last_pc */
    double pres_target[64], pres_call[64], pres_vb[64];   /* per present count: planned time, Present call, VBlank (ms) */
    uint32_t pres_vblank[64];
    int disp_state;                      /* PACE_DISP_* last logged */
    double disp_t;                       /* when the display was last classified */
    HANDLE timer;                        /* high-resolution waitable timer (the paced wait) */
    double last_present_ms;              /* QPC ms of the last Present call (headless paced runs: intervals) */
    double wait0, wait1;                 /* the last paced wait (QPC ms): a frame queued meanwhile started late because of it */
    double first_present_ms;             /* the first windowed present (auto: no statistics after 5 s -> immediate) */
    double content_ms;                   /* (pc-fps) the pictures' cadence the pacing state was made for (frame rate) */
    int vrr_logged;                      /* (pc-fps) the last VRR verdict logged: PACE_DISP_* + 1, 0 none */
} P;

static void vrr_log_driver(const char* when);    /* (pc-fps) */
extern void frame_rate_note_display(double hz) __attribute__((weak));   /* (pc-fps) runtime/frame_rate.c */
extern double frame_rate_target_hz(void) __attribute__((weak));
extern int frame_rate_high(void) __attribute__((weak));

/* (pc-pacing) presentation mode, set from any thread (gpu_present_set_mode), read by the render thread per picture */
static volatile LONG pace_mode = PACE_MODE_AUTO, pace_vsync = PACE_VSYNC_ON, pace_refresh_x1000;
static volatile LONG pace_disp_shared = PACE_DISP_UNKNOWN;
static int pace_log = -1;                /* PS2_PACE_LOG=1: one line per present */
static int pace_headless = -1;           /* PS2_PACE_HEADLESS=1: pace the offscreen presents too (measurements) */
void gpu_present_set_mode(int mode, int vsync) {
    if (mode >= 0 && mode < PACE_MODE_N) InterlockedExchange(&pace_mode, mode);
    if (vsync >= 0 && vsync < PACE_VSYNC_N) InterlockedExchange(&pace_vsync, vsync);
}
void gpu_present_get_mode(int* mode, int* vsync) { *mode = (int)pace_mode; *vsync = (int)pace_vsync; }
void gpu_present_set_refresh(double hz) { InterlockedExchange(&pace_refresh_x1000, (LONG)(hz * 1000.0)); }
int gpu_present_display_state(void) { return (int)pace_disp_shared; }
double gpu_present_delay_ms(void) { return P.pd.delay_ms; }
static volatile LONG pace_extra;          /* frames the paced present holds back (ceil(D / period)), 0 when not pacing */
int gpu_present_pace_frames(void) { return (int)pace_extra; }

static LARGE_INTEGER qpf;

/* picture done (gpu_set_picture_done): set before or after gpu_start, kept across restarts */
static void (*volatile done_fn)(uint32_t vblank, void* user);
static void* volatile done_user;
void gpu_set_picture_done(void (*fn)(uint32_t vblank, void* user), void* user) { done_user = user; done_fn = fn; }
static void (*volatile flip_fn)(void);
void gpu_set_flip_callback(void (*fn)(void)) { flip_fn = fn; }
void gpu_present_signal(uint32_t vblank) {
    void (*fn)(uint32_t, void*) = done_fn;
    if (fn) fn(vblank, done_user);
}

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

static ID3D11Texture2D* tex_create_fmt(int w, int h, DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage, UINT cpu) {
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = usage;
    td.BindFlags = bind;
    td.CPUAccessFlags = cpu;
    ID3D11Texture2D* t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gpu_rt.dev, &td, NULL, &t))) return NULL;
    return t;
}

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
    if (w < PICT_MIN) w = PICT_MIN;
    if (h < PICT_MIN) h = PICT_MIN;
    if (P.pict[0] && P.pict_w >= w && P.pict_h >= h) return 1;
    while (P.npend) pend_complete(1);           /* staging copies read the old pictures */
    int nw = w > P.pict_w ? w : P.pict_w, nh = h > P.pict_h ? h : P.pict_h;
    for (int i = 0; i < NPICT; i++) {
        GPU_RELEASE(P.pict_uav[i]); GPU_RELEASE(P.pict_srv[i]); GPU_RELEASE(P.pict_rtv[i]); GPU_RELEASE(P.pict[i]);
        P.pict[i] = tex_create_fmt(nw, nh, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0);
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R32_UINT;       /* (a 32-bit typeless format may be viewed as R32_UINT) */
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (!P.pict[i] ||
            FAILED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)P.pict[i], &rd, &P.pict_rtv[i])) ||
            FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)P.pict[i], &sd, &P.pict_srv[i])) ||
            FAILED(ID3D11Device_CreateUnorderedAccessView(gpu_rt.dev, (ID3D11Resource*)P.pict[i], &ud, &P.pict_uav[i])))
            return 0;
    }
    P.pict_w = nw;
    P.pict_h = nh;
    P.have = 0;
    return 1;
}

/* deinterlacer history (same layout as the pictures), allocated when the deinterlacer first runs (M7: not
 * at all while pictures are progressive) */
static int ensure_hist(int w, int h) {
    if (P.hist[0] && P.hist_w >= w && P.hist_h >= h) return 1;
    int nw = w > P.hist_w ? w : P.hist_w, nh = h > P.hist_h ? h : P.hist_h;
    for (int i = 0; i < 2; i++) {
        GPU_RELEASE(P.hist_srv[i]); GPU_RELEASE(P.hist[i]);
        P.hist[i] = tex_create_fmt(nw, nh, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (!P.hist[i] || FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)P.hist[i], &sd, &P.hist_srv[i]))) {
            P.hist_w = P.hist_h = 0;
            return 0;
        }
        P.hist_when[i] = 0;
    }
    P.hist_w = nw;
    P.hist_h = nh;
    return 1;
}

/* read-back staging textures, allocated at the first read-back (dumps, tests) */
static int ensure_stage(int w, int h) {
    if (P.stage[0] && P.stage_w >= w && P.stage_h >= h) return 1;
    while (P.npend) pend_complete(1);
    int nw = w > P.stage_w ? w : P.stage_w, nh = h > P.stage_h ? h : P.stage_h;
    for (int i = 0; i < NSTAGE; i++) {
        GPU_RELEASE(P.stage[i]);
        P.stage[i] = tex_create(nw, nh, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        if (!P.stage[i]) { P.stage_w = P.stage_h = 0; return 0; }
    }
    P.stage_w = nw;
    P.stage_h = nh;
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
    bd.ByteWidth = (sizeof(display_consts) + 15) & ~15u;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &P.dcb))) return 0;
    bd.ByteWidth = (sizeof(resample_consts) + 15) & ~15u;
    if (FAILED(ID3D11Device_CreateBuffer(gpu_rt.dev, &bd, NULL, &P.rcb))) return 0;
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
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 }, qe = { D3D11_QUERY_EVENT, 0 };
        for (int i = 0; i < NTS; i++)
            if (FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qd, &P.ts[i].dis)) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &P.ts[i].t0)) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &P.ts[i].t1)) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &P.ts[i].t1b)) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qt, &P.ts[i].t2)) ||
                FAILED(ID3D11Device_CreateQuery(gpu_rt.dev, &qe, &P.ts[i].ev)))
                return 0;
    }
    /* (pc-pacing) */
    P.content_ms = pace_content_ms();    /* (pc-fps: the frame rate's period) */
    pace_delay_init(&P.pd, P.content_ms);
    pace_disp_init(&P.disp, pace_refresh_x1000 > 0 ? 1000000.0 / (double)pace_refresh_x1000 : 0, P.content_ms);
    P.disp_state = PACE_DISP_UNKNOWN;
    if (gpu_rt.cfg.hwnd) vrr_log_driver("at start");
    P.timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (pace_log < 0) { const char* e = getenv("PS2_PACE_LOG"); pace_log = e ? atoi(e) : 0; }
    if (pace_headless < 0) { const char* e = getenv("PS2_PACE_HEADLESS"); pace_headless = e && atoi(e) > 0; }
    return 1;
}

/* (pc-menu) the Options menu / frame rate counter over the picture (runtime/menu_overlay.c; weak: not in every test) */
extern void overlay_d3d11_draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int w, int h)
    __attribute__((weak));
extern void overlay_d3d11_release(ID3D11Device* dev) __attribute__((weak));

void gpu_present_release(void) {
    if (!gpu_rt.dev) return;
    if (overlay_d3d11_release) overlay_d3d11_release(gpu_rt.dev);
    for (int i = 0; i < NTS; i++) {
        GPU_RELEASE(P.ts[i].dis); GPU_RELEASE(P.ts[i].t0); GPU_RELEASE(P.ts[i].t1);
        GPU_RELEASE(P.ts[i].t1b); GPU_RELEASE(P.ts[i].t2); GPU_RELEASE(P.ts[i].ev);
    }
    if (P.timer) CloseHandle(P.timer);
    P.timer = NULL;
    for (int i = 0; i < NSTAGE; i++) GPU_RELEASE(P.stage[i]);
    for (int i = 0; i < NPICT; i++) { GPU_RELEASE(P.pict_uav[i]); GPU_RELEASE(P.pict_srv[i]); GPU_RELEASE(P.pict_rtv[i]); GPU_RELEASE(P.pict[i]); }
    for (int i = 0; i < 2; i++) { GPU_RELEASE(P.hist_srv[i]); GPU_RELEASE(P.hist[i]); }
    GPU_RELEASE(P.dcb);
    GPU_RELEASE(P.mid_srv); GPU_RELEASE(P.mid_rtv); GPU_RELEASE(P.mid); GPU_RELEASE(P.rcb);
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

/* (pc-pacing) the frame in timestamp slot i: has its picture's GPU work finished (event query at the end of its drawing
 * and compose)? Then how long after its VBlank - a sample of the present delay. */
static void ready_note(int i, double ready) {
    if (P.ts[i].ready_done) return;
    P.ts[i].ready_done = 1;
    if (P.ts[i].vb_ms <= 0) return;
    if (pace_log > 1)                            /* PS2_PACE_LOG=2: where the time goes */
        gpu_log("pace: ready v%u: queued +%.2f, started +%.2f, GPU done +%.2f ms after its VBlank", P.ts[i].st.vblank,
                P.ts[i].q_ms - P.ts[i].vb_ms, P.ts[i].s_ms - P.ts[i].vb_ms, ready);
    ready -= P.ts[i].self_ms;                    /* (not the time it waited behind our own paced wait for the previous picture) */
    P.ts[i].st.ready_ms = ready;
    if (i == P.ts_cur) P.st.ready_ms = ready;
    pace_delay_sample(&P.pd, ready);
}
static int ready_poll(int i) {
    if (!P.ts[i].ev_end || P.ts[i].ready_done) return 1;
    BOOL done = FALSE;
    if (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].ev, &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        !done) return 0;
    ready_note(i, now_ms() - P.ts[i].vb_ms);
    return 1;
}
/* every frame whose event is still pending; returns 1 if one still is */
static int ready_poll_all(void) {
    int pending = 0;
    for (int k = 0; k < P.ts_n; k++) pending |= !ready_poll((P.ts_head + k) % NTS);
    if (P.ts_cur >= 0) pending |= !ready_poll(P.ts_cur);
    return pending;
}

static int ts_complete(int block) {             /* oldest timestamp set; returns 1 if still pending */
    if (!P.ts_n) return 0;
    int i = P.ts_head;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT flags = block ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
    HRESULT hr;
    long ph = gpu_rt_phase;
    if (block) gpu_rt_phase = GPU_PH_TS_WAIT;
    while ((hr = ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].dis, &dj, sizeof dj, flags)) == S_FALSE) {
        if (!block) { gpu_rt_phase = ph; return 1; }
        SwitchToThread();
    }
    gpu_rt_phase = ph;
    UINT64 t0 = 0, t1 = 0, t1b = 0, t2 = 0;
    if (hr == S_OK) {
        while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t0, &t0, sizeof t0, 0) == S_FALSE) SwitchToThread();
        while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t1, &t1, sizeof t1, 0) == S_FALSE) SwitchToThread();
        if (P.ts[i].pres) {
            while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t1b, &t1b, sizeof t1b, 0) == S_FALSE) SwitchToThread();
            while (ID3D11DeviceContext_GetData(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t2, &t2, sizeof t2, 0) == S_FALSE) SwitchToThread();
        }
    }
    ready_poll(i);                               /* (finished: its event is signalled by now, if not seen before) */
    gpu_frame_stats st = P.ts[i].st;
    int ok = hr == S_OK && !dj.Disjoint && dj.Frequency;
    st.gpu_ms = ok ? (double)(t1 - t0) * 1000.0 / (double)dj.Frequency : -1.0;
    st.present_gpu_ms = ok && P.ts[i].pres && t2 >= t1b ? (double)(t2 - t1b) * 1000.0 / (double)dj.Frequency : -1.0;
    stats_push(&st);
    P.ts[i].busy = 0;
    P.ts_head = (P.ts_head + 1) % NTS;
    P.ts_n--;
    return 0;
}

#define frame_begin gpu_stats_begin
#define frame_end gpu_stats_end
gpu_frame_stats* gpu_stats_cur(void) { return &P.st; }

void gpu_stats_begin(uint32_t vblank, double front_ms) {
    /* a full timestamp ring waits for the GPU before this picture's time starts */
    if (gpu_rt.cfg.timestamps && P.ts_n == NTS) ts_complete(1);
    QueryPerformanceCounter(&P.t_start);
    memset(&P.st, 0, sizeof P.st);
    P.st.latency_ms = -1.0;
    P.st.photon_ms = -1.0;
    P.st.vblank = vblank;
    P.st.front_ms = front_ms;
    P.st.gpu_ms = -1.0;
    P.st.present_gpu_ms = P.st.ready_ms = P.st.target_ms = -1.0;   /* (pc-pacing) */
    gpu_rt.n_draws = gpu_rt.n_dispatches = gpu_rt.n_copies = gpu_rt.n_uploads = gpu_rt.upload_bytes = 0;
    P.ts_cur = -1;
    if (gpu_rt.cfg.timestamps) {
        int i = (P.ts_head + P.ts_n) % NTS;
        P.ts[i].t1_end = P.ts[i].pres = P.ts[i].ev_end = P.ts[i].ready_done = 0;
        P.ts[i].vb_ms = 0;
        ID3D11DeviceContext_Begin(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].dis);
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t0);
        P.ts_cur = i;
    }
}

void gpu_stats_end(void) {
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
        if (!P.ts[i].t1_end) ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[i].t1);
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
    P.cur_shift[0] = P.cur_shift[1] = 0.0f;
    P.cur_vblank = p->vblank;
    P.have = 1;
}

/* ---- present ------------------------------------------------------------------------------------ */

/* The largest aw:ah rectangle centred in a cw x ch client area, widths and heights rounded to the
 * nearest pixel (window.c fit_aspect: the hw backend shows a picture exactly where sw does). */
static void fit_aspect(int cw, int ch, int aw, int ah, int* x, int* y, int* w, int* h) {
    *x = *y = 0;
    *w = cw;
    *h = ch;
    if (cw <= 0 || ch <= 0 || aw <= 0 || ah <= 0) { *w = *h = 0; return; }
    if ((int64_t)cw * ah > (int64_t)ch * aw) {
        *w = (int)(((int64_t)ch * aw * 2 + ah) / (2 * (int64_t)ah));
        *x = (cw - *w) / 2;
    } else {
        *h = (int)(((int64_t)cw * ah * 2 + aw) / (2 * (int64_t)aw));
        *y = (ch - *h) / 2;
    }
}

static void present_fit(int aw, int ah, int scaler);

void gpu_exec_present(const void* payload) {
    const gpu_op_present* p = payload;
    present_fit(p->aspect_w ? p->aspect_w : 4, p->aspect_h ? p->aspect_h : 3, GPU_SCALER_AUTO);
    frame_end();
}

void gpu_present_show(int aspect_w, int aspect_h, int scaler) {
    present_fit(aspect_w > 0 ? aspect_w : 4, aspect_h > 0 ? aspect_h : 3, scaler);
}

/* (M7, 8.1) PS2_GS_SCALER overrides the scaler of the settings (tests) */
static int scaler_override(int scaler) {
    static int env = -2;
    if (env == -2) {
        const char* e = getenv("PS2_GS_SCALER");
        env = -1;
        if (e && *e) {
            static const char* names[GPU_SCALER_N] = { "auto", "point", "box", "lanczos", "catmull-rom", "bilinear" };
            for (int i = 0; i < GPU_SCALER_N; i++) if (!_stricmp(e, names[i])) env = i;
            if (!_stricmp(e, "catmull")) env = GPU_SCALER_CATMULL;
        }
    }
    return env >= 0 ? env : scaler;
}

/* (M7, 8.1) the resampling filter of one axis: auto picks Lanczos-2 to shrink (supersampling: the sharp
 * downscale), Catmull-Rom to enlarge, point for 1:1 on the grid */
static int axis_mode(int scaler, int src, int dst, float shift) {
    int s = scaler == GPU_SCALER_AUTO ? (src > dst ? GPU_SCALER_LANCZOS : src < dst ? GPU_SCALER_CATMULL :
                                         shift == 0.0f ? GPU_SCALER_POINT : GPU_SCALER_CATMULL)
                                      : scaler;
    switch (s) {                         /* shaders/present.hlsl ps_resample modes */
    case GPU_SCALER_POINT: return 0;
    case GPU_SCALER_BOX: return 1;
    case GPU_SCALER_LANCZOS: return 2;
    case GPU_SCALER_CATMULL: return 3;
    default: return 4;
    }
}

static int ensure_mid(int w, int h) {
    if (P.mid && P.mid_w >= w && P.mid_h >= h) return 1;
    GPU_RELEASE(P.mid_srv); GPU_RELEASE(P.mid_rtv); GPU_RELEASE(P.mid);
    int nw = w > P.mid_w ? w : P.mid_w, nh = h > P.mid_h ? h : P.mid_h;
    P.mid = tex_create_fmt(nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
    if (!P.mid || FAILED(ID3D11Device_CreateRenderTargetView(gpu_rt.dev, (ID3D11Resource*)P.mid, NULL, &P.mid_rtv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(gpu_rt.dev, (ID3D11Resource*)P.mid, NULL, &P.mid_srv))) {
        GPU_RELEASE(P.mid_srv); GPU_RELEASE(P.mid_rtv); GPU_RELEASE(P.mid);
        P.mid_w = P.mid_h = 0;
        return 0;
    }
    P.mid_w = nw;
    P.mid_h = nh;
    return 1;
}

static void resample_pass(ID3D11RenderTargetView* rtv, int vx, int vy, int vw, int vh, ID3D11ShaderResourceView* src,
                          const resample_consts* k) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)P.rcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, k, sizeof *k);
        ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)P.rcb, 0);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 1, 1, &P.rcb);
    fullscreen_pass(rtv, vx, vy, vw, vh, GPU_SH_ps_resample, src);
}

/* The picture (cur_w x cur_h of a larger texture) into the viewport (x, y, lw, lh) of rtv: separable, the
 * columns first (into an RGBA16F intermediate of lw x cur_h), then the rows. The picture's sample grid is
 * shifted by cur_shift texels against a picture whose pixels are centred on the PS2 pixels (8.1: sample 0 is
 * the native pixel); the resample takes it out. */
static void present_resample(ID3D11RenderTargetView* rtv, int x, int y, int lw, int lh, int scaler) {
    if (lw <= 0 || lh <= 0 || !P.cur_w || !P.cur_h) return;
    scaler = scaler_override(scaler);
    if (!ensure_mid(lw, P.cur_h)) {      /* (no intermediate: the M6 bilinear present) */
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)P.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* f = m.pData;
            f[0] = (float)P.cur_w / (float)P.pict_w; f[1] = (float)P.cur_h / (float)P.pict_h;
            f[2] = ((float)P.cur_w - 0.5f) / (float)P.pict_w; f[3] = ((float)P.cur_h - 0.5f) / (float)P.pict_h;
            ID3D11DeviceContext_Unmap(gpu_rt.ctx, (ID3D11Resource*)P.cb, 0);
        }
        ID3D11DeviceContext_PSSetConstantBuffers(gpu_rt.ctx, 0, 1, &P.cb);
        fullscreen_pass(rtv, x, y, lw, lh, GPU_SH_ps_present, P.pict_srv[P.cur]);
        return;
    }
    resample_consts k;
    memset(&k, 0, sizeof k);
    k.src_n = (float)P.cur_w; k.dst_n = (float)lw; k.shift = P.cur_shift[0]; k.ratio = (float)P.cur_w / (float)lw;
    k.mode = (uint32_t)axis_mode(scaler, P.cur_w, lw, P.cur_shift[0]);
    k.axis = 0;
    k.other_n = (uint32_t)P.cur_h;
    resample_pass(P.mid_rtv, 0, 0, lw, P.cur_h, P.pict_srv[P.cur], &k);
    k.src_n = (float)P.cur_h; k.dst_n = (float)lh; k.shift = P.cur_shift[1]; k.ratio = (float)P.cur_h / (float)lh;
    k.mode = (uint32_t)axis_mode(scaler, P.cur_h, lh, P.cur_shift[1]);
    k.axis = 1;
    k.other_n = (uint32_t)lw;
    k.origin[0] = (float)x; k.origin[1] = (float)y;
    resample_pass(rtv, x, y, lw, lh, P.mid_srv, &k);
}

/* (pc-pacing) the display's refresh rate (exact rational of the active mode) for the monitor the window is on, Hz; 0 if
 * unknown. QueryDisplayConfig: the source whose GDI name is the monitor's device. */
double gpu_window_refresh_hz(void* hwnd_) {
    HWND hwnd = (HWND)hwnd_;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!hwnd || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO*)&mi)) return 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return 0;
    DISPLAYCONFIG_PATH_INFO* paths = calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO* modes = calloc(nm ? nm : 1, sizeof *modes);
    double hz = 0;
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np && hz == 0; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
            memset(&sn, 0, sizeof sn);
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof sn;
            sn.header.adapterId = paths[i].sourceInfo.adapterId;
            sn.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, mi.szDevice)) continue;
            DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
            if (r.Denominator) hz = (double)r.Numerator / (double)r.Denominator;
        }
    }
    free(paths);
    free(modes);
    if (hz <= 0) {                               /* (fallback: the integer rate of the mode) */
        DEVMODEW dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency;
    }
    return hz;
}

/* (pc-pacing) wait until `target` (QPC ms), noting meanwhile when the picture's GPU work finishes (timestamp slot tsi) */
static void pace_wait(double target, int tsi) {
    long ph = gpu_rt_phase;
    gpu_rt_phase = GPU_PH_SWAP_WAIT;
    double limit = now_ms() + 3.0 * P.content_ms;  /* (never more than 3 frames, whatever the clocks say) */
    if (target > limit) target = limit;
    for (;;) {
        if (tsi >= 0) ready_poll(tsi);
        double left = target - now_ms();
        if (left <= 0) break;
        if (left > 1.3 && P.timer) {             /* sleep in steps of at most 1 ms (the ready time's resolution) */
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left > 2.0 ? 1.0 : left - 0.8) * 10000.0);
            if (SetWaitableTimer(P.timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(P.timer, 20);
            else Sleep(1);
        } else if (left > 1.3) {
            Sleep(1);
        } else {
            SwitchToThread();                    /* (the last ~1 ms: yield-spin for precision) */
        }
    }
    gpu_rt_phase = ph;
}

/* (pc-pacing) the display's record of the presents (DXGI frame statistics): intervals between consecutive presents as
 * shown -> VRR detection; PS2_PACE_LOG lines */
static void pace_statistics(UINT pc) {
    DXGI_FRAME_STATISTICS fs;
    memset(&fs, 0, sizeof fs);
    HRESULT hr = IDXGISwapChain1_GetFrameStatistics(P.sc, &fs);
    if (FAILED(hr) || !fs.PresentCount || fs.PresentCount == P.last_pc || pc - fs.PresentCount >= 60) {
        /* (pc-fps) say why the display's record stopped (once per 10 s): no statistics = no VRR verdict */
        static double said;
        double t = now_ms();
        if (P.last_sync_ms > 0 && t - P.last_sync_ms > 3000.0 && t - said > 10000.0) {
            said = t;
            HWND fg = GetForegroundWindow();
            gpu_log("pace: no frame statistics for %.0f s (hr 0x%08lX, shown present %u, presented %u): the game window is %s",
                    (t - P.last_sync_ms) / 1000.0, (unsigned long)hr, (unsigned)fs.PresentCount, (unsigned)pc,
                    fg == (HWND)gpu_rt.cfg.hwnd ? "in front" : "not in front (another window has the focus)");
        }
        return;
    }
    double shown = (double)fs.SyncQPCTime.QuadPart * 1000.0 / (double)qpf.QuadPart;
    double q = P.pres_q[fs.PresentCount % 64];   /* (M9) photon latency: frame queued -> shown */
    if (q > 0 && shown >= q) P.st.photon_ms = shown - q;
    double iv = fs.PresentCount == P.last_pc + 1 && P.last_sync_ms > 0 ? shown - P.last_sync_ms : -1;
    if (iv > 0) pace_disp_interval(&P.disp, iv);
    if (pace_log) {
        int k = (int)(fs.PresentCount % 64);
        double vb = P.pres_vb[k];
        gpu_log("pace: shown present %u (v%u) at %.3f ms: VBlank %.3f, planned +%.2f, Present +%.2f, shown +%.2f, interval %.2f ms",
                (unsigned)fs.PresentCount, P.pres_vblank[k], shown, vb, P.pres_target[k] > 0 ? P.pres_target[k] - vb : -1.0,
                P.pres_call[k] - vb, vb > 0 ? shown - vb : -1.0, iv);
    }
    P.last_pc = fs.PresentCount;
    P.last_sync_ms = shown;
    double now = now_ms();
    if (now - P.disp_t < 2000.0) return;         /* every 2 s: what the display does with the presents */
    P.disp_t = now;
    double hz_now = gpu_window_refresh_hz(gpu_rt.cfg.hwnd);   /* (the display's mode may have changed: display_mode.c) */
    if (hz_now > 0 && frame_rate_note_display) frame_rate_note_display(hz_now);   /* (pc-fps: frame_rate = display) */
    if (hz_now > 0 && fabs(1000.0 / hz_now - P.disp.refresh_ms) > 0.05) {
        pace_disp_init(&P.disp, 1000.0 / hz_now, P.content_ms);
        P.disp_state = PACE_DISP_UNKNOWN;
        InterlockedExchange(&pace_disp_shared, PACE_DISP_UNKNOWN);
        InterlockedExchange(&pace_refresh_x1000, (LONG)(hz_now * 1000.0));
        gpu_log("pace: the display runs at %.3f Hz now", hz_now);
        return;
    }
    int s = pace_disp_classify(&P.disp);
    InterlockedExchange(&pace_disp_shared, s);
    if (s == P.disp_state || s == PACE_DISP_UNKNOWN) return;
    P.disp_state = s;
    double hz = P.disp.refresh_ms > 0 ? 1000.0 / P.disp.refresh_ms : 0;
    double c = P.content_ms, fps = 1000.0 / c;
    /* (pc-fps) one clear line: is variable refresh active, how the presents reach the display, what the driver says */
    int comp = gpu_vrr_composition(P.sc);
    HWND fg = GetForegroundWindow();
    int front = fg == (HWND)gpu_rt.cfg.hwnd || (fg && GetAncestor(fg, GA_ROOTOWNER) == (HWND)gpu_rt.cfg.hwnd);
    if (s == PACE_DISP_VRR) {
        gpu_log("VRR: ACTIVE - the display (%.2f Hz maximum) follows the game's pictures: each shown for %.2f ms (%.2f frames/s); "
                "presentation %s", hz, c, fps, gpu_vrr_composition_name(comp));
    } else if (s == PACE_DISP_FIXED) {
        gpu_log("VRR: NOT ACTIVE - the display refreshes at a fixed %.2f Hz: a %.2f frames/s picture lasts %.2f refreshes, so pictures "
                "alternate between %d and %d refreshes (judder). Presentation %s, game window %s", hz, fps, c / P.disp.refresh_ms,
                (int)(c / P.disp.refresh_ms), (int)(c / P.disp.refresh_ms) + 1, gpu_vrr_composition_name(comp),
                front ? "in front" : "NOT in front (another window has the focus: variable refresh follows the window in front)");
        vrr_log_driver("display fixed");
    } else {
        gpu_log("VRR: not needed - the display's %.2f Hz is %d refresh%s per %.2f frames/s picture: even", hz,
                pace_refresh_multiple(P.disp.refresh_ms, c), pace_refresh_multiple(P.disp.refresh_ms, c) > 1 ? "es" : "", fps);
    }
}

/* (pc-fps) the driver's view of variable refresh (NVIDIA: the G-SYNC mode and the display's VRR state) and what the player
 * can change; at start and when the display is found to refresh at a fixed rate */
static void vrr_log_driver(const char* when) {
    gpu_vrr_info v;
    char line[512];
    gpu_vrr_query(gpu_rt.cfg.hwnd, &v);
    gpu_log("VRR (%s): %s", when, gpu_vrr_describe(&v, line, sizeof line));
    const char* advice = gpu_vrr_advice(&v);
    if (advice) gpu_log("VRR: to turn it on: %s", advice);
}

static void present_fit(int aw, int ah, int scaler) {
    double t0 = now_ms();
    /* (pc-fps) the frame rate changed: the pacing state is for the new cadence */
    double content = pace_content_ms();
    if (fabs(content - P.content_ms) > 0.01) {
        P.content_ms = content;
        pace_delay_init(&P.pd, content);
        pace_disp_init(&P.disp, P.disp.refresh_ms, content);
        P.disp_state = PACE_DISP_UNKNOWN;
        InterlockedExchange(&pace_disp_shared, PACE_DISP_UNKNOWN);
        gpu_log("pace: pictures every %.2f ms (%.2f frames/s)", content, 1000.0 / content);
    }
    int tsi = P.ts_cur;
    double vb = gpu_rt.cur_vblank_ms;
    /* (pc-pacing) the frame's drawing and compose end here: its GPU time without the present (and without the paced wait
     * below, which the old t1 at the frame's end counted as GPU time), and the event that says when its picture is ready */
    if (tsi >= 0) {
        if (!P.ts[tsi].t1_end) { ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[tsi].t1); P.ts[tsi].t1_end = 1; }
        ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[tsi].ev);
        P.ts[tsi].ev_end = 1;
        P.ts[tsi].vb_ms = vb;
        /* the time it waited in the render queue (behind earlier pictures' paced waits, by design) is not part of when it
         * could be ready: ready = (queued - VBlank) + (GPU done - execution start) */
        double q = gpu_rt.cur_queued_ms, s = (double)P.t_start.QuadPart * 1000.0 / (double)qpf.QuadPart;
        P.ts[tsi].self_ms = q > 0 && s > q ? s - q : 0;
        P.ts[tsi].q_ms = q;
        P.ts[tsi].s_ms = s;
    }
    if (P.sc) {
        int cw = 0, ch = 0;
        if (gpu_rt.cfg.client_size) gpu_rt.cfg.client_size(&cw, &ch);
        else {
            RECT rc;
            GetClientRect((HWND)gpu_rt.cfg.hwnd, &rc);
            cw = rc.right;
            ch = rc.bottom;
        }
        if (cw <= 0 || ch <= 0) return;         /* minimised */
        if (cw != P.bb_w || ch != P.bb_h) {
            ID3D11DeviceContext_OMSetRenderTargets(gpu_rt.ctx, 0, NULL, NULL);
            GPU_RELEASE(P.rtv);
            ID3D11DeviceContext_Flush(gpu_rt.ctx);   /* (the old back buffers must not be referenced) */
            HRESULT hr = IDXGISwapChain1_ResizeBuffers(P.sc, 0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN, P.sc_flags);
            if (FAILED(hr) || !make_rtv()) {
                gpu_log("swap chain resize to %dx%d failed (hr=0x%08lX)", cw, ch, (unsigned long)hr);
                P.bb_w = -1;
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) gpu_rt.lost = 1;
                return;
            }
            P.bb_w = cw;
            P.bb_h = ch;
            /* (pc-pacing) the window may be on another monitor now (fullscreen switch, moved): its refresh rate */
            double hz = gpu_window_refresh_hz(gpu_rt.cfg.hwnd);
            if (hz > 0 && frame_rate_note_display) frame_rate_note_display(hz);   /* (pc-fps) */
            if (hz > 0 && fabs(1000.0 / hz - P.disp.refresh_ms) > 0.01) {
                pace_disp_init(&P.disp, 1000.0 / hz, P.content_ms);
                P.disp_state = PACE_DISP_UNKNOWN;
                InterlockedExchange(&pace_refresh_x1000, (LONG)(hz * 1000.0));
                gpu_log("pace: the window's display runs at %.3f Hz (%dx%d)", hz, cw, ch);
            }
        }
    }
    /* (pc-pacing) paced: picture v leaves at its VBlank's time + D (gpu_pace.c pace_delay), an even 16.68 ms cadence that a
     * VRR display shows as such; the render thread takes the next frame only after this present. Behind (a newer
     * frame already queued and this one more than half a frame late): the present is skipped, the next picture shows. */
    int mode = (int)pace_mode, vsync = (int)pace_vsync;
    /* auto: paced until the display's behaviour is known (pace_statistics), then paced on a VRR display only - on a fixed
     * refresh the picture waits for the display's next refresh anyway (on 60 / 120 / 240 Hz the display's VBlanks drive the
     * game: every picture lasts the same number of refreshes; on 144 / 165 Hz the 2-or-3-refresh judder is the same paced
     * or not), so it is presented as soon as it is ready: no added latency */
    if (P.sc && !P.first_present_ms) P.first_present_ms = t0;
    /* (no frame statistics at all after 5 s - the window hidden, or a system that does not report them: nothing says pacing
     * helps, so the latency wins) */
    int no_stats = P.disp.n < 10 && P.first_present_ms > 0 && t0 - P.first_present_ms > 5000.0;
    int auto_immediate = mode == PACE_MODE_AUTO && P.sc &&
                         (P.disp_state == PACE_DISP_FIXED || P.disp_state == PACE_DISP_MULTIPLE || (P.disp_state == PACE_DISP_UNKNOWN && no_stats));
    /* (pc-fps) frame_rate = unlimited: the pictures have no cadence to keep - each is presented when it is drawn */
    int unlimited = frame_rate_target_hz && frame_rate_target_hz() == 0;
    int paced = (P.sc || pace_headless) && mode != PACE_MODE_IMMEDIATE && !auto_immediate && !unlimited && vb > 0;
    double target = paced ? vb + P.pd.delay_ms : 0;
    if (paced) P.st.target_ms = P.pd.delay_ms;
    InterlockedExchange(&pace_extra, paced ? (LONG)ceil(P.pd.delay_ms / P.content_ms) : 0);
    if (paced && gpu_rt.cur_behind) paced = 0;   /* behind (a newer frame waits): no wait, present it now */
    /* (pc-fps) above 60 fps no picture is skipped: when the GPU is slower than the frame limiter every picture is "late" and
     * a newer one always queued - skipping them would show none; the queue is short there instead (gpu_submit_sink) */
    int fr_high = frame_rate_high && frame_rate_high();
    if (gpu_rt.cur_behind && !fr_high && vb > 0 && t0 > vb + P.pd.delay_ms + 0.5 * P.content_ms) {
        P.st.skipped = 1;
        P.st.present_ms += now_ms() - t0;
        return;
    }
    if (paced) {
        ID3D11DeviceContext_Flush(gpu_rt.ctx);   /* (the GPU draws the picture while we wait) */
        P.wait0 = now_ms();
        pace_wait(target, tsi);
        P.wait1 = now_ms();
    } else if (tsi >= 0) {
        ready_poll(tsi);
    }
    if (P.sc && P.waitable) {
        /* The waitable object (frame latency 1) is signalled once the GPU has finished the previous picture; normally
         * long before this one is ready. When the GPU falls behind (NVIDIA's power management lowers the clocks of a
         * lightly loaded GPU, and frames then take 15-40 ms until it raises them again), the render thread waits here
         * and the frames queued behind it absorb the delay (gpu_submit_sink) before the game notices. */
        long ph = gpu_rt_phase;
        gpu_rt_phase = GPU_PH_SWAP_WAIT;
        /* (pc-pacing) at most two frame periods (it was 100 ms): a display or system hiccup must not hold the renderer, and
         * behind it the game, for longer */
        DWORD w = WaitForSingleObjectEx(P.waitable, 34, TRUE);
        gpu_rt_phase = ph;
        if (w == WAIT_TIMEOUT) P.st.dropped = 1;   /* (presented anyway) */
    }
    if (tsi >= 0) { ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[tsi].t1b); P.ts[tsi].pres = 1; }
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(gpu_rt.ctx, P.rtv, black);
    if (P.have) {
        int x, y, lw, lh;
        fit_aspect(P.bb_w, P.bb_h, aw, ah, &x, &y, &lw, &lh);
        present_resample(P.rtv, x, y, lw, lh, scaler);
    }
    if (overlay_d3d11_draw) overlay_d3d11_draw(gpu_rt.dev, gpu_rt.ctx, P.rtv, P.bb_w, P.bb_h);
    if (tsi >= 0) ID3D11DeviceContext_End(gpu_rt.ctx, (ID3D11Asynchronous*)P.ts[tsi].t2);
    double call = now_ms();
    if (paced) P.st.present_late_ms = call - target;
    if (P.sc) {
        int tear = 0;
        /* (pc-pacing) vsync on: Present(1); off: Present(0) with tearing (the old pacing = vrr); adaptive: on time ->
         * synced, late -> at once */
        int interval = pace_present_flags(vsync,
                                          gpu_rt.caps.tearing, call, target, &tear);
        HRESULT hr = IDXGISwapChain1_Present(P.sc, (UINT)interval, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            gpu_log("device lost (hr=0x%08lX, reason 0x%08lX)", (unsigned long)hr,
                    (unsigned long)ID3D11Device_GetDeviceRemovedReason(gpu_rt.dev));
            gpu_rt.lost = 1;
        }
        P.st.presented = 1;
        void (*ff)(void) = flip_fn;
        if (ff) ff();
        UINT pc = 0;
        if (SUCCEEDED(IDXGISwapChain1_GetLastPresentCount(P.sc, &pc))) {
            int k = (int)(pc % 64);
            P.pres_q[k] = gpu_rt.cur_queued_ms;
            P.pres_target[k] = target;
            P.pres_call[k] = call;
            P.pres_vb[k] = vb;
            P.pres_vblank[k] = P.st.vblank;
        }
        pace_statistics(pc);
    } else if (paced && pace_log) {              /* (headless paced runs: the Present calls' cadence) */
        gpu_log("pace: offscreen present v%u: VBlank %.3f, planned +%.2f, Present +%.2f, interval %.2f ms", P.st.vblank, vb, target - vb,
                call - vb, P.last_present_ms > 0 ? call - P.last_present_ms : -1.0);
    }
    P.last_present_ms = call;
    P.st.present_ms += now_ms() - t0;
}

/* ---- M6: the picture composed on the GPU (shaders/display.hlsl) ------------------------------------- */

/* (M7) The standard flicker-filter pattern - both circuits on one buffer (FBP, FBW, PSM, DBX, DX equal), one
 * line apart (DBY), mixed at a fixed ALP (MMOD 1) - shows the circuit with the lower DBY alone: the TV
 * flicker filter off (Modern). Anything else is composed as programmed. Returns 1 when it changed priv. */
static int flicker_strip(uint64_t priv[16]) {
    uint64_t pmode = priv[0];
    if ((pmode & 3) != 3 || !((pmode >> 5) & 1)) return 0;
    uint64_t f1 = priv[7], f2 = priv[9], d1 = priv[8], d2 = priv[10];
    const uint64_t fbmask = 0x1FFull | 0x3Full << 9 | 0x1Full << 15 | 0x7FFull << 32;   /* FBP, FBW, PSM, DBX */
    if ((f1 & fbmask) != (f2 & fbmask) || (d1 & 0xFFF) != (d2 & 0xFFF)) return 0;
    int dby1 = (int)((f1 >> 43) & 0x7FF), dby2 = (int)((f2 >> 43) & 0x7FF);
    if (dby1 - dby2 != 1 && dby2 - dby1 != 1) return 0;
    priv[0] &= ~(uint64_t)(dby1 > dby2 ? 1 : 2);
    return 1;
}

void gpu_present_compose(const gpu_compose_in* in) {
    ID3D11DeviceContext* c = gpu_rt.ctx;
    gs_display_layout L;
    uint64_t priv[16];
    if (in->priv) {
        memcpy(priv, in->priv, sizeof priv);
        int stripped = in->flicker_off && flicker_strip(priv);
        /* PS2_PACE_LOG=3 (pc-pacing, "shake" checks): the circuits shown at each VBlank - enable bits, DBY / DY of each */
        if (pace_log >= 3) {
            uint64_t pm = in->priv[0];
            gpu_log("display v%u: PMODE en %d%d mmod %d -> shown en %d%d%s | DISPFB1 dby %d DISPLAY1 dy %d | DISPFB2 dby %d DISPLAY2 dy %d | field %d",
                    (unsigned)in->priv[15], (int)(pm & 1), (int)(pm >> 1 & 1), (int)(pm >> 5 & 1), (int)(priv[0] & 1), (int)(priv[0] >> 1 & 1),
                    stripped ? " (flicker filter off)" : "", (int)(in->priv[7] >> 43 & 0x7FF), (int)(in->priv[8] >> 12 & 0x7FF),
                    (int)(in->priv[9] >> 43 & 0x7FF), (int)(in->priv[10] >> 12 & 0x7FF), in->field_code);
        }
    }
    int ok = in->priv && gs_display_layout_of(priv, &L);
    int so = in->scale > 0 ? in->scale : 1;
    int sox = in->scale_x > 0 ? in->scale_x : so;   /* (M10: anamorphic widescreen, the display family's kx) */
    if (!ensure_pictures(ok ? L.W * sox : PICT_MIN, ok ? L.H * so : PICT_MIN)) {
        gpu_log("cannot allocate the pictures");
        gpu_rt.lost = 1;
        return;
    }
    int next = (P.cur + 1) % NPICT;
    P.cur_vblank = in->priv ? (uint32_t)in->priv[15] : 0;
    if (!ok) {                                  /* display off: black (gs_compose returns no picture) */
        static const float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(c, P.pict_rtv[next], black);
        P.cur = next;
        P.cur_w = 0;                            /* (a read-back of it fails: nothing to dump, as sw) */
        P.cur_h = 0;
        P.have = 0;
        return;
    }
    int W = L.W * sox, H = L.H * so;     /* (M7: the picture at so x so pixels per output pixel; M10: sox x so) */
    display_consts k;
    memset(&k, 0, sizeof k);
    k.W = (uint32_t)W; k.H = (uint32_t)H; k.ux0 = (uint32_t)L.ux0; k.uy0 = (uint32_t)L.uy0;
    k.hs = (uint32_t)L.hs; k.vs = (uint32_t)L.vs;
    k.flags = (L.mmod ? 1u : 0u) | (L.slbg ? 2u : 0u) | (L.field_mode ? 4u : 0u);
    /* progressive pictures of half-line-offset fields (the game draws an odd field one frame line lower,
     * gs.c weaves it): the odd field's rows go one frame line down, so the picture does not bob */
    if (!in->fields && L.field_mode && in->field_code == 8) k.flags |= 8u;
    k.alp = (uint32_t)L.alp;
    k.bg = L.bg;
    k.so = (uint32_t)so;
    k.sox = (uint32_t)sox;
    for (int i = 0; i < 2; i++) {
        const gs_circuit* ci = &L.c[i];
        k.cA[i][0] = (uint32_t)ci->en; k.cA[i][1] = (uint32_t)ci->fbp; k.cA[i][2] = (uint32_t)ci->fbw; k.cA[i][3] = (uint32_t)ci->psm;
        k.cB[i][0] = (uint32_t)ci->dbx; k.cB[i][1] = (uint32_t)ci->dby; k.cB[i][2] = (uint32_t)ci->dx; k.cB[i][3] = (uint32_t)ci->dy;
        k.cC[i][0] = (uint32_t)ci->magh; k.cC[i][1] = (uint32_t)ci->magv; k.cC[i][2] = (uint32_t)ci->dw; k.cC[i][3] = (uint32_t)ci->dh;
        k.cD[i][0] = in->tex[i] ? 1u : 0u;
        k.cD[i][1] = (uint32_t)(in->kx[i] > 0 ? in->kx[i] : 1);
        k.cD[i][2] = (uint32_t)(in->ky[i] > 0 ? in->ky[i] : 1);
    }
    /* the deinterlacer's history state (gs.c deinterlace): which field this is, what is usable */
    /* (M7: only with field display - interlacing on; progressive pictures need no history) */
    int f = in->field_code, deint = 0, par = 0, mode = 0;
    uint32_t now = in->priv ? (uint32_t)in->priv[15] + 1 : 1;
    if (L.field_mode && in->fields) {
        if (f != 0 && f != 8) P.hist_when[0] = P.hist_when[1] = 0;
        else if (ensure_hist(W, H)) {
            par = f == 8;
            int oth = !par;
            int have_same = P.hist_when[par] && P.hist_when[par] + 3 >= now && P.hist_dims[par][0] == W && P.hist_dims[par][1] == H;
            int have_oth = P.hist_when[oth] && P.hist_when[oth] + 2 >= now && P.hist_dims[oth][0] == W && P.hist_dims[oth][1] == H;
            deint = 1 + (par || have_oth);     /* 1: history only, 2: interpolate the other lines too */
            mode = have_oth && have_same;
        }
    }
    k.par = (uint32_t)par;
    k.deint_mode = (uint32_t)mode;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)P.dcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &k, sizeof k);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)P.dcb, 0);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    ID3D11ShaderResourceView* srv[7] = { in->vram, in->tex[0], in->tex[1], NULL, NULL, NULL, in->swz };
    ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_compose), NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &P.dcb);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 7, srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &P.pict_uav[next], NULL);
    ID3D11DeviceContext_Dispatch(c, (UINT)(W + 7) / 8, (UINT)(H + 7) / 8, 1);
    gpu_rt.n_dispatches++;
    if (deint == 2) {
        int oth = !par, rows = L.H > oth ? (L.H - oth + 1) / 2 * so : 0;   /* (frame lines of so rows) */
        ID3D11ShaderResourceView* hs[2] = { P.hist_srv[par], P.hist_srv[oth] };
        ID3D11DeviceContext_CSSetShaderResources(c, 3, 2, hs);
        ID3D11DeviceContext_CSSetShader(c, gpu_cs(GPU_SH_cs_deint), NULL, 0);
        if (rows) {
            ID3D11DeviceContext_Dispatch(c, (UINT)(W + 7) / 8, (UINT)(rows + 7) / 8, 1);
            gpu_rt.n_dispatches++;
        }
    }
    ID3D11ShaderResourceView* none[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    ID3D11UnorderedAccessView* nouav = NULL;
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 7, none);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &nouav, NULL);
    if (deint) {                                /* this field's lines become the history of its parity */
        D3D11_BOX box = { 0, 0, 0, (UINT)W, (UINT)H, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource*)P.hist[par], 0, 0, 0, 0, (ID3D11Resource*)P.pict[next], 0, &box);
        gpu_rt.n_copies++;
        P.hist_when[par] = now;
        P.hist_dims[par][0] = W;
        P.hist_dims[par][1] = H;
    }
    /* (M7, 8.1) the sample grid: a target with k samples per native pixel holds sample i at GS x + i / k, so the
     * picture's pixel centres sit (P - r) / 2 texels left of / above those of a picture centred on the PS2's
     * pixels, P = picture texels per native pixel (so; field mode: 2 so rows per buffer row), r = texels per
     * sample (P / k). Circuits read from memory repeat pixels: no shift. (The reference circuit decides.) */
    int rc = L.ref;
    P.cur_shift[0] = P.cur_shift[1] = 0.0f;
    if (in->tex[rc] && in->kx[rc] > 0 && in->ky[rc] > 0) {
        float px = (float)sox, py = (float)(so * (L.field_mode ? 2 : 1));
        float rx = px / (float)in->kx[rc], ry = py / (float)in->ky[rc];
        P.cur_shift[0] = (px - rx) * 0.5f;
        P.cur_shift[1] = (py - ry) * 0.5f;
    }
    P.cur = next;
    P.cur_w = W;
    P.cur_h = H;
    P.have = 1;
}

void gpu_present_readback(gpu_readback* rb) { gpu_exec_readback(&rb); }

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
    long ph = gpu_rt_phase;
    if (block) gpu_rt_phase = GPU_PH_MAP_WAIT;
    HRESULT hr = ID3D11DeviceContext_Map(gpu_rt.ctx, (ID3D11Resource*)st, 0, D3D11_MAP_READ,
                                         block ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    gpu_rt_phase = ph;
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
    rb->shift[0] = P.pend[i].shift[0];
    rb->shift[1] = P.pend[i].shift[1];
    P.pend_head = (P.pend_head + 1) % NSTAGE;
    P.npend--;
    rb_finish(rb, status);
}

void gpu_exec_readback(const void* payload) {
    gpu_readback* rb = ((const gpu_op_readback*)payload)->rb;
    if (gpu_rt.lost || !P.have) { rb_finish(rb, -1); return; }
    if (!ensure_stage(P.cur_w, P.cur_h)) { rb_finish(rb, -1); return; }
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
    P.pend[k].shift[0] = P.cur_shift[0];
    P.pend[k].shift[1] = P.cur_shift[1];
    D3D11_BOX box = { 0, 0, 0, (UINT)P.cur_w, (UINT)P.cur_h, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(gpu_rt.ctx, (ID3D11Resource*)P.stage[k], 0, 0, 0, 0,
                                              (ID3D11Resource*)P.pict[P.cur], 0, &box);
    gpu_rt.n_copies++;
    P.npend++;
}

/* (pc-pacing) pictures whose ready time is still unknown (the render thread then polls every millisecond when idle) */
int gpu_present_events_pending(void) { return ready_poll_all(); }

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
    gpu_picture_aspect(bgrx, ok, w, h, stride, vblank, 4, 3);
}

void gpu_picture_aspect(const uint32_t* bgrx, int ok, int w, int h, int stride, uint32_t vblank, int pic_aspect_w,
                        int pic_aspect_h) {
    if (!gpu_running()) return;
    double t0 = now_ms();
    gpu_op_picture pic;
    memset(&pic, 0, sizeof pic);
    pic.vblank = vblank;
    gpu_cmd_lock();
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
    q->aspect_w = (uint16_t)pic_aspect_w;
    q->aspect_h = (uint16_t)pic_aspect_h;
    gpu_cmd_commit();
    gpu_cmd_publish();                          /* a VBlank: show it now */
    gpu_cmd_unlock();
}

void gpu_read_picture(gpu_readback* rb) {
    rb->done = 0;
    rb->status = 0;
    if (!gpu_running()) { rb->status = -1; rb->done = 1; return; }
    gpu_cmd_lock();
    gpu_op_readback* p = gpu_cmd_alloc(GPU_OP_READBACK, sizeof *p);
    p->rb = rb;
    gpu_cmd_commit();
    gpu_cmd_publish();
    gpu_cmd_unlock();
}

void gpu_wait_readback(gpu_readback* rb) {
    long zero = 0;
    while (!InterlockedCompareExchange(&rb->done, 0, 0)) WaitOnAddress((volatile VOID*)&rb->done, &zero, sizeof zero, 50);
}
