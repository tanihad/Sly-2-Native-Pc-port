/* (pc-fps) What the system says about variable refresh (G-Sync / FreeSync) for the game's display, for the log and the
 * Options menu - read only, nothing is changed:
 *  - NVIDIA: nvapi64.dll (installed with the driver) - the driver profile's G-SYNC mode (global: off / full screen only /
 *    windowed and full screen; the game's own profile if the player made one) and the display's VRR state
 *    (NvAPI_Disp_GetVRRInfo: enabled, possible, in VRR mode now). Function ids from NVIDIA's public NVAPI SDK
 *    (nvapi_interface.h); a driver without one of them simply answers "unknown".
 *  - DXGI: how the swap chain's presents reach the display (IDXGISwapChainMedia: composed by DWM / overlay or independent
 *    flip), and whether the game's window is in front.
 * notes/frame-rate.md "VRR". */
#include "gpu_internal.h"
#include "gpu_vrr.h"

#include <stdint.h>
#include <string.h>

typedef void* (*nv_qi_fn)(uint32_t id);
static struct {
    int tried, ok;
    HMODULE dll;
    int (*init)(void);
    int (*by_name)(const char* name, uint32_t* id);
    int (*vrr_info)(uint32_t id, void* info);
    int (*drs_create)(void** s);
    int (*drs_destroy)(void* s);
    int (*drs_load)(void* s);
    int (*drs_global)(void* s, void** p);
    int (*drs_get)(void* s, void* p, uint32_t id, void* setting);
    int (*drs_find_app)(void* s, const uint16_t* name, void** p, void* app);
} nv;

