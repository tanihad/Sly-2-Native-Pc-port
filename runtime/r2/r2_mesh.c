/* R2 mesh decoder: a prelit mesh's prebuilt VIF1 DMA (level RAM) -> vertex array, triangle lists and material passes
 * (notes/modern-renderer.md 2.3, the VU1 main program's prelit path: notes/wip/r2-m1.md "Findings").
 *
 * The chain is walked like the DMAC does it (CNT/NEXT/REF/REFS/CALL/RET/END, the tag's upper 64 bits go to the VIF as
 * two VIF codes) and its VIF codes are interpreted into a private copy of VU1 data memory with VIF1's double buffering
 * (BASE 0x32, OFST 0x1E7 as the frame sets them). Every MSCAL of the main program's prelit routines then reads what the
 * routine reads:
 *   2E20 / 2E68 / 2EB0   the buffer 0x24B - TOP (2E68 first copies qw 0..9 and 0x15..[11].y from the other buffer):
 *                        sub-batch 1 = descriptor [13] (input start, -, end, NLOOP|EOP) and passes [14] (packets)
 *   2EE8                 the buffer TOP: sub-batch 2 = [17], [18]
 *   0000 / 3AB0          buffer flip / flush: nothing to read
 * Anything else (another routine, MPG, DIRECT, MARK, MSCNT, STMOD != 0, billboard flags, unknown packet layouts) makes the
 * mesh undecodable: its entries stay on the exact path. */
#include "r2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char* const r2m_reason[R2M_N] = { "ok", "dma tag", "vif code", "mscal", "header", "descriptor", "packet", "vertex",
                                         "size", "fade" };

#define QW 1024u                         /* VU1 data memory, quadwords */
#define MAXW (1u << 20)                  /* VIF words per chain (4 MB) */
int r2_fail_line;                       /* (diagnostics) the decoder's line of the last vertex failure */
int r2_fail_info[4];                     /* (diagnostics) flags, second, vertex, routine of the last failure */
#define R2FAIL(r) do { r2_fail_line = __LINE__; return (r); } while (0)

typedef struct {
    const uint8_t* ram;
    uint32_t vu[QW][4];
    uint8_t wr[QW];                      /* written by this chain */
    uint32_t cycle, mask, row[4], col[4], mode, base, ofst, tops, top, dbf;
    uint32_t vset;                       /* (r2-flicker) R2_VIF_* registers the chain wrote */
    uint32_t* w;                         /* the VIF word stream */
    uint32_t nw, capw;
    uint64_t hash;
    uint32_t bytes;
    /* output */
    r2_vtx* vtx;
    uint32_t nvtx, capv;
    uint32_t* idx;
    uint32_t nidx, capi;
    r2_batch* b;
    uint32_t nb, capb;
    int w_seen;                          /* 0..8 of a header buffer written by the chain before the first MSCAL */
    int w_set, w_fixed;                  /* the matrix / fade captured; set mid-chain (unsupported) */
    float wm[16], fade;
    int batches_started;
    /* (r2-m3) lit routines */
    uint32_t nbones, hq_hi;
    int lit_pending;                     /* a lit routine set up buffer lit_x: 0x3358 transforms it next */
    uint32_t lit_x;
    int lpos_set;
    uint16_t lpos_off, lpos_bone;
    uint32_t lsrc[QW];                   /* the buffer's output lists: list << 16 | input record, ~0 none */
    /* (r2-m3, R4) outline scripts: the chain's VU writes (addr | written lanes << 16) and MSCALs, logged */
    int omode;
    uint32_t* lwa;
    uint32_t (*lwv)[4];
    uint32_t nlw, caplw;
    uint32_t *sw, *sm;
    uint32_t nst, capst;
    int ofail;
    int ofs_seen;                        /* (r2-m5) an OFFSET code was interpreted */
    uint32_t (*seg)[2];                  /* (r2-m5) the RAM ranges walked (add_seg) */
    uint32_t nseg, capseg;
} dec;

static int grow(void** p, uint32_t* cap, uint32_t need, size_t el) {
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void* n = realloc(*p, (size_t)c * el);
    if (!n) return 0;
    *p = n;
    *cap = c;
    return 1;
}

static uint64_t hmix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 0x100000001B3ull;
    return h ^ (h >> 29);
}

static int ram_ok(uint32_t a, uint32_t bytes) { return (a & 0x80000000u) == 0 && a + bytes <= 0x02000000u && a + bytes >= a; }

static int push_words(dec* d, const uint8_t* src, uint32_t n) {
    if (d->nw + n > MAXW) return 0;
    if (!grow((void**)&d->w, &d->capw, d->nw + n, 4)) return 0;
    memcpy(d->w + d->nw, src, (size_t)n * 4);
    for (uint32_t i = 0; i < n; i++) d->hash = hmix(d->hash, d->w[d->nw + i]);
    d->nw += n;
    d->bytes += n * 4;
    return 1;
}

/* (r2-m5) the RAM ranges a chain reads (tags and data), merged where contiguous: the content check of cached meshes
 * and outline scripts (r2_src_same). The game rewrites mesh DMA in place while levels stream in (the ep1 hub: ~45 chains
 * decoded before their final content, the first 32 bytes unchanged): a stale decode drew another mesh's triangles. */
static int add_seg(dec* d, uint32_t a, uint32_t n) {
    if (d->nseg && d->seg[d->nseg - 1][0] + d->seg[d->nseg - 1][1] == a) { d->seg[d->nseg - 1][1] += n; return 1; }
    if (!grow((void**)&d->seg, &d->capseg, d->nseg + 1, sizeof d->seg[0])) return 0;
    d->seg[d->nseg][0] = a;
    d->seg[d->nseg][1] = n;
    d->nseg++;
    return 1;
}
struct r2_src { uint32_t nseg, bytes; uint32_t (*seg)[2]; uint8_t* copy; };
static r2_src* src_make(dec* d) {
    r2_src* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    uint32_t total = 0;
    for (uint32_t i = 0; i < d->nseg; i++) total += d->seg[i][1];
    s->copy = malloc(total ? total : 1);
    s->seg = malloc((d->nseg ? d->nseg : 1) * sizeof s->seg[0]);
    if (!s->copy || !s->seg) { free(s->copy); free(s->seg); free(s); return NULL; }
    uint32_t o = 0;
    for (uint32_t i = 0; i < d->nseg; i++) {
        memcpy(s->copy + o, d->ram + d->seg[i][0], d->seg[i][1]);
        o += d->seg[i][1];
    }
    memcpy(s->seg, d->seg, d->nseg * sizeof s->seg[0]);
    s->nseg = d->nseg;
    s->bytes = total;
    return s;
}
void r2_src_free(r2_src* s) { if (s) { free(s->copy); free(s->seg); free(s); } }
int r2_src_same(const r2_src* s, const uint8_t* ram) {
    if (!s) return 1;
    uint32_t o = 0;
    for (uint32_t i = 0; i < s->nseg; i++) {
        if (memcmp(ram + s->seg[i][0], s->copy + o, s->seg[i][1])) return 0;
        o += s->seg[i][1];
    }
    return 1;
}

