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
    int thread;                  /* the frontend on its own thread, the GS thread only decodes (needs sw = 0) */
    int display_ky2;             /* display-family targets get ky = 2N (3.1). M3 (REF at S=1) runs 1x1 samples
                                    everywhere (0); M2's benchmark used 1. M7 turns it on with Z targets that
                                    follow their colour target's factors. */
    int mixed;                   /* M5: frames are executed for real with FF and ROV draws (GPU_FD_MIXED); 0 with
                                    ref = 0: M2's submission benchmark (trivial shaders) */
    int ff_stq_exact;            /* FF triangles carry the full 256-byte record and step S/T/Q exactly as the
                                    software renderer (verification); 0: 128-byte records, S/T/Q interpolated */
    uint32_t ff_rov_mask;        /* GPU_FFC_* classes kept on the ROV tier (moving classes one at a time, tests) */
    int split_ff;                /* the plain two-draw ATST split (M5 measured it visibly different from REF); default 0:
                                    the exact split (a new draw where a pass-capable primitive covers a pixel of an
                                    earlier fail-capable one of the draw) */
} gpu_front_config;

enum { GPU_TIER_FF, GPU_TIER_ROV, GPU_TIER_ALIAS16, GPU_TIER_PUNT, GPU_TIER_N };
enum { GPU_PC_TRI, GPU_PC_SPRITE, GPU_PC_LINEPIX, GPU_PC_POINT, GPU_PC_N };
enum { GPU_ZF_32, GPU_ZF_24, GPU_ZF_16, GPU_ZF_16S, GPU_ZF_N };
/* FF classes (gpu_front_config.ff_rov_mask): blend class, ATST split, texture views (incl. feedback through
 * a shadow copy), primitive classes */
enum { GPU_FFC_OFF = 1 << 0, GPU_FFC_NOCOLOR = 1 << 1, GPU_FFC_ADD = 1 << 2, GPU_FFC_REVSUB = 1 << 3,
       GPU_FFC_DUAL = 1 << 4, GPU_FFC_SPLIT = 1 << 5, GPU_FFC_VIEW = 1 << 6, GPU_FFC_LINE = 1 << 7,
       GPU_FFC_SPRITE = 1 << 8, GPU_FFC_TRI = 1 << 9, GPU_FFC_DEPTH = 1 << 10, GPU_FFC_ALL = (1 << 11) - 1 };

/* why a primitive went to the ROV tier (first matching rule of 4.1) */
enum { GPU_ROV_REF, GPU_ROV_COLCLAMP, GPU_ROV_FBMSK, GPU_ROV_DATE, GPU_ROV_BLEND, GPU_ROV_C_OVER128,
       GPU_ROV_PABE_DTHE, GPU_ROV_Z32_HIGH, GPU_ROV_STRADDLE, GPU_ROV_CLASS /* (ff_rov_mask) */, GPU_ROV_N };
/* why a primitive was PUNTed (5.4) */
enum { GPU_PUNT_TARGET_OVERLAP, GPU_PUNT_FRAME_FMT, GPU_PUNT_TEX_FOREIGN, GPU_PUNT_TEX_FMT, GPU_PUNT_FORCED,
       GPU_PUNT_READBACK_DIFF /* (not a reason: PUNTs whose read-back differed from gs_vram, both mode) */, GPU_PUNT_N };

