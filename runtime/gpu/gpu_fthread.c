/* Frontend thread (notes/d3d11-renderer.md 2.1, milestone M2): the GS thread decodes the register
 * stream (gs_draw.c, unchanged) and the frontend (gpu_front.c) runs on a thread of its own, so the
 * two halves of the GS-side work overlap instead of adding up.
 *
 * GS thread (proxy hooks installed as gs_front): every primitive is copied into a single-producer /
 * single-consumer ring with its vertices and, when the decoded state moved on (gs_prim_version), the
 * 8-byte words of gs_prim and of the general registers that changed since the last copy (the texture
 * level bindings of the software rasterizer are not sent: the frontend never reads them). Transfers
 * are forwarded with the contents of the 256-byte blocks they wrote (taken right after the transfer,
 * before anything else is posted), CLUT loads with the CLUT buffer when it changed, then resets and
 * VBlanks. Requires the software rasterizer off (only transfers write GS memory then).
 *
 * Frontend thread: applies the copies to its own gs_prim / register / GS memory / CLUT mirrors and
 * calls the frontend's hooks in stream order, so its output is identical to the same-thread frontend
 * (gs_replay --stream-hash checks it). VBlanks close the frame there and call the sink (the render
 * thread's producer, gpu_exec.c). */
#include "gpu_front_int.h"
#include "gs_mem_swizzle.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <windows.h>
#include <x86intrin.h>

#define RING (8u << 20)                  /* small enough to stay in the L3 cache */
#define PUBLISH_EVERY (64u << 10)
#define SPINS 400                        /* pause loops before sleeping (a pause is 40-140 cycles) */

enum { FR_PAD, FR_PRIM, FR_STATE, FR_XFER, FR_XDATA, FR_BLOCKS, FR_CLUT, FR_TEX0, FR_RESET, FR_VBLANK, FR_FENCE, FR_STOP };
typedef struct { uint32_t kind, size; } fr_hdr;            /* size: whole record, multiple of 8 */
/* Primitives are the bulk of the stream (50k per frame): 8 bytes plus the vertices not already in
 * the consumer's 4-entry vertex cache (strips and fans repeat two of three), 32 bytes each (every
 * gs_vtx field the decoder can produce fits: x/y 16 bits, colour and fog bytes, u/v 14 bits). */
typedef struct { uint16_t x, y; uint32_t z; uint8_t r, g, b, a, f, pad[3]; float s, t, q; uint16_t u, v; } fr_vtx;
_Static_assert(sizeof(fr_vtx) == 32, "compact vertex");
typedef struct { fr_hdr h; uint8_t type, nv, ref[3], pad[3]; fr_vtx v[]; } fr_prim;   /* ref: slot, | 0x80 = next v[] */
typedef struct { uint32_t idx, pad; uint64_t val; } fr_word;   /* idx < 0x10000: gs_prim word, else register */
/* the environment of a state version: changed words of gs_prim / registers, and the per-version fields */
typedef struct { fr_hdr h; uint32_t n, version; int32_t ofx, ofy; uint8_t ctx, iip, aa1, draws, pad[4]; fr_word w[]; } fr_state;
typedef struct { fr_hdr h; uint64_t bb, pos, rr; uint32_t dir, pad; } fr_xfer;
typedef struct { fr_hdr h; uint32_t bytes, pad; } fr_xdata;
typedef struct { fr_hdr h; uint32_t n, pad; uint16_t id[]; } fr_blocks;   /* ids padded to 8, then n x 256 bytes */
typedef struct { fr_hdr h; uint64_t mask; uint16_t c[]; } fr_clut;   /* 16-byte pieces of the CLUT buffer (mask bit i: halves 8i..8i+7) */
typedef struct { fr_hdr h; uint32_t ctx, flags; uint64_t tex0; } fr_tex0;   /* flags: 1 loaded, 2 changed */
typedef struct { fr_hdr h; uint64_t priv[16]; uint32_t vblank, pad; } fr_vblank;
typedef struct { fr_hdr h; volatile long* done; } fr_fence;

