/* Targets, ownership and block validity (notes/d3d11-renderer.md 3.3, 3.4, 5.3). GS thread.
 *
 * GS memory is tracked per 256-byte block (16384 blocks), the unit of the GS's own layout (8x8
 * pixels of a 32-bit buffer, 16x8 of a 16-bit one):
 *   blk_cpu[b]     which 32-bit words of the block gs_vram / vram_buf hold the truth for (uploads
 *                  set them word-exactly, a GPU write clears them);
 *   blk_writer[b]  the target that wrote the block last (0: the CPU side, i.e. an upload);
 *   blk_serial[b]  bumped whenever the block's last writer changes.
 * A target's copy of one of its blocks is current iff it recorded the block's present serial
 * (gpu_target.vs). So "the last writer owns the block and every other copy goes stale" needs no
 * reverse mapping, overlapping targets of any kind (Z16S over Z32, the Binocucom RTTs and the photo
 * copy inside the main Z pages) are separate targets, and every cross-copy is counted:
 *   load      target <- vram_buf (stale block read or partially written by a draw);
 *   download  vram_buf <- target, sample 0 (the CPU side or another target needs GPU-written data).
 * Uploads are queued as vram_buf patches and flushed in one op when something reads a pending block,
 * or at the end of the frame (FMV: ~1200 macroblocks -> one patch). */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"

#include <stdlib.h>

#define ALL ~0ull
gpu_target gt[GPU_MAX_TARGETS + 1];
uint32_t own_gen;

static uint16_t blk_writer[GPU_BLOCKS];
static uint32_t blk_serial[GPU_BLOCKS];
static uint64_t blk_cpu[GPU_BLOCKS];
static uint32_t blk_stamp[GPU_BLOCKS];   /* upload that last touched the block */
static uint64_t blk_pend[GPU_BLOCKS / 64];
static uint16_t pend_list[GPU_BLOCKS];
static uint32_t npend;
static uint8_t page_full[512];           /* blocks of the page with every word CPU-valid */
static uint8_t page_pend[512];           /* blocks of the page with a pending patch */
static uint32_t serial_ctr = 1, upload_ctr;

/* block lists of the op being built */
static uint16_t load_list[GPU_BLOCKS], dl_list[GPU_BLOCKS];
static uint32_t nload, ndl;

void own_reset(void) {
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        free(gt[i].babs); free(gt[i].vs); free(gt[i].sdirty);
    }
    memset(gt, 0, sizeof gt);
    memset(blk_writer, 0, sizeof blk_writer);
    for (int i = 0; i < GPU_BLOCKS; i++) { blk_serial[i] = 1; blk_cpu[i] = ALL; }
    memset(blk_stamp, 0, sizeof blk_stamp);
    memset(blk_pend, 0, sizeof blk_pend);
    npend = 0;
    memset(page_full, 32, sizeof page_full);
    memset(page_pend, 0, sizeof page_pend);
    serial_ctr = 1;
    own_gen++;
}

int own_target_kind_of_zpsm(int zpsm) {
    switch (zpsm) {
    case PSMZ32: return TGT_Z32;
    case PSMZ24: return TGT_Z24;
    case PSMZ16: return TGT_Z16;
    case PSMZ16S: return TGT_Z16S;
    default: return -1;
    }
}

static void set_cpu(uint32_t b, uint64_t m) {
    uint64_t old = blk_cpu[b];
    if (old == m) return;
    if (old == ALL) page_full[b >> 5]--;
    if (m == ALL) page_full[b >> 5]++;
    if (old & ~m) own_gen++;
    blk_cpu[b] = m;
}

static void target_pages(const gpu_target* T, int rows, uint64_t set[8]) {
    memset(set, 0, 8 * sizeof(uint64_t));
    int ph = 1 << T->fmt->ph_shift;
    for (int r = 0; r < (rows + ph - 1) / ph; r++)
        for (int c = 0; c < (int)T->fbw; c++) {
            uint32_t p = (T->base + (uint32_t)r * T->fbw + (uint32_t)c) & 511u;
            set[p >> 6] |= 1ull << (p & 63);
        }
}

