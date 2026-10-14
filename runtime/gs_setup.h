/* Primitive setup shared by the software rasterizer (gs_draw.c) and the GPU frontend
 * (runtime/gpu/gpu_front.c, notes/d3d11-renderer.md 2.2): vertices, attribute planes, the 16.16
 * fixed-point row stepping of triangles, edge functions, the sprite rectangle rule and the line DDA.
 * Both backends call the same code, so the GPU's per-primitive integer records (exact Z, line
 * pixels, sprite rectangles) are the software renderer's values by construction. Factored out of
 * gs_draw.c without any arithmetic change (M1: VRAM hashes of every corpus recording identical).
 *
 * Also the frontend hook table: gs_draw.c calls it when a backend other than the software rasterizer
 * consumes the stream (NULL: software only). */
#pragma once
#include "gs_draw.h"

#include <math.h>

/* ---- vertices ---------------------------------------------------------------------------------- */
typedef struct {
    int32_t x, y;                        /* 12.4, before XYOFFSET */
    uint32_t z;
    int r, g, b, a, f;
    float s, t, q;
    int u, v;                            /* 1/16 texel */
} gs_vtx;

/* ---- numeric helpers ----------------------------------------------------------------------------- */
GS_AI int32_t gs_d2i32(double v) { return v > 2147483000.0 ? 2147483000 : v < -2147483000.0 ? -2147483000 : (int32_t)v; }
GS_AI int64_t gs_d2i64(double v) { return v > 9.0e18 ? (int64_t)9.0e18 : v < -9.0e18 ? -(int64_t)9.0e18 : (int64_t)v; }
GS_AI int64_t gs_fx_round(double v) { return v > 9.0e18 ? (int64_t)9.0e18 : v < -9.0e18 ? -(int64_t)9.0e18 : (int64_t)llround(v); }
GS_AI int32_t gs_sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }

/* ---- attribute planes ---------------------------------------------------------------------------- */
typedef struct { double c0, cx, cy; } gs_plane;            /* value = c0 + cx * px + cy * py */
typedef struct { gs_plane r, g, b, a, f, z, s, t, q, u, v; } gs_planes;

GS_AI void gs_plane_const(gs_plane* p, double v) { p->c0 = v; p->cx = p->cy = 0; }

GS_AI void gs_span_start_at(const gs_planes* pl, double x, double y, gs_span_start* st) {
#define GS_EV(p) ((p).c0 + (p).cx * x + (p).cy * y)
    st->r = gs_d2i32(GS_EV(pl->r) * 65536.0); st->g = gs_d2i32(GS_EV(pl->g) * 65536.0);
    st->b = gs_d2i32(GS_EV(pl->b) * 65536.0); st->a = gs_d2i32(GS_EV(pl->a) * 65536.0);
    st->f = gs_d2i32(GS_EV(pl->f) * 65536.0);
    st->z = gs_d2i64(GS_EV(pl->z) * 65536.0);
    st->s = (float)GS_EV(pl->s); st->t = (float)GS_EV(pl->t); st->q = (float)GS_EV(pl->q);
    st->u = gs_d2i32(GS_EV(pl->u) * (double)(1 << GS_UVSH)); st->v = gs_d2i32(GS_EV(pl->v) * (double)(1 << GS_UVSH));
#undef GS_EV
}

/* ---- triangles ----------------------------------------------------------------------------------- */
typedef struct {
    int64_t x0, y0, x1, y1, x2, y2;      /* 12.4, XYOFFSET subtracted */
    int64_t area;                        /* twice the signed area (12.4 squared) */
    int bx0, bx1, by0, by1;              /* pixel bounding box, scissored, inclusive */
    double X[3], Y[3], inv;              /* vertices in pixel units, 1 / determinant */
} gs_tri;

/* 0: nothing to draw (zero area or outside the scissor). */
int gs_tri_setup(gs_tri* t, const gs_vtx* v0, const gs_vtx* v1, const gs_vtx* v2, int ofx, int ofy,
                 int scx0, int scx1, int scy0, int scy1);