static int nv_load(void) {
    if (nv.tried) return nv.ok;
    nv.tried = 1;
    nv.dll = LoadLibraryExW(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    nv_qi_fn qi = nv.dll ? (nv_qi_fn)(void*)GetProcAddress(nv.dll, "nvapi_QueryInterface") : NULL;
    if (!qi) return 0;
    nv.init = (int (*)(void))qi(0x0150E828);                              /* NvAPI_Initialize */
    nv.by_name = (int (*)(const char*, uint32_t*))qi(0xAE457190);         /* NvAPI_DISP_GetDisplayIdByDisplayName */
    nv.vrr_info = (int (*)(uint32_t, void*))qi(0xDF8FDA57);               /* NvAPI_Disp_GetVRRInfo */
    nv.drs_create = (int (*)(void**))qi(0x0694D52E);                     /* NvAPI_DRS_CreateSession */
    nv.drs_destroy = (int (*)(void*))qi(0xDAD9CFF8);                     /* NvAPI_DRS_DestroySession */
    nv.drs_load = (int (*)(void*))qi(0x375DBD6B);                        /* NvAPI_DRS_LoadSettings */
    nv.drs_global = (int (*)(void*, void**))qi(0x617BFF9F);              /* NvAPI_DRS_GetCurrentGlobalProfile */
    nv.drs_get = (int (*)(void*, void*, uint32_t, void*))qi(0x73BF8338);  /* NvAPI_DRS_GetSetting */
    nv.drs_find_app = (int (*)(void*, const uint16_t*, void**, void*))qi(0xEEE566B2);   /* NvAPI_DRS_FindApplicationByName */
    nv.ok = nv.init && nv.init() == 0;
    return nv.ok;
}

/* NVDRS_SETTING_V1 (u32 values only) */
typedef struct {
    uint32_t version;
    uint16_t name[2048];
    uint32_t id, type, location, is_cur_predef, is_predef_valid;
    union { uint32_t u32; uint8_t bin[4100]; } predef, cur;
} nv_setting;

#define NV_VRR_MODE 0x1194F158u          /* "Enable G-SYNC globally" / application mode: 0 off, 1 full screen, 2 + windowed */
#define NV_VRR_APP_OVERRIDE 0x10A879CFu  /* "G-SYNC" (Manage 3D settings > Monitor Technology): 0 allow, else off / fixed */

static int drs_u32(void* s, void* p, uint32_t id, uint32_t* v) {
    static nv_setting st;
    memset(&st, 0, sizeof st);
    st.version = (uint32_t)sizeof st | 1u << 16;
    if (!nv.drs_get || nv.drs_get(s, p, id, &st) != 0 || st.type != 0) return 0;
    *v = st.cur.u32;
    return 1;
}

int gpu_vrr_query(void* hwnd, gpu_vrr_info* out) {
    memset(out, 0, sizeof *out);
    out->vendor_known = 0;
    out->mode = out->app_override = -1;
    out->enabled = out->possible = out->active = out->requested = out->indicator = -1;
    if (!nv_load()) return 0;
    out->vendor_known = 1;
    /* the driver profile: the global G-SYNC mode, the game's own profile if any */
    void* s = NULL;
    if (nv.drs_create && nv.drs_load && nv.drs_global && nv.drs_create(&s) == 0 && s) {
        void* p = NULL;
        uint32_t v;
        if (nv.drs_load(s) == 0 && nv.drs_global(s, &p) == 0) {
            if (drs_u32(s, p, NV_VRR_MODE, &v)) out->mode = (int)v;
            if (drs_u32(s, p, NV_VRR_APP_OVERRIDE, &v)) out->app_override = (int)v;
            wchar_t exe[MAX_PATH];
            DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
            if (n && n < MAX_PATH && nv.drs_find_app) {
                static uint8_t app[16384];        /* NVDRS_APPLICATION_V1 */
                memset(app, 0, sizeof app);
                *(uint32_t*)app = (uint32_t)(4 + 4 + 4096 * 3) | 1u << 16;
                const wchar_t* base = wcsrchr(exe, L'\\');
                void* ap = NULL;
                if (nv.drs_find_app(s, (const uint16_t*)(base ? base + 1 : exe), &ap, app) == 0 && ap) {
                    out->app_profile = 1;
                    if (drs_u32(s, ap, NV_VRR_MODE, &v)) out->mode = (int)v;
                    if (drs_u32(s, ap, NV_VRR_APP_OVERRIDE, &v)) out->app_override = (int)v;
                }
            }
        }
        if (nv.drs_destroy) nv.drs_destroy(s);
    }
    /* the display's state */
    MONITORINFOEXA mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    uint32_t id = 0;
    if (hwnd && nv.by_name && nv.vrr_info && GetMonitorInfoA(MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi) &&
        nv.by_name(mi.szDevice, &id) == 0) {
        uint32_t info[6];                /* NV_GET_VRR_INFO_V1 */
        memset(info, 0, sizeof info);
        info[0] = (uint32_t)sizeof info | 1u << 16;
        if (nv.vrr_info(id, info) == 0) {
            out->raw = info[1];
            out->enabled = (int)(info[1] & 1);
            out->possible = (int)(info[1] >> 1 & 1);
            out->requested = (int)(info[1] >> 2 & 1);
            out->indicator = (int)(info[1] >> 3 & 1);
            out->active = (int)(info[1] >> 4 & 1);
        }
    }
    return 1;
}

const char* gpu_vrr_describe(const gpu_vrr_info* v, char* buf, int cap) {
    if (!v->vendor_known) { snprintf(buf, (size_t)cap, "not an NVIDIA driver (no driver details)"); return buf; }
    static const char* const modes[] = { "off", "full screen only", "windowed and full screen" };
    /* exactly what the driver reports: the display's NV_GET_VRR_INFO flags and the profile's G-SYNC settings */
    char disp[160];
    if (v->enabled < 0) snprintf(disp, sizeof disp, "NvAPI_Disp_GetVRRInfo: no answer");
    else snprintf(disp, sizeof disp, "NvAPI_Disp_GetVRRInfo: enabled %d, possible %d, requested %d, indicator %d, in VRR mode %d (flags 0x%X)",
                  v->enabled, v->possible, v->requested, v->indicator, v->active, v->raw);
    snprintf(buf, (size_t)cap, "%s; driver profile%s: G-SYNC mode %d (%s), Monitor Technology %d (%s) -> %s", disp,
             v->app_profile ? " (the game's own)" : " (global)", v->mode, v->mode >= 0 && v->mode <= 2 ? modes[v->mode] : "unknown",
             v->app_override, v->app_override == 0 ? "G-SYNC allowed" : v->app_override > 0 ? "not G-SYNC" : "unknown",
             v->enabled < 0 ? "state unknown" : v->enabled && v->possible ? (v->active > 0 ? "variable refresh in use now" : "variable refresh enabled") :
             v->possible ? "variable refresh possible but not enabled for this display" :
             "variable refresh not enabled for this display (the driver says it is not possible)");
    return buf;
}

static volatile LONG st_code;
static char st_text[96];
static SRWLOCK st_lock = SRWLOCK_INIT;
static gpu_vrr_info st_drv;
static int st_have_drv;

void gpu_vrr_set_status(const gpu_vrr_info* drv, int disp_state, double hz) {
    AcquireSRWLockExclusive(&st_lock);
    if (drv) { st_drv = *drv; st_have_drv = 1; }
    int code = GPU_VRR_UNKNOWN;
    char t[96] = "Unknown (measuring...)";
    int drv_off = st_have_drv && st_drv.vendor_known && (st_drv.enabled == 0 || st_drv.possible == 0);
    if (disp_state == 1 /* PACE_DISP_VRR */ || (st_have_drv && st_drv.active > 0)) { code = GPU_VRR_ACTIVE; snprintf(t, sizeof t, "Active"); }
    else if (drv_off) {
        code = GPU_VRR_OFF_DRIVER;
        snprintf(t, sizeof t, st_drv.possible == 0 ? "Off in the driver / monitor" : "Off in the driver (G-SYNC not enabled)");
    } else if (disp_state == 2 /* PACE_DISP_FIXED */) { code = GPU_VRR_NOT_ACTIVE; snprintf(t, sizeof t, "Not active (fixed %.0f Hz)", hz); }
    else if (disp_state == 3 /* PACE_DISP_MULTIPLE */) { code = GPU_VRR_NOT_NEEDED; snprintf(t, sizeof t, "Not needed (%.0f Hz display)", hz); }
    else if (st_have_drv && st_drv.vendor_known && st_drv.enabled > 0) { code = GPU_VRR_UNKNOWN; snprintf(t, sizeof t, "Enabled in the driver"); }
    memcpy(st_text, t, sizeof t);
    InterlockedExchange(&st_code, code);
    ReleaseSRWLockExclusive(&st_lock);
}

int gpu_vrr_status(char* buf, int cap) {
    AcquireSRWLockShared(&st_lock);
    snprintf(buf, (size_t)cap, "%s", st_text[0] ? st_text : "Unknown (measuring...)");
    int c = (int)st_code;
    ReleaseSRWLockShared(&st_lock);
    return c;
}
const char* gpu_vrr_advice(const gpu_vrr_info* v) {
    if (!v->vendor_known) return "turn on FreeSync / Adaptive-Sync in the graphics driver and in the monitor's own menu";
    if (v->enabled == 0 || v->possible == 0)
        return "the driver has variable refresh off for this display: turn on FreeSync / Adaptive-Sync in the monitor's own menu, then in "
               "NVIDIA Control Panel > Display > Set up G-SYNC tick 'Enable G-SYNC, G-SYNC Compatible', choose 'Enable for windowed "
               "and full screen mode', select this display and tick 'Enable settings for the selected display model' if shown, Apply";
    if (v->mode == 0) return "G-SYNC is off: NVIDIA Control Panel > Display > Set up G-SYNC > Enable G-SYNC, G-SYNC Compatible";
    if (v->mode == 1) return "G-SYNC is set to full screen only: the game's borderless / windowed modes need 'Enable for windowed and full "
                             "screen mode' (NVIDIA Control Panel > Display > Set up G-SYNC)";
    if (v->app_override > 0) return "Manage 3D settings > Monitor Technology is not G-SYNC Compatible for this program";
    return NULL;
}

/* DXGI: the composition mode of the last presents */
static const IID iid_media = { 0xdd95b90b, 0xf05f, 0x4f6a, { 0xbd, 0x65, 0x25, 0xbf, 0xb2, 0x64, 0xbd, 0x84 } };
int gpu_vrr_composition(void* swapchain) {
    if (!swapchain) return -1;
    IDXGISwapChainMedia* m = NULL;
    if (FAILED(IUnknown_QueryInterface((IUnknown*)swapchain, &iid_media, (void**)&m)) || !m) return -1;
    DXGI_FRAME_STATISTICS_MEDIA fm;
    memset(&fm, 0, sizeof fm);
    int r = SUCCEEDED(IDXGISwapChainMedia_GetFrameStatisticsMedia(m, &fm)) ? (int)fm.CompositionMode : -1;
    IDXGISwapChainMedia_Release(m);
    return r;
}

const char* gpu_vrr_composition_name(int mode) {
    switch (mode) {
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: return "composed by the desktop (DWM)";
    case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: return "independent flip / overlay";
    case DXGI_FRAME_PRESENTATION_MODE_NONE: return "direct";
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: return "composition failure";
    default: return "unknown";
    }
}