/* the DMAC's source chain from addr until the RET that leaves it (or END), CALL depth 2 */
static int walk(dec* d, uint32_t addr) {
    uint32_t stack[2];
    int sp = 0;
    for (int n = 0; n < 65536; n++) {
        if ((addr & 15) || !ram_ok(addr, 16)) return R2M_TAG;
        const uint8_t* t = d->ram + addr;
        uint64_t tag;
        memcpy(&tag, t, 8);
        uint32_t qwc = (uint32_t)(tag & 0xFFFF), id = (uint32_t)((tag >> 28) & 7), a = (uint32_t)(tag >> 32) & 0xFFFFFFF0u;
        if (tag & (1ull << 31)) return R2M_TAG;                   /* IRQ */
        d->hash = hmix(d->hash, tag);
        if (!push_words(d, t + 8, 2)) return R2M_SIZE;           /* the tag's upper half: two VIF codes */
        if (!add_seg(d, addr, 16)) return R2M_SIZE;
        uint32_t data = id == 0 || id == 3 || id == 4 ? a : addr + 16, next = addr + 16 + qwc * 16;
        if (qwc) {
            if (!ram_ok(data, qwc * 16)) return R2M_TAG;
            if (!add_seg(d, data, qwc * 16)) return R2M_SIZE;
            if (!push_words(d, d->ram + data, qwc * 4)) return R2M_SIZE;
        }
        switch (id) {
        case 0: return R2M_OK;                                   /* REFE: data, then end */
        case 1: addr = next; break;                              /* CNT */
        case 2: addr = a; break;                                 /* NEXT */
        case 3: case 4: addr += 16; break;                       /* REF, REFS */
        case 5:                                                  /* CALL */
            if (sp == 2) return R2M_TAG;
            stack[sp++] = next;
            addr = a;
            break;
        case 6:                                                  /* RET */
            if (!sp) return R2M_OK;
            addr = stack[--sp];
            break;
        default: return R2M_OK;                                  /* END */
        }
    }
    return R2M_SIZE;
}

uint32_t r2_hash_bytes;                  /* (r2-m5) the bytes r2_mesh_hash walked last */
/* (r2-m5) the content hash r2_mesh_decode records (m->hash) of the chain at addr now; 0 when it does not walk */
uint64_t r2_mesh_hash(const uint8_t* ram, uint32_t addr) {
    static dec* d;                                               /* (EE thread only) */
    if (!d && !(d = calloc(1, sizeof *d))) return 0;
    d->nw = 0;
    d->bytes = 0;
    d->nseg = 0;
    d->ram = ram;
    d->hash = 0xCBF29CE484222325ull;
    if (walk(d, addr)) return 0;
    r2_hash_bytes = d->bytes;
    return d->hash;
}

/* ---- VU memory as the routines read it ----------------------------------------------------------------------- */
static uint32_t* vq(dec* d, uint32_t a) { return d->vu[a & (QW - 1)]; }
static float vf(dec* d, uint32_t a, int c) { float f; memcpy(&f, &vq(d, a)[c], 4); return f; }

static int add_batch_packets(dec* d, r2_batch* B, uint32_t X, const uint32_t* pk) {
    B->npass = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t pa = pk[i] & 0x3FF;
        if (!pa) break;
        const uint32_t* g = vq(d, X + pa);                       /* vertex GIF tag (NLOOP filled in by the VU) */
        uint32_t y = g[1];
        uint32_t pre = (y >> 14) & 1, prim = (y >> 15) & 0x7FF, flg = (y >> 26) & 3, nreg = y >> 28;
        uint32_t regs = g[2] & 0xFFFF;
        if (!pre || flg != 0 || nreg != 4 || (prim & 7) != 4 || !((prim >> 3) & 1) || ((prim >> 8) & 3)) return R2M_PACKET;
        if (regs != 0x4F12 && regs != 0x41F2) return R2M_PACKET;
        const uint32_t* ad = vq(d, X + pa + 2);                  /* A+D tag: NLOOP 6, PACKED, NREG 1, A+D */
        if ((ad[0] & 0x7FFF) != 6 || ((ad[1] >> 26) & 3) != 0 || (ad[1] >> 28) != 1 || (ad[2] & 0xF) != 0xE) return R2M_PACKET;
        r2_pass* P = &B->pass[B->npass];
        memset(P, 0, sizeof *P);
        int got = 0;
        for (int k = 0; k < 6; k++) {
            const uint32_t* r = vq(d, X + pa + 3 + k);
            uint64_t v = (uint64_t)r[0] | (uint64_t)r[1] << 32;
            switch (r[2] & 0xFF) {
            case 0x06: P->tex0 = v; got |= 1; break;
            case 0x14: P->tex1 = v; got |= 2; break;
            case 0x08: P->clamp = v; got |= 4; break;
            case 0x47: P->test = v; got |= 8; break;
            case 0x42: P->alpha = v; got |= 16; break;
            case 0x3D: P->fogcol = v; got |= 32; break;
            default: return R2M_PACKET;
            }
        }
        if (got != 63) return R2M_PACKET;
        uint32_t psm = (uint32_t)(P->tex0 >> 20) & 0x3F, cld = (uint32_t)(P->tex0 >> 61), csm = (uint32_t)(P->tex0 >> 55) & 1;
        uint32_t cpsm = (uint32_t)(P->tex0 >> 51) & 0xF;
        if ((psm != 0x14 && psm != 0x13) || cld != 1 || csm || cpsm != 0) return R2M_PACKET;   /* T4 / T8, CSM1 CT32 loaded */
        if ((P->clamp & 3) >= 2 || ((P->clamp >> 2) & 3) >= 2) return R2M_PACKET;              /* REPEAT / CLAMP only */
        if ((P->tex1 & 1) == 0 || ((P->tex1 >> 2) & 7) != 0) return R2M_PACKET;                 /* LCM 1, MXL 0 */
        P->prim = prim;
        P->csel = regs == 0x41F2;
        r2_pass_classify(P);
        if (i < 2) for (uint32_t k = 0; k < 8; k++) memcpy(B->pk[i][k], vq(d, X + pa + 1 + k), 16);   /* (r2-m5b) r2_clip.c */
        B->npass++;
    }
    return B->npass ? R2M_OK : R2M_PACKET;
}

/* (r2-m2) the pass's static drawing classes (as gpu_r2front.c emit_pass derived them per frame in r2-m1): blend class
 * (the exact path's FF table, gpu_front.c state_update), texture filter, the material's static shader flags */