/* (re)build the block maps for `rows` rows, keeping what is current */
static void target_layout(gpu_target* T, int rows) {
    int bcols = (int)T->fbw * 64 / T->bw, brows = rows / T->bh;
    if (bcols * brows > GPU_BLOCKS) { brows = GPU_BLOCKS / bcols; rows = brows * T->bh; }   /* (wraps 4 MB) */
    uint16_t* babs = malloc((size_t)bcols * (size_t)brows * sizeof *babs);
    uint32_t* vs = calloc((size_t)bcols * (size_t)brows, sizeof *vs);
    uint8_t* sd = calloc((size_t)bcols * (size_t)brows, 1);
    for (int by = 0; by < brows; by++)
        for (int bx = 0; bx < bcols; bx++) {
            babs[by * bcols + bx] = (uint16_t)(gs_swz_addr(T->fmt, T->base, T->fbw, bx * T->bw, by * T->bh) >> 8);
            if (T->vs && by < T->brows) vs[by * bcols + bx] = T->vs[by * T->bcols + bx];
        }
    free(T->babs); free(T->vs); free(T->sdirty);
    T->babs = babs; T->vs = vs; T->sdirty = sd;
    T->shadow_valid = 0;
    T->bcols = bcols; T->brows = brows; T->rows = rows;
    target_pages(T, rows, T->pages);
}

static void emit_target(const gpu_target* T, uint16_t op) {
    gpu_fop_target* o = gf_cmd(op, sizeof *o);
    o->id = T->id; o->kind = T->kind; o->base = T->base; o->fbw = T->fbw; o->rows = (uint16_t)T->rows;
    o->kx = T->kx; o->ky = T->ky; o->family = T->family;
}

static void set_scale(gpu_target* T) {
    int n = gf_cfg.scale > 0 ? gf_cfg.scale : 1;
    T->kx = (uint8_t)n;
    T->ky = (uint8_t)(T->family == FAM_DISPLAY ? 2 * n : n);
}

gpu_target* own_find(int kind, uint32_t base, uint32_t fbw) {
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        gpu_target* T = &gt[i];
        if (T->used && T->kind == kind && T->base == base && T->fbw == fbw) return T;
    }
    return NULL;
}

static void release(gpu_target* T);

gpu_target* own_target(int kind, int psm, uint32_t base, uint32_t fbw, int rows) {
    const gs_fmt* fmt = gs_fmt_of(psm);
    if (!fmt || !fbw) return NULL;
    int ph = 1 << fmt->ph_shift;
    if (rows < 1) rows = 1;
    rows = (rows + ph - 1) / ph * ph;
    if (rows > 2048) rows = 2048;
    gpu_target* T = own_find(kind, base, fbw);
    if (T) {
        if (rows > T->rows) {            /* taller use: recreate + copy */
            target_layout(T, rows);
            emit_target(T, GPU_FOP_TARGET);
        }
        T->last_used = gf_frame;
        return T;
    }
    /* a target of the same kind whose pages contain this base (a sub-buffer of it): PUNT */
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        gpu_target* U = &gt[i];
        if (U->used && U->kind == kind && (U->pages[base >> 6] >> (base & 63) & 1)) return NULL;
    }
    int id = 0;
    for (int i = 1; i <= GPU_MAX_TARGETS && !id; i++) if (!gt[i].used) id = i;
    if (!id) {                           /* full: evict the least recently used */
        uint32_t best = 0xFFFFFFFFu;
        for (int i = 1; i <= GPU_MAX_TARGETS; i++) if (gt[i].last_used < best) { best = gt[i].last_used; id = i; }
        release(&gt[id]);
    }
    T = &gt[id];
    memset(T, 0, sizeof *T);
    T->used = 1;
    T->id = (uint16_t)id;
    T->kind = (uint8_t)kind;
    T->psm = psm;
    T->fmt = fmt;
    T->base = (uint16_t)base;
    T->fbw = (uint16_t)fbw;
    T->bw = fmt->bits == 16 ? 16 : 8;
    T->bh = 8;
    T->family = FAM_SQUARE;
    set_scale(T);
    T->last_used = gf_frame;
    target_layout(T, rows);
    gf.targets_created++;
    emit_target(T, GPU_FOP_TARGET);
    return T;
}

