/* GPU backend frontend (notes/d3d11-renderer.md 2.1, 3.5, 4, 5): per GS primitive, on the GS
 * thread, through the gs_draw.c hooks (gs_setup.h).
 *
 *   state     the decoded GS state (gs_prim P + raw registers) is analysed once per state version:
 *             blend form, write masks, alpha/Z test outcomes, targets, the StateRec.
 *   tier      FF / ROV / ALIAS16 / PUNT by the static rule of 4.1, plus per-primitive inputs: the
 *             source-alpha range (CLUT/texture-format bound x vertex alpha under TFX, AA1 coverage)
 *             for the pass-A split and the As > 128 rule, Z32 vertices above 0x3F000000.
 *   emission  StateRec (deduplicated per frame), 32-byte vertices + u32 indices for triangles,
 *             records (exact Z 32 B, full 128 B for ROV triangles, sprite 48 B, line pixel 32 B),
 *             commands (gpu_front.h). Consecutive primitives with the same pipeline key merge into
 *             one D3D draw; ROV-tier primitives on one target pair form a ROV run; primitives into 16-bit
 *             frames over C32 targets form ALIAS16 runs (staging gather / ROV draws / scatter) split at
 *             read-after-write hazards (M4).
 *   views     textures that are live colour targets (CT32/CT24, alpha index, CT16) read the target;
 *             feedback reads of written byte lanes take a shadow copy of the blocks written since the
 *             last copy, reads of lanes the draw does not write read the bound target (M4).
 *   PUNT      (5.4) the software rasterizer draws the primitive into gs_vram (same-thread frontend):
 *             touched GPU data is downloaded and read back first, the written blocks patched after.
 *   ownership targets, block validity, uploads, cross-target copies (gpu_own.c), CLUT ring (gpu_clut.c).
 *
 * M1 was a dry run (gs_replay -Backend hwdry still is); M2 executes the per-frame command stream on the
 * render thread (gpu_exec.c), M3 the REF tier, M4 the exact effects. */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"

#include <stdlib.h>
#include <windows.h>
#include <x86intrin.h>

/* (M11) gpu_d3d11.c: the ROV tier without rasterizer-ordered views (weak: the frontend unit tests link no device) */
extern int gpu_no_rov(void) __attribute__((weak));

gpu_front_frame gf;
gpu_front_config gf_cfg;
uint32_t gf_frame;
const uint8_t* gf_vram;
const uint16_t* gf_clut_src;
int gf_clut_hint = -1;

/* ---- arenas ------------------------------------------------------------------------------------- */
typedef struct { uint8_t* p; size_t n, cap; } arena;
static arena A_cmd, A_vtx, A_idx, A_rec, A_state, A_data;

/* A sink that consumes a frame later (the render thread, M2) detaches the frame's arenas instead of
 * copying them; the frontend continues with arenas from a free list that gpu_front_release refills
 * (from any thread). */
typedef struct gf_block { struct gf_block* next; arena a[6]; } gf_block;
static gf_block* free_blocks;
static int free_count;
#define FREE_KEEP 3                      /* (M9) free frames kept with their arenas; more (after a deep queue) are freed */
static SRWLOCK free_lock = SRWLOCK_INIT;
static int in_sink;

void* gpu_front_detach(void) {
    if (!in_sink) return NULL;
    AcquireSRWLockExclusive(&free_lock);
    gf_block* fresh = free_blocks;
    if (fresh) { free_blocks = fresh->next; free_count--; }
    ReleaseSRWLockExclusive(&free_lock);
    if (!fresh && !(fresh = calloc(1, sizeof *fresh))) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
    gf_block* b = malloc(sizeof *b);
    if (!b) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
    arena* cur[6] = { &A_cmd, &A_vtx, &A_idx, &A_rec, &A_state, &A_data };
    for (int i = 0; i < 6; i++) { b->a[i] = *cur[i]; *cur[i] = fresh->a[i]; cur[i]->n = 0; }
    free(fresh);
    b->next = NULL;
    return b;
}

/* (M9) the capacity of every frame arena (the building frame's, the frames in flight, the free ones) */
volatile LONG64 gf_arena_bytes;

void gpu_front_release(void* frame) {
    gf_block* b = frame;
    if (!b) return;
    AcquireSRWLockExclusive(&free_lock);
    int keep = free_count < FREE_KEEP;
    if (keep) { b->next = free_blocks; free_blocks = b; free_count++; }
    ReleaseSRWLockExclusive(&free_lock);
    if (keep) return;
    for (int i = 0; i < 6; i++) { InterlockedAdd64(&gf_arena_bytes, -(LONG64)b->a[i].cap); free(b->a[i].p); }
    free(b);
}
size_t gpu_front_arena_bytes(void) { return (size_t)gf_arena_bytes; }

static void* ar_reserve(arena* a, size_t bytes) {         /* not zeroed */
    if (a->n + bytes > a->cap) {
        size_t c = a->cap ? a->cap : 1 << 16;
        while (c < a->n + bytes) c *= 2;
        a->p = realloc(a->p, c);
        if (!a->p) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
        InterlockedAdd64(&gf_arena_bytes, (LONG64)(c - a->cap));
        a->cap = c;
    }
    void* r = a->p + a->n;
    a->n += bytes;
    return r;
}

static void* ar_alloc(arena* a, size_t bytes) {
    if (a->n + bytes > a->cap) {
        size_t c = a->cap ? a->cap : 1 << 16;
        while (c < a->n + bytes) c *= 2;
        a->p = realloc(a->p, c);
        if (!a->p) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
        InterlockedAdd64(&gf_arena_bytes, (LONG64)(c - a->cap));
        a->cap = c;
    }
    void* r = a->p + a->n;
    memset(r, 0, bytes);
    a->n += bytes;
    return r;
}

static void flush_open(void);
static int in_flush, cw_barrier;
int gf_trace;
#define trace_on gf_trace                     /* PS2_GPU_FRONT_TRACE=<vblank>: log that VBlank's draws */

void* gf_cmd(uint16_t op, uint32_t bytes) {
    /* an op that reads GPU data the open draw may write (download, shadow copy) or changes memory
     * it may have sampled (patch, release) closes the open draw / run / batch first, so it never
     * overtakes drawing already merged. Target creation, CLUT ring slots (always a fresh slot),
     * loads (only stale blocks, which the open draw has not touched) and clears (the same pixels
     * as the fill sprites before them) need no ordering against the open draw. */
    if ((op == GPU_FOP_PATCH || op == GPU_FOP_DOWNLOAD || op == GPU_FOP_COPY_SHADOW || op == GPU_FOP_RELEASE || op == GPU_FOP_XCOPY) && !in_flush) {
        if (trace_on) fprintf(stderr, "[front] op %u\n", op);
        flush_open();
    }
    if (op != GPU_FOP_DRAW) cw_barrier = 1;
    bytes = (bytes + 3) & ~3u;
    gpu_fop* h = ar_alloc(&A_cmd, bytes);
    h->op = op;
    h->size = (uint16_t)bytes;
    return h;
}

uint32_t gf_data(const void* src, uint32_t bytes) {
    uint32_t off = (uint32_t)A_data.n;
    void* d = ar_alloc(&A_data, (bytes + 15) & ~15u);
    if (src) memcpy(d, src, bytes);
    return off;
}

void* gf_data_ptr(uint32_t off) { return A_data.p + off; }

/* ---- completed frames ----------------------------------------------------------------------------- */
static gpu_front_frame* done;
static size_t ndone, capdone;
static SRWLOCK done_lock = SRWLOCK_INIT;            /* frames are closed on the frontend thread */
static void (*sink_fn)(const gpu_front_frame_data*, void*);
static void* sink_user;

void gpu_front_set_sink(void (*sink)(const gpu_front_frame_data* fd, void* user), void* user) { sink_fn = sink; sink_user = user; }

int gpu_front_take_frames(gpu_front_frame* out, int max) {
    AcquireSRWLockExclusive(&done_lock);
    int n = (int)(ndone < (size_t)max ? ndone : (size_t)max);
    memcpy(out, done, (size_t)n * sizeof *out);
    memmove(done, done + n, (ndone - (size_t)n) * sizeof *done);
    ndone -= (size_t)n;
    ReleaseSRWLockExclusive(&done_lock);
    return n;
}

/* ---- timing (rdtsc, calibrated against QPC at every VBlank) ------------------------------------- */
static uint64_t tsc_acc, tsc_start;
static LARGE_INTEGER qpc_start;
static double tsc_hz = 3.0e9;

/* ---- decoded registers -------------------------------------------------------------------------- */
enum { R_TEX0_1 = 0x06, R_CLAMP_1 = 0x08, R_TEX1_1 = 0x14, R_XYOFFSET_1 = 0x18, R_TEXA = 0x3B, R_FOGCOL = 0x3D,
       R_SCISSOR_1 = 0x40, R_ALPHA_1 = 0x42, R_COLCLAMP = 0x46, R_TEST_1 = 0x47, R_PABE = 0x49, R_FBA_1 = 0x4A,
       R_FRAME_1 = 0x4C, R_ZBUF_1 = 0x4E, R_DTHE = 0x45 };
enum { PRIM_POINT, PRIM_LINE, PRIM_LINESTRIP, PRIM_TRI, PRIM_TRISTRIP, PRIM_TRIFAN, PRIM_SPRITE };

/* ---- per-state analysis --------------------------------------------------------------------------- */
enum { TK_NONE, TK_VRAM_IDX, TK_VRAM_DIRECT, TK_VIEW_C32, TK_VIEW_ALPHA, TK_ALIAS16 };     /* texture source */
enum { BL_OFF, BL_NOCOLOR, BL_ADD, BL_REVSUB, BL_DUAL };                                   /* FF blend classes */
enum { FATST_NONE, FATST_DISCARD, FATST_SPLIT };

typedef struct {
    uint32_t version;
    int ctx;
    int skip;                            /* draws nothing (ATST NEVER + KEEP, ZTST NEVER, all writes masked) */
    int punt;                            /* GPU_PUNT_* + 1 */
    int rov;                             /* GPU_ROV_* + 1 (state-level reasons) */
    int alias;                           /* CT16 frame over a C32 target */
    gpu_target *ct, *zt;
    int cwrite, wmask;                   /* colour written; RenderTargetWriteMask (R1 G2 B4 A8) */
    int c_lanes;                         /* 1 low 24 bits, 2 top byte */
    int zread, zwrite, zfi;              /* Z test reads, Z written, GPU_ZF_* (-1 none) */
    int reads_dest;                      /* needs the current destination contents */
    int opaque;                          /* a sprite writes every unmasked bit of its rectangle */
    int fatst;
    int atst, aref;                      /* real alpha test (FATST_*) */
    int cd_dep, c_as;                    /* lerp blend (Cs-Cd)*C+Cd; C = As */
    int blend;
    int fill;                            /* clear candidate: untextured, unfogged, blend-free opaque sprite */
    int rmw;                             /* a read-modify-write form only the ROV tier does (wrap, bit masks, DATE, blend) */
    int depth_class;
    uint32_t phase_hint;                 /* PHASE hint label the StateRec was built with */
    uint32_t hud_hint, hud_scale;        /* HUD hint state it was built with (M6) */
    uint32_t straddle_frame;             /* gf_frame + 1 when a primitive of this state was escalated (sticky) */
    /* texture */
    int tme, tpsm, tw, th, wms, wmt, minu, maxu, minv, maxv, bil, stq;
    uint32_t tbp, tbw;
    int indexed;
    int tamin, tamax;                    /* texture alpha bounds (indexed: per CLUT slot) */
    uint32_t clut_slot;
    int tex_lanes;
    int sc[4];
    /* StateRec */
    uint32_t state_index, state_texsrc;
    int state_valid;
    /* memory-source cache: texels in tc[] are CPU data as of (tc_own_gen, tc_up_gen) */
    int tc_valid, tc_kind, tc[4];
    uint32_t tc_own_gen, tc_up_gen;
    /* (M9) alpha range of the palette entries the texture's texels actually use (T8/T4 from memory), as of own
     * change generation tu_gen - 1 (0: not computed); tu_lo > tu_hi: unknown */
    uint32_t tu_gen;
    int tu_lo, tu_hi;
} fstate;

/* The analysis of the last few distinct states, keyed by the registers it depends on: Sly alternates
 * pass A / pass B and rewrites TEX0 (with a CLUT reload of the same palette) for every draw, so most
 * state versions repeat a recent state. */
#define SC_N 1024                        /* (M9: 2-way set associative, state_lookup) */
#define SIG_N 24
static fstate SC[SC_N];
static uint64_t SC_sig[SC_N][SIG_N], SC_hash[SC_N];
static uint32_t SC_age[SC_N], sc_clock, sc_epoch;
static int SC_used[SC_N];
static fstate* Sp = &SC[0];
#define S (*Sp)
static int prev_state_index = -1;

/* StateRec (128 bytes, 3.5): gpu_front.h gpu_staterec */
typedef gpu_staterec staterec;
_Static_assert(sizeof(staterec) == GPU_STATE_BYTES, "StateRec size (shaders/gs.hlsl STATE_BYTES)");

#define SMAP_N 65536
static uint64_t smap_hash[SMAP_N];
static uint32_t smap_idx[SMAP_N];        /* index + 1 */
static uint32_t nstates;

static uint64_t hash_bytes(const void* p, size_t n) {
    const uint64_t* w = p;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < n / 8; i++) { h ^= w[i]; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 31; }
    return h ? h : 1;
}

static uint32_t staterec_index(const staterec* r) {
    uint64_t h = hash_bytes(r, sizeof *r);
    uint32_t i = (uint32_t)h & (SMAP_N - 1);
    for (int n = 0; n < SMAP_N && smap_idx[i]; n++, i = (i + 1) & (SMAP_N - 1))
        if (smap_hash[i] == h && gf_eq64(A_state.p + (size_t)(smap_idx[i] - 1) * sizeof *r, r, sizeof *r / 8))
            return smap_idx[i] - 1;
    if (nstates >= SMAP_N / 2) return 0;   /* (never: Sly uses < 500 per frame) */
    memcpy(ar_alloc(&A_state, sizeof *r), r, sizeof *r);
    smap_hash[i] = h;
    smap_idx[i] = ++nstates;
    gf.states++;
    gf.state_bytes += sizeof *r;
    return nstates - 1;
}

