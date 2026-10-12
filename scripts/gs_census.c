/* GS census: measure exactly what a GS recording (runtime/gs_record.c, PS2_GS_RECORD) asks of the
 * GS, for designing a GPU (Direct3D 11) backend. Standalone: parses the GSREC001 records itself and
 * tracks the GS register state like runtime/gs_draw.c does (vertex queue, PRMODECONT contexts, TEX2
 * folded into TEX0, CLD rules), without rendering.
 *
 *   gs_census <recording> [-v]        report to stdout (-v: also every feedback draw)
 *
 * Per VBlank interval ("frame") it counts primitives by type and attribute, vertices, draw-state
 * changes, render targets, transfers, CLUT loads and feedback draws (textures sampling pages that
 * drawing wrote this or the previous frame); at the end it prints histograms of every FRAME/ZBUF/
 * TEX0/TEX1/CLAMP/TEXA/ALPHA/TEST combination, the feedback catalogue and the display registers.
 *
 * Memory overlap is tracked at page (8 KB) granularity from the pixel bounding box of every
 * primitive (XYOFFSET and SCISSOR applied) and the texel bounding box of its texture coordinates
 * (CLAMP modes applied, mip levels via MIPTBP or MTBA): precise enough for a census. Built and run
 * by scripts/gs_census.ps1. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A
};

enum {
    R_PRIM = 0x00, R_RGBAQ = 0x01, R_ST = 0x02, R_UV = 0x03, R_XYZF2 = 0x04, R_XYZ2 = 0x05, R_TEX0_1 = 0x06,
    R_TEX0_2 = 0x07, R_CLAMP_1 = 0x08, R_CLAMP_2 = 0x09, R_FOG = 0x0A, R_XYZF3 = 0x0C, R_XYZ3 = 0x0D,
    R_TEX1_1 = 0x14, R_TEX1_2 = 0x15, R_TEX2_1 = 0x16, R_TEX2_2 = 0x17, R_XYOFFSET_1 = 0x18,
    R_XYOFFSET_2 = 0x19, R_PRMODECONT = 0x1A, R_PRMODE = 0x1B, R_TEXCLUT = 0x1C, R_SCANMSK = 0x22,
    R_MIPTBP1_1 = 0x34, R_MIPTBP1_2 = 0x35, R_MIPTBP2_1 = 0x36, R_MIPTBP2_2 = 0x37, R_TEXA = 0x3B,
    R_FOGCOL = 0x3D, R_TEXFLUSH = 0x3F, R_SCISSOR_1 = 0x40, R_SCISSOR_2 = 0x41, R_ALPHA_1 = 0x42,
    R_ALPHA_2 = 0x43, R_DIMX = 0x44, R_DTHE = 0x45, R_COLCLAMP = 0x46, R_TEST_1 = 0x47, R_TEST_2 = 0x48,
    R_PABE = 0x49, R_FBA_1 = 0x4A, R_FBA_2 = 0x4B, R_FRAME_1 = 0x4C, R_FRAME_2 = 0x4D, R_ZBUF_1 = 0x4E,
    R_ZBUF_2 = 0x4F, R_BITBLTBUF = 0x50, R_TRXPOS = 0x51, R_TRXREG = 0x52, R_TRXDIR = 0x53, R_HWREG = 0x54,
    R_SIGNAL = 0x60, R_FINISH = 0x61, R_LABEL = 0x62
};

static const char* psm_name(int psm) {
    switch (psm) {
    case PSMCT32: return "CT32"; case PSMCT24: return "CT24"; case PSMCT16: return "CT16"; case PSMCT16S: return "CT16S";
    case PSMT8: return "T8"; case PSMT4: return "T4"; case PSMT8H: return "T8H"; case PSMT4HL: return "T4HL";
    case PSMT4HH: return "T4HH"; case PSMZ32: return "Z32"; case PSMZ24: return "Z24"; case PSMZ16: return "Z16";
    case PSMZ16S: return "Z16S"; default: { static char b[8][16]; static int i; char* p = b[i++ & 7]; snprintf(p, 16, "psm%02X", psm); return p; }
    }
}

static int psm_bits(int psm) {           /* storage bits per texel in host/transfer data */
    switch (psm) {
    case PSMCT32: case PSMZ32: return 32; case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8; case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

static void psm_page(int psm, int* pw, int* ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *pw = 64; *ph = 64; break;
    case PSMT8: *pw = 128; *ph = 64; break;
    case PSMT4: *pw = 128; *ph = 128; break;
    default: *pw = 64; *ph = 32; break;
    }
}

static int psm_indexed(int psm) { return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; }

/* ---- page sets ------------------------------------------------------------------------------------ */
typedef struct { uint64_t b[8]; } pset;
static void ps_add(pset* s, uint32_t p) { p &= 511; s->b[p >> 6] |= 1ull << (p & 63); }
static int ps_empty(const pset* s) { for (int i = 0; i < 8; i++) if (s->b[i]) return 0; return 1; }
static int ps_inter(const pset* a, const pset* b) { for (int i = 0; i < 8; i++) if (a->b[i] & b->b[i]) return 1; return 0; }
static int ps_count(const pset* s) { int n = 0; for (int i = 0; i < 8; i++) n += __builtin_popcountll(s->b[i]); return n; }

/* pages of pixels [x0,x1) x [y0,y1) of a buffer (bp in 256-byte blocks, bw in 64-pixel units) */
static void rect_pages(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1, pset* s) {
    int pw, ph;
    psm_page(psm, &pw, &ph);
    if (bw == 0) bw = 1;
    int ppr = (int)bw * 64 / pw;
    if (ppr < 1) ppr = 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 2048) x1 = 2048;
    if (y1 > 2048) y1 = 2048;
    if (x1 <= x0 || y1 <= y0) return;
    int px0 = x0 / pw, px1 = (x1 - 1) / pw, py0 = y0 / ph, py1 = (y1 - 1) / ph;
    for (int py = py0; py <= py1; py++)
        for (int px = px0; px <= px1; px++) {
            uint32_t blk = bp + 32u * (uint32_t)(py * ppr + px);
            ps_add(s, blk >> 5);
            if (blk & 31) ps_add(s, (blk + 31) >> 5);
        }
}

/* ---- counting tables ------------------------------------------------------------------------------ */
typedef struct { uint64_t key; uint64_t count; uint32_t first, last; int has_rect, rx0, ry0, rx1, ry1; char desc[200]; } ent;
static void ent_rect(ent* e, int x0, int y0, int x1, int y1) {
    if (!e || x1 <= x0 || y1 <= y0) return;
    if (!e->has_rect) { e->has_rect = 1; e->rx0 = x0; e->ry0 = y0; e->rx1 = x1; e->ry1 = y1; return; }
    if (x0 < e->rx0) e->rx0 = x0; if (y0 < e->ry0) e->ry0 = y0; if (x1 > e->rx1) e->rx1 = x1; if (y1 > e->ry1) e->ry1 = y1;
}
typedef struct { ent* e; int cap, n; } tab;

