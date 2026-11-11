/* The GPU the game draws with (notes/d3d11-renderer.md 2.1, "Laptops with two GPUs").
 *
 * D3D11CreateDevice(NULL, HARDWARE) takes Windows' default adapter: on a laptop with integrated + discrete graphics
 * (NVIDIA Optimus, AMD switchable) that is usually the integrated GPU, which drives the built-in screen. The game
 * then ran there (2026-11-11, owner's RTX 5070 laptop: slow, and pixelated at any setting because the scale
 * governor kept the internal resolution at its floor). So both device starts (gpu/gpu_d3d11.c, window.c) ask DXGI
 * for the HIGH_PERFORMANCE adapter (IDXGIFactory6, Windows 10 1803+), skipping the Microsoft Basic Render Driver,
 * and window.c also exports NvOptimusEnablement / AmdPowerXpressRequestHighPerformance for the drivers. Both
 * starts must agree, since the renderer and the window may share one window.
 *
 * Returns the adapter (caller releases) or NULL = the default adapter (no IDXGIFactory6, no hardware adapter, or
 * PS2_GPU_PREFER=default). With an adapter, D3D11CreateDevice takes D3D_DRIVER_TYPE_UNKNOWN. */
#ifndef PS2_GPU_ADAPTER_H
#define PS2_GPU_ADAPTER_H

#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dxgi1_6.h>

static inline IDXGIAdapter* ps2_pick_adapter(void) {
    const char* e = getenv("PS2_GPU_PREFER");
    if (e && !_stricmp(e, "default")) return NULL;
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    typedef HRESULT (WINAPI * create_fn)(UINT, REFIID, void**);
    create_fn create = m ? (create_fn)(void*)GetProcAddress(m, "CreateDXGIFactory2") : NULL;
    static const IID iid_factory6 = { 0xc1b6694f, 0xff09, 0x44a9, { 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17 } };
    static const IID iid_adapter = { 0x2411e7e1, 0x12ac, 0x4ccf, { 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0 } };
    IDXGIFactory6* f = NULL;
    if (!create || FAILED(create(0, &iid_factory6, (void**)&f))) return NULL;
    IDXGIAdapter* pick = NULL;
    for (UINT i = 0; !pick && i < 16; i++) {
        IDXGIAdapter* a = NULL;
        if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(f, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &iid_adapter,
                                                            (void**)&a)))
            break;
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(a, &d)) && d.VendorId == 0x1414 && d.DeviceId == 0x8c) {
            IDXGIAdapter_Release(a);     /* Microsoft Basic Render Driver (software) */
            continue;
        }
        pick = a;
    }
    IDXGIFactory6_Release(f);
    return pick;
}

#endif