static int clamp_byte(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* how many source alphas in [lo, hi] pass the alpha test (gs_prim_finalize's table, closed form) */
static int atst_count(int atst, int ref, int lo, int hi) {
    int a, b, in = ref >= lo && ref <= hi;
    switch (atst) {
    case 0: return 0;                                          /* NEVER */
    case 1: return hi - lo + 1;                                /* ALWAYS */
    case 2: a = lo; b = hi < ref - 1 ? hi : ref - 1; break;    /* LESS */
    case 3: a = lo; b = hi < ref ? hi : ref; break;            /* LEQUAL */
    case 4: return in;                                         /* EQUAL */
    case 5: a = lo > ref ? lo : ref; b = hi; break;            /* GEQUAL */
    case 6: a = lo > ref + 1 ? lo : ref + 1; b = hi; break;    /* GREATER */
    default: return hi - lo + 1 - in;                          /* NOTEQUAL */
    }
    return b >= a ? b - a + 1 : 0;
}

static void state_update(const gs_env_view* ev);

#ifdef GPU_FRONT_PROF
static uint64_t n_lookups, n_misses, n_prims;
#define PCOUNT(x) ((x)++)
#else
#define PCOUNT(x) ((void)0)
#endif
static void state_lookup(const gs_env_view* ev) {
    PCOUNT(n_lookups);
    const uint64_t* r = ev->reg;
    int ctx = ev->ctx;
    uint64_t attr = (r[0x1A] & 1) ? r[0x00] : r[0x1B];
    uint64_t sig[SIG_N] = {
        (attr & 0x7F8) | (uint64_t)ctx << 32, r[R_FRAME_1 + ctx], r[R_ZBUF_1 + ctx], r[R_TEST_1 + ctx], r[R_ALPHA_1 + ctx],
        r[R_TEX0_1 + ctx] & ~(7ull << 61), r[R_TEX1_1 + ctx], r[R_CLAMP_1 + ctx], r[R_TEXA], r[R_FOGCOL],
        r[R_SCISSOR_1 + ctx], r[R_XYOFFSET_1 + ctx], r[R_PABE], r[R_FBA_1 + ctx], r[R_COLCLAMP], r[R_DTHE], r[0x44],
        r[0x22], r[0x34 + ctx], r[0x36 + ctx], 0, 0, 0, 0 };
    uint32_t psm = (uint32_t)((r[R_TEX0_1 + ctx] >> 20) & 0x3F);
    if ((attr & 0x10) && (psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH)) {
        uint32_t cs = clut_current_slot();             /* the palette's ring slot: repeats hit; (M9) its generation */
        sig[20] = ((uint64_t)cs + 1) | (uint64_t)clut_slot_gen(cs) << 32;
    }
    if (sc_epoch != own_epoch) { memset(SC_used, 0, sizeof SC_used); sc_epoch = own_epoch; }
    /* (M9) four independent lanes (a single multiply chain over 24 words is latency-bound) */
    uint64_t h0 = 0x9E3779B97F4A7C15ull, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    for (int i = 0; i < SIG_N; i += 4) {
        h0 = (h0 ^ sig[i]) * 0xFF51AFD7ED558CCDull; h1 = (h1 ^ sig[i + 1]) * 0xFF51AFD7ED558CCDull;
        h2 = (h2 ^ sig[i + 2]) * 0xFF51AFD7ED558CCDull; h3 = (h3 ^ sig[i + 3]) * 0xFF51AFD7ED558CCDull;
        h0 ^= h0 >> 29; h1 ^= h1 >> 29; h2 ^= h2 >> 29; h3 ^= h3 >> 29;
    }
    uint64_t h = ((h0 * 31 + h1) * 31 + h2) * 31 + h3;
    h ^= h >> 32;
    /* (M9) 2-way set associative over SC_N entries (Sly cycles through 200-600 states per frame: the M2 cache of
     * the last 32, scanned linearly, missed ~16% of the lookups and every miss re-analyses the state) */
    int i0 = (int)((h >> 17) & (SC_N - 1)) & ~1, victim = i0;
    for (int i = i0; i < i0 + 2; i++) {
        if (SC_used[i] && SC_hash[i] == h && gf_eq64(SC_sig[i], sig, SIG_N)) {
            Sp = &SC[i];
            SC_age[i] = ++sc_clock;
            S.version = ev->version;
            return;
        }
        if (!SC_used[i] || (SC_used[victim] && SC_age[i] < SC_age[victim])) victim = i;
    }
    Sp = &SC[victim];
    PCOUNT(n_misses);
    state_update(ev);
    memcpy(SC_sig[victim], sig, sizeof sig);
    SC_hash[victim] = h;
    SC_used[victim] = 1;
    SC_age[victim] = ++sc_clock;
}

static void state_update(const gs_env_view* ev) {
    const gs_prim* P = ev->P;
    const uint64_t* reg = ev->reg;
    int ctx = ev->ctx;
    memset(&S, 0, sizeof S);
    S.version = ev->version;
    S.ctx = ctx;
    S.zfi = -1;

    /* ---- tests ---- */
    int ate = P->ate, atst = P->atst, afail = P->afail;
    int a_never = ate && atst == 0;
    int zwrite_ok = 1, c_ok = 1, a_ok = 1;
    if (a_never) {
        switch (afail) {
        case 0: S.skip = 1; break;                                   /* KEEP: nothing */
        case 1: zwrite_ok = 0; break;                                /* FB_ONLY */
        case 2: c_ok = 0; break;                                     /* ZB_ONLY */
        default: zwrite_ok = 0; a_ok = 0; break;                     /* RGB_ONLY */
        }
        S.fatst = FATST_NONE;
    } else if (ate) {
        S.fatst = afail == 0 ? FATST_DISCARD : FATST_SPLIT;
        S.atst = atst;
        S.aref = P->aref;
    }
    if (P->zfmt && P->ztst == 0) S.skip = 1;                         /* ZTST NEVER */

    /* ---- colour writes ---- */
    uint32_t fmask = P->ffmt->bits == 24 ? (P->fbmsk | 0xFF000000u) : P->fbmsk;
    int bytes_ok = 1, wm = 0;
    if (P->ffmt->bits == 16) {
        wm = (P->fmask & 0x7FFF) != 0x7FFF ? 7 : 0;                  /* (16-bit frames: ALIAS16 only) */
        wm |= (P->fmask & 0x8000) ? 0 : 8;
    } else {
        for (int b = 0; b < 4; b++) {
            uint32_t m = (fmask >> (8 * b)) & 0xFF;
            if (m != 0 && m != 0xFF) bytes_ok = 0;
            if (m != 0xFF) wm |= 1 << b;
        }
    }
    if (!c_ok) wm = 0;
    if (!a_ok) wm &= 7;

    /* ---- blend (4.1, 4.2) ---- */
    int A = P->asel, B = P->bsel, C = P->csel, D = P->dsel;
    int uses_cd = 0, rov_blend = 0, rov_cc = 0;
    S.blend = BL_OFF;
    if (P->abe) {
        uses_cd = A == 1 || B == 1 || C == 1 || D == 1;
        if (A == B) {
            /* colour unchanged - unless PABE skips blending for source alpha < 0x80, which then
             * writes Cs (M3 fuzz finding) */
            if (D == 1 && !P->pabe) { S.blend = BL_NOCOLOR; wm &= 8; uses_cd = 0; }
        } else if (!uses_cd) {
            S.blend = BL_OFF;                                             /* the PS computes it exactly */
        } else if (C != 1 && A == 0 && B == 2 && D == 1) {
            S.blend = BL_ADD;
            if (!P->colclamp) rov_cc = 1;
        } else if (C != 1 && A == 2 && B == 0 && D == 1) {
            S.blend = BL_REVSUB;
            if (!P->colclamp) rov_cc = 1;
        } else if (C != 1 && A == 0 && B == 1 && D == 1) {
            S.blend = BL_DUAL;
            S.cd_dep = 1;
            S.c_as = C == 0;
        } else {
            rov_blend = 1;
        }
    }
    S.wmask = wm;
    S.cwrite = wm != 0;
    S.c_lanes = ((wm & 7) ? 1 : 0) | ((wm & 8) ? 2 : 0);

    /* ---- Z ---- */
    int zuse = P->zuse && P->zfmt;
    if (zuse) {
        S.zread = P->ztst == 2 || P->ztst == 3;
        S.zwrite = !P->zmsk && zwrite_ok;
        if (!S.zread && !S.zwrite) zuse = 0;
    }
    if (zuse) {
        switch (P->zpsm) {
        case PSMZ32: S.zfi = GPU_ZF_32; break;
        case PSMZ24: S.zfi = GPU_ZF_24; break;
        case PSMZ16: S.zfi = GPU_ZF_16; break;
        default: S.zfi = GPU_ZF_16S; break;
        }
    }
    if (!S.cwrite && !S.zwrite && !P->date) S.skip = 1;
    S.depth_class = zuse ? (P->ztst * 2 + S.zwrite) : 0;

    /* ---- texture ---- */
    S.tme = P->tme;
    if (P->tme) {
        uint64_t t0 = reg[R_TEX0_1 + ctx], cl = reg[R_CLAMP_1 + ctx], texa = reg[R_TEXA];
        S.tbp = (uint32_t)(t0 & 0x3FFF);
        S.tbw = (uint32_t)((t0 >> 14) & 0x3F);
        S.tpsm = (int)((t0 >> 20) & 0x3F);
        int tw = (int)((t0 >> 26) & 0xF), th = (int)((t0 >> 30) & 0xF);
        S.tw = 1 << (tw > 10 ? 10 : tw);
        S.th = 1 << (th > 10 ? 10 : th);
        S.wms = (int)(cl & 3); S.wmt = (int)((cl >> 2) & 3);
        S.minu = (int)((cl >> 4) & 0x3FF); S.maxu = (int)((cl >> 14) & 0x3FF);
        S.minv = (int)((cl >> 24) & 0x3FF); S.maxv = (int)((cl >> 34) & 0x3FF);
        S.bil = P->texm != 1;
        S.stq = !P->fst;
        uint32_t ta0 = (uint32_t)(texa & 0xFF), aem = (uint32_t)((texa >> 15) & 1), ta1 = (uint32_t)((texa >> 32) & 0xFF);
        switch (S.tpsm) {
        case PSMT8: case PSMT4: case PSMT8H: case PSMT4HL: case PSMT4HH:
            S.indexed = 1;
            S.clut_slot = clut_current_slot();
            clut_alpha_range(S.clut_slot, t0, texa, &S.tamin, &S.tamax);
            S.tex_lanes = S.tpsm == PSMT8 || S.tpsm == PSMT4 ? 3 : 2;
            break;
        case PSMCT24: case PSMZ24:
            S.tamin = aem ? 0 : (int)ta0; S.tamax = (int)ta0; S.tex_lanes = 1;
            break;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
            int lo = (int)(ta0 < ta1 ? ta0 : ta1), hi = (int)(ta0 > ta1 ? ta0 : ta1);
            if (aem) lo = 0;
            S.tamin = lo; S.tamax = hi; S.tex_lanes = 3;
            break;
        }
        default:
            S.tamin = 0; S.tamax = 255; S.tex_lanes = 3;
            break;
        }
    }
    S.sc[0] = P->scx0; S.sc[1] = P->scy0; S.sc[2] = P->scx1; S.sc[3] = P->scy1;

    /* ---- reads of the destination; opaque sprites; fills ---- */
    /* (a CT24 frame keeps the top byte of every C32 word: never a full overwrite of the block) */
    int full_wm = 15;
    S.reads_dest = (P->abe && uses_cd) || S.zread || P->date || (S.cwrite && !bytes_ok);
    S.opaque = !S.reads_dest && S.fatst == FATST_NONE && (!S.cwrite || (wm & full_wm) == full_wm) && P->scanmsk < 2;
    /* tiled-fill candidate: constant value per pixel, written whatever the destination holds (byte
     * masks allowed: the alpha reset is a fill of the alpha byte) */
    /* (perf-p2) BL_NOCOLOR too: (A - A) * C + Cd leaves the colour and writes As into the alpha lane (wm & 8) - Sly's alpha
     * reset after the shadow darken, a full-screen FF pass at every sample, becomes an alpha-lane clear */
    S.fill = !P->tme && !P->fge && (S.blend == BL_OFF || S.blend == BL_NOCOLOR) && !P->date && S.fatst == FATST_NONE && !S.zread &&
             P->scanmsk < 2 && (S.cwrite || S.zwrite) && P->ffmt->bits != 16 && !S.reads_dest;
    /* (M4: a blend that reads Cd is no fill, e.g. the bit-plane steps (Cd-0)*2>>7 under FBMSK FFFFFEFF;
     * BL_OFF only says the FF table has no class for that blend) */

    /* ---- targets ---- */
    if (P->ffmt->bits == 16) {
        /* CT16 frame: an ALIAS16 view of the C32 target at the same base and width (created when
         * the 16-bit view comes first, e.g. a recording that starts between the two) */
        gpu_target* T = own_find(TGT_C32, P->fpage, P->fbw);
        if (!T) T = own_target(TGT_C32, PSMCT32, P->fpage, P->fbw, 1);
        if (T) S.alias = 1, S.ct = T;
        else S.punt = GPU_PUNT_TARGET_OVERLAP + 1;
    } else if (S.cwrite || P->date || (S.reads_dest && P->abe)) {
        S.ct = own_target(TGT_C32, PSMCT32, P->fpage, P->fbw, 1);   /* rows grow with the drawing */
        if (!S.ct) S.punt = GPU_PUNT_TARGET_OVERLAP + 1;
    }
    if (zuse) {
        int kind = own_target_kind_of_zpsm(P->zpsm);
        S.zt = own_target(kind, P->zpsm, P->zpage, P->zbw, 1);
        if (!S.zt) S.punt = GPU_PUNT_TARGET_OVERLAP + 1;
        /* (M7: the pair's sample grids are matched per primitive, fr_prim: families change at VBlanks) */
    }

    /* ---- state-level ROV reasons, in the order of 4.1 ---- */
    S.rmw = rov_cc || (S.cwrite && !bytes_ok) || P->date || rov_blend;
    if (gf_cfg.ref) S.rov = GPU_ROV_REF + 1;
    else if (rov_cc) S.rov = GPU_ROV_COLCLAMP + 1;
    else if (S.cwrite && !bytes_ok) S.rov = GPU_ROV_FBMSK + 1;
    else if (P->date) S.rov = GPU_ROV_DATE + 1;
    else if (rov_blend) S.rov = GPU_ROV_BLEND + 1;
    else if (S.cd_dep && C == 2 && P->fix > 128) S.rov = GPU_ROV_C_OVER128 + 1;
    else if ((P->pabe && P->abe) || (P->dthe && P->ffmt->bits == 16)) S.rov = GPU_ROV_PABE_DTHE + 1;
}

/* ---- StateRec for the primitive (texture source is per primitive) ------------------------------- */

/* bits per texel as gs_draw.c's bind_textures counts them for automatic mipmap addresses */
static uint32_t mip_bpp(uint32_t psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    default: return 32;
    }
}

/* texture mode, level and mipmap addresses from the registers, exactly as gs_draw.c's bind_textures
 * derives them (the frontend never reads the software rasterizer's level bindings) */
static void tex_levels(const uint64_t* reg, int ctx, staterec* r) {
    uint64_t t0 = reg[R_TEX0_1 + ctx], tex1 = reg[R_TEX1_1 + ctx];
    uint32_t tbp = (uint32_t)(t0 & 0x3FFF), tbw = (uint32_t)((t0 >> 14) & 0x3F), psm = (uint32_t)((t0 >> 20) & 0x3F);
    uint32_t th = (uint32_t)((t0 >> 30) & 0xF);
    if (th > 10) th = 10;
    int lcm = (int)(tex1 & 1), mxl = (int)((tex1 >> 2) & 7), mmag = (int)((tex1 >> 5) & 1), mmin = (int)((tex1 >> 6) & 7);
    if (mxl > 6) mxl = 6;
    if (mmin > 5) mmin = 1;
    int lshift = (int)((tex1 >> 19) & 3), k = (int)(((int64_t)(tex1 << 20)) >> 52), mtba = (int)((tex1 >> 9) & 1);
    if (!(mxl > 0 && mmin >= 2)) mxl = 0;
    int texm, cur = 0;
    if (lcm || (mmin <= 1 && mmin == mmag)) {
        int lod = lcm ? k : (mmag ? 1 : 0);
        if (!lcm || lod <= 0) texm = 1 + mmag;
        else switch (mmin) {
        case 0: texm = 1; break;
        case 1: texm = 2; break;
        case 2: case 4: texm = mmin == 4 ? 2 : 1; cur = (lod + 8) >> 4 < mxl ? (lod + 8) >> 4 : mxl; break;
        default:
            if ((lod >> 4) >= mxl) { texm = mmin == 5 ? 2 : 1; cur = mxl; }
            else texm = 3;
            break;
        }
    } else {
        texm = 3;
    }
    uint64_t mt1 = reg[0x34 + ctx], mt2 = reg[0x36 + ctx];
    for (int l = 1; l <= mxl; l++) {
        if (mtba && l <= 3) {
            uint32_t ph = 1u << (th > (uint32_t)(l - 1) ? th - (uint32_t)(l - 1) : 0);
            tbp = (tbp + (tbw * 64u * ph * mip_bpp(psm) / 8u) / 256u) & 0x3FFF;
            tbw = tbw > 1 ? tbw / 2 : 1;
        } else {
            uint64_t m = l <= 3 ? mt1 : mt2;
            int sh = ((l - 1) % 3) * 20;
            tbp = (uint32_t)((m >> sh) & 0x3FFF);
            tbw = (uint32_t)((m >> (sh + 14)) & 0x3F);
        }
        r->mip[l - 1] = tbp | tbw << 14;
    }
    r->tlev = (uint32_t)texm | (uint32_t)cur << 2 | (uint32_t)mxl << 5 | (uint32_t)mmag << 8 | (uint32_t)mmin << 9 |
              (uint32_t)lcm << 12 | (uint32_t)lshift << 13 | ((uint32_t)k & 0xFFFu) << 16;
}

/* phase label (diagnostic) of a draw: what it is, from its own state */
static uint32_t phase_of(uint32_t texsrc) {
    uint32_t k = texsrc & 0xFF, view = k >= TK_VIEW_C32;
    if (S.alias) return GPU_PH_ALIAS16;
    if (view && S.ct && (texsrc >> 8 & 0xFF) == S.ct->id) return GPU_PH_FEEDBACK;
    if (view) return GPU_PH_VIEW;
    if (S.fill) return GPU_PH_FILL;
    if (S.rmw) return GPU_PH_ROV_RMW;
    return GPU_PH_SCENE;
}

static uint32_t state_for(const gs_env_view* ev, uint32_t texsrc) {
    if (S.state_valid && S.state_texsrc == texsrc && S.phase_hint == ev->phase && S.hud_hint == ev->hud &&
        S.hud_scale == ev->hud_scale)
        return S.state_index;
    const gs_prim* P = ev->P;
    const uint64_t* reg = ev->reg;
    int ctx = ev->ctx;
    staterec r;
    memset(&r, 0, sizeof r);
    r.clut_slot = 0xFFFFFFFFu;
    if (P->tme) {
        r.tex0 = reg[R_TEX0_1 + ctx] & ~(7ull << 61);
        r.clamp = reg[R_CLAMP_1 + ctx];
        r.tex1 = reg[R_TEX1_1 + ctx];
        uint64_t texa = reg[R_TEXA];
        r.texa = (uint32_t)(texa & 0xFF) | (uint32_t)((texa >> 15) & 1) << 15 | (uint32_t)((texa >> 32) & 0xFF) << 16;
        if (S.indexed) r.clut_slot = S.clut_slot;
        r.texsrc = texsrc;
        tex_levels(reg, ctx, &r);
    }
    r.fogcol = P->fge ? (uint32_t)reg[R_FOGCOL] & 0xFFFFFFu : 0;
    r.test = (uint32_t)P->ate | (uint32_t)P->atst << 1 | (uint32_t)P->aref << 4 | (uint32_t)P->afail << 12 |
             (uint32_t)P->date << 14 | (uint32_t)P->datm << 15;
    r.fmask = P->fmask;
    r.flags = (P->fba ? GPU_SF_FBA : 0) | (P->colclamp ? GPU_SF_COLCLAMP : 0) | (uint32_t)P->tfx << GPU_SF_TFX_SHIFT |
              (P->tcc ? GPU_SF_TCC : 0) | (P->fge ? GPU_SF_FGE : 0) | (P->iip ? GPU_SF_IIP : 0) | (P->fst ? GPU_SF_FST : 0) |
              (P->abe ? GPU_SF_ABE : 0) | (ev->aa1 ? GPU_SF_AA1 : 0) | (P->pabe ? GPU_SF_PABE : 0) |
              (P->dthe ? GPU_SF_DTHE : 0) | (P->tme ? GPU_SF_TME : 0) | (P->ffmt->bits == 24 ? GPU_SF_FB24 : 0) |
              (uint32_t)P->scanmsk << GPU_SF_SCANMSK_SHIFT | (ctx ? GPU_SF_CTX : 0) | (P->ffmt->bits == 16 ? GPU_SF_FB16 : 0);
    if (P->ffmt->bits == 16 && P->dthe) r.dimx = reg[0x44];   /* DIMX */
    r.phase = phase_of(texsrc) | (ev->phase ? GPU_PH_HINT : 0);
    r.phase_label = ev->phase;           /* (the PHASE hint's label) */
    r.hud = ev->hud;                     /* (M6: HUD hint state, for the M10 vertex-shader transform) */
    r.hud_scale = ev->hud_scale;
    r.alpha = P->abe ? (uint32_t)P->asel | (uint32_t)P->bsel << 2 | (uint32_t)P->csel << 4 | (uint32_t)P->dsel << 6 |
                       (uint32_t)P->fix << 8 : 0;
    uint32_t zb = !P->zfmt ? 0 : P->zfmt->bits == 16 ? 1 : P->zfmt->bits == 24 ? 2 : 3;
    r.zinfo = (uint32_t)P->ztst | (uint32_t)(P->zmsk != 0) << 2 | (uint32_t)(P->zuse != 0) << 3 | zb << 4;
    r.xyoffset = (uint32_t)(ev->ofx & 0xFFFF) | (uint32_t)ev->ofy << 16;
    S.state_index = staterec_index(&r);
    S.state_texsrc = texsrc;
    S.phase_hint = ev->phase;
    S.hud_hint = ev->hud;
    S.hud_scale = ev->hud_scale;
    S.state_valid = 1;
    return S.state_index;
}

/* ---- open draw / ROV run / ALIAS16 batch ---------------------------------------------------------- */
typedef struct {
    int open;
    uint64_t key;
    gpu_fop_draw d;
    int outcomes;                        /* 1 pass, 2 fail, 4 straddle (FATST_SPLIT draws) */
    uint32_t prims;                      /* (trace) */
    uint64_t alpha, test, frame;
    int bbox[4];                         /* pixels touched (union of the primitives' rectangles) */
    uint32_t phase;                      /* phase label of its first primitive (trace) */
} open_draw;
static open_draw OD;
static uint32_t od_serial;               /* incremented whenever a draw opens (per-draw side tables know their draw) */

/* Commutation estimate (M2's first no-go mitigation, design 11): a closed FF draw could merge into
 * an earlier draw with the same pipeline key if it commutes with every draw between them (disjoint
 * pixel rectangles on shared targets, and no draw samples a target the other writes). Counted only
 * (d3d_draws_commute); the stream keeps the order. */
#define CW_N 16
typedef struct { uint64_t key; int bbox[4]; uint16_t ct, zt, tex; uint8_t wct, wzt; } cw_entry;
static cw_entry CW[CW_N];
static int cw_n;
static void cw_reset(void) { cw_n = 0; }

static int cw_commutes(const cw_entry* x, const cw_entry* d) {
    int disjoint = x->bbox[2] < d->bbox[0] || d->bbox[2] < x->bbox[0] || x->bbox[3] < d->bbox[1] || d->bbox[3] < x->bbox[1];
    /* sampling a target the other writes never commutes */
    if (d->tex && ((x->wct && x->ct == d->tex) || (x->wzt && x->zt == d->tex))) return 0;
    if (x->tex && ((d->wct && d->ct == x->tex) || (d->wzt && d->zt == x->tex))) return 0;
    int shared = (x->ct && (x->ct == d->ct)) || (x->zt && (x->zt == d->zt));
    return !shared || disjoint;
}

static int cw_add(uint64_t key, const int bbox[4], uint16_t ct, uint16_t zt, uint16_t tex, int wct, int wzt) {
    cw_entry d = { key, { bbox[0], bbox[1], bbox[2], bbox[3] }, ct, zt, tex, (uint8_t)wct, (uint8_t)wzt };
    for (int i = cw_n - 1; i >= 0; i--) {
        cw_entry* x = &CW[i];
        if (x->key == key) {             /* merge: D moves up to x */
            for (int k = 0; k < 2; k++) { if (bbox[k] < x->bbox[k]) x->bbox[k] = bbox[k]; if (bbox[k + 2] > x->bbox[k + 2]) x->bbox[k + 2] = bbox[k + 2]; }
            return 1;
        }
        if (!cw_commutes(x, &d)) break;
    }
    if (cw_n == CW_N) { memmove(CW, CW + 1, (CW_N - 1) * sizeof *CW); cw_n--; }
    CW[cw_n++] = d;
    return 0;
}
static uint32_t trace_vblank = 0xFFFFFFFFu;
static int dual_exact;                   /* PS2_GPU_DUAL_EXACT=1: partial-alpha dual-source blends on the ROV tier (M8) */

typedef struct {
    int open;
    uint16_t ct, zt;
    int wrote_z;
    int bbox[4];
} rov_run;
static rov_run RR;

/* ALIAS16 run (5.2): primitives into a 16-bit frame over the C32 target T, between a gather (T's
 * halfwords -> staging texture in the frame's pixel space) and a scatter (back); bbox in 16-bit pixels */
typedef struct {
    int open;
    gpu_target* T;
    int psm;
    uint32_t begin_off;                  /* byte offset of the begin op in the command arena (bbox filled at close) */
    int bbox[4];
    uint32_t prims;
    int copy;                            /* (perf-p2) still a copy run (gpu_fop_alias16.mode 1): every primitive so far
                                            a16_copy_prim */
} alias_batch;
static alias_batch AB;
static int nar;                          /* hazard rectangles of the open ALIAS16 run */
static int a16_copy_on = 1;              /* (perf-p2) PS2_GPU_A16_COPY=0: every ALIAS16 run through staging + ROV draws */

