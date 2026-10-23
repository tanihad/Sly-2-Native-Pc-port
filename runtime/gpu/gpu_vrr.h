/* (pc-fps) variable refresh (G-Sync / FreeSync): what the driver and DXGI say - read only (gpu_vrr.c). */
#pragma once

typedef struct {
    int vendor_known;                    /* 1: NVIDIA (nvapi64.dll answered); 0: no details (AMD / Intel / no driver) */
    int mode;                            /* driver profile G-SYNC mode: 0 off, 1 full screen only, 2 windowed and full screen; -1 ? */
    int app_override;                    /* Monitor Technology: 0 = G-SYNC allowed, > 0 fixed refresh / forced off; -1 ? */
    int app_profile;                     /* the game's exe has its own driver profile */
    int enabled, possible, active;       /* the display: VRR enabled, possible, in VRR mode now (1 / 0, -1 unknown) */
    int requested, indicator;            /* NV_GET_VRR_INFO bIsVRRRequested, bIsVRRIndicatorEnabled (-1 unknown) */
    unsigned raw;                        /* NV_GET_VRR_INFO's flag word as the driver returned it */
} gpu_vrr_info;

/* fills *out (any thread; a few ms: call when the window or display changes, not per frame); 1 if the driver answered */
int gpu_vrr_query(void* hwnd, gpu_vrr_info* out);
/* one line for the log ("NVIDIA G-SYNC: windowed and full screen; this display: variable refresh not enabled ...") */
const char* gpu_vrr_describe(const gpu_vrr_info* v, char* buf, int cap);
/* what the player can do when variable refresh is not active (NULL: nothing known to be off) */
const char* gpu_vrr_advice(const gpu_vrr_info* v);
/* the swap chain's composition mode now (DXGI_FRAME_PRESENTATION_MODE_*, -1 unknown) and its name */
int gpu_vrr_composition(void* swapchain);
const char* gpu_vrr_composition_name(int mode);

/* ---- status for the Options menu (any thread) ---- */
enum { GPU_VRR_UNKNOWN, GPU_VRR_ACTIVE, GPU_VRR_OFF_DRIVER, GPU_VRR_NOT_ACTIVE, GPU_VRR_NOT_NEEDED };
/* render thread: the driver's view (gpu_vrr_query result, NULL = keep) and the display classification (PACE_DISP_*, the
 * refresh in Hz) -> the status */
void gpu_vrr_set_status(const gpu_vrr_info* drv, int disp_state, double refresh_hz);
/* GPU_VRR_* and a short text ("Active", "Off in the driver / monitor", "Not active (fixed 165 Hz)") */
int gpu_vrr_status(char* buf, int cap);