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
 *             one D3D draw; ROV-tier primitives on one target pair form a ROV run; CT16 sprites over
 *             CT32 targets batch into ALIAS16 dispatches split at halfword hazards.
 *   ownership targets, block validity, uploads (gpu_own.c), CLUT ring (gpu_clut.c).
 *
 * M1 is a dry run: nothing reaches a GPU; gs_replay -Backend hwdry prints and records the counters
 * (gpu_front_frame) and M2 replays the per-frame command stream on the render thread. */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"

#include <stdlib.h>
#include <windows.h>
#include <x86intrin.h>

gpu_front_frame gf;
gpu_front_config gf_cfg;
uint32_t gf_frame;

/* ---- arenas ------------------------------------------------------------------------------------- */
typedef struct { uint8_t* p; size_t n, cap; } arena;
static arena A_cmd, A_vtx, A_idx, A_rec, A_state, A_data;

static void* ar_alloc(arena* a, size_t bytes) {
    if (a->n + bytes > a->cap) {
        size_t c = a->cap ? a->cap : 1 << 16;
        while (c < a->n + bytes) c *= 2;
        a->p = realloc(a->p, c);
        if (!a->p) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
        a->cap = c;
    }
    void* r = a->p + a->n;
    memset(r, 0, bytes);
    a->n += bytes;
    return r;
}

static void flush_open(void);
static int in_flush, cw_barrier;
static int trace_on;                     /* PS2_GPU_FRONT_TRACE=<vblank>: log that VBlank's draws */

