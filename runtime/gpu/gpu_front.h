/* GPU backend frontend on the GS thread (notes/d3d11-renderer.md 2.1-5): classification of every
 * GS primitive into a tier (FF / ROV / ALIAS16 / PUNT), StateRec dedup, vertex/index/record
 * emission, the target table, page/block ownership and the CLUT ring. It consumes the stream
 * gs_draw.c decodes through the hooks in gs_setup.h.
 *
 * Milestone M1 runs it as a dry run (gs_replay -Backend hwdry): it builds the per-frame command
 * stream and data (gpu_front_frame_data) without a GPU and counts what the D3D11 backend would do.
 * M2 executes that stream on the render thread. */
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int ref;                     /* REF mode: every draw on the ROV tier */
    int straddle_rov;            /* pass-A straddle escalation: straddling ATST draws on the ROV tier */
    int scale;                   /* N (target sample factors; 0 = 1) */
    int sw;                      /* also run the software rasterizer (hwdry default 1) */
    int selfcheck;               /* recompute Z records with the sw row stepping and compare (cost) */
} gpu_front_config;

enum { GPU_TIER_FF, GPU_TIER_ROV, GPU_TIER_ALIAS16, GPU_TIER_PUNT, GPU_TIER_N };
enum { GPU_PC_TRI, GPU_PC_SPRITE, GPU_PC_LINEPIX, GPU_PC_POINT, GPU_PC_N };
enum { GPU_ZF_32, GPU_ZF_24, GPU_ZF_16, GPU_ZF_16S, GPU_ZF_N };

/* why a primitive went to the ROV tier (first matching rule of 4.1) */
enum { GPU_ROV_REF, GPU_ROV_COLCLAMP, GPU_ROV_FBMSK, GPU_ROV_DATE, GPU_ROV_BLEND, GPU_ROV_C_OVER128,
       GPU_ROV_PABE_DTHE, GPU_ROV_Z32_HIGH, GPU_ROV_STRADDLE, GPU_ROV_N };
/* why a primitive was PUNTed (5.4) */
enum { GPU_PUNT_TARGET_OVERLAP, GPU_PUNT_FRAME_FMT, GPU_PUNT_TEX_FOREIGN, GPU_PUNT_TEX_FMT, GPU_PUNT_ALIAS16_PRIM,
       GPU_PUNT_ALIAS16_TEX, GPU_PUNT_N };

/* Counters of one VBlank (one picture). */
typedef struct {
    uint32_t vblank;
    double front_ms;             /* time spent in the frontend hooks */
    /* primitives */
    uint32_t kicks;              /* primitives kicked (env valid) */
    uint32_t nowrite, culled, skipped;   /* nothing writable / zero area or outside scissor / ATST or ZTST NEVER drop */
    uint32_t tier[GPU_TIER_N], cls[GPU_PC_N];
    uint32_t linepix;            /* line pixel records (lines + points) */
    uint32_t rov_why[GPU_ROV_N], punt_why[GPU_PUNT_N];
    /* predicted D3D11 work */
    uint32_t d3d_draws;          /* Draw* calls: FF + ROV draws + clears (+ split second draws) */
    uint32_t ff_draws, rov_draws, split_extra, rov_runs, zresolves;
    uint32_t d3d_draws_commute;          /* d3d_draws if disjoint same-key draws were batched (estimate) */
    uint32_t alias16_sprites, alias16_dispatches, alias16_selfcopy;
    uint32_t clears_c, clears_z; /* recognised full-target tiled fills (colour / Z) */
    uint32_t loads, load_blocks; /* OP_UPLOAD_TARGET (stale blocks loaded from vram_buf) */
    uint32_t shadow_copies;      /* feedback copies */
    uint32_t downloads, download_blocks;  /* sample-0 downloads (GPU data the CPU side needs) */
    uint32_t patches, patch_bytes;        /* vram_buf patch ops after batching, bytes */
    uint32_t xfers, xfer_bytes;           /* host->local transfers seen, bytes */
    uint32_t xfer_local, xfer_read;       /* TRXDIR=2 / TRXDIR=1 */
    uint32_t clut_loads, clut_slots, clut_new;   /* CLUT content changes, distinct slots this frame, new ring slots */
    uint32_t clut_gpu_reads;             /* CLUT loads whose 16x16/8x2 block holds GPU-written words (not downloaded) */
    uint32_t targets_live, targets_created, targets_evicted;
    uint32_t target_overlaps;            /* targets created inside another target of the same kind (aliasing) */
    /* state */
    uint32_t state_changes, states;      /* StateRec index changes between primitives, distinct StateRecs */
    /* emitted data */
    uint32_t vertices, indices, records_z, records_full, records_sprite, records_linepix;
    uint64_t vtx_bytes, idx_bytes, rec_bytes, state_bytes, cmd_bytes, data_bytes;
    /* pass A: real alpha test with AFAIL != KEEP */
    uint32_t atst_kicks, atst_pass, atst_fail, atst_straddle;
    uint32_t cd_as_kicks, as_over128;    /* Cd-dependent blends with C = As; of them max As > 128 */
    /* Z per format (primitives using the Z buffer): vertex Z range, Z32 vertices above 0x3F000000 */
    uint32_t zprims[GPU_ZF_N], zmin[GPU_ZF_N], zmax[GPU_ZF_N];
    uint32_t z32_over;
    /* texture sources */
    uint32_t tex_vram, tex_view_c32, tex_view_alpha, tex_alias16;
    uint32_t region_clamp, tbw_npot;     /* REGION_CLAMP/REPEAT sampling; TBW not a power of two (FMV TBW 12) */
    uint32_t selfcheck_bad;              /* Z records differing from the sw row stepping */
} gpu_front_frame;