/* ---- loads, downloads ---------------------------------------------------------------------------- */

static void flush_lists(const gpu_target* T) {
    if (ndl) {
        gpu_fop_blocks* o = gf_cmd(GPU_FOP_DOWNLOAD, sizeof *o);
        o->target = T ? T->id : 0;
        o->n = ndl;
        o->data_off = gf_data(dl_list, ndl * 2);
        gf.downloads++;
        gf.download_blocks += ndl;
        ndl = 0;
    }
    if (nload) {
        gpu_fop_blocks* o = gf_cmd(GPU_FOP_LOAD, sizeof *o);
        o->target = T->id;
        o->n = nload;
        o->data_off = gf_data(load_list, nload * 2);
        gf.loads++;
        gf.load_blocks += nload;
        nload = 0;
    }
}

static int pending(uint32_t b) { return (int)(blk_pend[b >> 6] >> (b & 63) & 1); }

/* GPU data of block b that the CPU side lacks goes to vram_buf (sample 0 of its writer) */
static void download_block(uint32_t b) {
    dl_list[ndl++] = (uint16_t)b;
    set_cpu(b, ALL);
}

/* T's local block i (absolute b) is stale: bring it up to date. The first stale block of a draw
 * loads every stale block of T whose data is in memory, in one op (an upload into a target's pages,
 * the title's background picture or a replay start, is followed by drawing all over it). */
static void load_block(gpu_target* T, int i, uint32_t b) {
    if (npend) own_flush_patches();
    if (!nload) {
        int n = T->bcols * T->brows;
        for (int k = 0; k < n; k++) {
            uint32_t a = T->babs[k];
            if (T->vs[k] != blk_serial[a] && blk_cpu[a] == ALL) {
                load_list[nload++] = (uint16_t)k;
                T->vs[k] = blk_serial[a];
            }
        }
    }
    if (T->vs[i] == blk_serial[b]) return;
    if (blk_cpu[b] != ALL) download_block(b);    /* another target's (or a lost writer's) data */
    load_list[nload++] = (uint16_t)i;
    T->vs[i] = blk_serial[b];
}

void own_draw(gpu_target* T, int x0, int y0, int x1, int y1, int writes, int opaque) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int bx0 = x0 / T->bw, by0 = y0 / T->bh, bx1 = x1 / T->bw, by1 = y1 / T->bh;
    if (bx1 >= T->bcols) bx1 = T->bcols - 1;
    if (by1 >= T->brows) by1 = T->brows - 1;
    T->last_used = gf_frame;
    for (int by = by0; by <= by1; by++) {
        int fully_y = by * T->bh >= y0 && by * T->bh + T->bh - 1 <= y1;
        for (int bx = bx0; bx <= bx1; bx++) {
            int i = by * T->bcols + bx;
            uint32_t b = T->babs[i];
            if (T->vs[i] != blk_serial[b]) {
                int full = writes && opaque && fully_y && bx * T->bw >= x0 && bx * T->bw + T->bw - 1 <= x1;
                if (!full) load_block(T, i, b);
            }
            if (writes) {
                if (blk_writer[b] != T->id) {
                    blk_writer[b] = T->id;
                    blk_serial[b] = ++serial_ctr;
                    set_cpu(b, 0);
                } else if (blk_cpu[b]) set_cpu(b, 0);
                T->vs[i] = blk_serial[b];
                T->sdirty[i] = 1;
            }
        }
    }
    if (nload || ndl) flush_lists(T);
}

void own_clear(gpu_target* T) { own_draw(T, 0, 0, T->bcols * T->bw - 1, T->rows - 1, 1, 1); }

void own_view_read(gpu_target* T, int x0, int y0, int x1, int y1) { own_draw(T, x0, y0, x1, y1, 0, 0); }

