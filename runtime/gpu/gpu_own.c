/* Targets, ownership and block validity (notes/d3d11-renderer.md 3.3, 3.4, 5.3). GS thread.
 *
 * GS memory is tracked per 256-byte block (16384 blocks), the unit of the GS's own layout (8x8
 * pixels of a 32-bit buffer, 16x8 of a 16-bit one):
 *   blk_cpu[b]     which 32-bit words of the block vram_buf holds the truth for (once the pending
 *                  patches are applied); the other words are GPU data held by the block's writer;
 *   blk_writer[b]  the target that wrote the block last (0: the CPU side, i.e. an upload);
 *   blk_serial[b]  bumped whenever the block's last writer changes.
 * A target's copy of one of its blocks is current iff it recorded the block's present serial
 * (gpu_target.vs). So "the last writer owns the block and every other copy goes stale" needs no
 * reverse mapping, overlapping targets of any kind (Z16S over Z32, the Binocucom RTTs and the photo
 * copy inside the main Z pages) are separate targets, and every cross-copy is counted:
 *   load      target <- vram_buf (stale block read or partially written by a draw);
 *   download  vram_buf <- target, sample 0, the GPU words only (the CPU side or another target needs
 *             GPU-written data);
 *   xcopy     target <- the target of the same kind and scale that wrote the whole block, every sample
 *             (M4: instead of download + load; e.g. the darken reads one row past its frame buffer,
 *             which is the other frame buffer's memory).
 * Uploads are queued as vram_buf patches and flushed in one op when something reads a pending block,
 * or at the end of the frame (FMV: ~1200 macroblocks -> one patch). A patch writes only the bits the
 * uploads wrote (per-word bit masks): gs_vram is the source of uploaded data, not of the rest (the hw
 * backend never draws into it). Words an upload writes whole become CPU data while the block's other
 * GPU words stay its writer's (M4: a CLUT uploaded into Z pages costs no download); a GPU word the
 * upload writes only partly is downloaded first (no writer is ever lost). */
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
static uint32_t pend_mask[GPU_BLOCKS * 64];   /* bits of each word written by the pending uploads */
static uint32_t up_mask[GPU_BLOCKS * 64];     /* bits of each word written by the upload being marked */
static uint8_t up_full[GPU_BLOCKS];            /* words of the block that upload wrote completely */
static uint64_t up_fullw[GPU_BLOCKS];          /* (the same as a word mask) */
static uint16_t up_list[GPU_BLOCKS];           /* blocks it touches */
static uint8_t blk_vstale[GPU_BLOCKS];         /* downloaded GPU data in vram_buf that gs_vram lacks */
static uint16_t blk_origin[GPU_BLOCKS];        /* (M6 verifier) 0x100 + kind of the target whose data a download put there */
static uint8_t page_full[512];           /* blocks of the page with every word CPU-valid */
static uint8_t page_pend[512];           /* blocks of the page with a pending patch */
static uint32_t serial_ctr = 1, upload_ctr;
static uint32_t change_gen;               /* bumped whenever any block's writer, serial or CPU mask changes */
uint32_t own_upload_gen;                  /* bumped by every upload (texture source caches) */
uint32_t own_epoch;
static uint32_t next_serial(void) { change_gen++; return ++serial_ctr; }

/* block lists of the op being built */
static uint16_t load_list[GPU_BLOCKS];
static gpu_dl_entry dl_list[GPU_BLOCKS], dl_tmp[GPU_BLOCKS];
static uint16_t dl_writer[GPU_BLOCKS];
static uint32_t nload, ndl, nxc;

void own_reset(void) {
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        free(gt[i].babs); free(gt[i].vs); free(gt[i].sdirty); free(gt[i].inv);
    }
    memset(gt, 0, sizeof gt);
    memset(blk_writer, 0, sizeof blk_writer);
    for (int i = 0; i < GPU_BLOCKS; i++) { blk_serial[i] = 1; blk_cpu[i] = ALL; }
    memset(blk_stamp, 0, sizeof blk_stamp);
    memset(blk_pend, 0, sizeof blk_pend);
    memset(pend_mask, 0, sizeof pend_mask);
    memset(blk_vstale, 0, sizeof blk_vstale);
    memset(blk_origin, 0, sizeof blk_origin);
    npend = 0;
    nload = ndl = nxc = 0;
    memset(page_full, 32, sizeof page_full);
    memset(page_pend, 0, sizeof page_pend);
    serial_ctr = 1;
    upload_ctr = 0;
    own_gen++;
}