void gpu_front_start(const gpu_front_config* cfg);   /* installs the gs_draw.c hooks */
void gpu_front_stop(void);
/* VBlank (stream order, before the picture): closes the frame. priv: the display registers
 * (gs_priv_snapshot order) for the display target family. */
void gpu_front_vblank(const uint64_t priv[16], uint32_t vblank);
/* Completed frames, oldest first; returns how many were taken. */
int  gpu_front_take_frames(gpu_front_frame* out, int max);
/* One line per live target (id, kind, base, FBW, rows, family, last use) to f. */
void gpu_front_dump_targets(FILE* f);

/* ---- the command stream of a frame (consumed by M2's render thread) ------------------------------ */
enum {
    GPU_FOP_TARGET = 1,          /* gpu_fop_target: create / grow */
    GPU_FOP_RELEASE,             /* gpu_fop_target: evicted */
    GPU_FOP_PATCH,               /* gpu_fop_blocks: vram_buf <- blocks (data: n x u16 block numbers, n x 256 B) */
    GPU_FOP_CLUT,                /* gpu_fop_clut: clut_ring[slot] <- 1 KB (data) */
    GPU_FOP_LOAD,                /* gpu_fop_blocks: target blocks <- vram_buf (data: n x u16 local block numbers) */
    GPU_FOP_DOWNLOAD,            /* gpu_fop_blocks: vram_buf/gs_vram <- target blocks, sample 0 */
    GPU_FOP_CLEAR,               /* gpu_fop_clear */
    GPU_FOP_COPY_SHADOW,         /* gpu_fop_shadow */
    GPU_FOP_DRAW,                /* gpu_fop_draw */
    GPU_FOP_ROV_END,             /* gpu_fop_rov_end: end of a ROV run (+ zresolve) */
    GPU_FOP_ALIAS16,             /* gpu_fop_alias16 */
    GPU_FOP_PRESENT,             /* gpu_fop_present */
};
typedef struct { uint16_t op, size; } gpu_fop;       /* size in bytes, header included, 4-byte multiple */
typedef struct { gpu_fop h; uint16_t id, kind, base, fbw, rows, kx, ky, family; } gpu_fop_target;
typedef struct { gpu_fop h; uint16_t target, pad; uint32_t n, data_off; } gpu_fop_blocks;
typedef struct { gpu_fop h; uint32_t slot, data_off; } gpu_fop_clut;
typedef struct { gpu_fop h; uint16_t target, zclear; uint32_t value, mask; } gpu_fop_clear;
typedef struct { gpu_fop h; uint16_t target, pad; int16_t x0, y0, x1, y1; } gpu_fop_shadow;
typedef struct {
    gpu_fop h;
    uint8_t tier, cls, blend, depth;     /* GPU_TIER_*, GPU_PC_*, blend-state class, depth-state class */
    uint32_t ps_key;                     /* permutation (4.5) */
    uint16_t ct, zt, tex;                /* target ids (0: none), texture SRV (0: vram_buf, else target id) */
    uint8_t wmask, split;                /* RenderTargetWriteMask; 1: ATST two-draw split */
    int16_t sc[4];                       /* scissor x0, y0, x1, y1 */
    uint32_t first, count;               /* first index / instance, count (indices or instances) */
    uint32_t vtx_first, rec_first;       /* vertex base, first record */
} gpu_fop_draw;
typedef struct { gpu_fop h; uint16_t ct, zt; uint32_t zresolve; int16_t bbox[4]; } gpu_fop_rov_end;
typedef struct { gpu_fop h; uint16_t target, pad; uint32_t first, count; } gpu_fop_alias16;
typedef struct { gpu_fop h; uint32_t vblank; } gpu_fop_present;

typedef struct {
    const uint8_t* cmd; size_t cmd_bytes;          /* gpu_fop records */
    const uint8_t* vtx; size_t vtx_bytes;          /* 32-byte vertices */
    const uint32_t* idx; size_t idx_count;
    const uint8_t* rec; size_t rec_bytes;          /* records (Z 32 B, full 128 B, sprite 48 B, line pixel 32 B) */
    const uint8_t* states; size_t state_count;     /* 96-byte StateRecs */
    const uint8_t* data; size_t data_bytes;        /* patches, CLUT snapshots, block lists */
    uint32_t vblank;
} gpu_front_frame_data;
/* Called at every VBlank with the frame just closed (before its buffers are reused). NULL: none. */
void gpu_front_set_sink(void (*sink)(const gpu_front_frame_data* fd, void* user), void* user);