/* Counters of one VBlank (one picture). */
typedef struct {
    uint32_t vblank;
    double front_ms;             /* time spent in the frontend hooks */
    double thread_ms;            /* CPU of the thread running the frontend since the previous VBlank (QueryThreadCycleTime;
                                    the frontend thread, or the GS thread with the decode when same-thread) */
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
    uint32_t fill_draws_dropped; /* (M5) FF draws of recognised fills not emitted (the clear does it) */
    uint32_t split_conflicts;    /* (M5) ATST split draws started anew by the exact split */
    uint32_t loads, load_blocks; /* OP_UPLOAD_TARGET (stale blocks loaded from vram_buf) */
    uint32_t xcopies, xcopy_blocks;  /* stale blocks copied from the target that wrote them (same kind and scale) */
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
    uint32_t lost_writer;                /* GPU words whose writer target is gone (never) */
    uint32_t post_prims;                 /* primitives the REF tier does not render yet (ALIAS16, target views, PUNT) */
    uint32_t stq_cuts;                   /* STQ triangles with batch cuts (gpu_rec_tri.cuts) */
    uint32_t checkpoints;                /* partial frames handed to the sink (lockstep) */
    uint32_t display_mem;                /* (M6) displayed circuits composed from vram_buf (not current in a target) */
    uint32_t phase_prims[6];             /* drawn primitives per phase label (GPU_PH_*: scene, fill, ROV read-modify-write,
                                            ALIAS16, target view, feedback) */
} gpu_front_frame;

void gpu_front_start(const gpu_front_config* cfg);   /* installs the gs_draw.c hooks */
void gpu_front_stop(void);                          /* (drains the frontend thread first) */
void gpu_front_drain(void);                         /* frontend thread: wait until it processed everything posted */
uint64_t gpu_front_thread_cycles(void);             /* frontend thread CPU (QueryThreadCycleTime), 0 without */
void gpu_front_thread_affinity(uint64_t mask);       /* benchmarks: pin the frontend thread */
/* VBlank (stream order, before the picture): closes the frame. priv: the display registers
 * (gs_priv_snapshot order) for the display target family. */
void gpu_front_vblank(const uint64_t priv[16], uint32_t vblank);
/* M6: the same with the picture to compose and show on the GPU (the frame's GPU_FOP_PRESENT carries
 * it to the render thread, gpu_exec.c / gpu_present.c). */
typedef struct {
    uint64_t priv[16];                   /* display registers (gs_priv_snapshot order; [15] VBlank count) */
    uint32_t vblank;
    uint8_t field_code;                  /* gs_display_field_code: deinterlacer field (0 / 8), 0xFF none */
    uint8_t content43;                   /* ASPECT_4_3 hint state at this VBlank (pillarboxed at 4:3) */
    uint8_t flags;                       /* GPU_DI_* */
    uint8_t pad;
    uint16_t aspect_w, aspect_h;         /* display aspect the picture is fitted to (4:3 content: 4, 3) */
    uint32_t pad2;
    void* rb;                            /* gpu_readback* of the composed picture (dumps), or NULL */
} gpu_display_info;
enum { GPU_DI_SHOW = 1,                  /* present it (else compose only: tests, read-backs) */
       GPU_DI_SIGNAL = 2 };              /* call the picture-done callback (gpu_set_picture_done) after it */
void gpu_front_vblank_display(const gpu_display_info* di);
/* Run fn(arg) on the frontend's thread in stream order (everything posted before it has been processed)
 * and wait for it; same thread: at once. The GS side must be idle or be the caller. */
void gpu_front_call(void (*fn)(void* arg), void* arg);
/* Move the frontend between its own thread and the caller's (the GS thread); the GS side must be idle
 * (gs_sync). PUNT needs the same thread (5.4). */
void gpu_front_set_thread(int on);
int  gpu_front_threaded(void);
/* PUNTs met on the frontend thread (not drawn: the GS thread had moved on), since start. */
uint32_t gpu_front_thread_punts(void);
/* Completed frames, oldest first; returns how many were taken. */
int  gpu_front_take_frames(gpu_front_frame* out, int max);
/* One line per live target (id, kind, base, FBW, rows, family, last use) to f. */
void gpu_front_dump_targets(FILE* f);
/* the kind of target id: 0 colour (C32), 1 Z32, 2 Z24, 3 Z16, 4 Z16S; -1 none (lockstep tolerance classes) */
int gpu_front_target_kind(uint32_t id);