void r2_pass_classify(r2_pass* P) {
    uint32_t prim = P->prim;
    int abe = (prim >> 6) & 1, fge = (prim >> 5) & 1;
    uint64_t alpha = P->alpha, tex1 = P->tex1;
    int A = (int)(alpha & 3), Bs = (int)((alpha >> 2) & 3), C = (int)((alpha >> 4) & 3), D = (int)((alpha >> 6) & 3);
    int fix = (int)((alpha >> 32) & 0xFF);
    int bl = 0;                                                  /* R2G_BLEND_OFF NOCOLOR ADD REVSUB DUAL = 0..4 */
    P->nocolor = 0;
    if (abe) {
        int uses_cd = A == 1 || Bs == 1 || C == 1 || D == 1;
        if (A == Bs) { if (D == 1) { bl = 1; P->nocolor = 1; } }
        else if (!uses_cd) bl = 0;
        else if (C != 1 && A == 0 && Bs == 2 && D == 1) bl = 2;
        else if (C != 1 && A == 2 && Bs == 0 && D == 1) bl = 3;
        else if (C != 1 && A == 0 && Bs == 1 && D == 1) bl = 4;
        else bl = 0xFF;
        if (bl == 4 && C == 2 && fix > 128) bl = 0xFF;
    }
    /* texture filter: LCM 1 (fixed LOD K), no mipmaps (checked at decode): K <= 0 magnifies (MMAG), else MMIN */
    int k = (int)((int64_t)(tex1 << 20) >> 52);
    int filt = k <= 0 ? (int)((tex1 >> 5) & 1) : (int)((tex1 >> 6) & 7);
    if (filt > 1) bl = 0xFF;
    P->texm = (uint8_t)(filt ? 2 : 1);
    P->bl = (uint8_t)bl;
    int tcc = (int)((P->tex0 >> 34) & 1);
    /* GPU_R2D_CSEL 1, ABE 8, FGE 16, TCC 32, TEXM << 8 (gpu_front.h); the blend class at bits 12..15 */
    P->mflags = (P->csel ? 1u : 0u) | (abe ? 8u : 0u) | (fge ? 16u : 0u) | (tcc ? 32u : 0u) | (uint32_t)P->texm << 8 |
                (uint32_t)(bl & 15) << 12;
    P->alpha_enc = (uint32_t)(A | Bs << 2 | C << 4 | D << 6) | (uint32_t)fix << 8;
}

/* (r2-m3) the lit routines' output list of a sub-batch (0x1050 / 0x16B0 / 0x1940 walk it alike): from [15].y, each
 * strip vertex's qw0.z links to the next output; a negative link (bit 15) means the next output takes the same input
 * record, a positive one the next record, 0 ends the list. src[k] = the input record of strip vertex k. */
/* The two sub-batches' lists may write into either strip of the buffer: d->lsrc[addr - X] = list << 16 | input, the
 * later list winning (the routines run list 1, then list 2). */
static int out_list(dec* d, uint32_t X, uint32_t list, uint32_t head) {
    if (!head) return R2M_OK;                                    /* (0: the routine's list is empty) */
    uint32_t cur = (X + head) & 0x3FF, in = 0;
    for (uint32_t guard = 0; guard < 1024; guard++) {
        d->lsrc[(cur - X) & 0x3FF] = list << 16 | in;
        uint32_t link = vq(d, cur)[2] & 0xFFFF;
        if (!link) return R2M_OK;
        if (!(((X + link) & 0xFFFF) & 0x8000)) in++;
        cur = (X + link) & 0x3FF;
    }
    R2FAIL(R2M_VERTEX);
}
static int lit_lists(dec* d, uint32_t X, int two) {
    memset(d->lsrc, 0xFF, sizeof d->lsrc);
    int r = out_list(d, X, 0, vq(d, X + 15)[1] & 0xFFFF);
    if (!r && two) r = out_list(d, X, 1, vq(d, X + 19)[1] & 0xFFFF);
    return r;
}

static int lit_prepare(dec* d, uint32_t X) {
    uint32_t flags = vq(d, X + 11)[0] & 0xFFFF;
    int skin = (vq(d, X + 16)[2] & 0xFFFF) && (flags & 0x30);
    if (!(flags & 2) && !skin) return R2M_OK;
    return lit_lists(d, X, (vq(d, X + 17)[3] & 0x7FFF) != 0);
}

