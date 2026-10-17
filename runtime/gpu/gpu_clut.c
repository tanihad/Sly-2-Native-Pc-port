/* CLUT ring (notes/d3d11-renderer.md 3.2, 3.5): snapshots of the GS CLUT buffer (1 KB, 512 halves
 * as gs_mem_tex.c models it), content-hashed, so a draw names its palette by slot and CLUT reloads
 * of identical content (Sly reloads on every TEX0) cost nothing. Each slot also answers the min/max
 * alpha of the entries an indexed texture can use (pass-A As range, 4.1). GS thread. */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"                /* gs_clut_buffer */

#include <stdlib.h>

typedef struct { uint64_t hash; uint16_t data[512]; } clut_slot;

static clut_slot* ring;                  /* GPU_CLUT_SLOTS */
static uint32_t next_slot, filled;
#define MAP_N 8192
static uint32_t map_slot[MAP_N];         /* slot + 1, 0 = empty; open addressing on the hash */
static uint64_t map_hash[MAP_N];
static int dirty = 1;
static int dirty_changed;                 /* the caller knows the content changed (frontend thread hint) */
static uint16_t last[512];               /* the CLUT buffer as last seen */
static int have_last;
static uint32_t content_id;
static uint32_t cur_slot;
static uint64_t used[GPU_CLUT_SLOTS / 64];  /* slots used this frame */

/* min/max alpha cache: (slot content hash, psm class, cpsm, csa, TEXA) */
#define AC_N 4096
typedef struct { uint64_t key; uint64_t hash; int16_t amin, amax; } alpha_entry;
static alpha_entry acache[AC_N];

static uint64_t hash1k(const uint16_t* c) {          /* four independent lanes (latency-bound otherwise) */
    const uint64_t* w = (const uint64_t*)c;
    uint64_t h0 = 0x9E3779B97F4A7C15ull, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    for (int i = 0; i < 128; i += 4) {
        h0 = (h0 ^ w[i]) * 0xFF51AFD7ED558CCDull; h1 = (h1 ^ w[i + 1]) * 0xFF51AFD7ED558CCDull;
        h2 = (h2 ^ w[i + 2]) * 0xFF51AFD7ED558CCDull; h3 = (h3 ^ w[i + 3]) * 0xFF51AFD7ED558CCDull;
        h0 ^= h0 >> 32; h1 ^= h1 >> 32; h2 ^= h2 >> 32; h3 ^= h3 >> 32;
    }
    uint64_t h = ((h0 * 31 + h1) * 31 + h2) * 31 + h3;
    h ^= h >> 29;
    return h ? h : 1;
}

void clut_reset(void) {
    if (!ring) ring = calloc(GPU_CLUT_SLOTS, sizeof *ring);
    next_slot = filled = 0;
    memset(map_slot, 0, sizeof map_slot);
    memset(acache, 0, sizeof acache);
    memset(used, 0, sizeof used);
    dirty = 1;
    have_last = 0;
}

void clut_mark_dirty(void) { dirty = 1; dirty_changed = 0; }
void clut_mark_changed(void) { dirty = 1; dirty_changed = 1; }

static void map_remove(uint64_t h, uint32_t slot) {
    uint32_t i = (uint32_t)h & (MAP_N - 1);
    for (int n = 0; n < MAP_N && map_slot[i]; n++, i = (i + 1) & (MAP_N - 1))
        if (map_slot[i] == slot + 1 && map_hash[i] == h) {
            /* backward-shift deletion keeps probe chains intact */
            uint32_t j = i;
            for (;;) {
                map_slot[j] = 0;
                uint32_t k = j;
                for (;;) {
                    k = (k + 1) & (MAP_N - 1);
                    if (!map_slot[k]) return;
                    uint32_t home = (uint32_t)map_hash[k] & (MAP_N - 1);
                    /* can entry k move to j? (home not cyclically in (j, k]) */
                    if (j <= k ? (home <= j || home > k) : (home <= j && home > k)) break;
                }
                map_slot[j] = map_slot[k]; map_hash[j] = map_hash[k];
                j = k;
            }
        }
}


/* a TEX0 write may have reloaded the CLUT: see whether its content changed (Sly reloads the same
 * palette on almost every TEX0, so most loads change nothing) */
