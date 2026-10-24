/* The game's own font for the PC menus (notes/pc-menu.md "How it is drawn").
 *
 * Sly 2's font is a vector font: one object in the level data (found in EE RAM at run time: nothing of the disc is
 * stored in the repository or the executable) with a glyph per character, each glyph a DMA chain of VIF codes for the
 * game's 2D VU1 program - outlines as triangle fans and strips that the game fills with a stencil-then-cover pass (the
 * triangles add +-2 to the green channel, the result is read back through a CLUT: the nonzero winding rule).
 * slyf_extract turns the glyphs into signed triangles (font units: 1/16 of the VU's integer coordinates, y down, the
 * baseline at 0), slyf_raster fills them into a coverage atlas with mipmaps (4 x 4 samples per pixel, nonzero rule),
 * which every renderer draws as textured quads: crisp at any output size.
 *
 * Before the game's font has been seen (start-up, or a run without the game: the unit tests) a Windows font is drawn
 * into an atlas of the same format (slyf_system), so the menu always has text. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SLYF_EM 60.0f                    /* font units of one "em" (ascent + descent of the game's font, ~46 + 13) */

typedef struct {
    int16_t ax, ay, aw, ah;              /* the glyph's rectangle in the atlas (level 0 pixels) */
    float ox, oy;                        /* its top-left corner relative to the pen (font units, y down) */
    float adv;                           /* advance (font units) */
    uint8_t present;                     /* drawn (a space has an advance and no picture) */
} slyf_glyph;

#define SLYF_LEVELS 8
typedef struct {
    int from_game;                       /* 1: the game's own font; 0: the Windows stand-in */
    int levels;
    int w[SLYF_LEVELS], h[SLYF_LEVELS];  /* the atlas and its mipmaps (R8 coverage, 0..255) */
    uint8_t* px[SLYF_LEVELS];
    float units_px;                      /* level-0 pixels per font unit */
    float ascent, descent;               /* font units above / below the baseline */
    slyf_glyph g[256];                   /* by character (Windows-1252) */
} slyf_atlas;

/* ---- the game's font in EE RAM ---- */
#define SLYF_MAX_TRIS 24000
typedef struct { float x[3], y[3]; int8_t w; uint8_t ch; } slyf_tri;   /* a triangle, its winding weight, its char */
typedef struct {
    int ntri;
    slyf_tri* tri;                       /* SLYF_MAX_TRIS */
    float adv[256];
    uint8_t has[256];                    /* the character is in the font */
} slyf_mesh;

/* The font object in EE RAM (ram, size bytes): 1 and its address. A font: +0x20 count (20..256), +0x24 its sorted
 * character table, +0x14 the glyphs (0x40 bytes, the glyph of table entry i at +0x40 * (i + 1)), +0x1C the advances. */
int slyf_find(const uint8_t* ram, uint32_t size, uint32_t* addr);
/* The glyphs' triangles (m->tri must hold SLYF_MAX_TRIS); 0 if anything does not parse as a glyph. */
int slyf_extract(const uint8_t* ram, uint32_t size, uint32_t font, slyf_mesh* m);
/* Fill the triangles into an atlas at units_px pixels per font unit (0 on out of memory). */
int slyf_raster(const slyf_mesh* m, float units_px, slyf_atlas* out);
/* The stand-in: a Windows font in the same format. */
int slyf_system(float units_px, slyf_atlas* out);
void slyf_free(slyf_atlas* a);

/* ---- the font the menus use (thread-safe; atlases once published stay valid for the process) ---- */
const slyf_atlas* slyf_current(void);    /* the game's once it is known, else the stand-in (made on first use) */
uint32_t slyf_serial(void);              /* changes when slyf_current changes (renderers re-upload) */
void slyf_publish(slyf_atlas* a);        /* (takes ownership) */
/* The EE thread, once per game frame (menu_game.c): looks for the game's font (every second until it is found),
 * extracts it there (the game is between frames: its memory is consistent) and fills the atlas on a worker thread. */
void slyf_ee_poll(const uint8_t* ram, uint32_t size);

/* ---- text ---- */
/* the width of s at px pixels per em (Windows-1252 bytes; UTF-8 input is mapped where it can be) */
float slyf_text_width(const slyf_atlas* a, const char* s, float px);
/* UTF-8 -> Windows-1252 (unknown characters: '?'); returns the length */
int slyf_cp1252(const char* utf8, char* out, int cap);