static int sub_batch(dec* d, uint32_t X, int second, int litr) {
    uint32_t flags = vq(d, X + 11)[0] & 0xFFFF;
    r2_fail_info[0] = (int)flags; r2_fail_info[1] = second; r2_fail_info[3] = litr;
    if (!litr && (flags & 0x702)) return R2M_DESC;               /* billboard modes, 0x2 (the lit routines') */
    if (litr && (flags & 0x750)) return R2M_DESC;                /* (r2-m3) billboards, bone streams (0x0E30), 0x16B0 */
    const uint32_t* q = vq(d, X + (second ? 17 : 13));
    const uint32_t* pk = vq(d, X + (second ? 18 : 14));
    uint32_t start = q[0] & 0x3FF, end = q[2] & 0x3FF, nl = q[3] & 0xFFFF, n = nl & 0x7FFF;
    if (!(nl & 0x8000) || n < 1 || n > 256 || end != start + 4 * n) return R2M_DESC;
    /* (r2-m3) the lit routines: lighting (flags & 2), skinning ([16].z of sub-batch 1 != 0: 0x10 positions, 0x20 positions
     * + normals), texture animation ([15].z, 0x0D28) and the view-dependent alpha ([15].w, 0x13F0) stay exact */
    int lit = 0, skin = 0;
    const uint32_t *e1 = vq(d, X + (second ? 19 : 15)), *e2 = vq(d, X + (second ? 20 : 16));
    if (litr) {
        lit = (flags & 2) != 0;
        if ((vq(d, X + 16)[2] & 0xFFFF) && (flags & 0x30)) skin = flags & 0x10 ? 1 : 2;
        if ((vq(d, X + 15)[3] & 0xFFFF) || (e1[3] & 0xFFFF))
            return R2M_DESC;
        if (!d->wr[(X + 11) & (QW - 1)] || !d->wr[(X + 15 + (second ? 4 : 0)) & (QW - 1)] ||
            !d->wr[(X + 16 + (second ? 4 : 0)) & (QW - 1)])
            return R2M_HEADER;
    }
    if (!grow((void**)&d->b, &d->capb, d->nb + 1, sizeof(r2_batch))) return R2M_SIZE;
    r2_batch* B = &d->b[d->nb];
    memset(B, 0, sizeof *B);
    int r = add_batch_packets(d, B, X, pk);
    if (r) return r;
    B->cull = !(flags & 0x80);
    B->fog_scale = vf(d, X + 9, 1);
    B->fog_wmax = vf(d, X + 9, 2);
    memcpy(B->vu9, vq(d, X + 9), 16);                           /* (r2-m5b) */
    if (!(B->fog_wmax > 0.0f) || !(B->fog_scale >= 0.0f) || B->fog_scale > 4096.0f) return R2M_HEADER;
    if (!d->wr[(X + 9) & (QW - 1)]) return R2M_HEADER;
    /* the matrix and VU 8 of the buffer: written by the chain (a whole static packet) or by the entry's header */
    int own = d->wr[X & (QW - 1)] && d->wr[(X + 3) & (QW - 1)] && d->wr[(X + 8) & (QW - 1)];
    if (own) {
        float m[16], fade = vf(d, X + 8, 0);
        for (int i = 0; i < 16; i++) m[i] = vf(d, X + (uint32_t)(i >> 2), i & 3);
        if (vq(d, X + 8)[3] != 0) return R2M_FADE;               /* VU 8.w: the alternate A+D qword (1D10) */
        if (!d->w_set) { memcpy(d->wm, m, sizeof m); d->fade = fade; d->w_set = 1; }
        else if (memcmp(d->wm, m, sizeof m) || d->fade != fade) return R2M_HEADER;   /* one matrix per mesh */
    } else if (d->w_set) {
        return R2M_HEADER;
    }
    d->w_fixed |= own ? 1 : 2;
    if (d->w_fixed == 3) return R2M_HEADER;
    /* (r2-m3) lit / skinned: the strip vertices' input records */
    (void)e1;
    if (lit || skin) {
        for (uint32_t k = 0; k < n; k++)
            if (d->lsrc[(start + 4 * k) & 0x3FF] == ~0u) R2FAIL(R2M_VERTEX);   /* (a vertex no routine writes) */
        B->lit = (uint8_t)lit;
        B->skin = (uint8_t)skin;
        int16_t br = (int16_t)(e2[0] & 0xFFFF);
        B->bright = (float)br * (1.0f / 4096.0f);
        if (skin) {
            const uint32_t* bq = vq(d, X + 12);
            if (!d->wr[(X + 12) & (QW - 1)]) return R2M_HEADER;
            for (int j = 0; j < 4; j++) {
                uint32_t o = bq[j] & 0xFFFF;
                if (o < R2_HQ_LO || o + 4 > R2_HQ_MAX) return R2M_HEADER;   /* (the palette: header qwords) */
                B->bone[j] = (uint16_t)o;
                if (o + 4 > d->hq_hi) d->hq_hi = o + 4;
            }
        }
    }
    /* (r2-m3) texture animation (0x0D28, [15].z / [19].z): a UV offset; a TEX0 swap stays exact. Written by the chain:
     * baked into the vertices below; else per frame from the entry's header (r2_capture.c) */
    int32_t du = 0, dv = 0;
    if (litr && (e1[2] & 0xFFFF)) {
        uint32_t t = e1[2] & 0xFFFF;
        if (d->wr[(X + t) & (QW - 1)]) {
            const uint32_t* tq = vq(d, X + t);
            if (tq[2] & 0xFFFF) return R2M_DESC;
            du = (int32_t)tq[0];
            dv = (int32_t)tq[1];
        } else {
            if (t < R2_HQ_LO || t + 1 > R2_HQ_MAX) return R2M_DESC;
            B->tanim = (uint16_t)t;
            if (t + 1 > d->hq_hi) d->hq_hi = t + 1;
        }
    }
    /* vertices */
    if (!grow((void**)&d->vtx, &d->capv, d->nvtx + n, sizeof(r2_vtx))) return R2M_SIZE;
    if (!grow((void**)&d->idx, &d->capi, d->nidx + 3 * n, 4)) return R2M_SIZE;
    B->vfirst = d->nvtx;
    B->vcount = n;
    B->ifirst = d->nidx;
    /* (r2-m5) the colours the passes read (RGBAQ from colour A: REGS 4F12, colour B: 41F2): a sub-batch whose passes all
     * take colour A need not unpack colour B (ep1 hub: 23 meshes) */
    int need_c[2] = { 0, 0 };
    for (uint32_t i = 0; i < B->npass; i++) need_c[B->pass[i].csel] = 1;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t a = X + start + 4 * k;
        const uint32_t *uv = vq(d, a), *ca = vq(d, a + 1), *cb = vq(d, a + 2), *p = vq(d, a + 3);
        if (!d->wr[a & (QW - 1)] || !d->wr[(a + 3) & (QW - 1)]) R2FAIL(R2M_VERTEX);
        int ok_a = d->wr[(a + 1) & (QW - 1)], ok_b = d->wr[(a + 2) & (QW - 1)];
        if (!lit && ((need_c[0] && !ok_a) || (need_c[1] && !ok_b))) R2FAIL(R2M_VERTEX);
        if (((p[3] >> 23) & 0xFF) != 150) R2FAIL(R2M_VERTEX);      /* w ~ 2^23 + m (the flag / fog word) */
        r2_vtx* v = &d->vtx[d->nvtx++];
        memset(v, 0, sizeof *v);
        const uint32_t* pp = p;
        if (lit || skin) {
            uint32_t s = d->lsrc[(start + 4 * k) & 0x3FF], li = s >> 16, in = s & 0xFFFF;
            const uint32_t *l1 = vq(d, X + (li ? 19 : 15)), *l2 = vq(d, X + (li ? 20 : 16));
            if ((l2[0] & 0xFFFF) != (e2[0] & 0xFFFF)) R2FAIL(R2M_VERTEX);   /* (the source list's brightness: the batch's) */
            uint32_t r = X + (l1[0] & 0x3FF) + 4 * in, wa = X + (l2[2] & 0x3FF) + in;
            if (!d->wr[(r + 1) & (QW - 1)] || !d->wr[(r + 2) & (QW - 1)] || !d->wr[(r + 3) & (QW - 1)]) R2FAIL(R2M_VERTEX);
            const uint32_t *nq = vq(d, r + 1), *cq = vq(d, r + 2);
            if (skin) {
                pp = vq(d, r + 3);
                const uint32_t* wq = vq(d, wa);
                if (!d->wr[wa & (QW - 1)]) R2FAIL(R2M_VERTEX);
                for (int c = 0; c < 4; c++) if (wq[c] > 255) R2FAIL(R2M_VERTEX);
                v->wts = wq[0] | wq[1] << 8 | wq[2] << 16 | wq[3] << 24;
            }
            if (lit) {
                for (int c = 0; c < 4; c++) {
                    if ((int32_t)nq[c] < -128 || (int32_t)nq[c] > 127 || cq[c] > 255) R2FAIL(R2M_VERTEX);
                    v->nrm |= (nq[c] & 255u) << (8 * c);
                    v->lcol |= cq[c] << (8 * c);
                }
            } else if (skin && (nq[0] || nq[1] || nq[2])) {
                /* (0x16B0 / 0x1940 on an unlit sub-batch: normals rewritten nobody reads) */
            }
        }
        memcpy(&v->x, &pp[0], 4);
        memcpy(&v->y, &pp[1], 4);
        memcpy(&v->z, &pp[2], 4);
        if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z)) R2FAIL(R2M_VERTEX);
        v->wbits = p[3];
        v->batch = d->nb;
        int32_t uu = (int32_t)uv[0] + du, vv = (int32_t)uv[1] + dv;
        if (uu < -32768 || uu > 32767 || vv < -32768 || vv > 32767) R2FAIL(R2M_VERTEX);
        v->u = (int16_t)uu;
        v->v = (int16_t)vv;
        if (!lit) {
            /* (the lit routines' unlit sub-batches may carry other words there, e.g. V4-8 normals: the GS takes the low
             * byte of each; a fade would scale the whole word, so those stay exact when faded - r2_capture.c) */
            static const uint32_t zero4[4];
            if (!ok_a) ca = zero4;                               /* (r2-m5) a colour no pass reads */
            if (!ok_b) cb = zero4;
            for (int c = 0; c < 4; c++) {
                if (ca[c] > 255 || cb[c] > 255) { if (!litr) R2FAIL(R2M_VERTEX); B->wide = 1; }
            }
            v->col[0] = (ca[0] & 255) | (ca[1] & 255) << 8 | (ca[2] & 255) << 16 | (ca[3] & 255) << 24;
            v->col[1] = (cb[0] & 255) | (cb[1] & 255) << 8 | (cb[2] & 255) << 16 | (cb[3] & 255) << 24;
        }
        if (k >= 2 && !((p[3] >> 15) & 1)) {                     /* ADC clear: the triangle ending here is drawn */
            uint32_t b0 = B->vfirst;
            uint32_t* o = &d->idx[d->nidx];
            if (k & 1) { o[0] = b0 + k - 1; o[1] = b0 + k - 2; }
            else { o[0] = b0 + k - 2; o[1] = b0 + k - 1; }
            o[2] = b0 + k;
            d->nidx += 3;
        }
    }
    B->icount = d->nidx - B->ifirst;
    B->amin[0] = B->amin[1] = 255;
    for (uint32_t k = 0; k < n; k++) {
        const r2_vtx* v = &d->vtx[B->vfirst + k];
        for (int c = 0; c < 2; c++) {
            /* (r2-m3) lit: colour A's alpha is the input colour's, colour B's 63.4995 x (2.0157 - k), 1..127 */
            uint8_t a = (uint8_t)(lit ? (c ? 1 : v->lcol >> 24) : v->col[c] >> 24);
            if (lit && c) { if (B->amax[1] < 127) B->amax[1] = 127; }
            if (a < B->amin[c]) B->amin[c] = a;
            if (a > B->amax[c]) B->amax[c] = a;
        }
    }
    d->nb++;
    return R2M_OK;
}

