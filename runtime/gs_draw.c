/* GS general registers (0x00-0x63), vertex queue, primitive setup and rasterization.
 *
 * Coordinates: XYZ X/Y are unsigned 12.4 fixed point; XYOFFSET is subtracted at drawing time. A
 * pixel (px, py) is sampled at (16*px, 16*py) in that space. Triangles use exact integer edge
 * functions with a top-left fill rule; sprites cover [X0, X1) x [Y0, Y1). Attributes are
 * interpolated as planes evaluated at the first pixel of each span, then stepped per pixel. */
#include "gs_setup.h"
#include "gs_hint.h"

#include <math.h>
#include <stdlib.h>

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

typedef gs_vtx vtx;                      /* gs_setup.h */

gs_stats_t gs_stats;
uint8_t gs_fb_yfrac[512] = { [0 ... 511] = 0xFF }, gs_fb_psm[512];
static uint64_t reg[0x64];
static vtx vq[3];
static uint32_t vtx_serial;
static int vcount;
static int env_dirty = 1, env_ok;
static gs_prim P;
static int ofx, ofy;                     /* XYOFFSET of the drawing context, 12.4 */
static int prim_draws;                   /* 0 when nothing can be written */
static int prim_aa1;                     /* PRIM.AA1 of the current drawing environment */
static int prim_iip;                     /* PRIM.IIP (P.iip is also set for AA1: per-pixel alpha) */
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
        /* (the texture cache binding is the software rasterizer's; a frontend alone needs none) */
        if ((P.texm == 3 || l == P.cur_level) && gs_sw_draw) gs_tex_bind(&P.lvl[l], &t, &ta, clamp, l);
    }
}

static void env_update(void) {
    env_dirty = 0;
    uint64_t attr = (reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE];
    int ctx = (int)((attr >> 9) & 1);
    prim_iip = (int)((attr >> 3) & 1);
    P.iip = prim_iip || ((attr >> 7) & 1);   /* AA1: per-pixel alpha (coverage) even when flat */
    P.tme = (int)((attr >> 4) & 1);
    P.fge = (int)((attr >> 5) & 1);
    P.abe = (int)((attr >> 6) & 1);
    P.fst = (int)((attr >> 8) & 1);
    prim_aa1 = (int)((attr >> 7) & 1);   /* lines: coverage alpha (draw_line); others: ignored */

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
    gs_fb_yfrac[P.fpage] = (uint8_t)(ofy & 15);
    gs_fb_psm[P.fpage] = (uint8_t)P.fpsm;

    if (P.tme) bind_textures(ctx);
    else P.texm = 0;
    gs_prim_finalize(&P);
    prim_draws = P.fbmsk != 0xFFFFFFFFu || (P.zuse && !P.zmsk);
    gs_prim_version++;                   /* deferred drawing snapshots the new state */
}

/* ---- attribute planes ---------------------------------------------------------------------------- */

/* (planes, row stepping, edges, the sprite rule and the line DDA: gs_setup.h/.c, shared with the
 * GPU frontend) */
static void set_steps(const gs_planes* pl) {
    P.dr = gs_d2i32(pl->r.cx * 65536.0); P.dg = gs_d2i32(pl->g.cx * 65536.0);
    P.db = gs_d2i32(pl->b.cx * 65536.0); P.da = gs_d2i32(pl->a.cx * 65536.0);
    P.df = gs_d2i32(pl->f.cx * 65536.0);
    P.dz = gs_d2i64(pl->z.cx * 65536.0);
    P.ds = (float)pl->s.cx; P.dt = (float)pl->t.cx; P.dq = (float)pl->q.cx;
    P.du = gs_d2i32(pl->u.cx * (double)(1 << UVSH)); P.dv = gs_d2i32(pl->v.cx * (double)(1 << UVSH));
}

/* ---- texture preparation per primitive -------------------------------------------------------- */

static void add_buf_pages(uint32_t page0, uint32_t bw, int ph_sh, int x0, int y0, int x1, int y1, uint64_t set[8]) {
    for (int r = y0 >> ph_sh; r <= y1 >> ph_sh; r++)
        for (int c = x0 >> 6; c <= x1 >> 6; c++) {
            uint32_t pg = (page0 + (uint32_t)r * bw + (uint32_t)c) & 511u;
            set[pg >> 6] |= 1ull << (pg & 63);
        }
}

/* Every texel of the (unwrapped, inclusive) range [u0,u1] x [v0,v1] is inside the texture and its
 * clamp region, so it is sampled at its own coordinates: no REPEAT wrap, no clamping. */
static int texels_unwrapped(const gs_texlvl* L, int u0, int u1, int v0, int v1) {
    return L->umode != 3 && L->vmode != 3 && u0 >= 0 && v0 >= 0 && u0 >= L->ulo && v0 >= L->vlo &&
           u1 <= L->w - 1 && v1 <= L->h - 1 && u1 <= L->uhi && v1 <= L->vhi;
}

/* Texel-space bounding box of the vertices (level 0), then per level: sample directly (recording
 * the pages read for deferred drawing) - from a snapshot of the pages when the texels are in pages
 * the primitive writes (inclusive pixel box x0..x1, y0..y1) - or decode what it can reach into the
 * texture cache (formats without direct sampling). `local`: the caller found that each pixel
 * samples the texel at its own position (draw_sprite); that is a pixel-local filter only if the
 * texture also covers the whole range unwrapped and lies on the frame buffer itself. */