/* ---- the command stream of a frame (consumed by M2's render thread) ------------------------------ */
enum {
    GPU_FOP_TARGET = 1,          /* gpu_fop_target: create / grow */
    GPU_FOP_RELEASE,             /* gpu_fop_target: evicted */
    GPU_FOP_PATCH,               /* gpu_fop_blocks: vram_buf <- blocks, bits of the uploaded texels only
                                    (data: n x u16 block numbers (padded to 16 B), n x 256 B data, n x 256 B
                                    per-word bit masks) */
    GPU_FOP_CLUT,                /* gpu_fop_clut: clut_ring[slot] <- 1 KB (data) */
    GPU_FOP_LOAD,                /* gpu_fop_blocks: target blocks <- vram_buf (data: n x u16 local block numbers) */
    GPU_FOP_DOWNLOAD,            /* gpu_fop_blocks: vram_buf <- blocks of the writer target `target`, sample 0,
                                    only the words of each entry's mask (data: n x gpu_dl_entry) */
    GPU_FOP_CLEAR,               /* gpu_fop_clear */
    GPU_FOP_COPY_SHADOW,         /* gpu_fop_shadow */
    GPU_FOP_DRAW,                /* gpu_fop_draw */
    GPU_FOP_ROV_END,             /* gpu_fop_rov_end: end of a ROV run (+ zresolve) */
    GPU_FOP_ALIAS16,             /* gpu_fop_alias16: an ALIAS16 run begins: gather the rect of the 16-bit frame
                                    (psm) over C32 target `target` into the staging texture; the run's draws
                                    (tier GPU_TIER_ALIAS16) write the staging texture */
    GPU_FOP_PRESENT,             /* gpu_fop_present */
    GPU_FOP_ALIAS16_END,         /* gpu_fop_alias16: the run ends: scatter the rect back into the target */
    GPU_FOP_XCOPY,               /* gpu_fop_xcopy: target blocks <- blocks of another target of the same kind and scale,
                                    every sample (data: n x (u16 source local block, u16 destination local block)) */
};
typedef struct { uint16_t op, size; } gpu_fop;       /* size in bytes, header included, 4-byte multiple */
typedef struct { gpu_fop h; uint16_t id, kind, base, fbw, rows, kx, ky, family; } gpu_fop_target;
typedef struct { gpu_fop h; uint16_t target, pad; uint32_t n, data_off; } gpu_fop_blocks;
typedef struct { gpu_fop h; uint32_t slot, data_off; } gpu_fop_clut;
/* a recognised full fill: rect [0, x1) x [0, y1) of the target (native pixels); colour: value under
 * the byte write mask `mask` (R1 G2 B4 A8); Z (zclear): the stored Z value (already clamped) */
typedef struct { gpu_fop h; uint16_t target, zclear; uint32_t value, mask; int16_t x1, y1; } gpu_fop_clear;
/* one block of a download: the writer target's local block, the absolute 256-byte block, the 32-bit
 * words of the block to write (the GPU-owned ones) */