static uint8_t* ring;
/* The shared positions sit on their own cache lines and are touched rarely (the producer re-reads
 * rd only when its remembered room runs out, the consumer stores rd every 64 KB or when the producer
 * waits): a shared line per record costs a cross-core transfer per record. */
static _Alignas(64) _Atomic uint64_t pub;            /* published by the producer */
static _Alignas(64) _Atomic uint64_t rd;             /* consumed by the frontend thread */
static _Alignas(64) _Atomic int cons_sleeping;
static _Alignas(64) _Atomic int prod_waiting;
static _Atomic uint64_t prod_need;      /* a waiting producer is woken once rd reaches this */
static _Alignas(64) _Atomic uint32_t prod_wake;   /* bumped (and woken) when it does */
static _Alignas(64) uint64_t wr, unpublished, room_end;   /* producer */
static HANDLE thread;

/* ---- producer (GS thread) ------------------------------------------------------------------------ */

static void publish(void) {
    unpublished = 0;
    if (atomic_load_explicit(&pub, memory_order_relaxed) == wr) return;
    _mm_sfence();                        /* the records were written with non-temporal stores */
    atomic_store_explicit(&pub, wr, memory_order_seq_cst);
    if (atomic_load_explicit(&cons_sleeping, memory_order_seq_cst)) WakeByAddressSingle((PVOID)&pub);
}

static int drop_mode;                    /* (diagnostics: PS2_GPU_FTHREAD_DROP=1 measures the proxy alone) */
/* Records are built in a small staging buffer (hot in L1) and streamed into the ring with
 * non-temporal stores: a normal store to a ring line the frontend thread read last would first fetch
 * it from that core (read for ownership), about one cross-core transfer per 64 bytes on each side. */
static _Alignas(64) uint8_t stage[(2u << 20) + 64];
static uint64_t stage_pos;              /* ring position of the staged record */

static void* reserve(uint32_t kind, uint32_t bytes) {
    bytes = (bytes + 15) & ~15u;
    if (drop_mode) { fr_hdr* h = (fr_hdr*)stage; h->kind = kind; h->size = bytes; return h; }
    uint64_t pos = wr % RING;
    uint64_t pad = pos + bytes > RING ? RING - pos : 0;
    for (int spins = 0; wr + pad + bytes > room_end; spins++) {
        uint64_t r = atomic_load_explicit(&rd, memory_order_acquire);
        room_end = r + RING;
        if (wr + pad + bytes <= room_end) break;
        publish();
        if (spins < 64) { _mm_pause(); continue; }
        /* full: the frontend is behind; sleep until a quarter of the ring is free (waking for every
         * record would ping-pong the two threads through the kernel) */
        uint64_t need = wr + pad + bytes - RING + RING / 4;
        atomic_store_explicit(&prod_need, need, memory_order_seq_cst);
        atomic_store_explicit(&prod_waiting, 1, memory_order_seq_cst);
        /* sleep on a word of its own: rd keeps moving while the frontend works, and WaitOnAddress
         * returns at once when the word differs from the value given (a busy loop on rd) */
        for (;;) {
            uint32_t w = atomic_load_explicit(&prod_wake, memory_order_seq_cst);
            if ((r = atomic_load_explicit(&rd, memory_order_seq_cst)) >= need) break;
            WaitOnAddress((volatile VOID*)&prod_wake, &w, sizeof w, 5);
        }
        atomic_store_explicit(&prod_waiting, 0, memory_order_relaxed);
        room_end = r + RING;
    }
    if (pad) {
        fr_hdr* p = (fr_hdr*)(ring + pos);
        p->kind = FR_PAD;
        p->size = (uint32_t)pad;
        wr += pad;
    }
    stage_pos = wr % RING;
    fr_hdr* h = (fr_hdr*)stage;
    h->kind = kind;
    h->size = bytes;
    return h;
}