/* The plane through three attribute values at the triangle's vertices. */
GS_AI void gs_tri_plane(const gs_tri* t, gs_plane* pl, double f0, double f1, double f2) {
    const double* X = t->X;
    const double* Y = t->Y;
    double a0 = f0, d1 = f1 - a0, d2 = f2 - a0;
    pl->cx = (d1 * (Y[2] - Y[0]) - d2 * (Y[1] - Y[0])) * t->inv;
    pl->cy = (d2 * (X[1] - X[0]) - d1 * (X[2] - X[0])) * t->inv;
    pl->c0 = a0 - pl->cx * X[0] - pl->cy * Y[0];
}

/* All planes of a triangle as the software rasterizer uses them: colour Gouraud with iip (else
 * flat from v2), fog with fge, Z with zuse, S/T/Q (stq) or U/V (fst) with tme. */
void gs_tri_planes(const gs_tri* t, const gs_vtx* const vs[3], int iip, int fge, int zuse, int tme, int fst,
                   gs_planes* pl);

/* Fixed-point attribute rows for triangles: value at x = 0 of the current row, per-row and
 * per-pixel steps. Order: r g b a f (16.16), z (32.16), u v (1/16 texel << GS_UVSH). */
enum { GS_A_R, GS_A_G, GS_A_B, GS_A_A, GS_A_F, GS_A_Z, GS_A_U, GS_A_V, GS_A_N };
typedef struct {
    int64_t row[GS_A_N], dy[GS_A_N], dx[GS_A_N];
    double s, t, q, sdy, tdy, qdy, sdx, tdx, qdx;
} gs_rowstep;

void gs_rowstep_init(gs_rowstep* rs, const gs_planes* pl, int py0);

GS_AI void gs_rowstep_start(const gs_rowstep* rs, int64_t x, gs_span_start* st) {
    st->r = gs_sat32(rs->row[GS_A_R] + rs->dx[GS_A_R] * x); st->g = gs_sat32(rs->row[GS_A_G] + rs->dx[GS_A_G] * x);
    st->b = gs_sat32(rs->row[GS_A_B] + rs->dx[GS_A_B] * x); st->a = gs_sat32(rs->row[GS_A_A] + rs->dx[GS_A_A] * x);
    st->f = gs_sat32(rs->row[GS_A_F] + rs->dx[GS_A_F] * x);
    st->z = rs->row[GS_A_Z] + rs->dx[GS_A_Z] * x;
    st->u = gs_sat32(rs->row[GS_A_U] + rs->dx[GS_A_U] * x); st->v = gs_sat32(rs->row[GS_A_V] + rs->dx[GS_A_V] * x);
    st->s = (float)(rs->s + rs->sdx * (double)x);
    st->t = (float)(rs->t + rs->tdx * (double)x);
    st->q = (float)(rs->q + rs->qdx * (double)x);
}

GS_AI void gs_rowstep_next(gs_rowstep* rs) {
    for (int k = 0; k < GS_A_N; k++) rs->row[k] += rs->dy[k];
    rs->s += rs->sdy; rs->t += rs->tdy; rs->q += rs->qdy;
}

/* Edge function of one triangle edge on the pixel grid: inside where a * px + c >= bias for the
 * current row (c advances by dc per row). bias is 0 on top/left edges, 1 elsewhere. */
typedef struct { int64_t a, c, dc, bias; double inv; } gs_edge;

GS_AI void gs_edge_setup(gs_edge* e, int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t sgn, int py0) {
    int64_t ex = bx - ax, ey = by - ay;
    e->a = -16 * ey * sgn;
    e->c = sgn * (ex * (16 * (int64_t)py0 - ay) + ey * ax);
    e->dc = sgn * 16 * ex;
    int left = e->a > 0, top = e->a == 0 && sgn * ex > 0;
    e->bias = (left || top) ? 0 : 1;
    e->inv = e->a ? 1.0 / (double)(e->a > 0 ? e->a : -e->a) : 0;
}