static void tex_prepare(const vtx* const* vs, int n, int as_uv, const double* su, const double* sv,
                        int x0, int y0, int x1, int y1, int local) {
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
    uint64_t reads[8] = { 0 }, snap[8] = { 0 }, dest[8];
    int have_dest = 0, any_reads = 0, any_snap = 0;
    /* The texels that contribute: point sampling reads texel floor(c), bilinear floor(c - 0.5) and,
     * unless c - 0.5 is a whole number (weight 0), the next one, for c between the extremes above
     * (the vertices; for sprites the exact values the pipeline uses at the first and last pixel).
     * EPS covers the interpolation of triangles. The range must be tight: an extra texel row
     * reaching into the destination buffer would make the primitive sample a snapshot (feedback),
     * and with REPEAT a margin below 0 wraps to the far end of the texture. */
    const double EPS = as_uv ? 0 : 1.0 / 16;
    const int bil = P.texm != 1;
    for (int l = lo; l <= hi; l++) {
        gs_texlvl* L = &P.lvl[l];
        int u0 = 0, u1 = L->w - 1, v0 = 0, v1 = L->h - 1;
        if (!all) {
            double sc = 1.0 / (double)(1 << l);
            if (bil) {
                u0 = (int)floor(umin * sc - 0.5 - EPS); u1 = (int)ceil(umax * sc - 0.5 + EPS);
                v0 = (int)floor(vmin * sc - 0.5 - EPS); v1 = (int)ceil(vmax * sc - 0.5 + EPS);
            } else {
                u0 = (int)floor(umin * sc - EPS); u1 = (int)floor(umax * sc + EPS);
                v0 = (int)floor(vmin * sc - EPS); v1 = (int)floor(vmax * sc + EPS);
            }
        }
        if (L->dkind >= 0 && local && !all && texels_unwrapped(L, u0, u1, v0, v1) && L->tab == P.ffmt->tab &&
            L->bp == P.fpage * 32 && L->t.tbw == P.fbw) {
            /* every pixel samples its own memory (an in-place filter on the frame buffer): like
             * blending, which reads the destination, the reads are ordered with the writes by the
             * band that owns the pages; no snapshot, nothing to record. (Beyond the texture, CLAMP
             * or REPEAT would make pixels sample other pixels, in other bands: feedback, below.) */
            gs_tex_use_snapshot(L, 0);
            continue;
        }
        if (L->dkind >= 0) {
            uint64_t rp[8] = { 0 };
            gs_tex_read_pages(L, u0, u1, v0, v1, rp);
            if (!have_dest) {
                memset(dest, 0, sizeof dest);
                if (P.fbmsk != 0xFFFFFFFFu) add_buf_pages(P.fpage, P.fbw, P.ffmt->ph_shift, x0, y0, x1, y1, dest);
                if (P.zuse && !P.zmsk) add_buf_pages(P.zpage, P.zbw, P.zfmt->ph_shift, x0, y0, x1, y1, dest);
                have_dest = 1;
            }
            int feedback = 0;
            for (int k = 0; k < 8; k++) feedback |= (rp[k] & dest[k]) != 0;
            gs_tex_use_snapshot(L, feedback);
            uint64_t* to = feedback ? snap : reads;
            for (int k = 0; k < 8; k++) to[k] |= rp[k];
            if (feedback) any_snap = 1; else any_reads = 1;
        }
        if (L->kind >= 0) continue;
        /* the cache: one more texel of margin, which costs nothing there */
        if (!all) { u0--; u1++; v0--; v1++; }
        gs_tex_ensure(L, u0, u1, v0, v1);
        /* every texel the primitive can reach is inside the texture and the clamp region: sampling
         * (from the cache) needs no wrapping */
        if (!all && P.texm != 3 && texels_unwrapped(L, u0, u1, v0, v1)) P.nowrap = 1;
    }
    gs_draw_set_reads(any_reads ? reads : NULL, any_snap ? snap : NULL);
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
    gs_sprite s;
    if (!gs_sprite_setup(&s, v0, v1, ofx, ofy, P.tme, P.fst, P.tw16, P.th16, P.scx0, P.scx1, P.scy0, P.scy1)) return;
    int px0 = s.px0, px1 = s.px1, py0 = s.py0, py1 = s.py1;
    gs_planes* pl = &s.pl;
    if (P.tme) {
        P.lod_q = v1->q;
        int32_t ua = s.ua, du = s.du, ub = s.ub, va = s.va, vb = s.vb;
        double su[2] = { (double)(ua >> UVSH), (double)(ub >> UVSH) }, sv[2] = { (double)(va >> UVSH), (double)(vb >> UVSH) };
        /* One texel per pixel, every pixel sampling the texel at its own coordinates (with bilinear
         * filtering: exactly at its centre, the neighbours weighing 0): tex_prepare can treat an
         * in-place filter on the frame buffer like blending, which reads the destination. Rows
         * have the same fraction when the V step is exactly one texel. */
        const int bil = P.texm == 2, off = bil ? 8 : 0;
        int ou = (ua >> UVSH) - off - 16 * px0, ov = (va >> UVSH) - off - 16 * py0;
        int local = (P.texm == 1 || P.texm == 2) && P.cur_level == 0 &&
                    (px1 == px0 || du == 16 << UVSH) && (py1 == py0 || pl->v.cy == 16.0) &&
                    (bil ? ou == 0 && ov == 0 : ou >= 0 && ou < 16 && ov >= 0 && ov < 16);
        tex_prepare(NULL, 2, 1, su, sv, px0, py0, px1, py1, local);
    }
    set_steps(pl);
    gs_batch_begin(&P, 0);               /* u along x, v along y: no per-pixel division */
    gs_span_start st;
    for (int py = py0; py <= py1; py++) {
        if (row_masked(py)) continue;
        gs_span_start_at(pl, px0, py, &st);
        gs_batch_span(py, px0, px1 - px0 + 1, &st);
    }
    gs_batch_end();
    mark_written(px0, py0, px1, py1);
}

static void draw_triangle(const vtx* v0, const vtx* v1, const vtx* v2) {
    gs_tri T;
    if (!gs_tri_setup(&T, v0, v1, v2, ofx, ofy, P.scx0, P.scx1, P.scy0, P.scy1)) return;
    int bx0 = T.bx0, bx1 = T.bx1, by0 = T.by0, by1 = T.by1;

    /* attribute planes (pixel units) */
    const vtx* vs[3] = { v0, v1, v2 };
    gs_planes pl;
    gs_tri_planes(&T, vs, prim_iip, P.fge, P.zuse, P.tme, P.fst, &pl);
    if (P.tme) {
        if (P.fst) P.lod_q = v2->q;
        tex_prepare(vs, 3, 0, NULL, NULL, bx0, by0, bx1, by1, 0);
    }
    gs_rowstep rs;
    gs_rowstep_init(&rs, &pl, by0);
    P.dr = (int32_t)rs.dx[GS_A_R]; P.dg = (int32_t)rs.dx[GS_A_G]; P.db = (int32_t)rs.dx[GS_A_B]; P.da = (int32_t)rs.dx[GS_A_A];
    P.df = (int32_t)rs.dx[GS_A_F]; P.dz = rs.dx[GS_A_Z]; P.du = (int32_t)rs.dx[GS_A_U]; P.dv = (int32_t)rs.dx[GS_A_V];
    P.ds = (float)rs.sdx; P.dt = (float)rs.tdx; P.dq = (float)rs.qdx;

    gs_edge e[3];
    gs_tri_edges(&T, e);
    gs_span_start st;
    gs_batch_begin(&P, P.tme && !P.fst);
    int rx0 = bx1, rx1 = bx0;
    for (int py = by0; py <= by1; py++, gs_tri_edges_next(e), gs_rowstep_next(&rs)) {
        if (P.scanmsk >= 2 && row_masked(py)) continue;
        int64_t xl, xr;
        gs_tri_row(e, bx0, bx1, &xl, &xr);
        if (xl > xr) continue;
        gs_rowstep_start(&rs, xl, &st);
        gs_batch_span(py, (int)xl, (int)(xr - xl + 1), &st);
        if (xl < rx0) rx0 = (int)xl;
        if (xr > rx1) rx1 = (int)xr;
    }
    gs_batch_end();
    if (rx0 <= rx1) mark_written(rx0, by0, rx1, by1);
}