int own_shadow_need(gpu_target* T, int x0, int y0, int x1, int y1, int* full) {
    int n = T->bcols * T->brows;
    *full = 0;
    if (!T->shadow_valid) {              /* first copy: the whole target, later reads of other blocks are free */
        T->shadow_valid = 1;
        memset(T->sdirty, 0, (size_t)n);
        *full = 1;
        return 1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int bx0 = x0 / T->bw, by0 = y0 / T->bh, bx1 = x1 / T->bw, by1 = y1 / T->bh, need = 0;
    if (bx1 >= T->bcols) bx1 = T->bcols - 1;
    if (by1 >= T->brows) by1 = T->brows - 1;
    for (int by = by0; by <= by1; by++)
        for (int bx = bx0; bx <= bx1; bx++) need |= T->sdirty[by * T->bcols + bx];
    if (!need) return 0;
    /* refresh everything written since the last copy in one copy (its bounding box): the other
     * blocks a feedback pass reads next are then current without another copy */
    int cx0 = T->bcols, cy0 = T->brows, cx1 = -1, cy1 = -1;
    for (int by = 0; by < T->brows; by++)
        for (int bx = 0; bx < T->bcols; bx++)
            if (T->sdirty[by * T->bcols + bx]) {
                if (bx < cx0) cx0 = bx;
                if (bx > cx1) cx1 = bx;
                if (by < cy0) cy0 = by;
                if (by > cy1) cy1 = by;
            }
    memset(T->sdirty, 0, (size_t)n);
    *full = 2;
    T->copy_rect[0] = cx0 * T->bw; T->copy_rect[1] = cy0 * T->bh;
    T->copy_rect[2] = cx1 * T->bw + T->bw - 1; T->copy_rect[3] = cy1 * T->bh + T->bh - 1;
    return 1;
}

/* ---- memory sources ---------------------------------------------------------------------------- */

/* 32-bit word address of texel (x, y) */
static uint32_t word_of(int psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    switch (psm) {
    case PSMCT16: return gs_addr16(gs_swz_ct16, bp, bw, x, y) >> 1;
    case PSMCT16S: return gs_addr16(gs_swz_ct16s, bp, bw, x, y) >> 1;
    case PSMZ16: return gs_addr16(gs_swz_z16, bp, bw, x, y) >> 1;
    case PSMZ16S: return gs_addr16(gs_swz_z16s, bp, bw, x, y) >> 1;
    case PSMT8: return gs_addr8(bp, bw, x, y) >> 2;
    case PSMT4: return gs_addr4(bp, bw, x, y) >> 3;
    case PSMZ32: case PSMZ24: return gs_addr32(gs_swz_z32, bp, bw, x, y);
    default: return gs_addr32(gs_swz_ct32, bp, bw, x, y);
    }
}

/* mode 0: test only; 1: flush/download as needed */
static int mem_check(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1, int act) {
    if (u0 < 0) u0 = 0;
    if (v0 < 0) v0 = 0;
    if (u1 > 2047) u1 = 2047;
    if (v1 > 2047) v1 = 2047;
    if (u0 > u1 || v0 > v1) return 0;
    uint64_t set[8] = { 0 };
    gs_rect_pages(psm, bp, bw, u0, v0, u1 + 1, v1 + 1, set);
    int slow = 0;
    for (int k = 0; k < 8 && !slow; k++)
        for (uint64_t m = set[k]; m; m &= m - 1) {
            int p = k * 64 + __builtin_ctzll(m);
            if (page_full[p] != 32 || page_pend[p]) { slow = 1; break; }
        }
    if (!slow) return 0;
    /* exact: every texel's word (texture rectangles here are small or rare) */
    int gpu = 0;
    for (int v = v0; v <= v1; v++)
        for (int u = u0; u <= u1; u++) {
            uint32_t w = word_of(psm, bp, bw, (uint32_t)u, (uint32_t)v), b = w >> 6;
            if (act && pending(b)) own_flush_patches();
            if (!(blk_cpu[b] >> (w & 63) & 1)) {
                gpu = 1;
                if (!act) return 1;
                download_block(b);
            }
        }
    if (act && ndl) flush_lists(NULL);
    return gpu;
}

int own_mem_read(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1) {
    return mem_check(psm, bp, bw, u0, v0, u1, v1, 1);
}

int own_mem_is_gpu(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1) {
    return mem_check(psm, bp, bw, u0, v0, u1, v1, 0);
}

/* ---- uploads -------------------------------------------------------------------------------------- */

void own_upload(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1) {
    if (x1 > 2048) { x0 = 0; x1 = 2048; }
    if (y1 > 2048) { y0 = 0; y1 = 2048; }
    if (gs_psm_bpp(psm) == 0) psm = PSMCT32;
    upload_ctr++;
    /* PSMCT24/Z24 (low 3 bytes) and T8H/T4HL/T4HH (part of the top byte) uploads keep the rest of
     * each word: such a word is CPU-valid afterwards only if it was before */
    int partial_word = psm == PSMCT24 || psm == PSMZ24 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t w = word_of(psm, bp, bw, (uint32_t)x, (uint32_t)y), b = w >> 6;
            if (blk_stamp[b] != upload_ctr) {
                blk_stamp[b] = upload_ctr;
                if (blk_writer[b] || blk_cpu[b] != ALL) {
                    /* GPU-written block (or partly): every target copy goes stale */
                    blk_writer[b] = 0;
                    blk_serial[b] = ++serial_ctr;
                } else {
                    blk_serial[b] = ++serial_ctr;          /* targets that loaded it are stale too */
                }
                if (!pending(b)) {
                    blk_pend[b >> 6] |= 1ull << (b & 63);
                    pend_list[npend++] = (uint16_t)b;
                    page_pend[b >> 5]++;
                }
            }
            if (!partial_word) set_cpu(b, blk_cpu[b] | 1ull << (w & 63));
        }
}

