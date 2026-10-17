/* GS CLUT buffer and texel fetch.
 *
 * CLUT buffer: 1 KB inside the GS, modelled as 512 16-bit halves. A 16-bit CLUT (PSMCT16/16S)
 * entry at position p (= CSA*16 + index) occupies half p; a 32-bit CLUT (PSMCT32) entry keeps its
 * low 16 bits in half p and its high 16 bits in half p + 256 (p < 256 for valid CSA 0-15).
 * So a PSMCT16 view at CSA 16-31 sees the high halves of 32-bit entries, as on hardware.
 *
 * Loading (TEX0/TEX2 write with an indexed PSM), CLD:
 *   0 no load; 1 load; 2 load, CBP0 = CBP; 3 load, CBP1 = CBP;
 *   4 load and CBP0 = CBP only if CBP != CBP0; 5 likewise with CBP1; 6/7 no load.
 * CSM1: 16-entry CLUTs are an 8x2 rectangle at CBP; 256-entry CLUTs a 16x16 rectangle whose rows
 *   hold entries in pairs of 8: row 2k = entries 32k+0..7 then 32k+16..23, row 2k+1 = 32k+8..15
 *   then 32k+24..31 (index bits 3 and 4 swapped against raster order).
 * CSM2: the entries are a horizontal line in a CBW-wide buffer, starting at (COU*16, COV). */
#include "gs_mem_swizzle.h"

static uint16_t clut[512];
static uint32_t cbp0 = 0xFFFFFFFFu, cbp1 = 0xFFFFFFFFu;   /* nothing loaded yet: first compare loads */

const uint16_t* gs_clut_buffer(void) { return clut; }

static int psm_is_indexed(uint32_t psm) {
    return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
}
static int psm_is_t8(uint32_t psm) { return psm == PSMT8 || psm == PSMT8H; }
static int cpsm_is_16(uint32_t cpsm) { return cpsm == PSMCT16 || cpsm == PSMCT16S; }

/* (M9) CSM1 fast path: the entries of a CSM1 CLUT lie in a 16x16 (256 entries) or 8x2 (16) rectangle at CBP with
 * buffer width 1, inside one page column, so entry i is word (CT32) / halfword (CT16, CT16S) CBP * 64 (128) + a fixed
 * in-page offset: the address formula below with x, y < 16 and bw = 1, taken once into tables (the GS thread loads
 * thousands of CLUTs per frame; same reads, same results) */
static uint16_t off_t[3][2][256];        /* [CT32, CT16, CT16S][16 entries, 256 entries][entry] */
static int off_ready;

static void clut_offsets(void) {
    for (int k = 0; k < 3; k++)
        for (int big = 0; big < 2; big++)
            for (uint32_t i = 0; i < (big ? 256u : 16u); i++) {
                uint32_t x = big ? (i & 7) | ((i >> 1) & 8) : i & 7, y = big ? ((i >> 4) & 14) | ((i >> 3) & 1) : i >> 3;
                off_t[k][big][i] = k == 0 ? gs_swz_ct32[(y & 31) << 6 | (x & 63)] : k == 1 ? gs_swz_ct16[(y & 63) << 6 | (x & 63)]
                                                                                       : gs_swz_ct16s[(y & 63) << 6 | (x & 63)];
            }
    off_ready = 1;
}

static void clut_load(const gs_tex0* t, const gs_texclut* tc) {
    int cpsm = (int)t->cpsm;
    if (cpsm != PSMCT32 && !cpsm_is_16(t->cpsm)) {
        gs_mem_warn(GSW_CLUT_CPSM, "CLUT load with invalid CPSM 0x%02X (treated as PSMCT32)", cpsm);
        cpsm = PSMCT32;
    }
    uint32_t n = psm_is_t8(t->psm) ? 256 : 16, base = t->csa * 16u;
    if (!t->csm) {                                       /* (M9) CSM1: the offset tables */
        if (!off_ready) clut_offsets();
        const uint16_t* off = off_t[cpsm == PSMCT32 ? 0 : cpsm == PSMCT16 ? 1 : 2][n == 256];
        if (cpsm == PSMCT32) {
            uint32_t wb = t->cbp * 64u;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t c = gs_vram_rd32((wb + off[i]) & 0xFFFFFu), p = (base + i) & 511u;
                clut[p] = (uint16_t)c;
                clut[p ^ 256u] = (uint16_t)(c >> 16);
            }
        } else {
            uint32_t hb = t->cbp * 128u;
            for (uint32_t i = 0; i < n; i++) clut[(base + i) & 511u] = (uint16_t)gs_vram_rd16((hb + off[i]) & 0x1FFFFFu);
        }
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x, y, bw;
        if (t->csm) {                                    /* CSM2: linear strip */
            x = (tc->cou * 16u + i) & 2047u;
            y = tc->cov & 2047u;
            bw = tc->cbw;
        } else if (n == 16) {                            /* CSM1, 16 entries: 8x2 */
            x = i & 7; y = i >> 3; bw = 1;
        } else {                                         /* CSM1, 256 entries: 16x16, bits 3/4 swapped */
            x = (i & 7) | ((i >> 1) & 8);
            y = ((i >> 4) & 14) | ((i >> 3) & 1);
            bw = 1;
        }
        uint32_t c = gs_px_read(cpsm, t->cbp, bw, x, y);
        uint32_t p = (base + i) & 511u;
        clut[p] = (uint16_t)c;
        if (cpsm == PSMCT32) clut[p ^ 256u] = (uint16_t)(c >> 16);
    }
    if (t->csm && !cpsm_is_16(t->cpsm))
        gs_mem_warn(GSW_CLUT_CSM2_PSM, "CSM2 CLUT with CPSM 0x%02X (hardware supports PSMCT16 only)", (int)t->cpsm);
}