/* T's local block holding absolute block b, or -1 */
static int target_local(gpu_target* T, uint32_t b) {
    if (!T->inv) {                       /* absolute block -> local block (-1: not T's) */
        T->inv = malloc(GPU_BLOCKS * sizeof *T->inv);
        if (!T->inv) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
        memset(T->inv, 0xFF, GPU_BLOCKS * sizeof *T->inv);
        for (int i = T->bcols * T->brows - 1; i >= 0; i--) T->inv[T->babs[i]] = (int16_t)i;
    }
    return T->inv[b & (GPU_BLOCKS - 1)];
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
    change_gen++;
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
    free(T->babs); free(T->vs); free(T->sdirty); free(T->inv);
    T->babs = babs; T->vs = vs; T->sdirty = sd; T->inv = NULL;
    T->shadow_valid = 0;
    T->clean = 0;
    T->bcols = bcols; T->brows = brows; T->rows = rows;
    target_pages(T, rows, T->pages);
}

static void emit_target(const gpu_target* T, uint16_t op) {
    gf_target_op(T->id);
    gpu_fop_target* o = gf_cmd(op, sizeof *o);
    o->id = T->id; o->kind = T->kind; o->base = T->base; o->fbw = T->fbw; o->rows = (uint16_t)T->rows;
    o->kx = T->kx; o->ky = T->ky; o->family = T->family;
}

/* The target's sample factors from its family and the configuration (3.1); 1 when they changed (a live
 * target's contents are then rescaled by the executor: GPU_FOP_TARGET with the new factors, M7). */
static int set_scale(gpu_target* T) {
    int n = gf_cfg.scale > 0 ? gf_cfg.scale : 1;
    if (n > 8) n = 8;
    int kx = n, ky = T->family == FAM_DISPLAY && gf_cfg.display_ky2 ? 2 * n : n;
    /* (M10, 8.3) anamorphic widescreen: the display family's 512 columns are shown A / (4/3) wider, so they get
     * round(N * W) samples each (the picture keeps the horizontal density of 4:3 at N) */
    if (T->family == FAM_DISPLAY && gf_cfg.display_kx > n) kx = gf_cfg.display_kx > 16 ? 16 : gf_cfg.display_kx;
    if (T->kx == kx && T->ky == ky) return 0;
    T->kx = (uint8_t)kx;
    T->ky = (uint8_t)ky;
    T->shadow_valid = 0;                 /* (the executor drops the shadow copy with the old samples) */
    return 1;
}

/* Families only go up (square -> display): a pair of targets that must share a sample grid (a draw's
 * colour and depth targets, a 1:1 read between two targets) ends in the larger family, never oscillates. */
void own_set_family(gpu_target* T, int family) {
    if (!T || !T->used || family <= T->family) return;
    T->family = (uint8_t)family;
    if (set_scale(T)) {
        if (gf_trace) fprintf(stderr, "[front] target %u -> family %d, %ux%u samples\n", T->id, family, T->kx, T->ky);
        emit_target(T, GPU_FOP_TARGET);
        gf.rescales++;
    }
}

void own_pair_family(gpu_target* A, gpu_target* B) {
    if (!A || !B || (A->kx == B->kx && A->ky == B->ky)) return;
    int f = A->family > B->family ? A->family : B->family;
    own_set_family(A, f);
    own_set_family(B, f);
}

/* (M7) the configuration changed (scale, display rows): every live target takes its new factors */
void own_rescale_all(void) {
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        gpu_target* T = &gt[i];
        if (T->used && set_scale(T)) { emit_target(T, GPU_FOP_TARGET); gf.rescales++; }
    }
}

/* DISPFB buffers (base | FBW << 16) shown at a VBlank: a C32 target created there later starts in the
 * display family (a recreated target, or a restart, does not rescale again) */
#define DISP_SEEN 8
static uint32_t disp_seen[DISP_SEEN];
static int ndisp_seen;
static int display_seen(uint32_t base, uint32_t fbw) {
    for (int i = 0; i < ndisp_seen; i++) if (disp_seen[i] == (base | fbw << 16)) return 1;
    return 0;
}
static void display_remember(uint32_t base, uint32_t fbw) {
    if (display_seen(base, fbw)) return;
    if (ndisp_seen == DISP_SEEN) { memmove(disp_seen, disp_seen + 1, (DISP_SEEN - 1) * sizeof *disp_seen); ndisp_seen--; }
    disp_seen[ndisp_seen++] = base | fbw << 16;
}

