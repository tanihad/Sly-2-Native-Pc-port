/* Decoded texture cache.
 *
 * Each mipmap level the rasterizer samples is decoded once into a linear 0xAABBGGRR array, lazily
 * in 8x8-texel tiles (only the tiles a primitive can reach). Texels come from gs_mem only:
 * direct-colour formats through gs_tex_fetch(), indexed formats as raw indices (gs_mem_read) plus a
 * palette obtained by calling gs_tex_fetch() once per distinct index at a texel holding it.
 *
 * Invalidation: an entry remembers the VRAM pages it covers; any write to one of them (drawing,
 * host->local or local->local transfer, see gs_mark_rect) drops all its tiles. A CLUT load
 * (gs_clut_version) re-probes the palette and only re-expands tiles if the colours changed. */
#include "gs_draw.h"

#include <limits.h>
#include <stdlib.h>

#define TEX_ENTRIES 512
#define TEX_BUDGET ((size_t)384 << 20)
#define NO_POS 0xFFFFFFFFu

enum { TILE_IDX = 1, TILE_RGBA = 2 };

typedef struct {
    int used, indexed;
    uint32_t id, last_use;
    uint32_t tbp, tbw, psm, tw, th, xkey;        /* cache key */
    int w, h, tiles_x, tiles_y;
    int rgba_tiles;                              /* tiles with TILE_RGBA set */
    uint8_t* tiles;                              /* TILE_* flags per 8x8 tile */
    uint8_t* idx;                                /* indexed formats: raw indices */
    uint32_t* rgba;
    size_t bytes;
    uint32_t stamp, checked_serial;              /* page validity */
    uint32_t page0, npages;
    uint32_t pal_clut, pal_texa;                 /* palette validity */
    gs_tex0 t;                                   /* last TEX0/TEXA used for decoding */
    gs_texa ta;
    uint32_t pos[256];                           /* (v << 16) | u of a texel with this index */
    uint32_t pal[256];
} tex_entry;

uint32_t gs_clut_version;
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
    ref_pages(e, -1);
    free(e->tiles); free(e->idx); free(e->rgba);
    cache_bytes -= e->bytes;
    memset(e, 0, sizeof *e);
}

void gs_tex_flush_all(void) {
    for (int i = 0; i < TEX_ENTRIES; i++)
        if (cache[i].used) free_entry(&cache[i]);
}

static void reset_tiles(tex_entry* e) {
    memset(e->tiles, 0, (size_t)e->tiles_x * (size_t)e->tiles_y);
    e->rgba_tiles = 0;
    if (e->indexed) for (int k = 0; k < 256; k++) e->pos[k] = NO_POS;
    e->stamp = gs_vram_serial;
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
    e->rgba = malloc((size_t)w * h * 4);
    e->idx = ind ? malloc((size_t)w * h) : NULL;
    e->tiles = malloc((size_t)tx * ty);
    if (!e->rgba || !e->tiles || (ind && !e->idx)) {
        free(e->rgba); free(e->idx); free(e->tiles);
        memset(e, 0, sizeof *e);
        return NULL;
    }
    e->used = 1;
    e->indexed = ind;
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

static void check_entry(tex_entry* e, const gs_tex0* t, const gs_texa* ta) {
    e->t = *t;
    e->ta = *ta;
    if (e->checked_serial != gs_vram_serial) {
        for (uint32_t i = 0; i < e->npages; i++)
            if (gs_page_gen[(e->page0 + i) & 511] > e->stamp) { reset_tiles(e); break; }
        e->checked_serial = gs_vram_serial;
    }
    if (e->indexed) {
        uint32_t tk = pal_texa_key(e, ta);
        if (e->pal_clut != gs_clut_version || e->pal_texa != tk) {
            int changed = 0;
            for (int k = 0; k < 256; k++) {
                if (e->pos[k] == NO_POS) continue;
                uint32_t c = gs_tex_fetch(&e->t, &e->ta, (int)(e->pos[k] & 0xFFFF), (int)(e->pos[k] >> 16));
                if (c != e->pal[k]) { e->pal[k] = c; changed = 1; }
            }
            if (changed) {
                size_t n = (size_t)e->tiles_x * (size_t)e->tiles_y;
                for (size_t i = 0; i < n; i++) e->tiles[i] &= (uint8_t)~TILE_RGBA;
                e->rgba_tiles = 0;
            }
            e->pal_clut = gs_clut_version;
            e->pal_texa = tk;
        }
    }
}

void gs_tex_bind(gs_texlvl* L, const gs_tex0* t0, const gs_texa* ta, uint64_t clamp, int level) {
    gs_tex0 t = *t0;
    if (t.tw > 10) t.tw = 10;
    if (t.th > 10) t.th = 10;
    L->t = t; L->ta = *ta; L->clamp = clamp; L->level = level;
    uint32_t xkey = entry_xkey(&t, ta);
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
    if (e) check_entry(e, &L->t, &L->ta);
}

void gs_tex_validate(gs_texlvl* L) {
    tex_entry* e = L->entry;
    if (!e || e->id != L->entry_id) {            /* evicted (or never allocated): bind again */
        gs_tex0 t = L->t; gs_texa ta = L->ta;
        gs_tex_bind(L, &t, &ta, L->clamp, L->level);
        return;
    }
    e->last_use = ++use_clock;
    if (e->checked_serial == gs_vram_serial && (!e->indexed || (e->pal_clut == gs_clut_version &&
        e->pal_texa == pal_texa_key(e, &L->ta))))
        return;
    check_entry(e, &L->t, &L->ta);
}

static void decode_tile(tex_entry* e, int tx, int ty) {
    uint8_t* fl = &e->tiles[ty * e->tiles_x + tx];
    int x0 = tx * 8, y0 = ty * 8, x1 = x0 + 8 < e->w ? x0 + 8 : e->w, y1 = y0 + 8 < e->h ? y0 + 8 : e->h;
    const gs_tex0* t = &e->t;
    e->rgba_tiles++;
    if (!e->indexed) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) e->rgba[y * e->w + x] = gs_tex_fetch(t, &e->ta, x, y);
        *fl |= TILE_RGBA;
        return;
    }
    if (!(*fl & TILE_IDX)) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                uint32_t k = gs_mem_read((int)t->psm, t->tbp0, t->tbw, x, y) & 0xFF;
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

void gs_tex_ensure(gs_texlvl* L, int u0, int u1, int v0, int v1) {
    tex_entry* e = L->entry;
    if (!e || e->rgba_tiles == e->tiles_x * e->tiles_y) return;
    int ur[4], vr[4];
    int nu = axis_ranges(L->umode, u0, u1, e->w, L->ulo, L->uhi, ur);
    int nv = axis_ranges(L->vmode, v0, v1, e->h, L->vlo, L->vhi, vr);
    for (int i = 0; i < nv; i++)
        for (int ty = vr[2 * i] >> 3; ty <= vr[2 * i + 1] >> 3; ty++)
            for (int j = 0; j < nu; j++)
                for (int tx = ur[2 * j] >> 3; tx <= ur[2 * j + 1] >> 3; tx++)
                    if (!(e->tiles[ty * e->tiles_x + tx] & TILE_RGBA)) decode_tile(e, tx, ty);
}