static uint64_t st_n[16], st_b[16], st_empty_state;      /* records / bytes per kind (PS2_GPU_FTHREAD_STATS) */

static void commit(const void* rec) {
    uint32_t n = ((const fr_hdr*)rec)->size;
    st_n[((const fr_hdr*)rec)->kind & 15]++;
    st_b[((const fr_hdr*)rec)->kind & 15] += n;
    if (drop_mode) return;
    const __m128i* s = (const __m128i*)rec;
    __m128i* d = (__m128i*)(ring + stage_pos);
    for (uint32_t i = 0; i < n / 16; i++) _mm_stream_si128(d + i, _mm_load_si128(s + i));
    wr += n;
    unpublished += n;
    if (unpublished >= PUBLISH_EVERY) publish();
}

/* state sent last (words of gs_prim outside the texture level bindings, registers) */
#define P_WORDS (sizeof(gs_prim) / 8)
static uint64_t sentP[P_WORDS], sentR[0x64];
static uint32_t sent_version;
static int have_sent, sent_env[6], clut_pending;
static uint32_t front_version;            /* the frontend's state version: bumped only when something it reads changed */
static uint32_t p_list_n;              /* wlist entries that are gs_prim words */
static uint16_t pclut[512];
static int xfer_open, xfer_data_seen;    /* a transfer whose blocks go out with the next record */
static int xf_psm, xf_x0, xf_y0, xf_x1, xf_y1;
static uint32_t xf_bp, xf_bw;
static uint16_t blist[GPU_BLOCKS];

static void send_blocks(void) {
    if (!xfer_open || !xfer_data_seen) return;
    xfer_data_seen = 0;
    int n = own_rect_blocks(xf_psm, xf_bp, xf_bw, xf_x0, xf_y0, xf_x1, xf_y1, blist, GPU_BLOCKS);
    for (int i = 0; i < n;) {
        int m = n - i > 4096 ? 4096 : n - i;
        uint32_t ids = ((uint32_t)m * 2 + 7) & ~7u;
        fr_blocks* r = reserve(FR_BLOCKS, (uint32_t)sizeof(fr_blocks) + ids + (uint32_t)m * 256);
        r->n = (uint32_t)m;
        uint8_t* data = (uint8_t*)r->id + ids;
        for (int k = 0; k < m; k++) {
            r->id[k] = blist[i + k];
            memcpy(data + (size_t)k * 256, gs_vram + (size_t)blist[i + k] * 256, 256);
        }
        commit(r);
        i += m;
    }
}

/* What the frontend reads of the decoded state: these gs_prim fields and general registers
 * (gpu_front.c state_lookup / state_update / state_for / fr_prim and the helpers it calls). Only their
 * 8-byte words are compared and sent when the state version moves on (Sly starts a new version for
 * almost every draw by rewriting TEX0, ~10k per frame, so a full compare would cost more than the
 * decode). A frontend change that reads another field must add it here: the equivalence check
 * (gs_replay --stream-hash, threaded vs same-thread, scripts/gpu_fthread_check.ps1) finds omissions,
 * and PS2_GPU_FTHREAD_FULL=1 sends every word instead. */
#define PF(f) offsetof(gs_prim, f)
static const uint16_t p_fields[] = {
    PF(ffmt), PF(fpsm), PF(fpage), PF(fbw), PF(fbmsk), PF(fmask), PF(zfmt), PF(zpsm), PF(ztst), PF(zmsk), PF(zuse),
    PF(zpage), PF(zbw), PF(zmax), PF(ate), PF(atst), PF(aref), PF(afail), PF(date), PF(datm), PF(abe), PF(asel), PF(bsel), PF(csel),
    PF(dsel), PF(fix), PF(pabe), PF(fba), PF(colclamp), PF(dthe), PF(fge), PF(tme), PF(fst), PF(tfx), PF(tcc), PF(texm),
    PF(tw16), PF(th16), PF(scanmsk), PF(scx0), PF(scx1), PF(scy0), PF(scy1), PF(iip),
};
#undef PF
static const uint8_t r_used[] = {
    0x00, 0x06, 0x07, 0x08, 0x09, 0x14, 0x15, 0x18, 0x19, 0x1A, 0x1B, 0x22, 0x34, 0x35, 0x36, 0x37, 0x3B, 0x3D, 0x40, 0x41,
    0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
};
static uint16_t wlist[P_WORDS + 0x64];   /* word indices (P words, then 0x10000 + register) */
static uint32_t wlist_n;