gpu_target* own_find(int kind, uint32_t base, uint32_t fbw) {
    static int last[TGT_N][2];           /* two most recent hits per kind */
    for (int j = 0; j < 2; j++) {
        gpu_target* T = &gt[last[kind][j]];
        if (last[kind][j] && T->used && T->kind == kind && T->base == base && T->fbw == fbw) return T;
    }
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        gpu_target* T = &gt[i];
        if (T->used && T->kind == kind && T->base == base && T->fbw == fbw) {
            last[kind][1] = last[kind][0];
            last[kind][0] = i;
            return T;
        }
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
    /* A target of the same kind whose pages contain this base (the ep2 photo's copy at 070 inside
     * a field buffer drawn taller than 224 rows): with per-block ownership this is ordinary
     * aliasing, exact and counted (loads/downloads), so it is a separate target, not a PUNT. */
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        gpu_target* U = &gt[i];
        if (U->used && U->kind == kind && (U->pages[base >> 6] >> (base & 63) & 1)) { gf.target_overlaps++; break; }
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
    T->family = kind == TGT_C32 && display_seen(base, fbw) ? FAM_DISPLAY : FAM_SQUARE;
    set_scale(T);
    T->last_used = gf_frame;
    target_layout(T, rows);
    gf.targets_created++;
    emit_target(T, GPU_FOP_TARGET);
    return T;
}

/* ---- loads, downloads ---------------------------------------------------------------------------- */

/* one download op per writer target */
static void flush_downloads(void) {
    while (ndl) {
        uint16_t w = dl_writer[0];
        uint32_t n = 0, k = 0;
        for (uint32_t i = 0; i < ndl; i++) {
            if (dl_writer[i] == w) dl_tmp[n++] = dl_list[i];
            else { dl_list[k] = dl_list[i]; dl_writer[k] = dl_writer[i]; k++; }
        }
        ndl = k;
        gpu_fop_blocks* o = gf_cmd(GPU_FOP_DOWNLOAD, sizeof *o);
        o->target = w;
        o->n = n;
        o->data_off = gf_data(dl_tmp, n * (uint32_t)sizeof *dl_tmp);
        gf.downloads++;
        gf.download_blocks += n;
    }
}

/* cross-target copies into the target being loaded: (source local block, destination local block)
 * pairs, grouped by source target */
static uint16_t xc_list[GPU_BLOCKS][2], xc_src[GPU_BLOCKS];

static void flush_xcopies(const gpu_target* T) {
    while (nxc) {
        uint16_t w = xc_src[0];
        uint32_t n = 0, k = 0;
        static uint16_t tmp[GPU_BLOCKS][2];
        for (uint32_t i = 0; i < nxc; i++) {
            if (xc_src[i] == w) { tmp[n][0] = xc_list[i][0]; tmp[n][1] = xc_list[i][1]; n++; }
            else { xc_list[k][0] = xc_list[i][0]; xc_list[k][1] = xc_list[i][1]; xc_src[k] = xc_src[i]; k++; }
        }
        nxc = k;
        if (gf_trace) fprintf(stderr, "[front] cross copy target %u -> %u: %u blocks\n", w, T->id, n);
        gf_target_op(T->id);
        gf_target_op(w);
        gpu_fop_xcopy* o = gf_cmd(GPU_FOP_XCOPY, sizeof *o);
        o->target = T->id;
        o->src = w;
        o->n = n;
        o->data_off = gf_data(tmp, n * 4);
        gf.xcopies++;
        gf.xcopy_blocks += n;
    }
}

