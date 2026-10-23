/* The Options menu's picture on the CPU (menu_draw.c): premultiplied BGRA (A << 24 | R << 16 | G << 8 | B), at the
 * output's size. Callers serialise (menu_overlay.c holds its lock). */
#pragma once
#include <stdint.h>

/* The menu over a transparent background (w x h, stride in pixels). 0 if the menu is closed (all transparent). */
int  menu_draw(uint32_t* px, int w, int h, int stride);
/* The frame rate counter (a small w x h box); u = the output's scale (height / 1080). */
void menu_draw_fps(uint32_t* px, int w, int h, int stride, double fps, float u);