static void build_wlist(int full, int apass) {
    static uint8_t mark[P_WORDS];
    memset(mark, 0, sizeof mark);
    wlist_n = 0;
    if (full) {
        for (size_t i = 0; i < P_WORDS; i++)
            if (i < offsetof(gs_prim, lvl) / 8 || i >= offsetof(gs_prim, iip) / 8) mark[i] = 1;
    } else {
        for (size_t k = 0; k < sizeof p_fields / sizeof *p_fields; k++) mark[p_fields[k] / 8] = 1;
        if (apass)                       /* (only --selfcheck reads the alpha test table) */
            for (size_t o = offsetof(gs_prim, apass); o < offsetof(gs_prim, apass) + 256; o += 8) mark[o / 8] = 1;
    }
    for (size_t i = 0; i < P_WORDS; i++) if (mark[i]) wlist[wlist_n++] = (uint16_t)i;
    p_list_n = wlist_n;
    if (full) for (uint32_t r = 0; r < 0x64; r++) wlist[wlist_n++] = (uint16_t)(0x8000u | r);
    else for (size_t k = 0; k < sizeof r_used; k++) wlist[wlist_n++] = (uint16_t)(0x8000u | r_used[k]);
}

static fr_word dtmp[P_WORDS + 0x64];

static void send_state(const gs_env_view* ev) {
    const uint8_t* P = (const uint8_t*)ev->P;
    uint32_t n = 0;
    int force = !have_sent;
    for (uint32_t k = 0; k < wlist_n; k++) {
        uint32_t i = wlist[k];
        uint64_t w, *sent;
        if (i & 0x8000u) { i &= 0x7FFFu; w = ev->reg[i]; sent = &sentR[i]; }
        else { memcpy(&w, P + (size_t)i * 8, 8); sent = &sentP[i]; }
        if (w != *sent || force) {
            *sent = w;
            dtmp[n].idx = k < p_list_n ? i : 0x10000u + i;
            dtmp[n].pad = 0;
            dtmp[n++].val = w;
        }
    }
    int env_same = have_sent && ev->ctx == sent_env[0] && ev->ofx == sent_env[1] && ev->ofy == sent_env[2] && ev->iip == sent_env[3] &&
                   ev->aa1 == sent_env[4] && ev->draws == sent_env[5];
    sent_version = ev->version;
    /* Nothing the frontend reads changed (Sly rewrites TEX0 with the same value for ~40% of its draws
     * in ep8): no new version for the frontend, which then skips its state lookup. A CLUT change
     * always starts one (the frontend's state includes the palette's ring slot). */
    if (!n && env_same && !clut_pending) { st_empty_state++; return; }
    clut_pending = 0;
    have_sent = 1;
    sent_env[0] = ev->ctx; sent_env[1] = ev->ofx; sent_env[2] = ev->ofy; sent_env[3] = ev->iip; sent_env[4] = ev->aa1; sent_env[5] = ev->draws;
    fr_state* s = reserve(FR_STATE, (uint32_t)(sizeof(fr_state) + n * sizeof(fr_word)));
    s->n = n;
    s->version = ++front_version;
    s->ofx = ev->ofx; s->ofy = ev->ofy;
    s->ctx = (uint8_t)ev->ctx; s->iip = (uint8_t)ev->iip; s->aa1 = (uint8_t)ev->aa1; s->draws = (uint8_t)ev->draws;
    memset(s->pad, 0, sizeof s->pad);
    memcpy(s->w, dtmp, n * sizeof(fr_word));
    commit(s);
}