static void flush_lists(const gpu_target* T) {
    if (ndl) flush_downloads();
    if (nxc) flush_xcopies(T);
    if (nload) {
        if (gf_trace) {
            fprintf(stderr, "[front] load target %u: %u blocks:", T->id, nload);
            for (uint32_t i = 0; i < nload && i < 16; i++) fprintf(stderr, " %04X", T->babs[load_list[i]]);
            fprintf(stderr, "\n");
        }
        gf_target_op(T->id);
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

/* GPU data of block b that vram_buf lacks goes there (sample 0 of its writer, the GPU words only) */
static void download_block(uint32_t b) {
    if (gf_trace)
        fprintf(stderr, "[front] download block %04X (page %03X) writer %u cpu %016llX\n", b, b >> 5, blk_writer[b],
                (unsigned long long)blk_cpu[b]);
    uint64_t words = ~blk_cpu[b];
    uint16_t w = blk_writer[b];
    if (words) {
        int local = w && gt[w].used ? target_local(&gt[w], b) : -1;
        if (local >= 0) {
            gpu_dl_entry e = { (uint16_t)local, (uint16_t)b, 0, words };
            dl_list[ndl] = e;
            dl_writer[ndl++] = w;
        } else {
            gf.lost_writer++;            /* (never: uploads over GPU words download first) */
        }
        blk_vstale[b] = 1;               /* vram_buf has data gs_vram lacks */
        if (w && gt[w].used) blk_origin[b] = (uint16_t)(0x100 + gt[w].kind);   /* (M6 verifier: its tolerance class) */
    }
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
                T->sdirty[k] = 1;        /* (the shadow copy predates the load: M4 photo finding) */
            }
        }
    }
    if (T->vs[i] == blk_serial[b]) return;
    if (!blk_cpu[b]) {                   /* all of it another target's: copied over directly, every sample (M4) */
        gpu_target* W = blk_writer[b] ? &gt[blk_writer[b]] : NULL;
        int wl = W && W->used && W != T ? target_local(W, b) : -1;
        if (wl >= 0 && W->vs[wl] == blk_serial[b] && W->kind == T->kind && W->kx == T->kx && W->ky == T->ky) {
            xc_list[nxc][0] = (uint16_t)wl;
            xc_list[nxc][1] = (uint16_t)i;
            xc_src[nxc++] = W->id;
            T->vs[i] = blk_serial[b];
            T->sdirty[i] = 1;
            return;
        }
    }
    if (blk_cpu[b] != ALL) download_block(b);    /* another target's (or a lost writer's) data */
    load_list[nload++] = (uint16_t)i;
    T->vs[i] = blk_serial[b];
    T->sdirty[i] = 1;
}

void own_grow(gpu_target* T, int rows) {
    int ph = 1 << T->fmt->ph_shift;
    rows = (rows + ph - 1) / ph * ph;
    if (rows > 2048) rows = 2048;
    if (rows <= T->rows) return;
    target_layout(T, rows);              /* recreate + copy */
    emit_target(T, GPU_FOP_TARGET);
}

/* T holds every one of its blocks, current and GPU-only: nothing to check until some block changes */
static void scan_clean(gpu_target* T) {
    int n = T->bcols * T->brows, clean = 1;
    for (int i = 0; i < n && clean; i++) {
        uint32_t b = T->babs[i];
        clean = T->vs[i] == blk_serial[b] && blk_writer[b] == T->id && !blk_cpu[b];
    }
    T->clean = clean;
    T->clean_gen = change_gen;
    T->slow_calls = 0;
}

void own_draw(gpu_target* T, int x0, int y0, int x1, int y1, int writes, int opaque) {
    if (y1 >= T->rows) own_grow(T, y1 + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int bx0 = x0 / T->bw, by0 = y0 / T->bh, bx1 = x1 / T->bw, by1 = y1 / T->bh;
    if (bx1 >= T->bcols) bx1 = T->bcols - 1;
    if (by1 >= T->brows) by1 = T->brows - 1;
    T->last_used = gf_frame;
    if (T->clean && T->clean_gen == change_gen) {     /* fast path: only the feedback bookkeeping */
        if (writes && T->shadow_valid)
            for (int by = by0; by <= by1; by++) memset(T->sdirty + by * T->bcols + bx0, 1, (size_t)(bx1 - bx0 + 1));
        return;
    }
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
                    blk_serial[b] = next_serial();
                    set_cpu(b, 0);
                } else if (blk_cpu[b]) {
                    /* the writer writes again after a download made the block readable: targets that
                     * loaded it since hold the serial too and must go stale (M3 fuzz finding) */
                    set_cpu(b, 0);
                    blk_serial[b] = next_serial();
                }
                T->vs[i] = blk_serial[b];
                T->sdirty[i] = 1;
            }
        }
    }
    if (nload || ndl || nxc) flush_lists(T);
    if (T->clean_gen != change_gen && ++T->slow_calls >= 16) scan_clean(T);
}