void own_flush_patches(void) {
    if (!npend) return;
    gpu_fop_blocks* o = gf_cmd(GPU_FOP_PATCH, sizeof *o);
    o->n = npend;
    o->data_off = gf_data(pend_list, npend * 2);
    uint32_t off = gf_data(NULL, npend * 256);
    uint8_t* d = gf_data_ptr(off);
    for (uint32_t i = 0; i < npend; i++) {
        uint32_t b = pend_list[i];
        memcpy(d + (size_t)i * 256, gs_vram + (size_t)b * 256, 256);
        blk_pend[b >> 6] &= ~(1ull << (b & 63));
        page_pend[b >> 5]--;
    }
    gf.patches++;
    gf.patch_bytes += npend * 256;
    npend = 0;
}

/* ---- frame end ----------------------------------------------------------------------------------- */

static void release(gpu_target* T) {
    /* blocks only this target holds go to vram_buf first */
    for (int i = 0; i < T->bcols * T->brows; i++) {
        uint32_t b = T->babs[i];
        if (blk_writer[b] == T->id) {
            if (blk_cpu[b] != ALL) download_block(b);
            blk_writer[b] = 0;
        }
    }
    flush_lists(NULL);
    emit_target(T, GPU_FOP_RELEASE);
    free(T->babs); free(T->vs); free(T->sdirty);
    T->babs = NULL; T->vs = NULL; T->sdirty = NULL;
    T->used = 0;
    gf.targets_evicted++;
}

void own_frame_end(const uint64_t priv[16]) {
    own_flush_patches();
    /* display family: colour targets shown by an enabled circuit */
    if (priv) {
        uint64_t pmode = priv[0], smode2 = priv[2];
        (void)smode2;
        for (int c = 0; c < 2; c++) {
            if (!((pmode >> c) & 1)) continue;
            uint64_t dispfb = priv[c == 0 ? 7 : 9];
            uint32_t fbp = (uint32_t)(dispfb & 0x1FF), fbw = (uint32_t)((dispfb >> 9) & 0x3F);
            gpu_target* T = own_find(TGT_C32, fbp, fbw);
            if (T && T->family != FAM_DISPLAY) { T->family = FAM_DISPLAY; set_scale(T); emit_target(T, GPU_FOP_TARGET); }
        }
    }
    int live = 0;
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        gpu_target* T = &gt[i];
        if (!T->used) continue;
        if (gf_frame - T->last_used > 120) { release(T); continue; }
        live++;
    }
    gf.targets_live = (uint32_t)live;
}