typedef struct { uint16_t local, abs; uint32_t pad; uint64_t words; } gpu_dl_entry;
typedef struct { gpu_fop h; uint16_t target, pad; int16_t x0, y0, x1, y1; } gpu_fop_shadow;
typedef struct {
    gpu_fop h;
    uint8_t tier, cls, blend, depth;     /* GPU_TIER_*, GPU_PC_*, blend-state class, depth-state class (ztst * 2 + zwrite) */
    uint32_t ps_key;                     /* permutation (4.5) */
    uint16_t ct, zt, tex;                /* target ids (0: none), texture SRV (0: vram_buf, else target id) */
    uint8_t wmask, split;                /* RenderTargetWriteMask; ATST split (FF): 1 the pass draw, 2 the fail draw,
                                            3 both (the two-draw split of 4.2) */
    int16_t sc[4];                       /* scissor x0, y0, x1, y1 */
    uint32_t first, count;               /* first index / instance, count (indices or instances) */
    uint32_t vtx_first, rec_first;       /* vertex base, first record */
    int16_t bbox[4];                     /* native pixels the primitives touch: x0, y0, x1, y1 (inclusive) */
    uint8_t zw, afail;                   /* some primitive may write Z; AFAIL of an ATST split */
    uint16_t pad;
} gpu_fop_draw;
enum { GPU_PSK_FF_FAST = 1 << 12 };      /* gpu_fop_draw.ps_key: FF triangles with 128-byte records (hardware S/T/Q) */
typedef struct { gpu_fop h; uint16_t ct, zt; uint32_t zresolve; int16_t bbox[4]; } gpu_fop_rov_end;
/* rect [x0, x1) x [y0, y1) in pixels of the 16-bit frame (whole 16x8 blocks) */
typedef struct { gpu_fop h; uint16_t target, psm; int16_t x0, y0, x1, y1; } gpu_fop_alias16;
typedef struct { gpu_fop h; uint16_t target, src; uint32_t n, data_off; } gpu_fop_xcopy;
/* VBlank. With a display info (M6, di_off != ~0: a gpu_display_info in the data arena) the picture is
 * composed on the GPU: circuit i reads colour target src[i] (sample 0 of every native pixel; the
 * displayed rect is current there), or, src[i] == 0, GS memory: a scratch copy of vram_buf with the GPU
 * words of the displayed rects (mem_n download entries at mem_off in the data arena, then their writer
 * target ids as u16) - nothing in vram_buf or the ownership changes. */
typedef struct { gpu_fop h; uint32_t vblank; uint32_t di_off; uint16_t src[2]; uint32_t mem_off, mem_n; } gpu_fop_present;

enum { GPU_FD_PARTIAL = 1, GPU_FD_REF = 2, GPU_FD_MIXED = 4,
       GPU_FD_SIGNAL = 8 };                      /* (M6) the frame's picture (GPU_DI_SIGNAL) is reported once the
                                                    frame is queued for the render thread (gpu_submit_sink) */
typedef struct {
    const uint8_t* cmd; size_t cmd_bytes;          /* gpu_fop records */
    const uint8_t* vtx; size_t vtx_bytes;          /* 32-byte vertices */
    const uint32_t* idx; size_t idx_count;
    const uint8_t* rec; size_t rec_bytes;          /* records (Z 32 B, triangle 256 B, sprite 64 B, line pixel 32 B) */
    const uint8_t* states; size_t state_count;     /* 96-byte StateRecs */
    const uint8_t* data; size_t data_bytes;        /* patches, CLUT snapshots, block lists, batch-cut tables */
    uint32_t vblank;
    uint32_t flags;                                /* GPU_FD_PARTIAL: a checkpoint inside the VBlank (no PRESENT);
                                                      GPU_FD_REF: REF mode (every draw ROV, depth in zpatch);
                                                      GPU_FD_MIXED: FF and ROV draws (M5) */
} gpu_front_frame_data;
/* Called at every VBlank with the frame just closed (before its buffers are reused), and at every
 * checkpoint (gpu_front_checkpoint) with the commands built since. NULL: none. */
void gpu_front_set_sink(void (*sink)(const gpu_front_frame_data* fd, void* user), void* user);

/* ---- checkpoints (lockstep, M3) ----------------------------------------------------------------------
 * Same-thread frontend only. The hook is called from the primitive hook before primitive `index` (the
 * VBlank's kick count) is processed; frame_changed: its FRAME or ZBUF register differs from the previous
 * drawing primitive's. Inside the hook, gpu_front_checkpoint() hands everything built so far to the sink
 * as a partial frame (pending patches flushed), so hw memory can be compared with the software
 * renderer's, which has drawn every earlier primitive. */
void gpu_front_set_checkpoint_hook(void (*hook)(uint32_t index, int frame_changed, const void* ev, int type, const void* const* v, void* user), void* user);
void gpu_front_checkpoint(void);
/* The blocks whose truth is in a target (GPU-written words), as download entries grouped by writer:
 * writers[i] is the writer target of out[i]. Returns the count (<= max). */