void own_blocks(gpu_target* T, const uint16_t* blocks, int n, int writes) {
    for (int k = 0; k < n; k++) {
        int i = target_local(T, blocks[k]);
        if (i < 0) continue;
        int bx = i % T->bcols, by = i / T->bcols;
        own_draw(T, bx * T->bw, by * T->bh, bx * T->bw + T->bw - 1, by * T->bh + T->bh - 1, writes, 0);
    }
}

void own_clear(gpu_target* T) { own_draw(T, 0, 0, T->bcols * T->bw - 1, T->rows - 1, 1, 1); }

int own_rect_gpu(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1, gpu_dl_entry* out, uint16_t* writers, int max) {
    static uint16_t bl[GPU_BLOCKS];
    int nb = own_rect_blocks(psm, bp, bw, x0, y0, x1 + 1, y1 + 1, bl, GPU_BLOCKS), n = 0;
    for (int i = 0; i < nb && n < max; i++) {
        uint32_t b = bl[i];
        uint16_t w = blk_writer[b];
        if (blk_cpu[b] == ALL || !w || !gt[w].used) continue;
        int local = target_local(&gt[w], b);
        if (local < 0) continue;
        int dup = 0;                     /* (a rect wider than its buffer wraps onto blocks listed already) */
        for (int k = n - 1; k >= 0 && k >= n - 64 && !dup; k--) dup = out[k].abs == b;
        if (dup) continue;
        gpu_dl_entry e = { (uint16_t)local, (uint16_t)b, 0, ~blk_cpu[b] };
        out[n] = e;
        writers[n++] = w;
    }
    return n;
}

/* (M6 verifier) the target that drew a block whose data was downloaded into vram_buf (now CPU data that
 * gs_vram lacks), or 0 */
int gpu_front_block_origin(uint32_t b) {
    b &= GPU_BLOCKS - 1;
    return blk_vstale[b] && blk_origin[b] ? blk_origin[b] : 0;
}

int own_rect_current(gpu_target* T, int x0, int y0, int x1, int y1) {
    if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 || x1 >= T->bcols * T->bw || y1 >= T->brows * T->bh) return 0;
    for (int by = y0 / T->bh; by <= y1 / T->bh; by++)
        for (int bx = x0 / T->bw; bx <= x1 / T->bw; bx++) {
            int i = by * T->bcols + bx;
            if (T->vs[i] != blk_serial[T->babs[i]]) return 0;
        }
    return 1;
}

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

/* the GS block (256 bytes) of each format, in texels */
static void block_dims(int psm, int* w, int* h) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *w = 16; *h = 8; break;
    case PSMT8: *w = 16; *h = 16; break;
    case PSMT4: *w = 32; *h = 16; break;
    default: *w = 8; *h = 8; break;
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
    /* per block of the texture's own block grid (the base is block-aligned); texel by texel only
     * inside blocks that are partly CPU data (an upload over part of a GPU-written block) */
    int gpu = 0, bwp, bhp;
    block_dims(psm, &bwp, &bhp);
    for (int by = v0 / bhp; by <= v1 / bhp; by++)
        for (int bx = u0 / bwp; bx <= u1 / bwp; bx++) {
            uint32_t b = word_of(psm, bp, bw, (uint32_t)(bx * bwp), (uint32_t)(by * bhp)) >> 6;
            if (act && pending(b)) own_flush_patches();
            if (blk_cpu[b] == ALL) continue;
            int ua = bx * bwp > u0 ? bx * bwp : u0, ub = bx * bwp + bwp - 1 < u1 ? bx * bwp + bwp - 1 : u1;
            int va = by * bhp > v0 ? by * bhp : v0, vb = by * bhp + bhp - 1 < v1 ? by * bhp + bhp - 1 : v1;
            for (int v = va; v <= vb; v++)
                for (int u = ua; u <= ub; u++) {
                    uint32_t w = word_of(psm, bp, bw, (uint32_t)u, (uint32_t)v);
                    if (blk_cpu[w >> 6] >> (w & 63) & 1) continue;
                    gpu = 1;
                    if (!act) return 1;
                    if (act == 2) { v = vb; break; }     /* flush only: the caller decides */
                    download_block(w >> 6);
                    v = vb; break;
                }
        }
    if (act && ndl) flush_lists(NULL);
    return gpu;
}

