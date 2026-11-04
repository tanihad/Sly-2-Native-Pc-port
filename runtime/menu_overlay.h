/* The Options menu (and the frame rate counter) on the screen, over the game's picture, for every presenter
 * (menu_overlay.c): the GPU renderer's present pass (gpu_present.c, before Present), the software renderer's
 * window presenter (window.c d3d_render, before Present) and its GDI fallback (window.c gdi_render). The pictures
 * come from menu_draw.c (CPU, premultiplied BGRA at the output's size) and are redrawn / uploaded only when the
 * menu changes. Any thread (one presenter at a time). */
#pragma once
#include <stdint.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

/* Draw the overlay over rtv (w x h, the back buffer) with this device; restores the pipeline state it changes. */
void overlay_d3d11_draw(struct ID3D11Device* dev, struct ID3D11DeviceContext* ctx, struct ID3D11RenderTargetView* rtv,
                        int w, int h);
/* The device is going away (release, device loss): drop the overlay's objects made with it. */
void overlay_d3d11_release(struct ID3D11Device* dev);
/* GDI presenter: blend the overlay onto dc (client size w x h). */
void overlay_gdi_draw(void* hdc, int w, int h);
/* The frame rate for the counter (window.c, once a second). */
void overlay_set_fps(double fps);
/* Write the next presented picture with the overlay to path (PNG; tests and screenshots: PS2_INPUT "menushot"). */
void overlay_request_shot(const char* path);
/* (pc-shot) F12 / PS2_INPUT "screenshot": the next presented picture, as shown, to %USERPROFILE%\Pictures\Sly 2\
 * Sly2_YYYY-MM-DD_HH-MM-SS.png (PS2_SCREENSHOT_DIR: another folder), written on a worker thread; "Screenshot saved"
 * shows for 1.5 s afterwards. Any thread. */
void overlay_request_screenshot(void);
/* (pc-menu2) the overlay changes between the game's pictures (the menu is open: the mouse, animations): the presenters
 * present at the display's refresh meanwhile (gpu_present.c gpu_present_menu, window.c) */
int  overlay_wants_frames(void);
/* menu pictures drawn so far (measurements: PS2_MENU_STATS) */
long overlay_draw_count(void);