/* the consumer's vertex cache as the producer sees it: the decoder's vertex serial per slot */
static uint32_t vserial[4], vage[4], vclock;

static void pack_vtx(fr_vtx* o, const gs_vtx* v) {
    o->x = (uint16_t)v->x; o->y = (uint16_t)v->y; o->z = v->z;
    o->r = (uint8_t)v->r; o->g = (uint8_t)v->g; o->b = (uint8_t)v->b; o->a = (uint8_t)v->a; o->f = (uint8_t)v->f;
    o->pad[0] = o->pad[1] = o->pad[2] = 0;
    o->s = v->s; o->t = v->t; o->q = v->q;
    o->u = (uint16_t)v->u; o->v = (uint16_t)v->v;
}

static void unpack_vtx(gs_vtx* v, const fr_vtx* o) {
    v->x = o->x; v->y = o->y; v->z = o->z;
    v->r = o->r; v->g = o->g; v->b = o->b; v->a = o->a; v->f = o->f;
    v->s = o->s; v->t = o->t; v->q = o->q;
    v->u = o->u; v->v = o->v;
}

static void px_prim(const gs_env_view* ev, int type, const gs_vtx* a, const gs_vtx* b, const gs_vtx* c) {
    send_blocks();
    /* (registers can change without a new state version, e.g. RGBAQ, but the frontend reads only
     * registers whose writes start a new version) */
    if (!have_sent || ev->version != sent_version || ev->ctx != sent_env[0] || ev->ofx != sent_env[1] || ev->ofy != sent_env[2] ||
        ev->iip != sent_env[3] || ev->aa1 != sent_env[4] || ev->draws != sent_env[5])
        send_state(ev);
    int nv = type == 0 ? 1 : type == 1 || type == 2 || type == 6 ? 2 : 3;
    const gs_vtx* vs[3] = { a, b, c };
    uint8_t ref[3] = { 0, 0, 0 };
    int used = 0, nnew = 0;
    const gs_vtx* add[3];
    for (int i = 0; i < nv; i++) {
        uint32_t sr = vs[i]->serial;
        int k = sr == vserial[0] ? 0 : sr == vserial[1] ? 1 : sr == vserial[2] ? 2 : sr == vserial[3] ? 3 : -1;
        if (k < 0 || !sr) {              /* not cached: the oldest slot this primitive does not use */
            k = -1;
            for (int j = 0; j < 4; j++)
                if (!(used & (1 << j)) && (k < 0 || vage[j] < vage[k])) k = j;
            vserial[k] = sr;
            ref[i] = (uint8_t)(k | 0x80);
            add[nnew++] = vs[i];
        } else {
            ref[i] = (uint8_t)k;
        }
        used |= 1 << k;
        vage[k] = ++vclock;
    }
    fr_prim* p = reserve(FR_PRIM, (uint32_t)(sizeof(fr_prim) + (size_t)nnew * sizeof(fr_vtx)));
    p->type = (uint8_t)type; p->nv = (uint8_t)nv;
    p->ref[0] = ref[0]; p->ref[1] = ref[1]; p->ref[2] = ref[2];
    p->pad[0] = p->pad[1] = p->pad[2] = 0;
    for (int i = 0; i < nnew; i++) pack_vtx(&p->v[i], add[i]);
    commit(p);
}

static void px_xfer_start(uint64_t bb, uint64_t pos, uint64_t rr, uint32_t dir) {
    send_blocks();
    xfer_open = 0;
    fr_xfer* x = reserve(FR_XFER, sizeof *x);
    x->bb = bb; x->pos = pos; x->rr = rr; x->dir = dir; x->pad = 0;
    commit(x);
    if (dir == 0 || dir == 2) {          /* destination blocks follow the data (local->local: at once) */
        xfer_open = 1;
        xfer_data_seen = dir == 2;
        xf_psm = (int)((bb >> 56) & 0x3F);
        xf_bp = (uint32_t)((bb >> 32) & 0x3FFF);
        xf_bw = (uint32_t)((bb >> 48) & 0x3F);
        xf_x0 = (int)((pos >> 32) & 0x7FF);
        xf_y0 = (int)((pos >> 48) & 0x7FF);
        xf_x1 = xf_x0 + (int)(rr & 0xFFF);
        xf_y1 = xf_y0 + (int)((rr >> 32) & 0xFFF);
    }
}