int gpu_front_gpu_blocks(gpu_dl_entry* out, uint16_t* writers, int max);
/* All of GS memory becomes CPU data from gs_vram (patched into vram_buf); every target goes stale and
 * reloads what it reads (lockstep resync after M4-only passes). */
void gpu_front_resync(void);
/* Primitives of this VBlank so far that the REF tier cannot render exactly yet (ALIAS16, target views,
 * PUNT): the frame's first "post pass" (M4). */
uint32_t gpu_front_post_prims(void);
/* (debugging) the frontend's ownership record of 256-byte block b and every target holding it */
void gpu_front_debug_block(uint32_t b, FILE* f);
/* PUNT (5.4, same-thread frontend): the software rasterizer draws the primitive into gs_vram (gs_draw.c,
 * gs_front_swdraw) and the written blocks become CPU data patched into vram_buf. Before that the GPU
 * data it touches is downloaded into vram_buf and read back with this reader (all of vram_buf as the
 * render thread has it after everything submitted; returns 0 on failure) into gs_vram - or, when the
 * software renderer also runs (config sw), compared with gs_vram (counted in punt_why[GPU_PUNT_READBACK_DIFF]).
 * Without a reader nothing is read back. PS2_GPU_PUNT=<every N>[:<class mask: 1 tri, 2 sprite, 4 line,
 * 8 point, 16 only 16-bit frames>] forces PUNTs (tests). */
void gpu_front_set_mem_reader(int (*reader)(uint8_t* out4mb, void* user), void* user);

/* ---- per-draw data read by the shaders (runtime/gpu/shaders/gs.hlsl, byte offsets there) ------------ */
enum {                                          /* gpu_staterec.flags */
    GPU_SF_FBA = 1 << 0, GPU_SF_COLCLAMP = 1 << 1, GPU_SF_TFX_SHIFT = 2, GPU_SF_TCC = 1 << 4, GPU_SF_FGE = 1 << 5,
    GPU_SF_IIP = 1 << 6, GPU_SF_FST = 1 << 7, GPU_SF_ABE = 1 << 8, GPU_SF_AA1 = 1 << 9, GPU_SF_PABE = 1 << 10,
    GPU_SF_DTHE = 1 << 11, GPU_SF_TME = 1 << 12, GPU_SF_FB24 = 1 << 13, GPU_SF_SCANMSK_SHIFT = 14, GPU_SF_CTX = 1 << 16,
    GPU_SF_FB16 = 1 << 17,                      /* 16-bit frame (ALIAS16 run: the colour is the staging texture) */
};
enum { GPU_TS_SELF = 1 << 16 };                 /* gpu_staterec.texsrc: the view is the bound colour target (byte lanes
                                                   the draw does not write: read in place, no copy) */
