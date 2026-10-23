/* VRR probe (pc-fps): does the display follow the presents (G-Sync / FreeSync active) or its fixed refresh grid?
 *
 * Presents a plain coloured flip-model swap chain (FLIP_DISCARD, ALLOW_TEARING, frame latency 1: the game's setup,
 * runtime/gpu/gpu_present.c) at an off-grid cadence for a few seconds in each presentation setup and reads the display's
 * own record of when each present was shown (DXGI frame statistics: SyncQPCTime) plus the composition mode DXGI reports
 * (IDXGISwapChainMedia: composed by DWM / overlay / independent). On a fixed-refresh display the shown intervals sit on
 * the refresh grid (165 Hz: multiples of 6.06 ms); with VRR they follow the cadence (10 ms at 100 Hz).
 *
 *   clang -O1 -o build\vrr_probe.exe scripts\vrr_probe.c -ld3d11 -ldxgi -ldxguid -luser32 -lgdi32 -ldwmapi
 *   vrr_probe.exe [seconds per phase (default 3)] [phases: b=borderless, t=borderless tearing, w=windowed, x=exclusive]
 *
 * Opens a window on the primary monitor (keep it short on a shared PC); the exclusive phase (x) uses SetFullscreenState
 * at the desktop's mode (no mode change requested). Writes one result line per phase to stdout. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LARGE_INTEGER qpf;
static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart; }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0;
    return DefWindowProcW(h, m, w, l);
}

static void bring_front(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (other && other != me) AttachThreadInput(other, me, TRUE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (other && other != me) AttachThreadInput(other, me, FALSE);
}

static void pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

static const char* comp_name(int m) {
    switch (m) {
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: return "composed (DWM)";
    case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: return "overlay";
    case DXGI_FRAME_PRESENTATION_MODE_NONE: return "none (independent)";
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: return "composition failure";
    default: return "?";
    }
}

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&qpf);
    double secs = argc > 1 ? atof(argv[1]) : 3.0;
    const char* phases = argc > 2 ? argv[2] : "btw";
    double cadence = argc > 3 ? atof(argv[3]) : 10.0;            /* ms between presents */
    SetProcessDPIAware();
    timeBeginPeriod(1);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"vrr_probe";
    RegisterClassW(&wc);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    HWND hwnd = CreateWindowExW(0, L"vrr_probe", L"VRR probe", WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, SW_SHOW);
    bring_front(hwnd);
    pump();

    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) { printf("no device\n"); return 1; }
    IDXGIDevice* dx = NULL; IDXGIAdapter* ad = NULL; IDXGIFactory2* f2 = NULL; IDXGIFactory5* f5 = NULL;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void**)&dx);
    IDXGIDevice_GetAdapter(dx, &ad);
    IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void**)&f2);
    BOOL tearing = FALSE;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(f2, &IID_IDXGIFactory5, (void**)&f5)))
        IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
    DXGI_ADAPTER_DESC adesc;
    IDXGIAdapter_GetDesc(ad, &adesc);
    DEVMODEW dm = { 0 };
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
    printf("adapter %ls, monitor %dx%d @ %lu Hz, tearing supported %d, cadence %.2f ms\n", adesc.Description, mw, mh,
           (unsigned long)dm.dmDisplayFrequency, (int)tearing, cadence);
    double refresh = 1000.0 / (double)dm.dmDisplayFrequency;

    DXGI_SWAP_CHAIN_DESC1 d = { 0 };
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    IDXGISwapChain1* sc = NULL;
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)dev, hwnd, &d, NULL, NULL, &sc))) { printf("no swap chain\n"); return 1; }
    IDXGIFactory2_MakeWindowAssociation(f2, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    IDXGISwapChain2* sc2 = NULL;
    HANDLE waitable = NULL;
    if (SUCCEEDED(IDXGISwapChain1_QueryInterface(sc, &IID_IDXGISwapChain2, (void**)&sc2))) {
        IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
        waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
    }
    IDXGISwapChainMedia* media = NULL;
    IDXGISwapChain1_QueryInterface(sc, &IID_IDXGISwapChainMedia, (void**)&media);

    for (const char* ph = phases; *ph; ph++) {
        int windowed = *ph == 'w', tear = *ph == 't', excl = *ph == 'x';
        if (windowed) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left + mw / 6, mi.rcMonitor.top + mh / 6, mw * 2 / 3, mh * 2 / 3, SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, SWP_FRAMECHANGED);
        }
        bring_front(hwnd);
        pump();
        if (excl && (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {   /* (exclusive fullscreen refuses tearing swap chains) */
            if (media) IDXGISwapChainMedia_Release(media);
            if (sc2) IDXGISwapChain2_Release(sc2);
            IDXGISwapChain1_Release(sc);
            ID3D11DeviceContext_ClearState(ctx);
            ID3D11DeviceContext_Flush(ctx);
            media = NULL; sc2 = NULL; sc = NULL; waitable = NULL;
            d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)dev, hwnd, &d, NULL, NULL, &sc))) { printf("no swap chain\n"); return 1; }
            if (SUCCEEDED(IDXGISwapChain1_QueryInterface(sc, &IID_IDXGISwapChain2, (void**)&sc2))) {
                IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
                waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
            }
            IDXGISwapChain1_QueryInterface(sc, &IID_IDXGISwapChainMedia, (void**)&media);
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        IDXGISwapChain1_ResizeBuffers(sc, 0, (UINT)rc.right, (UINT)rc.bottom, DXGI_FORMAT_UNKNOWN, d.Flags);
        if (excl) {
            HRESULT hr = IDXGISwapChain1_SetFullscreenState(sc, TRUE, NULL);
            pump();
            IDXGISwapChain1_ResizeBuffers(sc, 0, (UINT)mw, (UINT)mh, DXGI_FORMAT_UNKNOWN, d.Flags);
            if (FAILED(hr)) printf("exclusive: SetFullscreenState failed 0x%08lX\n", (unsigned long)hr);
        }
        double t_end = now_ms() + secs * 1000.0, next = now_ms();
        enum { N = 4096 };
        static double shown[N];
        static UINT shown_pc[N];
        int ns = 0, comp[5] = { 0 }, frames = 0, fg_frames = 0;
        UINT last = 0;
        while (now_ms() < t_end) {
            pump();
            if (waitable) WaitForSingleObject(waitable, 100);
            while (now_ms() < next) { double l = next - now_ms(); if (l > 1.5) Sleep(1); else SwitchToThread(); }
            next += cadence;
            if (now_ms() > next) next = now_ms();
            ID3D11Texture2D* bb = NULL;
            ID3D11RenderTargetView* rtv = NULL;
            IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&bb);
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)bb, NULL, &rtv);
            float g = (float)(frames % 64) / 64.0f, col[4] = { 0.1f, g * 0.4f, 0.2f, 1 };
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
            ID3D11RenderTargetView_Release(rtv);
            ID3D11Texture2D_Release(bb);
            IDXGISwapChain1_Present(sc, tear ? 0 : 1, tear && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
            frames++;
            fg_frames += GetForegroundWindow() == hwnd;
            DXGI_FRAME_STATISTICS fs = { 0 };
            if (SUCCEEDED(IDXGISwapChain1_GetFrameStatistics(sc, &fs)) && fs.PresentCount && fs.PresentCount != last && ns < N) {
                shown_pc[ns] = fs.PresentCount;
                shown[ns++] = (double)fs.SyncQPCTime.QuadPart * 1000.0 / (double)qpf.QuadPart;
                last = fs.PresentCount;
            }
            if (media) {
                DXGI_FRAME_STATISTICS_MEDIA fm = { 0 };
                if (SUCCEEDED(IDXGISwapChainMedia_GetFrameStatisticsMedia(media, &fm)) && fm.CompositionMode >= 0 && fm.CompositionMode < 4)
                    comp[fm.CompositionMode]++;
                else comp[4]++;
            }
        }
        if (excl) IDXGISwapChain1_SetFullscreenState(sc, FALSE, NULL);
        /* intervals between consecutive present counts, skipping the first second */
        static double iv[N];
        int n = 0;
        for (int i = 1; i < ns; i++)
            if (shown_pc[i] == shown_pc[i - 1] + 1 && shown[i] - shown[0] > 1000.0) iv[n++] = shown[i] - shown[i - 1];
        double grid = 0, sum = 0;
        for (int i = 0; i < n; i++) {
            double r = iv[i] / refresh;
            grid += fabs(r - floor(r + 0.5)) < 0.08;
            sum += iv[i];
        }
        qsort(iv, (size_t)n, sizeof iv[0], cmpd);
        const char* name = windowed ? "windowed" : excl ? "exclusive" : tear ? "borderless+tearing" : "borderless";
        int cm = 4, best = 0;
        for (int k = 0; k < 5; k++) if (comp[k] > best) { best = comp[k]; cm = k; }
        printf("%-19s presents %d, foreground %d%%, shown %d intervals: p10 %.2f median %.2f p90 %.2f mean %.2f ms, on the %.2f ms grid %.0f%% -> %s; "
               "DXGI composition: %s\n", name, frames, frames ? 100 * fg_frames / frames : 0, n, n ? iv[n / 10] : 0, n ? iv[n / 2] : 0,
               n ? iv[n * 9 / 10] : 0, n ? sum / n : 0, refresh, n ? 100.0 * grid / n : 0,
               n < 20 ? "no statistics" : grid / n > 0.8 ? "FIXED refresh" : "VARIABLE refresh (VRR active)", media ? (cm < 4 ? comp_name(cm) : "n/a") : "no IDXGISwapChainMedia");
        fflush(stdout);
    }
    DestroyWindow(hwnd);
    return 0;
}