static void px_xfer_data(uint32_t bytes) {
    xfer_data_seen = 1;
    fr_xdata* x = reserve(FR_XDATA, sizeof *x);
    x->bytes = bytes; x->pad = 0;
    commit(x);
}

static void px_tex0(int ctx, uint64_t tex0, int clut_loaded) {
    send_blocks();
    uint32_t flags = clut_loaded ? 1u : 0u;
    if (clut_loaded) {
        /* the 16-byte pieces of the CLUT buffer that differ from what was sent (a 16-entry load
         * changes a few of the 64) */
        const uint16_t* c = gs_clut_buffer();
        uint64_t mask = 0;
        for (int i = 0; i < 64; i++) {
            __m128i x = _mm_loadu_si128((const __m128i*)(c + 8 * i)), y = _mm_loadu_si128((const __m128i*)(pclut + 8 * i));
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(x, y)) != 0xFFFF) mask |= 1ull << i;
        }
        if (mask) {
            fr_clut* r = reserve(FR_CLUT, (uint32_t)(sizeof(fr_clut) + 16 * (size_t)__builtin_popcountll(mask)));
            r->mask = mask;
            int k = 0;
            for (uint64_t m = mask; m; m &= m - 1) {
                int i = __builtin_ctzll(m);
                memcpy(pclut + 8 * i, c + 8 * i, 16);
                memcpy(r->c + 8 * k++, c + 8 * i, 16);
            }
            commit(r);
            flags |= 2;
            clut_pending = 1;
        }
    }
    fr_tex0* t = reserve(FR_TEX0, sizeof *t);
    t->ctx = (uint32_t)ctx; t->flags = flags; t->tex0 = tex0;
    commit(t);
}

static void px_reset(void) {
    send_blocks();
    fr_hdr* h = reserve(FR_RESET, sizeof *h);
    commit(h);
}

static const gs_front_hooks proxy = { px_prim, px_xfer_start, px_xfer_data, px_tex0, px_reset };

void fthread_vblank(const uint64_t priv[16], uint32_t vblank) {
    send_blocks();
    fr_vblank* v = reserve(FR_VBLANK, sizeof *v);
    if (priv) memcpy(v->priv, priv, sizeof v->priv);
    else memset(v->priv, 0, sizeof v->priv);
    v->vblank = vblank; v->pad = 0;
    commit(v);
    publish();
}

static void post_wait(uint32_t kind) {
    volatile long done = 0;
    fr_fence* f = reserve(kind, sizeof *f);
    f->done = &done;
    commit(f);
    publish();
    long zero = 0;
    while (!InterlockedCompareExchange(&done, 0, 0)) WaitOnAddress((volatile VOID*)&done, &zero, sizeof zero, 5);
}

void fthread_drain(void) { if (thread && !drop_mode) { send_blocks(); post_wait(FR_FENCE); } }

/* ---- consumer (frontend thread) ------------------------------------------------------------------ */

static gs_prim fP;
static uint64_t fR[0x64];
static gs_env_view fev = { &fP, fR, 0, 0, 0, 0, 0, 0, 0 };
static gs_vtx fslot[4];
static uint8_t* fvram;                   /* GS memory as the frontend's position in the stream sees it */
static uint16_t fclut[512];
static uint64_t cons_rd, pub_seen, rd_stored;

