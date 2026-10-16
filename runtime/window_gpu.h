/* The game window for a renderer that presents by itself: the Direct3D 11 GS backend's render thread
 * owns a flip-model swap chain on the window (notes/d3d11-renderer.md 2.1, 8.2; runtime/gpu/gpu_hw.c).
 * window.c then creates no device and draws nothing; it keeps the window, input, Alt+Enter / F11
 * borderless fullscreen and the remembered placement, and tells the renderer its client size. */
#pragma once
#include <stdint.h>

/* Pictures given to window_present / window_present_aspect go to fn instead of the window's own
 * presenter (software pictures after a live backend switch; NULL: dropped). on = 0: the window's own
 * presenter again (Direct3D 11 or GDI), created on the window thread at its next turn - only once the
 * external renderer has released its swap chain. Call before window_init to start without a presenter. */
typedef void (*window_present_fn)(const uint32_t* bgrx, int w, int h, int stride, int aspect_w, int aspect_h);
void window_set_external_presenter(int on, window_present_fn fn);
int  window_external_presenter(void);

void* window_hwnd(int wait_ms);           /* the window once it exists (waits up to wait_ms), else NULL */
void window_client_size(int* w, int* h);  /* client area as of the last WM_SIZE (0 x 0: minimised); any thread */
int  window_fullscreen(void);             /* borderless fullscreen now */
uint32_t window_resize_serial(void);      /* bumped on every client size change */
void window_note_presented(void);         /* a picture was shown (the title's frame rate) */

/* Debug keys of the GPU backend, enabled with PS2_GS_DEBUG_KEYS=1: F9 switches between the GPU and the
 * software renderer, Shift+F9 simulates a device loss. Presses since the last call (bit 0: F9,
 * bit 1: Shift+F9); any thread. */
enum { WINDOW_KEY_SWITCH = 1, WINDOW_KEY_LOSE_DEVICE = 2 };
int  window_take_debug_keys(void);
