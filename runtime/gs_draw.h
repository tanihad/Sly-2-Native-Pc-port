/* Internals shared by gs.c (privileged registers, CSR, display) and gs_draw*.c (general registers,
 * vertex queue, rasterizer, pixel pipeline, texture cache). Owner: gs-draw.
 *
 * Frame and Z buffers are accessed directly in gs_vram using the GS manual's documented page/block/
 * column layout (gs_draw_swz.c). At start-up that layout is checked against gs_mem_write(); if the
 * two disagree, every frame/Z access goes through gs_mem_read/gs_mem_write instead (slow but
 * correct). Textures are decoded into a cache of linear texels, invalidated per 8 KB page when VRAM
 * is written; texels are read through gs_mem (gs_tex_fetch / gs_mem_read), or straight from
 * gs_vram for formats whose layout was verified against gs_mem at start-up (gs_draw_tex.c). The
 * CLUT always comes from gs_mem. gs_mem therefore must not cache anything derived from gs_vram. */
#pragma once
#include "gs_internal.h"

#define GS_AI static inline __attribute__((always_inline))
#define GS_MAX_SPAN 2048                 /* pixels per span: coordinates are 11-bit */
#define GS_UVSH 12                       /* fraction bits of interpolated u/v (in 1/16 texel units) */

/* ---- gs.c: CSR events and SIGLBLID, used by the SIGNAL/FINISH/LABEL registers ---------------- */
void gs_csr_signal(uint32_t id, uint32_t mask);   /* SIGLBLID.SIGID update + SIGNAL event */
void gs_csr_finish(void);                         /* FINISH event */
void gs_csr_label(uint32_t id, uint32_t mask);    /* SIGLBLID.LBLID update */

/* ---- gs_draw.c --------------------------------------------------------------------------------- */
void gs_draw_reset(void);                /* CSR.RESET: drop the vertex queue, cancel transfers */

/* activity counters (PS2_LOG_GS=1 prints them from gs_vsync once a second) */
typedef struct { uint32_t regs, prims, sprites, tris, image_bytes, xfers; } gs_stats_t;
extern gs_stats_t gs_stats;
extern uint32_t gs_vsync_count;          /* gs_vsync calls so far (debug output) */

/* ---- gs_draw_swz.c: direct frame/Z buffer addressing ------------------------------------------ */
typedef struct {
    const uint16_t* tab;                 /* [ (y & (ph-1)) * 64 + (x & 63) ] -> element index in page */
    uint8_t ph_shift;                    /* page height: 32 (5) for 32/24-bit, 64 (6) for 16-bit */
    uint8_t elem_shift;                  /* byte offset = element << elem_shift (2 or 1) */
    uint8_t bits;                        /* 32, 24 or 16 */
} gs_fmt;

extern int gs_swz_fast;                  /* 1: gs_mem agrees with the documented layout */
extern int gs_swz_ready;                 /* gs_swz_init has run */
void gs_swz_init(void);                  /* idempotent; runs the layout self-check once */
#define GS_INIT() do { if (!gs_swz_ready) gs_swz_init(); } while (0)
const gs_fmt* gs_fmt_of(int psm);        /* PSMCT32/24/16/16S, PSMZ32/24/16/16S; else NULL */

/* Byte address in gs_vram of pixel (x, y) of a page-aligned buffer (page0 in 8 KB pages). */
GS_AI uint32_t gs_swz_addr(const gs_fmt* f, uint32_t page0, uint32_t bw, int x, int y) {
    uint32_t page = (page0 + ((uint32_t)y >> f->ph_shift) * bw + ((uint32_t)x >> 6)) & 511u;
    uint32_t e = f->tab[(((uint32_t)y & ((1u << f->ph_shift) - 1)) << 6) | ((uint32_t)x & 63u)];
    return (page << 13) | (e << f->elem_shift);
}

/* VRAM write tracking for the texture cache: gs_page_gen[p] is the gs_vram_serial value of the
 * last write to 8 KB page p. Only pages some cached texture covers (gs_page_texrefs[p] != 0) are
 * tracked; writes elsewhere leave gs_vram_serial unchanged, so cached textures stay valid cheaply. */
extern uint32_t gs_page_gen[512];
extern uint32_t gs_vram_serial;
extern uint16_t gs_page_texrefs[512];
void gs_page_dims(int psm, int* pw, int* ph);     /* page size in pixels for any PSM */
/* Mark the pages holding rectangle [x0,x1) x [y0,y1) of a buffer (bp in 256-byte blocks). */
void gs_mark_rect(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1);