/* ---- VIF interpreter ------------------------------------------------------------------------------------------ */
static void upk_write(dec* d, uint32_t* addr, uint32_t* cyc, uint32_t cl, uint32_t wl, int masked, const uint32_t* v) {
    uint32_t a = *addr & (QW - 1);
    uint32_t* dst = d->vu[a];
    uint32_t row = *cyc < 3 ? *cyc : 3;
    uint32_t m = masked ? (d->mask >> (row * 8)) & 0xFF : 0;
    for (int j = 0; j < 4; j++) {
        switch ((m >> (j * 2)) & 3) {
        case 0: dst[j] = v ? v[j] : d->row[j]; break;
        case 1: dst[j] = d->row[j]; break;
        case 2: dst[j] = d->col[row]; break;
        default: break;
        }
    }
    d->wr[a] = 1;
    if (d->omode) {                                              /* (r2-m3) the outline script's write log */
        uint32_t lanes = 0;
        for (int j = 0; j < 4; j++) if (!(masked && ((m >> (j * 2)) & 3) == 3)) lanes |= 1u << j;
        if (!grow((void**)&d->lwa, &d->caplw, d->nlw + 1, 4) || !(d->lwv = realloc(d->lwv, (size_t)d->caplw * 16))) { d->ofail = 1; }
        else { d->lwa[d->nlw] = a | lanes << 16; memcpy(d->lwv[d->nlw], dst, 16); d->nlw++; }
    }
    (*addr)++;
    if (++*cyc >= wl) { *cyc = 0; if (cl > wl) *addr += cl - wl; }
}

static int unpack(dec* d, uint32_t code, uint32_t* pos) {
    uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    if (!num) num = 256;
    uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
    int masked = (cmd >> 4) & 1, usn = (imm >> 14) & 1;
    if (vl == 3 && vn != 3) return R2M_VIF;
    if (d->mode & 3) return R2M_VIF;                             /* offset / difference modes: never in meshes */
    uint32_t eb = vl == 3 ? 2 : 4u >> vl, vb = vl == 3 ? 2 : eb * (vn + 1);
    uint32_t cl = d->cycle & 0xFF, wl = (d->cycle >> 8) & 0xFF;
    if (!wl) cl = wl = 1;
    if (wl > cl) return R2M_VIF;                                 /* filling writes: never in meshes */
    uint32_t nin = num;
    uint32_t words = (nin * vb + 3) / 4;
    if (*pos + words > d->nw) return R2M_VIF;
    const uint8_t* src = (const uint8_t*)(d->w + *pos);
    uint32_t addr = (imm & 0x3FF) + ((imm & 0x8000) ? d->tops : 0), cyc = 0;
    for (uint32_t i = 0; i < nin; i++) {
        const uint8_t* s = src + (size_t)i * vb;
        uint32_t v[4];
        if (vl == 3) {
            uint16_t h;
            memcpy(&h, s, 2);
            v[0] = (h & 0x1Fu) << 3; v[1] = ((h >> 5) & 0x1Fu) << 3; v[2] = ((h >> 10) & 0x1Fu) << 3; v[3] = ((uint32_t)h >> 15) << 7;
        } else {
            uint32_t e[4] = { 0, 0, 0, 0 };
            uint32_t ne = vn + 1;
            for (uint32_t k = 0; k < ne; k++) {
                const uint8_t* p = s + k * eb;
                if (vl == 0) memcpy(&e[k], p, 4);
                else if (vl == 1) { uint16_t h; memcpy(&h, p, 2); e[k] = usn ? h : (uint32_t)(int32_t)(int16_t)h; }
                else e[k] = usn ? p[0] : (uint32_t)(int32_t)(int8_t)p[0];
            }
            if (vn == 0) v[0] = v[1] = v[2] = v[3] = e[0];
            else if (vn == 1) { v[0] = v[2] = e[0]; v[1] = v[3] = e[1]; }
            else if (vn == 2) {                                  /* V3: w = the next element of the stream (vif.c) */
                v[0] = e[0]; v[1] = e[1]; v[2] = e[2];
                if (i + 1 < nin) {
                    const uint8_t* p = s + 3 * eb;
                    if (vl == 0) memcpy(&v[3], p, 4);
                    else if (vl == 1) { uint16_t h; memcpy(&h, p, 2); v[3] = usn ? h : (uint32_t)(int32_t)(int16_t)h; }
                    else v[3] = usn ? p[0] : (uint32_t)(int32_t)(int8_t)p[0];
                } else v[3] = 0;
            } else { v[0] = e[0]; v[1] = e[1]; v[2] = e[2]; v[3] = e[3]; }
        }
        upk_write(d, &addr, &cyc, cl, wl, masked, v);
    }
    *pos += words;
    return R2M_OK;
}