int own_mem_read(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1) {
    return mem_check(psm, bp, bw, u0, v0, u1, v1, 1);
}

int own_mem_flush(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1) {
    return mem_check(psm, bp, bw, u0, v0, u1, v1, 2);
}

int own_mem_is_gpu(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1) {
    return mem_check(psm, bp, bw, u0, v0, u1, v1, 0);
}

int own_rect_blocks(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1, uint16_t* out, int max) {
    if (x1 > 2048) { x0 = 0; x1 = 2048; }
    if (y1 > 2048) { y0 = 0; y1 = 2048; }
    if (gs_psm_bpp(psm) == 0) psm = PSMCT32;
    if (x0 >= x1 || y0 >= y1) return 0;
    int bwp, bhp, n = 0;
    block_dims(psm, &bwp, &bhp);
    /* the buffer's own block grid (its base is block-aligned): one block per grid cell */
    for (int by = y0 / bhp; by <= (y1 - 1) / bhp && n < max; by++)
        for (int bx = x0 / bwp; bx <= (x1 - 1) / bwp && n < max; bx++)
            out[n++] = (uint16_t)(word_of(psm, bp, bw, (uint32_t)(bx * bwp), (uint32_t)(by * bhp)) >> 6);
    return n;
}

/* ---- uploads -------------------------------------------------------------------------------------- */

/* the 32-bit word texel (x, y) (0..2047) of a psm buffer lives in, and its bits there */
static uint32_t texel_word(int psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t* mask) {
    uint32_t a;
    switch (psm) {
    case PSMCT16: a = gs_addr16(gs_swz_ct16, bp, bw, x, y); *mask = 0xFFFFu << ((a & 1) * 16); return a >> 1;
    case PSMCT16S: a = gs_addr16(gs_swz_ct16s, bp, bw, x, y); *mask = 0xFFFFu << ((a & 1) * 16); return a >> 1;
    case PSMZ16: a = gs_addr16(gs_swz_z16, bp, bw, x, y); *mask = 0xFFFFu << ((a & 1) * 16); return a >> 1;
    case PSMZ16S: a = gs_addr16(gs_swz_z16s, bp, bw, x, y); *mask = 0xFFFFu << ((a & 1) * 16); return a >> 1;
    case PSMT8: a = gs_addr8(bp, bw, x, y); *mask = 0xFFu << ((a & 3) * 8); return a >> 2;
    case PSMT4: a = gs_addr4(bp, bw, x, y); *mask = 0xFu << ((a & 7) * 4); return a >> 3;
    case PSMZ32: *mask = 0xFFFFFFFFu; return gs_addr32(gs_swz_z32, bp, bw, x, y);
    case PSMZ24: *mask = 0x00FFFFFFu; return gs_addr32(gs_swz_z32, bp, bw, x, y);
    case PSMCT24: *mask = 0x00FFFFFFu; return gs_addr32(gs_swz_ct32, bp, bw, x, y);
    case PSMT8H: *mask = 0xFF000000u; return gs_addr32(gs_swz_ct32, bp, bw, x, y);
    case PSMT4HL: *mask = 0x0F000000u; return gs_addr32(gs_swz_ct32, bp, bw, x, y);
    case PSMT4HH: *mask = 0xF0000000u; return gs_addr32(gs_swz_ct32, bp, bw, x, y);
    default: *mask = 0xFFFFFFFFu; return gs_addr32(gs_swz_ct32, bp, bw, x, y);
    }
}

static void pend_add(uint32_t b) {
    if (pending(b)) return;
    blk_pend[b >> 6] |= 1ull << (b & 63);
    pend_list[npend++] = (uint16_t)b;
    page_pend[b >> 5]++;
}