static const char* const phase_names[GPU_PH_N] = { "scene", "fill", "rov-rmw", "alias16", "view", "feedback" };

static void close_draw(void) {
    if (!OD.open) return;
    OD.open = 0;
    gpu_fop_draw* d = gf_cmd(GPU_FOP_DRAW, sizeof *d);
    *d = OD.d;
    d->h.op = GPU_FOP_DRAW;
    d->h.size = sizeof *d;
    for (int i = 0; i < 4; i++) d->bbox[i] = (int16_t)(OD.bbox[i] < -32768 ? -32768 : OD.bbox[i] > 32767 ? 32767 : OD.bbox[i]);
    if (cw_barrier) { cw_reset(); cw_barrier = 0; }
    /* ATST split (FF): the pass draw for primitives that can pass, the fail draw for those that can fail */
    int halves = OD.d.tier != GPU_TIER_FF || !OD.outcomes ? 0 : ((OD.outcomes & 5) ? 1 : 0) | ((OD.outcomes & 6) ? 2 : 0);
    int split = halves == 3;
    d->split = (uint8_t)halves;
    gf.d3d_draws += 1 + split;
    gf.split_extra += split;
    if (OD.d.tier == GPU_TIER_ROV || OD.d.tier == GPU_TIER_ALIAS16) { gf.rov_draws++; cw_reset(); gf.d3d_draws_commute++; }
    else {
        gf.ff_draws += 1 + split;
        /* (the M2 estimate; M9: not in real frames, where it cost frontend time for a statistic) */
        if (gf_cfg.mixed || !cw_add(OD.key ^ (uint64_t)split << 62, OD.bbox, OD.d.ct, OD.d.zt, OD.d.tex, OD.d.wmask != 0, (OD.d.depth & 1) != 0))
            gf.d3d_draws_commute += 1 + split;
    }
    if (trace_on)
        fprintf(stderr, "[front] draw %s %-8s cls %d prims %u count %u ct %u zt %u tex %u ps %03X blend %d depth %d wmask %X split %d "
                        "FRAME %016llX ALPHA %010llX TEST %05llX bbox %d %d %d %d\n", OD.d.tier == GPU_TIER_ROV ? "ROV" : OD.d.tier == GPU_TIER_ALIAS16 ? "A16" : "FF ", phase_names[OD.phase % GPU_PH_N], OD.d.cls,
                OD.prims, OD.d.count, OD.d.ct, OD.d.zt, OD.d.tex, OD.d.ps_key, OD.d.blend, OD.d.depth, OD.d.wmask, split,
                (unsigned long long)OD.frame, (unsigned long long)OD.alpha, (unsigned long long)OD.test, OD.bbox[0], OD.bbox[1], OD.bbox[2], OD.bbox[3]);
}

static void close_run(void) {
    if (!RR.open) return;
    close_draw();
    RR.open = 0;
    gpu_fop_rov_end* e = gf_cmd(GPU_FOP_ROV_END, sizeof *e);
    e->ct = RR.ct; e->zt = RR.zt; e->zresolve = (uint32_t)RR.wrote_z;
    for (int i = 0; i < 4; i++) e->bbox[i] = (int16_t)RR.bbox[i];
    gf.rov_runs++;
    if (trace_on) fprintf(stderr, "[front] rov run end ct %u zt %u zresolve %d\n", RR.ct, RR.zt, RR.wrote_z);
    if (RR.wrote_z) { gf.zresolves++; gf.d3d_draws++; gf.d3d_draws_commute++; }
}

static void close_alias(void) {
    if (!AB.open) return;
    close_draw();
    AB.open = 0;
    /* the gather and scatter cover whole 16x8 blocks (= whole 32-bit words: both halfwords of every
     * word are staged; pixels no primitive wrote go back unchanged) */
    int x0 = AB.bbox[0] & ~15, y0 = AB.bbox[1] & ~7, x1 = (AB.bbox[2] | 15) + 1, y1 = (AB.bbox[3] | 7) + 1;
    gpu_fop_alias16* b = (gpu_fop_alias16*)(A_cmd.p + AB.begin_off);
    b->x0 = (int16_t)x0; b->y0 = (int16_t)y0; b->x1 = (int16_t)x1; b->y1 = (int16_t)y1;
    b->mode = (uint16_t)(AB.copy != 0);
    gpu_fop_alias16* e = gf_cmd(GPU_FOP_ALIAS16_END, sizeof *e);
    b = (gpu_fop_alias16*)(A_cmd.p + AB.begin_off);   /* (the arena may have moved) */
    *e = *b;
    e->h.op = GPU_FOP_ALIAS16_END;
    gf.alias16_dispatches++;
    if (trace_on)
        fprintf(stderr, "[front] alias16 run end target %u psm %02X prims %u (%d,%d)-(%d,%d) copy %d\n", AB.T->id, AB.psm, AB.prims, x0, y0, x1, y1, AB.copy);
    nar = 0;
}

/* gpu_own.c: an op on target id (create/grow, load) is about to be emitted: an ALIAS16 run on it ends
 * first (its staging copy was gathered before the op) */
void gf_target_op(uint16_t id) {
    if (AB.open && AB.T->id == id && !in_flush) {
        in_flush = 1;
        close_alias();
        in_flush = 0;
    }
}

static void flush_open(void) {
    in_flush = 1;
    close_draw();
    close_run();
    close_alias();
    in_flush = 0;
}

/* ---- emission ----------------------------------------------------------------------------------- */
typedef struct {                         /* 32 bytes */
    float x, y;
    uint32_t rgba, fog_state;
    float s, t, q;
    uint32_t pad;
} gvtx;
typedef struct { int64_t row, dy, dx; int16_t by0; uint16_t zfmt; uint32_t pad; } zrec;
typedef gpu_rec_tri fullrec;
typedef gpu_rec_sprite spriterec;
typedef gpu_rec_linepix linerec;
_Static_assert(sizeof(gvtx) == 32 && sizeof(zrec) == 32 && sizeof(fullrec) == 256 && sizeof(spriterec) == 64 &&
               sizeof(linerec) == 32, "record sizes (3.5)");
_Static_assert(offsetof(gpu_rec_tri, s0) == 120 && offsetof(gpu_rec_tri, ea) == 192 && offsetof(gpu_rec_tri, ec) == 208 &&
               offsetof(gpu_rec_tri, nedge) == 224 && offsetof(gpu_rec_sprite, vc0) == 32 && offsetof(gpu_rec_sprite, lod_q) == 48 &&
               offsetof(gpu_rec_linepix, s) == 16 && offsetof(gpu_staterec, texa) == 24 && offsetof(gpu_staterec, mip) == 64,
               "record layouts as shaders/gs.hlsl reads them");

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static gs_vtx vcache[3];
static uint32_t vcache_idx[3], vcache_state[3];
static uint8_t vcache_nb[3];
static int vcache_n, vcache_pos;

/* nb: (P1) the vertex carries no early-depth bound (pad 0xFFFFFFFF: the vertex shader's bound is 1.0, gpu_front.h) -
 * a triangle that needs that must not reuse a bounded copy; a bounded triangle may reuse either (a higher bound is
 * still a bound) */
static uint32_t emit_vertex(const gs_vtx* v, const gs_env_view* ev, uint32_t state, int nb) {
#ifndef GPU_NO_VCACHE
    for (int i = 0; i < vcache_n; i++)
        if (vcache_state[i] == state && (vcache_nb[i] || !nb) && !memcmp(&vcache[i], v, offsetof(gs_vtx, serial)))
            return vcache_idx[i];   /* (values, not the serial) */
#endif
    gvtx* o = ar_reserve(&A_vtx, sizeof *o);
    o->x = (float)(v->x - ev->ofx) * (1.0f / 16.0f);
    o->y = (float)(v->y - ev->ofy) * (1.0f / 16.0f);
    o->rgba = (uint32_t)v->r | (uint32_t)v->g << 8 | (uint32_t)v->b << 16 | (uint32_t)v->a << 24;
    o->fog_state = (uint32_t)(v->f & 0xFF) | state << 8;
    if (ev->P->fst) { o->s = (float)v->u; o->t = (float)v->v; o->q = 1.0f; }
    else { o->s = v->s; o->t = v->t; o->q = v->q; }
    o->pad = nb ? 0xFFFFFFFFu : v->z;    /* (P1) the vertex Z: the early-depth bound of FF triangles (vs_ff_tri) */
    uint32_t idx = (uint32_t)(A_vtx.n / sizeof *o) - 1;
    vcache[vcache_pos] = *v; vcache_idx[vcache_pos] = idx; vcache_state[vcache_pos] = state; vcache_nb[vcache_pos] = (uint8_t)nb;
    vcache_pos = (vcache_pos + 1) % 3;
    if (vcache_n < 3) vcache_n++;
    gf.vertices++;
    gf.vtx_bytes += sizeof *o;
    return idx;
}

static uint32_t rec_count(size_t sz) { return (uint32_t)(A_rec.n / sz); }

/* ---- texture source of a primitive --------------------------------------------------------------- */
typedef struct { int kind; gpu_target* T; int u0, v0, u1, v1; int punt; } texsrc_t;

static int wrap_range(int lo, int hi, int size, int mode, int rmin, int rmax, int* o0, int* o1) {
    switch (mode) {
    case 0:                              /* REPEAT */
        if (lo < 0 || hi > size - 1) { lo = 0; hi = size - 1; }
        break;
    case 1:                              /* CLAMP */
        lo = lo < 0 ? 0 : lo > size - 1 ? size - 1 : lo;
        hi = hi < 0 ? 0 : hi > size - 1 ? size - 1 : hi;
        break;
    case 2:                              /* REGION_CLAMP */
        lo = lo < rmin ? rmin : lo > rmax ? rmax : lo;
        hi = hi < rmin ? rmin : hi > rmax ? rmax : hi;
        break;
    default:                             /* REGION_REPEAT: (c & umsk) | ufix */
        lo = 0; hi = size - 1;
        if (rmax < size) { lo = rmax & ~rmin; hi = (size - 1) & rmin; hi |= rmax; if (hi > size - 1) hi = size - 1; if (lo > hi) lo = 0; }
        break;
    }
    *o0 = lo; *o1 = hi;
    return 1;
}

/* the texel rectangle (level 0, inclusive) a primitive can sample: tex_prepare's rule */
static void texel_rect(const gs_env_view* ev, const gs_vtx* const* vs, int n, const double* su, const double* sv,
                       int* u0, int* v0, int* u1, int* v1) {
    const gs_prim* P = ev->P;
    double umin = 1e30, umax = -1e30, vmin = 1e30, vmax = -1e30;
    int all = P->texm == 3;
    if (su) {
        for (int i = 0; i < 2; i++) {
            umin = fmin(umin, su[i] / 16.0); umax = fmax(umax, su[i] / 16.0);
            vmin = fmin(vmin, sv[i] / 16.0); vmax = fmax(vmax, sv[i] / 16.0);
        }
    } else if (P->fst) {
        for (int i = 0; i < n; i++) {
            umin = fmin(umin, vs[i]->u / 16.0); umax = fmax(umax, vs[i]->u / 16.0);
            vmin = fmin(vmin, vs[i]->v / 16.0); vmax = fmax(vmax, vs[i]->v / 16.0);
        }
    } else {
        int pos = 0, neg = 0;
        for (int i = 0; i < n; i++) {
            float q = vs[i]->q;
            if (q > 0) pos++; else if (q < 0) neg++;
            double iq = 1.0 / q, uu = vs[i]->s * iq * P->tw16 / 16.0, vv = vs[i]->t * iq * P->th16 / 16.0;
            if (!(fabs(uu) < 1e7) || !(fabs(vv) < 1e7)) all = 1;
            umin = fmin(umin, uu); umax = fmax(umax, uu);
            vmin = fmin(vmin, vv); vmax = fmax(vmax, vv);
        }
        if (pos != n && neg != n) all = 1;
    }
    int a0 = 0, a1 = S.tw - 1, b0 = 0, b1 = S.th - 1;
    if (!all) {
        const double EPS = su ? 0 : 1.0 / 16;
        if (S.bil) {
            a0 = (int)floor(umin - 0.5 - EPS); a1 = (int)ceil(umax - 0.5 + EPS);
            b0 = (int)floor(vmin - 0.5 - EPS); b1 = (int)ceil(vmax - 0.5 + EPS);
        } else {
            a0 = (int)floor(umin - EPS); a1 = (int)floor(umax + EPS);
            b0 = (int)floor(vmin - EPS); b1 = (int)floor(vmax + EPS);
        }
    }
    wrap_range(a0, a1, S.tw, S.wms, S.minu, S.maxu, u0, u1);
    wrap_range(b0, b1, S.th, S.wmt, S.minv, S.maxv, v0, v1);
}

static void tex_source(const gs_env_view* ev, int u0, int v0, int u1, int v1, texsrc_t* ts) {
    memset(ts, 0, sizeof *ts);
    ts->u0 = u0; ts->v0 = v0; ts->u1 = u1; ts->v1 = v1;
    int psm = S.tpsm;
    if (S.tc_valid && S.tc_own_gen == own_gen && S.tc_up_gen == own_upload_gen && u0 >= S.tc[0] && v0 >= S.tc[1] &&
        u1 <= S.tc[2] && v1 <= S.tc[3]) {
        ts->kind = S.tc_kind;            /* checked for this state already, nothing changed since */
        return;
    }
    /* small textures: check the whole of it once per state (later primitives hit the cache) */
    int a0 = u0, b0 = v0, a1 = u1, b1 = v1;
    if (S.tw * S.th <= 65536) { a0 = 0; b0 = 0; a1 = S.tw - 1; b1 = S.th - 1; }
    if (own_mem_is_gpu(psm, S.tbp, S.tbw, a0, b0, a1, b1)) { a0 = u0; b0 = v0; a1 = u1; b1 = v1; }
    if (!own_mem_is_gpu(psm, S.tbp, S.tbw, a0, b0, a1, b1)) {   /* memory source (vram_buf) */
        own_mem_read(psm, S.tbp, S.tbw, a0, b0, a1, b1);       /* flushes pending patches */
        ts->kind = S.indexed ? TK_VRAM_IDX : TK_VRAM_DIRECT;
        S.tc_valid = 1; S.tc_kind = ts->kind;
        S.tc[0] = a0; S.tc[1] = b0; S.tc[2] = a1; S.tc[3] = b1;
        S.tc_own_gen = own_gen; S.tc_up_gen = own_upload_gen;
        return;
    }
    gpu_target* T = (S.tbp & 31) == 0 ? own_find(TGT_C32, S.tbp >> 5, S.tbw) : NULL;
    if (!T) { ts->punt = GPU_PUNT_TEX_FOREIGN + 1; return; }   /* GPU data, not at a target's base/width */
    /* the texture is a live colour target (exact base and width) */
    switch (psm) {
    case PSMCT32: case PSMCT24: ts->kind = TK_VIEW_C32; break;
    case PSMT8H: case PSMT4HL: case PSMT4HH: ts->kind = TK_VIEW_ALPHA; break;
    case PSMCT16: case PSMCT16S: ts->kind = TK_ALIAS16; break;   /* valid only inside ALIAS16 */
    default: ts->punt = GPU_PUNT_TEX_FMT + 1; return;
    }
    ts->T = T;
    (void)ev;
}

/* (M9) The source-alpha bound of an indexed texture from the palette entries its texels use, not the whole
 * palette (the CLUT slot's min/max, clut_alpha_range). Sly's palettes hold entries with alpha above 0x80 that the
 * texels drawn with them never use: every "As > 128" pass-A lerp of the corpus (gpu_front C>128 counter: 105k
 * primitives in the ep7 boss, 80k ep8 boss, 40k ep5 street) has no texel above 0x80 in its texel rect, and those
 * primitives were the ROV runs that dominate the GPU time of such scenes. The used-index set (256 bits) is kept per
 * texture (TBP, TBW, PSM, size) and recomputed from gs_vram when a block of it changes (block serials); it is sound
 * for every texel the primitive can sample (the whole texture). Textures in GPU-written or downloaded memory have
 * no set (the palette bound stays). */
#define TU_N 512
typedef struct { uint64_t key, serials; uint32_t gen; int valid; uint64_t used[4]; } tu_entry;
static tu_entry TU[TU_N];

static int tex_used_indices(uint64_t used[4]) {
    if ((S.tpsm != PSMT8 && S.tpsm != PSMT4) || S.tw * S.th > 512 * 512) return 0;
    uint64_t key = (uint64_t)S.tbp | (uint64_t)S.tbw << 14 | (uint64_t)S.tpsm << 20 | (uint64_t)S.tw << 26 | (uint64_t)S.th << 38 | 1ull << 63;
    uint64_t hk = key * 0x9E3779B97F4A7C15ull;
    tu_entry* e = &TU[(hk >> 40) & (TU_N - 1)];
    uint32_t g = own_change_gen();
    if (e->valid && e->key == key && e->gen == g) { memcpy(used, e->used, 32); return 1; }
    uint64_t sh = own_rect_serials(S.tpsm, S.tbp, S.tbw, 0, 0, S.tw - 1, S.th - 1);
    if (!sh) return 0;
    if (!(e->valid && e->key == key && e->serials == sh)) {
        const uint8_t* vm = gf_vram ? gf_vram : gs_vram;
        memset(e->used, 0, sizeof e->used);
        for (int y = 0; y < S.th; y++)
            for (int x = 0; x < S.tw; x++) {
                uint32_t idx;
                if (S.tpsm == PSMT8) idx = vm[gs_addr8(S.tbp, S.tbw, (uint32_t)x, (uint32_t)y)];
                else { uint32_t na = gs_addr4(S.tbp, S.tbw, (uint32_t)x, (uint32_t)y); idx = (vm[na >> 1] >> ((na & 1) * 4)) & 15; }
                e->used[idx >> 6] |= 1ull << (idx & 63);
            }
        e->key = key;
        e->serials = sh;
        e->valid = 1;
        gf.tex_used_scans++;
    }
    e->gen = g;
    memcpy(used, e->used, 32);
    return 1;
}

/* the alpha range of the palette entries the current state's texture uses (S.tu_lo/tu_hi; lo > hi: unknown) */
static void tex_used_alpha(const gs_env_view* ev) {
    uint32_t g = own_change_gen() + 1;
    if (S.tu_gen == g) return;
    S.tu_gen = g;
    S.tu_lo = 255; S.tu_hi = -1;
    uint64_t used[4];
    if (!tex_used_indices(used)) return;
    uint64_t t0 = ev->reg[R_TEX0_1 + ev->ctx], texa = ev->reg[R_TEXA];
    for (uint32_t i = 0; i < 256; i++)
        if (used[i >> 6] >> (i & 63) & 1) {
            int a = clut_entry_alpha(S.clut_slot, t0, texa, i);
            if (a < S.tu_lo) S.tu_lo = a;
            if (a > S.tu_hi) S.tu_hi = a;
        }
}

/* feedback: the primitive samples the target it writes, on a lane it writes */
static void feedback_check(gpu_target* T, int x0, int y0, int x1, int y1) {
    int full;
    if (!own_shadow_need(T, x0, y0, x1, y1, &full)) return;
    if (full == 1) { x0 = 0; y0 = 0; x1 = T->fbw * 64 - 1; y1 = T->rows - 1; }
    else if (full == 2) { x0 = T->copy_rect[0]; y0 = T->copy_rect[1]; x1 = T->copy_rect[2]; y1 = T->copy_rect[3]; }
    gpu_fop_shadow* o = gf_cmd(GPU_FOP_COPY_SHADOW, sizeof *o);
    o->target = T->id;
    o->x0 = (int16_t)x0; o->y0 = (int16_t)y0; o->x1 = (int16_t)x1; o->y1 = (int16_t)y1;
    gf.shadow_copies++;
    if (trace_on) fprintf(stderr, "[front] shadow copy target %u %d,%d-%d,%d\n", T->id, x0, y0, x1, y1);
}

/* ---- ALIAS16 -------------------------------------------------------------------------------------
 * Primitives into a 16-bit frame (CT16/CT16S) over a C32 target T (same base and width: 16-bit page n
 * is 32-bit page n) form an ALIAS16 run: the render thread gathers the run's rectangle of T's
 * halfwords into a staging texture in the frame's own pixel space, the run's draws are ordinary ROV
 * draws (any primitive class) on that texture in 16-bit mode, and the rectangle is scattered back. ROV
 * order makes overlapping writes exact; reads of T (CT16 or C32 views) come from T as it was before
 * the run, so a primitive that reads what an earlier one of the run wrote starts a new run (the
 * software renderer samples the state before each primitive). Hazards are decided on rectangles:
 * within one view (same base, width and PSM) the GS address mapping is a bijection, so disjoint pixel
 * rectangles are disjoint halfword sets; different views of overlapping pages count as a hazard. */