uint32_t r2_fail_ms[8], r2_fail_nms;     /* (diagnostics) the MSCALs of the chain decoded last */
static int mscal(dec* d, uint32_t vaddr) {
    r2_fail_ms[r2_fail_nms++ & 7] = vaddr * 8;
    if (d->omode) {                                              /* (r2-m3) an outline step */
        d->top = d->tops;
        d->dbf ^= 1;
        d->tops = (d->base + (d->dbf ? d->ofst : 0)) & 0x3FF;
        if (vaddr * 8 == 0x0000) return R2M_OK;                  /* ([E] NOP: a buffer flip) */
        if (vaddr * 8 != 0x0738 && vaddr * 8 != 0x07F0) return R2M_MSCAL;
        if (!grow((void**)&d->sw, &d->capst, d->nst + 1, 4) || !(d->sm = realloc(d->sm, (size_t)d->capst * 4))) return R2M_SIZE;
        d->sw[d->nst] = d->nlw;
        d->sm[d->nst] = vaddr * 8 | d->top << 16;
        d->nst++;
        return R2M_OK;
    }
    d->top = d->tops;
    d->dbf ^= 1;
    d->tops = (d->base + (d->dbf ? d->ofst : 0)) & 0x3FF;
    uint32_t X = (0x24B - d->top) & 0x3FF;
    switch (vaddr * 8) {
    case 0x0000: case 0x3AB0: return R2M_OK;
    case 0x2E68: {                                               /* 0x0610: header 0..9 and packets from the other buffer */
        uint32_t Y = d->top;
        for (uint32_t i = 0; i < 10; i++) { memcpy(vq(d, X + i), vq(d, Y + i), 16); d->wr[(X + i) & (QW - 1)] = d->wr[(Y + i) & (QW - 1)]; }
        uint32_t e = vq(d, X + 11)[1] & 0x3FF;
        for (uint32_t i = 0x15; i < e && i < 0x40; i++) { memcpy(vq(d, X + i), vq(d, Y + i), 16); d->wr[(X + i) & (QW - 1)] = d->wr[(Y + i) & (QW - 1)]; }
    }   /* fall through */
    case 0x2E20: case 0x2EB0:
        if (d->lit_pending) return R2M_MSCAL;
        if (vaddr * 8 != 0x2E20 && !d->batches_started) return R2M_MSCAL;   /* a mesh begins with the matrix set-up */
        d->batches_started = 1;
        return sub_batch(d, X, 0, 0);
    case 0x2EE8:
        if (!d->batches_started || d->lit_pending) return R2M_MSCAL;
        return sub_batch(d, d->top, 1, 0);
    /* (r2-m3) the lit routines (0x2EF8 with the matrix and light set-up, 0x2F80 with the 0x0610 copy, 0x2FC8): lighting
     * and skinning of both sub-batches of the buffer 0x24B - TOP; 0x3358 (next MSCAL, buffer TOP) transforms them */
    case 0x2F80: {
        uint32_t Y = d->top;
        for (uint32_t i = 0; i < 10; i++) { memcpy(vq(d, X + i), vq(d, Y + i), 16); d->wr[(X + i) & (QW - 1)] = d->wr[(Y + i) & (QW - 1)]; }
        uint32_t e = vq(d, X + 11)[1] & 0x3FF;
        for (uint32_t i = 0x15; i < e && i < 0x200; i++) { memcpy(vq(d, X + i), vq(d, Y + i), 16); d->wr[(X + i) & (QW - 1)] = d->wr[(Y + i) & (QW - 1)]; }
    }   /* fall through */
    case 0x2EF8: case 0x2FC8: {
        if (d->lit_pending) return R2M_MSCAL;
        if (vaddr * 8 != 0x2EF8 && !d->batches_started) return R2M_MSCAL;
        if (vaddr * 8 == 0x2EF8 && !d->lpos_set) {
            d->lpos_off = (uint16_t)(vq(d, X + 11)[3] & 0xFFFF);
            d->lpos_bone = (uint16_t)(vq(d, X + 12)[0] & 0xFFFF);
            d->lpos_set = 1;
        }
        d->batches_started = 1;
        int r = lit_prepare(d, X);
        if (!r) r = sub_batch(d, X, 0, 1);
        if (r) return r;
        const uint32_t* q2 = vq(d, X + 17);
        if (q2[3] & 0x7FFF) { r = sub_batch(d, X, 1, 1); if (r) return r; }
        d->lit_pending = 1;
        d->lit_x = X;
        return R2M_OK;
    }
    case 0x3358:
        if (!d->lit_pending || d->top != d->lit_x) return R2M_MSCAL;
        d->lit_pending = 0;
        return R2M_OK;
    default: return R2M_MSCAL;
    }
}

static int interpret(dec* d) {
    uint32_t pos = 0;
    while (pos < d->nw) {
        uint32_t code = d->w[pos++];
        uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF;
        if (cmd >= 0x60) {
            int r = unpack(d, code, &pos);
            if (r) return r;
            continue;
        }
        switch (cmd) {
        case 0x00: break;                                        /* NOP */
        case 0x01: d->cycle = imm; d->vset |= R2_VIF_CYCLE; break;   /* STCYCL */
        case 0x02: d->ofst = imm & 0x3FF; d->dbf = 0; d->tops = d->base; d->ofs_seen = 1; break;   /* OFFSET */
        case 0x03: d->base = imm & 0x3FF; break;                 /* BASE */
        case 0x04: break;                                        /* ITOP */
        case 0x05: d->mode = imm & 3; d->vset |= R2_VIF_MODE; break;   /* STMOD */
        case 0x06: break;                                        /* MSKPATH3 */
        case 0x10: case 0x11: case 0x13: break;                  /* FLUSHE / FLUSH / FLUSHA */
        case 0x14: { int r = mscal(d, imm); if (r) return r; break; }   /* MSCAL */
        case 0x20:                                               /* STMASK */
            if (pos >= d->nw) return R2M_VIF;
            d->mask = d->w[pos++];
            d->vset |= R2_VIF_MASK;
            break;
        case 0x30: case 0x31:                                    /* STROW / STCOL */
            if (pos + 4 > d->nw) return R2M_VIF;
            memcpy(cmd == 0x30 ? d->row : d->col, d->w + pos, 16);
            d->vset |= cmd == 0x30 ? R2_VIF_ROW : R2_VIF_COL;
            pos += 4;
            break;
        case 0x07:                                               /* MARK: EE-visible */
        case 0x15: case 0x17:                                    /* MSCALF, MSCNT */
        case 0x4A:                                               /* MPG */
        case 0x50: case 0x51:                                    /* DIRECT, DIRECTHL */
            return R2M_VIF;
        default:
            break;                                               /* invalid codes (tag words of floats): ignored (ERR.ME1) */
        }
    }
    return R2M_OK;
}