static void draw_line(const vtx* v0, const vtx* v1) {
    gs_line L;
    if (!gs_line_setup(&L, v0, v1, ofx, ofy)) return;
    int64_t X0 = L.X0, Y0 = L.Y0, X1 = L.X1, Y1 = L.Y1;

    if (P.tme) {
        const vtx* vs[2] = { v0, v1 };
        P.lod_q = v1->q;
        int lx0 = imax((int)((X0 < X1 ? X0 : X1) >> 4) - 1, P.scx0), lx1 = imin((int)((X0 < X1 ? X1 : X0) >> 4) + 1, P.scx1);
        int ly0 = imax((int)((Y0 < Y1 ? Y0 : Y1) >> 4) - 1, P.scy0), ly1 = imin((int)((Y0 < Y1 ? Y1 : Y0) >> 4) + 1, P.scy1);
        tex_prepare(vs, 2, 0, NULL, NULL, lx0, ly0, lx1, ly1, 0);
    }
    gs_planes pl;                        /* evaluated with x = t (0..1 along the line), y = 0 */
    gs_line_planes(v0, v1, prim_iip, &pl);
    P.dr = P.dg = P.db = P.da = P.df = 0; P.dz = 0; P.ds = P.dt = P.dq = 0; P.du = P.dv = 0;
    gs_span_start st;
    int mx0 = 2047, mx1 = 0, my0 = 2047, my1 = 0;
    /* AA1 (with alpha blending): the GS antialiases a line by covering the two pixels that straddle
     * it across the minor axis, each with its share of the line as coverage, and the coverage
     * replaces the source alpha (0x80 = fully covered). Sly 2 draws its character outlines this way
     * (Z test GREATER, RGB only): only the half outside the silhouette shows, so drawing one pixel
     * per step instead loses whole stretches of outline. */
    int aa = prim_aa1 && P.abe;
    gs_batch_begin(&P, P.tme && !P.fst);
    for (int p = L.p0; p <= L.p1; p++) {
        int q, qn, cov[2];
        double t = gs_line_step(&L, p, aa, &q, cov, &qn);
        for (int k = 0; k < qn; k++) {
            if (!cov[k]) continue;
            int px = L.xmajor ? p : q + k, py = L.xmajor ? q + k : p;
            if (px < P.scx0 || px > P.scx1 || py < P.scy0 || py > P.scy1 || row_masked(py)) continue;
            gs_span_start_at(&pl, t, 0, &st);
            if (aa) st.a = cov[k] << 16;
            gs_batch_span(py, px, 1, &st);
            mx0 = imin(mx0, px); mx1 = imax(mx1, px); my0 = imin(my0, py); my1 = imax(my1, py);
        }
    }
    gs_batch_end();
    if (mx0 <= mx1) mark_written(mx0, my0, mx1, my1);
}

static void draw_point(const vtx* v) {
    int px, py;
    gs_point_pixel(v, ofx, ofy, &px, &py);
    if (px < P.scx0 || px > P.scx1 || py < P.scy0 || py > P.scy1 || row_masked(py)) return;
    if (P.tme) {
        const vtx* vs[1] = { v };
        P.lod_q = v->q;
        tex_prepare(vs, 1, 0, NULL, NULL, px, py, px, py, 0);
    }
    gs_span_start st = { v->r << 16, v->g << 16, v->b << 16, v->a << 16, v->f << 16, (int64_t)v->z << 16,
                         v->s, v->t, v->q, v->u << UVSH, v->v << UVSH };
    P.dr = P.dg = P.db = P.da = P.df = 0; P.dz = 0; P.ds = P.dt = P.dq = 0; P.du = P.dv = 0;
    gs_batch_begin(&P, P.tme && !P.fst);
    gs_batch_span(py, px, 1, &st);
    gs_batch_end();
    mark_written(px, py, px, py);
}

/* ---- debug trace: PS2_GS_TRACE_RECT=x0,y0,x1,y1 ----------------------------------------------------
 * Logs every primitive whose vertex bounding box touches that rectangle (drawing-buffer pixels after
 * XYOFFSET, inclusive) with its vertices, the drawing environment and, for textured primitives, the
 * texels it covers (at most 32x32, as gs_tex_fetch returns them: 0xAABBGGRR after CLUT/TEXA; indexed
 * formats also show the raw indices). PS2_GS_TRACE_FROM=<vsync> starts later, PS2_GS_TRACE_MAX=<n>
 * (default 64) limits the number of primitives logged, PS2_GS_TRACE_TEXELS=0 leaves out the texels,
 * PS2_GS_TRACE_BRIEF=1 prints one line per primitive, PS2_GS_TRACE_MINSIZE=<n> skips primitives whose
 * bounding box is under n pixels in both directions. Output goes to stderr, or (much faster, fully
 * buffered) to the file PS2_GS_TRACE_OUT names. */
static int trace_on = -1, trace_x0, trace_y0, trace_x1, trace_y1, trace_left, trace_texels_on, trace_brief;
static int trace_minsize;
static uint32_t trace_from;
static FILE* trace_out;

static void trace_init(void) {
    const char* o = getenv("PS2_GS_TRACE_OUT");
    trace_out = o ? fopen(o, "w") : NULL;
    if (!trace_out) trace_out = stderr;
    const char* r = getenv("PS2_GS_TRACE_RECT");
    trace_on = r && sscanf(r, "%d,%d,%d,%d", &trace_x0, &trace_y0, &trace_x1, &trace_y1) == 4;
    const char* f = getenv("PS2_GS_TRACE_FROM");
    const char* m = getenv("PS2_GS_TRACE_MAX");
    const char* t = getenv("PS2_GS_TRACE_TEXELS");
    const char* b = getenv("PS2_GS_TRACE_BRIEF");
    trace_from = f ? (uint32_t)strtoul(f, NULL, 0) : 0;
    trace_left = m ? atoi(m) : 64;
    trace_texels_on = !t || atoi(t) != 0;
    trace_brief = b && atoi(b) != 0;
    const char* ms = getenv("PS2_GS_TRACE_MINSIZE");
    trace_minsize = ms ? atoi(ms) : 0;
}