typedef struct { uint32_t bp, bw; int psm, w; int r[4]; uint64_t pages[8]; } arect;
#define AR_N 512
static arect AR[AR_N];

static void arect_make(arect* a, uint32_t bp, uint32_t bw, int psm, int w, int x0, int y0, int x1, int y1) {
    a->bp = bp; a->bw = bw; a->psm = psm; a->w = w;
    a->r[0] = x0; a->r[1] = y0; a->r[2] = x1; a->r[3] = y1;
    memset(a->pages, 0, sizeof a->pages);
    gs_rect_pages(psm, bp, bw, x0, y0, x1 + 1, y1 + 1, a->pages);
}

static int arect_overlap(const arect* a, const arect* b) {
    int pg = 0;
    for (int k = 0; k < 8; k++) pg |= (a->pages[k] & b->pages[k]) != 0;
    if (!pg) return 0;
    if (a->bp != b->bp || a->bw != b->bw || a->psm != b->psm) return 1;
    return !(a->r[2] < b->r[0] || b->r[2] < a->r[0] || a->r[3] < b->r[1] || b->r[3] < a->r[1]);
}

/* the 256-byte blocks a CT16 rectangle touches */
static int ct16_blocks(int psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1, uint16_t* out, int max) {
    const uint16_t* tab = psm == PSMCT16S ? gs_swz_ct16s : gs_swz_ct16;
    int n = 0;
    for (int y = y0 & ~7; y <= y1 && n < max; y += 8)
        for (int x = x0 & ~15; x <= x1 && n < max; x += 16)
            out[n++] = (uint16_t)(gs_addr16(tab, bp, bw, (uint32_t)x & 2047u, (uint32_t)y & 2047u) >> 7);
    return n;
}

static uint16_t ablk[GPU_BLOCKS];

/* C32 rows a 16-bit rectangle down to row y1 needs (a 16-bit page is 64 rows over a 32-row C32 page) */
static int rows16(int y1) { return (y1 / 64 + 1) * 32; }

/* a 16-bit rectangle's C32 pixel bounds (whole pages: conservative) */
static void rect16_to32(int x0, int y0, int x1, int y1, int r[4]) {
    r[0] = x0 & ~63; r[1] = (y0 / 64) * 32; r[2] = x1 | 63; r[3] = (y1 / 64) * 32 + 31;
}

/* ownership of a target view read: C32 / alpha views read T's pixels (texel = pixel), CT16 views the
 * blocks their rectangle covers */
static void view_read(const texsrc_t* ts) {
    gpu_target* T = ts->T;
    if (S.tpsm == PSMCT16 || S.tpsm == PSMCT16S) {
        own_grow(T, rows16(ts->v1));
        own_blocks(T, ablk, ct16_blocks(S.tpsm, S.tbp, S.tbw, ts->u0, ts->v0, ts->u1, ts->v1, ablk, GPU_BLOCKS), 0);
    } else {
        own_view_read(T, ts->u0, ts->v0, ts->u1, ts->v1);
    }
}

/* (perf-p2) ALIAS16 copy runs (gpu_fop_alias16.mode 1). A sprite whose every fragment is a function of one texel of a
 * CT16 view of a C32 target and of its own state - no bilinear filter or LOD, no fog, alpha test, DATE, Z, dither,
 * scan mask, and a blend that leaves Cs - writes each halfword it covers as (texel packed & ~FBMSK) | (old & FBMSK);
 * the render thread computes that for every sample of the draw's sprites in one compute dispatch (shaders/alias16.hlsl
 * cs_a16_copy, the shader's own U / V / texel / view formulas) and writes the bits straight into the target with atomic
 * AND / OR, so no staging copy, ROV draws or scatter are needed. Exact as the staging path when, inside one draw, no
 * sprite writes a halfword another writes or reads (the ROV draws order writes per pixel; a dispatch does not); reads of
 * what an earlier primitive of the run wrote already end the run (alias_prim). So a copy run's draws are cut where a
 * sprite's destination overlaps an earlier destination or source of the draw, or its source an earlier destination
 * (the hazard rectangles of the run, CB below), and at GPU_A16_COPY_MAX sprites; a sprite reading its own destination
 * rectangle, or any other primitive, makes the run an ordinary one. The Sly channel shuffles: 3 dispatches per frame. */
#define CB_N 128
static arect CB[CB_N];                   /* hazard rectangles of the open copy-run draw (destinations w = 1, sources w = 0) */
static int cb_n;
static uint32_t cb_owner = ~0u;          /* od_serial of the draw CB belongs to */
static arect cb_d, cb_s;                 /* the primitive being added (appended once its draw is known, fr_prim) */
static int cb_have_s, cb_pending;

static int a16_copy_prim(const gs_env_view* ev, int cls, const texsrc_t* ts) {
    const gs_prim* P = ev->P;
    return a16_copy_on && cls == GPU_PC_SPRITE && P->tme && ts->kind == TK_ALIAS16 && ts->T && !ts->punt &&
           P->texm == 1 && P->cur_level == 0 && !P->fge && !P->date && !S.zt && !P->dthe &&
           (!P->ate || P->atst == 1 || (P->atst == 0 && P->afail == 1)) &&   /* (colour written as passing: no Z here) */
           P->scanmsk < 2 && (!P->abe || (P->asel == P->bsel && P->dsel == 0)) && (S.tpsm == PSMCT16 || S.tpsm == PSMCT16S) &&
           (S.wms < 2 && S.wmt < 2);
}

/* an ALIAS16 primitive (pixel rect [x0,x1] x [y0,y1] of the 16-bit frame), after its ownership: the
 * run it belongs to (a new one at a read-after-write hazard) */
static void alias_prim(const gs_env_view* ev, const texsrc_t* ts, int x0, int y0, int x1, int y1, int copy_ok) {
    const gs_prim* P = ev->P;
    gpu_target* T = S.ct;
    arect d, s;
    arect_make(&d, P->fpage * 32, P->fbw, P->fpsm, 1, x0, y0, x1, y1);
    int have_s = P->tme != 0;
    if (have_s) arect_make(&s, S.tbp, S.tbw, S.tpsm, 0, ts->u0, ts->v0, ts->u1, ts->v1);
    int hazard = AB.open && (AB.T != T || AB.psm != P->fpsm || nar + 2 > AR_N);
    for (int i = 0; i < nar && !hazard && have_s; i++)
        if (AR[i].w && arect_overlap(&AR[i], &s)) hazard = 1;
    if (hazard) close_alias();
    int self = have_s && arect_overlap(&d, &s);
    if (self) gf.alias16_selfcopy++;     /* (in place: reads the state before the run) */
    if (!AB.open) {
        close_draw();
        close_run();
        gpu_fop_alias16* b = gf_cmd(GPU_FOP_ALIAS16, sizeof *b);
        b->target = T->id;
        b->psm = (uint16_t)P->fpsm;
        AB.begin_off = (uint32_t)((uint8_t*)b - A_cmd.p);
        AB.open = 1; AB.T = T; AB.psm = P->fpsm; AB.prims = 0;
        AB.bbox[0] = x0; AB.bbox[1] = y0; AB.bbox[2] = x1; AB.bbox[3] = y1;
        b->mode = 0; b->pad = 0;
        AB.copy = a16_copy_on;
        nar = 0;
    } else {
        if (x0 < AB.bbox[0]) AB.bbox[0] = x0;
        if (y0 < AB.bbox[1]) AB.bbox[1] = y0;
        if (x1 > AB.bbox[2]) AB.bbox[2] = x1;
        if (y1 > AB.bbox[3]) AB.bbox[3] = y1;
    }
    AR[nar++] = d;
    if (have_s) AR[nar++] = s;
    AB.prims++;
    gf.alias16_sprites++;
    /* (perf-p2) copy runs: this sprite's draw must hold no sprite it races with (above) */
    if (!copy_ok || self) AB.copy = 0;
    cb_pending = 0;
    if (AB.copy) {
        if (OD.open && OD.d.tier == GPU_TIER_ALIAS16 && cb_owner == od_serial) {
            int cut = OD.prims >= GPU_A16_COPY_MAX || cb_n + 2 > CB_N;
            for (int i = 0; i < cb_n && !cut; i++)
                if (arect_overlap(&CB[i], &d) || (have_s && CB[i].w && arect_overlap(&CB[i], &s))) cut = 1;
            if (cut) close_draw();
        }
        cb_d = d; cb_s = s; cb_have_s = have_s; cb_pending = 1;
    }
}

/* (perf-p2) after the primitive joined its draw (od_serial): its rectangles join the draw's copy-run hazard list */
static void alias_prim_drawn(void) {
    if (!cb_pending) return;
    cb_pending = 0;
    if (cb_owner != od_serial) { cb_n = 0; cb_owner = od_serial; }
    if (cb_n < CB_N) CB[cb_n++] = cb_d;
    if (cb_have_s && cb_n < CB_N) CB[cb_n++] = cb_s;
}

/* ---- tiled fills -> clears ------------------------------------------------------------------------ */
typedef struct {
    int active;
    gpu_target *ct, *zt;
    uint32_t color, z, wmask, zw;
    int64_t area;
    int x0, y0, x1, y1;
    int droppable;                       /* (M5) the fill's sprites opened the draw they are in */
    size_t start_cmd;                    /* the command arena when the first sprite's draw began */
    uint32_t prims;
} fill_acc;
static fill_acc FA;
static uint32_t zclamp(const gs_prim* P, int64_t z);

/* a recognised full clear: the FF draws of its sprites (nothing but them since start_cmd) are dropped,
 * the clear does the same (4.1, "tiled fill -> Clear"; REF mode keeps drawing them) */
static void fill_drop_draws(void) {
    if (!FA.droppable || gf_cfg.ref || !OD.open || OD.d.tier != GPU_TIER_FF || OD.prims > FA.prims) return;
    uint32_t n = 0;
    for (size_t pos = FA.start_cmd; pos < A_cmd.n;) {
        const gpu_fop* h = (const gpu_fop*)(A_cmd.p + pos);
        if (h->op != GPU_FOP_DRAW || ((const gpu_fop_draw*)h)->tier != GPU_TIER_FF) return;
        n++;
        pos += h->size;
    }
    A_cmd.n = FA.start_cmd;
    OD.open = 0;
    gf.d3d_draws -= n;
    gf.ff_draws -= n;
    gf.fill_draws_dropped += n + 1;
}

static void fill_add(const gs_env_view* ev, const gs_sprite* sp, const gs_vtx* v1) {
    const gs_prim* P = ev->P;
    gpu_target* ct = S.cwrite ? S.ct : NULL;
    gpu_target* zt = S.zwrite ? S.zt : NULL;
    uint32_t color = ((uint32_t)v1->r | (uint32_t)v1->g << 8 | (uint32_t)v1->b << 16 | (uint32_t)v1->a << 24) | (uint32_t)P->fba << 24;
    if (!FA.active || FA.ct != ct || FA.zt != zt || FA.color != color || FA.z != v1->z || FA.wmask != (uint32_t)S.wmask) {
        memset(&FA, 0, sizeof FA);
        FA.active = 1; FA.ct = ct; FA.zt = zt; FA.color = color; FA.z = v1->z; FA.wmask = (uint32_t)S.wmask;
        FA.x0 = sp->px0; FA.y0 = sp->py0; FA.x1 = sp->px1; FA.y1 = sp->py1;
        FA.droppable = OD.open && OD.prims == 1;   /* (this sprite opened the open draw) */
        FA.start_cmd = A_cmd.n;
    }
    FA.prims++;
    FA.area += (int64_t)(sp->px1 - sp->px0 + 1) * (sp->py1 - sp->py0 + 1);
    if (sp->px0 < FA.x0) FA.x0 = sp->px0;
    if (sp->py0 < FA.y0) FA.y0 = sp->py0;
    if (sp->px1 > FA.x1) FA.x1 = sp->px1;
    if (sp->py1 > FA.y1) FA.y1 = sp->py1;
    gpu_target* T = ct ? ct : zt;
    if (!T) return;
    int w = T->fbw * 64, h = P->scy1 + 1;
    if (h > T->rows) h = T->rows;
    if (FA.x0 == 0 && FA.y0 == 0 && FA.x1 >= w - 1 && FA.y1 >= h - 1 && FA.area == (int64_t)w * h) {
        fill_drop_draws();
        if (ct) {
            gpu_fop_clear* c = gf_cmd(GPU_FOP_CLEAR, sizeof *c);
            c->target = ct->id; c->value = color; c->mask = (uint32_t)S.wmask;
            c->x1 = (int16_t)w; c->y1 = (int16_t)h;
            gf.clears_c++;
        }
        if (zt) {
            gpu_fop_clear* c = gf_cmd(GPU_FOP_CLEAR, sizeof *c);
            c->target = zt->id; c->zclear = 1; c->value = zclamp(P, (int64_t)v1->z << 16);
            c->x1 = (int16_t)w; c->y1 = (int16_t)h;
            gf.clears_z++;
            zt->z_high = 0;
        }
        FA.active = 0;
    }
}

/* ---- the primitive hook ----------------------------------------------------------------------------- */
static int pc_of(int type) {
    switch (type) {
    case PRIM_POINT: return GPU_PC_POINT;
    case PRIM_LINE: case PRIM_LINESTRIP: return GPU_PC_LINEPIX;
    case PRIM_SPRITE: return GPU_PC_SPRITE;
    default: return GPU_PC_TRI;
    }
}

static int row_masked(const gs_prim* P, int py) { return (P->scanmsk == 2 && !(py & 1)) || (P->scanmsk == 3 && (py & 1)); }

/* (P1, notes/d3d11-renderer.md appendix P) the early-depth decision for a depth triangle drawn on the FF tier (its Z24 /
 * Z32 vertices are <= GPU_Z_FF_MAX: higher ones took the ROV tier before): 0 the vertex bound (Z + GPU_ZB_MARGIN) holds -
 * every vertex Z within the format and GPU_Z_FF_MAX and the plane no steeper than GPU_ZB_SLOPE per pixel; 1 no bound
 * (1.0), the exact Z still < 2^20 - Z16 (clamped to 0xFFFF), or a steep Z24 / Z32 plane whose integer Z (the Z record's
 * row + dy * rows + dx * px + the sub-sample step, int64 without overflow) stays below 2^20 at every sample inside the
 * triangle, at any scale (its affine extension at the three vertices, below); 2 that is not proven: the ROV tier (zpatch
 * holds any 32-bit Z). zp: the Z plane (gs_tri_plane of the vertices' Z). */
int gpu_front_zbound(const gs_tri* T, const gs_vtx* a, const gs_vtx* b, const gs_vtx* c, int zfi, gs_plane* zp) {
    int z16 = zfi == GPU_ZF_16 || zfi == GPU_ZF_16S;
    uint32_t lim = z16 ? 0xFFFFu : GPU_Z_FF_MAX;
    gs_tri_plane(T, zp, a->z, b->z, c->z);
    if (a->z <= lim && b->z <= lim && c->z <= lim && fabs(zp->cx) + fabs(zp->cy) <= GPU_ZB_SLOPE) return 0;
    if (z16) return 1;
    if (a->z > lim || b->z > lim || c->z > lim) return 2;      /* (the caller's z_high rule: never here) */
    int64_t row, dy, dx;
    gs_rowstep_attr(zp, T->by0, 65536.0, 1, &row, &dy, &dx);
    const int64_t SL = (int64_t)1 << 40, RL = (int64_t)1 << 52;   /* (products of < 2^12 rows / columns: no overflow) */
    if (dx > SL || dx < -SL || dy > SL || dy < -SL || row > RL || row < -RL) return 2;
    /* The pixel shader's Z16 at sample (i, j) of native pixel (px, py) is A(px, py) + F, A(x, y) = row + dy * (y - by0) +
     * dx * x exactly (no overflow here), F = floor(float(dy * j / ky + dx * i / kx)) = A(sample position) - A(px, py) up to
     * (|dx| + |dy|) * 2^-22 + 1. A is affine and every covered sample lies in the triangle, so Z16 <= max of A over the
     * three vertices (exact, in 1/16 pixel: |dx| * 2^16 < 2^57) + that rounding. */
    const int64_t X[3] = { T->x0, T->x1, T->x2 }, Y[3] = { T->y0, T->y1, T->y2 };
    int64_t amax = INT64_MIN;
    for (int k = 0; k < 3; k++) {
        int64_t a16 = row * 16 + dy * (Y[k] - 16 * (int64_t)T->by0) + dx * X[k];
        if (a16 > amax) amax = a16;
    }
    int64_t ad = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    int64_t zmax16 = (amax >> 4) + 1 + (ad >> 20) + 2;
    return zmax16 < ((int64_t)GPU_ZE_ONE << 16) ? 1 : 2;
}

static uint32_t zclamp(const gs_prim* P, int64_t z) {
    int64_t zi = z >> 16;
    return zi < 0 ? 0 : zi > (int64_t)P->zmax ? P->zmax : (uint32_t)zi;
}

/* the sw DDA pixel by pixel (gs_line_step): one line-pixel record per covered pixel inside the
 * scissor, appended to the records; attribute values as gs_span_start_at computes them at (t, 0),
 * only those the draw uses (the others are constant or unused). Returns the number of records. */
static uint32_t emit_linepix(const gs_env_view* ev, const gs_line* L, const gs_vtx* a, const gs_vtx* b, int aa, uint32_t state) {
    const gs_prim* P = ev->P;
    gs_planes pl;
    gs_line_planes(a, b, ev->iip, &pl);
    int need_z = S.zfi >= 0, need_c = ev->iip, need_f = P->fge, need_t = P->tme;
    uint32_t rgba0 = (uint32_t)clamp_byte(b->r) | (uint32_t)clamp_byte(b->g) << 8 | (uint32_t)clamp_byte(b->b) << 16 |
                     (uint32_t)clamp_byte(b->a) << 24;
    size_t maxn = (size_t)(L->p1 - L->p0 + 1) * 2;
    linerec* r0 = (linerec*)ar_reserve(&A_rec, maxn * sizeof(linerec));
    linerec* r = r0;
    const double y = 0;
#define LEV(f) (pl.f.c0 + pl.f.cx * t + pl.f.cy * y)
    for (int p = L->p0; p <= L->p1; p++) {
        int q, qn, cov[2];
        double t = gs_line_step(L, p, aa, &q, cov, &qn);
        for (int k = 0; k < qn; k++) {
            if (!cov[k]) continue;
            int px = L->xmajor ? p : q + k, py = L->xmajor ? q + k : p;
            if (px < P->scx0 || px > P->scx1 || py < P->scy0 || py > P->scy1 || row_masked(P, py)) continue;
            r->px = (int16_t)px; r->py = (int16_t)py;
            r->z = need_z ? zclamp(P, gs_d2i64(LEV(z) * 65536.0)) : 0;
            uint32_t rgba = rgba0;
            if (need_c)
                rgba = (uint32_t)clamp_byte(gs_d2i32(LEV(r) * 65536.0) >> 16) |
                       (uint32_t)clamp_byte(gs_d2i32(LEV(g) * 65536.0) >> 16) << 8 |
                       (uint32_t)clamp_byte(gs_d2i32(LEV(b) * 65536.0) >> 16) << 16 |
                       (uint32_t)clamp_byte(gs_d2i32(LEV(a) * 65536.0) >> 16) << 24;
            if (aa) rgba = (rgba & 0xFFFFFFu) | (uint32_t)clamp_byte(cov[k]) << 24;
            r->rgba = rgba;
            r->fog_state = (uint32_t)(need_f ? clamp_byte(gs_d2i32(LEV(f) * 65536.0) >> 16) : clamp_byte(b->f)) | state << 8;
            r->s = r->t = r->q = 0;
            r->lod_q = b->q;                 /* (gs_draw.c draw_line: P.lod_q = v1->q) */
            if (need_t) {
                if (P->fst) {                /* exact integers (1/16 texel << 12 exceeds a float's mantissa) */
                    r->s = (uint32_t)gs_d2i32(LEV(u) * (double)(1 << GS_UVSH));
                    r->t = (uint32_t)gs_d2i32(LEV(v) * (double)(1 << GS_UVSH));
                } else { r->s = f2u((float)LEV(s)); r->t = f2u((float)LEV(t)); r->q = f2u((float)LEV(q)); }
            }
            r++;
        }
    }
#undef LEV
    uint32_t n = (uint32_t)(r - r0);
    A_rec.n -= (maxn - n) * sizeof(linerec);
    return n;
}