/* The three edges of a set-up triangle at row by0. */
GS_AI void gs_tri_edges(const gs_tri* t, gs_edge e[3]) {
    int64_t sgn = t->area > 0 ? 1 : -1;
    gs_edge_setup(&e[0], t->x0, t->y0, t->x1, t->y1, sgn, t->by0);
    gs_edge_setup(&e[1], t->x1, t->y1, t->x2, t->y2, sgn, t->by0);
    gs_edge_setup(&e[2], t->x2, t->y2, t->x0, t->y0, sgn, t->by0);
}

/* Exact row bounds without 64-bit division: a double estimate (within +-1, the numerator is exact
 * in a double) corrected with the integer edge function. Results only matter inside [0, 2047]. */
GS_AI int64_t gs_edge_min_x(const gs_edge* e) {                /* a > 0: smallest inside px */
    double est = ceil((double)(e->bias - e->c) * e->inv);
    if (est < -4096.0) return -4096;
    if (est > 4096.0) return 4096;
    int64_t x = (int64_t)est;
    while (e->a * x + e->c < e->bias) x++;
    while (e->a * (x - 1) + e->c >= e->bias) x--;
    return x;
}

GS_AI int64_t gs_edge_max_x(const gs_edge* e) {                /* a < 0: largest inside px */
    double est = floor((double)(e->c - e->bias) * e->inv);
    if (est < -4096.0) return -4096;
    if (est > 4096.0) return 4096;
    int64_t x = (int64_t)est;
    while (e->a * x + e->c < e->bias) x--;
    while (e->a * (x + 1) + e->c >= e->bias) x++;
    return x;
}

/* The pixels [xl, xr] of the current row inside all three edges (xl > xr: none), within bx0..bx1. */
GS_AI void gs_tri_row(const gs_edge e[3], int bx0, int bx1, int64_t* pxl, int64_t* pxr) {
    int64_t xl = bx0, xr = bx1;
    for (int k = 0; k < 3; k++) {
        if (e[k].a > 0) { int64_t m = gs_edge_min_x(&e[k]); if (m > xl) xl = m; }
        else if (e[k].a < 0) { int64_t m = gs_edge_max_x(&e[k]); if (m < xr) xr = m; }
        else if (e[k].c < e[k].bias) { xl = 1; xr = 0; }
    }
    *pxl = xl;
    *pxr = xr;
}

GS_AI void gs_tri_edges_next(gs_edge e[3]) { e[0].c += e[0].dc; e[1].c += e[1].dc; e[2].c += e[2].dc; }

/* ---- sprites ------------------------------------------------------------------------------------- */
typedef struct {
    int X0, Y0, X1, Y1;                  /* 12.4, XYOFFSET subtracted, sorted */
    double U0, U1, V0, V1;               /* texture coordinates (1/16 texel) at the sorted corners */
    int px0, px1, py0, py1;              /* pixels covered: [X0, X1) x [Y0, Y1), scissored, inclusive */
    gs_planes pl;                        /* flat colour/fog/Z from v1; u along x, v along y */
    /* texturing (tme): the 1/16-texel << GS_UVSH coordinates the pixel pipeline uses at the first
     * pixel (ua, va), the per-pixel step du, at the last pixel (ub, vb) */
    int32_t ua, ub, va, vb, du;
} gs_sprite;

/* 0: nothing to draw. tw16/th16: 16 * the level-0 texture size (STQ sprites). */
int gs_sprite_setup(gs_sprite* s, const gs_vtx* v0, const gs_vtx* v1, int ofx, int ofy, int tme, int fst,
                    float tw16, float th16, int scx0, int scx1, int scy0, int scy1);

/* ---- lines (DDA) --------------------------------------------------------------------------------- */
typedef struct {
    int64_t X0, Y0, X1, Y1, dx, dy, a0;  /* 12.4, XYOFFSET subtracted; a0: major-axis start */
    int xmajor, p0, p1;                  /* major-axis pixels p0..p1 (end excluded) */
    double len;
} gs_line;

