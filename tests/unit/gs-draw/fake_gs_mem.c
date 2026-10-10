/* Test fake of gs_mem.c: VRAM addressing, CLUT, texel fetch and transfers, written only for the
 * gs-draw unit tests.
 *
 * Default: 32/24/16-bit formats use the documented page/block/column layout (block order written
 * here as bit interleavings, independently of gs_draw_swz.c's tables), so gs_draw's direct-access
 * fast path is enabled and cross-checked. With -DFAKE_LINEAR every format uses a plain row-major
 * layout inside each page, which gs_draw must detect (slow path through gs_mem_read/write).
 * T8/T4 always use a row-major page layout. CLUTs are read with the documented CSM1 arrangement
 * (CBW 1; T8: 16x16 with entries 8-15 / 16-23 of every 32 swapped, T4: 8x2). */
#include "gs_internal.h"

uint8_t gs_vram[GS_VRAM_SIZE];

static int bits_of(int psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMCT24: case PSMZ24: return 24;
    case PSMT8: return 8;
    case PSMT4: return 4;
    default: return 32;
    }
}

static void dims(int psm, int* pw, int* ph) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *pw = 64; *ph = 64; break;
    case PSMT8: *pw = 128; *ph = 64; break;
    case PSMT4: *pw = 128; *ph = 128; break;
    default: *pw = 64; *ph = 32; break;
    }
}

#define BIT(v, b) (((v) >> (b)) & 1u)

/* byte address (and nibble select for T4 in *nib) */
static uint32_t addr_of(int psm, uint32_t bp, uint32_t bw, int x, int y, int* nib) {
    int pw, ph;
    int base_psm = (psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH) ? PSMCT32 : psm;
    dims(base_psm, &pw, &ph);
    x &= 2047; y &= 2047;
    uint32_t ppr = bw * 64 / (uint32_t)pw;
    if (!ppr) ppr = 1;
    uint32_t page = (uint32_t)(y / ph) * ppr + (uint32_t)(x / pw);
    uint32_t lx = (uint32_t)(x % pw), ly = (uint32_t)(y % ph);
    *nib = 0;
    int bits = bits_of(base_psm);
#ifndef FAKE_LINEAR
    if (bits >= 16) {
        uint32_t blk, inblk;
        int z = base_psm >= PSMZ32;
        if (bits != 16) {
            uint32_t bx = lx / 8, by = ly / 8;
            blk = BIT(bx, 0) | BIT(by, 0) << 1 | BIT(bx, 1) << 2 | BIT(by, 1) << 3 | BIT(bx, 2) << 4;
            uint32_t cx = lx % 8, cy = ly % 8;
            inblk = ((cy / 2) * 16 + (cx / 2) * 4 + (cy % 2) * 2 + cx % 2) * 4;
        } else {
            uint32_t bx = lx / 16, by = ly / 8;
            if (base_psm == PSMCT16S || base_psm == PSMZ16S)
                blk = BIT(by, 0) | BIT(bx, 0) << 1 | BIT(by, 2) << 2 | BIT(by, 1) << 3 | BIT(bx, 1) << 4;
            else
                blk = BIT(by, 0) | BIT(bx, 0) << 1 | BIT(by, 1) << 2 | BIT(bx, 1) << 3 | BIT(by, 2) << 4;
            uint32_t cx = lx % 16, cy = ly % 8, w = (cx % 8 / 2) * 4 + (cy % 2) * 2 + cx % 2;
            inblk = ((cy / 2) * 32 + w * 2 + cx / 8) * 2;
        }
        if (z) blk ^= 24;
        uint32_t a = ((bp + page * 32 + blk) * 256 + inblk) & (GS_VRAM_SIZE - 1);
        if (psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH) { a += 3; *nib = psm == PSMT4HH; }
        return a;
    }
#endif
    uint32_t e = ly * (uint32_t)pw + lx;
    uint32_t a = (bp * 256 + page * 8192 + e * (uint32_t)(bits >= 24 ? 4 : bits) / (bits == 4 ? 8 : bits >= 24 ? 1 : 8));
    if (bits == 4) *nib = (int)(e & 1);
    if (psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH) { a += 3; *nib = psm == PSMT4HH; }
    return a & (GS_VRAM_SIZE - 1);
}