static void refresh(void) {
    if (!ring) clut_reset();
    if (dirty) {
        dirty = 0;
        const uint16_t* c = gf_clut_src ? gf_clut_src : gs_clut_buffer();
        if (dirty_changed) have_last = 0;  /* (no compare, no copy: nothing compares with it later) */
        else {
            if (have_last && gf_eq64(c, last, sizeof last / 8)) return;
            memcpy(last, c, sizeof last);
            have_last = 1;
        }
        content_id++;
        uint64_t h = hash1k(c);
        uint32_t i = (uint32_t)h & (MAP_N - 1), found = 0xFFFFFFFFu;
        for (int n = 0; n < MAP_N && map_slot[i]; n++, i = (i + 1) & (MAP_N - 1))
            if (map_hash[i] == h && gf_eq64(ring[map_slot[i] - 1].data, c, 128)) { found = map_slot[i] - 1; break; }
        if (found == 0xFFFFFFFFu) {      /* miss: next ring slot, snapshot uploaded */
            found = next_slot;
            next_slot = (next_slot + 1) % GPU_CLUT_SLOTS;
            if (filled < GPU_CLUT_SLOTS) filled++;
            else map_remove(ring[found].hash, found);
            ring[found].hash = h;
            memcpy(ring[found].data, c, 1024);
            i = (uint32_t)h & (MAP_N - 1);
            while (map_slot[i]) i = (i + 1) & (MAP_N - 1);
            map_slot[i] = found + 1;
            map_hash[i] = h;
            gf.clut_new++;
            gpu_fop_clut* op = gf_cmd(GPU_FOP_CLUT, sizeof *op);
            op->slot = found;
            op->data_off = gf_data(c, 1024);
        }
        cur_slot = found;
        gf.clut_loads++;
    }
}

uint32_t clut_content_id(void) { refresh(); return content_id; }

uint32_t clut_current_slot(void) {
    refresh();
    if (!(used[cur_slot >> 6] & (1ull << (cur_slot & 63)))) {
        used[cur_slot >> 6] |= 1ull << (cur_slot & 63);
        gf.clut_slots++;
    }
    return cur_slot;
}

void clut_alpha_range(uint32_t slot, uint64_t tex0, uint64_t texa, int* amin, int* amax) {
    uint32_t psm = (uint32_t)((tex0 >> 20) & 0x3F), cpsm = (uint32_t)((tex0 >> 51) & 0xF), csa = (uint32_t)((tex0 >> 56) & 0x1F);
    int t8 = psm == PSMT8 || psm == PSMT8H;
    int c16 = cpsm == PSMCT16 || cpsm == PSMCT16S;
    uint64_t key = (uint64_t)t8 | (uint64_t)c16 << 1 | (uint64_t)csa << 2 | (c16 ? (texa & 0xFF000080FFull) << 8 : 0);
    const clut_slot* s = &ring[slot];
    uint64_t hk = (s->hash ^ key * 0x9E3779B97F4A7C15ull);
    alpha_entry* e = &acache[(hk ^ (hk >> 29)) & (AC_N - 1)];
    if (e->hash == s->hash && e->key == key && e->amin >= 0) { *amin = e->amin; *amax = e->amax; return; }
    uint32_t ta0 = (uint32_t)(texa & 0xFF), aem = (uint32_t)((texa >> 15) & 1), ta1 = (uint32_t)((texa >> 32) & 0xFF);
    int lo = 255, hi = 0, n = t8 ? 256 : 16;
    for (int idx = 0; idx < n; idx++) {
        uint32_t p = (csa * 16u + (uint32_t)idx) & 511u, a;
        if (c16) {
            uint32_t c = s->data[p];
            a = (c & 0x8000u) ? ta1 : (aem && !(c & 0x7FFFu)) ? 0 : ta0;
        } else {
            a = (uint32_t)s->data[p ^ 256u] >> 8;
        }
        if ((int)a < lo) lo = (int)a;
        if ((int)a > hi) hi = (int)a;
    }
    e->hash = s->hash; e->key = key; e->amin = (int16_t)lo; e->amax = (int16_t)hi;
    *amin = lo; *amax = hi;
}

/* (M9) the alpha of palette entry idx of an indexed texture (as clut_alpha_range reads it) */
int clut_entry_alpha(uint32_t slot, uint64_t tex0, uint64_t texa, uint32_t idx) {
    uint32_t cpsm = (uint32_t)((tex0 >> 51) & 0xF), csa = (uint32_t)((tex0 >> 56) & 0x1F);
    int c16 = cpsm == PSMCT16 || cpsm == PSMCT16S;
    const clut_slot* s = &ring[slot];
    uint32_t p = (csa * 16u + idx) & 511u;
    if (c16) {
        uint32_t ta0 = (uint32_t)(texa & 0xFF), aem = (uint32_t)((texa >> 15) & 1), ta1 = (uint32_t)((texa >> 32) & 0xFF);
        uint32_t c = s->data[p];
        return (int)((c & 0x8000u) ? ta1 : (aem && !(c & 0x7FFFu)) ? 0 : ta0);
    }
    return (int)((uint32_t)s->data[p ^ 256u] >> 8);
}

void clut_frame_end(void) { memset(used, 0, sizeof used); }
