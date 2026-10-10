/* GS general registers (0x00-0x63), vertex queue, primitive setup and rasterization.
 *
 * Coordinates: XYZ X/Y are unsigned 12.4 fixed point; XYOFFSET is subtracted at drawing time. A
 * pixel (px, py) is sampled at (16*px, 16*py) in that space. Triangles use exact integer edge
 * functions with a top-left fill rule; sprites cover [X0, X1) x [Y0, Y1). Attributes are
 * interpolated as planes evaluated at the first pixel of each span, then stepped per pixel. */
#include "gs_draw.h"

#include <math.h>

enum {
    R_PRIM = 0x00, R_RGBAQ = 0x01, R_ST = 0x02, R_UV = 0x03, R_XYZF2 = 0x04, R_XYZ2 = 0x05,
    R_TEX0_1 = 0x06, R_TEX0_2 = 0x07, R_CLAMP_1 = 0x08, R_CLAMP_2 = 0x09, R_FOG = 0x0A,
    R_XYZF3 = 0x0C, R_XYZ3 = 0x0D, R_TEX1_1 = 0x14, R_TEX1_2 = 0x15, R_TEX2_1 = 0x16, R_TEX2_2 = 0x17,
    R_XYOFFSET_1 = 0x18, R_XYOFFSET_2 = 0x19, R_PRMODECONT = 0x1A, R_PRMODE = 0x1B, R_TEXCLUT = 0x1C,
    R_SCANMSK = 0x22, R_MIPTBP1_1 = 0x34, R_MIPTBP1_2 = 0x35, R_MIPTBP2_1 = 0x36, R_MIPTBP2_2 = 0x37,
    R_TEXA = 0x3B, R_FOGCOL = 0x3D, R_TEXFLUSH = 0x3F, R_SCISSOR_1 = 0x40, R_SCISSOR_2 = 0x41,
    R_ALPHA_1 = 0x42, R_ALPHA_2 = 0x43, R_DIMX = 0x44, R_DTHE = 0x45, R_COLCLAMP = 0x46,
    R_TEST_1 = 0x47, R_TEST_2 = 0x48, R_PABE = 0x49, R_FBA_1 = 0x4A, R_FBA_2 = 0x4B,
    R_FRAME_1 = 0x4C, R_FRAME_2 = 0x4D, R_ZBUF_1 = 0x4E, R_ZBUF_2 = 0x4F, R_BITBLTBUF = 0x50,
    R_TRXPOS = 0x51, R_TRXREG = 0x52, R_TRXDIR = 0x53, R_HWREG = 0x54, R_SIGNAL = 0x60,
    R_FINISH = 0x61, R_LABEL = 0x62,
};
enum { PRIM_POINT, PRIM_LINE, PRIM_LINESTRIP, PRIM_TRI, PRIM_TRISTRIP, PRIM_TRIFAN, PRIM_SPRITE };

#define UVSH GS_UVSH

typedef struct {
    int32_t x, y;                        /* 12.4, before XYOFFSET */
    uint32_t z;
    int r, g, b, a, f;
    float s, t, q;
    int u, v;                            /* 1/16 texel */
} vtx;

gs_stats_t gs_stats;
static uint64_t reg[0x64];
static vtx vq[3];
static int vcount;
static int env_dirty = 1, env_ok;
static gs_prim P;
static int ofx, ofy;                     /* XYOFFSET of the drawing context, 12.4 */
static int prim_draws;                   /* 0 when nothing can be written */
static int xfer_dir = 3;                 /* last TRXDIR */
static int xfer_dpsm, xfer_x0, xfer_y0, xfer_x1, xfer_y1;
static uint32_t xfer_dbp, xfer_dbw;

/* ---- helpers ------------------------------------------------------------------------------------ */

static void log_once(const char* what, uint32_t key) {
    static uint32_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 64) seen[n++] = key;
    fprintf(stderr, "[gs] %s\n", what);
}

static int32_t d2i32(double v) { return v > 2147483000.0 ? 2147483000 : v < -2147483000.0 ? -2147483000 : (int32_t)v; }
static int64_t d2i64(double v) { return v > 9.0e18 ? (int64_t)9.0e18 : v < -9.0e18 ? -(int64_t)9.0e18 : (int64_t)v; }
static float asf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }


static gs_tex0 decode_tex0(uint64_t v) {
    gs_tex0 t;
    t.tbp0 = (uint32_t)(v & 0x3FFF); t.tbw = (uint32_t)((v >> 14) & 0x3F); t.psm = (uint32_t)((v >> 20) & 0x3F);
    t.tw = (uint32_t)((v >> 26) & 0xF); t.th = (uint32_t)((v >> 30) & 0xF); t.tcc = (uint32_t)((v >> 34) & 1);
    t.tfx = (uint32_t)((v >> 35) & 3); t.cbp = (uint32_t)((v >> 37) & 0x3FFF); t.cpsm = (uint32_t)((v >> 51) & 0xF);
    t.csm = (uint32_t)((v >> 55) & 1); t.csa = (uint32_t)((v >> 56) & 0x1F); t.cld = (uint32_t)((v >> 61) & 7);
    return t;
}

static gs_texa decode_texa(uint64_t v) {
    gs_texa a = { (uint32_t)(v & 0xFF), (uint32_t)((v >> 15) & 1), (uint32_t)((v >> 32) & 0xFF) };
    return a;
}

static gs_texclut decode_texclut(uint64_t v) {
    gs_texclut c = { (uint32_t)(v & 0x3F), (uint32_t)((v >> 6) & 0x3F), (uint32_t)((v >> 12) & 0x3FF) };
    return c;
}

static int psm_bpp(uint32_t psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    default: return 32;
    }
}

