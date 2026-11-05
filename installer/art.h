/* art.c: the pictures of the launcher and the game's icon (notes/disc-art.md).
 *
 * Where they come from, first match wins:
 *   1. the owner's own files in <user data>\art (%LOCALAPPDATA%\Sly2\art): icon.ico, icon.png|webp|jpg|bmp,
 *      logo.png|webp (the game's logo: its raccoon mask becomes the icon, the whole logo the launcher's title),
 *      backdrop.png|jpg|webp|bmp (the launcher's background)
 *   2. art made from the player's disc by the build (scripts/disc_art.py): <dir>\game\art\sly2.ico, backdrop.bmp
 *   3. our own: the launcher's built-in icon (scripts/make_icon.py) and its drawn dark theme
 * Nothing of 1 or 2 is ever part of the repository, Setup or a source bundle: they exist only on the player's PC.
 */
#pragma once
#include "setup.h"

typedef struct { int w, h; uint32_t* px; } art_img;   /* premultiplied BGRA, top-down rows, w*h pixels */

int art_load(const wchar_t* path, art_img* out);      /* WIC: PNG, JPEG, BMP, GIF, TIFF, ICO; WebP if Windows has the codec */
void art_free(art_img* a);
int art_alloc(art_img* a, int w, int h);              /* zeroed (transparent) */
/* the source rectangle (sx, sy, sw, sh: may be fractional) resampled into dst (w, h set, px allocated):
 * tent filter = bilinear when enlarging, an area average when shrinking */
void art_resample(const art_img* src, double sx, double sy, double sw, double sh, art_img* dst);
/* dst = src over dst at (x, y) with an extra opacity */
void art_blend(art_img* dst, const art_img* src, int x, int y, double opacity);

/* anti-aliased shapes in premultiplied colours (0xAARRGGBB with straight alpha in `argb`) */
void art_rrect(art_img* d, double x, double y, double w, double h, double r, uint32_t argb);
void art_rrect_v(art_img* d, double x, double y, double w, double h, double r, uint32_t top, uint32_t bottom);
void art_rrect_ring(art_img* d, double x, double y, double w, double h, double r, double width, uint32_t argb);
void art_line(art_img* d, double x0, double y0, double x1, double y1, double width, uint32_t argb);
void art_glow(art_img* d, double cx, double cy, double rx, double ry, uint32_t argb);   /* soft elliptical light */
/* a horizontal ramp of black from opacity a0 at x0 to a1 at x1 (constant outside), and the same vertically */
void art_shade_h(art_img* d, int x0, int x1, double a0, double a1);
void art_shade_v(art_img* d, int y0, int y1, double a0, double a1);

/* <dir>\<base>.<ext> for the first of png, webp, jpg, jpeg, bmp that exists */
int art_find(const wchar_t* dir, const wchar_t* base, wchar_t* out);
/* the owner's art folder: <user data>\art */
void art_user_dir(setup_t* s, wchar_t* out);

/* The logo's raccoon mask cut out of it (the official logo's shape; 0 if this picture is not that logo) */
int art_logo_mask(const art_img* logo, art_img* out, int width);

/* <dir>\art\sly2.ico from the best source (above). 1 = it changed (shortcuts and the Apps & features entry
 * should be pointed at it again), 0 = unchanged or nothing to do. *what: "owner", "disc" or "" (none). */
int art_icon_refresh(setup_t* s, const wchar_t** what);
/* the icon for shortcuts and Apps & features: <dir>\art\sly2.ico if there is one, else the launcher */
void art_icon_location(setup_t* s, wchar_t* out);
/* builds a .ico (16..256) from a picture: kind 0 = the picture fitted as it is, 1 = the logo's mask on a badge */
int art_write_icon(const art_img* src, int kind, const wchar_t* path);
