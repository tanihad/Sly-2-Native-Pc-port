/* The Options menu's pacing interface (menu_ext.h; pc-pacing): these strong definitions replace menu_stubs.c's weak ones. */
#include "menu_ext.h"
#include "display_mode.h"
#include "gfx.h"
#include "settings.h"

void* window_hwnd(int wait_ms);          /* window.c */

/* graphics.pacing / graphics.vsync are live: the presenter (gpu_present_set_mode) and the display clock (gpu_hw.c) take
 * them from the next picture. -1: only asks. */
int pacing_apply_mode(int pacing) {
    if (pacing < 0) return 1;
    gfx_apply_pacing();                  /* (reads ps2_settings.pacing / vsync, which the settings table has stored) */
    return 1;
}

/* exclusive fullscreen: called by window.c once the window is fullscreen (on) or before it leaves exclusive (off). The
 * window stays a popup over the monitor (DXGI flip model: independent flip = exclusive's latency); "exclusive" means the
 * game owns the display's mode: a refresh that is a multiple of 59.94 Hz (120 Hz on a 165 Hz display) while it is in
 * front, so every picture lasts the same number of refreshes. */
int pacing_exclusive_fullscreen(int on) {
    return display_mode_exclusive(window_hwnd(0), on);
}