/* ---- drawing environment ------------------------------------------------------------------------ */

static void bind_textures(int ctx) {
    gs_tex0 t0 = decode_tex0(reg[R_TEX0_1 + ctx]);
    gs_texa ta = decode_texa(reg[R_TEXA]);
    uint64_t tex1 = reg[R_TEX1_1 + ctx], clamp = reg[R_CLAMP_1 + ctx];
    if (t0.tw > 10) t0.tw = 10;
    if (t0.th > 10) t0.th = 10;
    P.tfx = (int)t0.tfx;
    P.tcc = (int)t0.tcc;
    P.tw16 = (float)(16 << t0.tw);
    P.th16 = (float)(16 << t0.th);
    P.lcm = (int)(tex1 & 1);
    int mxl = (int)((tex1 >> 2) & 7);
    if (mxl > 6) mxl = 6;
    P.mmag = (int)((tex1 >> 5) & 1);
    P.mmin = (int)((tex1 >> 6) & 7);
    if (P.mmin > 5) P.mmin = 1;
    P.lshift = (int)((tex1 >> 19) & 3);
    P.k = (int)(((int64_t)(tex1 << 20)) >> 52);   /* bits 32-43, signed (7.4) */
    int mtba = (int)((tex1 >> 9) & 1);
    int mip = mxl > 0 && P.mmin >= 2;
    if (!mip) mxl = 0;

    /* texture mode and level */
    P.cur_level = 0;
    if (P.lcm || (P.mmin <= 1 && P.mmin == P.mmag)) {
        int lod = P.lcm ? P.k : (P.mmag ? 1 : 0);  /* only the sign matters in the second case */
        if (!P.lcm) { P.texm = 1 + P.mmag; }
        else if (lod <= 0) { P.texm = 1 + P.mmag; }
        else switch (P.mmin) {
        case 0: P.texm = 1; break;
        case 1: P.texm = 2; break;
        case 2: case 4: P.texm = P.mmin == 4 ? 2 : 1; P.cur_level = imin((lod + 8) >> 4, mxl); break;
        default:
            if ((lod >> 4) >= mxl) { P.texm = P.mmin == 5 ? 2 : 1; P.cur_level = mxl; }
            else P.texm = 3;
            break;
        }
    } else {
        P.texm = 3;
    }
    P.mxl = mxl;
    P.nlevels = mxl + 1;

    uint64_t mt1 = reg[R_MIPTBP1_1 + ctx], mt2 = reg[R_MIPTBP2_1 + ctx];
    uint32_t tbp = t0.tbp0, tbw = t0.tbw;
    for (int l = 0; l <= mxl; l++) {
        gs_tex0 t = t0;
        if (l > 0) {
            if (mtba && l <= 3) {        /* automatic: each level follows the previous one, width halves */
                uint32_t ph = 1u << (t0.th > (uint32_t)(l - 1) ? t0.th - (uint32_t)(l - 1) : 0);
                tbp = (tbp + (tbw * 64u * ph * (uint32_t)psm_bpp(t0.psm) / 8u) / 256u) & 0x3FFF;
                tbw = tbw > 1 ? tbw / 2 : 1;
            } else {
                uint64_t m = l <= 3 ? mt1 : mt2;
                int sh = ((l - 1) % 3) * 20;
                tbp = (uint32_t)((m >> sh) & 0x3FFF);
                tbw = (uint32_t)((m >> (sh + 14)) & 0x3F);
            }
            t.tbp0 = tbp; t.tbw = tbw;
            t.tw = t0.tw > (uint32_t)l ? t0.tw - (uint32_t)l : 0;
            t.th = t0.th > (uint32_t)l ? t0.th - (uint32_t)l : 0;
        }
        if (P.texm == 3 || l == P.cur_level) gs_tex_bind(&P.lvl[l], &t, &ta, clamp, l);
    }
}