/* (M7) outlines = thin at scale > 1: the line as one analytic record (gpu_rec_thin): its covered major-axis
 * range and minor-axis position as the DDA computes them, its attribute planes, and a width in native pixels
 * of PS2_GS_THIN_WIDTH samples across the minor axis (default half the scale, at least one sample; T: the
 * draw's target, whose factors set the sample size) */
typedef gpu_rec_thin thinrec;
_Static_assert(sizeof(thinrec) == 128, "thin line record (shaders/gs.hlsl PRIM 3)");

static float thin_width_samples(void) {
    static float env = -1.0f;
    if (env < 0.0f) {
        const char* e = getenv("PS2_GS_THIN_WIDTH");
        env = e && atof(e) > 0.0 ? (float)atof(e) : 0.0f;
    }
    if (env > 0.0f) return env;
    float s = (float)(gf_cfg.scale > 0 ? gf_cfg.scale : 1) * 0.5f;
    return s < 1.0f ? 1.0f : s;
}

static void emit_thin(const gs_env_view* ev, const gs_line* L, const gs_vtx* a, const gs_vtx* b, int aa, uint32_t state,
                      const gpu_target* T) {
    const gs_prim* P = ev->P;
    gs_planes pl;
    gs_line_planes(a, b, ev->iip, &pl);
    thinrec* r = ar_alloc(&A_rec, sizeof *r);
    int64_t a1 = L->xmajor ? L->X1 : L->Y1;
    int64_t lo = L->a0 < a1 ? L->a0 : a1, hi = L->a0 < a1 ? a1 : L->a0;
    r->lo = (float)lo; r->hi = (float)hi;
    r->a0 = (float)L->a0; r->len = (float)L->len;
    r->o0 = (float)(L->xmajor ? L->Y0 : L->X0);
    r->od = (float)(L->xmajor ? L->dy : L->dx);
    r->flags = (L->xmajor ? 1u : 0u) | (aa ? 2u : 0u) | (ev->iip ? 4u : 0u) | (P->fst ? 8u : 0u);
    r->state = state;
    r->zc0 = (float)pl.z.c0; r->zcx = (float)pl.z.cx;
    if (ev->iip) {
        const gs_plane* c[4] = { &pl.r, &pl.g, &pl.b, &pl.a };
        for (int k = 0; k < 4; k++) { r->cc0[k] = (float)c[k]->c0; r->ccx[k] = (float)c[k]->cx; }
    } else {
        int v[4] = { b->r, b->g, b->b, b->a };
        for (int k = 0; k < 4; k++) { r->cc0[k] = (float)clamp_byte(v[k]); r->ccx[k] = 0.0f; }
    }
    if (P->fge) { r->fc0 = (float)pl.f.c0; r->fcx = (float)pl.f.cx; }
    else { r->fc0 = (float)clamp_byte(b->f); r->fcx = 0.0f; }
    if (P->tme) {
        if (P->fst) {
            r->sc0[0] = (float)pl.u.c0; r->scx[0] = (float)pl.u.cx; r->sc0[1] = (float)pl.v.c0; r->scx[1] = (float)pl.v.cx;
        } else {
            r->sc0[0] = (float)pl.s.c0; r->scx[0] = (float)pl.s.cx; r->sc0[1] = (float)pl.t.c0; r->scx[1] = (float)pl.t.cx;
            r->sc0[2] = (float)pl.q.c0; r->scx[2] = (float)pl.q.cx;
        }
    }
    r->lod_q = b->q;
    int k = T ? (L->xmajor ? T->ky : T->kx) : (gf_cfg.scale > 0 ? gf_cfg.scale : 1);
    r->w = thin_width_samples() / (float)(k > 0 ? k : 1);
    if (r->w > 1.0f) r->w = 1.0f;        /* (never wider than the PS2's own line) */
}

/* ---- the exact ATST split (M5) ----------------------------------------------------------------------
 * The two-draw split of 4.2 (passing fragments first, then the failing ones) reorders fragments only where
 * a failing fragment of a primitive is followed, at the same pixel, by a passing fragment of a later
 * primitive of the same draw (a passing fragment followed by a failing one keeps its order: the pass draw
 * comes first). So the open split draw keeps a bitmap of the pixels its fail-capable primitives cover (the
 * software renderer's spans, word operations per row); a pass-capable primitive touching one of them starts
 * a new draw. M5 measured the plain split visibly different from REF (ep6, ep2 hub, ep1 hub, m1 brawl) and
 * the ROV escalation slower than REF in the hubs (UAV draws and FF <-> UAV switches). At scale > 1 a
 * triangle's samples can reach native pixels outside its sample-0 spans: the spans are widened by a pixel. */
#define SB_W 32                                   /* 64-bit words per row: 2048 pixels */
static uint64_t sbits[2048][SB_W];
static int sb_y0 = 2048, sb_y1 = -1, sb_w0 = SB_W, sb_w1 = -1;
static uint32_t sb_owner;                         /* the open draw the bitmap belongs to (od_serial) */

/* (M11) no rasterizer-ordered views (gf_cfg.no_rov): a ROV-tier or ALIAS16 draw is a plain UAV draw, whose pixel-shader
 * invocations run in no particular order, so it may only hold primitives that touch disjoint pixels; D3D11 orders the
 * UAV accesses of consecutive draws, which gives every pixel its primitives in stream order, as the ROV draws do. The
 * open draw's primitive rectangles (native or 16-bit frame pixels, inclusive; triangles and thin lines grown by one
 * pixel on every side: the samples of a triangle at scale reach into the row above and the column left, thin lines are
 * quads around the line) are kept here; an overlapping primitive starts a new draw of the same key in the same run. */
#define NR_MAX 256
static int16_t nr_rect[NR_MAX][4];
static int nr_n, nr_box[4];
static uint32_t nr_owner;
static int nr_hit(const int r[4]) {
    if (nr_n >= NR_MAX) return 1;
    if (!nr_n || r[2] < nr_box[0] || r[0] > nr_box[2] || r[3] < nr_box[1] || r[1] > nr_box[3]) return 0;
    for (int i = 0; i < nr_n; i++)
        if (!(r[2] < nr_rect[i][0] || r[0] > nr_rect[i][2] || r[3] < nr_rect[i][1] || r[1] > nr_rect[i][3])) return 1;
    return 0;
}
static void nr_add(const int r[4]) {
    if (!nr_n) for (int k = 0; k < 4; k++) nr_box[k] = r[k];
    else {
        if (r[0] < nr_box[0]) nr_box[0] = r[0];
        if (r[1] < nr_box[1]) nr_box[1] = r[1];
        if (r[2] > nr_box[2]) nr_box[2] = r[2];
        if (r[3] > nr_box[3]) nr_box[3] = r[3];
    }
    for (int k = 0; k < 4; k++) nr_rect[nr_n][k] = (int16_t)(r[k] < -32768 ? -32768 : r[k] > 32767 ? 32767 : r[k]);
    nr_n++;
}

static void sb_reset(void) {
    for (int y = sb_y0; y <= sb_y1; y++) memset(&sbits[y][sb_w0], 0, (size_t)(sb_w1 - sb_w0 + 1) * 8);
    sb_y0 = 2048; sb_y1 = -1; sb_w0 = SB_W; sb_w1 = -1;
}

/* pixels [xa, xb] of row y: set (set = 1) or test; returns 1 when a tested pixel is set */
static int sb_span(int y, int xa, int xb, int set) {
    if (y < 0 || y > 2047) return 0;
    if (xa < 0) xa = 0;
    if (xb > 2047) xb = 2047;
    if (xa > xb) return 0;
    int wa = xa >> 6, wb = xb >> 6;
    if (set) {
        if (y < sb_y0) sb_y0 = y;
        if (y > sb_y1) sb_y1 = y;
        if (wa < sb_w0) sb_w0 = wa;
        if (wb > sb_w1) sb_w1 = wb;
    }
    for (int w = wa; w <= wb; w++) {
        int lo = w == wa ? xa & 63 : 0, hi = w == wb ? xb & 63 : 63;
        uint64_t m = (hi == 63 ? ~0ull : (1ull << (hi + 1)) - 1) & ~((1ull << lo) - 1);
        if (set) sbits[y][w] |= m;
        else if (sbits[y][w] & m) return 1;
    }
    return 0;
}

/* the primitive's pixels (triangle spans, sprite rectangle, line / point pixel records): set or test */
static int sb_prim(int cls, const gs_tri* T, const gs_sprite* sp, size_t lp_start, uint32_t lp_n, int px, int py, int set) {
    int d = gf_cfg.scale > 1;                     /* (widened at scale > 1) */
    /* (M9) a test outside the rectangle of the bits set so far cannot hit */
    int qx0 = (cls == GPU_PC_TRI ? T->bx0 : cls == GPU_PC_SPRITE ? sp->px0 : px) - d - 1;
    int qx1 = (cls == GPU_PC_TRI ? T->bx1 : cls == GPU_PC_SPRITE ? sp->px1 : px) + d + 1;
    int qy0 = (cls == GPU_PC_TRI ? T->by0 : cls == GPU_PC_SPRITE ? sp->py0 : py) - d - 1;
    int qy1 = (cls == GPU_PC_TRI ? T->by1 : cls == GPU_PC_SPRITE ? sp->py1 : py) + d + 1;
    if (!set && cls != GPU_PC_LINEPIX && (qy1 < sb_y0 || qy0 > sb_y1 || (qx1 >> 6) < sb_w0 || (qx0 >> 6) > sb_w1)) return 0;
    if (cls == GPU_PC_TRI) {
        gs_edge e[3];
        gs_tri_edges(T, e);
        int hit = 0;
        if (!d) {
            for (int y = T->by0; y <= T->by1; y++, gs_tri_edges_next(e)) {
                int64_t xl, xr;
                gs_tri_row(e, T->bx0, T->bx1, &xl, &xr);
                if (xl > xr) continue;
                hit |= sb_span(y, (int)xl, (int)xr, set);
                if (hit && !set) return 1;
            }
            return hit;
        }
        /* (M9) at scale: bitmap row y takes the hull of the spans of rows y - 1, y, y + 1, widened by a pixel - one
         * span per row instead of three (the hull covers at least their union: conservative, so still exact) */
        int alo = 1 << 30, ahi = -1;              /* the span of triangle row q - 1 (empty: lo > hi) */
        int blo = 1 << 30, bhi = -1;              /* of row q */
        for (int q = T->by0 - 1; q <= T->by1 + 1; q++) {
            int clo = 1 << 30, chi = -1;          /* of row q + 1 (rows by0 .. by1 in order, edges stepped with them) */
            if (q + 1 <= T->by1) {
                int64_t xl, xr;
                gs_tri_row(e, T->bx0, T->bx1, &xl, &xr);
                gs_tri_edges_next(e);
                if (xl <= xr) { clo = (int)xl; chi = (int)xr; }
            }
            int lo = alo < blo ? alo : blo, hi = ahi > bhi ? ahi : bhi;
            if (clo < lo) lo = clo;
            if (chi > hi) hi = chi;
            if (hi >= lo) {
                hit |= sb_span(q, lo - 1, hi + 1, set);
                if (hit && !set) return 1;
            }
            alo = blo; ahi = bhi;
            blo = clo; bhi = chi;
        }
        return hit;
    }
    if (cls == GPU_PC_SPRITE) {
        for (int y = sp->py0 - d; y <= sp->py1 + d; y++)
            if (sb_span(y, sp->px0 - d, sp->px1 + d, set) && !set) return 1;
        return 0;
    }
    if (cls == GPU_PC_LINEPIX) {
        const linerec* r = (const linerec*)(A_rec.p + lp_start);
        for (uint32_t i = 0; i < lp_n; i++)
            for (int k = -d; k <= d; k++)
                if (sb_span(r[i].py + k, r[i].px - d, r[i].px + d, set) && !set) return 1;
        return 0;
    }
    for (int k = -d; k <= d; k++)
        if (sb_span(py + k, px - d, px + d, set) && !set) return 1;
    return 0;
}

/* lockstep checkpoints (gpu_front_set_checkpoint_hook) */
static void (*cp_hook)(uint32_t index, int frame_changed, const void* ev, int type, const void* const* v, void* user);
static void* cp_user;
static uint64_t cp_frame = ~0ull, cp_zbuf = ~0ull;
static uint32_t cp_vblank;               /* the VBlank the frame being built ends with (last + 1) */

/* STQ triangles: the software rasterizer steps S/T/Q in floats along a span and restarts each piece
 * of it with s + ds * take (gs_draw_pixel.c gs_batch_span): pieces end at 64-pixel columns, which the
 * pixel shader finds itself, and where the primitive's batch is full (2048 pixels), which depends on
 * every earlier row. The rows with such a cut and its x go into a table (gpu_rec_tri.cuts). */
static void stq_cuts(const gs_tri* T, const gs_prim* P, fullrec* r) {
    static int16_t cut[2 * 4096];
    gs_edge e[3];
    gs_tri_edges(T, e);
    int nc = 0, nseg = 0, npix = 0;
    for (int py = T->by0; py <= T->by1; py++, gs_tri_edges_next(e)) {
        if (P->scanmsk >= 2 && row_masked(P, py)) continue;
        int64_t xl, xr;
        gs_tri_row(e, T->bx0, T->bx1, &xl, &xr);
        if (xl > xr) continue;
        int x0 = (int)xl, n = (int)(xr - xl + 1);
        while (n > 0) {
            if (nseg == 512 || npix == GS_MAX_SPAN) nseg = npix = 0;     /* (gs_draw_pixel.c MAX_SEGS 512) */
            int col = 64 - (x0 & 63), room = GS_MAX_SPAN - npix;
            int take = col < room ? col : room;
            if (n < take) take = n;
            if (take < (col < n ? col : n) && nc < 4096) { cut[2 * nc] = (int16_t)py; cut[2 * nc + 1] = (int16_t)(x0 + take); nc++; }
            nseg++;
            npix += take;
            n -= take;
            x0 += take;
        }
    }
    if (nc) {
        r->cuts = gf_data(cut, (uint32_t)nc * 4);
        r->ncuts = (uint32_t)nc;
        gf.stq_cuts++;
    }
}

/* ---- PUNT (5.4) ---------------------------------------------------------------------------------- */
static int (*mem_reader)(uint8_t* out, void* user);
static void* mem_reader_user;
static uint8_t* punt_mem;
static volatile LONG thread_punts;       /* PUNTs met on the frontend thread (gpu_front_thread_punts) */
static int force_every = -1, force_mask;
static uint32_t force_ctr;
static struct { int on, cpsm, zpsm; uint32_t cbp, cbw, zbp, zbw; int r[4]; } PP;   /* the PUNT being drawn */

void gpu_front_set_mem_reader(int (*reader)(uint8_t* out4mb, void* user), void* user) { mem_reader = reader; mem_reader_user = user; }

static int gf_force_punt(const gs_env_view* ev, int cls) {
    if (force_every < 0) {
        const char* e = getenv("PS2_GPU_PUNT");
        force_every = 0;
        force_mask = 15;
        if (e && *e) sscanf(e, "%d:%d", &force_every, &force_mask);
    }
    if (force_every <= 0) return 0;      /* (M6: also on the frontend thread: tests of its PUNT fallback) */
    if (!(force_mask & (1 << cls))) return 0;
    if ((force_mask & 16) && ev->P->ffmt->bits != 16) return 0;
    return ++force_ctr % (uint32_t)force_every == 0;
}

/* the software rasterizer draws it: the GPU data it reads or writes goes to vram_buf (download) and
 * from there into gs_vram (read back), gs_draw.c draws it into gs_vram, fr_sw_done patches the written
 * blocks back (they are CPU data now: every target reloads them, replicated to all samples) */
static void punt_prim(const gs_env_view* ev, int cls, const texsrc_t* ts, int x0, int y0, int x1, int y1) {
    const gs_prim* P = ev->P;
    (void)cls;
    flush_open();
    if (gf_cfg.thread) {                 /* the frontend thread cannot make the decode thread draw: counted, not drawn;
                                            the integrator moves the frontend to the GS thread (gpu_hw.c, M6) */
        InterlockedIncrement(&thread_punts);
        return;
    }
    if (y1 >= 2048) y1 = 2047;
    if (x1 >= 2048) x1 = 2047;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int cw = S.cwrite || S.reads_dest || P->date, zw = S.zfi >= 0;
    if (cw) own_mem_read(P->fpsm, P->fpage * 32, P->fbw, x0, y0, x1, y1);
    if (zw) own_mem_read(P->zpsm, P->zpage * 32, P->zbw, x0, y0, x1, y1);
    if (P->tme) own_mem_read(S.tpsm, S.tbp, S.tbw, ts->u0, ts->v0, ts->u1, ts->v1);
    if (mem_reader) {
        gpu_front_checkpoint();          /* everything so far to the render thread */
        if (!punt_mem && !(punt_mem = malloc(GS_VRAM_SIZE))) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
        if (mem_reader(punt_mem, mem_reader_user)) {
            int d = own_readback_apply(punt_mem, !gf_cfg.sw);
            if (d) gf.punt_why[GPU_PUNT_READBACK_DIFF]++;
        }
    }
    PP.on = 1;
    PP.cpsm = S.cwrite ? P->fpsm : -1; PP.cbp = P->fpage * 32; PP.cbw = P->fbw;
    PP.zpsm = S.zwrite ? P->zpsm : -1; PP.zbp = P->zpage * 32; PP.zbw = P->zbw;
    PP.r[0] = x0; PP.r[1] = y0; PP.r[2] = x1; PP.r[3] = y1;
    gs_front_swdraw = 1;
    if (trace_on) fprintf(stderr, "[front] PUNT prim %u (%d,%d)-(%d,%d) FRAME psm %02X\n", gf.kicks - 1, x0, y0, x1, y1, P->fpsm);
}

static void fr_sw_done(void) {
    if (!PP.on) return;
    PP.on = 0;
    /* (the rect's blocks were all made current in gs_vram first: patching the whole rect is exact) */
    if (PP.cpsm >= 0) own_upload(PP.cpsm, PP.cbp, PP.cbw, PP.r[0], PP.r[1], PP.r[2] + 1, PP.r[3] + 1);
    if (PP.zpsm >= 0) own_upload(PP.zpsm, PP.zbp, PP.zbw, PP.r[0], PP.r[1], PP.r[2] + 1, PP.r[3] + 1);
    own_flush_patches();                 /* (now: gs_vram holds the state right after this primitive) */
}

#ifdef GPU_FRONT_PROF                    /* section profile of fr_prim (build with -DGPU_FRONT_PROF) */
static uint64_t prof[12], prof_t;
static int prof_emit;
#define PROF_MARK(i) do { uint64_t n_ = __rdtsc(); prof[i] += n_ - prof_t; prof_t = n_; } while (0)
#define PROF_START() (prof_t = __rdtsc())
#else
#define PROF_MARK(i) do { } while (0)
#define PROF_START() do { } while (0)
#endif
/* (M10) HUD transform statistics (gpu_front_hud_stats) */
static gpu_hud_stats hud_st;
static void hud_stats_reset(void) {
    memset(&hud_st, 0, sizeof hud_st);
    for (int i = 0; i < 3; i++) { hud_st.x0[i] = 1 << 30; hud_st.x1[i] = -(1 << 30); }
    hud_st.scale_min = 1e9f;
    hud_st.scale_max = -1e9f;
}
void gpu_front_hud_stats(gpu_hud_stats* out) { *out = hud_st; }