void own_upload(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1) {
    if (x1 > 2048) { x0 = 0; x1 = 2048; }
    if (y1 > 2048) { y0 = 0; y1 = 2048; }
    if (gs_psm_bpp(psm) == 0) psm = PSMCT32;
    upload_ctr++;
    own_upload_gen++;
    /* the bits of every word this upload writes (PSMCT24/Z24: the low 3 bytes, T8H/T4HL/T4HH: part
     * of the top byte, 16/8/4-bit formats: their half, byte, nibble) */
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t m, w = texel_word(psm, bp, bw, (uint32_t)x, (uint32_t)y, &m), b = w >> 6;
            if (blk_stamp[b] != upload_ctr) {
                blk_stamp[b] = upload_ctr;
                up_full[b] = 0;
                memset(up_mask + (size_t)b * 64, 0, 256);
                up_list[n++] = (uint16_t)b;
            }
            uint32_t o = up_mask[w];
            if (o != 0xFFFFFFFFu && (o | m) == 0xFFFFFFFFu) up_full[b]++;
            up_mask[w] = o | m;
        }
    /* Words the upload writes whole become CPU data; GPU words it does not touch stay GPU data of the
     * block's writer (an upload of a CLUT into Z pages: the rest of the block is still the Z buffer's,
     * M4: no download). Only a GPU word the upload writes partly (T8/T4/16-bit texels, CT24) goes to
     * vram_buf first (download), so the patch adds the uploaded bits to the right word. */
    for (int i = 0; i < n; i++) {
        uint32_t b = up_list[i];
        const uint32_t* um = up_mask + (size_t)b * 64;
        uint64_t full = 0, touched = 0;
        for (int k = 0; k < 64; k++) {
            if (um[k]) touched |= 1ull << k;
            if (um[k] == 0xFFFFFFFFu) full |= 1ull << k;
        }
        up_fullw[b] = full;
        if (touched & ~full & ~blk_cpu[b]) download_block(b);
    }
    if (ndl) flush_downloads();
    for (int i = 0; i < n; i++) {
        uint32_t b = up_list[i];
        uint64_t cpu = blk_cpu[b] | up_fullw[b];
        if (gf_trace) fprintf(stderr, "[front] upload block %04X: CPU words %016llX -> %016llX writer %u\n", b,
                              (unsigned long long)blk_cpu[b], (unsigned long long)cpu, blk_writer[b]);
        if (cpu == ALL) blk_writer[b] = 0;
        blk_serial[b] = next_serial();   /* every target copy (the writer's too: some words changed) goes stale */
        set_cpu(b, cpu);
        if (up_full[b] == 64) blk_vstale[b] = 0;
        pend_add(b);
        uint32_t* pm = pend_mask + (size_t)b * 64;
        const uint32_t* um = up_mask + (size_t)b * 64;
        for (int k = 0; k < 64; k++) pm[k] |= um[k];
    }
}

void own_flush_patches(void) {
    if (!npend) return;
    gpu_fop_blocks* o = gf_cmd(GPU_FOP_PATCH, sizeof *o);
    o->n = npend;
    o->data_off = gf_data(pend_list, npend * 2);
    uint32_t off = gf_data(NULL, npend * 512);
    uint8_t* d = gf_data_ptr(off);
    const uint8_t* src = gf_vram ? gf_vram : gs_vram;
    for (uint32_t i = 0; i < npend; i++) {
        uint32_t b = pend_list[i];
        memcpy(d + (size_t)i * 256, src + (size_t)b * 256, 256);
        memcpy(d + (size_t)npend * 256 + (size_t)i * 256, pend_mask + (size_t)b * 64, 256);
        memset(pend_mask + (size_t)b * 64, 0, 256);
        blk_pend[b >> 6] &= ~(1ull << (b & 63));
        page_pend[b >> 5]--;
    }
    gf.patches++;
    gf.patch_bytes += npend * 256;
    npend = 0;
}

/* lockstep resync: gs_vram is the truth everywhere (patched whole), every target copy is stale */
void own_resync(void) {
    for (uint32_t b = 0; b < GPU_BLOCKS; b++) {
        blk_writer[b] = 0;
        blk_serial[b] = next_serial();
        set_cpu(b, ALL);
        blk_vstale[b] = 0;
        pend_add(b);
        memset(pend_mask + (size_t)b * 64, 0xFF, 256);
    }
    own_gen++;
    own_upload_gen++;
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) gt[i].clean = 0;
}

int own_gpu_blocks(gpu_dl_entry* out, uint16_t* writers, int max) {
    int n = 0;
    for (int w = 1; w <= GPU_MAX_TARGETS; w++) {
        gpu_target* T = &gt[w];
        if (!T->used) continue;
        for (int i = 0; i < T->bcols * T->brows && n < max; i++) {
            uint32_t b = T->babs[i];
            if (blk_writer[b] != w || blk_cpu[b] == ALL || target_local(T, b) != i) continue;
            gpu_dl_entry e = { (uint16_t)i, (uint16_t)b, 0, ~blk_cpu[b] };
            out[n] = e;
            writers[n++] = (uint16_t)w;
        }
    }
    return n;
}

