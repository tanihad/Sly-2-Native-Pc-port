/* Decoded texture cache.
 *
 * Each mipmap level the rasterizer samples is decoded once into a linear 0xAABBGGRR array, lazily
 * in 8x8-texel tiles (only the tiles a primitive can reach). Direct-colour texels come from
 * gs_tex_fetch() and indexed formats are stored as raw indices (gs_mem_read) plus a palette
 * obtained by calling gs_tex_fetch() once per distinct index at a texel holding it. When the
 * VRAM layout of a format has been verified against gs_mem at start-up (gs_tex_fast_init), the
 * raw texels are read from gs_vram directly instead, with the documented TEXA expansion; the CLUT
 * always comes from gs_mem.
 *
 * Invalidation: an entry remembers the VRAM page behind each page-sized rectangle of the texture; a
 * write to a page (drawing, host->local or local->local transfer, see gs_mark_rect) drops the tiles
 * of the rectangles it backs. A CLUT load (gs_clut_version) re-probes the palette and only
 * re-expands tiles if the colours changed. With deferred drawing, dropped tiles are marked
 * TILE_STALE and replaced texel buffers are kept until the batches sampling them are done. */
#include "gs_draw.h"

#include <limits.h>
#include <stdlib.h>

#define TEX_ENTRIES 512
#define TEX_BUDGET ((size_t)384 << 20)
#define NO_POS 0xFFFFFFFFu

/* TILE_STALE: the tile's texels were dropped (VRAM written, palette changed); deferred drawing may
 * still sample the old ones, so decoding it again must wait for that drawing. */
enum { TILE_IDX = 1, TILE_RGBA = 2, TILE_STALE = 4 };

typedef struct {
    int used, indexed;
    int fast;                                    /* decode from gs_vram directly (layout verified) */
    uint32_t id, last_use;
    uint32_t tbp, tbw, psm, tw, th, xkey;        /* cache key */
    int w, h, tiles_x, tiles_y;
    int rgba_tiles;                              /* tiles with TILE_RGBA set */
    uint8_t* tiles;                              /* TILE_* flags per 8x8 tile */
    uint16_t* row_rgba;                          /* tiles with TILE_RGBA set, per row of tiles */
    uint8_t* idx;                                /* indexed formats: raw indices */
    uint32_t* rgba;
    size_t bytes;
    uint32_t stamp, checked_serial;              /* page validity */
    uint32_t page0, npages;
    /* the texture as a grid of page-sized rectangles (pw x ph texels, pw/8 x ph/8 tiles): the VRAM
     * page holding each (rect_page; a TBP that is not page-aligned spills into the next page too) */
    int pw_sh, ph_sh, rcols, rrows, spill;
    uint16_t* rect_page;
    uint32_t pal_clut, pal_texa;                 /* palette validity */
    int pos_lost;                                /* some pos[] dropped with their tiles (reset_written) */
    gs_tex0 t;                                   /* last TEX0/TEXA used for decoding */
    gs_texa ta;
    uint32_t pos[256];                           /* (v << 16) | u of a texel with this index */
    uint32_t pal[256];
    uint32_t use_serial;                         /* newest deferred batch sampling rgba (gs_draw_pixel.c) */
} tex_entry;

uint32_t gs_clut_version;
uint32_t gs_tex_decodes;
static uint8_t raw_ok[64][2];            /* [psm][TBW & 1]: direct texel decode verified (see below) */
static tex_entry cache[TEX_ENTRIES];
static size_t cache_bytes;
static uint32_t next_id = 1, use_clock;
static uint32_t dummy_texel;                     /* when allocation fails */

static int is_indexed(uint32_t psm) {
    return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
}

static uint32_t texa_key(const gs_texa* ta) { return ta->ta0 | (ta->aem << 8) | (ta->ta1 << 16) | 0x80000000u; }

static uint32_t entry_xkey(const gs_tex0* t, const gs_texa* ta) {
    if (is_indexed(t->psm)) return t->cpsm | (t->csa << 8) | (t->csm << 16);
    if (t->psm == PSMCT32 || t->psm == PSMZ32) return 0;
    return texa_key(ta);                         /* 24/16-bit texels expand alpha through TEXA */
}

static uint32_t pal_texa_key(const tex_entry* e, const gs_texa* ta) {
    return e->t.cpsm == PSMCT32 ? 0 : texa_key(ta);
}

uint16_t gs_page_texrefs[512];

static void ref_pages(const tex_entry* e, int d) {
    for (uint32_t i = 0; i < e->npages; i++) gs_page_texrefs[(e->page0 + i) & 511] = (uint16_t)(gs_page_texrefs[(e->page0 + i) & 511] + d);
}

static void free_entry(tex_entry* e) {
    if (gs_draw_busy(e->use_serial)) gs_draw_sync_why(GS_SYNC_TEXCACHE);   /* deferred drawing still samples it */
    ref_pages(e, -1);
    free(e->tiles); free(e->idx); free(e->rgba); free(e->row_rgba); free(e->rect_page);
    cache_bytes -= e->bytes;
    memset(e, 0, sizeof *e);
}

void gs_tex_flush_all(void) {
    for (int i = 0; i < TEX_ENTRIES; i++)
        if (cache[i].used) free_entry(&cache[i]);
}

