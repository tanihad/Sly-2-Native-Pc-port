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

typedef struct {
    const uint8_t* ram;
    uint32_t vu[QW][4];
    uint8_t wr[QW];                      /* written by this chain */
    uint32_t cycle, mask, row[4], col[4], mode, base, ofst, tops, top, dbf;
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
        uint32_t data = id == 0 || id == 3 || id == 4 ? a : addr + 16, next = addr + 16 + qwc * 16;
        if (qwc) {
            if (!ram_ok(data, qwc * 16)) return R2M_TAG;
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
        B->npass++;
    }
    return B->npass ? R2M_OK : R2M_PACKET;
}

static int sub_batch(dec* d, uint32_t X, int second) {
    uint32_t flags = vq(d, X + 11)[0] & 0xFFFF;
    if (flags & 0x702) return R2M_DESC;                          /* billboard modes, 0x2 (3358 lit path) */
    const uint32_t* q = vq(d, X + (second ? 17 : 13));
    const uint32_t* pk = vq(d, X + (second ? 18 : 14));
    uint32_t start = q[0] & 0x3FF, end = q[2] & 0x3FF, nl = q[3] & 0xFFFF, n = nl & 0x7FFF;
    if (!(nl & 0x8000) || n < 1 || n > 256 || end != start + 4 * n) return R2M_DESC;
    if (!grow((void**)&d->b, &d->capb, d->nb + 1, sizeof(r2_batch))) return R2M_SIZE;
    r2_batch* B = &d->b[d->nb];
    memset(B, 0, sizeof *B);
    int r = add_batch_packets(d, B, X, pk);
    if (r) return r;
    B->cull = !(flags & 0x80);
    B->fog_scale = vf(d, X + 9, 1);
    B->fog_wmax = vf(d, X + 9, 2);
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
    /* vertices */
    if (!grow((void**)&d->vtx, &d->capv, d->nvtx + n, sizeof(r2_vtx))) return R2M_SIZE;
    if (!grow((void**)&d->idx, &d->capi, d->nidx + 3 * n, 4)) return R2M_SIZE;
    B->vfirst = d->nvtx;
    B->vcount = n;
    B->ifirst = d->nidx;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t a = X + start + 4 * k;
        const uint32_t *uv = vq(d, a), *ca = vq(d, a + 1), *cb = vq(d, a + 2), *p = vq(d, a + 3);
        if (!d->wr[a & (QW - 1)] || !d->wr[(a + 1) & (QW - 1)] || !d->wr[(a + 2) & (QW - 1)] || !d->wr[(a + 3) & (QW - 1)])
            return R2M_VERTEX;
        if (((p[3] >> 23) & 0xFF) != 150) return R2M_VERTEX;      /* w ~ 2^23 + m (the flag / fog word) */
        r2_vtx* v = &d->vtx[d->nvtx++];
        memset(v, 0, sizeof *v);
        memcpy(&v->x, &p[0], 4);
        memcpy(&v->y, &p[1], 4);
        memcpy(&v->z, &p[2], 4);
        if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z)) return R2M_VERTEX;
        v->wbits = p[3];
        v->u = (int16_t)uv[0];
        v->v = (int16_t)uv[1];
        for (int c = 0; c < 4; c++) {
            if (ca[c] > 255 || cb[c] > 255) return R2M_VERTEX;
        }
        v->col[0] = ca[0] | ca[1] << 8 | ca[2] << 16 | ca[3] << 24;
        v->col[1] = cb[0] | cb[1] << 8 | cb[2] << 16 | cb[3] << 24;
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
            uint8_t a = (uint8_t)(v->col[c] >> 24);
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

