/* Primitive setup shared by the software rasterizer and the GPU frontend (gs_setup.h). The
 * arithmetic is gs_draw.c's, moved here unchanged. */
#include "gs_setup.h"

const gs_front_hooks* gs_front;
int gs_sw_draw = 1;
int gs_front_swdraw;

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

int gs_tri_setup(gs_tri* t, const gs_vtx* v0, const gs_vtx* v1, const gs_vtx* v2, int ofx, int ofy,
                 int scx0, int scx1, int scy0, int scy1) {
    int64_t x0 = v0->x - ofx, y0 = v0->y - ofy, x1 = v1->x - ofx, y1 = v1->y - ofy, x2 = v2->x - ofx, y2 = v2->y - ofy;
    int64_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0) return 0;
    int64_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int64_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int64_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int64_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int bx0 = imax((int)((minx + 15) >> 4), scx0), bx1 = imin((int)((maxx + 15) >> 4) - 1, scx1);
    int by0 = imax((int)((miny + 15) >> 4), scy0), by1 = imin((int)((maxy + 15) >> 4) - 1, scy1);
    if (bx0 > bx1 || by0 > by1) return 0;
    t->x0 = x0; t->y0 = y0; t->x1 = x1; t->y1 = y1; t->x2 = x2; t->y2 = y2;
    t->area = area;
    t->bx0 = bx0; t->bx1 = bx1; t->by0 = by0; t->by1 = by1;
    /* attribute planes (pixel units) */
    t->X[0] = (double)x0 / 16.0; t->X[1] = (double)x1 / 16.0; t->X[2] = (double)x2 / 16.0;
    t->Y[0] = (double)y0 / 16.0; t->Y[1] = (double)y1 / 16.0; t->Y[2] = (double)y2 / 16.0;
    const double* X = t->X;
    const double* Y = t->Y;
    double det = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    t->inv = 1.0 / det;
    return 1;
}

void gs_tri_planes(const gs_tri* t, const gs_vtx* const vs[3], int iip, int fge, int zuse, int tme, int fst,
                   gs_planes* pl) {
#define PLANE(p, field) gs_tri_plane(t, &(p), vs[0]->field, vs[1]->field, vs[2]->field)
    if (iip) { PLANE(pl->r, r); PLANE(pl->g, g); PLANE(pl->b, b); PLANE(pl->a, a); }
    else { gs_plane_const(&pl->r, vs[2]->r); gs_plane_const(&pl->g, vs[2]->g); gs_plane_const(&pl->b, vs[2]->b); gs_plane_const(&pl->a, vs[2]->a); }
    if (fge) PLANE(pl->f, f); else gs_plane_const(&pl->f, vs[2]->f);
    if (zuse) PLANE(pl->z, z); else gs_plane_const(&pl->z, vs[2]->z);
    gs_plane_const(&pl->s, 0); gs_plane_const(&pl->t, 0); gs_plane_const(&pl->q, 1);
    gs_plane_const(&pl->u, 0); gs_plane_const(&pl->v, 0);
    if (tme) {
        if (fst) { PLANE(pl->u, u); PLANE(pl->v, v); }
        else { PLANE(pl->s, s); PLANE(pl->t, t); PLANE(pl->q, q); }
    }
#undef PLANE
}

void gs_rowstep_init(gs_rowstep* rs, const gs_planes* pl, int py0) {
    const gs_plane* p[GS_A_N] = { &pl->r, &pl->g, &pl->b, &pl->a, &pl->f, &pl->z, &pl->u, &pl->v };
    static const double scale[GS_A_N] = { 65536.0, 65536.0, 65536.0, 65536.0, 65536.0, 65536.0,
                                          (double)(1 << GS_UVSH), (double)(1 << GS_UVSH) };
    for (int k = 0; k < GS_A_N; k++) gs_rowstep_attr(p[k], py0, scale[k], k == GS_A_Z, &rs->row[k], &rs->dy[k], &rs->dx[k]);
    rs->s = pl->s.c0 + pl->s.cy * py0; rs->sdy = pl->s.cy; rs->sdx = pl->s.cx;
    rs->t = pl->t.c0 + pl->t.cy * py0; rs->tdy = pl->t.cy; rs->tdx = pl->t.cx;
    rs->q = pl->q.c0 + pl->q.cy * py0; rs->qdy = pl->q.cy; rs->qdx = pl->q.cx;
}