static void reset_tiles(tex_entry* e) {
    memset(e->tiles, 0, (size_t)e->tiles_x * (size_t)e->tiles_y);
    memset(e->row_rgba, 0, (size_t)e->tiles_y * sizeof *e->row_rgba);
    e->rgba_tiles = 0;
    if (e->indexed) for (int k = 0; k < 256; k++) e->pos[k] = NO_POS;
    e->pos_lost = 0;
    e->stamp = gs_vram_serial;
}

/* Drop the tiles of every page-sized rectangle whose VRAM page(s) were written since the entry was
 * last checked. Tiles elsewhere stay decoded: a texture often covers more than the buffer it is
 * read from (a 1024x1024 TEX0 over a 512x448 frame buffer also covers the buffers after it). */
static void reset_written(tex_entry* e) {
    int dropped = 0;
    const int tw = 1 << (e->pw_sh - 3), th = 1 << (e->ph_sh - 3);    /* tiles per rectangle */
    for (int r = 0; r < e->rrows; r++)
        for (int c = 0; c < e->rcols; c++) {
            uint32_t pg = e->rect_page[r * e->rcols + c];
            if (gs_page_gen[pg] <= e->stamp && !(e->spill && gs_page_gen[(pg + 1) & 511] > e->stamp)) continue;
            int ty1 = (r + 1) * th < e->tiles_y ? (r + 1) * th : e->tiles_y;
            int tx1 = (c + 1) * tw < e->tiles_x ? (c + 1) * tw : e->tiles_x;
            for (int ty = r * th; ty < ty1; ty++)
                for (int tx = c * tw; tx < tx1; tx++) {
                    uint8_t* f = &e->tiles[ty * e->tiles_x + tx];
                    if (!(*f & (TILE_IDX | TILE_RGBA))) continue;
                    if (*f & TILE_RGBA) { e->rgba_tiles--; e->row_rgba[ty]--; }
                    *f = TILE_STALE;
                    dropped = 1;
                }
        }
    if (dropped && e->indexed)                   /* palette probe positions must be in valid tiles */
        for (int k = 0; k < 256; k++)
            if (e->pos[k] != NO_POS && !(e->tiles[((e->pos[k] >> 19) * (uint32_t)e->tiles_x) + ((e->pos[k] & 0xFFFF) >> 3)] & TILE_IDX)) {
                e->pos[k] = NO_POS;
                e->pos_lost = 1;                 /* other valid tiles may still hold index k */
            }
    e->stamp = gs_vram_serial;
}

/* Find a probe position for every index that lost its own (reset_written) among the tiles whose
 * indices are still valid, so a palette change re-expands them correctly. */
static void find_lost_positions(tex_entry* e) {
    e->pos_lost = 0;
    for (int ty = 0; ty < e->tiles_y; ty++)
        for (int tx = 0; tx < e->tiles_x; tx++) {
            if (!(e->tiles[ty * e->tiles_x + tx] & TILE_IDX)) continue;
            int x0 = tx * 8, y0 = ty * 8, x1 = x0 + 8 < e->w ? x0 + 8 : e->w, y1 = y0 + 8 < e->h ? y0 + 8 : e->h;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint8_t k = e->idx[y * e->w + x];
                    if (e->pos[k] == NO_POS) e->pos[k] = ((uint32_t)y << 16) | (uint32_t)x;
                }
        }
}

/* Least recently used entry that is not part of the last few binds (one primitive binds <= 7). */
static tex_entry* evict_one(void) {
    tex_entry* best = NULL;
    for (int i = 0; i < TEX_ENTRIES; i++) {
        tex_entry* e = &cache[i];
        if (!e->used || e->last_use + 8 > use_clock) continue;
        if (!best || e->last_use < best->last_use) best = e;
    }
    if (best) free_entry(best);
    return best;
}

static tex_entry* create_entry(const gs_tex0* t, uint32_t xkey) {
    int w = 1 << t->tw, h = 1 << t->th;
    int tx = (w + 7) / 8, ty = (h + 7) / 8, ind = is_indexed(t->psm);
    size_t bytes = (size_t)w * h * 4 + (ind ? (size_t)w * h : 0) + (size_t)tx * ty;
    while (cache_bytes + bytes > TEX_BUDGET && evict_one()) {}
    tex_entry* e = NULL;
    for (int i = 0; i < TEX_ENTRIES && !e; i++) if (!cache[i].used) e = &cache[i];
    if (!e && !(e = evict_one())) return NULL;
    e->rgba = malloc(((size_t)w * h + (size_t)w + 1) * 4);     /* one spare row: never read, just a guard */
    e->idx = ind ? malloc((size_t)w * h) : NULL;
    e->tiles = malloc((size_t)tx * ty);
    e->row_rgba = malloc((size_t)ty * sizeof *e->row_rgba);
    if (!e->rgba || !e->tiles || !e->row_rgba || (ind && !e->idx)) {
        free(e->rgba); free(e->idx); free(e->tiles); free(e->row_rgba);
        memset(e, 0, sizeof *e);
        return NULL;
    }
    e->used = 1;
    e->indexed = ind;
    e->fast = t->tbw != 0 && raw_ok[t->psm & 63][t->tbw & 1];
    e->id = next_id++;
    e->tbp = t->tbp0; e->tbw = t->tbw; e->psm = t->psm; e->tw = t->tw; e->th = t->th; e->xkey = xkey;
    e->w = w; e->h = h; e->tiles_x = tx; e->tiles_y = ty;
    e->bytes = bytes;
    cache_bytes += bytes;

    /* pages covered: rows of pages starting at TBP (a block offset spills into one more page) */
    int pw, ph;
    gs_page_dims((int)t->psm, &pw, &ph);
    uint32_t ppr = t->tbw * 64 / (uint32_t)pw;
    if (!ppr) ppr = 1;
    uint32_t rows = ((uint32_t)h + (uint32_t)ph - 1) / (uint32_t)ph, cols = ((uint32_t)w + (uint32_t)pw - 1) / (uint32_t)pw;
    uint32_t n = (rows - 1) * ppr + cols + ((t->tbp0 & 31) != 0);
    e->page0 = t->tbp0 >> 5;
    e->npages = n > 512 ? 512 : n;
    e->pw_sh = __builtin_ctz((unsigned)pw);
    e->ph_sh = __builtin_ctz((unsigned)ph);
    e->rcols = (int)cols;
    e->rrows = (int)rows;
    e->spill = (t->tbp0 & 31) != 0;
    e->rect_page = malloc(rows * cols * sizeof *e->rect_page);
    if (!e->rect_page) { e->used = 0; free(e->rgba); free(e->idx); free(e->tiles); free(e->row_rgba); cache_bytes -= bytes; memset(e, 0, sizeof *e); return NULL; }
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++) e->rect_page[r * cols + c] = (uint16_t)((e->page0 + r * ppr + c) & 511);
    ref_pages(e, +1);
    e->pal_clut = gs_clut_version - 1;           /* force a palette probe */
    reset_tiles(e);
    e->checked_serial = gs_vram_serial;
    return e;
}