int r2_mesh_decode(const uint8_t* ram, uint32_t addr, int form, r2_mesh** out) {
    *out = NULL;
    dec* d = calloc(1, sizeof *d);
    if (!d) return R2M_SIZE;
    d->ram = ram;
    d->hash = 0xCBF29CE484222325ull;
    d->base = 0x32;
    d->ofst = 0x1E7;
    d->tops = d->base;
    d->cycle = 0x0101;
    r2_fail_nms = 0;
    int r = walk(d, addr);
    if (!r) r = interpret(d);
    if (!r && !d->nb) r = R2M_DESC;
    if (!r && d->lit_pending) r = R2M_MSCAL;                    /* (r2-m3) a lit buffer never transformed */
    if (!r && form == 0 && d->w_fixed != 1) r = R2M_HEADER;     /* a whole packet carries its own matrix */
    if (!r && form == 1 && d->w_fixed != 2) r = R2M_HEADER;     /* a mesh DMA takes the entry's */
    if (r) {
        free(d->vtx); free(d->idx); free(d->b); free(d->w); free(d->seg); free(d);
        return r;
    }
    r2_mesh* m = calloc(1, sizeof *m);
    if (!m) { free(d->vtx); free(d->idx); free(d->b); free(d->w); free(d->seg); free(d); return R2M_SIZE; }
    m->addr = addr;
    m->bytes = d->bytes;
    m->hash = d->hash;
    m->form = form;
    m->flips = d->dbf;
    m->vif_set = d->vset; m->vif_cycle = d->cycle; m->vif_mode = d->mode; m->vif_mask = d->mask;
    memcpy(m->vif_row, d->row, 16); memcpy(m->vif_col, d->col, 16);
    if (!(m->src = src_make(d))) { free(m); free(d->vtx); free(d->idx); free(d->b); free(d->w); free(d->seg); free(d); return R2M_SIZE; }
    m->has_w = d->w_fixed == 1;
    if (m->has_w) { memcpy(m->w, d->wm, sizeof m->w); m->fade = d->fade; }
    m->vtx = d->vtx; m->nvtx = d->nvtx;
    m->idx = d->idx; m->nidx = d->nidx;
    m->b = d->b; m->nb = d->nb;
    m->last_pass = m->b[m->nb - 1].npass - 1;
    for (uint32_t b = 0; b < m->nb; b++) { m->any_lit |= m->b[b].lit; m->any_skin |= m->b[b].skin != 0; m->any_wide |= m->b[b].wide; }
    m->lit = m->any_lit || m->any_skin || m->hq_hi;
    m->nbones = d->nbones;
    m->hq_hi = d->hq_hi;
    m->lpos_off = d->lpos_off;
    m->lpos_bone = d->lpos_bone;
    for (int c = 0; c < 3; c++) { m->bmin[c] = 1e30f; m->bmax[c] = -1e30f; }
    for (uint32_t i = 0; i < m->nvtx; i++) {
        const float* p = &m->vtx[i].x;
        for (int c = 0; c < 3; c++) {
            if (p[c] < m->bmin[c]) m->bmin[c] = p[c];
            if (p[c] > m->bmax[c]) m->bmax[c] = p[c];
        }
    }
    free(d->w);
    free(d->seg); free(d);
    *out = m;
    return R2M_OK;
}

static dec* hdr_d;                       /* (EE thread only: one decoder state reused, no allocation per entry) */
static uint32_t hdr_x;
const uint32_t* r2_header_qw(uint32_t i) {
    if (!hdr_d) return NULL;
    uint32_t a = (hdr_x + i) & (QW - 1);
    return hdr_d->wr[a] ? hdr_d->vu[a] : NULL;
}

int r2_header_parse(const uint8_t* ram, uint32_t addr, float w[16], float vu8[4]) {
    if (!hdr_d && !(hdr_d = calloc(1, sizeof *hdr_d))) return 0;
    dec* d = hdr_d;
    uint32_t* keep_w = d->w;
    uint32_t keep_cap = d->capw;
    uint32_t (*keep_seg)[2] = d->seg;
    uint32_t keep_capseg = d->capseg;
    memset(d, 0, sizeof *d);
    d->w = keep_w;
    d->capw = keep_cap;
    d->seg = keep_seg;
    d->capseg = keep_capseg;
    d->ram = ram;
    d->base = 0x32;
    d->ofst = 0x1E7;
    d->tops = d->base;
    d->cycle = 0x0101;
    if (walk(d, addr) || d->nw > 4096) return 0;
    if (interpret(d) || d->batches_started || d->dbf) return 0;     /* (an MSCAL would have flipped the buffers) */
    uint32_t X = d->tops;
    hdr_x = X;
    for (uint32_t i = 0; i < 4; i++) if (!d->wr[X + i]) return 0;
    if (!d->wr[X + 8]) return 0;
    for (int i = 0; i < 16; i++) memcpy(&w[i], &vq(d, X + (uint32_t)(i >> 2))[i & 3], 4);
    memcpy(vu8, vq(d, X + 8), 16);
    return 1;
}

/* (r2-m3, the lit oracle: scripts/r2_mesh_check.c) the sub-batches a lit routine (ra) sets up in a recorded VU1 data
 * memory image (TOP = top), decoded as r2_mesh_decode would */
int r2_lit_decode_vu(const uint32_t (*vu)[4], uint32_t top, uint32_t ra, r2_mesh** out) {
    *out = NULL;
    dec* d = calloc(1, sizeof *d);
    if (!d) return R2M_SIZE;
    memcpy(d->vu, vu, sizeof d->vu);
    memset(d->wr, 1, sizeof d->wr);
    d->top = top;
    d->batches_started = 1;
    uint32_t X = (0x24B - top) & 0x3FF;
    if (ra == 0x2F80) {
        for (uint32_t i = 0; i < 10; i++) memcpy(vq(d, X + i), vq(d, top + i), 16);
        uint32_t e = vq(d, X + 11)[1] & 0x3FF;
        for (uint32_t i = 0x15; i < e && i < 0x200; i++) memcpy(vq(d, X + i), vq(d, top + i), 16);
    }
    int r = lit_prepare(d, X);
    if (!r) r = sub_batch(d, X, 0, 1);
    if (!r && (vq(d, X + 17)[3] & 0x7FFF)) r = sub_batch(d, X, 1, 1);
    if (r) { free(d->vtx); free(d->idx); free(d->b); free(d->seg); free(d); return r; }
    r2_mesh* m = calloc(1, sizeof *m);
    m->vtx = d->vtx; m->nvtx = d->nvtx; m->idx = d->idx; m->nidx = d->nidx; m->b = d->b; m->nb = d->nb;
    m->nbones = d->nbones;
    m->hq_hi = d->hq_hi;
    m->lpos_off = (uint16_t)(vq(d, X + 11)[3] & 0xFFFF);
    m->lpos_bone = (uint16_t)(vq(d, X + 12)[0] & 0xFFFF);
    free(d->w);
    free(d->seg); free(d);
    *out = m;
    return R2M_OK;
}

/* ---- (r2-m3, R4) outline entries: the mesh chain as a script of VU writes and MSCALs (r2_outline_run per step) ---- */
struct r2_oscript {
    uint32_t hdr_x;                      /* the header's buffer (as r2_header_parse sees it) */
    uint32_t nw, nst;
    uint32_t* wa;                        /* write i: VU address | lanes << 16 */
    uint32_t (*wv)[4];
    uint32_t *sw, *sm;                   /* step i: writes [sw[i-1], sw[i]) first, then MSCAL sm[i] & 0xFFFF (TOP sm >> 16) */
    uint32_t flips;                      /* (r2-m5) VIF1 double-buffer flips of the chain (its MSCALs) mod 2 */
    r2_src* src;                         /* (r2-m5) the mesh chain's bytes (content check) */
};
uint32_t r2_outline_flips(const r2_oscript* s) { return s ? s->flips : 0; }
int r2_outline_same(const r2_oscript* s, const uint8_t* ram) { return s ? r2_src_same(s->src, ram) : 1; }