static int mscal(dec* d, uint32_t vaddr) {
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
        if (vaddr * 8 != 0x2E20 && !d->batches_started) return R2M_MSCAL;   /* a mesh begins with the matrix set-up */
        d->batches_started = 1;
        return sub_batch(d, X, 0);
    case 0x2EE8:
        if (!d->batches_started) return R2M_MSCAL;
        return sub_batch(d, d->top, 1);
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
        case 0x01: d->cycle = imm; break;                        /* STCYCL */
        case 0x02: d->ofst = imm & 0x3FF; d->dbf = 0; d->tops = d->base; break;   /* OFFSET */
        case 0x03: d->base = imm & 0x3FF; break;                 /* BASE */
        case 0x04: break;                                        /* ITOP */
        case 0x05: d->mode = imm & 3; break;                     /* STMOD */
        case 0x06: break;                                        /* MSKPATH3 */
        case 0x10: case 0x11: case 0x13: break;                  /* FLUSHE / FLUSH / FLUSHA */
        case 0x14: { int r = mscal(d, imm); if (r) return r; break; }   /* MSCAL */
        case 0x20:                                               /* STMASK */
            if (pos >= d->nw) return R2M_VIF;
            d->mask = d->w[pos++];
            break;
        case 0x30: case 0x31:                                    /* STROW / STCOL */
            if (pos + 4 > d->nw) return R2M_VIF;
            memcpy(cmd == 0x30 ? d->row : d->col, d->w + pos, 16);
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
    int r = walk(d, addr);
    if (!r) r = interpret(d);
    if (!r && !d->nb) r = R2M_DESC;
    if (!r && form == 0 && d->w_fixed != 1) r = R2M_HEADER;     /* a whole packet carries its own matrix */
    if (!r && form == 1 && d->w_fixed != 2) r = R2M_HEADER;     /* a mesh DMA takes the entry's */
    if (r) {
        free(d->vtx); free(d->idx); free(d->b); free(d->w); free(d);
        return r;
    }
    r2_mesh* m = calloc(1, sizeof *m);
    if (!m) { free(d->vtx); free(d->idx); free(d->b); free(d->w); free(d); return R2M_SIZE; }
    m->addr = addr;
    m->bytes = d->bytes;
    m->hash = d->hash;
    m->form = form;
    m->has_w = d->w_fixed == 1;
    if (m->has_w) { memcpy(m->w, d->wm, sizeof m->w); m->fade = d->fade; }
    m->vtx = d->vtx; m->nvtx = d->nvtx;
    m->idx = d->idx; m->nidx = d->nidx;
    m->b = d->b; m->nb = d->nb;
    m->last_pass = m->b[m->nb - 1].npass - 1;
    for (int c = 0; c < 3; c++) { m->bmin[c] = 1e30f; m->bmax[c] = -1e30f; }
    for (uint32_t i = 0; i < m->nvtx; i++) {
        const float* p = &m->vtx[i].x;
        for (int c = 0; c < 3; c++) {
            if (p[c] < m->bmin[c]) m->bmin[c] = p[c];
            if (p[c] > m->bmax[c]) m->bmax[c] = p[c];
        }
    }
    free(d->w);
    free(d);
    *out = m;
    return R2M_OK;
}

int r2_header_parse(const uint8_t* ram, uint32_t addr, float w[16], float vu8[4]) {
    static dec* d;                       /* (EE thread only: one decoder state reused, no allocation per entry) */
    if (!d && !(d = calloc(1, sizeof *d))) return 0;
    uint32_t* keep_w = d->w;
    uint32_t keep_cap = d->capw;
    memset(d, 0, sizeof *d);
    d->w = keep_w;
    d->capw = keep_cap;
    d->ram = ram;
    d->base = 0x32;
    d->ofst = 0x1E7;
    d->tops = d->base;
    d->cycle = 0x0101;
    if (walk(d, addr) || d->nw > 4096) return 0;
    if (interpret(d) || d->batches_started || d->dbf) return 0;     /* (an MSCAL would have flipped the buffers) */
    uint32_t X = d->tops;
    for (uint32_t i = 0; i < 4; i++) if (!d->wr[X + i]) return 0;
    if (!d->wr[X + 8]) return 0;
    for (int i = 0; i < 16; i++) memcpy(&w[i], &vq(d, X + (uint32_t)(i >> 2))[i & 3], 4);
    memcpy(vu8, vq(d, X + 8), 16);
    return 1;
}

void r2_mesh_free(r2_mesh* m) {
    if (!m) return;
    free(m->vtx);
    free(m->idx);
    free(m->b);
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

const r2_mesh* r2_mesh_get(uint32_t handle) {
    uint32_t g = handle >> 20, i = handle & 0xFFFFF;
    gen_store* s = G[g % GEN_N];
    if (!s || s->gen != g || i >= atomic_load_explicit(&s->n, memory_order_acquire)) return NULL;
    return s->m[i];
}