static void axis_params(int mode, int mn, int mx, int size, int level, int* lo, int* hi, int* msk, int* fix) {
    switch (mode) {
    case 0:                                      /* REPEAT */
        *lo = INT_MIN / 4; *hi = INT_MAX / 4; *msk = size - 1; *fix = 0; break;
    case 1:                                      /* CLAMP */
        *lo = 0; *hi = size - 1; *msk = -1; *fix = 0; break;
    case 2:                                      /* REGION_CLAMP */
        mn >>= level; mx >>= level;
        *hi = mx < size - 1 ? mx : size - 1;
        *lo = mn < *hi ? mn : *hi;
        *msk = -1; *fix = 0; break;
    default:                                     /* REGION_REPEAT: (c & UMSK) | UFIX */
        *lo = INT_MIN / 4; *hi = INT_MAX / 4; *msk = mn & (size - 1); *fix = mx & (size - 1); break;
    }
}

static void bind_entry(gs_texlvl* L, tex_entry* e) {
    if (!e) {
        L->tex = &dummy_texel; L->ushift = 0; L->w = L->h = 1;
        L->ulo = L->vlo = 0; L->uhi = L->vhi = 0; L->umsk = L->vmsk = 0; L->ufix = L->vfix = 0;
        L->entry = NULL; L->entry_id = 0;
        return;
    }
    e->last_use = ++use_clock;
    L->entry = e; L->entry_id = e->id;
    L->tex = e->rgba; L->ushift = (int)e->tw; L->w = e->w; L->h = e->h;
    uint64_t c = L->clamp;
    L->umode = (int)(c & 3); L->vmode = (int)((c >> 2) & 3);
    axis_params(L->umode, (int)((c >> 4) & 0x3FF), (int)((c >> 14) & 0x3FF), e->w, L->level, &L->ulo, &L->uhi, &L->umsk, &L->ufix);
    axis_params(L->vmode, (int)((c >> 24) & 0x3FF), (int)((c >> 34) & 0x3FF), e->h, L->level, &L->vlo, &L->vhi, &L->vmsk, &L->vfix);
}

/* Texel buffers replaced while deferred drawing (batch `serial`) may still sample them. */
typedef struct { void* p; uint32_t serial; } grave;
static grave graves[256];
static int ngraves;

static void bury(void* p, uint32_t serial) {
    int j = 0;
    for (int i = 0; i < ngraves; i++) {
        if (gs_draw_busy(graves[i].serial)) graves[j++] = graves[i];
        else free(graves[i].p);
    }
    ngraves = j;
    if (ngraves == (int)(sizeof graves / sizeof graves[0])) {
        gs_draw_sync_why(GS_SYNC_TEXCACHE);
        for (int i = 0; i < ngraves; i++) free(graves[i].p);
        ngraves = 0;
    }
    graves[ngraves++] = (grave){ p, serial };
}