static void store_rd(void) {
    rd_stored = cons_rd;
    atomic_store_explicit(&rd, cons_rd, memory_order_seq_cst);
    if (atomic_load_explicit(&prod_waiting, memory_order_seq_cst) && cons_rd >= atomic_load_explicit(&prod_need, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&prod_wake, 1, memory_order_seq_cst);
        WakeByAddressSingle((PVOID)&prod_wake);
    }
}

static const fr_hdr* next_rec(void) {
    for (;;) {
        if (cons_rd == pub_seen) {
            store_rd();                  /* everything published is consumed: the producer gets its room */
            uint64_t p = atomic_load_explicit(&pub, memory_order_acquire);
            for (int i = 0; p == cons_rd && i < SPINS; i++) { _mm_pause(); p = atomic_load_explicit(&pub, memory_order_acquire); }
            if (p == cons_rd) {
                atomic_store_explicit(&cons_sleeping, 1, memory_order_seq_cst);
                if ((p = atomic_load_explicit(&pub, memory_order_seq_cst)) == cons_rd) {
                    uint64_t seen = cons_rd;
                    WaitOnAddress((volatile VOID*)&pub, &seen, sizeof seen, 10);
                }
                atomic_store_explicit(&cons_sleeping, 0, memory_order_relaxed);
                continue;
            }
            pub_seen = p;
        }
        const fr_hdr* h = (const fr_hdr*)(ring + cons_rd % RING);
        if (h->kind == FR_PAD) { cons_rd += h->size; continue; }
        return h;
    }
}

static void done_rec(const fr_hdr* h) {
    cons_rd += h->size;
    if (cons_rd - rd_stored >= PUBLISH_EVERY ||
        (atomic_load_explicit(&prod_waiting, memory_order_relaxed) && cons_rd >= atomic_load_explicit(&prod_need, memory_order_relaxed)))
        store_rd();
}

static DWORD WINAPI fthread_main(LPVOID arg) {
    (void)arg;
    const gs_front_hooks* H = gf_hooks();
    for (;;) {
        const fr_hdr* h = next_rec();
        int stop = 0;
        switch (h->kind) {
        case FR_PRIM: {
            const fr_prim* p = (const fr_prim*)h;
            const fr_vtx* nv = p->v;
            const gs_vtx* vs[3] = { NULL, NULL, NULL };
            for (int i = 0; i < p->nv; i++) {
                int k = p->ref[i] & 3;
                if (p->ref[i] & 0x80) unpack_vtx(&fslot[k], nv++);
                vs[i] = &fslot[k];
            }
            H->prim(&fev, p->type, vs[0], vs[1], vs[2]);
            break;
        }
        case FR_STATE: {
            const fr_state* s = (const fr_state*)h;
            fev.ctx = s->ctx; fev.ofx = s->ofx; fev.ofy = s->ofy; fev.iip = s->iip; fev.aa1 = s->aa1; fev.draws = s->draws;
            fev.version = s->version;
            for (uint32_t i = 0; i < s->n; i++) {
                if (s->w[i].idx < 0x10000u) memcpy((uint8_t*)&fP + (size_t)s->w[i].idx * 8, &s->w[i].val, 8);
                else fR[s->w[i].idx - 0x10000u] = s->w[i].val;
            }
            break;
        }
        case FR_XFER: { const fr_xfer* x = (const fr_xfer*)h; H->xfer_start(x->bb, x->pos, x->rr, x->dir); break; }
        case FR_XDATA: H->xfer_data(((const fr_xdata*)h)->bytes); break;
        case FR_BLOCKS: {
            const fr_blocks* b = (const fr_blocks*)h;
            const uint8_t* data = (const uint8_t*)b->id + ((b->n * 2 + 7) & ~7u);
            for (uint32_t k = 0; k < b->n; k++) memcpy(fvram + (size_t)b->id[k] * 256, data + (size_t)k * 256, 256);
            break;
        }
        case FR_CLUT: {
            const fr_clut* r = (const fr_clut*)h;
            int k = 0;
            for (uint64_t m = r->mask; m; m &= m - 1) memcpy(fclut + 8 * __builtin_ctzll(m), r->c + 8 * k++, 16);
            break;
        }
        case FR_TEX0: {
            const fr_tex0* t = (const fr_tex0*)h;
            gf_clut_hint = (t->flags & 2) ? 1 : 0;
            H->tex0((int)t->ctx, t->tex0, (int)(t->flags & 1));
            gf_clut_hint = -1;
            break;
        }
        case FR_RESET: H->reset(); break;
        case FR_VBLANK: { const fr_vblank* v = (const fr_vblank*)h; gf_vblank_impl(v->priv, v->vblank); break; }
        case FR_FENCE: case FR_STOP: {
            volatile long* d = ((const fr_fence*)h)->done;
            stop = h->kind == FR_STOP;
            done_rec(h);
            InterlockedExchange(d, 1);
            WakeByAddressAll((PVOID)d);
            if (stop) return 0;
            continue;
        }
        default: break;
        }
        done_rec(h);
    }
}