static void fr_prim(const gs_env_view* ev, int type, const gs_vtx* a, const gs_vtx* b, const gs_vtx* c) {
    /* time every 16th primitive (rdtsc costs about as much as a cheap primitive) */
    int timed = (gf.kicks & 15) == 0;
    uint64_t t0 = timed ? __rdtsc() : 0;
    PROF_START();
    PCOUNT(n_prims);
    const gs_prim* P = ev->P;
    /* (M10, 8.3) the HUD transform: between HUD_BEGIN and HUD_END (the StateRec's hint state, ev->hud) the game's
     * vertices are scaled about the anchor (left edge, centre, right edge of the FRAME width), full-width
     * primitives exempt - gs_hud_prim, the software renderer's rule. Positions and every record (Z rows, sprite
     * rects, edges, line pixels) are computed from the transformed vertices, so sample 0 stays exact. */
    gs_vtx hv[3];
    if (ev->hud & GS_ENV_HUD_ON) {
        int n = type == PRIM_POINT ? 1 : type == PRIM_TRI || type == PRIM_TRISTRIP || type == PRIM_TRIFAN ? 3 : 2;
        const gs_vtx* in[3] = { a, b, c };
        if (gs_hud_prim(in, n, ev->reg[R_XYOFFSET_1 + ev->ctx], ev->reg[R_FRAME_1 + ev->ctx], ev->hud, ev->hud_scale, hv)) {
            a = &hv[0];
            b = n > 1 ? &hv[1] : NULL;
            c = n > 2 ? &hv[2] : NULL;
            int an = (int)(ev->hud >> 1) & 3;
            if (an < 3) {                /* (statistics: gpu_front_hud_stats) */
                hud_st.prims[an]++;
                for (int i = 0; i < n; i++) {
                    int32_t x = (hv[i].x - (int32_t)(ev->reg[R_XYOFFSET_1 + ev->ctx] & 0xFFFF)) >> 4;
                    if (x < hud_st.x0[an]) hud_st.x0[an] = x;
                    if (x > hud_st.x1[an]) hud_st.x1[an] = x;
                }
                hud_st.frame_w = (uint32_t)((ev->reg[R_FRAME_1 + ev->ctx] >> 16) & 0x3F) * 64;
                float s;
                memcpy(&s, &ev->hud_scale, 4);
                if (s < hud_st.scale_min) hud_st.scale_min = s;
                if (s > hud_st.scale_max) hud_st.scale_max = s;
            }
        } else hud_st.full++;
    }
    if (cp_hook && ev->draws) {          /* lockstep: compare before this primitive */
        uint64_t fr = ev->reg[R_FRAME_1 + ev->ctx], zb = ev->reg[R_ZBUF_1 + ev->ctx];
        int changed = fr != cp_frame || zb != cp_zbuf;
        cp_frame = fr;
        cp_zbuf = zb;
        const void* vv[3] = { a, b, c };
        cp_hook(gf.kicks, changed, ev, type, vv, cp_user);
    }
    gf.kicks++;
    if (!ev->draws) { gf.nowrite++; goto out; }
    if (S.version != ev->version || S.ctx != ev->ctx || !S.version) state_lookup(ev);
    if (S.skip) { gf.skipped++; goto out; }
    int cls = pc_of(type);

    PROF_MARK(0);
    /* ---- geometry: pixel rectangle, culling (the sw rules) ---- */
    gs_tri T;
    gs_sprite sp;
    gs_line L;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    const gs_vtx* vs[3] = { a, b, c };
    int nv = 1;
    switch (cls) {
    case GPU_PC_TRI:
        nv = 3;
        if (!gs_tri_setup(&T, a, b, c, ev->ofx, ev->ofy, P->scx0, P->scx1, P->scy0, P->scy1)) { gf.culled++; goto out; }
        x0 = T.bx0; y0 = T.by0; x1 = T.bx1; y1 = T.by1;
        break;
    case GPU_PC_SPRITE:
        nv = 2;
        if (!gs_sprite_setup(&sp, a, b, ev->ofx, ev->ofy, P->tme, P->fst, P->tw16, P->th16, P->scx0, P->scx1, P->scy0, P->scy1)) {
            gf.culled++; goto out;
        }
        x0 = sp.px0; y0 = sp.py0; x1 = sp.px1; y1 = sp.py1;
        break;
    case GPU_PC_LINEPIX:
        nv = 2;
        if (!gs_line_setup(&L, a, b, ev->ofx, ev->ofy)) { gf.culled++; goto out; }
        x0 = (int)((L.X0 < L.X1 ? L.X0 : L.X1) >> 4) - 1; x1 = (int)((L.X0 < L.X1 ? L.X1 : L.X0) >> 4) + 1;
        y0 = (int)((L.Y0 < L.Y1 ? L.Y0 : L.Y1) >> 4) - 1; y1 = (int)((L.Y0 < L.Y1 ? L.Y1 : L.Y0) >> 4) + 1;
        if (x0 < P->scx0) x0 = P->scx0;
        if (y0 < P->scy0) y0 = P->scy0;
        if (x1 > P->scx1) x1 = P->scx1;
        if (y1 > P->scy1) y1 = P->scy1;
        if (x0 > x1 || y0 > y1) { gf.culled++; goto out; }
        break;
    default: {
        int px, py;
        gs_point_pixel(a, ev->ofx, ev->ofy, &px, &py);
        if (px < P->scx0 || px > P->scx1 || py < P->scy0 || py > P->scy1 || row_masked(P, py)) { gf.culled++; goto out; }
        x0 = x1 = px; y0 = y1 = py;
        break;
    }
    }
    gf.cls[cls]++;

    PROF_MARK(1);
    /* ---- per-primitive inputs: alpha range, Z range ---- */
    int aa = cls == GPU_PC_LINEPIX && ev->aa1 && P->abe;
    int va_lo = 255, va_hi = 0;
    if (aa) { va_lo = 0; va_hi = 128; }
    else if (ev->iip && nv > 1) {
        for (int i = 0; i < nv; i++) { if (vs[i]->a < va_lo) va_lo = vs[i]->a; if (vs[i]->a > va_hi) va_hi = vs[i]->a; }
    } else {
        int av = vs[nv - 1]->a;
        va_lo = va_hi = av;
    }
    va_lo = clamp_byte(va_lo); va_hi = clamp_byte(va_hi);
    int as_lo = va_lo, as_hi = va_hi;
    if (P->tme && P->tcc) {
        switch (P->tfx) {
        case 0: as_lo = (va_lo * S.tamin) >> 7; as_hi = (va_hi * S.tamax) >> 7; break;
        case 2: as_lo = S.tamin + va_lo; as_hi = S.tamax + va_hi; break;
        default: as_lo = S.tamin; as_hi = S.tamax; break;
        }
        as_lo = clamp_byte(as_lo); as_hi = clamp_byte(as_hi);
    }
    int z32_high = 0;
    if (S.zfi >= 0) {
        uint32_t zl = 0xFFFFFFFFu, zh = 0;
        int nz = cls == GPU_PC_SPRITE ? 1 : nv;
        const gs_vtx* const* zv = cls == GPU_PC_SPRITE ? &vs[1] : vs;
        for (int i = 0; i < nz; i++) { if (zv[i]->z < zl) zl = zv[i]->z; if (zv[i]->z > zh) zh = zv[i]->z; }
        int f = S.zfi;
        if (!gf.zprims[f] || zl < gf.zmin[f]) gf.zmin[f] = zl;
        if (!gf.zprims[f] || zh > gf.zmax[f]) gf.zmax[f] = zh;
        gf.zprims[f]++;
        /* (P1) beyond the DSV's exact range (gpu_front.h): the ROV tier, zpatch as the target's depth until a full clear */
        if ((f == GPU_ZF_32 || f == GPU_ZF_24) && zh > GPU_Z_FF_MAX) { z32_high = 1; gf.z32_over++; if (S.zt) S.zt->z_high = 1; }
    }

    PROF_MARK(2);
    /* ---- texture source ---- */
    texsrc_t ts;
    memset(&ts, 0, sizeof ts);
    if (P->tme) {
        int u0, v0, u1, v1;
        if (cls == GPU_PC_SPRITE) {
            double su[2] = { (double)(sp.ua >> GS_UVSH), (double)(sp.ub >> GS_UVSH) }, sv[2] = { (double)(sp.va >> GS_UVSH), (double)(sp.vb >> GS_UVSH) };
            texel_rect(ev, NULL, 2, su, sv, &u0, &v0, &u1, &v1);
        } else {
            texel_rect(ev, vs, nv, NULL, NULL, &u0, &v0, &u1, &v1);
        }
        tex_source(ev, u0, v0, u1, v1, &ts);
        if (S.wms >= 2 || S.wmt >= 2) gf.region_clamp++;
        if (S.tbw & (S.tbw - 1)) gf.tbw_npot++;
    }

    /* (M9) a tighter source-alpha bound where the palette's decides a tier or an ATST split: the palette entries the
     * texture's texels use (tex_used_alpha) */
    int as_hi_pal = as_hi;               /* (the palette bound: the As > 128 rule with PS2_GPU_NO_TEX_ALPHA=2) */
    if (P->tme && P->tcc && S.indexed && ts.kind == TK_VRAM_IDX && !ts.punt && gf_cfg.no_tex_alpha != 1 &&
        ((S.cd_dep && S.c_as && as_hi > 128 && !gf_cfg.no_tex_alpha) ||
         (S.fatst == FATST_SPLIT && atst_count(S.atst, S.aref, as_lo, as_hi) % (as_hi - as_lo + 1) != 0))) {
        tex_used_alpha(ev);
        if (S.tu_lo <= S.tu_hi && (S.tu_lo > S.tamin || S.tu_hi < S.tamax)) {
            switch (P->tfx) {
            case 0: as_lo = (va_lo * S.tu_lo) >> 7; as_hi = (va_hi * S.tu_hi) >> 7; break;
            case 2: as_lo = S.tu_lo + va_lo; as_hi = S.tu_hi + va_hi; break;
            default: as_lo = S.tu_lo; as_hi = S.tu_hi; break;
            }
            as_lo = clamp_byte(as_lo); as_hi = clamp_byte(as_hi);
            gf.tex_alpha_refined++;
        }
    }

    PROF_MARK(3);
    /* ---- tier ---- */
    int tier = GPU_TIER_FF, why = -1, punt = S.punt, zb = -1;
    gs_plane zbp;                        /* (P1) the Z plane gpu_front_zbound computed (zb >= 0), reused by the record */
    if (!punt && ts.punt) punt = ts.punt;
    if (!punt && gf_force_punt(ev, cls)) punt = GPU_PUNT_FORCED + 1;
    int outcome = 0;
    if (S.fatst == FATST_SPLIT) {
        int n = atst_count(S.atst, S.aref, as_lo, as_hi);
        if (gf_cfg.selfcheck) {          /* against the sw alpha test table */
            int m = 0;
            for (int x = as_lo; x <= as_hi; x++) m += P->apass[x];
            if (m != n) gf.selfcheck_bad++;
        }
        outcome = n == as_hi - as_lo + 1 ? 1 : n == 0 ? 2 : 4;
        gf.atst_kicks++;
        if (outcome == 1) gf.atst_pass++; else if (outcome == 2) gf.atst_fail++; else gf.atst_straddle++;
    }
    if (S.cd_dep && S.c_as) { gf.cd_as_kicks++; if (as_hi > 128) gf.as_over128++; }
    if (punt) tier = GPU_TIER_PUNT;
    else if (S.alias) tier = GPU_TIER_ALIAS16;
    else {
        if (S.rov) why = S.rov - 1;
        else if (S.cd_dep && S.c_as && (gf_cfg.no_tex_alpha == 2 ? as_hi_pal : as_hi) > 128) why = GPU_ROV_C_OVER128;
        else if (dual_exact && S.cd_dep &&       /* (M8 measurement: PS2_GPU_DUAL_EXACT=1 partial-alpha dual-source blends exact) */
                 (S.c_as ? !(as_lo == as_hi && (as_lo == 0 || as_lo == 128)) : (P->fix != 0 && P->fix != 128))) why = GPU_ROV_CLASS;
        else if (z32_high || (S.zt && S.zt->z_high)) why = GPU_ROV_Z32_HIGH;
        else if (cls == GPU_PC_TRI && S.zfi >= 0 && (zb = gpu_front_zbound(&T, a, b, c, S.zfi, &zbp)) == 2) {
            why = GPU_ROV_Z32_HIGH;      /* (P1: a steep Z24 / Z32 plane whose exact Z may reach 2^20) */
            gf.zb_rov++;
        }
        else if (outcome && gf_cfg.straddle_rov && (outcome == 4 || S.straddle_frame == gf_frame + 1)) {
            /* (the escalation flag of 4.3, off by default since M5's measurement: the exact split, sb_prim,
             * does the same on the FF tier. Sticky for the rest of the frame: further primitives of a state
             * that straddled join its ROV draw instead of alternating FF / ROV draws) */
            why = GPU_ROV_STRADDLE;
            S.straddle_frame = gf_frame + 1;
        }
        else if (gf_cfg.ff_rov_mask) {   /* (classes kept on the ROV tier: M5 moved them to FF one at a time) */
            static const uint32_t bc[5] = { GPU_FFC_OFF, GPU_FFC_NOCOLOR, GPU_FFC_ADD, GPU_FFC_REVSUB, GPU_FFC_DUAL };
            uint32_t c = bc[S.blend] | (S.fatst == FATST_SPLIT ? GPU_FFC_SPLIT : 0) | (ts.T ? GPU_FFC_VIEW : 0) |
                         (cls == GPU_PC_TRI ? GPU_FFC_TRI : cls == GPU_PC_SPRITE ? GPU_FFC_SPRITE : GPU_FFC_LINE) |
                         (S.zfi >= 0 ? GPU_FFC_DEPTH : 0);
            if (c & gf_cfg.ff_rov_mask) why = GPU_ROV_CLASS;
        }
        if (why >= 0) tier = GPU_TIER_ROV;
    }
    gf.tier[tier]++;
    if (why >= 0) gf.rov_why[why]++;
    if (tier == GPU_TIER_ALIAS16 || tier == GPU_TIER_PUNT || (P->tme && ts.T))
        gf.post_prims++;                 /* (post passes: ALIAS16, target views, PUNT) */
    if (punt) {
        gf.punt_why[punt - 1]++;
        punt_prim(ev, cls, &ts, x0, y0, x1, y1);
        goto out;
    }
    /* (M7, 3.1) one sample grid for the colour and depth targets of a draw, and for a 1:1 read of another
     * target (the larger family wins; a change rescales the target before this primitive's draw) */
    if (S.ct && S.zt && (S.ct->kx != S.zt->kx || S.ct->ky != S.zt->ky)) own_pair_family(S.ct, S.zt);
    if (ts.T && S.ct && ts.T != S.ct && cls == GPU_PC_SPRITE && (ts.T->kx != S.ct->kx || ts.T->ky != S.ct->ky) &&
        sp.du == 16 << GS_UVSH && gs_d2i32(sp.pl.v.cy * (double)(1 << GS_UVSH)) == 16 << GS_UVSH)
        own_pair_family(ts.T, S.ct);
    if (P->tme) {
        switch (ts.kind) {
        case TK_VIEW_C32: gf.tex_view_c32++; break;
        case TK_VIEW_ALPHA: gf.tex_view_alpha++; break;
        case TK_ALIAS16: gf.tex_alias16++; break;
        default: gf.tex_vram++; break;
        }
    }
    /* a view of the colour target the primitive writes: overlapping byte lanes need the state before
     * the primitive (a shadow copy, below); disjoint lanes are read in place (the ROV tier reads the
     * bound target; the lanes it reads are not written). ALIAS16 runs read T itself, unbound. */
    uint32_t self = tier == GPU_TIER_ROV && ts.T && ts.T == S.ct && !(S.tex_lanes & S.c_lanes) ? GPU_TS_SELF : 0;
    /* (FF draws cannot read their render target: such a view is a feedback read through the shadow copy) */
    uint32_t texsrc = P->tme ? (uint32_t)ts.kind | (ts.T ? (uint32_t)ts.T->id << 8 : 0) | self : 0;
    uint32_t state = state_for(ev, texsrc);
    if ((int)state != prev_state_index) { gf.state_changes++; prev_state_index = (int)state; }
    uint32_t phase = phase_of(texsrc);
    gf.phase_prims[phase]++;

    if (tier != GPU_TIER_ALIAS16 && AB.open) close_alias();

    /* ---- line pixels first: a line whose DDA covers no pixel inside the scissor draws nothing and
     * must not open a D3D draw (that would also split the draws around it) ---- */
    size_t lp_start = A_rec.n;
    uint32_t lp_n = 0;
    /* (M7) outlines = thin at scale > 1: one analytic record instead of the DDA's pixels */
    int thin = cls == GPU_PC_LINEPIX && gf_cfg.thin && gf_cfg.scale > 1;
    int dcls = thin ? GPU_PC_THIN : cls;
    if (thin) {
        emit_thin(ev, &L, a, b, aa, state, S.ct ? S.ct : S.zt);
        lp_n = 1;
    } else if (cls == GPU_PC_LINEPIX) {
        lp_n = emit_linepix(ev, &L, a, b, aa, state);
        if (!lp_n) { gf.culled++; gf.cls[cls]--; goto out; }
    }

    PROF_MARK(4);
    /* ---- ownership: texture views, feedback, destination ---- */
    if (ts.T) {
        view_read(&ts);
        if (!S.alias && ((ts.T == S.ct && !self) || ts.T == S.zt)) {
            int r[4] = { ts.u0, ts.v0, ts.u1, ts.v1 };
            if (S.tpsm == PSMCT16 || S.tpsm == PSMCT16S) rect16_to32(ts.u0, ts.v0, ts.u1, ts.v1, r);
            feedback_check(ts.T, r[0], r[1], r[2], r[3]);
        }
    }
    int alias = tier == GPU_TIER_ALIAS16;
    int rov = tier == GPU_TIER_ROV || alias;  /* (ALIAS16 draws are ROV draws on the staging texture) */
    /* (an opaque sprite overwrites its rect whatever the tier: the REF tier reads nothing there either) */
    int opaque = cls == GPU_PC_SPRITE && S.opaque && !alias;
    if (alias) {                         /* the 16-bit rect's blocks of T (both halfwords of a word: never opaque) */
        own_grow(S.ct, rows16(y1));
        own_blocks(S.ct, ablk, ct16_blocks(P->fpsm, P->fpage * 32, P->fbw, x0, y0, x1, y1, ablk, GPU_BLOCKS), S.cwrite);
    } else if (S.ct && (S.cwrite || S.reads_dest || rov)) {
        own_draw(S.ct, x0, y0, x1, y1, S.cwrite, opaque);
    }
    /* (Z24 writes keep the top byte of every word: never a full overwrite) */
    if (S.zt) own_draw(S.zt, x0, y0, x1, y1, S.zwrite, opaque && !S.zread && S.zt->kind != TGT_Z24);
    if (S.ct) S.ct->last_used = gf_frame;
    if (alias) alias_prim(ev, &ts, x0, y0, x1, y1, a16_copy_prim(ev, cls, &ts));

    PROF_MARK(5);
    /* ---- pipeline key and draw merging ---- */
    int tk = P->tme ? ts.kind : TK_NONE;
    int zmode = S.zfi >= 0 ? (rov ? 2 : 1) : 0;
    /* (mixed frames, default mode: FF and ROV triangles carry 128-byte records, S/T/Q interpolated) */
    int ff_fast = !alias && cls == GPU_PC_TRI && gf_cfg.mixed && !gf_cfg.ref && !gf_cfg.ff_stq_exact;
    uint32_t ps_key = (uint32_t)(alias ? 3 : rov ? 2 : S.blend == BL_DUAL ? 0 : 1) | (uint32_t)tk << 2 | (uint32_t)cls << 5 |
                      (uint32_t)zmode << 7 | (uint32_t)(P->tme && S.stq) << 9 | (uint32_t)(S.fatst == FATST_DISCARD) << 10 |
                      (uint32_t)(self != 0) << 11 | (ff_fast ? (uint32_t)GPU_PSK_FF_FAST : 0u);
    uint16_t ctid = S.ct ? S.ct->id : 0, ztid = S.zt ? S.zt->id : 0, texid = ts.T ? ts.T->id : 0;
    if (tier == GPU_TIER_ROV) {
        /* a run binds one colour target and (when some draw of it uses Z) one depth target: draws
         * without Z join any run on their colour target */
        if (!RR.open || RR.ct != ctid || (ztid && RR.zt && RR.zt != ztid)) {
            close_draw();
            close_run();
            RR.open = 1; RR.ct = ctid; RR.zt = 0; RR.wrote_z = 0;
            RR.bbox[0] = x0; RR.bbox[1] = y0; RR.bbox[2] = x1; RR.bbox[3] = y1;
        }
        if (ztid) RR.zt = ztid;
        RR.wrote_z |= S.zwrite;
        if (x0 < RR.bbox[0]) RR.bbox[0] = x0;
        if (y0 < RR.bbox[1]) RR.bbox[1] = y0;
        if (x1 > RR.bbox[2]) RR.bbox[2] = x1;
        if (y1 > RR.bbox[3]) RR.bbox[3] = y1;
    } else if (RR.open) {
        close_run();
    }
    uint64_t key;
    /* ROV tier: blend, masks, tests and Z mode are StateRec data (uniform branches), so only the
     * texture kind, primitive class, STQ, the bound texture, the scissor and the targets split its draws
     * (M4: a run can bind a depth target after draws without one; the draw binds the targets of its first
     * primitive) */
    if (rov) key = (uint64_t)(ps_key & ~(3u << 7)) | (uint64_t)texid << 32 | 1ull << 63 | (uint64_t)ctid << 12 | (uint64_t)ztid << 20;
    else {
        /* FF (M5): texture kind, STQ/UV, the alpha test and fog are StateRec data the pixel shader branches
         * on, so only the blend and depth state, the write mask, the ATST split (with its AFAIL), the
         * primitive class, the shader output set and the bindings split draws (the M2 benchmark path took
         * the texture kind of a draw's first primitive) */
        int split = S.fatst == FATST_SPLIT;
        key = (uint64_t)(ps_key & 3u) | (uint64_t)cls << 5 | (uint64_t)zmode << 7 | (uint64_t)ff_fast << 9 |
              (uint64_t)S.blend << 12 | (uint64_t)S.depth_class << 16 | (uint64_t)S.wmask << 20 | (uint64_t)split << 24 |
              (uint64_t)(split ? P->afail : 0) << 25 | (uint64_t)ctid << 32 | (uint64_t)ztid << 40 | (uint64_t)texid << 48;
        if (!gf_cfg.mixed) key |= (uint64_t)(ps_key & 0x1Cu) << 56;   /* (the M2 benchmark shaders: texture class) */
    }
    uint64_t sckey = (uint64_t)P->scx0 | (uint64_t)P->scy0 << 11 | (uint64_t)P->scx1 << 22 | (uint64_t)P->scy1 << 33;
    key ^= sckey * 0x9E3779B97F4A7C15ull;    /* (the draw's scissor is its first primitive's) */
    if (thin) key ^= 1ull << 61;             /* (M7: thin lines are their own draws) */
    /* the exact ATST split (above sb_prim): a pass-capable primitive over pixels of an earlier fail-capable
     * one of the open draw starts a new draw (thin lines: their bounding box) */
    int xsplit = tier == GPU_TIER_FF && S.fatst == FATST_SPLIT && outcome && !gf_cfg.split_ff;
    { static int nosb = -1; if (nosb < 0) { const char* e = getenv("PS2_GPU_DBG_NOSB"); nosb = e && atoi(e); } if (nosb) xsplit = 0; }   /* (M9 measurement: wrong pictures) */
    gs_sprite tbox;
    int scls = cls;
    if (thin) { memset(&tbox, 0, sizeof tbox); tbox.px0 = x0; tbox.py0 = y0; tbox.px1 = x1; tbox.py1 = y1; scls = GPU_PC_SPRITE; }
    const gs_sprite* spp = thin ? &tbox : &sp;
    if (xsplit && OD.open && OD.key == key && sb_owner == od_serial && (outcome & 5) &&
        sb_prim(scls, &T, spp, lp_start, lp_n, x0, y0, 0)) {
        close_draw();
        gf.split_conflicts++;
    }
    /* (M11 no-ROV) this primitive's pixels: triangles and thin lines grown by one (samples at scale, rasterizer edges),
     * sprites and points exactly (single rate: the samples of their own native pixels), line pixels within the line's
     * box (already one larger) */
    int nrm = cls == GPU_PC_TRI || thin;
    int nrr[4] = { x0 - nrm, y0 - nrm, x1 + nrm, y1 + nrm };
    if (gf_cfg.no_rov && rov && OD.open && OD.key == key && nr_owner == od_serial && nr_hit(nrr)) {
        close_draw();
        gf.norov_splits++;
    }
    if (!OD.open || OD.key != key) {
        if (trace_on && OD.open) fprintf(stderr, "[front] key %016llX -> %016llX\n", (unsigned long long)OD.key, (unsigned long long)key);
        close_draw();
        od_serial++;
        OD.open = 1;
        OD.key = key;
        OD.outcomes = 0;
        OD.prims = 0;
        memset(&OD.d, 0, sizeof OD.d);
        OD.d.tier = (uint8_t)tier; OD.d.cls = (uint8_t)dcls; OD.d.blend = (uint8_t)S.blend; OD.d.depth = (uint8_t)S.depth_class;
        OD.d.ps_key = ps_key; OD.d.ct = ctid; OD.d.zt = ztid; OD.d.tex = texid; OD.d.wmask = (uint8_t)S.wmask;
        OD.phase = phase;
        OD.d.sc[0] = (int16_t)P->scx0; OD.d.sc[1] = (int16_t)P->scy0; OD.d.sc[2] = (int16_t)P->scx1; OD.d.sc[3] = (int16_t)P->scy1;
        OD.d.first = cls == GPU_PC_TRI ? (uint32_t)(A_idx.n / 4) : 0;
        OD.d.vtx_first = (uint32_t)(A_vtx.n / sizeof(gvtx));
        OD.d.rec_first = (uint32_t)(cls == GPU_PC_LINEPIX ? lp_start : A_rec.n);   /* (thin lines too) */
        OD.d.afail = (uint8_t)(S.fatst == FATST_SPLIT ? P->afail : 0);
    }
    OD.d.zw |= (uint8_t)(S.zwrite != 0);
    OD.outcomes |= outcome;
    if (gf_cfg.no_rov && rov) {
        if (nr_owner != od_serial) { nr_n = 0; nr_owner = od_serial; }
        nr_add(nrr);
    }
    if (xsplit) {
        if (sb_owner != od_serial) { sb_reset(); sb_owner = od_serial; }
        if (outcome & 6) sb_prim(scls, &T, spp, lp_start, lp_n, x0, y0, 1);
    }
    /* (M7) at scale a triangle's samples reach into the native row above and the column left of its native
     * pixels (sample (i, j) of pixel (x, y) sits at x + i / kx, y + j / ky): the draw's bbox (depth tile sync,
     * shadow copies) covers them, inside the scissor */
    int bx0 = x0, by0 = y0;
    if (cls == GPU_PC_TRI && gf_cfg.scale > 1) {
        if (bx0 > P->scx0) bx0--;
        if (by0 > P->scy0) by0--;
    }
    if (!OD.prims) { OD.bbox[0] = bx0; OD.bbox[1] = by0; OD.bbox[2] = x1; OD.bbox[3] = y1; }
    else { if (bx0 < OD.bbox[0]) OD.bbox[0] = bx0; if (by0 < OD.bbox[1]) OD.bbox[1] = by0; if (x1 > OD.bbox[2]) OD.bbox[2] = x1; if (y1 > OD.bbox[3]) OD.bbox[3] = y1; }
    if (!OD.prims) { OD.alpha = ev->reg[R_ALPHA_1 + ev->ctx]; OD.test = ev->reg[R_TEST_1 + ev->ctx]; OD.frame = ev->reg[R_FRAME_1 + ev->ctx]; }
    OD.prims++;
    if (alias) alias_prim_drawn();
    if (P->tme) {                        /* (perf-p2) features of the draw's primitives: the specialised shaders lack them */
        int o2o = cls == GPU_PC_SPRITE && sp.du == 16 << GS_UVSH && gs_d2i32(sp.pl.v.cy * (double)(1 << GS_UVSH)) == 16 << GS_UVSH;
        OD.d.feat |= (uint16_t)(GPU_DF_TEX | (tk >= TK_VIEW_C32 ? GPU_DF_VIEW : GPU_DF_VRAM) | (P->texm == 3 ? GPU_DF_LOD : 0) |
                                ((tk == TK_VIEW_C32 || tk == TK_VIEW_ALPHA) && !o2o ? GPU_DF_HIRES : 0));
    }
    /* (perf-p2) ROV-tier early depth rejection needs every primitive depth-tested GEQUAL / GREATER (no ALWAYS: stored Z only
     * grows within the draw, so the DSV stays a lower bound) and, for triangles, the proven vertex bound (P1's
     * gpu_front_zbound case 0, as FF triangles; the ROV fast path's vertex shader is vs_ff_tri) */
    int rov_zb = -1;
    if (tier == GPU_TIER_ROV && cls == GPU_PC_TRI && S.zfi >= 0 && ff_fast) {
        if (zb < 0) zb = gpu_front_zbound(&T, a, b, c, S.zfi, &zbp);
        rov_zb = zb;
    }
    if (tier == GPU_TIER_ROV && !(S.zfi >= 0 && (P->ztst == 2 || P->ztst == 3) && (cls != GPU_PC_TRI || rov_zb == 0)))
        OD.d.feat |= GPU_DF_ZNGE;

    PROF_MARK(6);
#ifdef GPU_FRONT_PROF
    prof_emit = 1;
#endif
    /* ---- emission ---- */
    switch (cls) {
    case GPU_PC_TRI: {
        /* (P1, appendix P) the early-depth bound vertex Z + GPU_ZB_MARGIN holds for an FF triangle whose vertex Z are all
         * within the format and the DSV's exact range and whose Z plane is not steeper than GPU_ZB_SLOPE per pixel; any
         * other FF depth triangle is drawn with the bound 1.0 (exact, no early rejection) */
        int nb = 0;
        if (zmode == 1 && tier == GPU_TIER_FF) {
            if (zb < 0) zb = gpu_front_zbound(&T, a, b, c, S.zfi, &zbp);   /* (decided with the tier: never here) */
            nb = zb != 0 || !P->zuse;    /* (!zuse: a constant plane - never here: no Z then) */
            if (nb) gf.zb_sat++;
        } else if (rov_zb >= 0) {
            nb = rov_zb != 0 || !P->zuse;   /* (perf-p2: ROV triangles carry the bound only where it is proven) */
        }
        uint32_t* ix = ar_reserve(&A_idx, 12);
        for (int i = 0; i < 3; i++) ix[i] = emit_vertex(vs[i], ev, state, nb);
        gf.indices += 3;
        gf.idx_bytes += 12;
        OD.d.count += 3;
        PROF_MARK(8);
        if (ff_fast && gf_cfg.mixed) {
            /* FF triangle (M5): the integer part of the record (128 bytes: Z, colour, fog, U/V rows), S/T/Q
             * interpolated by the hardware */
            /* (M9) only the planes this record holds and only where they vary: gs_tri_planes + gs_rowstep_init
             * computed every plane (S/T/Q too, which this record does not carry); a constant plane gives
             * row = round(value * scale) and zero steps, exactly what gs_rowstep_attr gives for it */
            fullrec* r = ar_reserve(&A_rec, 128);
            gs_plane pp;
            int64_t row, dyy, dxx;
#define FF_ROW(cond, fld, scale, is_z, ROW, DY, DX)                                                      \
            do {                                                                                        \
                if (cond) {                                                                             \
                    gs_tri_plane(&T, &pp, vs[0]->fld, vs[1]->fld, vs[2]->fld);                          \
                    gs_rowstep_attr(&pp, T.by0, scale, is_z, &row, &dyy, &dxx);                         \
                } else {                                                                                \
                    row = gs_fx_round((double)vs[2]->fld * (scale)); dyy = 0; dxx = 0;                  \
                }                                                                                       \
                ROW; DY; DX;                                                                            \
            } while (0)
            int iip = ev->iip, uvp = P->tme && P->fst;
            if (zb >= 0 && P->zuse) {            /* (P1: the plane of the tier decision - the same values) */
                gs_rowstep_attr(&zbp, T.by0, 65536.0, 1, &row, &dyy, &dxx);
                r->zrow = row; r->zdy = dyy; r->zdx = dxx;
            } else FF_ROW(P->zuse, z, 65536.0, 1, r->zrow = row, r->zdy = dyy, r->zdx = dxx);
            FF_ROW(iip, r, 65536.0, 0, r->row[0] = (int32_t)(uint32_t)(uint64_t)row, r->dy[0] = (int32_t)(uint32_t)(uint64_t)dyy, r->dx[0] = (int32_t)(uint32_t)(uint64_t)dxx);
            FF_ROW(iip, g, 65536.0, 0, r->row[1] = (int32_t)(uint32_t)(uint64_t)row, r->dy[1] = (int32_t)(uint32_t)(uint64_t)dyy, r->dx[1] = (int32_t)(uint32_t)(uint64_t)dxx);
            FF_ROW(iip, b, 65536.0, 0, r->row[2] = (int32_t)(uint32_t)(uint64_t)row, r->dy[2] = (int32_t)(uint32_t)(uint64_t)dyy, r->dx[2] = (int32_t)(uint32_t)(uint64_t)dxx);
            FF_ROW(iip, a, 65536.0, 0, r->row[3] = (int32_t)(uint32_t)(uint64_t)row, r->dy[3] = (int32_t)(uint32_t)(uint64_t)dyy, r->dx[3] = (int32_t)(uint32_t)(uint64_t)dxx);
            FF_ROW(P->fge, f, 65536.0, 0, r->row[4] = (int32_t)(uint32_t)(uint64_t)row, r->dy[4] = (int32_t)(uint32_t)(uint64_t)dyy, r->dx[4] = (int32_t)(uint32_t)(uint64_t)dxx);
            if (uvp) {
                FF_ROW(1, u, (double)(1 << GS_UVSH), 0, r->row[5] = (int32_t)(uint32_t)(uint64_t)row, r->dy[5] = (int32_t)(uint32_t)(uint64_t)dyy, r->dx[5] = (int32_t)(uint32_t)(uint64_t)dxx);
                FF_ROW(1, v, (double)(1 << GS_UVSH), 0, r->row[6] = (int32_t)(uint32_t)(uint64_t)row, r->dy[6] = (int32_t)(uint32_t)(uint64_t)dyy, r->dx[6] = (int32_t)(uint32_t)(uint64_t)dxx);
            } else {                             /* (U / V planes are 0 without FST texturing) */
                r->row[5] = r->dy[5] = r->dx[5] = 0;
                r->row[6] = r->dy[6] = r->dx[6] = 0;
            }
#undef FF_ROW
            r->by0 = (int16_t)T.by0;
            r->bx0 = (int16_t)T.bx0;
            r->state = state;
            r->lod_q = c->q;
            memset((uint8_t*)r + 120, 0, 8);
            gf.records_full++;
            gf.rec_bytes += 128;
        } else if (rov || gf_cfg.mixed) {
            gs_planes pl;
            gs_tri_planes(&T, vs, ev->iip, P->fge, P->zuse, P->tme, P->fst, &pl);
            gs_rowstep rs;
            gs_rowstep_init(&rs, &pl, T.by0);
            fullrec* r = ar_alloc(&A_rec, sizeof *r);
            r->zrow = rs.row[GS_A_Z]; r->zdy = rs.dy[GS_A_Z]; r->zdx = rs.dx[GS_A_Z];
            static const int map[7] = { GS_A_R, GS_A_G, GS_A_B, GS_A_A, GS_A_F, GS_A_U, GS_A_V };
            /* low 32 bits: the software rasterizer's int32 values per pixel are row + dy * rows +
             * dx * x modulo 2^32 (its span starts saturate only far outside any real range) */
            for (int k = 0; k < 7; k++) {
                r->row[k] = (int32_t)(uint32_t)(uint64_t)rs.row[map[k]];
                r->dy[k] = (int32_t)(uint32_t)(uint64_t)rs.dy[map[k]];
                r->dx[k] = (int32_t)(uint32_t)(uint64_t)rs.dx[map[k]];
            }
            r->by0 = (int16_t)T.by0;
            r->bx0 = (int16_t)T.bx0;
            r->state = state;
            r->lod_q = c->q;                     /* (draw_triangle: P.lod_q = v2->q for FST) */
            r->s0 = rs.s; r->t0 = rs.t; r->q0 = rs.q;
            r->sdy = rs.sdy; r->tdy = rs.tdy; r->qdy = rs.qdy;
            r->sdx = rs.sdx; r->tdx = rs.tdx; r->qdx = rs.qdx;
            gs_edge e[3];
            gs_tri_edges(&T, e);
            uint32_t ne = 0;
            for (int k = 0; k < 3; k++)
                if (e[k].a > 0 && ne < 2) { r->ea[ne] = (int32_t)e[k].a; r->edc[ne] = (int32_t)e[k].dc; r->ec[ne] = e[k].c; ne++; }
            r->nedge = ne;
            if (P->tme && !P->fst && (int64_t)(T.bx1 - T.bx0 + 1) * (T.by1 - T.by0 + 1) > GS_MAX_SPAN) stq_cuts(&T, P, r);
            if (trace_on)
                fprintf(stderr, "[front] tri record %u: by0 %d bx0 %d s0 %.17g sdy %.17g sdx %.17g q0 %.17g edges %u\n",
                        rec_count(sizeof *r) - 1, r->by0, r->bx0, r->s0, r->sdy, r->sdx, r->q0, r->nedge);
            gf.records_full++;
            gf.rec_bytes += sizeof *r;
        } else if (zmode) {
            gs_plane zp;
            gs_tri_plane(&T, &zp, a->z, b->z, c->z);
            zrec* r = ar_reserve(&A_rec, sizeof *r);
            gs_rowstep_attr(&zp, T.by0, 65536.0, 1, &r->row, &r->dy, &r->dx);
            r->pad = 0;
            r->by0 = (int16_t)T.by0;
            r->zfmt = (uint16_t)P->zpsm;
            gf.records_z++;
            gf.rec_bytes += sizeof *r;
            if (gf_cfg.selfcheck) {      /* the sw rasterizer's own row stepping for this triangle */
                gs_planes pl;
                gs_tri_planes(&T, vs, ev->iip, P->fge, P->zuse, P->tme, P->fst, &pl);
                gs_rowstep rs;
                gs_rowstep_init(&rs, &pl, T.by0);
                if (rs.row[GS_A_Z] != r->row || rs.dy[GS_A_Z] != r->dy || rs.dx[GS_A_Z] != r->dx) gf.selfcheck_bad++;
            }
        }
        break;
    }
    case GPU_PC_SPRITE: {
        spriterec* r = ar_alloc(&A_rec, sizeof *r);
        r->x0 = (int16_t)sp.px0; r->y0 = (int16_t)sp.py0; r->x1 = (int16_t)(sp.px1 + 1); r->y1 = (int16_t)(sp.py1 + 1);
        r->rgba = (uint32_t)b->r | (uint32_t)b->g << 8 | (uint32_t)b->b << 16 | (uint32_t)b->a << 24;
        r->z = zclamp(P, (int64_t)b->z << 16);
        r->fog = (uint32_t)b->f;
        r->ua = sp.ua; r->va = sp.va; r->du = sp.du; r->dv = gs_d2i32(sp.pl.v.cy * (double)(1 << GS_UVSH));
        r->flags = sp.du == 16 << GS_UVSH && r->dv == 16 << GS_UVSH;   /* one texel per pixel and row: 1:1 (5.1) */
        /* V of each row as draw_sprite evaluates it (gs_span_start_at at (px0, py)) */
        r->vc0 = sp.pl.v.c0 + sp.pl.v.cx * (double)sp.px0;
        r->vcy = sp.pl.v.cy;
        r->lod_q = b->q;                     /* (draw_sprite: P.lod_q = v1->q) */
        r->state = state;
        gf.records_sprite++;
        gf.rec_bytes += sizeof *r;
        OD.d.count++;
        if (S.fill) fill_add(ev, &sp, b);
        else FA.active = 0;
        break;
    }
    case GPU_PC_LINEPIX: {
        uint32_t n = lp_n;                   /* the records were emitted before the draw opened */
        if (thin) {                          /* (M7: one thin-line instance) */
            gf.rec_bytes += sizeof(thinrec);
            OD.d.count += 1;
            break;
        }
        gf.linepix += n;
        gf.records_linepix += n;
        gf.rec_bytes += n * sizeof(linerec);
        OD.d.count += n;
        break;
    }
    default: {
        linerec* r = ar_alloc(&A_rec, sizeof *r);
        r->px = (int16_t)x0; r->py = (int16_t)y0;
        r->z = S.zfi >= 0 ? zclamp(P, (int64_t)a->z << 16) : 0;
        r->rgba = (uint32_t)a->r | (uint32_t)a->g << 8 | (uint32_t)a->b << 16 | (uint32_t)a->a << 24;
        r->fog_state = (uint32_t)(a->f & 0xFF) | state << 8;
        r->s = P->fst ? (uint32_t)(a->u << GS_UVSH) : f2u(a->s);
        r->t = P->fst ? (uint32_t)(a->v << GS_UVSH) : f2u(a->t);
        r->q = f2u(a->q);
        r->lod_q = a->q;
        gf.linepix++;
        gf.records_linepix++;
        gf.rec_bytes += sizeof *r;
        OD.d.count++;
        break;
    }
    }
    if (cls != GPU_PC_SPRITE) FA.active = 0;
out:
    if (timed) tsc_acc += (__rdtsc() - t0) * 16;
#ifdef GPU_FRONT_PROF
    if (prof_emit) PROF_MARK(7); else PROF_MARK(11);
    prof_emit = 0;
#endif
}