static void trace_texels(double umin, double umax, double vmin, double vmax) {
    gs_tex0 t = decode_tex0(reg[R_TEX0_1 + ((((reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE]) >> 9) & 1)]);
    gs_texa ta = decode_texa(reg[R_TEXA]);
    int u0 = (int)floor(umin), u1 = (int)ceil(umax) - 1, v0 = (int)floor(vmin), v1 = (int)ceil(vmax) - 1;
    if (u1 < u0) u1 = u0;
    if (v1 < v0) v1 = v0;
    if (u1 - u0 > 31) u1 = u0 + 31;
    if (v1 - v0 > 31) v1 = v0 + 31;
    int indexed = t.psm == PSMT8 || t.psm == PSMT4 || t.psm == PSMT8H || t.psm == PSMT4HL || t.psm == PSMT4HH;
    fprintf(trace_out, "[gstrace]   texels u %d..%d v %d..%d (AABBGGRR%s)\n", u0, u1, v0, v1, indexed ? ", index" : "");
    int tw = 1 << (t.tw > 10 ? 10 : t.tw), th = 1 << (t.th > 10 ? 10 : t.th);
    for (int v = v0; v <= v1; v++) {
        char line[32 * 13 + 32];
        int n = snprintf(line, sizeof line, "[gstrace]   v%4d:", v);
        for (int u = u0; u <= u1; u++) {
            int uu = u & (tw - 1), vv = v & (th - 1);
            uint32_t c = gs_tex_fetch(&t, &ta, uu, vv);
            if (indexed)
                n += snprintf(line + n, sizeof line - (size_t)n, " %08X/%02X", c,
                              gs_mem_read((int)t.psm, t.tbp0, t.tbw, uu, vv) & 0xFF);
            else
                n += snprintf(line + n, sizeof line - (size_t)n, " %08X", c);
            if (n >= (int)sizeof line - 16) break;
        }
        fprintf(trace_out, "%s\n", line);
    }
}

static void trace_prim(int type, const vtx* const* vs, int nv) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < nv; i++) {
        int x = (vs[i]->x - ofx) >> 4, y = (vs[i]->y - ofy) >> 4;
        x0 = imin(x0, x); x1 = imax(x1, x); y0 = imin(y0, y); y1 = imax(y1, y);
    }
    if (x1 < trace_x0 || x0 > trace_x1 || y1 < trace_y0 || y0 > trace_y1) return;
    if (x1 - x0 < trace_minsize && y1 - y0 < trace_minsize) return;
    trace_left--;
    static const char* names[8] = { "point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "prim7" };
    uint64_t attr = (reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE];
    int ctx = (int)((attr >> 9) & 1);
    if (trace_brief) {
        uint64_t fr = reg[R_FRAME_1 + ctx], al = reg[R_ALPHA_1 + ctx], t0 = reg[R_TEX0_1 + ctx];
        fprintf(trace_out, "[gstrace] v%u %-9s %4d,%4d..%4d,%4d rgba %02X%02X%02X%02X z %u FBP %u/%02X msk %08X%s%s ABE %d "
                        "ALPHA %u%u%u%u/%02X TME %d TBP %u/%02X TFX %u TCC %u\n",
                gs_stream_vsync, names[type & 7], x0, y0, x1, y1, vs[nv - 1]->r, vs[nv - 1]->g, vs[nv - 1]->b,
                vs[nv - 1]->a, vs[nv - 1]->z, (unsigned)(fr & 0x1FF), (unsigned)((fr >> 24) & 0x3F),
                (unsigned)(fr >> 32), (attr >> 3) & 1 ? " gouraud" : "", (attr >> 7) & 1 ? " AA1" : "", (int)(attr >> 6) & 1,
                (unsigned)(al & 3), (unsigned)((al >> 2) & 3), (unsigned)((al >> 4) & 3), (unsigned)((al >> 6) & 3),
                (unsigned)((al >> 32) & 0xFF), (int)(attr >> 4) & 1, (unsigned)(t0 & 0x3FFF),
                (unsigned)((t0 >> 20) & 0x3F), (unsigned)((t0 >> 35) & 3), (unsigned)((t0 >> 34) & 1));
        return;
    }
    fprintf(trace_out, "[gstrace] v%u %s IIP %d TME %d FGE %d ABE %d AA1 %d FST %d CTXT %d FIX %d\n", gs_stream_vsync,
            names[type & 7], (int)(attr >> 3) & 1, (int)(attr >> 4) & 1, (int)(attr >> 5) & 1, (int)(attr >> 6) & 1,
            (int)(attr >> 7) & 1, (int)(attr >> 8) & 1, ctx, (int)(attr >> 10) & 1);
    for (int i = 0; i < nv; i++) {
        const vtx* v = vs[i];
        fprintf(trace_out, "[gstrace]   xy (%.4f, %.4f) z %u rgba %02X %02X %02X %02X q %g st %g %g uv %.4f %.4f f %d\n",
                (v->x - ofx) / 16.0, (v->y - ofy) / 16.0, v->z, v->r, v->g, v->b, v->a, v->q, v->s, v->t,
                v->u / 16.0, v->v / 16.0, v->f);
    }
    uint64_t fr = reg[R_FRAME_1 + ctx], zb = reg[R_ZBUF_1 + ctx], te = reg[R_TEST_1 + ctx], al = reg[R_ALPHA_1 + ctx];
    uint64_t sc = reg[R_SCISSOR_1 + ctx];
    fprintf(trace_out, "[gstrace]   FRAME FBP %u FBW %u PSM %02X FBMSK %08X  ZBUF ZBP %u PSM %02X ZMSK %u  "
                    "SCISSOR %u..%u,%u..%u  XYOFFSET %.4f,%.4f\n",
            (unsigned)(fr & 0x1FF), (unsigned)((fr >> 16) & 0x3F), (unsigned)((fr >> 24) & 0x3F), (unsigned)(fr >> 32),
            (unsigned)(zb & 0x1FF), (unsigned)((zb >> 24) & 0xF), (unsigned)((zb >> 32) & 1),
            (unsigned)(sc & 0x7FF), (unsigned)((sc >> 16) & 0x7FF), (unsigned)((sc >> 32) & 0x7FF), (unsigned)((sc >> 48) & 0x7FF),
            ofx / 16.0, ofy / 16.0);
    fprintf(trace_out, "[gstrace]   TEST ATE %u ATST %u AREF %02X AFAIL %u DATE %u DATM %u ZTE %u ZTST %u  "
                    "ALPHA A %u B %u C %u D %u FIX %02X  PABE %u FBA %u COLCLAMP %u DTHE %u\n",
            (unsigned)(te & 1), (unsigned)((te >> 1) & 7), (unsigned)((te >> 4) & 0xFF), (unsigned)((te >> 12) & 3),
            (unsigned)((te >> 14) & 1), (unsigned)((te >> 15) & 1), (unsigned)((te >> 16) & 1), (unsigned)((te >> 17) & 3),
            (unsigned)(al & 3), (unsigned)((al >> 2) & 3), (unsigned)((al >> 4) & 3), (unsigned)((al >> 6) & 3),
            (unsigned)((al >> 32) & 0xFF), (unsigned)(reg[R_PABE] & 1), (unsigned)(reg[R_FBA_1 + ctx] & 1),
            (unsigned)(reg[R_COLCLAMP] & 1), (unsigned)(reg[R_DTHE] & 1));
    if (!((attr >> 4) & 1)) return;
    gs_tex0 t = decode_tex0(reg[R_TEX0_1 + ctx]);
    gs_texa ta = decode_texa(reg[R_TEXA]);
    uint64_t t1 = reg[R_TEX1_1 + ctx], cl = reg[R_CLAMP_1 + ctx];
    fprintf(trace_out, "[gstrace]   TEX0 TBP0 %u TBW %u PSM %02X TW %u TH %u TCC %u TFX %u CBP %u CPSM %02X CSM %u CSA %u CLD %u"
                    "  TEXA TA0 %02X AEM %u TA1 %02X\n",
            t.tbp0, t.tbw, t.psm, t.tw, t.th, t.tcc, t.tfx, t.cbp, t.cpsm, t.csm, t.csa, t.cld, ta.ta0, ta.aem, ta.ta1);
    fprintf(trace_out, "[gstrace]   TEX1 LCM %u MXL %u MMAG %u MMIN %u MTBA %u L %u K %d  CLAMP WMS %u WMT %u U %u..%u V %u..%u\n",
            (unsigned)(t1 & 1), (unsigned)((t1 >> 2) & 7), (unsigned)((t1 >> 5) & 1), (unsigned)((t1 >> 6) & 7),
            (unsigned)((t1 >> 9) & 1), (unsigned)((t1 >> 19) & 3), (int)(((int64_t)(t1 << 20)) >> 52),
            (unsigned)(cl & 3), (unsigned)((cl >> 2) & 3), (unsigned)((cl >> 4) & 0x3FF), (unsigned)((cl >> 14) & 0x3FF),
            (unsigned)((cl >> 24) & 0x3FF), (unsigned)((cl >> 34) & 0x3FF));
    double umin = 1e30, umax = -1e30, vmin = 1e30, vmax = -1e30;
    for (int i = 0; i < nv; i++) {
        double u, v;
        if ((attr >> 8) & 1) { u = vs[i]->u / 16.0; v = vs[i]->v / 16.0; }
        else { u = vs[i]->s / vs[i]->q * (double)(1 << t.tw); v = vs[i]->t / vs[i]->q * (double)(1 << t.th); }
        if (!(fabs(u) < 1e6) || !(fabs(v) < 1e6)) return;
        umin = fmin(umin, u); umax = fmax(umax, u); vmin = fmin(vmin, v); vmax = fmax(vmax, v);
    }
    if (trace_texels_on) trace_texels(umin, umax, vmin, vmax);
}