static void check_entry(tex_entry* e, const gs_tex0* t, const gs_texa* ta) {
    e->t = *t;
    e->ta = *ta;
    if (e->checked_serial != gs_vram_serial) {
        for (uint32_t i = 0; i < e->npages; i++)
            if (gs_page_gen[(e->page0 + i) & 511] > e->stamp) { reset_written(e); break; }
        e->checked_serial = gs_vram_serial;
    }
    if (e->indexed) {
        uint32_t tk = pal_texa_key(e, ta);
        if (e->pal_clut != gs_clut_version || e->pal_texa != tk) {
            if (gs_draw_pages_pending(e->page0, e->npages)) gs_draw_sync_why(GS_SYNC_TEXCACHE);   /* reads texels in VRAM */
            if (e->pos_lost) find_lost_positions(e);
            int changed = 0;
            for (int k = 0; k < 256; k++) {
                if (e->pos[k] == NO_POS) continue;
                uint32_t c = gs_tex_fetch(&e->t, &e->ta, (int)(e->pos[k] & 0xFFFF), (int)(e->pos[k] >> 16));
                if (c != e->pal[k]) { e->pal[k] = c; changed = 1; }
            }
            if (changed) {
                /* Every tile gets expanded again with the new palette. If deferred drawing still
                 * samples the old colours (a font drawn with two CLUTs in turn), expand into a new
                 * buffer and free the old one once that drawing is done, instead of waiting. */
                uint8_t stale = 0;
                if (e->rgba_tiles && gs_draw_busy(e->use_serial)) {
                    uint32_t* fresh = malloc(((size_t)e->w * e->h + (size_t)e->w + 1) * 4);
                    if (fresh) { bury(e->rgba, e->use_serial); e->rgba = fresh; }
                    else stale = TILE_STALE;
                }
                size_t n = (size_t)e->tiles_x * (size_t)e->tiles_y;
                for (size_t i = 0; i < n; i++)
                    if (e->tiles[i] & TILE_RGBA) e->tiles[i] = (uint8_t)((e->tiles[i] & ~TILE_RGBA) | stale);
                memset(e->row_rgba, 0, (size_t)e->tiles_y * sizeof *e->row_rgba);
                e->rgba_tiles = 0;
            }
            e->pal_clut = gs_clut_version;
            e->pal_texa = tk;
        }
    }
}

/* Look up (or create) the cache entry for L's texture and bind it. */
static void cache_bind(gs_texlvl* L) {
    const gs_tex0 t = L->t;
    uint32_t xkey = entry_xkey(&t, &L->ta);
#define KEY_MATCH(c) ((c)->used && (c)->tbp == t.tbp0 && (c)->tbw == t.tbw && (c)->psm == t.psm && \
                      (c)->tw == t.tw && (c)->th == t.th && (c)->xkey == xkey)
    /* direct-mapped memo of recent lookups in front of the linear search */
    static tex_entry* memo[256];
    uint32_t h = (t.tbp0 * 2654435761u ^ t.psm * 40503u ^ (t.tw << 4 | t.th) * 977u ^ t.tbw * 131u ^ xkey) >> 24;
    tex_entry* e = memo[h];
    if (!e || !KEY_MATCH(e)) {
        e = NULL;
        for (int i = 0; i < TEX_ENTRIES; i++)
            if (KEY_MATCH(&cache[i])) { e = &cache[i]; break; }
        if (!e) e = create_entry(&t, xkey);
        memo[h] = e;
    }
#undef KEY_MATCH
    bind_entry(L, e);
    if (e) {
        check_entry(e, &L->t, &L->ta);
        L->tex = e->rgba;                        /* (a palette change may have replaced it) */
    }
}

static void set_wrap(gs_texlvl* L) {
    uint64_t c = L->clamp;
    L->umode = (int)(c & 3); L->vmode = (int)((c >> 2) & 3);
    axis_params(L->umode, (int)((c >> 4) & 0x3FF), (int)((c >> 14) & 0x3FF), L->w, L->level, &L->ulo, &L->uhi, &L->umsk, &L->ufix);
    axis_params(L->vmode, (int)((c >> 24) & 0x3FF), (int)((c >> 34) & 0x3FF), L->h, L->level, &L->vlo, &L->vhi, &L->vmsk, &L->vfix);
}

static int direct_setup(gs_texlvl* L);           /* below, with the raw texel layouts */
static void palette_update(gs_texlvl* L);

void gs_tex_bind(gs_texlvl* L, const gs_tex0* t0, const gs_texa* ta, uint64_t clamp, int level) {
    gs_prim_version++;                           /* *L is part of the primitive state */
    gs_tex0 t = *t0;
    if (t.tw > 10) t.tw = 10;
    if (t.th > 10) t.th = 10;
    L->t = t; L->ta = *ta; L->clamp = clamp; L->level = level;
    L->w = 1 << t.tw; L->h = 1 << t.th; L->ushift = (int)t.tw;
    set_wrap(L);
    L->entry = NULL; L->entry_id = 0;
    L->tex = &dummy_texel;
    L->pal = NULL; L->pal_slot = -1;
    L->vram = gs_vram;
    L->dkind = direct_setup(L);
    L->kind = L->dkind;
    if (L->kind >= 0) palette_update(L);
    else cache_bind(L);
}

void gs_tex_validate(gs_texlvl* L) {
    if (L->kind >= 0) {                          /* direct: only the palette can change */
        palette_update(L);
        return;
    }
    tex_entry* e = L->entry;
    if (!e || e->id != L->entry_id) {            /* evicted (or never allocated): bind again */
        cache_bind(L);
        gs_prim_version++;
        return;
    }
    e->last_use = ++use_clock;
    if (!(e->checked_serial == gs_vram_serial && (!e->indexed || (e->pal_clut == gs_clut_version &&
          e->pal_texa == pal_texa_key(e, &L->ta)))))
        check_entry(e, &L->t, &L->ta);
    if (L->tex != e->rgba) {                     /* a palette change replaced the texel buffer */
        L->tex = e->rgba;
        gs_prim_version++;
    }
}

void gs_tex_use_snapshot(gs_texlvl* L, int on) {
    const uint8_t* v = on ? gs_vram_shadow : gs_vram;
    if (L->kind < 0 || L->vram == v) return;
    L->vram = v;
    gs_prim_version++;
}