int gs_sprite_setup(gs_sprite* s, const gs_vtx* v0, const gs_vtx* v1, int ofx, int ofy, int tme, int fst,
                    float tw16, float th16, int scx0, int scx1, int scy0, int scy1) {
    int X0 = v0->x - ofx, Y0 = v0->y - ofy, X1 = v1->x - ofx, Y1 = v1->y - ofy;
    double U0 = 0, U1 = 0, V0 = 0, V1 = 0;
    if (tme) {
        if (fst) { U0 = v0->u; U1 = v1->u; V0 = v0->v; V1 = v1->v; }
        else {
            U0 = v0->s / v0->q * tw16; U1 = v1->s / v1->q * tw16;
            V0 = v0->t / v0->q * th16; V1 = v1->t / v1->q * th16;
            if (!(fabs(U0) < 1e7)) U0 = 0;
            if (!(fabs(U1) < 1e7)) U1 = 0;
            if (!(fabs(V0) < 1e7)) V0 = 0;
            if (!(fabs(V1) < 1e7)) V1 = 0;
        }
    }
    if (X0 > X1) { int t = X0; X0 = X1; X1 = t; double d = U0; U0 = U1; U1 = d; }
    if (Y0 > Y1) { int t = Y0; Y0 = Y1; Y1 = t; double d = V0; V0 = V1; V1 = d; }
    int px0 = imax((X0 + 15) >> 4, scx0), px1 = imin(((X1 + 15) >> 4) - 1, scx1);
    int py0 = imax((Y0 + 15) >> 4, scy0), py1 = imin(((Y1 + 15) >> 4) - 1, scy1);
    if (px0 > px1 || py0 > py1) return 0;
    s->X0 = X0; s->Y0 = Y0; s->X1 = X1; s->Y1 = Y1;
    s->U0 = U0; s->U1 = U1; s->V0 = V0; s->V1 = V1;
    s->px0 = px0; s->px1 = px1; s->py0 = py0; s->py1 = py1;

    gs_planes* pl = &s->pl;
    gs_plane_const(&pl->r, v1->r); gs_plane_const(&pl->g, v1->g); gs_plane_const(&pl->b, v1->b); gs_plane_const(&pl->a, v1->a);
    gs_plane_const(&pl->f, v1->f); gs_plane_const(&pl->z, v1->z);
    gs_plane_const(&pl->s, 0); gs_plane_const(&pl->t, 0); gs_plane_const(&pl->q, 1);
    gs_plane_const(&pl->u, 0); gs_plane_const(&pl->v, 0);
    s->ua = s->ub = s->va = s->vb = s->du = 0;
    if (tme) {
        double dx = (X1 - X0) / 16.0, dy = (Y1 - Y0) / 16.0;
        pl->u.cx = dx > 0 ? (U1 - U0) / dx : 0;
        pl->u.c0 = U0 - pl->u.cx * (X0 / 16.0);
        pl->v.cy = dy > 0 ? (V1 - V0) / dy : 0;
        pl->v.c0 = V0 - pl->v.cy * (Y0 / 16.0);
        /* the coordinates (1/16 texel) the pixel pipeline will use at the first and last pixel
         * sampled, computed exactly as gs_span_start_at and the per-pixel stepping do (the edges
         * X1/Y1 are not sampled) */
        const double fs = (double)(1 << GS_UVSH);
        s->ua = gs_d2i32((pl->u.c0 + pl->u.cx * px0) * fs);
        s->du = gs_d2i32(pl->u.cx * fs);
        s->ub = (int32_t)((int64_t)s->ua + (int64_t)s->du * (px1 - px0));
        s->va = gs_d2i32((pl->v.c0 + pl->v.cy * py0) * fs);
        s->vb = gs_d2i32((pl->v.c0 + pl->v.cy * py1) * fs);
    }
    return 1;
}

void gs_line_planes(const gs_vtx* v0, const gs_vtx* v1, int iip, gs_planes* pl) {
#define LPLANE(p, field) do { (p).c0 = v0->field; (p).cx = (double)v1->field - (double)v0->field; (p).cy = 0; } while (0)
    if (iip) { LPLANE(pl->r, r); LPLANE(pl->g, g); LPLANE(pl->b, b); LPLANE(pl->a, a); }
    else { gs_plane_const(&pl->r, v1->r); gs_plane_const(&pl->g, v1->g); gs_plane_const(&pl->b, v1->b); gs_plane_const(&pl->a, v1->a); }
    LPLANE(pl->f, f); LPLANE(pl->z, z); LPLANE(pl->s, s); LPLANE(pl->t, t); LPLANE(pl->q, q); LPLANE(pl->u, u); LPLANE(pl->v, v);
#undef LPLANE
}