void gs_clut_on_tex0(const gs_tex0* t, const gs_texclut* tc) {
    if (!psm_is_indexed(t->psm)) return;
    switch (t->cld) {
    case 1: clut_load(t, tc); break;
    case 2: clut_load(t, tc); cbp0 = t->cbp; break;
    case 3: clut_load(t, tc); cbp1 = t->cbp; break;
    case 4: if (t->cbp != cbp0) { clut_load(t, tc); cbp0 = t->cbp; } break;
    case 5: if (t->cbp != cbp1) { clut_load(t, tc); cbp1 = t->cbp; } break;
    default: break;                                      /* 0: keep; 6, 7: reserved */
    }
}

/* 16-bit colour (A1 B5 G5 R5) to 0xAABBGGRR through TEXA. */
static inline uint32_t expand16(uint32_t c, const gs_texa* ta) {
    uint32_t rgb = (c & 0x1Fu) << 3 | (c & 0x3E0u) << 6 | (c & 0x7C00u) << 9;
    uint32_t a = (c & 0x8000u) ? ta->ta1 : (ta->aem && !(c & 0x7FFFu)) ? 0 : ta->ta0;
    return rgb | (a & 0xFFu) << 24;
}

static inline uint32_t expand24(uint32_t c, const gs_texa* ta) {
    uint32_t a = (ta->aem && !c) ? 0 : ta->ta0;
    return c | (a & 0xFFu) << 24;
}

static inline uint32_t clut_lookup(const gs_tex0* t, const gs_texa* ta, uint32_t idx) {
    uint32_t p = (t->csa * 16u + idx) & 511u;
    if (cpsm_is_16(t->cpsm)) return expand16(clut[p], ta);
    return clut[p] | (uint32_t)clut[p ^ 256u] << 16;
}

uint32_t gs_clut_lookup(const gs_tex0* t, const gs_texa* ta, uint32_t index) {
    return clut_lookup(t, ta, index & 255u);
}

uint32_t gs_tex_fetch(const gs_tex0* t, const gs_texa* ta, int u, int v) {
    uint32_t x = (uint32_t)u & 2047u, y = (uint32_t)v & 2047u, bp = t->tbp0, bw = t->tbw;
    switch (t->psm) {
    case PSMCT32: return gs_vram_rd32(gs_addr32(gs_swz_ct32, bp, bw, x, y));
    case PSMCT24: return expand24(gs_vram_rd32(gs_addr32(gs_swz_ct32, bp, bw, x, y)) & 0xFFFFFFu, ta);
    case PSMCT16: return expand16(gs_vram_rd16(gs_addr16(gs_swz_ct16, bp, bw, x, y)), ta);
    case PSMCT16S: return expand16(gs_vram_rd16(gs_addr16(gs_swz_ct16s, bp, bw, x, y)), ta);
    case PSMT8: return clut_lookup(t, ta, gs_vram[gs_addr8(bp, bw, x, y)]);
    case PSMT4: {
        uint32_t n = gs_addr4(bp, bw, x, y);
        return clut_lookup(t, ta, (gs_vram[n >> 1] >> ((n & 1) << 2)) & 15u);
    }
    case PSMT8H: return clut_lookup(t, ta, gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3]);
    case PSMT4HL: return clut_lookup(t, ta, gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3] & 15u);
    case PSMT4HH: return clut_lookup(t, ta, (uint32_t)gs_vram[(gs_addr32(gs_swz_ct32, bp, bw, x, y) << 2) + 3] >> 4);
    case PSMZ32: return gs_vram_rd32(gs_addr32(gs_swz_z32, bp, bw, x, y));
    case PSMZ24: return expand24(gs_vram_rd32(gs_addr32(gs_swz_z32, bp, bw, x, y)) & 0xFFFFFFu, ta);
    case PSMZ16: return expand16(gs_vram_rd16(gs_addr16(gs_swz_z16, bp, bw, x, y)), ta);
    case PSMZ16S: return expand16(gs_vram_rd16(gs_addr16(gs_swz_z16s, bp, bw, x, y)), ta);
    default:
        gs_mem_warn(GSW_TEX_PSM, "texture with invalid PSM 0x%02X (read as PSMCT32)", (int)t->psm);
        return gs_vram_rd32(gs_addr32(gs_swz_ct32, bp, bw, x, y));
    }
}