/* (debugging) what the frontend knows about block b */
void gpu_front_debug_block(uint32_t b, FILE* f) {
    b &= GPU_BLOCKS - 1;
    static const char* kinds[TGT_N] = { "C32", "Z32", "Z24", "Z16", "Z16S" };
    fprintf(f, "    block %04X: writer %u serial %u CPU words %016llX%s%s\n", b, blk_writer[b], blk_serial[b],
            (unsigned long long)blk_cpu[b], pending(b) ? " (patch pending)" : "", blk_vstale[b] ? " (vstale)" : "");
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        gpu_target* T = &gt[i];
        if (!T->used) continue;
        int k = target_local(T, b);
        if (k < 0) continue;
        fprintf(f, "      target %d %s base %03X FBW %u: local block %d (x %d y %d), %s\n", i, kinds[T->kind], T->base, T->fbw, k,
                (k % T->bcols) * T->bw, (k / T->bcols) * T->bh, T->vs[k] == blk_serial[b] ? "current" : "stale");
    }
}

/* PUNT read-back (5.4): mem is vram_buf as read back after the downloads. write: gs_vram takes every
 * block whose downloaded GPU data it lacks; else (the software renderer keeps gs_vram itself, "both"
 * mode) those blocks are compared. Returns the number of differing blocks. */
int own_readback_apply(const uint8_t* mem, int write) {
    int diff = 0;
    for (uint32_t b = 0; b < GPU_BLOCKS; b++) {
        if (!blk_vstale[b] || blk_cpu[b] != ALL || pending(b)) continue;
        if (write) {
            memcpy(gs_vram + (size_t)b * 256, mem + (size_t)b * 256, 256);
            blk_vstale[b] = 0;
        } else if (memcmp(gs_vram + (size_t)b * 256, mem + (size_t)b * 256, 256)) {
            diff++;
            if (gf_trace) {
                const uint32_t *x = (const uint32_t*)(mem + (size_t)b * 256), *y = (const uint32_t*)(gs_vram + (size_t)b * 256);
                int k = 0;
                while (k < 63 && x[k] == y[k]) k++;
                fprintf(stderr, "[front] PUNT read-back differs: block %04X word %d: vram_buf %08X gs_vram %08X (writer %u)\n", b, k, x[k],
                        y[k], blk_writer[b]);
            }
        }
    }
    return diff;
}

/* (M9) bumped whenever any block's writer, serial or CPU mask changes */
uint32_t own_change_gen(void) { return change_gen; }

/* (M9) a hash of the serials of the blocks of the rect [x0, x1] x [y0, y1] of a psm buffer, or 0 when one of them holds
 * GPU-written words or downloaded data gs_vram lacks (gs_vram is not the truth there): the texel contents seen in
 * gs_vram are unchanged while the hash is */
uint64_t own_rect_serials(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1) {
    static uint16_t bl[GPU_BLOCKS];
    int n = own_rect_blocks(psm, bp, bw, x0, y0, x1 + 1, y1 + 1, bl, GPU_BLOCKS);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < n; i++) {
        uint32_t b = bl[i];
        if (blk_cpu[b] != ALL || blk_vstale[b]) return 0;
        h = (h ^ blk_serial[b] ^ (uint64_t)b << 32) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    return h ? h : 1;
}

/* some block of the rect holds downloaded data gs_vram lacks (CLUT loads read gs_vram) */
int own_mem_vstale(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1) {
    for (int v = v0; v <= v1; v++)
        for (int u = u0; u <= u1; u++) {
            uint32_t m, w = texel_word(psm, bp, bw, (uint32_t)u & 2047u, (uint32_t)v & 2047u, &m);
            if (blk_vstale[w >> 6]) return 1;
        }
    return 0;
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
    free(T->babs); free(T->vs); free(T->sdirty); free(T->inv);
    T->babs = NULL; T->vs = NULL; T->sdirty = NULL; T->inv = NULL;
    T->used = 0;
    own_epoch++;
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
            int psm = (int)((dispfb >> 15) & 0x1F);
            if (!fbw || (psm != PSMCT32 && psm != PSMCT24)) continue;
            display_remember(fbp, fbw);
            own_set_family(own_find(TGT_C32, fbp, fbw), FAM_DISPLAY);
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
