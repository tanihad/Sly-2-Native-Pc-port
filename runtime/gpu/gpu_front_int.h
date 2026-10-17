/* Internals shared by gpu_front.c (classification, emission), gpu_own.c (targets, ownership, block
 * validity, uploads) and gpu_clut.c (CLUT ring). GS thread only. */
#pragma once
#include "gpu_front.h"
#include "gs_setup.h"

/* equality of n 64-bit words (memcmp of the C library compares bytes) */
static inline int gf_eq64(const void* a, const void* b, size_t n) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    uint64_t d = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t u, v;
        memcpy(&u, x + 8 * i, 8);
        memcpy(&v, y + 8 * i, 8);
        d |= u ^ v;
    }
    return d == 0;
}

/* ---- frame output: counters and arenas (gpu_front.c) ------------------------------------------- */
extern gpu_front_frame gf;               /* counters of the frame being built */
extern gpu_front_config gf_cfg;
extern uint32_t gf_frame;                /* frame (VBlank) number since start */
extern int gf_trace;                     /* PS2_GPU_FRONT_TRACE: this frame is logged to stderr */

void* gf_cmd(uint16_t op, uint32_t bytes);          /* append a command (zeroed), returns it */
uint32_t gf_data(const void* src, uint32_t bytes);  /* append to the data arena (src NULL: zeroed), returns offset */
void* gf_data_ptr(uint32_t off);
void gf_target_op(uint16_t id);                     /* an op on target id follows: an ALIAS16 run on it ends first */

/* ---- targets and ownership (gpu_own.c) ---------------------------------------------------------- */
enum { TGT_C32, TGT_Z32, TGT_Z24, TGT_Z16, TGT_Z16S, TGT_N };
enum { FAM_SQUARE, FAM_DISPLAY };
#define GPU_MAX_TARGETS 64
#define GPU_BLOCKS 16384                 /* 256-byte GS blocks in 4 MB */

typedef struct gpu_target {
    int used;
    uint16_t id;                         /* 1..GPU_MAX_TARGETS */
    uint16_t base, fbw;                  /* exact base page and width (64-pixel units) */
    uint8_t kind, family, kx, ky;
    int psm;                             /* layout (PSMCT32 for C32, PSMZ* for depth) */
    const gs_fmt* fmt;
    int rows;                            /* native rows (multiple of the page height) */
    int bw, bh;                          /* block size in pixels (8x8 for 32-bit, 16x8 for 16-bit) */
    int bcols, brows;
    uint16_t* babs;                      /* local block -> absolute 256-byte block */
    uint32_t* vs;                        /* per local block: blk_serial value it is current for */
    int16_t* inv;                        /* absolute block -> local block (built on demand) */
    uint64_t pages[8];
    uint32_t last_used;                  /* gf_frame of the last draw or read */
    uint8_t* sdirty;                     /* per local block: written since the shadow copy */
    int shadow_valid;                    /* the shadow copy exists (feedback, 5.1) */
    int copy_rect[4];                    /* own_shadow_need full == 2: the rect to copy */
    int clean, slow_calls;               /* every block current and only T's (as of clean_gen) */
    uint32_t clean_gen;
    int z_high;                          /* Z32 above 0x3F000000 seen: zpatch as depth until a full clear */
} gpu_target;

extern gpu_target gt[GPU_MAX_TARGETS + 1];          /* [id] */
extern uint32_t own_gen;                 /* bumped when a block stops being CPU-valid (texture source checks) */
extern uint32_t own_upload_gen;          /* bumped by every upload */
extern uint32_t own_epoch;               /* bumped when a target is released (cached target pointers) */

void own_reset(void);
/* The target of exactly (kind, base, fbw), created (or grown to `rows` native rows) if needed.
 * NULL: a target of the same kind overlaps at another base/FBW (PUNT). */
gpu_target* own_target(int kind, int psm, uint32_t base, uint32_t fbw, int rows);
/* The live colour target with this base and width (ALIAS16 views, texture views), or NULL. */
gpu_target* own_find(int kind, uint32_t base, uint32_t fbw);
/* A draw touching native rect [x0,x1] x [y0,y1] (inclusive) of T: stale blocks are loaded first
 * unless the draw overwrites them whole (opaque: every pixel of the rect is written regardless of
 * the destination); writes: the blocks become T's. */
void own_draw(gpu_target* T, int x0, int y0, int x1, int y1, int writes, int opaque);
void own_grow(gpu_target* T, int rows);  /* taller use: recreate + copy */
/* (M7) sample families (3.1): upgrade T to `family` (square -> display only); a pair that must share a sample
 * grid takes the larger family; every live target takes the factors of the current configuration. A factor
 * change emits GPU_FOP_TARGET: the executor rescales the contents (sample 0 kept). */