/* ---- side-band hints (GS register 0x7F, gs_hint.h) ----------------------------------------------
 * HUD_BEGIN/END: X of every vertex in between is scaled about the anchor (left edge, centre or right
 * edge of the drawing buffer's FRAME width, after XYOFFSET), so 2D content drawn for a 4:3 screen
 * keeps its proportions in a picture shown stretched to a wider aspect. ASPECT_4_3_BEGIN/END mark
 * pictures the presenter shows pillarboxed at 4:3 (gs_hint_content43). Nothing else changes: a
 * stream without hints (every 4:3 stream) draws exactly as before. */
static int hud_on, hud_anchor, hint_43;
static uint32_t hint_phase;              /* label of the last PHASE hint (GPU frontend: phase labels of its draws) */
static double hud_scale = 1.0;

/* PS2_HUD_LOG=1 (with the EE side, aspect.c): PHASE hints label the 2D render-list entries by scene
 * (or creator | 0x80000000); the X/Y extent (pixels, before the HUD transform) drawn under each label
 * is printed every 60th frame. Diagnostics only. */
static int hudlog = -1;
static uint32_t hudlog_label, hudlog_frame;
static struct { uint32_t label; int32_t x0, x1, y0, y1, n, anchor; } hudlog_t[64];
static int hudlog_n;

static void hudlog_prim(const vtx* const* in, int n, uint64_t xyo) {
    if (!hudlog_label || hudlog_label == 0xFFFFFFFFu) return;
    int k = 0;
    while (k < hudlog_n && hudlog_t[k].label != hudlog_label) k++;
    if (k == hudlog_n) {
        if (hudlog_n == 64) return;
        hudlog_n++;
        hudlog_t[k].label = hudlog_label;
        hudlog_t[k].x0 = hudlog_t[k].y0 = 1 << 30;
        hudlog_t[k].x1 = hudlog_t[k].y1 = -(1 << 30);
        hudlog_t[k].n = 0;
    }
    hudlog_t[k].anchor = hud_on ? hud_anchor : -1;
    for (int i = 0; i < n; i++) {
        int32_t x = (in[i]->x - (int32_t)(xyo & 0xFFFF)) / 16, y = (in[i]->y - (int32_t)((xyo >> 32) & 0xFFFF)) / 16;
        if (x < hudlog_t[k].x0) hudlog_t[k].x0 = x;
        if (x > hudlog_t[k].x1) hudlog_t[k].x1 = x;
        if (y < hudlog_t[k].y0) hudlog_t[k].y0 = y;
        if (y > hudlog_t[k].y1) hudlog_t[k].y1 = y;
    }
    hudlog_t[k].n++;
}