static void env_update(void) {
    env_dirty = 0;
    uint64_t attr = (reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE];
    int ctx = (int)((attr >> 9) & 1);
    P.iip = (int)((attr >> 3) & 1);
    P.tme = (int)((attr >> 4) & 1);
    P.fge = (int)((attr >> 5) & 1);
    P.abe = (int)((attr >> 6) & 1);
    P.fst = (int)((attr >> 8) & 1);
    if (attr & (1u << 7)) log_once("antialiasing (AA1) requested; drawn without it", 1);

    uint64_t frame = reg[R_FRAME_1 + ctx], zbuf = reg[R_ZBUF_1 + ctx], test = reg[R_TEST_1 + ctx];
    uint64_t alpha = reg[R_ALPHA_1 + ctx], sc = reg[R_SCISSOR_1 + ctx], xyo = reg[R_XYOFFSET_1 + ctx];

    P.fpsm = (int)((frame >> 24) & 0x3F);
    P.ffmt = gs_fmt_of(P.fpsm);
    P.fpage = (uint32_t)(frame & 0x1FF);
    P.fbw = (uint32_t)((frame >> 16) & 0x3F);
    P.fbmsk = (uint32_t)(frame >> 32);
    env_ok = P.ffmt != NULL;
    if (!env_ok) { log_once("unsupported FRAME PSM; primitives skipped", 0x100 + (uint32_t)P.fpsm); return; }

    P.zpsm = 0x30 | (int)((zbuf >> 24) & 0xF);
    P.zfmt = gs_fmt_of(P.zpsm);
    P.zpage = (uint32_t)(zbuf & 0x1FF);
    P.zbw = P.fbw;                       /* the Z buffer uses the frame buffer width */
    P.zmsk = (int)((zbuf >> 32) & 1);
    P.zmax = P.zfmt ? (P.zfmt->bits == 32 ? 0xFFFFFFFFu : P.zfmt->bits == 24 ? 0xFFFFFFu : 0xFFFFu) : 0;

    P.ate = (int)(test & 1);
    P.atst = (int)((test >> 1) & 7);
    P.aref = (int)((test >> 4) & 0xFF);
    P.afail = (int)((test >> 12) & 3);
    P.date = (int)((test >> 14) & 1);
    P.datm = (int)((test >> 15) & 1);
    int zte = (int)((test >> 16) & 1);
    P.ztst = zte ? (int)((test >> 17) & 3) : 1;
    P.zuse = P.zfmt && !(P.ztst == 1 && P.zmsk);
    if (!P.zfmt) P.zmsk = 1;

    P.asel = (int)(alpha & 3); P.bsel = (int)((alpha >> 2) & 3);
    P.csel = (int)((alpha >> 4) & 3); P.dsel = (int)((alpha >> 6) & 3);
    P.fix = (int)((alpha >> 32) & 0xFF);
    P.pabe = (int)(reg[R_PABE] & 1);
    P.fba = (reg[R_FBA_1 + ctx] & 1) ? 0x80 : 0;
    P.colclamp = (int)(reg[R_COLCLAMP] & 1);
    P.dthe = (int)(reg[R_DTHE] & 1);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int d = (int)((reg[R_DIMX] >> (r * 16 + c * 4)) & 7);
            P.dimx[r][c] = (int8_t)(d >= 4 ? d - 8 : d);
        }
    uint64_t fc = reg[R_FOGCOL];
    P.fogr = (int)(fc & 0xFF); P.fogg = (int)((fc >> 8) & 0xFF); P.fogb = (int)((fc >> 16) & 0xFF);
    P.scanmsk = (int)(reg[R_SCANMSK] & 3);
    P.scx0 = (int)(sc & 0x7FF); P.scx1 = (int)((sc >> 16) & 0x7FF);
    P.scy0 = (int)((sc >> 32) & 0x7FF); P.scy1 = (int)((sc >> 48) & 0x7FF);
    ofx = (int)(xyo & 0xFFFF);
    ofy = (int)((xyo >> 32) & 0xFFFF);

    if (P.tme) bind_textures(ctx);
    else P.texm = 0;
    gs_prim_finalize(&P);
    prim_draws = P.fbmsk != 0xFFFFFFFFu || (P.zuse && !P.zmsk);
}

/* ---- attribute planes ---------------------------------------------------------------------------- */

typedef struct { double c0, cx, cy; } plane;          /* value = c0 + cx * px + cy * py */
typedef struct { plane r, g, b, a, f, z, s, t, q, u, v; } planes;

static void plane_const(plane* p, double v) { p->c0 = v; p->cx = p->cy = 0; }

static void span_start_at(const planes* pl, double x, double y, gs_span_start* st) {
#define EV(p) ((p).c0 + (p).cx * x + (p).cy * y)
    st->r = d2i32(EV(pl->r) * 65536.0); st->g = d2i32(EV(pl->g) * 65536.0);
    st->b = d2i32(EV(pl->b) * 65536.0); st->a = d2i32(EV(pl->a) * 65536.0);
    st->f = d2i32(EV(pl->f) * 65536.0);
    st->z = d2i64(EV(pl->z) * 65536.0);
    st->s = (float)EV(pl->s); st->t = (float)EV(pl->t); st->q = (float)EV(pl->q);
    st->u = d2i32(EV(pl->u) * (double)(1 << UVSH)); st->v = d2i32(EV(pl->v) * (double)(1 << UVSH));
#undef EV
}

static void set_steps(const planes* pl) {
    P.dr = d2i32(pl->r.cx * 65536.0); P.dg = d2i32(pl->g.cx * 65536.0);
    P.db = d2i32(pl->b.cx * 65536.0); P.da = d2i32(pl->a.cx * 65536.0);
    P.df = d2i32(pl->f.cx * 65536.0);
    P.dz = d2i64(pl->z.cx * 65536.0);
    P.ds = (float)pl->s.cx; P.dt = (float)pl->t.cx; P.dq = (float)pl->q.cx;
    P.du = d2i32(pl->u.cx * (double)(1 << UVSH)); P.dv = d2i32(pl->v.cx * (double)(1 << UVSH));
}

/* ---- texture preparation per primitive -------------------------------------------------------- */