/* ---- transfers, CLUT, reset ------------------------------------------------------------------------ */
static void fr_xfer_start(uint64_t bb, uint64_t pos, uint64_t rr, uint32_t dir) {
    uint64_t t0 = __rdtsc();
    if (dir == 0) {
        flush_open();                    /* drawing before the upload stays before its patch */
        int x0 = (int)((pos >> 32) & 0x7FF), y0 = (int)((pos >> 48) & 0x7FF);
        int x1 = x0 + (int)(rr & 0xFFF), y1 = y0 + (int)((rr >> 32) & 0xFFF);
        int psm = (int)((bb >> 56) & 0x3F);
        own_upload(psm, (uint32_t)((bb >> 32) & 0x3FFF), (uint32_t)((bb >> 48) & 0x3F), x0, y0, x1, y1);
        gf.xfers++;
        FA.active = 0;
    } else if (dir == 1) {
        gf.xfer_read++;                  /* TRXDIR=1: download of the source (none in the corpus) */
        flush_open();
        int sx = (int)(pos & 0x7FF), sy = (int)((pos >> 16) & 0x7FF);
        own_mem_read((int)((bb >> 24) & 0x3F), (uint32_t)(bb & 0x3FFF), (uint32_t)((bb >> 16) & 0x3F), sx, sy,
                     sx + (int)(rr & 0xFFF) - 1, sy + (int)((rr >> 32) & 0xFFF) - 1);
    } else if (dir == 2) {
        gf.xfer_local++;                 /* TRXDIR=2: download source + CPU copy + invalidate (none in the corpus) */
        flush_open();
        int sx = (int)(pos & 0x7FF), sy = (int)((pos >> 16) & 0x7FF), w = (int)(rr & 0xFFF), h = (int)((rr >> 32) & 0xFFF);
        own_mem_read((int)((bb >> 24) & 0x3F), (uint32_t)(bb & 0x3FFF), (uint32_t)((bb >> 16) & 0x3F), sx, sy, sx + w - 1, sy + h - 1);
        int dx = (int)((pos >> 32) & 0x7FF), dy = (int)((pos >> 48) & 0x7FF);
        own_upload((int)((bb >> 56) & 0x3F), (uint32_t)((bb >> 32) & 0x3FFF), (uint32_t)((bb >> 48) & 0x3F), dx, dy, dx + w, dy + h);
    }
    tsc_acc += __rdtsc() - t0;
}