static void hudlog_phase(uint32_t label) {
    if (hudlog < 0) { const char* e = getenv("PS2_HUD_LOG"); hudlog = e && *e && *e != '0'; }
    if (!hudlog) return;
    if (label == 0xFFFFFFFFu) {
        if (hudlog_frame++ % 60 == 0) {
            fprintf(stderr, "[hudlog] v%u:", gs_stream_vsync);
            for (int k = 0; k < hudlog_n; k++)
                fprintf(stderr, " %08X[%d..%d,%d..%d n%d a%d]", hudlog_t[k].label, hudlog_t[k].x0, hudlog_t[k].x1,
                        hudlog_t[k].y0, hudlog_t[k].y1, hudlog_t[k].n, hudlog_t[k].anchor);
            fprintf(stderr, "\n");
        }
        hudlog_n = 0;
    }
    hudlog_label = label;
}

int gs_hint_content43(void) { return hint_43; }

/* The software rasterizer on / off while the GS is idle (M6: live switch between the GPU backend and
 * this one): textures are rebound and the state re-derived at the next primitive. */
void gs_draw_set_sw(int on) {
    gs_sw_draw = on != 0;
    gs_tex_flush_all();
    env_dirty = 1;
}

/* x: 12.4 window coordinate as written to XYZ (before XYOFFSET) */
int32_t gs_hud_transform_x(int32_t x, uint64_t xyoffset, uint64_t frame, int anchor, double scale) {
    int64_t w16 = (int64_t)((frame >> 16) & 0x3F) * 64 * 16;          /* FRAME width, 1/16 pixels */
    int64_t c = (int64_t)(xyoffset & 0xFFFF) + (anchor == GS_HINT_ANCHOR_C ? w16 / 2 : anchor == GS_HINT_ANCHOR_R ? w16 : 0);
    int64_t v = c + llround((double)(x - c) * scale);
    return (int32_t)(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v);
}

static void draw_prim(int type, const vtx* a, const vtx* b, const vtx* c);

/* SIDE_FILL: the FRAME width outside the centre 4:3 area (scale of the last HUD_BEGIN) in one
 * colour, drawn as two flat sprites with a state of its own (no texture, blending, fog, alpha or
 * Z test, Z not written, no dithering); the game's registers are put back afterwards. */
static void side_fill(uint32_t rgb) {
    if (!(hud_scale > 0.0 && hud_scale < 1.0)) return;
    int ctx = (int)((((reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE]) >> 9) & 1);
    uint64_t sv_prim = reg[R_PRIM], sv_cont = reg[R_PRMODECONT], sv_test = reg[R_TEST_1 + ctx];
    uint64_t sv_zbuf = reg[R_ZBUF_1 + ctx], sv_dthe = reg[R_DTHE], sv_fba = reg[R_FBA_1 + ctx];
    reg[R_PRMODECONT] = 1;
    reg[R_PRIM] = PRIM_SPRITE | ((uint64_t)ctx << 9);
    reg[R_TEST_1 + ctx] = 0;                     /* no alpha / destination alpha / Z test */
    reg[R_ZBUF_1 + ctx] = sv_zbuf | (1ull << 32);    /* ZMSK */
    reg[R_DTHE] = 0;
    reg[R_FBA_1 + ctx] = 0;
    env_dirty = 1;
    uint64_t xyo = reg[R_XYOFFSET_1 + ctx], frame = reg[R_FRAME_1 + ctx], sc = reg[R_SCISSOR_1 + ctx];
    int32_t ox = (int32_t)(xyo & 0xFFFF), oy = (int32_t)((xyo >> 32) & 0xFFFF);
    int32_t w16 = (int32_t)((frame >> 16) & 0x3F) * 64 * 16;
    int32_t side = (int32_t)llround((double)w16 * (1.0 - hud_scale) / 2.0);
    int32_t y0 = oy + (int32_t)((sc >> 32) & 0x7FF) * 16, y1 = oy + ((int32_t)((sc >> 48) & 0x7FF) + 1) * 16;
    vtx a, b;
    memset(&a, 0, sizeof a);
    a.r = (int)(rgb & 0xFF); a.g = (int)((rgb >> 8) & 0xFF); a.b = (int)((rgb >> 16) & 0xFF); a.a = 0x80;
    a.q = 1.0f;
    b = a;
    a.x = ox; a.y = y0; b.x = ox + side; b.y = y1;
    draw_prim(PRIM_SPRITE, &a, &b, NULL);
    a.x = ox + w16 - side; b.x = ox + w16;
    draw_prim(PRIM_SPRITE, &a, &b, NULL);
    reg[R_PRIM] = sv_prim; reg[R_PRMODECONT] = sv_cont; reg[R_TEST_1 + ctx] = sv_test;
    reg[R_ZBUF_1 + ctx] = sv_zbuf; reg[R_DTHE] = sv_dthe; reg[R_FBA_1 + ctx] = sv_fba;
    env_dirty = 1;
}

static void hint_write(uint64_t v) {
    uint32_t kind = gs_hint_kind(v), value = gs_hint_value(v);
    switch (kind) {
    case GS_HINT_HUD_BEGIN: {
        float s;
        memcpy(&s, &value, 4);
        hud_on = 1;
        hud_anchor = (int)gs_hint_arg(v);
        hud_scale = value && s > 0.0f && s <= 4.0f ? (double)s : 1.0;
        break;
    }
    case GS_HINT_HUD_END: hud_on = 0; break;
    case GS_HINT_ASPECT_4_3_BEGIN: hint_43 = 1; break;
    case GS_HINT_ASPECT_4_3_END: hint_43 = 0; break;
    case GS_HINT_PHASE: hint_phase = value; hudlog_phase(value); break;
    case GS_HINT_SIDE_FILL: side_fill(value); break;
    default: break;                      /* unknown kinds: ignored */
    }
    if (trace_on > 0 && trace_left > 0 && gs_stream_vsync >= trace_from)
        fprintf(trace_out, "[gstrace] v%u hint kind %u arg %u value 0x%08X\n", gs_stream_vsync, kind,
                gs_hint_arg(v), value);
}

/* ---- vertex queue -------------------------------------------------------------------------------- */