/* phase labels (diagnostic, gpu_staterec.phase and traces): what a draw is, from its own state */
enum { GPU_PH_SCENE, GPU_PH_FILL, GPU_PH_ROV_RMW, GPU_PH_ALIAS16, GPU_PH_VIEW, GPU_PH_FEEDBACK, GPU_PH_N, GPU_PH_HINT = 0x100 };
#define GPU_STATE_BYTES 128
typedef struct {                                /* StateRec, 128 bytes */
    uint64_t tex0, clamp, tex1;                 /* TEX0 (CLD cleared), CLAMP, TEX1 of the context */
    uint32_t texa;                              /* TA0 | AEM << 15 | TA1 << 16 */
    uint32_t clut_slot;                         /* CLUT ring slot (indexed formats), else ~0 */
    uint32_t texsrc;                            /* texture kind (gpu_front.c TK_*) | view target id << 8 */
    uint32_t fogcol;
    uint32_t test;                              /* as gs_prim_finalize leaves it: ate | atst << 1 | aref << 4 |
                                                   afail << 12 | date << 14 | datm << 15 */
    uint32_t fmask;                             /* frame write mask, stored 32-bit form (1 = keep; CT24: FF000000 set) */
    uint32_t flags;                             /* GPU_SF_* */
    uint32_t alpha;                             /* asel | bsel << 2 | csel << 4 | dsel << 6 | fix << 8 */
    uint32_t zinfo;                             /* ztst | zmsk << 2 | zuse << 3 | Z bits (0 none, 1 16, 2 24, 3 32) << 4 */
    uint32_t tlev;                              /* texm | cur_level << 2 | mxl << 5 | mmag << 8 | mmin << 9 | lcm << 12 |
                                                   lshift << 13 | (K & 0xFFF) << 16 */
    uint32_t mip[6];                            /* mipmap levels 1..6: TBP | TBW << 14 */
    uint32_t xyoffset, pad;
    uint64_t dimx;                              /* DIMX (16-bit frames with DTHE) */
    uint32_t phase;                             /* GPU_PH_* (diagnostic; | GPU_PH_HINT + label << 16 from a PHASE hint) */
    uint32_t phase_label;                       /* the PHASE hint's label (diagnostic) */
    uint32_t hud, hud_scale;                    /* (M6) HUD hint of its primitives: GS_ENV_HUD_ON | anchor << 1, X scale
                                                   (float bits); the decoder applies it today, the VS at scale in M10 */
    uint32_t pad2[2];
} gpu_staterec;
typedef struct {                                /* ROV triangle, 256 bytes (SV_PrimitiveID) */
    int64_t zrow, zdy, zdx;                     /* Z 16.16 at x = 0 of row by0, per-row and per-pixel steps */
    int32_t row[7], dy[7], dx[7];               /* R G B A F U V: low 32 bits of the same (wrapping) */
    int16_t by0, bx0;                           /* first row, first column of the scissored bounding box */
    uint32_t state;
    float lod_q;                                /* Q of the LOD of FST triangles (v2) */
    double s0, t0, q0, sdy, tdy, qdy, sdx, tdx, qdx;   /* S/T/Q at x = 0 of row by0 and steps (gs_rowstep) */
    int32_t ea[2], edc[2];                      /* left edges (a > 0): inside where ea * x + ec >= 0 */
    int64_t ec[2];                              /* at row by0; advances by edc per row */
    uint32_t nedge;
    uint32_t cuts, ncuts;                       /* STQ batch cuts: byte offset in the frame's data, count of
                                                   (int16 y, int16 x) entries (0: none) */
    uint32_t pad[5];
} gpu_rec_tri;
typedef struct {                                /* sprite, 64 bytes (SV_InstanceID) */
    int16_t x0, y0, x1, y1;                     /* native pixels [x0, x1) x [y0, y1) */
    uint32_t rgba, z, fog, state;               /* flat colour (v1), stored Z (clamped), fog */
    int32_t ua, du;                             /* U (1/16 texel << 12) at x0, per pixel */
    double vc0, vcy;                            /* V of row py: trunc((vc0 + vcy * py) * 4096) (gs_span_start_at) */
    float lod_q;
    int32_t va, dv;                             /* V at y0 and per row */
    uint32_t flags;                             /* 1: steps one texel per pixel and per row (1:1 rule, 5.1) */
} gpu_rec_sprite;
typedef struct {                                /* line / point pixel, 32 bytes (SV_InstanceID) */
    int16_t px, py;
    uint32_t z, rgba, fog_state;                /* stored Z, colour (AA1: coverage alpha), fog | state << 8 */
    uint32_t s, t, q;                           /* STQ (float bits); FST: U, V (int32, 1/16 texel << 12) in s, t */
    float lod_q;
} gpu_rec_linepix;
/* Inside the sink: take ownership of the frame's buffers (fd's pointers stay valid until
 * gpu_front_release(handle), which any thread may call); the frontend continues with other buffers.
 * Returns the handle (NULL outside a sink call). */
void* gpu_front_detach(void);
void  gpu_front_release(void* handle);