/* Texel-space bounding box of the vertices (level 0), then decode what the primitive can reach. */
static void tex_prepare(const vtx* const* vs, int n, int as_uv, const double* su, const double* sv) {
    if (!P.tme) return;
    int lo = P.texm == 3 ? 0 : P.cur_level, hi = P.texm == 3 ? P.mxl : P.cur_level;
    for (int l = lo; l <= hi; l++) gs_tex_validate(&P.lvl[l]);

    double umin = 1e30, umax = -1e30, vmin = 1e30, vmax = -1e30;
    int all = 0;
    if (as_uv) {                          /* caller supplies coordinates in 1/16 texel */
        for (int i = 0; i < n; i++) {
            umin = fmin(umin, su[i] / 16.0); umax = fmax(umax, su[i] / 16.0);
            vmin = fmin(vmin, sv[i] / 16.0); vmax = fmax(vmax, sv[i] / 16.0);
        }
    } else if (P.fst) {
        for (int i = 0; i < n; i++) {
            umin = fmin(umin, vs[i]->u / 16.0); umax = fmax(umax, vs[i]->u / 16.0);
            vmin = fmin(vmin, vs[i]->v / 16.0); vmax = fmax(vmax, vs[i]->v / 16.0);
        }
    } else {
        int pos = 0, neg = 0;
        for (int i = 0; i < n; i++) {
            float q = vs[i]->q;
            if (q > 0) pos++; else if (q < 0) neg++;
            double iq = 1.0 / q, uu = vs[i]->s * iq * P.tw16 / 16.0, vv = vs[i]->t * iq * P.th16 / 16.0;
            if (!(fabs(uu) < 1e7) || !(fabs(vv) < 1e7)) all = 1;
            umin = fmin(umin, uu); umax = fmax(umax, uu);
            vmin = fmin(vmin, vv); vmax = fmax(vmax, vv);
        }
        if (pos != n && neg != n) all = 1;
    }
    P.nowrap = 0;
    for (int l = lo; l <= hi; l++) {
        gs_texlvl* L = &P.lvl[l];
        if (all) { gs_tex_ensure(L, 0, L->w - 1, 0, L->h - 1); continue; }
        double sc = 1.0 / (double)(1 << l);
        int u0 = (int)floor(umin * sc) - 1, u1 = (int)floor(umax * sc) + 1;
        int v0 = (int)floor(vmin * sc) - 1, v1 = (int)floor(vmax * sc) + 1;
        gs_tex_ensure(L, u0, u1, v0, v1);
        /* every texel the primitive can reach (one texel of margin) is inside the texture and the
         * clamp region: sampling needs no wrapping */
        if (P.texm != 3 && L->umode != 3 && L->vmode != 3 && u0 >= 0 && v0 >= 0 &&
            u0 >= L->ulo && v0 >= L->vlo && u1 <= L->w - 1 && v1 <= L->h - 1 && u1 <= L->uhi && v1 <= L->vhi)
            P.nowrap = 1;
    }
}

static void mark_written(int x0, int y0, int x1, int y1) {   /* inclusive pixel bbox */
    if (P.fbmsk != 0xFFFFFFFFu) gs_mark_rect(P.fpsm, P.fpage * 32, P.fbw, x0, y0, x1 + 1, y1 + 1);
    if (P.zuse && !P.zmsk) gs_mark_rect(P.zpsm, P.zpage * 32, P.zbw, x0, y0, x1 + 1, y1 + 1);
}

static int row_masked(int py) {
    return (P.scanmsk == 2 && !(py & 1)) || (P.scanmsk == 3 && (py & 1));
}

/* ---- primitives ---------------------------------------------------------------------------------- */

static void draw_sprite(const vtx* v0, const vtx* v1) {
    int X0 = v0->x - ofx, Y0 = v0->y - ofy, X1 = v1->x - ofx, Y1 = v1->y - ofy;
    double U0 = 0, U1 = 0, V0 = 0, V1 = 0;
    if (P.tme) {
        if (P.fst) { U0 = v0->u; U1 = v1->u; V0 = v0->v; V1 = v1->v; }
        else {
            U0 = v0->s / v0->q * P.tw16; U1 = v1->s / v1->q * P.tw16;
            V0 = v0->t / v0->q * P.th16; V1 = v1->t / v1->q * P.th16;
            if (!(fabs(U0) < 1e7)) U0 = 0;
            if (!(fabs(U1) < 1e7)) U1 = 0;
            if (!(fabs(V0) < 1e7)) V0 = 0;
            if (!(fabs(V1) < 1e7)) V1 = 0;
        }
    }
    if (X0 > X1) { int t = X0; X0 = X1; X1 = t; double d = U0; U0 = U1; U1 = d; }
    if (Y0 > Y1) { int t = Y0; Y0 = Y1; Y1 = t; double d = V0; V0 = V1; V1 = d; }
    int px0 = imax((X0 + 15) >> 4, P.scx0), px1 = imin(((X1 + 15) >> 4) - 1, P.scx1);
    int py0 = imax((Y0 + 15) >> 4, P.scy0), py1 = imin(((Y1 + 15) >> 4) - 1, P.scy1);
    if (px0 > px1 || py0 > py1) return;

    planes pl;
    plane_const(&pl.r, v1->r); plane_const(&pl.g, v1->g); plane_const(&pl.b, v1->b); plane_const(&pl.a, v1->a);
    plane_const(&pl.f, v1->f); plane_const(&pl.z, v1->z);
    plane_const(&pl.s, 0); plane_const(&pl.t, 0); plane_const(&pl.q, 1);
    plane_const(&pl.u, 0); plane_const(&pl.v, 0);
    if (P.tme) {
        double dx = (X1 - X0) / 16.0, dy = (Y1 - Y0) / 16.0;
        pl.u.cx = dx > 0 ? (U1 - U0) / dx : 0;
        pl.u.c0 = U0 - pl.u.cx * (X0 / 16.0);
        pl.v.cy = dy > 0 ? (V1 - V0) / dy : 0;
        pl.v.c0 = V0 - pl.v.cy * (Y0 / 16.0);
        P.lod_q = v1->q;
        double su[2] = { U0, U1 }, sv[2] = { V0, V1 };
        tex_prepare(NULL, 2, 1, su, sv);
    }
    set_steps(&pl);
    gs_batch_begin(&P, 0);               /* u along x, v along y: no per-pixel division */
    gs_span_start st;
    for (int py = py0; py <= py1; py++) {
        if (row_masked(py)) continue;
        span_start_at(&pl, px0, py, &st);
        gs_batch_span(py, px0, px1 - px0 + 1, &st);
    }
    gs_batch_end();
    mark_written(px0, py0, px1, py1);
}

/* Edge function of one triangle edge on the pixel grid: inside where a * px + c >= bias for the
 * current row (c advances by dc per row). bias is 0 on top/left edges, 1 elsewhere. */
typedef struct { int64_t a, c, dc, bias; double inv; } edge;