/* ---- fast raw texel access ----------------------------------------------------------------------
 * Stored texel values read straight from gs_vram (instead of one gs_mem call per texel). Layouts:
 * 32/24/16-bit formats use gs_draw_swz.c's documented tables (checked against gs_mem); T8 and T4
 * page layouts are probed from gs_mem_write at start-up. Every format is then verified against
 * gs_mem_read with random data, separately for even and odd buffer widths and unaligned bases;
 * only verified (format, width parity) pairs use this path. */

enum { K32 = GS_TK_32, K24 = GS_TK_24, K16 = GS_TK_16, K8 = GS_TK_8, K4 = GS_TK_4, K8H = GS_TK_8H,
       K4HL = GS_TK_4HL, K4HH = GS_TK_4HH };
typedef struct { int kind, pw_sh, ph_sh, es; const uint16_t* tab; uint32_t ppr, bp; } raw_layout;

static uint16_t t8_tab[64 * 128];        /* byte offset in a page of T8 texel (x & 127, y & 63) */
static uint16_t t4_tab[128 * 128];       /* nibble offset in a page of T4 texel (x & 127, y & 127) */

static int raw_layout_of(uint32_t psm, uint32_t bp, uint32_t bw, raw_layout* r) {
    const gs_fmt* f = NULL;
    switch (psm) {
    case PSMCT32: case PSMZ32: r->kind = K32; f = gs_fmt_of((int)psm); break;
    case PSMCT24: case PSMZ24: r->kind = K24; f = gs_fmt_of((int)psm); break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: r->kind = K16; f = gs_fmt_of((int)psm); break;
    case PSMT8H: r->kind = K8H; f = gs_fmt_of(PSMCT32); break;
    case PSMT4HL: r->kind = K4HL; f = gs_fmt_of(PSMCT32); break;
    case PSMT4HH: r->kind = K4HH; f = gs_fmt_of(PSMCT32); break;
    case PSMT8: r->kind = K8; r->tab = t8_tab; r->pw_sh = 7; r->ph_sh = 6; r->es = 0; break;
    case PSMT4: r->kind = K4; r->tab = t4_tab; r->pw_sh = 7; r->ph_sh = 7; r->es = 0; break;
    default: return 0;
    }
    if (f) { r->tab = f->tab; r->pw_sh = 6; r->ph_sh = f->ph_shift; r->es = f->elem_shift; }
    r->ppr = (bw * 64) >> r->pw_sh;
    if (!r->ppr) r->ppr = 1;
    r->bp = bp;
    return 1;
}

GS_AI uint32_t raw_texel(const raw_layout* r, int x, int y, const int KIND) {
    uint32_t page = ((uint32_t)y >> r->ph_sh) * r->ppr + ((uint32_t)x >> r->pw_sh);
    uint32_t ti = (((uint32_t)y & ((1u << r->ph_sh) - 1)) << r->pw_sh) | ((uint32_t)x & ((1u << r->pw_sh) - 1));
    if (KIND == K4) {
        uint32_t n = (((r->bp + page * 32) << 9) + r->tab[ti]) & 0x7FFFFFu;
        return (gs_vram[n >> 1] >> ((n & 1) * 4)) & 15;
    }
    uint32_t a = (((r->bp + page * 32) << 8) + ((uint32_t)r->tab[ti] << r->es)) & 0x3FFFFFu, v;
    switch (KIND) {
    case K32: memcpy(&v, gs_vram + a, 4); return v;
    case K24: memcpy(&v, gs_vram + a, 4); return v & 0xFFFFFF;
    case K16: { uint16_t h; memcpy(&h, gs_vram + a, 2); return h; }
    case K8: return gs_vram[a];
    case K8H: return gs_vram[a + 3];
    case K4HL: return gs_vram[a + 3] & 15;
    default: return gs_vram[a + 3] >> 4;
    }
}

/* raw values of one tile's texels [x0,x1) x [y0,y1) into out (row stride 8). A tile row never
 * crosses a page (tiles are 8-aligned, pages are 64 or 128 wide), so the page base is per row. */
GS_AI void raw_rows(const raw_layout* r, int x0, int y0, int x1, int y1, uint32_t* out, const int KIND) {
    const uint32_t phm = (1u << r->ph_sh) - 1, pwm = (1u << r->pw_sh) - 1;
    const int n = x1 - x0;
    for (int y = y0; y < y1; y++, out += 8) {
        uint32_t page = ((uint32_t)y >> r->ph_sh) * r->ppr + ((uint32_t)x0 >> r->pw_sh);
        const uint16_t* trow = r->tab + (((uint32_t)y & phm) << r->pw_sh) + ((uint32_t)x0 & pwm);
        if (KIND == K4) {
            uint32_t base = (r->bp + page * 32) << 9;
            for (int k = 0; k < n; k++) {
                uint32_t nb = (base + trow[k]) & 0x7FFFFFu;
                out[k] = (gs_vram[nb >> 1] >> ((nb & 1) * 4)) & 15;
            }
            continue;
        }
        uint32_t base = (r->bp + page * 32) << 8;
        for (int k = 0; k < n; k++) {
            uint32_t a = (base + ((uint32_t)trow[k] << r->es)) & 0x3FFFFFu, v;
            switch (KIND) {
            case K32: memcpy(&v, gs_vram + a, 4); break;
            case K24: memcpy(&v, gs_vram + a, 4); v &= 0xFFFFFF; break;
            case K16: { uint16_t h; memcpy(&h, gs_vram + a, 2); v = h; break; }
            case K8: v = gs_vram[a]; break;
            case K8H: v = gs_vram[a + 3]; break;
            case K4HL: v = gs_vram[a + 3] & 15u; break;
            default: v = (uint32_t)gs_vram[a + 3] >> 4; break;
            }
            out[k] = v;
        }
    }
}

