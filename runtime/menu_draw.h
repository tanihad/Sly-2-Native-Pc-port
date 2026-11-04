/* The Options menu's picture (menu_draw.c), as a list of primitives - boxes, capsules, arrows and glyphs of the game's
 * own font (sly_font.h) - in output pixels. The GPU presenters draw the list with a few hundred quads at every present
 * (menu_overlay.c); menu_draw rasterises the same list on the CPU (premultiplied BGRA, A << 24 | R << 16 | G << 8 | B)
 * for the GDI presenter and the unit tests' pictures. Callers serialise (menu_overlay.c holds its lock). */
#pragma once
#include <stdint.h>

enum { UI_GLYPH, UI_BOX, UI_CAPSULE, UI_ARROW };
typedef struct {
    uint8_t kind, clip;                  /* UI_*, clip rectangle (0: none) */
    float x, y, w, h;                    /* the quad, output pixels */
    float u0, v0, u1, v1;                /* GLYPH: the atlas rectangle (0..1) */
    uint32_t c0, c1, c2;                 /* 0xAARRGGBB, straight: fill (BOX: top), BOX: bottom, border / GLYPH: outline */
    float p[4];                          /* BOX: radius, border width; GLYPH: outline width (px); CAPSULE: a.x, a.y, b.x,
                                            b.y (px) and width in u0; ARROW: direction (1 right, -1 left) */
} ui_prim;

#define UI_MAX_PRIMS 12000
#define UI_MAX_CLIPS 8
typedef struct {
    ui_prim* p;                          /* UI_MAX_PRIMS */
    int n;
    int clip[UI_MAX_CLIPS][4];           /* x, y, w, h (clip 0 unused) */
    int nclip;
    int w, h;
    uint32_t font_serial;                /* the atlas the glyph coordinates are for (slyf_serial) */
} ui_list;

/* Build the menu's picture for an output of w x h at time now_s (seconds; animations), plus the frame rate counter when
 * fps >= 0. Returns the number of primitives (0: nothing to draw). */
int  menu_build(ui_list* L, int w, int h, double now_s, double fps);
/* (pc-shot) a short notice at the bottom of the picture (NULL: none); set before menu_build, same serialisation. */
/* The caller decides when it shows (menu_overlay.c: 1.5 s after a screenshot). */
void menu_set_toast(const char* text);
/* The list on the CPU over a transparent background. */
void ui_raster(const ui_list* L, uint32_t* px, int w, int h, int stride);

/* The menu over a transparent background (w x h, stride in pixels). 0 if the menu is closed (all transparent). */
int  menu_draw(uint32_t* px, int w, int h, int stride);
/* The frame rate counter (a small w x h box); u = the output's scale (height / 1080). */
void menu_draw_fps(uint32_t* px, int w, int h, int stride, double fps, float u);