static void edge_setup(edge* e, int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t sgn, int py0) {
    int64_t ex = bx - ax, ey = by - ay;
    e->a = -16 * ey * sgn;
    e->c = sgn * (ex * (16 * (int64_t)py0 - ay) + ey * ax);
    e->dc = sgn * 16 * ex;
    int left = e->a > 0, top = e->a == 0 && sgn * ex > 0;
    e->bias = (left || top) ? 0 : 1;
    e->inv = e->a ? 1.0 / (double)(e->a > 0 ? e->a : -e->a) : 0;
}

/* Exact row bounds without 64-bit division: a double estimate (within +-1, the numerator is exact
 * in a double) corrected with the integer edge function. Results only matter inside [0, 2047]. */
static int64_t edge_min_x(const edge* e) {                     /* a > 0: smallest inside px */
    double est = ceil((double)(e->bias - e->c) * e->inv);
    if (est < -4096.0) return -4096;
    if (est > 4096.0) return 4096;
    int64_t x = (int64_t)est;
    while (e->a * x + e->c < e->bias) x++;
    while (e->a * (x - 1) + e->c >= e->bias) x--;
    return x;
}

static int64_t edge_max_x(const edge* e) {                     /* a < 0: largest inside px */
    double est = floor((double)(e->c - e->bias) * e->inv);
    if (est < -4096.0) return -4096;
    if (est > 4096.0) return 4096;
    int64_t x = (int64_t)est;
    while (e->a * x + e->c < e->bias) x--;
    while (e->a * (x + 1) + e->c >= e->bias) x++;
    return x;
}

/* Fixed-point attribute rows for triangles: value at x = 0 of the current row, per-row and
 * per-pixel steps. Order: r g b a f (16.16), z (32.16), u v (1/16 texel << UVSH). */
enum { A_R, A_G, A_B, A_A, A_F, A_Z, A_U, A_V, A_N };
typedef struct {
    int64_t row[A_N], dy[A_N], dx[A_N];
    double s, t, q, sdy, tdy, qdy, sdx, tdx, qdx;
} rowstep;

static int64_t fx_round(double v) { return v > 9.0e18 ? (int64_t)9.0e18 : v < -9.0e18 ? -(int64_t)9.0e18 : (int64_t)llround(v); }
static int32_t sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }

static void rowstep_init(rowstep* rs, const planes* pl, int py0) {
    const plane* p[A_N] = { &pl->r, &pl->g, &pl->b, &pl->a, &pl->f, &pl->z, &pl->u, &pl->v };
    static const double scale[A_N] = { 65536.0, 65536.0, 65536.0, 65536.0, 65536.0, 65536.0,
                                       (double)(1 << UVSH), (double)(1 << UVSH) };
    for (int k = 0; k < A_N; k++) {
        rs->row[k] = fx_round((p[k]->c0 + p[k]->cy * py0) * scale[k]);
        rs->dy[k] = fx_round(p[k]->cy * scale[k]);
        rs->dx[k] = k == A_Z ? fx_round(p[k]->cx * scale[k]) : sat32(fx_round(p[k]->cx * scale[k]));
    }
    rs->s = pl->s.c0 + pl->s.cy * py0; rs->sdy = pl->s.cy; rs->sdx = pl->s.cx;
    rs->t = pl->t.c0 + pl->t.cy * py0; rs->tdy = pl->t.cy; rs->tdx = pl->t.cx;
    rs->q = pl->q.c0 + pl->q.cy * py0; rs->qdy = pl->q.cy; rs->qdx = pl->q.cx;
    P.dr = (int32_t)rs->dx[A_R]; P.dg = (int32_t)rs->dx[A_G]; P.db = (int32_t)rs->dx[A_B]; P.da = (int32_t)rs->dx[A_A];
    P.df = (int32_t)rs->dx[A_F]; P.dz = rs->dx[A_Z]; P.du = (int32_t)rs->dx[A_U]; P.dv = (int32_t)rs->dx[A_V];
    P.ds = (float)rs->sdx; P.dt = (float)rs->tdx; P.dq = (float)rs->qdx;
}

static void rowstep_start(const rowstep* rs, int64_t x, gs_span_start* st) {
    st->r = sat32(rs->row[A_R] + rs->dx[A_R] * x); st->g = sat32(rs->row[A_G] + rs->dx[A_G] * x);
    st->b = sat32(rs->row[A_B] + rs->dx[A_B] * x); st->a = sat32(rs->row[A_A] + rs->dx[A_A] * x);
    st->f = sat32(rs->row[A_F] + rs->dx[A_F] * x);
    st->z = rs->row[A_Z] + rs->dx[A_Z] * x;
    st->u = sat32(rs->row[A_U] + rs->dx[A_U] * x); st->v = sat32(rs->row[A_V] + rs->dx[A_V] * x);
    st->s = (float)(rs->s + rs->sdx * (double)x);
    st->t = (float)(rs->t + rs->tdx * (double)x);
    st->q = (float)(rs->q + rs->qdx * (double)x);
}

static void rowstep_next(rowstep* rs) {
    for (int k = 0; k < A_N; k++) rs->row[k] += rs->dy[k];
    rs->s += rs->sdy; rs->t += rs->tdy; rs->q += rs->qdy;
}