static void raw_block(const raw_layout* r, int x0, int y0, int x1, int y1, uint32_t* out) {
    switch (r->kind) {
    case K32: raw_rows(r, x0, y0, x1, y1, out, K32); break;
    case K24: raw_rows(r, x0, y0, x1, y1, out, K24); break;
    case K16: raw_rows(r, x0, y0, x1, y1, out, K16); break;
    case K8: raw_rows(r, x0, y0, x1, y1, out, K8); break;
    case K4: raw_rows(r, x0, y0, x1, y1, out, K4); break;
    case K8H: raw_rows(r, x0, y0, x1, y1, out, K8H); break;
    case K4HL: raw_rows(r, x0, y0, x1, y1, out, K4HL); break;
    default: raw_rows(r, x0, y0, x1, y1, out, K4HH); break;
    }
}

/* stored value -> 0xAABBGGRR for direct-colour formats, with TEXA (GS manual: 24-bit texels get
 * TA0, or 0 when AEM and RGB = 0; 16-bit texels get TA1 when A = 1, else TA0 / 0 likewise) */
GS_AI uint32_t expand_texel(int kind, uint32_t v, const gs_texa* ta) {
    if (kind == K32) return v;
    if (kind == K24) return v | ((ta->aem && !v) ? 0 : ta->ta0 << 24);
    uint32_t rgb = ((v & 0x1F) << 3) | ((v & 0x3E0) << 6) | ((v & 0x7C00) << 9);
    uint32_t a = (v & 0x8000) ? ta->ta1 : (ta->aem && !(v & 0x7FFF)) ? 0 : ta->ta0;
    return rgb | a << 24;
}

static uint32_t lcg(uint32_t* s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

/* T8/T4 page layouts from gs_mem: write the texel index in 8-/4-bit slices, see where it lands */
static int probe_t8(void) {
    static uint8_t snap[2][8192];
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 128; x++) {
                uint32_t i = (uint32_t)(y * 128 + x);
                gs_mem_write(PSMT8, 0, 2, x, y, pass ? i >> 8 : i & 0xFF);
            }
        memcpy(snap[pass], gs_vram, 8192);
    }
    static uint8_t seen[8192];
    memset(seen, 0, sizeof seen);
    for (int b = 0; b < 8192; b++) {
        uint32_t i = snap[0][b] | (uint32_t)snap[1][b] << 8;
        if (i >= 8192 || seen[i]) return 0;
        seen[i] = 1;
        t8_tab[i] = (uint16_t)b;
    }
    return 1;
}

static int probe_t4(void) {
    static uint8_t snap[4][8192];
    for (int pass = 0; pass < 4; pass++) {
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++)
                gs_mem_write(PSMT4, 0, 2, x, y, ((uint32_t)(y * 128 + x) >> (pass * 4)) & 15);
        memcpy(snap[pass], gs_vram, 8192);
    }
    static uint8_t seen[16384];
    memset(seen, 0, sizeof seen);
    for (int n = 0; n < 16384; n++) {
        uint32_t i = 0;
        for (int pass = 0; pass < 4; pass++) i |= (uint32_t)((snap[pass][n >> 1] >> ((n & 1) * 4)) & 15) << (pass * 4);
        if (i >= 16384 || seen[i]) return 0;
        seen[i] = 1;
        t4_tab[i] = (uint16_t)n;
    }
    return 1;
}

/* random texels through gs_mem_write, read back with raw_texel; TEXA expansion vs gs_tex_fetch */
static int verify_raw(uint32_t psm, uint32_t bp, uint32_t bw) {
    raw_layout r;
    if (!raw_layout_of(psm, bp, bw, &r)) return 0;
    uint32_t seed = psm * 977u + bp * 13u + bw, bits = 0xFFFFFFFFu;
    switch (r.kind) { case K24: bits = 0xFFFFFF; break; case K16: bits = 0xFFFF; break;
                      case K8: case K8H: bits = 0xFF; break; case K4: case K4HL: case K4HH: bits = 15; break; }
    const int W = 200, H = 140;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) gs_mem_write((int)psm, bp, bw, x, y, lcg(&seed) & bits);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t want = gs_mem_read((int)psm, bp, bw, x, y), got;
            switch (r.kind) {
            case K32: got = raw_texel(&r, x, y, K32); break;
            case K24: got = raw_texel(&r, x, y, K24); break;
            case K16: got = raw_texel(&r, x, y, K16); break;
            case K8: got = raw_texel(&r, x, y, K8); break;
            case K4: got = raw_texel(&r, x, y, K4); break;
            case K8H: got = raw_texel(&r, x, y, K8H); break;
            case K4HL: got = raw_texel(&r, x, y, K4HL); break;
            default: got = raw_texel(&r, x, y, K4HH); break;
            }
            if (got != want) return 0;
        }
    if (r.kind == K32 || r.kind == K24 || r.kind == K16) {
        static const gs_texa tas[3] = { { 0x80, 0, 0x40 }, { 0x33, 1, 0x7F }, { 0, 1, 0xFF } };
        gs_tex0 t;
        memset(&t, 0, sizeof t);
        t.tbp0 = bp; t.tbw = bw; t.psm = psm; t.tw = 8; t.th = 8;
        for (int k = 0; k < 3; k++)
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    uint32_t v = raw_texel(&r, x * 7, y * 5, r.kind == K32 ? K32 : r.kind == K24 ? K24 : K16);
                    if (y == 0 && x < 4) v = 0;          /* exercise AEM with RGB = 0 */
                    gs_mem_write((int)psm, bp, bw, x * 7, y * 5, v);
                    if (expand_texel(r.kind, v, &tas[k]) != gs_tex_fetch(&t, &tas[k], x * 7, y * 5)) return 0;
                }
    }
    return 1;
}