void own_set_family(gpu_target* T, int family);
void own_pair_family(gpu_target* A, gpu_target* B);
void own_rescale_all(void);
/* Absolute 256-byte blocks of T touched through another view (ALIAS16 CT16 views). */
void own_blocks(gpu_target* T, const uint16_t* blocks, int n, int writes);
/* Every block of T written (a recognised full clear). */
void own_clear(gpu_target* T);
/* A memory-source read of texels [u0,u1] x [v0,v1] (inclusive, inside 0..2047) of a buffer in
 * format psm at bp (blocks), bw: makes sure gs_vram/vram_buf hold them (pending patches flushed,
 * GPU-written data downloaded: counted). Returns 1 when some texel was GPU data (download). */
int  own_mem_read(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1);
/* Pending patches of the rect flushed, nothing downloaded; returns 1 when some texel is GPU data. */
int  own_mem_flush(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1);
/* 1 when any texel of the rect is GPU-written data not in memory (no side effects). */
int  own_mem_is_gpu(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1);
/* A target view read of T's native rect: stale blocks loaded. */
void own_view_read(gpu_target* T, int x0, int y0, int x1, int y1);
/* A draw samples T while writing it (feedback): 1 when the shadow copy must be refreshed for the
 * rect first (full 1: the whole target; 2: T->copy_rect, every block written since the last copy). */
int  own_shadow_need(gpu_target* T, int x0, int y0, int x1, int y1, int* full);
/* Host->local transfer rectangle (in psm units at bp/bw). */
void own_upload(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1);
void own_flush_patches(void);
void own_frame_end(const uint64_t priv[16]);        /* families, eviction, counters */
void own_resync(void);                   /* gpu_front_resync */
int  own_gpu_blocks(gpu_dl_entry* out, uint16_t* writers, int max);   /* gpu_front_gpu_blocks */
/* 1 when a block of the rect holds downloaded GPU data that gs_vram lacks (CLUT loads read gs_vram) */
int  own_mem_vstale(int psm, uint32_t bp, uint32_t bw, int u0, int v0, int u1, int v1);
int  own_target_kind_of_zpsm(int zpsm);
int  own_readback_apply(const uint8_t* mem, int write);   /* PUNT: gs_vram <- downloaded blocks (or compare) */

/* The 256-byte blocks of a transfer rectangle [x0,x1) x [y0,y1) (psm units; the own_upload rule for
 * rectangles past 2048), at most max; any thread (pure geometry). */
int own_rect_blocks(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1, uint16_t* out, int max);

/* ---- frontend thread (gpu_fthread.c, M2) ---------------------------------------------------------
 * With gpu_front_config.thread the GS thread only decodes: proxy hooks copy each primitive (vertices,
 * changed state words, changed registers), transfers (with the written blocks' contents), CLUT
 * changes and VBlanks into a ring, and the frontend runs on its own thread. The frontend then reads
 * GS memory and the CLUT buffer from its own mirrors: */
extern const uint8_t* gf_vram;           /* NULL: gs_vram (same thread) */
extern const uint16_t* gf_clut_src;      /* NULL: gs_clut_buffer() */
extern int gf_clut_hint;                 /* tex0 hook: -1 unknown, 0 the loaded entries did not change, 1 changed */
/* the VBlank on the frontend's thread (di: M6 display info, or NULL) */
void gf_vblank_impl(const uint64_t priv[16], uint32_t vblank, const gpu_display_info* di);
void gf_configure(int scale, int progressive, int thin);   /* (M7) run-time settings, between two frames */
const gs_front_hooks* gf_hooks(void);    /* the frontend's own hooks */
void fthread_start(void);                /* installs the proxy hooks, starts the thread */
void fthread_stop(void);                 /* drains and ends it */
void fthread_vblank(const uint64_t priv[16], uint32_t vblank, const gpu_display_info* di);
void fthread_drain(void);                /* everything posted so far has been processed */
void fthread_call(void (*fn)(void*), void* arg);   /* fn(arg) on the frontend thread, in stream order; waits */
/* (M6) a display rect (inclusive) of a CT32/CT24 buffer is current in colour target T (every block) */
int  own_rect_current(gpu_target* T, int x0, int y0, int x1, int y1);
/* (M6) the GPU words of a rect (inclusive) of a psm buffer as download entries and their writers (at most
 * max; no side effects) */
int  own_rect_gpu(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1, gpu_dl_entry* out, uint16_t* writers, int max);
uint64_t fthread_cycles(void);           /* QueryThreadCycleTime of the frontend thread */
void fthread_affinity(uint64_t mask);

/* ---- CLUT ring (gpu_clut.c) --------------------------------------------------------------------- */
#define GPU_CLUT_SLOTS 4096
void clut_reset(void);
void clut_mark_dirty(void);              /* a TEX0 write may have loaded the CLUT */
void clut_mark_changed(void);            /* a TEX0 write loaded different CLUT content (known) */
/* The ring slot holding the current CLUT buffer (snapshot uploaded on a miss: counted). */
uint32_t clut_current_slot(void);
uint32_t clut_content_id(void);         /* changes whenever the CLUT buffer's content does */
/* min/max alpha over the palette entries an indexed texture (tex0, TEXA) can use. */
void clut_alpha_range(uint32_t slot, uint64_t tex0, uint64_t texa, int* amin, int* amax);
void clut_frame_end(void);