static void draw_triangle(const vtx* v0, const vtx* v1, const vtx* v2) {
    int64_t x0 = v0->x - ofx, y0 = v0->y - ofy, x1 = v1->x - ofx, y1 = v1->y - ofy, x2 = v2->x - ofx, y2 = v2->y - ofy;
    int64_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0) return;
    int64_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int64_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int64_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int64_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int bx0 = imax((int)((minx + 15) >> 4), P.scx0), bx1 = imin((int)((maxx + 15) >> 4) - 1, P.scx1);
    int by0 = imax((int)((miny + 15) >> 4), P.scy0), by1 = imin((int)((maxy + 15) >> 4) - 1, P.scy1);
    if (bx0 > bx1 || by0 > by1) return;

    /* attribute planes (pixel units) */
    double X[3] = { (double)x0 / 16.0, (double)x1 / 16.0, (double)x2 / 16.0 };
    double Y[3] = { (double)y0 / 16.0, (double)y1 / 16.0, (double)y2 / 16.0 };
    double det = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]), inv = 1.0 / det;
    const vtx* vs[3] = { v0, v1, v2 };
#define PLANE(pl, field) do { \
        double a0 = vs[0]->field, d1 = vs[1]->field - a0, d2 = vs[2]->field - a0; \
        (pl).cx = (d1 * (Y[2] - Y[0]) - d2 * (Y[1] - Y[0])) * inv; \
        (pl).cy = (d2 * (X[1] - X[0]) - d1 * (X[2] - X[0])) * inv; \
        (pl).c0 = a0 - (pl).cx * X[0] - (pl).cy * Y[0]; } while (0)
    planes pl;
    if (P.iip) { PLANE(pl.r, r); PLANE(pl.g, g); PLANE(pl.b, b); PLANE(pl.a, a); }
    else { plane_const(&pl.r, v2->r); plane_const(&pl.g, v2->g); plane_const(&pl.b, v2->b); plane_const(&pl.a, v2->a); }
    if (P.fge) PLANE(pl.f, f); else plane_const(&pl.f, v2->f);
    if (P.zuse) PLANE(pl.z, z); else plane_const(&pl.z, v2->z);
    plane_const(&pl.s, 0); plane_const(&pl.t, 0); plane_const(&pl.q, 1);
    plane_const(&pl.u, 0); plane_const(&pl.v, 0);
    if (P.tme) {
        if (P.fst) { PLANE(pl.u, u); PLANE(pl.v, v); P.lod_q = v2->q; }
        else { PLANE(pl.s, s); PLANE(pl.t, t); PLANE(pl.q, q); }
        tex_prepare(vs, 3, 0, NULL, NULL);
    }
#undef PLANE
    rowstep rs;
    rowstep_init(&rs, &pl, by0);

    int64_t sgn = area > 0 ? 1 : -1;
    edge e[3];
    edge_setup(&e[0], x0, y0, x1, y1, sgn, by0);
    edge_setup(&e[1], x1, y1, x2, y2, sgn, by0);
    edge_setup(&e[2], x2, y2, x0, y0, sgn, by0);
    gs_span_start st;
    gs_batch_begin(&P, P.tme && !P.fst);
    int rx0 = bx1, rx1 = bx0;
    for (int py = by0; py <= by1; py++, e[0].c += e[0].dc, e[1].c += e[1].dc, e[2].c += e[2].dc, rowstep_next(&rs)) {
        if (P.scanmsk >= 2 && row_masked(py)) continue;
        int64_t xl = bx0, xr = bx1;
        for (int k = 0; k < 3; k++) {
            if (e[k].a > 0) { int64_t m = edge_min_x(&e[k]); if (m > xl) xl = m; }
            else if (e[k].a < 0) { int64_t m = edge_max_x(&e[k]); if (m < xr) xr = m; }
            else if (e[k].c < e[k].bias) { xl = 1; xr = 0; }
        }
        if (xl > xr) continue;
        rowstep_start(&rs, xl, &st);
        gs_batch_span(py, (int)xl, (int)(xr - xl + 1), &st);
        if (xl < rx0) rx0 = (int)xl;
        if (xr > rx1) rx1 = (int)xr;
    }
    gs_batch_end();
    if (rx0 <= rx1) mark_written(rx0, by0, rx1, by1);
}

static void draw_line(const vtx* v0, const vtx* v1) {
    int64_t X0 = v0->x - ofx, Y0 = v0->y - ofy, X1 = v1->x - ofx, Y1 = v1->y - ofy;
    int64_t dx = X1 - X0, dy = Y1 - Y0;
    if (!dx && !dy) return;
    int xmajor = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
    int64_t a0 = xmajor ? X0 : Y0, a1 = xmajor ? X1 : Y1;
    int64_t lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
    int p0 = (int)((lo + 15) >> 4), p1 = (int)((hi + 15) >> 4) - 1;   /* major-axis pixels, end excluded */
    double len = (double)(a1 - a0);

    if (P.tme) {
        const vtx* vs[2] = { v0, v1 };
        P.lod_q = v1->q;
        tex_prepare(vs, 2, 0, NULL, NULL);
    }
    planes pl;                           /* evaluated with x = t (0..1 along the line), y = 0 */
#define LPLANE(pl, field) do { (pl).c0 = v0->field; (pl).cx = (double)v1->field - (double)v0->field; (pl).cy = 0; } while (0)
    if (P.iip) { LPLANE(pl.r, r); LPLANE(pl.g, g); LPLANE(pl.b, b); LPLANE(pl.a, a); }
    else { plane_const(&pl.r, v1->r); plane_const(&pl.g, v1->g); plane_const(&pl.b, v1->b); plane_const(&pl.a, v1->a); }
    LPLANE(pl.f, f); LPLANE(pl.z, z); LPLANE(pl.s, s); LPLANE(pl.t, t); LPLANE(pl.q, q); LPLANE(pl.u, u); LPLANE(pl.v, v);
#undef LPLANE
    P.dr = P.dg = P.db = P.da = P.df = 0; P.dz = 0; P.ds = P.dt = P.dq = 0; P.du = P.dv = 0;
    gs_span_start st;
    int mx0 = 2047, mx1 = 0, my0 = 2047, my1 = 0;
    gs_batch_begin(&P, P.tme && !P.fst);
    for (int p = p0; p <= p1; p++) {
        double t = ((double)p * 16.0 - (double)a0) / len;
        double other = xmajor ? (double)Y0 + t * (double)dy : (double)X0 + t * (double)dx;
        int q = (int)floor((other + 8.0) / 16.0);
        int px = xmajor ? p : q, py = xmajor ? q : p;
        if (px < P.scx0 || px > P.scx1 || py < P.scy0 || py > P.scy1 || row_masked(py)) continue;
        span_start_at(&pl, t, 0, &st);
        gs_batch_span(py, px, 1, &st);
        mx0 = imin(mx0, px); mx1 = imax(mx1, px); my0 = imin(my0, py); my1 = imax(my1, py);
    }
    gs_batch_end();
    if (mx0 <= mx1) mark_written(mx0, my0, mx1, my1);
}