void gs_tex_fast_init(void) {
    static int inited;
    if (inited) return;
    inited = 1;
    if (!gs_swz_fast) return;            /* gs_mem disagrees with the documented layout (or is a stub) */
    static uint8_t saved[GS_VRAM_SIZE];
    memcpy(saved, gs_vram, sizeof saved);
    int t8 = probe_t8(), t4 = probe_t4();
    static const uint32_t psms[] = { PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMZ32, PSMZ24, PSMZ16, PSMZ16S,
                                     PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH };
    char off[160] = "";
    for (unsigned i = 0; i < sizeof psms / sizeof psms[0]; i++) {
        uint32_t psm = psms[i];
        if ((psm == PSMT8 && !t8) || (psm == PSMT4 && !t4)) { snprintf(off + strlen(off), sizeof off - strlen(off), " %02X", psm); continue; }
        raw_ok[psm][0] = (uint8_t)(verify_raw(psm, 0, 2) && verify_raw(psm, 32 * 9 + 5, 4));
        raw_ok[psm][1] = (uint8_t)(verify_raw(psm, 32 * 21 + 17, 3) && verify_raw(psm, 32 * 40 + 3, 1));
        if (!raw_ok[psm][0] || !raw_ok[psm][1])
            snprintf(off + strlen(off), sizeof off - strlen(off), " %02X%s", psm, raw_ok[psm][0] ? "(odd TBW)" : "");
    }
    if (off[0]) fprintf(stderr, "[gs] direct texel decode not verified for PSM%s: those use gs_mem per texel\n", off);
    memcpy(gs_vram, saved, sizeof saved);
}

/* ---- direct sampling ------------------------------------------------------------------------------ */

/* Kind and layout for sampling L straight from VRAM, or -1 (layout of the format not verified, slow
 * gs_mem path, or TBW 0, where the raw layout's page arithmetic differs from gs_mem's). */
static int direct_setup(gs_texlvl* L) {
    raw_layout r;
    const gs_tex0* t = &L->t;
    if (!gs_swz_fast || !t->tbw || !raw_ok[t->psm & 63][t->tbw & 1] || !raw_layout_of(t->psm, t->tbp0, t->tbw, &r)) return -1;
    L->pw_sh = r.pw_sh; L->ph_sh = r.ph_sh; L->es = r.es;
    L->bp = r.bp; L->ppr = r.ppr; L->tab = r.tab;
    L->vram = gs_vram;
    return r.kind;
}

/* Palette snapshots for indexed kinds: deferred drawing samples them later, so a slot is reused
 * only when the batches that used it are done (pal_serial). Consecutive identical palettes (a CLUT
 * loaded again with the same colours) share a slot. */
#define PAL_SLOTS 2048
static uint32_t pal_mem[PAL_SLOTS][256];
static uint32_t pal_serial[PAL_SLOTS];
static int pal_next, pal_last = -1;
static uint32_t pal_last_n;

static void palette_update(gs_texlvl* L) {
    int k = L->kind;
    if (k != K8 && k != K4 && k != K8H && k != K4HL && k != K4HH) return;
    uint32_t tk = (L->t.cpsm == PSMCT16 || L->t.cpsm == PSMCT16S) ? texa_key(&L->ta) : 0;
    if (L->pal && L->pal_clut == gs_clut_version && L->pal_texa == tk) return;
    uint32_t n = (k == K8 || k == K8H) ? 256 : 16, tmp[256];
    for (uint32_t i = 0; i < n; i++) tmp[i] = gs_clut_lookup(&L->t, &L->ta, i);
    int s = pal_last;
    if (s < 0 || pal_last_n != n || memcmp(pal_mem[s], tmp, n * 4)) {
        int tries = 0;
        do {                                     /* the next slot deferred drawing no longer samples */
            s = pal_next;
            pal_next = (pal_next + 1) % PAL_SLOTS;
        } while (gs_draw_busy(pal_serial[s]) && ++tries < 64);
        if (gs_draw_busy(pal_serial[s])) gs_draw_sync_why(GS_SYNC_PALETTE);
        memcpy(pal_mem[s], tmp, n * 4);
        pal_last = s;
        pal_last_n = n;
    }
    if (L->pal != pal_mem[s]) gs_prim_version++;
    L->pal = pal_mem[s];
    L->pal_slot = s;
    L->pal_clut = gs_clut_version;
    L->pal_texa = tk;
}

static int axis_ranges(int mode, int a, int b, int size, int lo, int hi, int out[4]);