static void fr_xfer_data(uint32_t bytes) { gf.xfer_bytes += bytes; }

static void fr_tex0(int ctx, uint64_t tex0, int clut_loaded) {
    (void)ctx;
    if (!clut_loaded) return;
    uint64_t t0 = __rdtsc();
    /* a CLUT load reads memory: it must hold the data (pending patches, GPU data downloaded) */
    uint32_t psm = (uint32_t)((tex0 >> 20) & 0x3F), cbp = (uint32_t)((tex0 >> 37) & 0x3FFF), cpsm = (uint32_t)((tex0 >> 51) & 0xF);
    uint32_t csm = (uint32_t)((tex0 >> 55) & 1);
    if ((psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH) && !csm) {
        int t8 = psm == PSMT8 || psm == PSMT8H;
        if (gf_trace) fprintf(stderr, "[front] CLUT load CBP %04X %s CPSM %02X\n", cbp, t8 ? "T8" : "T4", cpsm);
        /* A CLUT load reads all 16/256 entries; Sly uploads only the colours a texture uses, so the
         * rest of the 16x16 block can be Z-buffer data drawn over it in an earlier frame. Those
         * entries are never indexed (else the PS2 would show Z garbage too): no download (it would
         * stall every frame); counted, and lockstep (M3+) compares the result. */
        int cfmt = cpsm == PSMCT16 || cpsm == PSMCT16S ? (int)cpsm : PSMCT32;
        if (own_mem_flush(cfmt, cbp, 1, 0, 0, t8 ? 15 : 7, t8 ? 15 : 1) ||
            own_mem_vstale(cfmt, cbp, 1, 0, 0, t8 ? 15 : 7, t8 ? 15 : 1))   /* (downloaded words gs_vram lacks) */
            gf.clut_gpu_reads++;
    }
    /* (frontend thread: the GS thread compared the loaded entries and says whether they changed) */
    if (gf_clut_hint > 0) clut_mark_changed();
    else if (gf_clut_hint < 0) clut_mark_dirty();
    tsc_acc += __rdtsc() - t0;
}

static void fr_reset(void) { flush_open(); }

static const gs_front_hooks hooks = { fr_prim, fr_xfer_start, fr_xfer_data, fr_tex0, fr_reset, fr_sw_done };
const gs_front_hooks* gf_hooks(void) { return &hooks; }

/* ---- start / stop / VBlank ------------------------------------------------------------------------- */
static void frame_begin(void) {
    memset(&gf, 0, sizeof gf);
    A_cmd.n = A_vtx.n = A_idx.n = A_rec.n = A_state.n = A_data.n = 0;
    memset(smap_idx, 0, sizeof smap_idx);
    nstates = 0;
    /* StateRecs are per frame. (M9) The state analyses stay valid across frames (target pointers: own_epoch; CLUT ring
     * slots: their generation is in the signature; the texture caches carry their own generations): every frame
     * re-analysed its 200-600 states before */
    for (int i = 0; i < SC_N; i++) SC[i].state_valid = 0;
    Sp = &SC[0];
    S.version = 0;
    prev_state_index = -1;
    vcache_n = vcache_pos = 0;           /* (M4 fix: a stale ring position kept old entries valid) */
    cw_n = 0;
}

/* hand the arenas to the sink (a VBlank's frame, or a checkpoint's partial frame) */
static void sink_frame(uint32_t vblank, uint32_t flags) {
    if (!sink_fn) return;
    gpu_front_frame_data fd = { A_cmd.p, A_cmd.n, A_vtx.p, A_vtx.n, (const uint32_t*)A_idx.p, A_idx.n / 4,
                                A_rec.p, A_rec.n, A_state.p, A_state.n / sizeof(staterec), A_data.p, A_data.n, vblank,
                                flags | (gf_cfg.ref ? GPU_FD_REF : gf_cfg.mixed ? GPU_FD_MIXED : 0) };
    in_sink = 1;
    sink_fn(&fd, sink_user);
    in_sink = 0;
}

void gpu_front_set_checkpoint_hook(void (*hook)(uint32_t index, int frame_changed, const void* ev, int type, const void* const* v, void* user), void* user) {
    cp_hook = hook;
    cp_user = user;
    cp_frame = cp_zbuf = ~0ull;
}

void gpu_front_checkpoint(void) {
    flush_open();
    own_flush_patches();
    sink_frame(cp_vblank, GPU_FD_PARTIAL);
    gf.checkpoints++;
    /* a new partial frame: arenas, StateRecs and vertex reuse start over (counters and state
     * analyses go on) */
    A_cmd.n = A_vtx.n = A_idx.n = A_rec.n = A_state.n = A_data.n = 0;
    memset(smap_idx, 0, sizeof smap_idx);
    nstates = 0;
    for (int i = 0; i < SC_N; i++) SC[i].state_valid = 0;
    prev_state_index = -1;
    vcache_n = vcache_pos = 0;           /* (M4 fix: a stale ring position kept old entries valid) */
    cw_n = 0;
}

int gpu_front_gpu_blocks(gpu_dl_entry* out, uint16_t* writers, int max) { return own_gpu_blocks(out, writers, max); }

void gpu_front_resync(void) {
    flush_open();
    own_resync();
    cp_frame = cp_zbuf = ~0ull;
}

uint32_t gpu_front_post_prims(void) { return gf.post_prims; }

void gpu_front_start(const gpu_front_config* cfg) {
    memset(&gf_cfg, 0, sizeof gf_cfg);
    if (cfg) gf_cfg = *cfg;
    {   /* (M9) PS2_GPU_NO_TEX_ALPHA=1: the M8 source-alpha bound (whole palettes), for comparisons */
        const char* e = getenv("PS2_GPU_NO_TEX_ALPHA");
        if (e && atoi(e)) gf_cfg.no_tex_alpha = atoi(e);   /* (2: only for ATST split outcomes, which stay exact) */
    }
    if (!gf_cfg.no_rov && gpu_no_rov && gpu_no_rov()) gf_cfg.no_rov = 1;   /* (M11) */
    nr_n = 0;
    nr_owner = 0;
    memset(TU, 0, sizeof TU);            /* (block serials start over) */
    GS_INIT();
    nar = 0;
    hud_stats_reset();
    own_reset();
    own_resync();                        /* GS memory as it is now goes to vram_buf (one patch of everything) */
    clut_reset();
    memset(SC, 0, sizeof SC);
    memset(SC_used, 0, sizeof SC_used);
    Sp = &SC[0];
    memset(&OD, 0, sizeof OD);
    memset(&RR, 0, sizeof RR);
    memset(&AB, 0, sizeof AB);
    memset(&FA, 0, sizeof FA);
    gf_frame = 0;
    cp_frame = cp_zbuf = ~0ull;
    cp_vblank = 0;
    frame_begin();
    tsc_acc = 0;
    tsc_start = __rdtsc();
    QueryPerformanceCounter(&qpc_start);
    gs_sw_draw = gf_cfg.sw;
    dual_exact = getenv("PS2_GPU_DUAL_EXACT") && atoi(getenv("PS2_GPU_DUAL_EXACT")) != 0;
    a16_copy_on = !(getenv("PS2_GPU_A16_COPY") && atoi(getenv("PS2_GPU_A16_COPY")) == 0);   /* (perf-p2) */
    const char* tr = getenv("PS2_GPU_FRONT_TRACE");
    trace_vblank = tr ? (uint32_t)strtoul(tr, NULL, 0) : 0xFFFFFFFFu;
    trace_on = 0;
    gf_vram = NULL;
    gf_clut_src = NULL;
    gf_clut_hint = -1;
    if (gf_cfg.thread) fthread_start();   /* proxy hooks on this thread, the frontend on its own */
    else gs_front = &hooks;
}

void gpu_front_drain(void) { if (gf_cfg.thread) fthread_drain(); }
uint64_t gpu_front_thread_cycles(void) { return gf_cfg.thread ? fthread_cycles() : 0; }
void gpu_front_thread_affinity(uint64_t mask) { if (gf_cfg.thread) fthread_affinity(mask); }

void gpu_front_stop(void) {
    if (gf_cfg.thread) fthread_stop();
#ifdef GPU_FRONT_PROF
    fprintf(stderr, "[gpu_front] profile (Mcycles): state %.1f geometry %.1f inputs %.1f texture %.1f tier %.1f ownership %.1f key %.1f emission %.1f\n",
            prof[0] / 1e6, prof[1] / 1e6, prof[2] / 1e6, prof[3] / 1e6, prof[4] / 1e6, prof[5] / 1e6, prof[6] / 1e6, prof[7] / 1e6);
    fprintf(stderr, "[gpu_front] prims %llu, state lookups %llu, misses %llu\n", (unsigned long long)n_prims, (unsigned long long)n_lookups, (unsigned long long)n_misses);
    fprintf(stderr, "[gpu_front] emission: tri vertices %.1f, line pixels %.1f, rest (records) %.1f; early exits %.1f\n", prof[8] / 1e6, prof[10] / 1e6, prof[7] / 1e6, prof[11] / 1e6);
#endif
    gs_front = NULL;
    gs_sw_draw = 1;
    cp_hook = NULL;
}

void gpu_front_vblank(const uint64_t priv[16], uint32_t vblank) {
    if (gf_cfg.thread) fthread_vblank(priv, vblank, NULL);
    else gf_vblank_impl(priv, vblank, NULL);
}

void gpu_front_vblank_display(const gpu_display_info* di) {
    if (gf_cfg.thread) fthread_vblank(di->priv, di->vblank, di);
    else gf_vblank_impl(di->priv, di->vblank, di);
}

void gpu_front_call(void (*fn)(void* arg), void* arg) {
    if (gf_cfg.thread) fthread_call(fn, arg);
    else fn(arg);
}

int gpu_front_threaded(void) { return gf_cfg.thread; }
uint32_t gpu_front_thread_punts(void) { return (uint32_t)thread_punts; }

void gpu_front_set_thread(int on) {
    on = on != 0;
    if (on == gf_cfg.thread) return;
    if (on) {
        S.version = 0;                   /* (the frontend thread numbers state versions on its own) */
        gf_cfg.thread = 1;
        fthread_start();                 /* mirrors from gs_vram / the CLUT buffer as they are now */
    } else {
        fthread_stop();                  /* everything posted is processed; mirrors dropped (gf_vram NULL) */
        gf_cfg.thread = 0;
        S.version = 0;
        gs_front = &hooks;
    }
}

/* M6: where each displayed circuit's picture comes from. A CT32/CT24 buffer whose displayed rect is
 * current in the colour target at its base and width is sampled there (no copy; at any scale the
 * target's samples); anything else is read from a scratch copy of vram_buf with the GPU words of the
 * rect downloaded into it (exact; vram_buf and the ownership stay as they are). The rect is what
 * gs_compose_from reads: columns DBX .. DBX + DW / (MAGH + 1), rows DBY .. DBY + DH / (MAGV + 1), in field
 * mode DBY .. DBY + ((DH + 1) / (MAGV + 1)) / 2 - 1. */
static gpu_dl_entry dsp_e[GPU_BLOCKS];
static uint16_t dsp_w[GPU_BLOCKS];

static uint32_t display_sources(const gpu_display_info* di, uint16_t src[2], uint32_t* mem_off) {
    src[0] = src[1] = 0;
    uint64_t pmode = di->priv[0];
    int field = (di->priv[2] & 3) == 3, n = 0;
    for (int i = 0; i < 2; i++) {
        if (!((pmode >> i) & 1)) continue;
        uint64_t fb = di->priv[7 + 2 * i], d = di->priv[8 + 2 * i];
        uint32_t fbp = (uint32_t)(fb & 0x1FF), fbw = (uint32_t)((fb >> 9) & 0x3F);
        int psm = (int)((fb >> 15) & 0x1F);
        int dbx = (int)((fb >> 32) & 0x7FF), dby = (int)((fb >> 43) & 0x7FF);
        int magh = (int)((d >> 23) & 0xF), magv = (int)((d >> 27) & 3), dw = (int)((d >> 32) & 0xFFF), dh = (int)((d >> 44) & 0x7FF);
        int x1 = dbx + dw / (magh + 1), y1 = field ? dby + (((dh + 1) / (magv + 1)) >> 1) - 1 : dby + dh / (magv + 1);
        if (y1 < dby) y1 = dby;
        int wraps = x1 > 2047 || y1 > 2047;
        if (!fbw) continue;
        if ((psm == PSMCT32 || psm == PSMCT24) && !wraps) {
            gpu_target* T = own_find(TGT_C32, fbp, fbw);
            if (T && x1 < (int)T->fbw * 64 && own_rect_current(T, dbx, dby, x1, y1)) {
                T->last_used = gf_frame;
                src[i] = T->id;
                continue;
            }
        }
        int mp = gs_fmt_of(psm) ? psm : PSMCT32;
        if (wraps) { dbx = 0; dby = 0; x1 = x1 > 2047 ? 2047 : x1; y1 = y1 > 2047 ? 2047 : y1; }
        int n0 = n;
        n += own_rect_gpu(mp, fbp * 32, fbw, dbx, dby, x1, y1, dsp_e + n, dsp_w + n, GPU_BLOCKS - n);
        for (int k = n0; k < n; k++) gt[dsp_w[k]].last_used = gf_frame;   /* (no eviction before the picture) */
        gf.display_mem++;
    }
    *mem_off = 0;
    if (n) {
        *mem_off = gf_data(dsp_e, (uint32_t)n * (uint32_t)sizeof *dsp_e);
        gf_data(dsp_w, (uint32_t)n * 2);
    }
    return (uint32_t)n;
}

void gf_vblank_impl(const uint64_t priv[16], uint32_t vblank, const gpu_display_info* di) {
    uint64_t t0 = __rdtsc();
    flush_open();
    uint16_t src[2] = { 0, 0 };
    uint32_t mem_off = 0, mem_n = di ? display_sources(di, src, &mem_off) : 0;   /* (before the eviction: a shown
                                                                                     target stays in use) */
    own_frame_end(priv);                 /* (flushes the frame's patches: vram_buf is complete for the picture) */
    clut_frame_end();
    gpu_fop_present* p = gf_cmd(GPU_FOP_PRESENT, sizeof *p);
    p->vblank = vblank;
    p->di_off = di ? gf_data(di, sizeof *di) : ~0u;
    p->src[0] = src[0];
    p->src[1] = src[1];
    p->mem_off = mem_off;
    p->mem_n = mem_n;
    p->scale = (uint16_t)(gf_cfg.scale > 0 ? gf_cfg.scale : 1);
    p->scale_x = (uint16_t)(gf_cfg.display_kx > p->scale ? (gf_cfg.display_kx > 16 ? 16 : gf_cfg.display_kx) : p->scale);   /* (M10) */
    tsc_acc += __rdtsc() - t0;
    /* calibrate the TSC against QPC */
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    double secs = (double)(q.QuadPart - qpc_start.QuadPart) / (double)f.QuadPart;
    if (secs > 0.05) tsc_hz = (double)(__rdtsc() - tsc_start) / secs;
    gf.vblank = vblank;
    gf.front_ms = (double)tsc_acc * 1000.0 / tsc_hz;
    {
        static ULONG64 last_cyc;
        ULONG64 cyc = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cyc);
        gf.thread_ms = last_cyc && cyc > last_cyc ? (double)(cyc - last_cyc) * 1000.0 / tsc_hz : 0;
        last_cyc = cyc;
    }
    gf.cmd_bytes = A_cmd.n;
    gf.data_bytes = A_data.n;
    sink_frame(vblank, di && (di->flags & GPU_DI_SIGNAL) ? GPU_FD_SIGNAL : 0);
    cp_vblank = vblank + 1;
    AcquireSRWLockExclusive(&done_lock);
    if (ndone == capdone) {
        capdone = capdone ? capdone * 2 : 1024;
        done = realloc(done, capdone * sizeof *done);
        if (!done) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
    }
    done[ndone++] = gf;
    ReleaseSRWLockExclusive(&done_lock);
    tsc_acc = 0;
    gf_frame++;
    frame_begin();
    trace_on = vblank + 1 == trace_vblank;
    if (di && (di->flags & GPU_DI_CONFIG))
        gf_configure(di->scale, (di->gfx & GPU_GFX_PROGRESSIVE) != 0, (di->gfx & GPU_GFX_THIN) != 0, di->scale_x);
}

/* (M7) settings changed at run time: from this frame on (the targets take their new factors before any draw);
 * (M10) display_kx: the display family's kx (anamorphic widescreen, the aspect switched with F7) */
void gf_configure(int scale, int progressive, int thin, int display_kx) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    if (display_kx <= scale) display_kx = 0;
    if (display_kx > 16) display_kx = 16;
    gf_cfg.thin = thin;
    if (scale == (gf_cfg.scale > 0 ? gf_cfg.scale : 1) && progressive == gf_cfg.display_ky2 && display_kx == gf_cfg.display_kx) return;
    gf_cfg.scale = scale;
    gf_cfg.display_ky2 = progressive;
    gf_cfg.display_kx = display_kx;
    own_rescale_all();
}

void gpu_front_dump_targets(FILE* f) {
    static const char* kinds[TGT_N] = { "C32", "Z32", "Z24", "Z16", "Z16S" };
    for (int i = 1; i <= GPU_MAX_TARGETS; i++) {
        const gpu_target* T = &gt[i];
        if (!T->used) continue;
        fprintf(f, "  target %2d %-4s base %03X FBW %2u rows %4d %s %ux%u last used frame %u\n", T->id, kinds[T->kind],
                T->base, T->fbw, T->rows, T->family == FAM_DISPLAY ? "display" : "square ", T->kx, T->ky, T->last_used);
    }
}

int gpu_front_target_kind(uint32_t id) { return id && id <= GPU_MAX_TARGETS && gt[id].used ? gt[id].kind : -1; }