uint32_t gs_mem_read(int psm, uint32_t bp, uint32_t bw, int x, int y) {
    int nib;
    uint32_t a = addr_of(psm, bp, bw, x, y, &nib), v = 0;
    switch (psm) {
    case PSMT8: case PSMT8H: return gs_vram[a];
    case PSMT4: case PSMT4HL: case PSMT4HH: return (gs_vram[a] >> (nib * 4)) & 15;
    }
    int bits = bits_of(psm);
    memcpy(&v, gs_vram + a, bits == 16 ? 2 : 4);
    return bits == 24 ? v & 0xFFFFFF : v;
}

void gs_mem_write(int psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t value) {
    int nib;
    uint32_t a = addr_of(psm, bp, bw, x, y, &nib);
    switch (psm) {
    case PSMT8: case PSMT8H: gs_vram[a] = (uint8_t)value; return;
    case PSMT4: case PSMT4HL: case PSMT4HH:
        gs_vram[a] = (uint8_t)((gs_vram[a] & ~(15 << (nib * 4))) | ((value & 15) << (nib * 4)));
        return;
    }
    int bits = bits_of(psm);
    if (bits == 16) { uint16_t h = (uint16_t)value; memcpy(gs_vram + a, &h, 2); }
    else if (bits == 24) { uint32_t o; memcpy(&o, gs_vram + a, 4); o = (o & 0xFF000000u) | (value & 0xFFFFFF); memcpy(gs_vram + a, &o, 4); }
    else memcpy(gs_vram + a, &value, 4);
}

/* ---- CLUT ----------------------------------------------------------------------------------- */

static uint32_t clut[256];               /* raw entries (32-bit or 16-bit values) */
static uint32_t cbp0 = ~0u, cbp1 = ~0u;
int fake_clut_loads;

void gs_clut_on_tex0(const gs_tex0* t, const gs_texclut* tc) {
    (void)tc;
    int load = 0;
    switch (t->cld) {
    case 1: load = 1; break;
    case 2: load = 1; cbp0 = t->cbp; break;
    case 3: load = 1; cbp1 = t->cbp; break;
    case 4: load = t->cbp != cbp0; cbp0 = t->cbp; break;
    case 5: load = t->cbp != cbp1; cbp1 = t->cbp; break;
    default: break;
    }
    if (!load) return;
    fake_clut_loads++;
    int t4 = t->psm == PSMT4 || t->psm == PSMT4HL || t->psm == PSMT4HH;
    for (int i = 0; i < (t4 ? 16 : 256); i++) {
        /* CSM1: T8 CLUTs are 16x16 with entries 8-15 and 16-23 of every 32 swapped; T4 CLUTs 8x2 */
        int pos = t4 ? i : (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1);
        uint32_t v = gs_mem_read((int)t->cpsm, t->cbp, 1, t4 ? pos % 8 : pos % 16, t4 ? pos / 8 : pos / 16);
        clut[t4 ? ((t->csa * 16 + (uint32_t)i) & 255) : (uint32_t)i] = v;
    }
}

static uint32_t expand16(uint32_t v, const gs_texa* ta) {
    uint32_t rgb = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19);
    uint32_t a = (v & 0x8000) ? ta->ta1 : (ta->aem && rgb == 0) ? 0 : ta->ta0;
    return rgb | (a << 24);
}