static void draw_prim(int type, const vtx* a, const vtx* b, const vtx* c) {
    if (env_dirty) env_update();
    gs_stats.prims++;
    int swnow = 0;
    if (trace_on) {
        if (trace_on < 0) trace_init();
        if (trace_on && trace_left > 0 && gs_stream_vsync >= trace_from) {
            static uint32_t flushed_at;  /* the process may end without flushing stdio */
            if (flushed_at != gs_stream_vsync) { flushed_at = gs_stream_vsync; fflush(trace_out); }
            const vtx* vs[3] = { a, b, c };
            trace_prim(type, vs, type == PRIM_POINT ? 1 : type == PRIM_TRI || type == PRIM_TRISTRIP || type == PRIM_TRIFAN ? 3 : 2);
            /* the last record: nothing above flushes after this, and the game ends with ExitProcess,
             * which drops stdio buffers */
            if (trace_left <= 0) fflush(trace_out);
        }
    }
    if (gs_front && env_ok) {            /* GPU frontend (gs_setup.h): sees every primitive */
        uint64_t attr = (reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE];
        float hs = (float)hud_scale;
        uint32_t hsb;
        memcpy(&hsb, &hs, 4);
        gs_env_view ev = { &P, reg, (int)((attr >> 9) & 1), ofx, ofy, prim_iip, prim_aa1, prim_draws, gs_prim_version, hint_phase,
                           hud_on ? GS_ENV_HUD_ON | (uint32_t)hud_anchor << 1 : 0u, hud_on ? hsb : 0u };
        gs_front_swdraw = 0;
        gs_front->prim(&ev, type, a, b, c);
        swnow = gs_front_swdraw;         /* the frontend PUNTs it: the software rasterizer draws it now */
        gs_front_swdraw = 0;
    }
    if (!env_ok || !prim_draws || (!gs_sw_draw && !swnow)) return;
    int sw_off = !gs_sw_draw;
    if (sw_off) {                        /* (the frontend alone: bind the textures for this primitive) */
        gs_sw_draw = 1;
        gs_tex_flush_all();
        env_update();
    }
    if (prim_aa1 && type != PRIM_LINE && type != PRIM_LINESTRIP)
        log_once("antialiasing (AA1) of points/triangles/sprites requested; drawn without it", 1);
    switch (type) {
    case PRIM_POINT: draw_point(a); break;
    case PRIM_LINE: case PRIM_LINESTRIP: draw_line(a, b); break;
    case PRIM_TRI: case PRIM_TRISTRIP: case PRIM_TRIFAN: gs_stats.tris++; draw_triangle(a, b, c); break;
    case PRIM_SPRITE: gs_stats.sprites++; draw_sprite(a, b); break;
    default: break;
    }
    if (swnow) {
        if (sw_off) { gs_sw_draw = 0; env_dirty = 1; }
        gs_draw_sync();
        if (gs_front && gs_front->sw_done) gs_front->sw_done();
    }
}

/* A primitive kicked between HUD_BEGIN and HUD_END is drawn with its vertices' X scaled about the
 * anchor - unless it spans the whole FRAME width (a full-screen fill, fade or bar): that one still
 * covers the whole widened picture. The vertex queue keeps the game's coordinates (strips and fans
 * share vertices between primitives). */