static void draw_point(const vtx* v) {
    int px = (v->x - ofx + 8) >> 4, py = (v->y - ofy + 8) >> 4;
    if (px < P.scx0 || px > P.scx1 || py < P.scy0 || py > P.scy1 || row_masked(py)) return;
    if (P.tme) {
        const vtx* vs[1] = { v };
        P.lod_q = v->q;
        tex_prepare(vs, 1, 0, NULL, NULL);
    }
    gs_span_start st = { v->r << 16, v->g << 16, v->b << 16, v->a << 16, v->f << 16, (int64_t)v->z << 16,
                         v->s, v->t, v->q, v->u << UVSH, v->v << UVSH };
    P.dr = P.dg = P.db = P.da = P.df = 0; P.dz = 0; P.ds = P.dt = P.dq = 0; P.du = P.dv = 0;
    gs_batch_begin(&P, P.tme && !P.fst);
    gs_batch_span(py, px, 1, &st);
    gs_batch_end();
    mark_written(px, py, px, py);
}

/* ---- vertex queue -------------------------------------------------------------------------------- */

static void draw(int type, const vtx* a, const vtx* b, const vtx* c) {
    if (env_dirty) env_update();
    gs_stats.prims++;
    if (!env_ok || !prim_draws) return;
    switch (type) {
    case PRIM_POINT: draw_point(a); break;
    case PRIM_LINE: case PRIM_LINESTRIP: draw_line(a, b); break;
    case PRIM_TRI: case PRIM_TRISTRIP: case PRIM_TRIFAN: gs_stats.tris++; draw_triangle(a, b, c); break;
    case PRIM_SPRITE: gs_stats.sprites++; draw_sprite(a, b); break;
    default: break;
    }
}

static void vertex(uint64_t xyz, int has_f, int kick) {
    vtx* n = &vq[vcount];
    n->x = (int32_t)(xyz & 0xFFFF);
    n->y = (int32_t)((xyz >> 16) & 0xFFFF);
    if (has_f) { n->z = (uint32_t)((xyz >> 32) & 0xFFFFFF); n->f = (int)(xyz >> 56); }
    else { n->z = (uint32_t)(xyz >> 32); n->f = (int)(reg[R_FOG] >> 56); }
    uint64_t c = reg[R_RGBAQ];
    n->r = (int)(c & 0xFF); n->g = (int)((c >> 8) & 0xFF); n->b = (int)((c >> 16) & 0xFF); n->a = (int)((c >> 24) & 0xFF);
    n->q = asf((uint32_t)(c >> 32));
    n->s = asf((uint32_t)(reg[R_ST] & 0xFFFFFF00u));          /* low 8 mantissa bits are dropped */
    n->t = asf((uint32_t)(reg[R_ST] >> 32) & 0xFFFFFF00u);
    n->u = (int)(reg[R_UV] & 0x3FFF);
    n->v = (int)((reg[R_UV] >> 16) & 0x3FFF);
    vcount++;

    int type = (int)(reg[R_PRIM] & 7);
    switch (type) {
    case PRIM_POINT:
        if (kick) draw(type, &vq[0], NULL, NULL);
        vcount = 0;
        break;
    case PRIM_LINE: case PRIM_SPRITE:
        if (vcount == 2) { if (kick) draw(type, &vq[0], &vq[1], NULL); vcount = 0; }
        break;
    case PRIM_LINESTRIP:
        if (vcount == 2) { if (kick) draw(type, &vq[0], &vq[1], NULL); vq[0] = vq[1]; vcount = 1; }
        break;
    case PRIM_TRI:
        if (vcount == 3) { if (kick) draw(type, &vq[0], &vq[1], &vq[2]); vcount = 0; }
        break;
    case PRIM_TRISTRIP:
        if (vcount == 3) { if (kick) draw(type, &vq[0], &vq[1], &vq[2]); vq[0] = vq[1]; vq[1] = vq[2]; vcount = 2; }
        break;
    case PRIM_TRIFAN:
        if (vcount == 3) { if (kick) draw(type, &vq[0], &vq[1], &vq[2]); vq[1] = vq[2]; vcount = 2; }
        break;
    default:
        log_once("PRIM type 7 (reserved) kicked", 2);
        vcount = 0;
        break;
    }
}

/* ---- transfers ------------------------------------------------------------------------------------ */

/* Marks the destination pages of the current transfer. Data arriving later only needs another
 * mark if some texture was decoded in between (it may have read the partly written area). */
static uint32_t xfer_marked_at;          /* gs_tex_decodes when last marked */

static void mark_xfer_dest(void) {
    int x0 = xfer_x0, x1 = xfer_x1, y0 = xfer_y0, y1 = xfer_y1;
    if (x1 > 2048) { x0 = 0; x1 = 2048; }
    if (y1 > 2048) { y0 = 0; y1 = 2048; }
    gs_mark_rect(xfer_dpsm, xfer_dbp, xfer_dbw, x0, y0, x1, y1);
    xfer_marked_at = gs_tex_decodes;
}