/* ---- gs_draw_tex.c: decoded texture cache ------------------------------------------------------ */
typedef struct {
    const uint32_t* tex;                 /* 0xAABBGGRR texels, row stride 1 << ushift */
    int ushift, w, h;
    /* coordinate wrap per axis: c = clamp(c, lo, hi); c = (c & msk) | fix  (result in [0, w)) */
    int ulo, uhi, umsk, ufix, vlo, vhi, vmsk, vfix;
    int umode, vmode;                    /* CLAMP WMS/WMT */
    /* binding */
    gs_tex0 t;                           /* this level's TEX0 (tbp0/tbw/tw/th per level) */
    gs_texa ta;
    uint64_t clamp;
    int level;
    void* entry;                         /* cache entry backing `tex` */
    uint32_t entry_id;
} gs_texlvl;

extern uint32_t gs_clut_version;         /* bumped on every TEX0/TEX2 write that may load the CLUT */
extern uint32_t gs_tex_decodes;          /* bumped whenever texels are decoded from VRAM */

/* Bind one mipmap level (t describes that level: tbp0/tbw/tw/th already per level). */
void gs_tex_bind(gs_texlvl* L, const gs_tex0* t, const gs_texa* ta, uint64_t clamp, int level);
/* Re-validate a bound level against VRAM writes / CLUT changes since it was last used. */
void gs_tex_validate(gs_texlvl* L);
/* Make sure texels in the (unwrapped, inclusive) range [u0,u1] x [v0,v1] are decoded. */
void gs_tex_ensure(gs_texlvl* L, int u0, int u1, int v0, int v1);
void gs_tex_flush_all(void);             /* forget every cached texture (reset) */
void gs_tex_fast_init(void);             /* probe/verify direct texel decoding (called by gs_swz_init) */

/* ---- gs_draw_pixel.c: primitive state and span drawing ---------------------------------------- */
typedef struct gs_prim gs_prim;
typedef struct gs_span_start gs_span_start;

struct gs_prim {
    int solid;                           /* set by gs_prim_finalize: flat fill, nothing else */
    /* frame buffer */
    const gs_fmt* ffmt;
    int fpsm;
    uint32_t fpage, fbw, fbmsk;          /* fbmsk: 1 bits are not written (32-bit form) */
    uint32_t fmask, amask;               /* derived: write mask in the stored format; alpha bits (RGB_ONLY) */
    /* Z buffer */
    const gs_fmt* zfmt;
    int zpsm, ztst, zmsk, zuse;          /* zuse: Z buffer is read or written */
    uint32_t zpage, zbw, zmax;
    /* tests */
    int ate, atst, aref, afail, date, datm;
    uint8_t apass[256];                  /* alpha test result per source alpha */
    int apass_atst, apass_aref;          /* what apass[] was built for (zeroed = NEVER, 0) */
    /* blending */
    int abe, asel, bsel, csel, dsel, fix, pabe, fba, colclamp, dthe, std_blend;
    int8_t dimx[4][4];
    /* fog */
    int fge, fogr, fogg, fogb;
    /* texture */
    int tme, fst, tfx, tcc;
    int texm;                            /* 0 none, 1 point, 2 bilinear, 3 general (mipmaps/LOD) */
    int mmag, mmin, mxl, lcm, lshift, k; /* TEX1 */
    int nlevels, cur_level;              /* bound levels; level used by texm 1/2 */
    int nowrap;                          /* this primitive's texels need no wrap/clamp (texm 1/2) */
    float tw16, th16;                    /* 16 * level-0 size (STQ -> 1/16 texel units) */
    gs_texlvl lvl[7];
    /* misc */
    int iip, scanmsk;
    int scx0, scx1, scy0, scy1;          /* scissor, inclusive */
    /* per-pixel x steps */
    int32_t dr, dg, db, da, df;          /* 16.16 */
    int64_t dz;                          /* 32.16 */
    float ds, dt, dq;
    int32_t du, dv;                      /* 1/16 texel << GS_UVSH */
    float lod_q;                         /* Q for a per-primitive LOD (UV mode or sprites) */
};

struct gs_span_start {                   /* attribute values at the first pixel of a span */
    int32_t r, g, b, a, f;               /* 16.16 */
    int64_t z;                           /* 32.16 */
    float s, t, q;
    int32_t u, v;                        /* 1/16 texel << GS_UVSH */
};

/* Derive the per-primitive tables (alpha test, write masks, solid-fill shortcut). */
void gs_prim_finalize(gs_prim* p);

/* Spans of one primitive are collected and run through the pipeline stages together (pixels of
 * one primitive never overlap, so the order of stages across its spans does not matter).
 * stq: interpolate S/T/Q (else U/V). The state in *p must not change until gs_batch_end.
 * Multithreading: the stage functions in gs_draw_pixel.c work on an explicit batch, so worker
 * threads can each run their own batch (e.g. split by row bands); textures are decoded before
 * the spans are generated, so workers only read the texture cache. */
void gs_batch_begin(const gs_prim* p, int stq);
void gs_batch_span(int y, int x0, int n, const gs_span_start* s);   /* already scissored */
void gs_batch_end(void);