uint32_t gs_tex_fetch(const gs_tex0* t, const gs_texa* ta, int u, int v) {
    uint32_t raw = gs_mem_read((int)t->psm, t->tbp0, t->tbw, u, v);
    switch (t->psm) {
    case PSMCT32: case PSMZ32: return raw;
    case PSMCT24: case PSMZ24: return raw | ((ta->aem && raw == 0) ? 0 : ta->ta0 << 24);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return expand16(raw, ta);
    default: {
        int t4 = t->psm == PSMT4 || t->psm == PSMT4HL || t->psm == PSMT4HH;
        uint32_t e = clut[t4 ? ((t->csa * 16 + raw) & 255) : raw];
        return t->cpsm == PSMCT32 ? e : expand16(e, ta);
    }
    }
}

/* ---- transfers ------------------------------------------------------------------------------ */

static struct { int active, dir, psm, sx, sy, dx, dy, w, h, x, y; uint32_t sbp, sbw, dbp, dbw; uint32_t acc; int accbits; } X;

void gs_mem_xfer_start(uint64_t bb, uint64_t pos, uint64_t rr, uint32_t dir) {
    X.dir = (int)dir;
    X.sbp = (uint32_t)(bb & 0x3FFF); X.sbw = (uint32_t)((bb >> 16) & 0x3F);
    X.dbp = (uint32_t)((bb >> 32) & 0x3FFF); X.dbw = (uint32_t)((bb >> 48) & 0x3F);
    X.sx = (int)(pos & 0x7FF); X.sy = (int)((pos >> 16) & 0x7FF);
    X.dx = (int)((pos >> 32) & 0x7FF); X.dy = (int)((pos >> 48) & 0x7FF);
    X.w = (int)(rr & 0xFFF); X.h = (int)((rr >> 32) & 0xFFF);
    X.x = X.y = 0; X.acc = 0; X.accbits = 0;
    X.active = dir <= 1;
    if (dir == 0) X.psm = (int)((bb >> 56) & 0x3F);
    if (dir == 1) X.psm = (int)((bb >> 24) & 0x3F);
    if (dir == 2) {
        int spsm = (int)((bb >> 24) & 0x3F), dpsm = (int)((bb >> 56) & 0x3F);
        for (int y = 0; y < X.h; y++)
            for (int x = 0; x < X.w; x++)
                gs_mem_write(dpsm, X.dbp, X.dbw, X.dx + x, X.dy + y, gs_mem_read(spsm, X.sbp, X.sbw, X.sx + x, X.sy + y));
    }
}

void gs_mem_xfer_write(const void* data, uint32_t bytes) {
    const uint8_t* p = data;
    if (!X.active || X.dir != 0) return;
    int bits = bits_of(X.psm == PSMT8H ? PSMT8 : (X.psm == PSMT4HL || X.psm == PSMT4HH) ? PSMT4 : X.psm);
    for (uint32_t i = 0; i < bytes && X.active; i++) {
        X.acc |= (uint32_t)p[i] << X.accbits;
        X.accbits += 8;
        while (X.accbits >= bits && X.active) {
            uint32_t v = bits == 32 ? X.acc : X.acc & ((1u << bits) - 1);
            gs_mem_write(X.psm, X.dbp, X.dbw, X.dx + X.x, X.dy + X.y, v);
            X.acc = bits == 32 ? 0 : X.acc >> bits;
            X.accbits -= bits;
            if (++X.x == X.w) { X.x = 0; if (++X.y == X.h) X.active = 0; }
        }
    }
}

uint32_t gs_mem_xfer_read(void* data, uint32_t bytes) {
    uint8_t* p = data;
    memset(data, 0, bytes);
    if (!X.active || X.dir != 1) return bytes;
    int bits = bits_of(X.psm);
    uint32_t i = 0;
    while (i < bytes && X.active) {
        uint32_t v = gs_mem_read(X.psm, X.sbp, X.sbw, X.sx + X.x, X.sy + X.y);
        for (int b = 0; b < bits; b += 8) p[i++] = (uint8_t)(v >> b);
        if (++X.x == X.w) { X.x = 0; if (++X.y == X.h) X.active = 0; }
    }
    return bytes;
}

int gs_mem_xfer_active(void) { return X.active; }