void fthread_start(void) {
    if (thread) return;
    if (!ring) ring = VirtualAlloc(NULL, RING, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!fvram) fvram = VirtualAlloc(NULL, GS_VRAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ring || !fvram) { fprintf(stderr, "[gpu_front] out of memory for the frontend thread\n"); exit(1); }
    wr = unpublished = cons_rd = pub_seen = rd_stored = 0;
    room_end = RING;
    atomic_store(&pub, 0);
    atomic_store(&rd, 0);
    have_sent = 0;
    clut_pending = 0;
    front_version = 0;
    xfer_open = xfer_data_seen = 0;
    memset(vserial, 0, sizeof vserial);
    build_wlist(getenv("PS2_GPU_FTHREAD_FULL") != NULL, gf_cfg.selfcheck);
    memset(&fP, 0, sizeof fP);
    memset(fR, 0, sizeof fR);
    memcpy(fvram, gs_vram, GS_VRAM_SIZE);    /* the stream starts from this memory */
    memcpy(fclut, gs_clut_buffer(), sizeof fclut);
    memcpy(pclut, fclut, sizeof pclut);
    gf_vram = fvram;
    gf_clut_src = fclut;
    drop_mode = getenv("PS2_GPU_FTHREAD_DROP") != NULL;
    if (drop_mode) { gs_front = &proxy; return; }
    thread = CreateThread(NULL, 0, fthread_main, NULL, 0, NULL);
    if (!thread) { fprintf(stderr, "[gpu_front] cannot start the frontend thread\n"); exit(1); }
    gs_front = &proxy;
}

void fthread_stop(void) {
    if (drop_mode) { drop_mode = 0; return; }
    if (!thread) return;
    send_blocks();
    post_wait(FR_STOP);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (getenv("PS2_GPU_FTHREAD_STATS"))
        fprintf(stderr, "[fthread] empty state records %llu; records/MB: prim %llu/%.1f state %llu/%.1f xfer %llu blocks %llu/%.1f clut %llu tex0 %llu vblank %llu\n",
                (unsigned long long)st_empty_state, (unsigned long long)st_n[FR_PRIM], st_b[FR_PRIM] / 1e6, (unsigned long long)st_n[FR_STATE], st_b[FR_STATE] / 1e6,
                (unsigned long long)st_n[FR_XFER], (unsigned long long)st_n[FR_BLOCKS], st_b[FR_BLOCKS] / 1e6,
                (unsigned long long)st_n[FR_CLUT], (unsigned long long)st_n[FR_TEX0], (unsigned long long)st_n[FR_VBLANK]);
    thread = NULL;
    gf_vram = NULL;
    gf_clut_src = NULL;
}

void fthread_affinity(uint64_t mask) { if (thread && mask) SetThreadAffinityMask(thread, (DWORD_PTR)mask); }

uint64_t fthread_cycles(void) {
    ULONG64 c = 0;
    if (thread) QueryThreadCycleTime(thread, &c);
    return c;
}