static void trxdir(uint32_t dir) {
    uint64_t bb = reg[R_BITBLTBUF], pos = reg[R_TRXPOS], rr = reg[R_TRXREG];
    xfer_dir = (int)dir;
    xfer_dbp = (uint32_t)((bb >> 32) & 0x3FFF);
    xfer_dbw = (uint32_t)((bb >> 48) & 0x3F);
    xfer_dpsm = (int)((bb >> 56) & 0x3F);
    xfer_x0 = (int)((pos >> 32) & 0x7FF);
    xfer_y0 = (int)((pos >> 48) & 0x7FF);
    xfer_x1 = xfer_x0 + (int)(rr & 0xFFF);
    xfer_y1 = xfer_y0 + (int)((rr >> 32) & 0xFFF);
    gs_stats.xfers += dir != 3;
    gs_mem_xfer_start(bb, pos, rr, dir);
    if (dir == 0 || dir == 2) mark_xfer_dest();
}

void gs_exec_image_write(const void* data, uint32_t bytes) {
    GS_INIT();
    gs_stats.image_bytes += bytes;
    if (xfer_dir == 0 && xfer_marked_at != gs_tex_decodes) mark_xfer_dest();
    gs_mem_xfer_write(data, bytes);
}

uint32_t gs_exec_image_read(void* data, uint32_t bytes) {
    return gs_mem_xfer_read(data, bytes);
}

void gs_draw_reset(void) {
    vcount = 0;
    env_dirty = 1;
    if (xfer_dir != 3) gs_mem_xfer_start(reg[R_BITBLTBUF], reg[R_TRXPOS], reg[R_TRXREG], 3);
    xfer_dir = 3;
}

/* ---- register writes ------------------------------------------------------------------------------ */

static void tex0_write(int ctx, uint64_t v) {
    reg[R_TEX0_1 + ctx] = v;
    gs_tex0 t = decode_tex0(v);
    gs_texclut tc = decode_texclut(reg[R_TEXCLUT]);
    gs_clut_on_tex0(&t, &tc);
    if (t.cld) gs_clut_version++;
    env_dirty = 1;
}

void gs_exec_write_reg(uint32_t r, uint64_t v) {
    GS_INIT();
    gs_stats.regs++;
    switch (r) {
    case R_PRIM:
        /* the type only matters to the vertex queue; attributes only when PRMODECONT.AC = 1 */
        if ((reg[R_PRMODECONT] & 1) && ((reg[r] ^ v) & 0x7F8)) env_dirty = 1;
        reg[r] = v;
        vcount = 0;
        break;
    case R_RGBAQ: case R_ST: case R_UV: case R_FOG:
        reg[r] = v;
        break;
    case R_XYZF2: vertex(v, 1, 1); break;
    case R_XYZ2: vertex(v, 0, 1); break;
    case R_XYZF3: vertex(v, 1, 0); break;
    case R_XYZ3: vertex(v, 0, 0); break;
    case R_TEX0_1: case R_TEX0_2: tex0_write((int)(r - R_TEX0_1), v); break;
    case R_TEX2_1: case R_TEX2_2: {
        /* PSM and the CLUT fields of TEX0 */
        const uint64_t m = (0x3Full << 20) | (~0ull << 37);
        int ctx = (int)(r - R_TEX2_1);
        tex0_write(ctx, (reg[R_TEX0_1 + ctx] & ~m) | (v & m));
        break;
    }
    case R_CLAMP_1: case R_CLAMP_2: case R_TEX1_1: case R_TEX1_2: case R_XYOFFSET_1: case R_XYOFFSET_2:
    case R_PRMODECONT: case R_PRMODE: case R_SCANMSK: case R_MIPTBP1_1: case R_MIPTBP1_2:
    case R_MIPTBP2_1: case R_MIPTBP2_2: case R_TEXA: case R_FOGCOL: case R_SCISSOR_1: case R_SCISSOR_2:
    case R_ALPHA_1: case R_ALPHA_2: case R_DIMX: case R_DTHE: case R_COLCLAMP: case R_TEST_1:
    case R_TEST_2: case R_PABE: case R_FBA_1: case R_FBA_2: case R_FRAME_1: case R_FRAME_2:
    case R_ZBUF_1: case R_ZBUF_2:
        if (reg[r] != v) { reg[r] = v; env_dirty = 1; }
        break;
    case R_TEXCLUT: case R_BITBLTBUF: case R_TRXPOS: case R_TRXREG:
        reg[r] = v;
        break;
    case R_TEXFLUSH:
        break;                           /* the texture cache tracks VRAM writes itself */
    case R_TRXDIR:
        reg[r] = v;
        trxdir((uint32_t)(v & 3));
        break;
    case R_HWREG:
        if (xfer_dir == 0 && xfer_marked_at != gs_tex_decodes) mark_xfer_dest();
        gs_mem_xfer_write(&v, 8);
        break;
    case R_SIGNAL: gs_csr_signal((uint32_t)v, (uint32_t)(v >> 32)); break;
    case R_FINISH: gs_csr_finish(); break;
    case R_LABEL: gs_csr_label((uint32_t)v, (uint32_t)(v >> 32)); break;
    case 0x0E: case 0x0F:                /* A+D / NOP descriptors: nothing to do */
        break;
    default:
        if (r < 0x64) reg[r] = v;
        {
            char msg[64];
            snprintf(msg, sizeof msg, "write to unknown general register 0x%02X", r);
            log_once(msg, 0x1000 + r);
        }
        break;
    }
}