static void draw(int type, const vtx* a, const vtx* b, const vtx* c) {
    if (!hud_on && hudlog_label == 0) { draw_prim(type, a, b, c); return; }
    int n = type == PRIM_POINT ? 1 : (type == PRIM_TRI || type == PRIM_TRISTRIP || type == PRIM_TRIFAN) ? 3 : 2;
    const vtx* in[3] = { a, b, c };
    int ctx = (int)((((reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE]) >> 9) & 1);
    uint64_t xyo = reg[R_XYOFFSET_1 + ctx], frame = reg[R_FRAME_1 + ctx];
    if (hudlog_label) hudlog_prim(in, n, xyo);
    if (!hud_on) { draw_prim(type, a, b, c); return; }
    int32_t left = (int32_t)(xyo & 0xFFFF), right = left + (int32_t)((frame >> 16) & 0x3F) * 64 * 16;
    int32_t x0 = in[0]->x, x1 = in[0]->x;
    for (int i = 1; i < n; i++) { x0 = in[i]->x < x0 ? in[i]->x : x0; x1 = in[i]->x > x1 ? in[i]->x : x1; }
    if (x0 <= left + 16 && x1 >= right - 16) { draw_prim(type, a, b, c); return; }
    vtx t[3];
    for (int i = 0; i < n; i++) {
        t[i] = *in[i];
        t[i].x = gs_hud_transform_x(t[i].x, xyo, frame, hud_anchor, hud_scale);
        /* (a vertex of its own: the GPU frontend thread's vertex cache is keyed by serial, and the next
         * primitive of a strip may use the untransformed vertex - M6) */
        t[i].serial = 0;
    }
    draw_prim(type, &t[0], n > 1 ? &t[1] : NULL, n > 2 ? &t[2] : NULL);
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
    n->serial = ++vtx_serial;
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

/* Pages the current host->local transfer writes: deferred drawing that writes, samples or will
 * snapshot them has to be done before the data lands (it is written at once, on this thread). */
static uint64_t xfer_dest_pages[8];

static void xfer_wait(void) {
    if (gs_draw_pages_touched(xfer_dest_pages, 0)) gs_draw_sync_why(GS_SYNC_XFER);
}

static void trxdir(uint32_t dir) {
    uint64_t bb = reg[R_BITBLTBUF], pos = reg[R_TRXPOS], rr = reg[R_TRXREG];
    /* transfers read/write VRAM in stream order: wait for the deferred drawing they could see */
    if (dir == 0 || dir == 2) {
        int x0 = (int)((pos >> 32) & 0x7FF), y0 = (int)((pos >> 48) & 0x7FF);
        int x1 = x0 + (int)(rr & 0xFFF), y1 = y0 + (int)((rr >> 32) & 0xFFF);
        memset(xfer_dest_pages, 0, sizeof xfer_dest_pages);
        if (x1 > 2048 || y1 > 2048) memset(xfer_dest_pages, 0xFF, sizeof xfer_dest_pages);   /* wraps */
        else gs_rect_pages((int)((bb >> 56) & 0x3F), (uint32_t)((bb >> 32) & 0x3FFF), (uint32_t)((bb >> 48) & 0x3F),
                           x0, y0, x1, y1, xfer_dest_pages);
        int busy = gs_draw_pages_touched(xfer_dest_pages, 0);
        if (!busy && dir == 2) {                 /* the source must not have drawing pending either */
            uint64_t src[8] = { 0 };
            int sx = (int)(pos & 0x7FF), sy = (int)((pos >> 16) & 0x7FF);
            int sx1 = sx + (int)(rr & 0xFFF), sy1 = sy + (int)((rr >> 32) & 0xFFF);
            if (sx1 > 2048 || sy1 > 2048) memset(src, 0xFF, sizeof src);
            else gs_rect_pages((int)((bb >> 24) & 0x3F), (uint32_t)(bb & 0x3FFF), (uint32_t)((bb >> 16) & 0x3F),
                               sx, sy, sx1, sy1, src);
            busy = gs_draw_pages_touched(src, 1);
        }
        if (busy) gs_draw_sync_why(GS_SYNC_XFER);
    } else if (dir == 1) {
        gs_draw_sync_why(GS_SYNC_XFER);       /* local->host: the EE reads VRAM */
    }
    xfer_dir = (int)dir;
    xfer_dbp = (uint32_t)((bb >> 32) & 0x3FFF);
    xfer_dbw = (uint32_t)((bb >> 48) & 0x3F);
    xfer_dpsm = (int)((bb >> 56) & 0x3F);
    xfer_x0 = (int)((pos >> 32) & 0x7FF);
    xfer_y0 = (int)((pos >> 48) & 0x7FF);
    xfer_x1 = xfer_x0 + (int)(rr & 0xFFF);
    xfer_y1 = xfer_y0 + (int)((rr >> 32) & 0xFFF);
    gs_stats.xfers += dir != 3;
    if (trace_on > 0 && trace_left > 0 && gs_stream_vsync >= trace_from && dir != 3)
        fprintf(trace_out, "[gstrace] v%u xfer dir %u %ux%u src %u,%u BP %u BW %u PSM %02X -> dst %d,%d BP %u BW %u PSM %02X\n",
                gs_stream_vsync, dir, (unsigned)(rr & 0xFFF), (unsigned)((rr >> 32) & 0xFFF), (unsigned)(pos & 0x7FF),
                (unsigned)((pos >> 16) & 0x7FF), (unsigned)(bb & 0x3FFF), (unsigned)((bb >> 16) & 0x3F),
                (unsigned)((bb >> 24) & 0x3F), xfer_x0, xfer_y0, xfer_dbp, xfer_dbw, xfer_dpsm);
    if (gs_front) gs_front->xfer_start(bb, pos, rr, dir);
    gs_mem_xfer_start(bb, pos, rr, dir);
    if (dir == 0 || dir == 2) mark_xfer_dest();
}

void gs_exec_image_write(const void* data, uint32_t bytes) {
    GS_INIT();
    xfer_wait();                         /* (drawing recorded since TRXDIR may touch the pages) */
    if (gs_front && xfer_dir == 0) gs_front->xfer_data(bytes);
    gs_stats.image_bytes += bytes;
    if (xfer_dir == 0 && xfer_marked_at != gs_tex_decodes) mark_xfer_dest();
    gs_mem_xfer_write(data, bytes);
}

uint32_t gs_exec_image_read(void* data, uint32_t bytes) {
    return gs_mem_xfer_read(data, bytes);
}

void gs_draw_reset(void) {
    gs_draw_sync_why(GS_SYNC_OTHER);
    vcount = 0;
    env_dirty = 1;
    if (xfer_dir != 3) gs_mem_xfer_start(reg[R_BITBLTBUF], reg[R_TRXPOS], reg[R_TRXREG], 3);
    xfer_dir = 3;
    if (gs_front) gs_front->reset();
}

/* ---- register writes ------------------------------------------------------------------------------ */

static void tex0_write(int ctx, uint64_t v) {
    reg[R_TEX0_1 + ctx] = v;
    gs_tex0 t = decode_tex0(v);
    gs_texclut tc = decode_texclut(reg[R_TEXCLUT]);
    /* a CLUT load reads VRAM (CSM1: from CBP, at most 1 KB; CSM2: a line of its buffer) */
    if (t.cld && t.cld < 6 && (t.psm == PSMT8 || t.psm == PSMT4 || t.psm == PSMT8H || t.psm == PSMT4HL || t.psm == PSMT4HH)) {
        uint64_t pages[8] = { 0 };
        if (t.csm) {
            uint32_t n = (t.psm == PSMT8 || t.psm == PSMT8H) ? 256 : 16;
            int x = (int)(tc.cou * 16), y = (int)tc.cov;
            if (x + (int)n > 2048) memset(pages, 0xFF, sizeof pages);
            else gs_rect_pages((int)t.cpsm, t.cbp, tc.cbw, x, y, x + (int)n, y + 1, pages);
        } else {
            /* CSM1: 16x16 (T8) or 8x2 (T4) pixels at CBP, i.e. blocks CBP .. CBP+3 (PSMCT32, T8),
             * CBP .. CBP+1 (PSMCT16, T8) or CBP (T4) */
            uint32_t last = (t.psm == PSMT8 || t.psm == PSMT8H) ? (t.cpsm == PSMCT32 ? 3 : 1) : 0;
            for (uint32_t p = t.cbp >> 5; p <= (t.cbp + last) >> 5; p++) pages[(p & 511) >> 6] |= 1ull << (p & 63);
        }
        /* (Sly 2 keeps CLUTs in pages its Z buffer covers and draws there before loading them:
         * about two waits per frame, which block-level tracking showed to be true dependencies) */
        if (gs_draw_pages_touched(pages, 1)) gs_draw_sync_why(GS_SYNC_CLUT);
    }
    gs_clut_on_tex0(&t, &tc);
    if (t.cld) gs_clut_version++;
    if (gs_front) gs_front->tex0(ctx, v, t.cld != 0);
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
        xfer_wait();
        if (gs_front && xfer_dir == 0) gs_front->xfer_data(8);
        if (xfer_dir == 0 && xfer_marked_at != gs_tex_decodes) mark_xfer_dest();
        gs_mem_xfer_write(&v, 8);
        break;
    case R_SIGNAL: gs_csr_signal((uint32_t)v, (uint32_t)(v >> 32)); break;
    case R_FINISH: gs_csr_finish(); break;
    case R_LABEL: gs_csr_label((uint32_t)v, (uint32_t)(v >> 32)); break;
    case 0x0E: case 0x0F:                /* A+D / NOP descriptors: nothing to do */
        break;
    case GS_HINT_REG:                    /* the PC port's side-band hints (gs_hint.h) */
        hint_write(v);
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