static void tab_init(tab* t, int cap) { t->e = calloc((size_t)cap, sizeof(ent)); t->cap = cap; t->n = 0; }
static ent* tab_get(tab* t, uint64_t key) {
    if (!key) key = 1;
    uint32_t i = (uint32_t)(key ^ (key >> 29) ^ (key >> 47)) & (uint32_t)(t->cap - 1);
    for (int n = 0; n < t->cap; n++) {
        ent* e = &t->e[i];
        if (!e->key) {
            if (t->n >= t->cap - 1) return NULL;
            e->key = key;
            t->n++;
            return e;
        }
        if (e->key == key) return e;
        i = (i + 1) & (uint32_t)(t->cap - 1);
    }
    return NULL;
}
static uint64_t fnv(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
#define FNV0 14695981039346656037ull
static uint32_t cur_frame;
static ent* tab_add_p(tab* t, uint64_t key, uint64_t n, const char* desc) {
    ent* e = tab_get(t, key);
    if (!e) return NULL;
    if (!e->count) { e->first = cur_frame; strncpy(e->desc, desc, sizeof e->desc - 1); }
    e->count += n;
    e->last = cur_frame;
    return e;
}
#define tab_add(t, ...) tab_add_p(&(t), __VA_ARGS__)
static int cmp_ent(const void* a, const void* b) {
    const ent *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : strcmp(x->desc, y->desc);
}
static void tab_print(tab* t, const char* title, int max) {
    ent* v = malloc((size_t)t->n * sizeof(ent));
    int n = 0;
    for (int i = 0; i < t->cap; i++) if (t->e[i].key && t->e[i].count) v[n++] = t->e[i];
    qsort(v, (size_t)n, sizeof(ent), cmp_ent);
    printf("\n== %s (%d distinct%s)\n", title, n, t->n >= t->cap - 1 ? ", table full" : "");
    for (int i = 0; i < n && i < max; i++) {
        printf("%10llu  f%u-%u  %s", (unsigned long long)v[i].count, v[i].first, v[i].last, v[i].desc);
        if (v[i].has_rect) printf("  drawn=%d,%d-%d,%d (%dx%d)", v[i].rx0, v[i].ry0, v[i].rx1 - 1, v[i].ry1 - 1, v[i].rx1 - v[i].rx0, v[i].ry1 - v[i].ry0);
        printf("\n");
    }
    if (n > max) printf("        ... %d more\n", n - max);
    free(v);
}

/* ---- GS state -------------------------------------------------------------------------------------- */
static uint64_t reg[0x64];
static int verbose;

typedef struct { int x, y; uint32_t z; float s, t, q; int u, v; } vtx;
static vtx vq[3];
static int vcount;

typedef struct {
    uint32_t seq, frame;                 /* draw sequence / frame of the last write, 0 = never */
    uint8_t psm, kind;                   /* kind: 1 draw FRAME, 2 draw ZBUF, 3 upload, 4 local copy */
    uint8_t from_draw;                   /* copy of drawn data (kind 4) */
    uint16_t base;                       /* base page of the target */
} pagew;
static pagew pages[512];
static uint32_t draw_seq;
static uint32_t clut_version;
static uint32_t cbp0, cbp1;              /* CLUT compare registers */

/* per-frame counters */
typedef struct {
    uint64_t prims, verts, kicks_nodraw, types[8], aa1, iip, tme, fge, abe, fst, ctx2, fix, states, states_uniq, tex_binds,
        clut_loads, clut_from_draw, area, feedback, feedback_prev, inplace, zread, uploads, upload_bytes, upload_px,
        readbacks, readback_bytes, copies, copy_px, texflush, fb_targets, z_targets, upload_to_fb, date, ate, pabe, fba,
        dthe, colclamp0, ztst_on, zwrite, sprites_fullscreen, type_changes, regs, fb_area, clut_uniq, tex_uniq;
} counters;
static counters F, T;                    /* frame / total */

/* unique keys per frame (draw states, textures, CLUTs) */
typedef struct { uint64_t k[1 << 16]; int n; } uset;
static uset u_state, u_tex, u_cbp;
static void uniq_clear(void) { memset(&u_state, 0, sizeof u_state); memset(&u_tex, 0, sizeof u_tex); memset(&u_cbp, 0, sizeof u_cbp); }
static int uniq_add(uset* s, uint64_t k) {
    if (!k) k = 1;
    uint32_t i = (uint32_t)(k ^ (k >> 31)) & 0xFFFF;
    for (int n = 0; n < 0x10000; n++) {
        if (!s->k[i]) { if (s->n > 0xF000) return 0; s->k[i] = k; s->n++; return 1; }
        if (s->k[i] == k) return 0;
        i = (i + 1) & 0xFFFF;
    }
    return 0;
}

static tab t_frame, t_zbuf, t_framez, t_tex0, t_texsz, t_tex1, t_clamp, t_texa, t_alpha, t_test, t_ztst, t_feedback, t_upload,
    t_readback, t_copy, t_clut, t_display, t_fx, t_fogcol, t_xyoff, t_prim, t_scissor, t_dimx, t_target_frames, t_sprite, t_pass;
static uint64_t prev_key;
static int prev_type = -1;
/* what changed between consecutive draw states */
enum { G_ATTR, G_TEX, G_CLUT, G_TEXENV, G_TARGET, G_TEST, G_ALPHA, G_FOG, G_COUNT };
static const char* g_names[G_COUNT] = { "attr(IIP/TME/FGE/ABE/AA1/FST/CTX)", "TEX0 texture", "CLUT reload only", "TEX1/CLAMP/TEXA/MIPTBP",
                                        "FRAME/ZBUF/SCISSOR/XYOFFSET", "TEST", "ALPHA/PABE/FBA/COLCLAMP/DTHE", "FOGCOL" };
static uint64_t g_prev[G_COUNT], g_changes[G_COUNT], g_only[G_COUNT];
static pset frame_fb_pages;              /* pages drawn as FRAME this frame (for display matching) */
static pset prev_fb_pages;
static int frame_fbps[64], frame_nfbp;   /* FRAME FBPs drawn this frame */
static int prev_fbps[64], prev_nfbp;

static void note_fbp(int fbp) {
    for (int i = 0; i < frame_nfbp; i++) if (frame_fbps[i] == fbp) return;
    if (frame_nfbp < 64) frame_fbps[frame_nfbp++] = fbp;
}

static float asf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static void mark_pages(const pset* s, int kind, int psm, uint32_t base, int from_draw) {
    for (int i = 0; i < 512; i++)
        if (s->b[i >> 6] >> (i & 63) & 1) {
            pages[i].seq = draw_seq; pages[i].frame = cur_frame + 1; pages[i].psm = (uint8_t)psm;
            pages[i].kind = (uint8_t)kind; pages[i].base = (uint16_t)base; pages[i].from_draw = (uint8_t)from_draw;
        }
}

/* classify what a set of read pages was last written by: returns a bitmask
 * 1 drawn-frame this frame, 2 drawn-frame previous frame, 4 drawn-z this/prev frame, 8 uploaded this frame,
 * 16 local copy (of drawn data) this/prev frame, 32 drawn-frame older; sets wpsm, wbase of the first drawn hit */
static int wkind;                        /* kind of the page that set wpsm/wbase in classify_read */
static int classify_read(const pset* s, int* wpsm, int* wbase, int* wframe) {
    int m = 0;
    *wpsm = -1; *wbase = -1; *wframe = 0; wkind = 0;
    for (int i = 0; i < 512; i++) {
        if (!(s->b[i >> 6] >> (i & 63) & 1)) continue;
        const pagew* p = &pages[i];
        if (!p->kind) continue;
        int age = (int)(cur_frame + 1) - (int)p->frame;
        if (p->kind == 1) {
            if (age == 0) m |= 1; else if (age == 1) m |= 2; else m |= 32;
            if (*wpsm < 0 && age <= 1) { *wpsm = p->psm; *wbase = p->base; *wframe = (int)p->frame - 1; wkind = 1; }
        } else if (p->kind == 2 && age <= 1) {
            m |= 4;
            if (*wpsm < 0) { *wpsm = p->psm; *wbase = p->base; *wframe = (int)p->frame - 1; wkind = 2; }
        } else if (p->kind == 3 && age == 0) m |= 8;
        else if (p->kind == 4 && p->from_draw && age <= 1) {
            m |= 16;
            if (*wpsm < 0) { *wpsm = p->psm; *wbase = p->base; *wframe = (int)p->frame - 1; wkind = 4; }
        }
    }
    return m;
}

/* texel bbox of a primitive for level 0, in texels; returns 0 when unusable */
static int tex_bbox(const vtx* v[3], int n, int fst, int tw, int th, uint64_t clamp, int* u0, int* u1, int* v0, int* v1) {
    float mu = 1e30f, Mu = -1e30f, mv = 1e30f, Mv = -1e30f;
    for (int i = 0; i < n; i++) {
        float u, w;
        if (fst) { u = (float)v[i]->u / 16.0f; w = (float)v[i]->v / 16.0f; }
        else {
            float q = v[i]->q;
            if (q == 0.0f || !isfinite(q) || !isfinite(v[i]->s) || !isfinite(v[i]->t)) return 0;
            u = v[i]->s / q * (float)tw; w = v[i]->t / q * (float)th;
            if (!isfinite(u) || !isfinite(w)) return 0;
        }
        if (u < mu) mu = u; if (u > Mu) Mu = u; if (w < mv) mv = w; if (w > Mv) Mv = w;
    }
    int wms = (int)(clamp & 3), wmt = (int)((clamp >> 2) & 3);
    int minu = (int)((clamp >> 4) & 0x3FF), maxu = (int)((clamp >> 14) & 0x3FF);
    int minv = (int)((clamp >> 24) & 0x3FF), maxv = (int)((clamp >> 34) & 0x3FF);
    if (mu < -1e8f) mu = -1e8f; if (Mu > 1e8f) Mu = 1e8f; if (mv < -1e8f) mv = -1e8f; if (Mv > 1e8f) Mv = 1e8f;
    int a0 = (int)floorf(mu), a1 = (int)ceilf(Mu), b0 = (int)floorf(mv), b1 = (int)ceilf(Mv);
    switch (wms) {
    case 0: if (a0 < 0 || a1 > tw) { a0 = 0; a1 = tw; } break;
    case 1: if (a0 < 0) a0 = 0; if (a1 > tw) a1 = tw; break;
    case 2: if (a0 < minu) a0 = minu; if (a1 > maxu + 1) a1 = maxu + 1; break;
    default: a0 = maxu; a1 = (maxu | minu) + 1; break;
    }
    switch (wmt) {
    case 0: if (b0 < 0 || b1 > th) { b0 = 0; b1 = th; } break;
    case 1: if (b0 < 0) b0 = 0; if (b1 > th) b1 = th; break;
    case 2: if (b0 < minv) b0 = minv; if (b1 > maxv + 1) b1 = maxv + 1; break;
    default: b0 = maxv; b1 = (maxv | minv) + 1; break;
    }
    if (a1 <= a0) a1 = a0 + 1;
    if (b1 <= b0) b1 = b0 + 1;
    *u0 = a0; *u1 = a1; *v0 = b0; *v1 = b1;
    return 1;
}

static const char* type_names[8] = { "point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "prim7" };
static const char* clamp_names[4] = { "REPEAT", "CLAMP", "RCLAMP", "RREPEAT" };
static const char* tfx_names[4] = { "MODULATE", "DECAL", "HIGHLIGHT", "HIGHLIGHT2" };
static const char* ztst_names[4] = { "NEVER", "ALWAYS", "GEQUAL", "GREATER" };
static const char* atst_names[8] = { "NEVER", "ALWAYS", "LESS", "LEQUAL", "EQUAL", "GEQUAL", "GREATER", "NOTEQUAL" };
static const char* afail_names[4] = { "KEEP", "FB_ONLY", "ZB_ONLY", "RGB_ONLY" };
static const char* ab_names[4] = { "Cs", "Cd", "0", "?" };
static const char* c_names[4] = { "As", "Ad", "FIX", "?" };

static void kick(int type, const vtx* a, const vtx* b, const vtx* c) {
    const vtx* v[3] = { a, b, c };
    int n = type == 0 ? 1 : (type == 3 || type == 4 || type == 5) ? 3 : 2;
    uint64_t attr = (reg[R_PRMODECONT] & 1) ? reg[R_PRIM] : reg[R_PRMODE];
    int ctx = (int)((attr >> 9) & 1), iip = (int)((attr >> 3) & 1), tme = (int)((attr >> 4) & 1), fge = (int)((attr >> 5) & 1);
    int abe = (int)((attr >> 6) & 1), aa1 = (int)((attr >> 7) & 1), fst = (int)((attr >> 8) & 1), fix = (int)((attr >> 10) & 1);
    uint64_t frame = reg[R_FRAME_1 + ctx], zbuf = reg[R_ZBUF_1 + ctx], test = reg[R_TEST_1 + ctx];
    uint64_t alpha = reg[R_ALPHA_1 + ctx], sc = reg[R_SCISSOR_1 + ctx], xyo = reg[R_XYOFFSET_1 + ctx];
    uint64_t tex0 = reg[R_TEX0_1 + ctx], tex1 = reg[R_TEX1_1 + ctx], clamp = reg[R_CLAMP_1 + ctx];
    int fbp = (int)(frame & 0x1FF), fbw = (int)((frame >> 16) & 0x3F), fpsm = (int)((frame >> 24) & 0x3F);
    uint32_t fbmsk = (uint32_t)(frame >> 32);
    int zbp = (int)(zbuf & 0x1FF), zpsm = 0x30 | (int)((zbuf >> 24) & 0xF), zmsk = (int)((zbuf >> 32) & 1);
    int ate = (int)(test & 1), atst = (int)((test >> 1) & 7), aref = (int)((test >> 4) & 0xFF), afail = (int)((test >> 12) & 3);
    int date = (int)((test >> 14) & 1), datm = (int)((test >> 15) & 1), zte = (int)((test >> 16) & 1);
    int ztst = zte ? (int)((test >> 17) & 3) : 1;
    int scx0 = (int)(sc & 0x7FF), scx1 = (int)((sc >> 16) & 0x7FF), scy0 = (int)((sc >> 32) & 0x7FF), scy1 = (int)((sc >> 48) & 0x7FF);
    int ofx = (int)(xyo & 0xFFFF), ofy = (int)((xyo >> 32) & 0xFFFF);
    draw_seq++;
    F.prims++; F.verts += (uint64_t)n; F.types[type]++; F.aa1 += (uint64_t)aa1; F.iip += (uint64_t)iip; F.tme += (uint64_t)tme;
    F.fge += (uint64_t)fge; F.abe += (uint64_t)abe; F.fst += (uint64_t)(tme && fst); F.ctx2 += (uint64_t)ctx; F.fix += (uint64_t)fix;
    F.date += (uint64_t)date; F.ate += (uint64_t)ate; F.pabe += reg[R_PABE] & 1; F.fba += reg[R_FBA_1 + ctx] & 1;
    F.dthe += reg[R_DTHE] & 1; F.colclamp0 += !(reg[R_COLCLAMP] & 1); F.ztst_on += (uint64_t)(zte && ztst >= 2);

    /* pixel bbox */
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < n; i++) {
        int px = v[i]->x - ofx, py = v[i]->y - ofy;
        if (px < x0) x0 = px; if (px > x1) x1 = px; if (py < y0) y0 = py; if (py > y1) y1 = py;
    }
    int bx0, bx1, by0, by1;
    if (type == 6) { bx0 = (x0 + 8) >> 4; bx1 = (x1 + 8) >> 4; by0 = (y0 + 8) >> 4; by1 = (y1 + 8) >> 4; }
    else { bx0 = x0 >> 4; bx1 = (x1 >> 4) + 1; by0 = y0 >> 4; by1 = (y1 >> 4) + 1; }
    if (bx0 < scx0) bx0 = scx0; if (bx1 > scx1 + 1) bx1 = scx1 + 1; if (by0 < scy0) by0 = scy0; if (by1 > scy1 + 1) by1 = scy1 + 1;
    int visible = bx1 > bx0 && by1 > by0;
    int w = visible ? bx1 - bx0 : 0, h = visible ? by1 - by0 : 0;
    uint64_t area = type == 6 ? (uint64_t)w * (uint64_t)h : type >= 3 ? (uint64_t)w * (uint64_t)h / 2 : type == 0 ? 1 : (uint64_t)(w > h ? w : h);
    F.area += area;

    /* what gets written */
    int wr_fb = fbmsk != 0xFFFFFFFFu, wr_z = !zmsk && ztst != 0;
    if (ate && atst == 0) {               /* ATST NEVER: only AFAIL decides */
        if (afail == 0) wr_fb = wr_z = 0; else if (afail == 1) wr_z = 0; else if (afail == 2) wr_fb = 0;
    }
    if (!wr_fb && !wr_z) F.kicks_nodraw++;
    if (wr_z) F.zwrite++;

    char d[200];
    /* pass signature: everything but the texture identity and the primitive type */
    {
        int tpsm = tme ? (int)((tex0 >> 20) & 0x3F) : -1;
        snprintf(d, sizeof d, "PASS iip=%d tme=%d fge=%d abe=%d fst=%d | fbmsk=%08X | %s %s zmsk=%d | ate=%d %s %d %s date=%d | (%s-%s)*%s+%s %d fba=%d cc=%d | tex=%s tfx=%d tcc=%d %s/%s %s",
                 iip, tme, fge, abe, fst, fbmsk, psm_name(fpsm), ztst_names[ztst], zmsk, ate, atst_names[atst], aref, afail_names[afail], date,
                 ab_names[alpha & 3], ab_names[(alpha >> 2) & 3], c_names[(alpha >> 4) & 3], ab_names[(alpha >> 6) & 3], (int)((alpha >> 32) & 0xFF),
                 (int)(reg[R_FBA_1 + ctx] & 1), (int)(reg[R_COLCLAMP] & 1), tme ? psm_name(tpsm) : "-", tme ? (int)((tex0 >> 35) & 3) : 0,
                 tme ? (int)((tex0 >> 34) & 1) : 0, tme ? clamp_names[clamp & 3] : "-", tme ? clamp_names[(clamp >> 2) & 3] : "-",
                 tme ? (((tex1 >> 5) & 1) ? "LIN" : "NEAR") : "");
        tab_add(t_pass, fnv(d, strlen(d), FNV0), 1, d);
    }
    pset fbs = { { 0 } }, zbs = { { 0 } };
    if (visible) {
        rect_pages(fpsm, (uint32_t)fbp * 32, (uint32_t)fbw, bx0, by0, bx1, by1, &fbs);
        rect_pages(zpsm, (uint32_t)zbp * 32, (uint32_t)fbw, bx0, by0, bx1, by1, &zbs);
    }
    /* targets */
    {
        snprintf(d, sizeof d, "FRAME fbp=%03X fbw=%d(%dpx) %s fbmsk=%08X  scissor=%d,%d-%d,%d  xyoff=%d.%X,%d.%X", fbp, fbw, fbw * 64,
                 psm_name(fpsm), fbmsk, scx0, scy0, scx1, scy1, ofx >> 4, ofx & 15, ofy >> 4, ofy & 15);
        uint64_t k = fnv(&frame, 8, fnv(&sc, 8, fnv(&xyo, 8, FNV0)));
        ent* e = tab_add(t_frame, k, 1, d);
        if (visible && (wr_fb || wr_z)) ent_rect(e, bx0, by0, bx1, by1);
        snprintf(d, sizeof d, "ZBUF zbp=%03X %s zmsk=%d ztst=%s zwrite=%d  with FRAME fbp=%03X fbw=%d %s fbmsk=%08X", zbp, psm_name(zpsm), zmsk,
                 ztst_names[ztst], wr_z, fbp, fbw, psm_name(fpsm), fbmsk);
        uint64_t zk = fnv(d, strlen(d), FNV0);
        tab_add(t_zbuf, zk, 1, d);
        /* per-frame target list */
        note_fbp(fbp);
        if (wr_fb && visible) {
            mark_pages(&fbs, 1, fpsm, (uint32_t)fbp, 0);
            for (int i = 0; i < 8; i++) frame_fb_pages.b[i] |= fbs.b[i];
        }
        if (wr_z && visible) mark_pages(&zbs, 2, zpsm, (uint32_t)zbp, 0);
    }
    /* full-screen-ish sprites */
    if (type == 6 && w >= 256 && h >= 128) {
        F.sprites_fullscreen++;
        snprintf(d, sizeof d, "sprite %dx%d at %d,%d -> fbp=%03X %s%s tme=%d abe=%d A=%d B=%d C=%d D=%d fix=%d tex=%s tbp=%04X tfx=%d tcc=%d ztst=%s zmsk=%d atst=%s", w, h,
                 bx0, by0, fbp, psm_name(fpsm), fbmsk ? " msk" : "", tme, abe, (int)(alpha & 3), (int)((alpha >> 2) & 3), (int)((alpha >> 4) & 3),
                 (int)((alpha >> 6) & 3), (int)((alpha >> 32) & 0xFF), tme ? psm_name((int)((tex0 >> 20) & 0x3F)) : "-", tme ? (int)(tex0 & 0x3FFF) : 0,
                 (int)((tex0 >> 35) & 3), (int)((tex0 >> 34) & 1), ztst_names[ztst], zmsk, ate ? atst_names[atst] : "off");
        uint64_t k = fnv(d, strlen(d), FNV0);
        tab_add(t_sprite, k, 1, d);
    }

    /* alpha / test */
    if (abe) {
        int A = (int)(alpha & 3), B = (int)((alpha >> 2) & 3), C = (int)((alpha >> 4) & 3), D = (int)((alpha >> 6) & 3), fx = (int)((alpha >> 32) & 0xFF);
        snprintf(d, sizeof d, "ALPHA (%s - %s) * %s + %s  FIX=%d  pabe=%d fba=%d colclamp=%d", ab_names[A], ab_names[B], c_names[C], ab_names[D],
                 fx, (int)(reg[R_PABE] & 1), (int)(reg[R_FBA_1 + ctx] & 1), (int)(reg[R_COLCLAMP] & 1));
        uint64_t k = fnv(d, strlen(d), FNV0);
        tab_add(t_alpha, k, 1, d);
    }
    {
        snprintf(d, sizeof d, "TEST ate=%d %s aref=%d afail=%s date=%d datm=%d zte=%d ztst=%s zmsk=%d fpsm=%s", ate, atst_names[atst], aref,
                 afail_names[afail], date, datm, zte, ztst_names[ztst], zmsk, psm_name(fpsm));
        uint64_t k = fnv(d, strlen(d), FNV0);
        tab_add(t_test, k, 1, d);
    }
    {
        snprintf(d, sizeof d, "%s iip=%d tme=%d fge=%d abe=%d aa1=%d fst=%d ctx=%d fix=%d", type_names[type], iip, tme, fge, abe, aa1, fst, ctx, fix);
        uint64_t k = fnv(d, strlen(d), FNV0);
        tab_add(t_prim, k, 1, d);
    }
    if (fge) {
        snprintf(d, sizeof d, "FOGCOL %06llX (fog prims)", (unsigned long long)(reg[R_FOGCOL] & 0xFFFFFF));
        tab_add(t_fogcol, reg[R_FOGCOL] | 1, 1, d);
    }
    if (reg[R_DTHE] & 1) {
        snprintf(d, sizeof d, "DIMX %016llX fpsm=%s", (unsigned long long)reg[R_DIMX], psm_name(fpsm));
        tab_add(t_dimx, fnv(d, strlen(d), FNV0), 1, d);
    }

    /* textures */
    uint64_t skey = FNV0;
    skey = fnv(&attr, 8, skey);
    skey = fnv(&frame, 8, skey); skey = fnv(&zbuf, 8, skey); skey = fnv(&test, 8, skey); skey = fnv(&alpha, 8, skey);
    skey = fnv(&sc, 8, skey); skey = fnv(&xyo, 8, skey);
    skey = fnv(&reg[R_PABE], 8, skey); skey = fnv(&reg[R_FBA_1 + ctx], 8, skey); skey = fnv(&reg[R_COLCLAMP], 8, skey);
    skey = fnv(&reg[R_DTHE], 8, skey);
    if (reg[R_DTHE] & 1) skey = fnv(&reg[R_DIMX], 8, skey);
    if (fge) skey = fnv(&reg[R_FOGCOL], 8, skey);
    if (tme) {
        int tbp0 = (int)(tex0 & 0x3FFF), tbw = (int)((tex0 >> 14) & 0x3F), tpsm = (int)((tex0 >> 20) & 0x3F);
        int twl = (int)((tex0 >> 26) & 0xF), thl = (int)((tex0 >> 30) & 0xF), tcc = (int)((tex0 >> 34) & 1), tfx = (int)((tex0 >> 35) & 3);
        int cbp = (int)((tex0 >> 37) & 0x3FFF), cpsm = (int)((tex0 >> 51) & 0xF), csm = (int)((tex0 >> 55) & 1), csa = (int)((tex0 >> 56) & 0x1F);
        int cld = (int)((tex0 >> 61) & 7);
        int tw = 1 << twl, th = 1 << thl;
        int lcm = (int)(tex1 & 1), mxl = (int)((tex1 >> 2) & 7), mmag = (int)((tex1 >> 5) & 1), mmin = (int)((tex1 >> 6) & 7);
        int mtba = (int)((tex1 >> 9) & 1), L = (int)((tex1 >> 19) & 3), K = (int)((tex1 >> 32) & 0xFFF);
        skey = fnv(&tex0, 8, skey); skey = fnv(&tex1, 8, skey); skey = fnv(&clamp, 8, skey); skey = fnv(&reg[R_TEXA], 8, skey);
        if (mxl) { skey = fnv(&reg[R_MIPTBP1_1 + ctx], 8, skey); skey = fnv(&reg[R_MIPTBP2_1 + ctx], 8, skey); }
        if (psm_indexed(tpsm)) skey = fnv(&clut_version, 4, skey);
        if (uniq_add(&u_tex, tex0 & ~((1ull << 34) | (3ull << 35) | (7ull << 61)))) F.tex_uniq++;

        snprintf(d, sizeof d, "TEX0 %s tbw=%d %dx%d tcc=%d tfx=%s%s", psm_name(tpsm), tbw, tw, th, tcc, tfx_names[tfx],
                 psm_indexed(tpsm) ? "" : "");
        if (psm_indexed(tpsm)) {
            char c2[80];
            snprintf(c2, sizeof c2, " clut=%s csm=%d csa=%d cld=%d", psm_name(cpsm), csm, csa, cld);
            strncat(d, c2, sizeof d - strlen(d) - 1);
        }
        tab_add(t_tex0, fnv(d, strlen(d), FNV0), 1, d);
        snprintf(d, sizeof d, "%dx%d %s", tw, th, psm_name(tpsm));
        tab_add(t_texsz, fnv(d, strlen(d), FNV0), 1, d);
        snprintf(d, sizeof d, "TEX1 lcm=%d mxl=%d mmag=%s mmin=%d(%s) mtba=%d L=%d K=%d%s", lcm, mxl, mmag ? "LINEAR" : "NEAREST", mmin,
                 (const char*[]){ "NEAREST", "LINEAR", "N_MIP_N", "N_MIP_L", "L_MIP_N", "L_MIP_L", "?", "?" }[mmin], mtba, L, K,
                 mxl && !mtba ? (reg[R_MIPTBP1_1 + ctx] ? " miptbp1" : " miptbp1=0") : "");
        tab_add(t_tex1, fnv(d, strlen(d), FNV0), 1, d);
        {
            int wms = (int)(clamp & 3), wmt = (int)((clamp >> 2) & 3);
            if (wms >= 2 || wmt >= 2)
                snprintf(d, sizeof d, "CLAMP %s/%s minu=%d maxu=%d minv=%d maxv=%d (tex %dx%d)", clamp_names[wms], clamp_names[wmt],
                         (int)((clamp >> 4) & 0x3FF), (int)((clamp >> 14) & 0x3FF), (int)((clamp >> 24) & 0x3FF), (int)((clamp >> 34) & 0x3FF), tw, th);
            else snprintf(d, sizeof d, "CLAMP %s/%s", clamp_names[wms], clamp_names[wmt]);
            tab_add(t_clamp, fnv(d, strlen(d), FNV0), 1, d);
        }
        {
            uint64_t ta = reg[R_TEXA];
            snprintf(d, sizeof d, "TEXA ta0=%d aem=%d ta1=%d (tex %s)", (int)(ta & 0xFF), (int)((ta >> 15) & 1), (int)((ta >> 32) & 0xFF), psm_name(tpsm));
            if (tpsm == PSMCT24 || tpsm == PSMCT16 || tpsm == PSMCT16S || psm_indexed(tpsm))
                tab_add(t_texa, fnv(d, strlen(d), FNV0), 1, d);
        }

        /* texel bbox -> pages read (level 0 and mips); a sprite clipped by the scissor only reads the
         * texels of its visible part (texture coordinates are linear across it) */
        int u0, u1, v0, v1;
        pset ts = { { 0 } };
        vtx sv[2];
        if (type == 6 && visible && (x1 > x0) && (y1 > y0)) {
            const vtx *p = a, *q = b;
            if (p->x > q->x || (p->x == q->x && p->y > q->y)) { p = b; q = a; }
            sv[0] = *p; sv[1] = *q;
            /* visible rect in vertex units */
            float fx0 = (float)(bx0 * 16 + 8), fx1 = (float)(bx1 * 16 - 8), fy0 = (float)(by0 * 16 + 8), fy1 = (float)(by1 * 16 - 8);
            float ax = (float)(p->x - ofx), bxx = (float)(q->x - ofx), ay = (float)(p->y - ofy), byy = (float)(q->y - ofy);
            float tx0 = bxx != ax ? (fx0 - ax) / (bxx - ax) : 0.0f, tx1 = bxx != ax ? (fx1 - ax) / (bxx - ax) : 1.0f;
            float ty0, ty1;
            if (ay <= byy) { ty0 = byy != ay ? (fy0 - ay) / (byy - ay) : 0.0f; ty1 = byy != ay ? (fy1 - ay) / (byy - ay) : 1.0f; }
            else { ty0 = (fy1 - ay) / (byy - ay); ty1 = (fy0 - ay) / (byy - ay); }
            if (tx0 < 0.0f) tx0 = 0.0f; if (tx1 > 1.0f) tx1 = 1.0f; if (ty0 < 0.0f) ty0 = 0.0f; if (ty1 > 1.0f) ty1 = 1.0f;
            if (fst) {
                sv[0].u = p->u + (int)((float)(q->u - p->u) * tx0); sv[1].u = p->u + (int)((float)(q->u - p->u) * tx1);
                sv[0].v = p->v + (int)((float)(q->v - p->v) * ty0); sv[1].v = p->v + (int)((float)(q->v - p->v) * ty1);
            } else {
                sv[0].s = p->s + (q->s - p->s) * tx0; sv[1].s = p->s + (q->s - p->s) * tx1;
                sv[0].t = p->t + (q->t - p->t) * ty0; sv[1].t = p->t + (q->t - p->t) * ty1;
                sv[0].q = sv[1].q = q->q;
            }
            v[0] = &sv[0]; v[1] = &sv[1];
        }
        if (tex_bbox(v, n, fst, tw, th, clamp, &u0, &u1, &v0, &v1)) {
            rect_pages(tpsm, (uint32_t)tbp0, (uint32_t)tbw, u0, v0, u1, v1, &ts);
            if (mxl > 0 && (lcm == 0 || K > 0)) {
                uint32_t tb = (uint32_t)tbp0, bw = (uint32_t)tbw;
                int lw = tw, lh = th;
                for (int l = 1; l <= mxl && l <= 6; l++) {
                    if (mtba && l <= 3) {
                        uint32_t bytes = (uint32_t)(bw * 64) * (uint32_t)lh * (uint32_t)psm_bits(tpsm) / 8;
                        tb += (bytes + 255) / 256; bw = bw > 1 ? bw / 2 : 1;
                    } else {
                        uint64_t m = l <= 3 ? reg[R_MIPTBP1_1 + ctx] : reg[R_MIPTBP2_1 + ctx];
                        int s = l <= 3 ? (l - 1) * 20 : (l - 4) * 20;
                        tb = (uint32_t)((m >> s) & 0x3FFF); bw = (uint32_t)((m >> (s + 14)) & 0x3F);
                    }
                    lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
                    rect_pages(tpsm, tb, bw, u0 >> l, v0 >> l, ((u1 - 1) >> l) + 1, ((v1 - 1) >> l) + 1, &ts);
                }
            }
        } else rect_pages(tpsm, (uint32_t)tbp0, (uint32_t)tbw, 0, 0, tw, th, &ts);

        int wpsm, wbase, wframe;
        int m = classify_read(&ts, &wpsm, &wbase, &wframe);
        if (m & (1 | 2 | 4 | 16)) {
            if (m & 1) F.feedback++; else F.feedback_prev++;
            if (m & 4) F.zread++;
            int inplace = ps_inter(&ts, &fbs) && wr_fb;
            if (inplace) F.inplace++;
            int uw = u1 - u0, uh = v1 - v0;
            F.fb_area += area;
            snprintf(d, sizeof d, "%s %s%s%s: read %s tbp=%04X tbw=%d %dx%d (texels %dx%d at %d,%d) <- written as %s base=%03X %s; dst fbp=%03X %s %dx%d at %d,%d abe=%d alpha=%d%d%d%d/%d tfx=%d tcc=%d atst=%s z=%s%s fbmsk=%08X",
                     type_names[type], m & 1 ? "SAME" : m & 2 ? "PREV" : m & 4 ? "Z" : "COPY", (m & 4) ? "+Z" : "", inplace ? " INPLACE" : "", psm_name(tpsm), tbp0, tbw,
                     tw, th, uw, uh, u0, v0, wpsm >= 0 ? psm_name(wpsm) : "?", wbase, wkind == 2 ? "ZBUF" : wkind == 4 ? "copy" : "FRAME", fbp, psm_name(fpsm), w, h, bx0, by0,
                     abe, (int)(alpha & 3), (int)((alpha >> 2) & 3), (int)((alpha >> 4) & 3), (int)((alpha >> 6) & 3), (int)((alpha >> 32) & 0xFF), tfx,
                     tcc, ate ? atst_names[atst] : "off", ztst_names[ztst], zmsk ? "" : "+zw", fbmsk);
            /* group: drop the exact texel/pixel sizes from the key */
            char kd[200];
            snprintf(kd, sizeof kd, "%s %d %d %d %04X %d %d %d %d %03X %d %03X %d %d %llx %d %d %08X", type_names[type], m & 7, inplace, tpsm, tbp0, tbw, tw, th,
                     wpsm, wbase, wkind, fbp, fpsm, abe, (unsigned long long)alpha, tfx, (int)(ate ? atst : 9), fbmsk);
            tab_add(t_feedback, fnv(kd, strlen(kd), FNV0), 1, d);
            if (verbose) printf("  f%u #%u %s\n", cur_frame, draw_seq, d);
        }
    }
    if (skey != prev_key) {
        F.states++; prev_key = skey;
        uint64_t g[G_COUNT];
        g[G_ATTR] = attr & 0x7F8;
        g[G_TEX] = tme ? (tex0 & ~((1ull << 34) | (3ull << 35) | (7ull << 61))) : 0;
        g[G_CLUT] = (tme && psm_indexed((int)((tex0 >> 20) & 0x3F))) ? clut_version : 0;
        g[G_TEXENV] = tme ? fnv(&tex1, 8, fnv(&clamp, 8, fnv(&reg[R_TEXA], 8, fnv(&reg[R_MIPTBP1_1 + ctx], 8, FNV0)))) | (tex0 >> 34 & 7) << 60 : 0;
        g[G_TARGET] = fnv(&frame, 8, fnv(&zbuf, 8, fnv(&sc, 8, fnv(&xyo, 8, FNV0))));
        g[G_TEST] = test;
        g[G_ALPHA] = abe ? fnv(&alpha, 8, fnv(&reg[R_PABE], 8, fnv(&reg[R_FBA_1 + ctx], 8, fnv(&reg[R_COLCLAMP], 8, fnv(&reg[R_DTHE], 8, FNV0))))) : 0;
        g[G_FOG] = fge ? reg[R_FOGCOL] : 0;
        int n = 0, which = -1;
        for (int i = 0; i < G_COUNT; i++) if (g[i] != g_prev[i]) { g_changes[i]++; n++; which = i; }
        if (n == 1) g_only[which]++;
        memcpy(g_prev, g, sizeof g);
    }
    if (type != prev_type) { F.type_changes++; prev_type = type; }
    if (uniq_add(&u_state, skey)) F.states_uniq++;
}

static void vertex(uint64_t xyz, int has_f, int kk) {
    vtx* nv = &vq[vcount];
    nv->x = (int32_t)(xyz & 0xFFFF);
    nv->y = (int32_t)((xyz >> 16) & 0xFFFF);
    nv->z = has_f ? (uint32_t)((xyz >> 32) & 0xFFFFFF) : (uint32_t)(xyz >> 32);
    uint64_t c = reg[R_RGBAQ];
    nv->q = asf((uint32_t)(c >> 32));
    nv->s = asf((uint32_t)(reg[R_ST] & 0xFFFFFF00u));
    nv->t = asf((uint32_t)(reg[R_ST] >> 32) & 0xFFFFFF00u);
    nv->u = (int)(reg[R_UV] & 0x3FFF);
    nv->v = (int)((reg[R_UV] >> 16) & 0x3FFF);
    vcount++;
    int type = (int)(reg[R_PRIM] & 7);
    switch (type) {
    case 0: if (kk) kick(type, &vq[0], NULL, NULL); vcount = 0; break;
    case 1: case 6: if (vcount == 2) { if (kk) kick(type, &vq[0], &vq[1], NULL); vcount = 0; } break;
    case 2: if (vcount == 2) { if (kk) kick(type, &vq[0], &vq[1], NULL); vq[0] = vq[1]; vcount = 1; } break;
    case 3: if (vcount == 3) { if (kk) kick(type, &vq[0], &vq[1], &vq[2]); vcount = 0; } break;
    case 4: if (vcount == 3) { if (kk) kick(type, &vq[0], &vq[1], &vq[2]); vq[0] = vq[1]; vq[1] = vq[2]; vcount = 2; } break;
    case 5: if (vcount == 3) { if (kk) kick(type, &vq[0], &vq[1], &vq[2]); vq[1] = vq[2]; vcount = 2; } break;
    default: vcount = 0; break;
    }
}

/* ---- transfers ------------------------------------------------------------------------------------ */
static int xfer_dir = 3;
static uint64_t xfer_bytes;

static void trxdir(uint32_t dir) {
    uint64_t bb = reg[R_BITBLTBUF], pos = reg[R_TRXPOS], rr = reg[R_TRXREG];
    int sbp = (int)(bb & 0x3FFF), sbw = (int)((bb >> 16) & 0x3F), spsm = (int)((bb >> 24) & 0x3F);
    int dbp = (int)((bb >> 32) & 0x3FFF), dbw = (int)((bb >> 48) & 0x3F), dpsm = (int)((bb >> 56) & 0x3F);
    int ssax = (int)(pos & 0x7FF), ssay = (int)((pos >> 16) & 0x7FF), dsax = (int)((pos >> 32) & 0x7FF), dsay = (int)((pos >> 48) & 0x7FF);
    int ddir = (int)((pos >> 59) & 3);
    int rrw = (int)(rr & 0xFFF), rrh = (int)((rr >> 32) & 0xFFF);
    char d[200];
    xfer_dir = (int)dir;
    xfer_bytes = 0;
    if (dir == 0) {
        pset ds = { { 0 } };
        rect_pages(dpsm, (uint32_t)dbp, (uint32_t)dbw, dsax, dsay, dsax + rrw, dsay + rrh, &ds);
        int wpsm, wbase, wframe;
        int m = classify_read(&ds, &wpsm, &wbase, &wframe);
        int to_fb = (m & (1 | 2)) || ps_inter(&ds, &frame_fb_pages) || ps_inter(&ds, &prev_fb_pages);
        F.uploads++; F.upload_px += (uint64_t)rrw * (uint64_t)rrh;
        if (to_fb) F.upload_to_fb++;
        snprintf(d, sizeof d, "UPLOAD %s dbw=%d %dx%d at %d,%d dbp=%04X%s", psm_name(dpsm), dbw, rrw, rrh, dsax, dsay, dbp,
                 to_fb ? " -> drawn target" : "");
        char kd[100];
        snprintf(kd, sizeof kd, "%d %d %d %d %d", dpsm, dbw, rrw, rrh, to_fb);
        tab_add(t_upload, fnv(kd, strlen(kd), FNV0), 1, d);
        mark_pages(&ds, 3, dpsm, (uint32_t)dbp >> 5, 0);
    } else if (dir == 1) {
        pset ss = { { 0 } };
        rect_pages(spsm, (uint32_t)sbp, (uint32_t)sbw, ssax, ssay, ssax + rrw, ssay + rrh, &ss);
        int wpsm, wbase, wframe;
        int m = classify_read(&ss, &wpsm, &wbase, &wframe);
        F.readbacks++; F.readback_bytes += (uint64_t)rrw * (uint64_t)rrh * (uint64_t)psm_bits(spsm) / 8;
        snprintf(d, sizeof d, "READBACK %s sbw=%d %dx%d at %d,%d sbp=%04X (%llu bytes) src %s", psm_name(spsm), sbw, rrw, rrh, ssax, ssay, sbp,
                 (unsigned long long)rrw * (unsigned long long)rrh * (unsigned long long)psm_bits(spsm) / 8,
                 (m & 1) ? "drawn this frame" : (m & 2) ? "drawn prev frame" : (m & 4) ? "zbuf" : (m & 32) ? "drawn earlier" : "not drawn");
        tab_add(t_readback, fnv(d, strlen(d), FNV0), 1, d);
        if (verbose) printf("  f%u %s\n", cur_frame, d);
    } else if (dir == 2) {
        pset ss = { { 0 } }, ds = { { 0 } };
        rect_pages(spsm, (uint32_t)sbp, (uint32_t)sbw, ssax, ssay, ssax + rrw, ssay + rrh, &ss);
        rect_pages(dpsm, (uint32_t)dbp, (uint32_t)dbw, dsax, dsay, dsax + rrw, dsay + rrh, &ds);
        int wpsm, wbase, wframe;
        int m = classify_read(&ss, &wpsm, &wbase, &wframe);
        F.copies++; F.copy_px += (uint64_t)rrw * (uint64_t)rrh;
        int from_draw = (m & (1 | 2 | 4 | 16)) != 0;
        snprintf(d, sizeof d, "COPY %s sbp=%04X sbw=%d %d,%d -> %s dbp=%04X dbw=%d %d,%d  %dx%d dir=%d src %s%s", psm_name(spsm), sbp, sbw, ssax, ssay,
                 psm_name(dpsm), dbp, dbw, dsax, dsay, rrw, rrh, ddir, (m & 1) ? "drawn this frame" : (m & 2) ? "drawn prev frame" : (m & 4) ? "zbuf" : (m & 32) ? "drawn earlier" : "not drawn",
                 ps_inter(&ss, &ds) ? " OVERLAP" : "");
        char kd[100];
        snprintf(kd, sizeof kd, "%d %d %d %d %d %d %d %d", spsm, sbp, dpsm, dbp, rrw, rrh, ddir, from_draw);
        tab_add(t_copy, fnv(kd, strlen(kd), FNV0), 1, d);
        mark_pages(&ds, 4, spsm, (uint32_t)sbp >> 5, from_draw);
        if (verbose) printf("  f%u %s\n", cur_frame, d);
    }
}

static void tex0_write(int ctx, uint64_t v) {
    uint64_t old = reg[R_TEX0_1 + ctx];
    reg[R_TEX0_1 + ctx] = v;
    if (old != v) F.tex_binds++;
    int tpsm = (int)((v >> 20) & 0x3F), cld = (int)((v >> 61) & 7);
    uint32_t cbp = (uint32_t)((v >> 37) & 0x3FFF);
    if (!psm_indexed(tpsm) || !cld) return;
    int load = 0;
    switch (cld) {
    case 1: load = 1; break;
    case 2: load = 1; cbp0 = cbp; break;
    case 3: load = 1; cbp1 = cbp; break;
    case 4: load = cbp != cbp0; cbp0 = cbp; break;
    case 5: load = cbp != cbp1; cbp1 = cbp; break;
    default: break;
    }
    if (!load) return;
    clut_version++;
    F.clut_loads++;
    int cpsm = (int)((v >> 51) & 0xF), csm = (int)((v >> 55) & 1), csa = (int)((v >> 56) & 0x1F);
    int n = (tpsm == PSMT8 || tpsm == PSMT8H) ? 256 : 16;
    pset cs = { { 0 } };
    if (csm) {
        uint64_t tc = reg[R_TEXCLUT];
        int cbw = (int)(tc & 0x3F), cou = (int)((tc >> 6) & 0x3F), cov = (int)((tc >> 12) & 0x3FF);
        rect_pages(cpsm, cbp, (uint32_t)cbw, cou * 16, cov, cou * 16 + n, cov + 1, &cs);
    } else {
        uint32_t last = n == 256 ? (cpsm == PSMCT32 ? 3 : 1) : 0;
        for (uint32_t p = cbp >> 5; p <= (cbp + last) >> 5; p++) ps_add(&cs, p);
    }
    int wpsm, wbase, wframe;
    int m = classify_read(&cs, &wpsm, &wbase, &wframe);
    char d[200];
    snprintf(d, sizeof d, "CLUT %s %d entries cpsm=%s csm=%d csa=%d cld=%d cbp=%04X src=%s", psm_name(tpsm), n, psm_name(cpsm), csm, csa, cld, cbp,
             (m & (1 | 2 | 32)) ? "DRAWN" : (m & 4) ? "ZBUF" : (m & 16) ? "copy" : (m & 8) ? "uploaded this frame" : "resident");
    if (m & (1 | 2 | 4 | 16 | 32)) F.clut_from_draw++;
    if (uniq_add(&u_cbp, (uint64_t)cbp << 8 | (uint64_t)csa << 1 | 1)) F.clut_uniq++;
    char kd[100];
    snprintf(kd, sizeof kd, "%d %d %d %d %d %d", tpsm, cpsm, csm, csa, cld, m);
    tab_add(t_clut, fnv(kd, strlen(kd), FNV0), 1, d);
}

static void write_reg(uint32_t r, uint64_t v) {
    F.regs++;
    switch (r) {
    case R_PRIM: reg[r] = v; vcount = 0; break;
    case R_XYZF2: vertex(v, 1, 1); break;
    case R_XYZ2: vertex(v, 0, 1); break;
    case R_XYZF3: vertex(v, 1, 0); break;
    case R_XYZ3: vertex(v, 0, 0); break;
    case R_TEX0_1: case R_TEX0_2: tex0_write((int)(r - R_TEX0_1), v); break;
    case R_TEX2_1: case R_TEX2_2: {
        const uint64_t m = (0x3Full << 20) | (~0ull << 37);
        int ctx = (int)(r - R_TEX2_1);
        tex0_write(ctx, (reg[R_TEX0_1 + ctx] & ~m) | (v & m));
        break;
    }
    case R_TEXFLUSH: F.texflush++; break;
    case R_TRXDIR: reg[r] = v; trxdir((uint32_t)(v & 3)); break;
    case R_HWREG:
        if (xfer_dir == 0) { F.upload_bytes += 8; xfer_bytes += 8; }
        break;
    default: if (r < 0x64) reg[r] = v; break;
    }
}

static void image(uint32_t bytes) {
    if (xfer_dir == 0) { F.upload_bytes += bytes; xfer_bytes += bytes; }
}

/* ---- per frame / display ------------------------------------------------------------------------- */
static void add_counters(counters* t, const counters* f) {
    const uint64_t* a = (const uint64_t*)f;
    uint64_t* b = (uint64_t*)t;
    for (size_t i = 0; i < sizeof(counters) / 8; i++) b[i] += a[i];
}

static uint32_t frames_nonempty, frames_total;
static uint64_t max_prims, max_states, max_verts;

static void vblank(uint32_t vb, const uint64_t* snap) {
    uint64_t pmode = snap[0], smode2 = snap[2], dispfb1 = snap[7], display1 = snap[8], dispfb2 = snap[9], display2 = snap[10], bg = snap[14];
    int en1 = (int)(pmode & 1), en2 = (int)((pmode >> 1) & 1);
    int fbp1 = (int)(dispfb1 & 0x1FF), fbw1 = (int)((dispfb1 >> 9) & 0x3F), psm1 = (int)((dispfb1 >> 15) & 0x1F);
    int dbx1 = (int)((dispfb1 >> 32) & 0x7FF), dby1 = (int)((dispfb1 >> 43) & 0x7FF);
    int fbp2 = (int)(dispfb2 & 0x1FF), fbw2 = (int)((dispfb2 >> 9) & 0x3F), psm2 = (int)((dispfb2 >> 15) & 0x1F);
    int dbx2 = (int)((dispfb2 >> 32) & 0x7FF), dby2 = (int)((dispfb2 >> 43) & 0x7FF);
    char d[200];
    snprintf(d, sizeof d, "PMODE en1=%d en2=%d crtmd=%d mmod=%d amod=%d slbg=%d alp=%d | SMODE2 int=%d ffmd=%d dpms=%d | BGCOLOR %06llX", en1, en2,
             (int)((pmode >> 2) & 7), (int)((pmode >> 5) & 1), (int)((pmode >> 6) & 1), (int)((pmode >> 7) & 1), (int)((pmode >> 8) & 0xFF),
             (int)(smode2 & 1), (int)((smode2 >> 1) & 1), (int)((smode2 >> 2) & 3), (unsigned long long)(bg & 0xFFFFFF));
    tab_add(t_display, fnv(d, strlen(d), FNV0), 1, d);
    for (int i = 0; i < 2; i++) {
        uint64_t dsp = i ? display2 : display1, fb = i ? dispfb2 : dispfb1;
        if (!(i ? en2 : en1)) continue;
        snprintf(d, sizeof d, "DISPFB%d fbw=%d %s dbx=%d dby=%d | DISPLAY%d dx=%d dy=%d magh=%d magv=%d dw=%d dh=%d (-> %dx%d)", i + 1,
                 i ? fbw2 : fbw1, psm_name(i ? psm2 : psm1), i ? dbx2 : dbx1, i ? dby2 : dby1, i + 1, (int)(dsp & 0xFFF), (int)((dsp >> 12) & 0x7FF),
                 (int)((dsp >> 23) & 0xF), (int)((dsp >> 27) & 3), (int)((dsp >> 32) & 0xFFF), (int)((dsp >> 44) & 0x7FF),
                 ((int)((dsp >> 32) & 0xFFF) + 1) / ((int)((dsp >> 23) & 0xF) + 1), ((int)((dsp >> 44) & 0x7FF) + 1) / ((int)((dsp >> 27) & 3) + 1));
        tab_add(t_display, fnv(d, strlen(d), FNV0), 1, d);
    }
    /* what is displayed vs what was drawn */
    const char* rel1 = "none";
    if (en1) {
        rel1 = "other";
        for (int i = 0; i < frame_nfbp; i++) if (frame_fbps[i] == fbp1) rel1 = "drawn-this-frame";
        if (!strcmp(rel1, "other")) for (int i = 0; i < prev_nfbp; i++) if (prev_fbps[i] == fbp1) rel1 = "drawn-prev-frame";
    }
    const char* rel2 = "none";
    if (en2) {
        rel2 = "other";
        for (int i = 0; i < frame_nfbp; i++) if (frame_fbps[i] == fbp2) rel2 = "drawn-this-frame";
        if (!strcmp(rel2, "other")) for (int i = 0; i < prev_nfbp; i++) if (prev_fbps[i] == fbp2) rel2 = "drawn-prev-frame";
    }
    snprintf(d, sizeof d, "displayed: rc1 fbp=%03X %s, rc2 fbp=%03X %s", en1 ? fbp1 : -1, rel1, en2 ? fbp2 : -1, rel2);
    {
        char kd[100];
        snprintf(kd, sizeof kd, "%d %s %d %s", en1 ? fbp1 : -1, rel1, en2 ? fbp2 : -1, rel2);
        tab_add(t_display, fnv(kd, strlen(kd), FNV0), 1, d);
    }

    frames_total++;
    if (F.prims || F.uploads || F.copies || F.readbacks) frames_nonempty++;
    if (F.prims > max_prims) max_prims = F.prims;
    if (F.states > max_states) max_states = F.states;
    if (F.verts > max_verts) max_verts = F.verts;
    char fl[400];
    int o = snprintf(fl, sizeof fl, "f%u vb%u: prims=%llu verts=%llu states=%llu uniq=%llu tex=%llu/%llu | pt=%llu ln=%llu ls=%llu tr=%llu ts=%llu tf=%llu sp=%llu | tme=%llu abe=%llu fge=%llu iip=%llu aa1=%llu date=%llu ctx2=%llu | fb=%llu/%llu inpl=%llu z=%llu fbpx=%lluK clut=%llu/%llu/%llu | up=%llu/%lluK cp=%llu rb=%llu | area=%lluK | fbp=",
                     cur_frame, vb, (unsigned long long)F.prims, (unsigned long long)F.verts, (unsigned long long)F.states, (unsigned long long)F.states_uniq,
                     (unsigned long long)F.tex_binds, (unsigned long long)F.tex_uniq, (unsigned long long)F.types[0], (unsigned long long)F.types[1], (unsigned long long)F.types[2],
                     (unsigned long long)F.types[3], (unsigned long long)F.types[4], (unsigned long long)F.types[5], (unsigned long long)F.types[6],
                     (unsigned long long)F.tme, (unsigned long long)F.abe, (unsigned long long)F.fge, (unsigned long long)F.iip, (unsigned long long)F.aa1,
                     (unsigned long long)F.date, (unsigned long long)F.ctx2, (unsigned long long)F.feedback, (unsigned long long)F.feedback_prev,
                     (unsigned long long)F.inplace, (unsigned long long)F.zread, (unsigned long long)(F.fb_area / 1024), (unsigned long long)F.clut_loads,
                     (unsigned long long)F.clut_uniq, (unsigned long long)F.clut_from_draw,
                     (unsigned long long)F.uploads, (unsigned long long)(F.upload_bytes / 1024), (unsigned long long)F.copies, (unsigned long long)F.readbacks,
                     (unsigned long long)(F.area / 1024));
    for (int i = 0; i < frame_nfbp && o < (int)sizeof fl - 8; i++) o += snprintf(fl + o, sizeof fl - (size_t)o, "%s%03X", i ? "," : "", frame_fbps[i]);
    snprintf(fl + o, sizeof fl - (size_t)o, " | disp=%03X%s", en1 ? fbp1 : (en2 ? fbp2 : -1), en1 && en2 ? "+" : "");
    printf("%s\n", fl);
    {
        snprintf(d, sizeof d, "FRAME targets per frame: %d", frame_nfbp);
        tab_add(t_target_frames, (uint64_t)frame_nfbp + 1, 1, d);
    }

    add_counters(&T, &F);
    memset(&F, 0, sizeof F);
    prev_fb_pages = frame_fb_pages;
    memset(&frame_fb_pages, 0, sizeof frame_fb_pages);
    memcpy(prev_fbps, frame_fbps, sizeof prev_fbps);
    prev_nfbp = frame_nfbp;
    frame_nfbp = 0;
    uniq_clear();
    cur_frame++;
}

static int rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: gs_census <recording> [-v]\n"); return 2; }
    verbose = argc > 2 && !strcmp(argv[2], "-v");
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    setvbuf(f, NULL, _IOFBF, 1 << 22);
    char magic[8];
    static uint16_t clut[512];
    if (!rd(f, magic, 8) || memcmp(magic, "GSREC001", 8) || fseek(f, 4 << 20, SEEK_CUR) || !rd(f, clut, sizeof clut) || !rd(f, reg, sizeof reg)) {
        fprintf(stderr, "%s: not a GS recording\n", argv[1]);
        return 1;
    }
    tab_init(&t_frame, 4096); tab_init(&t_zbuf, 4096); tab_init(&t_framez, 4096); tab_init(&t_tex0, 8192); tab_init(&t_texsz, 1024);
    tab_init(&t_tex1, 1024); tab_init(&t_clamp, 4096); tab_init(&t_texa, 1024); tab_init(&t_alpha, 1024); tab_init(&t_test, 4096);
    tab_init(&t_ztst, 64); tab_init(&t_feedback, 8192); tab_init(&t_upload, 4096); tab_init(&t_readback, 1024); tab_init(&t_copy, 4096);
    tab_init(&t_clut, 1024); tab_init(&t_display, 1024); tab_init(&t_fx, 1024); tab_init(&t_fogcol, 1024); tab_init(&t_xyoff, 1024);
    tab_init(&t_prim, 1024); tab_init(&t_scissor, 1024); tab_init(&t_dimx, 64); tab_init(&t_target_frames, 64); tab_init(&t_sprite, 8192);
    tab_init(&t_pass, 8192);
    printf("# gs_census %s\n", argv[1]);
    {
        uint64_t t0 = reg[R_TEX0_1];
        printf("initial: PRMODECONT=%llu FRAME_1=%016llX ZBUF_1=%016llX TEX0_1=%016llX SCISSOR_1=%016llX XYOFFSET_1=%016llX\n",
               (unsigned long long)(reg[R_PRMODECONT] & 1), (unsigned long long)reg[R_FRAME_1], (unsigned long long)reg[R_ZBUF_1],
               (unsigned long long)t0, (unsigned long long)reg[R_SCISSOR_1], (unsigned long long)reg[R_XYOFFSET_1]);
    }
    uint64_t nrec = 0;
    for (;;) {
        uint32_t h[2];
        if (!rd(f, h, 8)) break;
        nrec++;
        if (h[0] == 1) {
            uint64_t v;
            if (!rd(f, &v, 8)) break;
            write_reg(h[1], v);
        } else if (h[0] == 2) {
            image(h[1]);
            if (fseek(f, (long)((h[1] + 3) & ~3u), SEEK_CUR)) break;
        } else if (h[0] == 3) {
            uint64_t snap[16];
            if (!rd(f, snap, sizeof snap)) break;
            vblank(h[1], snap);
        } else {
            fprintf(stderr, "bad record tag %u\n", h[0]);
            return 1;
        }
    }
    fclose(f);
    printf("\n# totals over %u VBlanks (%u with work): records=%llu regs=%llu\n", frames_total, frames_nonempty, (unsigned long long)nrec,
           (unsigned long long)T.regs);
    double nf = frames_nonempty ? (double)frames_nonempty : 1.0;
#define AVG(x) (double)T.x / nf
    printf("prims=%llu (avg %.0f/frame, max %llu) verts=%llu (avg %.0f, max %llu) state changes=%llu (avg %.0f, max %llu) uniq states=%llu (avg %.0f) type changes avg %.0f tex binds avg %.0f distinct textures avg %.0f distinct CLUTs avg %.0f\n",
           (unsigned long long)T.prims, AVG(prims), (unsigned long long)max_prims, (unsigned long long)T.verts, AVG(verts), (unsigned long long)max_verts,
           (unsigned long long)T.states, AVG(states), (unsigned long long)max_states, (unsigned long long)T.states_uniq, AVG(states_uniq), AVG(type_changes), AVG(tex_binds),
           AVG(tex_uniq), AVG(clut_uniq));
    printf("types: point=%llu line=%llu linestrip=%llu tri=%llu tristrip=%llu trifan=%llu sprite=%llu\n", (unsigned long long)T.types[0],
           (unsigned long long)T.types[1], (unsigned long long)T.types[2], (unsigned long long)T.types[3], (unsigned long long)T.types[4],
           (unsigned long long)T.types[5], (unsigned long long)T.types[6]);
    printf("attrs: aa1=%llu iip=%llu tme=%llu fge=%llu abe=%llu fst(of tme)=%llu ctx2=%llu fix=%llu | ate=%llu date=%llu pabe=%llu fba=%llu dthe=%llu colclamp0=%llu ztest(GEQ/GT)=%llu zwrite=%llu nodraw=%llu\n",
           (unsigned long long)T.aa1, (unsigned long long)T.iip, (unsigned long long)T.tme, (unsigned long long)T.fge, (unsigned long long)T.abe,
           (unsigned long long)T.fst, (unsigned long long)T.ctx2, (unsigned long long)T.fix, (unsigned long long)T.ate, (unsigned long long)T.date,
           (unsigned long long)T.pabe, (unsigned long long)T.fba, (unsigned long long)T.dthe, (unsigned long long)T.colclamp0, (unsigned long long)T.ztst_on,
           (unsigned long long)T.zwrite, (unsigned long long)T.kicks_nodraw);
    printf("feedback prims: same-frame=%llu prev-frame=%llu in-place=%llu zbuf-read=%llu feedback area %.0f Kpx/frame | clut loads=%llu (from drawn=%llu) texflush=%llu | sprites >=256x128: %llu\n",
           (unsigned long long)T.feedback, (unsigned long long)T.feedback_prev, (unsigned long long)T.inplace, (unsigned long long)T.zread, AVG(fb_area) / 1024.0,
           (unsigned long long)T.clut_loads, (unsigned long long)T.clut_from_draw, (unsigned long long)T.texflush, (unsigned long long)T.sprites_fullscreen);
    printf("transfers: uploads=%llu (%.1f/frame, %.1f KB/frame, %llu px) to drawn targets=%llu | copies=%llu (%llu px) | readbacks=%llu (%llu bytes) | area %.1f Mpx/frame (bbox estimate)\n",
           (unsigned long long)T.uploads, AVG(uploads), AVG(upload_bytes) / 1024.0, (unsigned long long)T.upload_px, (unsigned long long)T.upload_to_fb,
           (unsigned long long)T.copies, (unsigned long long)T.copy_px, (unsigned long long)T.readbacks, (unsigned long long)T.readback_bytes, AVG(area) / 1048576.0);
    printf("state changes by register group (a change may count in several groups; 'alone' = the only group that changed):\n");
    for (int i = 0; i < G_COUNT; i++)
        printf("  %-36s %10llu changes (%5.1f%%)  alone %10llu (%5.1f%%)\n", g_names[i], (unsigned long long)g_changes[i],
               T.states ? 100.0 * (double)g_changes[i] / (double)T.states : 0.0, (unsigned long long)g_only[i],
               T.states ? 100.0 * (double)g_only[i] / (double)T.states : 0.0);
    tab_print(&t_pass, "pass signatures (state without texture identity / primitive type)", 60);
    tab_print(&t_prim, "primitive attribute combos", 40);
    tab_print(&t_frame, "FRAME targets (+scissor, xyoffset)", 60);
    tab_print(&t_zbuf, "ZBUF with FRAME", 40);
    tab_print(&t_target_frames, "distinct FRAME FBPs per frame", 10);
    tab_print(&t_tex0, "TEX0 (per textured prim)", 60);
    tab_print(&t_texsz, "texture sizes", 40);
    tab_print(&t_tex1, "TEX1", 20);
    tab_print(&t_clamp, "CLAMP", 30);
    tab_print(&t_texa, "TEXA (prims with 24/16-bit or indexed textures)", 10);
    tab_print(&t_clut, "CLUT loads", 30);
    tab_print(&t_alpha, "ALPHA (ABE prims)", 40);
    tab_print(&t_test, "TEST", 40);
    tab_print(&t_fogcol, "FOGCOL", 10);
    tab_print(&t_dimx, "DIMX (DTHE prims)", 10);
    tab_print(&t_feedback, "feedback: textures reading drawn memory (this/prev frame)", 80);
    tab_print(&t_sprite, "large sprites (>=256x128)", 60);
    tab_print(&t_upload, "host->local uploads", 50);
    tab_print(&t_copy, "local->local copies", 40);
    tab_print(&t_readback, "local->host readbacks", 20);
    tab_print(&t_display, "display registers (per VBlank)", 40);
    return 0;
}