void* gf_cmd(uint16_t op, uint32_t bytes) {
    /* an op that reads GPU data the open draw may write (download, shadow copy) or changes memory
     * it may have sampled (patch, release) closes the open draw / run / batch first, so it never
     * overtakes drawing already merged. Target creation, CLUT ring slots (always a fresh slot),
     * loads (only stale blocks, which the open draw has not touched) and clears (the same pixels
     * as the fill sprites before them) need no ordering against the open draw. */
    if ((op == GPU_FOP_PATCH || op == GPU_FOP_DOWNLOAD || op == GPU_FOP_COPY_SHADOW || op == GPU_FOP_RELEASE) && !in_flush) {
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
static void (*sink_fn)(const gpu_front_frame_data*, void*);
static void* sink_user;

void gpu_front_set_sink(void (*sink)(const gpu_front_frame_data* fd, void* user), void* user) { sink_fn = sink; sink_user = user; }

int gpu_front_take_frames(gpu_front_frame* out, int max) {
    int n = (int)(ndone < (size_t)max ? ndone : (size_t)max);
    memcpy(out, done, (size_t)n * sizeof *out);
    memmove(done, done + n, (ndone - (size_t)n) * sizeof *done);
    ndone -= (size_t)n;
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
    int apre[257];                       /* prefix sums of the alpha test pass table */
    int cd_dep, c_as;                    /* lerp blend (Cs-Cd)*C+Cd; C = As */
    int blend;
    int fill;                            /* clear candidate: untextured, unfogged, blend-free opaque sprite */
    int depth_class;
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
} fstate;

static fstate S;
static int prev_state_index = -1;

/* StateRec (96 bytes, 3.5) */
typedef struct {
    uint64_t tex0, clamp, tex1, texa, alpha, scissor;
    uint32_t clut_slot, texsrc, fogcol, test, fbmsk, flags, xyoffset, hud, rec_base, phase;
    uint32_t pad[2];
} staterec;
_Static_assert(sizeof(staterec) == 96, "StateRec is 96 bytes");

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
        if (smap_hash[i] == h && !memcmp(A_state.p + (size_t)(smap_idx[i] - 1) * sizeof *r, r, sizeof *r))
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

static void state_update(const gs_env_view* ev) {
    const gs_prim* P = ev->P;
    const uint64_t* reg = ev->reg;
    int ctx = ev->ctx;
    memset(&S, 0, sizeof S);
    S.version = gs_prim_version;
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
        int c = 0;
        for (int a = 0; a < 256; a++) { S.apre[a] = c; c += P->apass[a]; }
        S.apre[256] = c;
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
            if (D == 1) { S.blend = BL_NOCOLOR; wm &= 8; uses_cd = 0; }   /* colour unchanged */
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
    int full_wm = P->ffmt->bits == 24 ? 7 : 15;
    S.reads_dest = (P->abe && uses_cd) || S.zread || P->date || (S.cwrite && !bytes_ok);
    S.opaque = !S.reads_dest && S.fatst == FATST_NONE && (!S.cwrite || (wm & full_wm) == full_wm) && P->scanmsk < 2;
    /* tiled-fill candidate: constant value per pixel, written whatever the destination holds (byte
     * masks allowed: the alpha reset is a fill of the alpha byte) */
    S.fill = !P->tme && !P->fge && S.blend == BL_OFF && !P->date && S.fatst == FATST_NONE && !S.zread &&
             P->scanmsk < 2 && (S.cwrite || S.zwrite);

    /* ---- targets ---- */
    int rows = P->scy1 + 1;
    if (P->ffmt->bits == 16) {
        /* CT16 frame: an ALIAS16 view of the C32 target at the same base and width (created when
         * the 16-bit view comes first, e.g. a recording that starts between the two) */
        gpu_target* T = own_find(TGT_C32, P->fpage, P->fbw);
        if (!T) T = own_target(TGT_C32, PSMCT32, P->fpage, P->fbw, (rows + 1) / 2);
        if (T) S.alias = 1, S.ct = T;
        else S.punt = GPU_PUNT_TARGET_OVERLAP + 1;
    } else if (S.cwrite || P->date || (S.reads_dest && P->abe)) {
        S.ct = own_target(TGT_C32, PSMCT32, P->fpage, P->fbw, rows);
        if (!S.ct) S.punt = GPU_PUNT_TARGET_OVERLAP + 1;
    }
    if (zuse && !S.alias) {
        int kind = own_target_kind_of_zpsm(P->zpsm);
        S.zt = own_target(kind, P->zpsm, P->zpage, P->zbw, rows);
        if (!S.zt) S.punt = GPU_PUNT_TARGET_OVERLAP + 1;
        else if (S.ct) S.zt->family = S.ct->family;
    }

    /* ---- state-level ROV reasons, in the order of 4.1 ---- */
    if (gf_cfg.ref) S.rov = GPU_ROV_REF + 1;
    else if (rov_cc) S.rov = GPU_ROV_COLCLAMP + 1;
    else if (S.cwrite && !bytes_ok) S.rov = GPU_ROV_FBMSK + 1;
    else if (P->date) S.rov = GPU_ROV_DATE + 1;
    else if (rov_blend) S.rov = GPU_ROV_BLEND + 1;
    else if (S.cd_dep && C == 2 && P->fix > 128) S.rov = GPU_ROV_C_OVER128 + 1;
    else if ((P->pabe && P->abe) || (P->dthe && P->ffmt->bits == 16)) S.rov = GPU_ROV_PABE_DTHE + 1;
}

/* ---- StateRec for the primitive (texture source is per primitive) ------------------------------- */
static uint32_t state_for(const gs_env_view* ev, uint32_t texsrc) {
    if (S.state_valid && S.state_texsrc == texsrc) return S.state_index;
    const gs_prim* P = ev->P;
    const uint64_t* reg = ev->reg;
    int ctx = ev->ctx;
    staterec r;
    memset(&r, 0, sizeof r);
    if (P->tme) {
        r.tex0 = reg[R_TEX0_1 + ctx] & ~(7ull << 61);
        r.clamp = reg[R_CLAMP_1 + ctx];
        r.tex1 = reg[R_TEX1_1 + ctx];
        r.texa = reg[R_TEXA];
        r.clut_slot = S.indexed ? S.clut_slot : 0xFFFFFFFFu;
        r.texsrc = texsrc;
    }
    r.alpha = P->abe ? reg[R_ALPHA_1 + ctx] : 0;
    r.scissor = reg[R_SCISSOR_1 + ctx];
    r.fogcol = P->fge ? (uint32_t)reg[R_FOGCOL] : 0;
    r.test = (uint32_t)reg[R_TEST_1 + ctx];
    r.fbmsk = P->fbmsk;
    r.flags = (uint32_t)P->fba | (uint32_t)P->colclamp << 8 | (uint32_t)P->tfx << 9 | (uint32_t)P->tcc << 11 |
              (uint32_t)P->fge << 12 | (uint32_t)ev->iip << 13 | (uint32_t)P->fst << 14 | (uint32_t)P->abe << 15 |
              (uint32_t)ev->aa1 << 16 | (uint32_t)P->pabe << 17 | (uint32_t)P->dthe << 18 | (uint32_t)ctx << 19 |
              (uint32_t)P->texm << 20;
    r.xyoffset = (uint32_t)(ev->ofx & 0xFFFF) | (uint32_t)ev->ofy << 16;
    S.state_index = staterec_index(&r);
    S.state_texsrc = texsrc;
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
} open_draw;
static open_draw OD;

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

typedef struct {
    int open;
    uint16_t ct, zt;
    int wrote_z;
    int bbox[4];
} rov_run;
static rov_run RR;

typedef struct {
    int open;
    gpu_target* T;
    uint32_t first, count;
} alias_batch;
static alias_batch AB;
static uint64_t* aw_bits;                /* halfwords written / read in the open ALIAS16 dispatch */
static uint64_t* ar_bits;
static uint64_t* ad_bits;                /* this sprite's writes (self-overlap) */

static void close_draw(void) {
    if (!OD.open) return;
    OD.open = 0;
    gpu_fop_draw* d = gf_cmd(GPU_FOP_DRAW, sizeof *d);
    *d = OD.d;
    d->h.op = GPU_FOP_DRAW;
    d->h.size = sizeof *d;
    if (cw_barrier) { cw_reset(); cw_barrier = 0; }
    int split = OD.d.tier == GPU_TIER_FF && ((OD.outcomes & 4) || (OD.outcomes & 3) == 3);
    d->split = (uint8_t)split;
    gf.d3d_draws += 1 + split;
    gf.split_extra += split;
    if (OD.d.tier == GPU_TIER_ROV) { gf.rov_draws++; cw_reset(); gf.d3d_draws_commute++; }
    else {
        gf.ff_draws += 1 + split;
        if (!cw_add(OD.key ^ (uint64_t)split << 62, OD.bbox, OD.d.ct, OD.d.zt, OD.d.tex, OD.d.wmask != 0, (OD.d.depth & 1) != 0))
            gf.d3d_draws_commute += 1 + split;
    }
    if (trace_on)
        fprintf(stderr, "[front] draw %s cls %d prims %u count %u ct %u zt %u tex %u ps %03X blend %d depth %d wmask %X split %d "
                        "FRAME %016llX ALPHA %010llX TEST %05llX\n", OD.d.tier == GPU_TIER_ROV ? "ROV" : "FF ", OD.d.cls,
                OD.prims, OD.d.count, OD.d.ct, OD.d.zt, OD.d.tex, OD.d.ps_key, OD.d.blend, OD.d.depth, OD.d.wmask, split,
                (unsigned long long)OD.frame, (unsigned long long)OD.alpha, (unsigned long long)OD.test);
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
    AB.open = 0;
    gpu_fop_alias16* a = gf_cmd(GPU_FOP_ALIAS16, sizeof *a);
    a->target = AB.T->id; a->first = AB.first; a->count = AB.count;
    gf.alias16_dispatches++;
    if (trace_on) fprintf(stderr, "[front] alias16 dispatch target %u sprites %u\n", AB.T->id, AB.count);
    memset(aw_bits, 0, (1u << 21) / 8);
    memset(ar_bits, 0, (1u << 21) / 8);
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
typedef struct { int64_t zrow, zdy, zdx; int32_t row[7], dy[7], dx[7]; int16_t by0; uint16_t flags; uint8_t pad[12]; } fullrec;
typedef struct { int16_t x0, y0, x1, y1; uint32_t rgba, z, fog, state; int32_t ua, va, du, dv; uint32_t pad[2]; } spriterec;
typedef struct { int16_t px, py; uint32_t z, rgba, fog_state; float s, t, q; uint32_t pad; } linerec;
_Static_assert(sizeof(gvtx) == 32 && sizeof(zrec) == 32 && sizeof(fullrec) == 128 && sizeof(spriterec) == 48 &&
               sizeof(linerec) == 32, "record sizes (3.5)");

static gs_vtx vcache[3];
static uint32_t vcache_idx[3], vcache_state[3];
static int vcache_n, vcache_pos;

static uint32_t emit_vertex(const gs_vtx* v, const gs_env_view* ev, uint32_t state) {
    for (int i = 0; i < vcache_n; i++)
        if (vcache_state[i] == state && !memcmp(&vcache[i], v, sizeof *v)) return vcache_idx[i];
    gvtx* o = ar_alloc(&A_vtx, sizeof *o);
    o->x = (float)(v->x - ev->ofx) / 16.0f;
    o->y = (float)(v->y - ev->ofy) / 16.0f;
    o->rgba = (uint32_t)v->r | (uint32_t)v->g << 8 | (uint32_t)v->b << 16 | (uint32_t)v->a << 24;
    o->fog_state = (uint32_t)(v->f & 0xFF) | state << 8;
    if (ev->P->fst) { o->s = (float)v->u; o->t = (float)v->v; }
    else { o->s = v->s; o->t = v->t; o->q = v->q; }
    uint32_t idx = (uint32_t)(A_vtx.n / sizeof *o) - 1;
    vcache[vcache_pos] = *v; vcache_idx[vcache_pos] = idx; vcache_state[vcache_pos] = state;
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
    if (!own_mem_is_gpu(psm, S.tbp, S.tbw, u0, v0, u1, v1)) {   /* memory source (vram_buf) */
        own_mem_read(psm, S.tbp, S.tbw, u0, v0, u1, v1);       /* flushes pending patches */
        ts->kind = S.indexed ? TK_VRAM_IDX : TK_VRAM_DIRECT;
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

/* ---- ALIAS16 ------------------------------------------------------------------------------------- */
static const uint16_t* tab16(int psm) { return psm == PSMCT16S ? gs_swz_ct16s : gs_swz_ct16; }
#define BIT_GET(m, i) ((m)[(i) >> 6] >> ((i) & 63) & 1)
#define BIT_SET(m, i) ((m)[(i) >> 6] |= 1ull << ((i) & 63))

static uint16_t ablk_d[GPU_BLOCKS], ablk_s[GPU_BLOCKS];
static uint32_t ablk_stamp[GPU_BLOCKS], ablk_ctr;

static void own_abs_blocks(gpu_target* T, const uint16_t* list, int n, int writes) {
    /* absolute blocks of a CT16 view -> the C32 target's local blocks */
    for (int k = 0; k < n; k++) {
        uint32_t b = list[k], page = b >> 5;
        int rel = (int)((page - T->base) & 511u);
        int prow = rel / T->fbw, pcol = rel % T->fbw;
        if (prow * 32 >= T->rows) continue;
        /* find the local block of this page holding b (8 x 4 blocks per CT32 page) */
        for (int j = 0; j < 32; j++) {
            int bx = pcol * 8 + (j & 7), by = prow * 4 + (j >> 3);
            if (T->babs[by * T->bcols + bx] == b) {
                own_draw(T, bx * 8, by * 8, bx * 8 + 7, by * 8 + 7, writes, 0);
                break;
            }
        }
    }
}

static int alias_sprite(const gs_env_view* ev, const gs_sprite* sp, const texsrc_t* ts, uint32_t state) {
    const gs_prim* P = ev->P;
    gpu_target* T = S.ct;
    const uint16_t* tf = tab16(P->fpsm);
    const uint16_t* tt = S.tme ? tab16(S.tpsm) : NULL;
    uint32_t fbp = P->fpage * 32;
    int hazard = 0, self = 0;
    ablk_ctr++;
    int nd = 0, ns = 0;
    /* pass 1: hazards against the open dispatch, this sprite's own writes */
    for (int y = sp->py0; y <= sp->py1; y++) {
        for (int x = sp->px0; x <= sp->px1; x++) {
            uint32_t h = gs_addr16(tf, fbp, P->fbw, (uint32_t)x, (uint32_t)y);
            if (BIT_GET(aw_bits, h) || BIT_GET(ar_bits, h)) hazard = 1;
            BIT_SET(ad_bits, h);
            uint32_t b = h >> 7;
            if (ablk_stamp[b] != ablk_ctr) { ablk_stamp[b] = ablk_ctr; ablk_d[nd++] = (uint16_t)b; }
        }
    }
    ablk_ctr++;
    if (tt) {
        for (int y = sp->py0; y <= sp->py1; y++) {
            int32_t vv = gs_d2i32((sp->pl.v.c0 + sp->pl.v.cy * y) * (double)(1 << GS_UVSH)) >> GS_UVSH >> 4;
            int32_t uu = sp->ua;
            for (int x = sp->px0; x <= sp->px1; x++, uu += sp->du) {
                uint32_t h = gs_addr16(tt, S.tbp, S.tbw, (uint32_t)(uu >> GS_UVSH >> 4) & 2047u, (uint32_t)vv & 2047u);
                if (BIT_GET(aw_bits, h)) hazard = 1;
                if (BIT_GET(ad_bits, h)) self = 1;
                uint32_t b = h >> 7;
                if (ablk_stamp[b] != ablk_ctr) { ablk_stamp[b] = ablk_ctr; ablk_s[ns++] = (uint16_t)b; }
            }
        }
    }
    if (hazard && AB.open) close_alias();
    /* ownership: source reads, destination writes (both lanes) */
    if (tt) {
        if (ts->T) own_abs_blocks(ts->T, ablk_s, ns, 0);
        else own_mem_read(S.tpsm, S.tbp, S.tbw, ts->u0, ts->v0, ts->u1, ts->v1);
    }
    if (self) { gf.alias16_selfcopy++; gf.shadow_copies++; }
    own_abs_blocks(T, ablk_d, nd, 1);
    /* pass 2: record the sprite in the dispatch */
    for (int y = sp->py0; y <= sp->py1; y++)
        for (int x = sp->px0; x <= sp->px1; x++) {
            uint32_t h = gs_addr16(tf, fbp, P->fbw, (uint32_t)x, (uint32_t)y);
            BIT_SET(aw_bits, h);
            ad_bits[h >> 6] = 0;
        }
    if (tt) {
        for (int y = sp->py0; y <= sp->py1; y++) {
            int32_t vv = gs_d2i32((sp->pl.v.c0 + sp->pl.v.cy * y) * (double)(1 << GS_UVSH)) >> GS_UVSH >> 4;
            int32_t uu = sp->ua;
            for (int x = sp->px0; x <= sp->px1; x++, uu += sp->du)
                BIT_SET(ar_bits, gs_addr16(tt, S.tbp, S.tbw, (uint32_t)(uu >> GS_UVSH >> 4) & 2047u, (uint32_t)vv & 2047u));
        }
    }
    if (!AB.open || AB.T != T) {
        if (AB.open) close_alias();
        AB.open = 1; AB.T = T; AB.first = rec_count(sizeof(spriterec)); AB.count = 0;
    }
    spriterec* r = ar_alloc(&A_rec, sizeof *r);
    r->x0 = (int16_t)sp->px0; r->y0 = (int16_t)sp->py0; r->x1 = (int16_t)(sp->px1 + 1); r->y1 = (int16_t)(sp->py1 + 1);
    r->ua = sp->ua; r->du = sp->du; r->va = sp->va; r->dv = gs_d2i32(sp->pl.v.cy * (double)(1 << GS_UVSH));
    r->state = state;
    gf.records_sprite++;
    gf.rec_bytes += sizeof *r;
    AB.count++;
    gf.alias16_sprites++;
    return 1;
}

/* ---- tiled fills -> clears ------------------------------------------------------------------------ */
typedef struct {
    int active;
    gpu_target *ct, *zt;
    uint32_t color, z, wmask, zw;
    int64_t area;
    int x0, y0, x1, y1;
} fill_acc;
static fill_acc FA;

static void fill_add(const gs_env_view* ev, const gs_sprite* sp, const gs_vtx* v1) {
    const gs_prim* P = ev->P;
    gpu_target* ct = S.cwrite ? S.ct : NULL;
    gpu_target* zt = S.zwrite ? S.zt : NULL;
    uint32_t color = ((uint32_t)v1->r | (uint32_t)v1->g << 8 | (uint32_t)v1->b << 16 | (uint32_t)v1->a << 24) | (uint32_t)P->fba << 24;
    if (!FA.active || FA.ct != ct || FA.zt != zt || FA.color != color || FA.z != v1->z || FA.wmask != (uint32_t)S.wmask) {
        memset(&FA, 0, sizeof FA);
        FA.active = 1; FA.ct = ct; FA.zt = zt; FA.color = color; FA.z = v1->z; FA.wmask = (uint32_t)S.wmask;
        FA.x0 = sp->px0; FA.y0 = sp->py0; FA.x1 = sp->px1; FA.y1 = sp->py1;
    }
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
        if (ct) {
            gpu_fop_clear* c = gf_cmd(GPU_FOP_CLEAR, sizeof *c);
            c->target = ct->id; c->value = color; c->mask = (uint32_t)S.wmask;
            gf.clears_c++;
        }
        if (zt) {
            gpu_fop_clear* c = gf_cmd(GPU_FOP_CLEAR, sizeof *c);
            c->target = zt->id; c->zclear = 1; c->value = v1->z;
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

static uint32_t zclamp(const gs_prim* P, int64_t z) {
    int64_t zi = z >> 16;
    return zi < 0 ? 0 : zi > (int64_t)P->zmax ? P->zmax : (uint32_t)zi;
}

static void fr_prim(const gs_env_view* ev, int type, const gs_vtx* a, const gs_vtx* b, const gs_vtx* c) {
    uint64_t t0 = __rdtsc();
    const gs_prim* P = ev->P;
    gf.kicks++;
    if (!ev->draws) { gf.nowrite++; goto out; }
    if (S.version != gs_prim_version || S.ctx != ev->ctx || !S.version) state_update(ev);
    if (S.skip) { gf.skipped++; goto out; }
    int cls = pc_of(type);

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
        if (f == GPU_ZF_32 && zh > 0x3F000000u) { z32_high = 1; gf.z32_over++; if (S.zt) S.zt->z_high = 1; }
    }

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

    /* ---- tier ---- */
    int tier = GPU_TIER_FF, why = -1, punt = S.punt;
    if (!punt && ts.punt) punt = ts.punt;
    if (!punt && S.alias) {
        if (cls != GPU_PC_SPRITE) punt = GPU_PUNT_ALIAS16_PRIM + 1;
        else if (P->tme && (ts.kind == TK_VIEW_C32 || ts.kind == TK_VIEW_ALPHA || (S.tpsm != PSMCT16 && S.tpsm != PSMCT16S) ||
                            P->texm != 1))
            punt = GPU_PUNT_ALIAS16_TEX + 1;
    }
    if (!punt && !S.alias && ts.kind == TK_ALIAS16) punt = GPU_PUNT_TEX_FMT + 1;   /* CT16 view outside ALIAS16 */
    int outcome = 0;
    if (S.fatst == FATST_SPLIT) {
        int n = S.apre[as_hi + 1] - S.apre[as_lo];
        outcome = n == as_hi - as_lo + 1 ? 1 : n == 0 ? 2 : 4;
        gf.atst_kicks++;
        if (outcome == 1) gf.atst_pass++; else if (outcome == 2) gf.atst_fail++; else gf.atst_straddle++;
    }
    if (S.cd_dep && S.c_as) { gf.cd_as_kicks++; if (as_hi > 128) gf.as_over128++; }
    if (punt) tier = GPU_TIER_PUNT;
    else if (S.alias) tier = GPU_TIER_ALIAS16;
    else {
        if (S.rov) why = S.rov - 1;
        else if (S.cd_dep && S.c_as && as_hi > 128) why = GPU_ROV_C_OVER128;
        else if (z32_high || (S.zt && S.zt->z_high)) why = GPU_ROV_Z32_HIGH;
        else if (outcome == 4 && gf_cfg.straddle_rov) why = GPU_ROV_STRADDLE;
        if (why >= 0) tier = GPU_TIER_ROV;
    }
    gf.tier[tier]++;
    if (why >= 0) gf.rov_why[why]++;
    if (punt) {
        gf.punt_why[punt - 1]++;
        /* PUNT (5.4): flush, download the pages it touches, sw draws it, reload: counted only here */
        flush_open();
        goto out;
    }
    if (P->tme) {
        switch (ts.kind) {
        case TK_VIEW_C32: gf.tex_view_c32++; break;
        case TK_VIEW_ALPHA: gf.tex_view_alpha++; break;
        case TK_ALIAS16: gf.tex_alias16++; break;
        default: gf.tex_vram++; break;
        }
    }
    uint32_t texsrc = P->tme ? (uint32_t)ts.kind | (ts.T ? (uint32_t)ts.T->id << 8 : 0) : 0;
    uint32_t state = state_for(ev, texsrc);
    if ((int)state != prev_state_index) { gf.state_changes++; prev_state_index = (int)state; }

    if (tier == GPU_TIER_ALIAS16) {
        if (OD.open || RR.open) { close_draw(); close_run(); }
        alias_sprite(ev, &sp, &ts, state);
        goto out;
    }
    if (AB.open) close_alias();

    /* ---- ownership: texture views, feedback, destination ---- */
    if (ts.T) {
        own_view_read(ts.T, ts.u0, ts.v0, ts.u1, ts.v1);
        if ((ts.T == S.ct && (S.tex_lanes & S.c_lanes)) || ts.T == S.zt) feedback_check(ts.T, ts.u0, ts.v0, ts.u1, ts.v1);
    }
    int rov = tier == GPU_TIER_ROV;
    int opaque = cls == GPU_PC_SPRITE && S.opaque && !rov;
    if (S.ct && (S.cwrite || S.reads_dest || rov)) own_draw(S.ct, x0, y0, x1, y1, S.cwrite, opaque);
    if (S.zt) own_draw(S.zt, x0, y0, x1, y1, S.zwrite, opaque && !S.zread);
    if (S.ct) S.ct->last_used = gf_frame;

    /* ---- pipeline key and draw merging ---- */
    int tk = P->tme ? ts.kind : TK_NONE;
    int zmode = S.zfi >= 0 ? (rov ? 2 : 1) : 0;
    uint32_t ps_key = (uint32_t)(rov ? 2 : S.blend == BL_DUAL ? 0 : 1) | (uint32_t)tk << 2 | (uint32_t)cls << 5 |
                      (uint32_t)zmode << 7 | (uint32_t)(P->tme && S.stq) << 9 | (uint32_t)(S.fatst == FATST_DISCARD) << 10;
    uint16_t ctid = S.ct ? S.ct->id : 0, ztid = S.zt ? S.zt->id : 0, texid = ts.T ? ts.T->id : 0;
    if (rov) {
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
     * texture kind, primitive class, STQ and the bound texture split its draws */
    if (rov) key = (uint64_t)(ps_key & ~(3u << 7)) | (uint64_t)texid << 32 | 1ull << 63;
    else key = (uint64_t)ps_key | (uint64_t)S.blend << 12 | (uint64_t)S.depth_class << 16 | (uint64_t)S.wmask << 20 |
               (uint64_t)(S.fatst == FATST_SPLIT) << 24 | (uint64_t)ctid << 32 | (uint64_t)ztid << 40 | (uint64_t)texid << 48;
    uint64_t sckey = (uint64_t)P->scx0 | (uint64_t)P->scy0 << 11 | (uint64_t)P->scx1 << 22 | (uint64_t)P->scy1 << 33;
    if (!rov) key ^= sckey * 0x9E3779B97F4A7C15ull;
    if (!OD.open || OD.key != key) {
        if (trace_on && OD.open) fprintf(stderr, "[front] key %016llX -> %016llX\n", (unsigned long long)OD.key, (unsigned long long)key);
        close_draw();
        OD.open = 1;
        OD.key = key;
        OD.outcomes = 0;
        OD.prims = 0;
        memset(&OD.d, 0, sizeof OD.d);
        OD.d.tier = (uint8_t)tier; OD.d.cls = (uint8_t)cls; OD.d.blend = (uint8_t)S.blend; OD.d.depth = (uint8_t)S.depth_class;
        OD.d.ps_key = ps_key; OD.d.ct = ctid; OD.d.zt = ztid; OD.d.tex = texid; OD.d.wmask = (uint8_t)S.wmask;
        OD.d.sc[0] = (int16_t)P->scx0; OD.d.sc[1] = (int16_t)P->scy0; OD.d.sc[2] = (int16_t)P->scx1; OD.d.sc[3] = (int16_t)P->scy1;
        OD.d.first = cls == GPU_PC_TRI ? (uint32_t)(A_idx.n / 4) : 0;
        OD.d.vtx_first = (uint32_t)(A_vtx.n / sizeof(gvtx));
        OD.d.rec_first = (uint32_t)A_rec.n;
    }
    OD.outcomes |= outcome;
    if (!OD.prims) { OD.bbox[0] = x0; OD.bbox[1] = y0; OD.bbox[2] = x1; OD.bbox[3] = y1; }
    else { if (x0 < OD.bbox[0]) OD.bbox[0] = x0; if (y0 < OD.bbox[1]) OD.bbox[1] = y0; if (x1 > OD.bbox[2]) OD.bbox[2] = x1; if (y1 > OD.bbox[3]) OD.bbox[3] = y1; }
    if (!OD.prims) { OD.alpha = ev->reg[R_ALPHA_1 + ev->ctx]; OD.test = ev->reg[R_TEST_1 + ev->ctx]; OD.frame = ev->reg[R_FRAME_1 + ev->ctx]; }
    OD.prims++;

    /* ---- emission ---- */
    switch (cls) {
    case GPU_PC_TRI: {
        uint32_t* ix = ar_alloc(&A_idx, 12);
        for (int i = 0; i < 3; i++) ix[i] = emit_vertex(vs[i], ev, state);
        gf.indices += 3;
        gf.idx_bytes += 12;
        OD.d.count += 3;
        if (rov) {
            gs_planes pl;
            gs_tri_planes(&T, vs, ev->iip, P->fge, P->zuse, P->tme, P->fst, &pl);
            gs_rowstep rs;
            gs_rowstep_init(&rs, &pl, T.by0);
            fullrec* r = ar_alloc(&A_rec, sizeof *r);
            r->zrow = rs.row[GS_A_Z]; r->zdy = rs.dy[GS_A_Z]; r->zdx = rs.dx[GS_A_Z];
            static const int map[7] = { GS_A_R, GS_A_G, GS_A_B, GS_A_A, GS_A_F, GS_A_U, GS_A_V };
            for (int k = 0; k < 7; k++) {
                r->row[k] = gs_sat32(rs.row[map[k]]); r->dy[k] = gs_sat32(rs.dy[map[k]]); r->dx[k] = gs_sat32(rs.dx[map[k]]);
            }
            r->by0 = (int16_t)T.by0;
            gf.records_full++;
            gf.rec_bytes += sizeof *r;
        } else if (zmode) {
            gs_plane zp;
            gs_tri_plane(&T, &zp, a->z, b->z, c->z);
            zrec* r = ar_alloc(&A_rec, sizeof *r);
            gs_rowstep_attr(&zp, T.by0, 65536.0, 1, &r->row, &r->dy, &r->dx);
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
        r->state = state;
        gf.records_sprite++;
        gf.rec_bytes += sizeof *r;
        OD.d.count++;
        if (S.fill) fill_add(ev, &sp, b);
        else FA.active = 0;
        break;
    }
    case GPU_PC_LINEPIX: {
        gs_planes pl;
        gs_line_planes(a, b, ev->iip, &pl);
        for (int p = L.p0; p <= L.p1; p++) {
            int q, qn, cov[2];
            double t = gs_line_step(&L, p, aa, &q, cov, &qn);
            for (int k = 0; k < qn; k++) {
                if (!cov[k]) continue;
                int px = L.xmajor ? p : q + k, py = L.xmajor ? q + k : p;
                if (px < P->scx0 || px > P->scx1 || py < P->scy0 || py > P->scy1 || row_masked(P, py)) continue;
                gs_span_start st;
                gs_span_start_at(&pl, t, 0, &st);
                if (aa) st.a = cov[k] << 16;
                linerec* r = ar_alloc(&A_rec, sizeof *r);
                r->px = (int16_t)px; r->py = (int16_t)py;
                r->z = S.zfi >= 0 ? zclamp(P, st.z) : 0;
                r->rgba = (uint32_t)clamp_byte(st.r >> 16) | (uint32_t)clamp_byte(st.g >> 16) << 8 |
                          (uint32_t)clamp_byte(st.b >> 16) << 16 | (uint32_t)clamp_byte(st.a >> 16) << 24;
                r->fog_state = (uint32_t)clamp_byte(st.f >> 16) | state << 8;
                if (P->fst) { r->s = (float)st.u; r->t = (float)st.v; } else { r->s = st.s; r->t = st.t; r->q = st.q; }
                gf.linepix++;
                gf.records_linepix++;
                gf.rec_bytes += sizeof *r;
                OD.d.count++;
            }
        }
        break;
    }
    default: {
        linerec* r = ar_alloc(&A_rec, sizeof *r);
        r->px = (int16_t)x0; r->py = (int16_t)y0;
        r->z = S.zfi >= 0 ? zclamp(P, (int64_t)a->z << 16) : 0;
        r->rgba = (uint32_t)a->r | (uint32_t)a->g << 8 | (uint32_t)a->b << 16 | (uint32_t)a->a << 24;
        r->fog_state = (uint32_t)(a->f & 0xFF) | state << 8;
        r->s = P->fst ? (float)a->u : a->s; r->t = P->fst ? (float)a->v : a->t; r->q = a->q;
        gf.linepix++;
        gf.records_linepix++;
        gf.rec_bytes += sizeof *r;
        OD.d.count++;
        break;
    }
    }
    if (cls != GPU_PC_SPRITE) FA.active = 0;
out:
    tsc_acc += __rdtsc() - t0;
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
        own_mem_read(cpsm == PSMCT16 || cpsm == PSMCT16S ? (int)cpsm : PSMCT32, cbp, 1, 0, 0, t8 ? 15 : 7, t8 ? 15 : 1);
    }
    clut_mark_dirty();
    tsc_acc += __rdtsc() - t0;
}

static void fr_reset(void) { flush_open(); }

static const gs_front_hooks hooks = { fr_prim, fr_xfer_start, fr_xfer_data, fr_tex0, fr_reset };

/* ---- start / stop / VBlank ------------------------------------------------------------------------- */
static void frame_begin(void) {
    memset(&gf, 0, sizeof gf);
    A_cmd.n = A_vtx.n = A_idx.n = A_rec.n = A_state.n = A_data.n = 0;
    memset(smap_idx, 0, sizeof smap_idx);
    nstates = 0;
    S.state_valid = 0;
    S.version = 0;                       /* targets may have been released */
    prev_state_index = -1;
    vcache_n = 0;
    cw_n = 0;
}

void gpu_front_start(const gpu_front_config* cfg) {
    memset(&gf_cfg, 0, sizeof gf_cfg);
    if (cfg) gf_cfg = *cfg;
    GS_INIT();
    if (!aw_bits) {
        aw_bits = calloc(1, (1u << 21) / 8);
        ar_bits = calloc(1, (1u << 21) / 8);
        ad_bits = calloc(1, (1u << 21) / 8);
    }
    own_reset();
    clut_reset();
    memset(&S, 0, sizeof S);
    memset(&OD, 0, sizeof OD);
    memset(&RR, 0, sizeof RR);
    memset(&AB, 0, sizeof AB);
    memset(&FA, 0, sizeof FA);
    gf_frame = 0;
    frame_begin();
    tsc_acc = 0;
    tsc_start = __rdtsc();
    QueryPerformanceCounter(&qpc_start);
    gs_sw_draw = gf_cfg.sw;
    const char* tr = getenv("PS2_GPU_FRONT_TRACE");
    trace_vblank = tr ? (uint32_t)strtoul(tr, NULL, 0) : 0xFFFFFFFFu;
    trace_on = 0;
    gs_front = &hooks;
}

void gpu_front_stop(void) {
    gs_front = NULL;
    gs_sw_draw = 1;
}

void gpu_front_vblank(const uint64_t priv[16], uint32_t vblank) {
    uint64_t t0 = __rdtsc();
    flush_open();
    own_frame_end(priv);
    clut_frame_end();
    gpu_fop_present* p = gf_cmd(GPU_FOP_PRESENT, sizeof *p);
    p->vblank = vblank;
    tsc_acc += __rdtsc() - t0;
    /* calibrate the TSC against QPC */
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    double secs = (double)(q.QuadPart - qpc_start.QuadPart) / (double)f.QuadPart;
    if (secs > 0.05) tsc_hz = (double)(__rdtsc() - tsc_start) / secs;
    gf.vblank = vblank;
    gf.front_ms = (double)tsc_acc * 1000.0 / tsc_hz;
    gf.cmd_bytes = A_cmd.n;
    gf.data_bytes = A_data.n;
    if (sink_fn) {
        gpu_front_frame_data fd = { A_cmd.p, A_cmd.n, A_vtx.p, A_vtx.n, (const uint32_t*)A_idx.p, A_idx.n / 4,
                                    A_rec.p, A_rec.n, A_state.p, A_state.n / sizeof(staterec), A_data.p, A_data.n, vblank };
        sink_fn(&fd, sink_user);
    }
    if (ndone == capdone) {
        capdone = capdone ? capdone * 2 : 1024;
        done = realloc(done, capdone * sizeof *done);
        if (!done) { fprintf(stderr, "[gpu_front] out of memory\n"); exit(1); }
    }
    done[ndone++] = gf;
    tsc_acc = 0;
    gf_frame++;
    frame_begin();
    trace_on = vblank + 1 == trace_vblank;
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