/* 0: zero length. */
GS_AI int gs_line_setup(gs_line* L, const gs_vtx* v0, const gs_vtx* v1, int ofx, int ofy) {
    int64_t X0 = v0->x - ofx, Y0 = v0->y - ofy, X1 = v1->x - ofx, Y1 = v1->y - ofy;
    int64_t dx = X1 - X0, dy = Y1 - Y0;
    if (!dx && !dy) return 0;
    int xmajor = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
    int64_t a0 = xmajor ? X0 : Y0, a1 = xmajor ? X1 : Y1;
    int64_t lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
    L->X0 = X0; L->Y0 = Y0; L->X1 = X1; L->Y1 = Y1; L->dx = dx; L->dy = dy; L->a0 = a0;
    L->xmajor = xmajor;
    L->p0 = (int)((lo + 15) >> 4);
    L->p1 = (int)((hi + 15) >> 4) - 1;
    L->len = (double)(a1 - a0);
    return 1;
}

/* Planes of a line, evaluated with x = t (0..1 along the line), y = 0. */
void gs_line_planes(const gs_vtx* v0, const gs_vtx* v1, int iip, gs_planes* pl);

/* Step p (p0..p1) of the DDA: returns t, the minor-axis pixel q and the pixels to cover: qn = 1 (q
 * with coverage cov[0] = 0x80), or with AA1 (aa) qn = 2: pixels q and q + 1 with coverage cov[0] and
 * cov[1] (0..0x80; 0 = skip). Pixel k: px = xmajor ? p : q + k, py = xmajor ? q + k : p. */
GS_AI double gs_line_step(const gs_line* L, int p, int aa, int* pq, int cov[2], int* pqn) {
    double t = ((double)p * 16.0 - (double)L->a0) / L->len;
    double other = L->xmajor ? (double)L->Y0 + t * (double)L->dy : (double)L->X0 + t * (double)L->dx;
    int q = (int)floor((other + 8.0) / 16.0), qn = 1;
    cov[0] = 0x80; cov[1] = 0;
    if (aa) {                            /* pixel centres sit at multiples of 16 */
        q = (int)floor(other / 16.0);
        int frac = (int)lround((other - 16.0 * q) * 8.0);   /* 0..128 towards pixel q + 1 */
        cov[0] = 0x80 - frac; cov[1] = frac; qn = 2;
    }
    *pq = q;
    *pqn = qn;
    return t;
}

/* ---- points -------------------------------------------------------------------------------------- */
GS_AI void gs_point_pixel(const gs_vtx* v, int ofx, int ofy, int* px, int* py) {
    *px = (v->x - ofx + 8) >> 4;
    *py = (v->y - ofy + 8) >> 4;
}

/* ---- frontend hooks (GPU backend) ---------------------------------------------------------------
 * The drawing environment as gs_draw.c decoded it, for a frontend that consumes the same stream. */
typedef struct {
    const gs_prim* P;                    /* decoded state (env_update) */
    const uint64_t* reg;                 /* general registers 0x00-0x63 as last written */
    int ctx;                             /* drawing context of the primitive (0/1) */
    int ofx, ofy;                        /* XYOFFSET, 12.4 */
    int iip, aa1;                        /* PRIM.IIP, PRIM.AA1 */
    int draws;                           /* 0: the primitive can write nothing (FBMSK all ones, no Z write) */
} gs_env_view;

typedef struct {
    /* a primitive kicked (type: PRIM type 0-6; a/b/c as many vertices as the type uses) */
    void (*prim)(const gs_env_view* env, int type, const gs_vtx* a, const gs_vtx* b, const gs_vtx* c);
    /* host->local transfer started (dir 0) / local->local (2) / local->host (1); raw registers */
    void (*xfer_start)(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint32_t dir);
    /* image data of a host->local transfer is about to be written (bytes) */
    void (*xfer_data)(uint32_t bytes);
    /* TEX0/TEX2 written (after the CLUT load it may have caused); clut_loaded: the CLUT changed */
    void (*tex0)(int ctx, uint64_t tex0, int clut_loaded);
    void (*reset)(void);                 /* CSR.RESET (gs_draw_reset) */
} gs_front_hooks;

/* NULL: software only. sw: 0 when the software rasterizer is off (the frontend alone). */
extern const gs_front_hooks* gs_front;
extern int gs_sw_draw;