int r2_outline_decode(const uint8_t* ram, uint32_t hdr, uint32_t mesh, r2_oscript** out) {
    *out = NULL;
    dec* d = calloc(1, sizeof *d);
    if (!d) return R2M_SIZE;
    d->ram = ram;
    d->base = 0x32;
    d->ofst = 0x1E7;
    d->tops = d->base;
    d->cycle = 0x0101;
    int r = walk(d, hdr);
    if (!r) r = interpret(d);                                   /* (no MSCAL in a header: mscal() would refuse) */
    uint32_t hx = d->tops;
    if (!r && d->dbf) r = R2M_HEADER;
    if (!r) {
        d->nw = 0;
        d->nseg = 0;                                            /* (r2-m5) the content check covers the mesh chain */
        d->omode = 1;
        r2_fail_nms = 0;
        d->ofs_seen = 0;
        r = walk(d, mesh);
        if (!r) r = interpret(d);
        if (!r && d->ofs_seen) r = R2M_VIF;                     /* (r2-m5) an OFFSET in the chain: DBF absolute */
    }
    if (!r && (d->ofail || !d->nst || (d->sm[0] & 0xFFFF) != 0x0738)) r = R2M_MSCAL;
    if (!r && d->nlw > d->sw[d->nst - 1]) r = R2M_VERTEX;        /* (writes after the last step: none in outlines) */
    r2_oscript* s = r ? NULL : calloc(1, sizeof *s);
    if (!r && !s) r = R2M_SIZE;
    if (r) { free(d->lwa); free(d->lwv); free(d->sw); free(d->sm); free(d->w); free(d->vtx); free(d->idx); free(d->b); free(d->seg); free(d); return r; }
    s->hdr_x = hx;
    s->nw = d->nlw; s->wa = d->lwa; s->wv = d->lwv;
    s->nst = d->nst; s->sw = d->sw; s->sm = d->sm;
    s->flips = d->dbf;
    s->src = src_make(d);
    free(d->w); free(d->vtx); free(d->idx); free(d->b); free(d->seg); free(d);
    *out = s;
    return R2M_OK;
}

void r2_outline_free(r2_oscript* s) {
    if (!s) return;
    free(s->wa); free(s->wv); free(s->sw); free(s->sm); r2_src_free(s->src); free(s);
}

int r2_outline_exec(const r2_oscript* s, const uint32_t* (*hq)(uint32_t), const r2_frame_block* fb, uint32_t fill, uint8_t* out,
                    uint32_t cap, uint32_t* nout) {
    static uint32_t img[QW][4];          /* (EE thread only) */
    for (uint32_t i = 0; i < QW; i++) img[i][0] = img[i][1] = img[i][2] = img[i][3] = fill;
    memcpy(img[0], fb->c0, 16);
    memcpy(img[1], fb->c1, 16);
    memcpy(img[2], fb->clip, 64);
    for (uint32_t i = 0; i < QW; i++) {
        const uint32_t* q = hq(i);
        if (q) memcpy(img[(s->hdr_x + i) & (QW - 1)], q, 16);
    }
    uint32_t x = 0, w = 0;
    *nout = 0;
    for (uint32_t k = 0; k < s->nst; k++) {
        for (; w < s->sw[k]; w++) {
            uint32_t a = s->wa[w] & (QW - 1), lanes = s->wa[w] >> 16;
            for (int j = 0; j < 4; j++) if (lanes & (1u << j)) img[a][j] = s->wv[w][j];
        }
        uint32_t ms = s->sm[k] & 0xFFFF, top = s->sm[k] >> 16;
        if (ms == 0x0738) x = (0x24B - top) & 0x3FF;
        if (!r2_outline_run(img, ms, &x, out, cap, nout)) return 0;
    }
    return 1;
}

void r2_mesh_free(r2_mesh* m) {
    if (!m) return;
    r2_src_free(m->src);
    free(m->vtx);
    free(m->idx);
    free(m->b);
    free(m->gpu_tdec);
    free(m);
}

/* ---- the mesh store: handle = generation << 20 | index; a ring of 4 generations (frames in flight never span 4 level
 * loads). One writer (the EE thread, or a replay tool), readers on any thread (published with release order). ---------- */
#define GEN_N 4
#define MESH_MAX 16384u
#include <stdatomic.h>
typedef struct {
    uint32_t gen;
    r2_mesh* m[MESH_MAX];
    _Atomic uint32_t n;
} gen_store;
static gen_store* G[GEN_N];
static uint32_t cur_gen;

uint32_t r2_store_gen(void) { return cur_gen; }

const r2_mesh* (*r2_mesh_resolve)(uint32_t handle, uint32_t addr, int form);

uint32_t r2_store_new_gen(void) {
    gen_store* s = G[(cur_gen + 1) % GEN_N];
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) return cur_gen;
        G[(cur_gen + 1) % GEN_N] = s;
    } else {
        uint32_t n = atomic_load(&s->n);
        atomic_store(&s->n, 0);
        for (uint32_t i = 0; i < n; i++) { r2_mesh_free(s->m[i]); s->m[i] = NULL; }   /* (generation - 4: executed long ago) */
    }
    cur_gen++;
    s->gen = cur_gen;
    return cur_gen;
}

uint32_t r2_store_add(r2_mesh* m) {
    gen_store* s = G[cur_gen % GEN_N];
    if (!s || s->gen != cur_gen) return 0;
    uint32_t n = atomic_load(&s->n);
    if (n >= MESH_MAX) return 0;
    m->handle = cur_gen << 20 | n;
    s->m[n] = m;
    atomic_store_explicit(&s->n, n + 1, memory_order_release);
    return m->handle;
}

/* (offline replays) a mesh at a recorded handle: its generation's slot, index (holes stay NULL) */
int r2_store_put(uint32_t handle, r2_mesh* m) {
    uint32_t g = handle >> 20, i = handle & 0xFFFFF;
    if (i >= MESH_MAX) return 0;
    gen_store* s = G[g % GEN_N];
    if (!s) { if (!(s = calloc(1, sizeof *s))) return 0; G[g % GEN_N] = s; }
    if (s->gen != g) { atomic_store(&s->n, 0); memset(s->m, 0, sizeof s->m); s->gen = g; }
    m->handle = handle;
    s->m[i] = m;
    if (atomic_load(&s->n) <= i) atomic_store_explicit(&s->n, i + 1, memory_order_release);
    if (g > cur_gen) cur_gen = g;
    return 1;
}

/* (r2-m2) the scene slot ring (r2.h): here with the store, so that tools linking the frontend without the EE capture
 * (gs_replay) resolve it */
static r2_slot* slot_ring;
r2_slot* r2_slot_ring(void) {
    if (!slot_ring) slot_ring = calloc(R2_SLOTS, sizeof *slot_ring);
    return slot_ring;
}
const r2_slot* r2_slot_at(uint32_t index) { return slot_ring ? &slot_ring[index % R2_SLOTS] : NULL; }

const r2_mesh* r2_mesh_get(uint32_t handle) {
    uint32_t g = handle >> 20, i = handle & 0xFFFFF;
    gen_store* s = G[g % GEN_N];
    if (!s || s->gen != g || i >= atomic_load_explicit(&s->n, memory_order_acquire)) return NULL;
    return s->m[i];
}