void gs_tex_read_pages(const gs_texlvl* L, int u0, int u1, int v0, int v1, uint64_t set[8]) {
    int ur[4], vr[4];
    int nu = axis_ranges(L->umode, u0, u1, L->w, L->ulo, L->uhi, ur);
    int nv = axis_ranges(L->vmode, v0, v1, L->h, L->vlo, L->vhi, vr);
    const uint32_t page0 = L->bp >> 5, spill = (L->bp & 31) != 0;
    for (int i = 0; i < nv; i++)
        for (int r = vr[2 * i] >> L->ph_sh; r <= vr[2 * i + 1] >> L->ph_sh; r++)
            for (int j = 0; j < nu; j++)
                for (int c = ur[2 * j] >> L->pw_sh; c <= ur[2 * j + 1] >> L->pw_sh; c++) {
                    uint32_t pg = (page0 + (uint32_t)r * L->ppr + (uint32_t)c) & 511u;
                    set[pg >> 6] |= 1ull << (pg & 63);
                    if (spill) { pg = (pg + 1) & 511u; set[pg >> 6] |= 1ull << (pg & 63); }
                }
}

static void decode_tile(tex_entry* e, int tx, int ty) {
    uint8_t* fl = &e->tiles[ty * e->tiles_x + tx];
    int x0 = tx * 8, y0 = ty * 8, x1 = x0 + 8 < e->w ? x0 + 8 : e->w, y1 = y0 + 8 < e->h ? y0 + 8 : e->h;
    /* deferred drawing (gs_draw_pixel.c) must not still write the VRAM page this tile is read from,
     * nor still sample the texels this tile held before it was dropped */
    uint32_t pg = e->rect_page[(y0 >> e->ph_sh) * e->rcols + (x0 >> e->pw_sh)];
    if (gs_draw_pages_pending(pg, 1u + (uint32_t)e->spill) || ((*fl & TILE_STALE) && gs_draw_busy(e->use_serial)))
        gs_draw_sync_why(GS_SYNC_TEXCACHE);
    *fl &= (uint8_t)~TILE_STALE;
    const gs_tex0* t = &e->t;
    raw_layout r;
    uint32_t raw[64];
    int fast = e->fast && raw_layout_of(t->psm, t->tbp0, t->tbw, &r);
    e->rgba_tiles++;
    e->row_rgba[ty]++;
    gs_tex_decodes++;
    if (!e->indexed) {
        if (fast) {
            raw_block(&r, x0, y0, x1, y1, raw);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) e->rgba[y * e->w + x] = expand_texel(r.kind, raw[(y - y0) * 8 + (x - x0)], &e->ta);
        } else {
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) e->rgba[y * e->w + x] = gs_tex_fetch(t, &e->ta, x, y);
        }
        *fl |= TILE_RGBA;
        return;
    }
    if (!(*fl & TILE_IDX)) {
        if (fast) raw_block(&r, x0, y0, x1, y1, raw);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                uint32_t k = fast ? raw[(y - y0) * 8 + (x - x0)] : gs_mem_read((int)t->psm, t->tbp0, t->tbw, x, y) & 0xFF;
                e->idx[y * e->w + x] = (uint8_t)k;
                if (e->pos[k] == NO_POS) {
                    e->pos[k] = ((uint32_t)y << 16) | (uint32_t)x;
                    e->pal[k] = gs_tex_fetch(t, &e->ta, x, y);
                }
            }
        *fl |= TILE_IDX;
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) e->rgba[y * e->w + x] = e->pal[e->idx[y * e->w + x]];
    *fl |= TILE_RGBA;
}

/* Texel ranges (inclusive, inside [0, size)) reachable from unwrapped coordinates [a, b]. */
static int axis_ranges(int mode, int a, int b, int size, int lo, int hi, int out[4]) {
    if (mode == 0) {
        if (b - a + 1 >= size) { out[0] = 0; out[1] = size - 1; return 1; }
        int a2 = a & (size - 1), b2 = b & (size - 1);
        if (a2 <= b2) { out[0] = a2; out[1] = b2; return 1; }
        out[0] = 0; out[1] = b2; out[2] = a2; out[3] = size - 1;
        return 2;
    }
    if (mode == 3) { out[0] = 0; out[1] = size - 1; return 1; }
    out[0] = a < lo ? lo : a > hi ? hi : a;
    out[1] = b < lo ? lo : b > hi ? hi : b;
    return 1;
}

void gs_tex_note_use(const gs_texlvl* L, uint32_t serial) {
    if (L->kind >= 0) {
        if (L->pal_slot >= 0 && serial > pal_serial[L->pal_slot]) pal_serial[L->pal_slot] = serial;
        return;
    }
    tex_entry* e = L->entry;
    if (e && e->id == L->entry_id && serial > e->use_serial) e->use_serial = serial;
}

void gs_tex_ensure(gs_texlvl* L, int u0, int u1, int v0, int v1) {
    tex_entry* e = L->entry;
    if (L->kind >= 0 || !e || e->rgba_tiles == e->tiles_x * e->tiles_y) return;
    int ur[4], vr[4];
    int nu = axis_ranges(L->umode, u0, u1, e->w, L->ulo, L->uhi, ur);
    int nv = axis_ranges(L->vmode, v0, v1, e->h, L->vlo, L->vhi, vr);
    for (int i = 0; i < nv; i++)
        for (int ty = vr[2 * i] >> 3; ty <= vr[2 * i + 1] >> 3; ty++) {
            if (e->row_rgba[ty] == e->tiles_x) continue;             /* the whole row is decoded */
            for (int j = 0; j < nu; j++)
                for (int tx = ur[2 * j] >> 3; tx <= ur[2 * j + 1] >> 3; tx++)
                    if (!(e->tiles[ty * e->tiles_x + tx] & TILE_RGBA)) decode_tile(e, tx, ty);
        }
}
