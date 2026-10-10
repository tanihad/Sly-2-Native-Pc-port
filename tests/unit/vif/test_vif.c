/* Unit tests for runtime/vif.c (VIF0/VIF1 command processor), run against fakes.c. */
#include "fakes.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b); checks++; if (a_ != b_) { failures++; \
    fprintf(stderr, "%s:%d: %s = 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #a, \
            (unsigned long long)a_, (unsigned long long)b_); } } while (0)

/* ---- helpers ------------------------------------------------------------------------------------ */

enum { R_STAT = 0x00, R_FBRST = 0x10, R_ERR = 0x20, R_MARK = 0x30, R_CYCLE = 0x40, R_MODE = 0x50,
       R_NUM = 0x60, R_MASK = 0x70, R_CODE = 0x80, R_ITOPS = 0x90, R_BASE = 0xA0, R_OFST = 0xB0,
       R_TOPS = 0xC0, R_ITOP = 0xD0, R_TOP = 0xE0, R_R0 = 0x100, R_C0 = 0x140 };
enum { ST_VPS = 3, ST_VEW = 1 << 2, ST_VGW = 1 << 3, ST_MRK = 1 << 6, ST_DBF = 1 << 7, ST_VSS = 1 << 8,
       ST_VFS = 1 << 9, ST_VIS = 1 << 10, ST_INT = 1 << 11, ST_ER0 = 1 << 12, ST_ER1 = 1 << 13,
       ST_FDR = 1 << 23 };
#define FQC(st) (((st) >> 24) & 0x1F)

static uint32_t reg_addr(int vif, uint32_t off) { return (vif ? 0x10003C00u : 0x10003800u) + off; }
static uint32_t rd(int vif, uint32_t off) {
    uint32_t v = 0xDEADBEEF;
    vif_access(reg_addr(vif, off), &v, 4, 0);
    return v;
}
static void wr(int vif, uint32_t off, uint32_t val) { vif_access(reg_addr(vif, off), &val, 4, 1); }

#define VC(cmd, num, imm) (((uint32_t)(cmd) << 24) | ((uint32_t)(num) << 16) | (uint32_t)(imm))
#define IBIT 0x80000000u

typedef struct {
    _Alignas(16) uint32_t w[300000];
    uint32_t n;
} stream;
static stream S, S2;

static void s_clear(stream* s) { s->n = 0; }
static void s_put(stream* s, uint32_t w) { s->w[s->n++] = w; }
static void s_bytes(stream* s, const uint8_t* b, uint32_t len) {
    uint32_t words = (len + 3) / 4;
    memset(&s->w[s->n], 0, words * 4);
    memcpy(&s->w[s->n], b, len);
    s->n += words;
}
static void s_align(stream* s) { while (s->n & 3) s_put(s, 0); }   /* NOP padding */

static void reset_all(void) {
    fakes_reset();
    wr(0, R_FBRST, 1);
    wr(1, R_FBRST, 1);
    wr(0, R_ERR, 0);
    wr(1, R_ERR, 0);
    fake_path3_mask = -1;
}

static uint32_t mem32(int vu, uint32_t qaddr, int elem) {
    uint32_t v;
    memcpy(&v, &fake_vu_data[vu][qaddr * 16 + elem * 4], 4);
    return v;
}
#define CHECK_VEC(vu, qa, X, Y, Z, W) do { CHECK_EQ(mem32(vu, qa, 0), X); CHECK_EQ(mem32(vu, qa, 1), Y); \
    CHECK_EQ(mem32(vu, qa, 2), Z); CHECK_EQ(mem32(vu, qa, 3), W); } while (0)

static uint32_t run(int vif, stream* s) {   /* whole stream in one DMA */
    s_align(s);
    return vif_dma(vif, s->w, s->n / 4);
}

static uint32_t rng_state = 1;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

/* ---- reference UNPACK model (written independently of vif.c, from closed-form addresses) ------- */

typedef struct { uint32_t cycle, mode, mask, r[4], c[4], tops; } ref_regs;

static uint32_t ref_elem(const uint8_t* p, int vl, int usn) {
    if (vl == 0) return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    if (vl == 1) {
        uint32_t h = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        return usn ? h : (h & 0x8000 ? h | 0xFFFF0000u : h);
    }
    return usn ? p[0] : (p[0] & 0x80 ? p[0] | 0xFFFFFF00u : p[0]);
}

static void ref_unpack(uint8_t* mem, uint32_t memq, int vif1, uint32_t code, const uint8_t* data, ref_regs* g) {
    uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    if (!num) num = 256;
    int vn = (cmd >> 2) & 3, vl = cmd & 3, masked = (cmd >> 4) & 1, usn = (imm >> 14) & 1;
    int v45 = vn == 3 && vl == 3;
    uint32_t eb = v45 ? 2 : 4u >> vl, vb = v45 ? 2 : eb * (uint32_t)(vn + 1);
    uint32_t base = (imm & 0x3FF) + ((vif1 && (imm & 0x8000)) ? g->tops : 0);
    uint32_t cl = g->cycle & 0xFF, wl = (g->cycle >> 8) & 0xFF;
    uint32_t n_in = 0;
    for (uint32_t k = 0; k < num; k++) if (cl >= wl || k % wl < cl) n_in++;
    for (uint32_t k = 0; k < num; k++) {
        uint32_t blk = k / wl, idx = k % wl, addr, in = 0;
        int isdata;
        if (cl >= wl) { addr = base + blk * cl + idx; isdata = 1; in = k; }
        else { addr = base + k; isdata = idx < cl; in = blk * cl + idx; }
        uint32_t row = idx < 3 ? idx : 3, d[4] = { 0, 0, 0, 0 };
        if (isdata) {
            const uint8_t* p = data + in * vb;
            if (v45) {
                uint32_t h = (uint32_t)p[0] | (uint32_t)p[1] << 8;
                d[0] = (h & 31) << 3; d[1] = ((h >> 5) & 31) << 3; d[2] = ((h >> 10) & 31) << 3; d[3] = (h >> 15) << 7;
            } else if (vn == 0) {
                d[0] = d[1] = d[2] = d[3] = ref_elem(p, vl, usn);
            } else if (vn == 1) {
                d[0] = d[2] = ref_elem(p, vl, usn); d[1] = d[3] = ref_elem(p + eb, vl, usn);
            } else if (vn == 2) {
                for (int j = 0; j < 3; j++) d[j] = ref_elem(p + j * eb, vl, usn);
                d[3] = in + 1 < n_in ? ref_elem(data + (in + 1) * vb, vl, usn) : 0;
            } else {
                for (int j = 0; j < 4; j++) d[j] = ref_elem(p + j * eb, vl, usn);
            }
        }
        uint8_t* q = mem + (addr % memq) * 16;
        for (int j = 0; j < 4; j++) {
            uint32_t m = masked ? (g->mask >> (row * 8 + j * 2)) & 3 : 0, val;
            if (m == 3) continue;
            if (m == 1) val = g->r[j];
            else if (m == 2) val = g->c[row];
            else if (!isdata) val = g->r[j];
            else if (g->mode == 1) val = d[j] + g->r[j];
            else if (g->mode == 2) val = g->r[j] = g->r[j] + d[j];
            else val = d[j];
            memcpy(q + j * 4, &val, 4);
        }
    }
}

/* ---- tests ------------------------------------------------------------------------------------- */

static void test_registers(void) {
    reset_all();
    CHECK_EQ(rd(1, R_STAT), 0);
    wr(1, R_ERR, 6);
    CHECK_EQ(rd(1, R_ERR), 6);
    wr(1, R_STAT, ST_FDR | ST_VIS);                    /* only FDR is writable */
    CHECK_EQ(rd(1, R_STAT), ST_FDR);
    wr(1, R_STAT, 0);
    wr(0, R_STAT, ST_FDR);                             /* VIF0 has no FDR */
    CHECK_EQ(rd(0, R_STAT), 0);

    s_clear(&S);
    s_put(&S, 0);                                      /* NOP: makes MSKPATH3 the last word */
    s_put(&S, VC(0x01, 0, 0x0204));                    /* STCYCL CL=4 WL=2 */
    s_put(&S, VC(0x05, 0, 0x0002));                    /* STMOD 2 */
    s_put(&S, VC(0x04, 0, 0x0123));                    /* ITOP */
    s_put(&S, VC(0x07, 0, 0xBEEF));                    /* MARK */
    s_put(&S, VC(0x20, 0, 0));                         /* STMASK */
    s_put(&S, 0xA5A5F00F);
    s_put(&S, VC(0x30, 0, 0));                         /* STROW */
    s_put(&S, 1); s_put(&S, 2); s_put(&S, 3); s_put(&S, 4);
    s_put(&S, VC(0x31, 0, 0));                         /* STCOL */
    s_put(&S, 5); s_put(&S, 6); s_put(&S, 7); s_put(&S, 8);
    s_put(&S, VC(0x03, 0, 0x0040));                    /* BASE */
    s_put(&S, VC(0x02, 0, 0x0100));                    /* OFFSET */
    s_put(&S, VC(0x06, 0, 0x8000));                    /* MSKPATH3 on */
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_EQ(rd(1, R_CYCLE), 0x0204);
    CHECK_EQ(rd(1, R_MODE), 2);
    CHECK_EQ(rd(1, R_ITOPS), 0x123);
    CHECK_EQ(rd(1, R_MARK), 0xBEEF);
    CHECK_EQ(rd(1, R_MASK), 0xA5A5F00F);
    for (int i = 0; i < 4; i++) { CHECK_EQ(rd(1, R_R0 + 16 * i), i + 1); CHECK_EQ(rd(1, R_C0 + 16 * i), i + 5); }
    CHECK_EQ(rd(1, R_BASE), 0x40);
    CHECK_EQ(rd(1, R_OFST), 0x100);
    CHECK_EQ(rd(1, R_TOPS), 0x40);                     /* OFFSET: TOPS = BASE, DBF = 0 */
    CHECK_EQ(fake_path3_mask, 1);
    CHECK_EQ(rd(1, R_STAT) & (ST_MRK | ST_DBF | ST_VPS), ST_MRK);
    CHECK_EQ(rd(1, R_CODE), VC(0x06, 0, 0x8000));
    wr(1, R_MARK, 0x1234);                             /* writing MARK clears MRK */
    CHECK_EQ(rd(1, R_MARK), 0x1234);
    CHECK_EQ(rd(1, R_STAT) & ST_MRK, 0);

    /* 64- and 128-bit register reads see the value in the low word */
    uint32_t q[4] = { 1, 1, 1, 1 };
    vif_access(reg_addr(1, R_MASK), q, 16, 0);
    CHECK_EQ(q[0], 0xA5A5F00F); CHECK_EQ(q[1], 0); CHECK_EQ(q[3], 0);

    /* reset clears state */
    wr(1, R_FBRST, 1);
    CHECK_EQ(rd(1, R_CYCLE), 0);
    CHECK_EQ(rd(1, R_MASK), 0);
    CHECK_EQ(fake_path3_mask, 0);
    CHECK_EQ(fake_intc, 0);
}

static void test_unpack_formats(void) {
    reset_all();
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0101));                    /* CL = WL = 1 */
    /* V4-32 at 0x10, 2 vectors */
    s_put(&S, VC(0x6C, 2, 0x010));
    for (uint32_t i = 0; i < 8; i++) s_put(&S, 0x100 + i);
    /* S-32 at 0x20 */
    s_put(&S, VC(0x60, 1, 0x020)); s_put(&S, 0xCAFEBABE);
    /* S-16 signed at 0x21, unsigned at 0x22 */
    s_put(&S, VC(0x61, 1, 0x021)); s_put(&S, 0xFFFE);
    s_put(&S, VC(0x61, 1, 0x4022)); s_put(&S, 0xFFFE);
    /* S-8 signed, 3 vectors at 0x23: bytes 0x80, 0x7F, 0x01 */
    s_put(&S, VC(0x62, 3, 0x023)); s_put(&S, 0x00017F80);
    /* V2-32 at 0x30 */
    s_put(&S, VC(0x64, 1, 0x030)); s_put(&S, 11); s_put(&S, 22);
    /* V2-16 unsigned at 0x31, V2-8 signed at 0x32 */
    s_put(&S, VC(0x65, 1, 0x4031)); s_put(&S, 0x8001FFFF);
    s_put(&S, VC(0x66, 1, 0x032)); s_put(&S, 0x000081FF);
    /* V3-32, 2 vectors at 0x40: w = next element, 0 after the last */
    s_put(&S, VC(0x68, 2, 0x040));
    for (uint32_t i = 1; i <= 6; i++) s_put(&S, i);
    /* V3-16 signed, 2 vectors at 0x42 (12 bytes) */
    s_put(&S, VC(0x69, 2, 0x042));
    s_put(&S, 0x0002FFFF); s_put(&S, 0x00040003); s_put(&S, 0x00060005);
    /* V3-8 unsigned, 1 vector at 0x44 (3 bytes + padding) */
    s_put(&S, VC(0x6A, 1, 0x4044)); s_put(&S, 0xAA0302FF);
    /* V4-16 signed at 0x50, V4-8 unsigned at 0x51 */
    s_put(&S, VC(0x6D, 1, 0x050)); s_put(&S, 0x80000001); s_put(&S, 0x7FFFFFFF);
    s_put(&S, VC(0x6E, 1, 0x4051)); s_put(&S, 0x80FF0102);
    /* V4-5: two vectors at 0x52 */
    s_put(&S, VC(0x6F, 2, 0x052)); s_put(&S, (0x0000u | (1u << 10) | (2u << 5) | 3u) << 16 | (0x8000u | (31u << 10) | (1u << 5) | 2u));
    CHECK_EQ(run(1, &S), S.n / 4);

    CHECK_VEC(1, 0x10, 0x100, 0x101, 0x102, 0x103);
    CHECK_VEC(1, 0x11, 0x104, 0x105, 0x106, 0x107);
    CHECK_VEC(1, 0x20, 0xCAFEBABE, 0xCAFEBABE, 0xCAFEBABE, 0xCAFEBABE);
    CHECK_VEC(1, 0x21, 0xFFFFFFFE, 0xFFFFFFFE, 0xFFFFFFFE, 0xFFFFFFFE);
    CHECK_VEC(1, 0x22, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE);
    CHECK_VEC(1, 0x23, 0xFFFFFF80, 0xFFFFFF80, 0xFFFFFF80, 0xFFFFFF80);
    CHECK_VEC(1, 0x24, 0x7F, 0x7F, 0x7F, 0x7F);
    CHECK_VEC(1, 0x25, 1, 1, 1, 1);
    CHECK_VEC(1, 0x30, 11, 22, 11, 22);
    CHECK_VEC(1, 0x31, 0xFFFF, 0x8001, 0xFFFF, 0x8001);
    CHECK_VEC(1, 0x32, 0xFFFFFFFF, 0xFFFFFF81, 0xFFFFFFFF, 0xFFFFFF81);
    CHECK_VEC(1, 0x40, 1, 2, 3, 4);
    CHECK_VEC(1, 0x41, 4, 5, 6, 0);
    CHECK_VEC(1, 0x42, 0xFFFFFFFF, 2, 3, 4);
    CHECK_VEC(1, 0x43, 4, 5, 6, 0);
    CHECK_VEC(1, 0x44, 0xFF, 2, 3, 0);
    CHECK_VEC(1, 0x50, 1, 0xFFFF8000, 0xFFFFFFFF, 0x7FFF);
    CHECK_VEC(1, 0x51, 2, 1, 0xFF, 0x80);
    CHECK_VEC(1, 0x52, 2 << 3, 1 << 3, 31 << 3, 0x80);
    CHECK_VEC(1, 0x53, 3 << 3, 2 << 3, 1 << 3, 0);
    CHECK_EQ(rd(1, R_NUM), 0);
    CHECK_EQ(rd(1, R_STAT), 0);
    CHECK_EQ(fake_unhandled, 0);
}

static void test_unpack_mask_mode_cycle(void) {
    reset_all();
    s_clear(&S);
    s_put(&S, VC(0x30, 0, 0)); s_put(&S, 100); s_put(&S, 200); s_put(&S, 300); s_put(&S, 400);   /* STROW */
    s_put(&S, VC(0x31, 0, 0)); s_put(&S, 0xC0); s_put(&S, 0xC1); s_put(&S, 0xC2); s_put(&S, 0xC3); /* STCOL */
    /* Mask row 0: x data, y row, z col, w protected; row 1: all data; row 2: all col; row 3: all row */
    s_put(&S, VC(0x20, 0, 0)); s_put(&S, (0xE4u) | (0x00u << 8) | (0xAAu << 16) | (0x55u << 24));
    s_put(&S, VC(0x01, 0, 0x0404));                    /* CL = WL = 4 */
    for (uint32_t a = 0; a < 6; a++) memset(&fake_vu_data[1][(0x100 + a) * 16], 0xEE, 16);
    s_put(&S, VC(0x7C, 5, 0x100));                     /* masked V4-32, 5 vectors */
    for (uint32_t i = 0; i < 20; i++) s_put(&S, i + 1);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x100, 1, 200, 0xC0, 0xEEEEEEEE);
    CHECK_VEC(1, 0x101, 5, 6, 7, 8);
    CHECK_VEC(1, 0x102, 0xC2, 0xC2, 0xC2, 0xC2);
    CHECK_VEC(1, 0x103, 100, 200, 300, 400);
    CHECK_VEC(1, 0x104, 17, 200, 0xC0, 0xEEEEEEEE);    /* 5th write starts a new cycle: row 0 */

    /* Unmasked UNPACK ignores MASK */
    s_clear(&S);
    s_put(&S, VC(0x6C, 1, 0x110)); s_put(&S, 9); s_put(&S, 8); s_put(&S, 7); s_put(&S, 6);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x110, 9, 8, 7, 6);

    /* Row index clamps at 3 when WL > 4 */
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0606));
    s_put(&S, VC(0x7C, 6, 0x120));
    for (uint32_t i = 0; i < 24; i++) s_put(&S, 0x1000 + i);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x124, 100, 200, 300, 400);
    CHECK_VEC(1, 0x125, 100, 200, 300, 400);

    /* MODE 1 (offset): data + row, row unchanged */
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0101));
    s_put(&S, VC(0x05, 0, 1));
    s_put(&S, VC(0x6C, 2, 0x130));
    for (uint32_t i = 0; i < 8; i++) s_put(&S, i);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x130, 100, 201, 302, 403);
    CHECK_VEC(1, 0x131, 104, 205, 306, 407);
    CHECK_EQ(rd(1, R_R0), 100);

    /* MODE 2 (difference): row += data, written; masked elements leave the row alone */
    s_clear(&S);
    s_put(&S, VC(0x05, 0, 2));
    s_put(&S, VC(0x20, 0, 0)); s_put(&S, 0xC0);        /* row 0: w protected */
    s_put(&S, VC(0x7C, 2, 0x140));
    for (uint32_t i = 0; i < 8; i++) s_put(&S, 1);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x140, 101, 201, 301, 0);
    CHECK_VEC(1, 0x141, 102, 202, 302, 0);
    CHECK_EQ(rd(1, R_R0), 102);
    CHECK_EQ(rd(1, R_R0 + 0x30), 400);
    s_clear(&S);
    s_put(&S, VC(0x05, 0, 0));
    CHECK_EQ(run(1, &S), 1);

    /* Skipping write: CL = 4, WL = 2 -> addresses 0, 1, 4, 5 */
    reset_all();
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0204));
    s_put(&S, VC(0x6C, 4, 0x000));
    for (uint32_t i = 0; i < 16; i++) s_put(&S, 0x10 + i);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0, 0x10, 0x11, 0x12, 0x13);
    CHECK_VEC(1, 1, 0x14, 0x15, 0x16, 0x17);
    CHECK_VEC(1, 2, 0, 0, 0, 0);
    CHECK_VEC(1, 3, 0, 0, 0, 0);
    CHECK_VEC(1, 4, 0x18, 0x19, 0x1A, 0x1B);
    CHECK_VEC(1, 5, 0x1C, 0x1D, 0x1E, 0x1F);

    /* Filling write: CL = 1, WL = 3, masked: row 1 = R, row 2 = C2 */
    s_clear(&S);
    s_put(&S, VC(0x30, 0, 0)); s_put(&S, 0xA0); s_put(&S, 0xA1); s_put(&S, 0xA2); s_put(&S, 0xA3);
    s_put(&S, VC(0x31, 0, 0)); s_put(&S, 0xB0); s_put(&S, 0xB1); s_put(&S, 0xB2); s_put(&S, 0xB3);
    s_put(&S, VC(0x20, 0, 0)); s_put(&S, (0x55u << 8) | (0xAAu << 16));
    s_put(&S, VC(0x01, 0, 0x0301));
    s_put(&S, VC(0x7D, 6, 0x200));                     /* masked V4-16: 2 input vectors, 16 bytes */
    s_put(&S, 0x00020001); s_put(&S, 0x00040003); s_put(&S, 0x00060005); s_put(&S, 0x00080007);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x200, 1, 2, 3, 4);
    CHECK_VEC(1, 0x201, 0xA0, 0xA1, 0xA2, 0xA3);
    CHECK_VEC(1, 0x202, 0xB2, 0xB2, 0xB2, 0xB2);
    CHECK_VEC(1, 0x203, 5, 6, 7, 8);
    CHECK_VEC(1, 0x204, 0xA0, 0xA1, 0xA2, 0xA3);
    CHECK_VEC(1, 0x205, 0xB2, 0xB2, 0xB2, 0xB2);

    /* Filling with NUM not a multiple of WL: CL = 2, WL = 3, NUM = 5 -> 4 inputs, unmasked fill = row */
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0302));
    s_put(&S, VC(0x60, 5, 0x210));                     /* S-32 */
    s_put(&S, 1); s_put(&S, 2); s_put(&S, 3); s_put(&S, 4);
    s_put(&S, VC(0x07, 0, 0x77));                      /* MARK right after: proves the data length */
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x210, 1, 1, 1, 1);
    CHECK_VEC(1, 0x211, 2, 2, 2, 2);
    CHECK_VEC(1, 0x212, 0xA0, 0xA1, 0xA2, 0xA3);
    CHECK_VEC(1, 0x213, 3, 3, 3, 3);
    CHECK_VEC(1, 0x214, 4, 4, 4, 4);
    CHECK_EQ(rd(1, R_MARK), 0x77);

    /* CL = 0: every vector is a fill, no data words at all */
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0200));
    s_put(&S, VC(0x6C, 3, 0x220));
    s_put(&S, VC(0x07, 0, 0x78));
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x220, 0xA0, 0xA1, 0xA2, 0xA3);
    CHECK_VEC(1, 0x222, 0xA0, 0xA1, 0xA2, 0xA3);
    CHECK_EQ(rd(1, R_MARK), 0x78);
}

static void test_unpack_address(void) {
    reset_all();
    /* Wrap at the end of VU1 data memory (1024 qwords) */
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0101));
    s_put(&S, VC(0x6C, 3, 0x3FF));
    for (uint32_t i = 0; i < 12; i++) s_put(&S, 0x50 + i);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x3FF, 0x50, 0x51, 0x52, 0x53);
    CHECK_VEC(1, 0x000, 0x54, 0x55, 0x56, 0x57);
    CHECK_VEC(1, 0x001, 0x58, 0x59, 0x5A, 0x5B);

    /* FLG adds TOPS (VIF1) */
    s_clear(&S);
    s_put(&S, VC(0x03, 0, 0x300));                     /* BASE */
    s_put(&S, VC(0x02, 0, 0x080));                     /* OFFSET -> TOPS = BASE */
    s_put(&S, VC(0x60, 1, 0x8005));                    /* S-32 to TOPS + 5 */
    s_put(&S, 0x42);
    s_put(&S, VC(0x60, 1, 0x8105));                    /* TOPS + 0x105 wraps: 0x405 -> 5 */
    s_put(&S, 0x43);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x305, 0x42, 0x42, 0x42, 0x42);
    CHECK_VEC(1, 0x005, 0x43, 0x43, 0x43, 0x43);

    /* VIF0: 256-qword memory, FLG ignored */
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0101));
    s_put(&S, VC(0x60, 2, 0x80FF));
    s_put(&S, 7); s_put(&S, 8);
    CHECK_EQ(run(0, &S), S.n / 4);
    CHECK_VEC(0, 0xFF, 7, 7, 7, 7);
    CHECK_VEC(0, 0x00, 8, 8, 8, 8);
}

/* Randomised UNPACKs against the reference, delivered in different ways. */
static void deliver(int vif, stream* s, int how) {
    uint32_t total = s->n / 4;
    if (how == 0) { CHECK_EQ(vif_dma(vif, s->w, total), total); return; }
    if (how == 1) {
        for (uint32_t q = 0; q < total;) {
            uint32_t c = 1 + rnd() % 7;
            if (c > total - q) c = total - q;
            uint32_t k = vif_dma(vif, s->w + q * 4, c);
            CHECK_EQ(k, c);
            if (k != c) return;
            q += c;
        }
        return;
    }
    uint32_t fifo = vif ? 0x10005000u : 0x10004000u;
    if (how == 2) {
        for (uint32_t i = 0; i < s->n; i++) { uint32_t w = s->w[i]; vif_access(fifo, &w, 4, 1); }
        return;
    }
    for (uint32_t q = 0; q < total; q++) {             /* 128-bit FIFO writes, or TTE tags */
        if (how == 3) { uint32_t w[4]; memcpy(w, &s->w[q * 4], 16); vif_access(fifo, w, 16, 1); }
        else {
            _Alignas(16) uint32_t tag[4] = { 0x70000000, 0x12345678, s->w[q * 4], s->w[q * 4 + 1] };
            CHECK_EQ(vif_dma_tag(vif, tag), 1);
            CHECK_EQ(vif_dma(vif, &s->w[q * 4 + 2], 0), 0);
            _Alignas(16) uint32_t tag2[4] = { 0x70000000, 0, s->w[q * 4 + 2], s->w[q * 4 + 3] };
            CHECK_EQ(vif_dma_tag(vif, tag2), 1);
        }
    }
}

static uint8_t ref_mem[16384];

static void test_unpack_random(void) {
    reset_all();
    rng_state = 0x1234567;
    int bad = 0;
    for (int iter = 0; iter < 4000 && bad < 5; iter++) {
        int vif = rnd() % 4 ? 1 : 0;
        uint32_t memq = vif ? 1024 : 256, cmd;
        do cmd = 0x60 | (rnd() & 0x1F); while ((cmd & 3) == 3 && (cmd & 0xC) != 0xC);
        uint32_t num = rnd() % 257 & 0xFF, imm = (rnd() & 0x3FF) | (rnd() & 0xC000);
        uint32_t code = VC(cmd, num, imm);
        ref_regs g;
        g.cycle = (rnd() % 6) | (1 + rnd() % 6) << 8;
        g.mode = rnd() % 3;
        g.mask = rnd();
        for (int j = 0; j < 4; j++) { g.r[j] = rnd(); g.c[j] = rnd(); }
        g.tops = vif ? rnd() & 0x3FF : 0;

        uint32_t cl = g.cycle & 0xFF, wl = g.cycle >> 8, n = num ? num : 256, n_in = 0;
        for (uint32_t k = 0; k < n; k++) if (cl >= wl || k % wl < cl) n_in++;
        int v45 = (cmd & 0xF) == 0xF;
        uint32_t vb = v45 ? 2 : (4u >> (cmd & 3)) * (((cmd >> 2) & 3) + 1);
        static uint8_t data[4096];
        uint32_t len = n_in * vb;
        for (uint32_t i = 0; i < (len + 3) / 4 * 4; i++) data[i] = (uint8_t)rnd();
        for (uint32_t i = len; i < (len + 3) / 4 * 4; i++) data[i] = 0;

        for (uint32_t i = 0; i < memq * 16; i++) fake_vu_data[vif][i] = (uint8_t)(i * 7 + iter);
        memcpy(ref_mem, fake_vu_data[vif], memq * 16);

        s_clear(&S);
        s_put(&S, VC(0x01, 0, g.cycle));
        s_put(&S, VC(0x05, 0, g.mode));
        s_put(&S, VC(0x20, 0, 0)); s_put(&S, g.mask);
        s_put(&S, VC(0x30, 0, 0)); for (int j = 0; j < 4; j++) s_put(&S, g.r[j]);
        s_put(&S, VC(0x31, 0, 0)); for (int j = 0; j < 4; j++) s_put(&S, g.c[j]);
        if (vif) { s_put(&S, VC(0x03, 0, g.tops)); s_put(&S, VC(0x02, 0, rnd() & 0x3FF)); }
        s_put(&S, code);
        s_bytes(&S, data, len);
        s_align(&S);

        ref_unpack(ref_mem, memq, vif, code, data, &g);
        int how = (int)(rnd() % 5);
        deliver(vif, &S, how);

        if (memcmp(ref_mem, fake_vu_data[vif], memq * 16) != 0) {
            bad++;
            failures++;
            uint32_t q = 0;
            while (memcmp(ref_mem + q * 16, fake_vu_data[vif] + q * 16, 16) == 0) q++;
            fprintf(stderr, "random unpack %d: VIF%d code 0x%08X cycle 0x%04X mode %u how %d: qword 0x%X differs "
                    "(got %08X %08X %08X %08X, expected %08X %08X %08X %08X)\n", iter, vif, code, g.cycle, g.mode,
                    how, q, mem32(vif, q, 0), mem32(vif, q, 1), mem32(vif, q, 2), mem32(vif, q, 3),
                    ((uint32_t*)ref_mem)[q * 4], ((uint32_t*)ref_mem)[q * 4 + 1], ((uint32_t*)ref_mem)[q * 4 + 2],
                    ((uint32_t*)ref_mem)[q * 4 + 3]);
        }
        checks++;
        for (int j = 0; j < 4; j++) CHECK_EQ(rd(vif, R_R0 + 16 * j), g.r[j]);
        uint32_t st = rd(vif, R_STAT);
        CHECK_EQ(st & (ST_VPS | 0x1F000000u | ST_VIS | ST_ER1), 0);
        CHECK_EQ(rd(vif, R_NUM), 0);
    }
}

static void test_mpg(void) {
    reset_all();
    s_clear(&S);
    s_put(&S, 0);                                      /* NOP: MPG data must be 64-bit aligned */
    s_put(&S, VC(0x4A, 3, 0x010));                     /* 3 instructions at byte 0x80 */
    for (uint32_t i = 0; i < 6; i++) s_put(&S, 0xA000 + i);
    CHECK_EQ(run(1, &S), S.n / 4);
    for (uint32_t i = 0; i < 6; i++) { uint32_t v; memcpy(&v, &fake_vu_micro[1][0x80 + 4 * i], 4); CHECK_EQ(v, 0xA000 + i); }
    CHECK_EQ(rd(1, R_NUM), 0);

    /* NUM = 0 -> 256 instructions; wraps at the end of micro memory */
    s_clear(&S);
    s_put(&S, 0);
    s_put(&S, VC(0x4A, 0, 0x7FF));                     /* last instruction slot of VU1 */
    for (uint32_t i = 0; i < 512; i++) s_put(&S, 0xB0000 + i);
    s_put(&S, VC(0x07, 0, 0x55));
    CHECK_EQ(run(1, &S), S.n / 4);
    uint32_t v;
    memcpy(&v, &fake_vu_micro[1][0x3FF8], 4); CHECK_EQ(v, 0xB0000);
    memcpy(&v, &fake_vu_micro[1][0x3FFC], 4); CHECK_EQ(v, 0xB0001);
    memcpy(&v, &fake_vu_micro[1][0], 4); CHECK_EQ(v, 0xB0002);
    memcpy(&v, &fake_vu_micro[1][254 * 8 + 4], 4); CHECK_EQ(v, 0xB0000 + 511);
    CHECK_EQ(rd(1, R_MARK), 0x55);

    /* VIF0 MPG goes to VU0 micro memory (4 KB) */
    s_clear(&S);
    s_put(&S, 0);
    s_put(&S, VC(0x4A, 1, 0x1FF));
    s_put(&S, 0x11); s_put(&S, 0x22);
    CHECK_EQ(run(0, &S), S.n / 4);
    memcpy(&v, &fake_vu_micro[0][0xFF8], 4); CHECK_EQ(v, 0x11);
    memcpy(&v, &fake_vu_micro[0][0xFFC], 4); CHECK_EQ(v, 0x22);

    /* MPG waits while the VU runs */
    fake_vu1_busy = 1;
    s_clear(&S);
    s_put(&S, 0);
    s_put(&S, VC(0x4A, 1, 0x000));
    s_put(&S, 0x33); s_put(&S, 0x44);
    s_align(&S);
    CHECK_EQ(vif_dma(1, S.w, 1), 1);                   /* NOP taken; the rest waits in the FIFO */
    CHECK_EQ(FQC(rd(1, R_STAT)), 1);
    CHECK(rd(1, R_STAT) & ST_VEW);
    memcpy(&v, &fake_vu_micro[1][0], 4); CHECK(v != 0x33);
    fake_vu1_busy = 0;
    CHECK_EQ(FQC(rd(1, R_STAT)), 0);                   /* a STAT poll lets it continue */
    memcpy(&v, &fake_vu_micro[1][0], 4); CHECK_EQ(v, 0x33);
}

static void test_mscal_double_buffer(void) {
    reset_all();
    s_clear(&S);
    s_put(&S, VC(0x03, 0, 0x010));                     /* BASE 0x10 */
    s_put(&S, VC(0x02, 0, 0x200));                     /* OFFSET 0x200: TOPS = 0x10, DBF = 0 */
    s_put(&S, VC(0x04, 0, 0x033));                     /* ITOP 0x33 */
    s_put(&S, VC(0x60, 1, 0x8000)); s_put(&S, 0xAA);   /* S-32 to TOPS */
    s_put(&S, VC(0x14, 0, 0x0040));                    /* MSCAL 0x40 */
    s_put(&S, VC(0x60, 1, 0x8000)); s_put(&S, 0xBB);
    s_put(&S, VC(0x15, 0, 0x0050));                    /* MSCALF 0x50 */
    s_put(&S, VC(0x60, 1, 0x8000)); s_put(&S, 0xCC);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_VEC(1, 0x010, 0xCC, 0xCC, 0xCC, 0xCC);       /* 0xAA, then 0xCC after the second start */
    CHECK_VEC(1, 0x210, 0xBB, 0xBB, 0xBB, 0xBB);
    CHECK_VEC(1, 0x011, 0, 0, 0, 0);
    CHECK_EQ(fake_vu1_starts, 2);
    CHECK_EQ(fake_vu1_start_addr[0], 0x40);
    CHECK_EQ(fake_vu1_start_addr[1], 0x50);
    CHECK_EQ(fake_top_at_start[0], 0x010);             /* vu_set_vif_tops before vu1_start */
    CHECK_EQ(fake_top_at_start[1], 0x210);
    CHECK_EQ(fake_tops_itop[1], 0x33);
    CHECK_EQ(rd(1, R_ITOP), 0x33);
    CHECK_EQ(rd(1, R_TOP), 0x210);
    CHECK_EQ(rd(1, R_TOPS), 0x010);                    /* two starts: back to BASE */
    CHECK_EQ(rd(1, R_STAT) & ST_DBF, 0);
    s_clear(&S);
    s_put(&S, VC(0x14, 0, 0x0060));
    CHECK_EQ(run(1, &S), 1);
    CHECK_EQ(rd(1, R_STAT) & ST_DBF, ST_DBF);
    CHECK_EQ(rd(1, R_TOPS), 0x210);
    CHECK_EQ(rd(1, R_TOP), 0x010);
    /* OFFSET resets the double buffer */
    s_clear(&S);
    s_put(&S, VC(0x02, 0, 0x100));
    CHECK_EQ(run(1, &S), 1);
    CHECK_EQ(rd(1, R_STAT) & ST_DBF, 0);
    CHECK_EQ(rd(1, R_TOPS), 0x010);

    /* MSCAL waits while VU1 is busy */
    fake_vu1_busy = 1;
    int before = fake_vu1_starts;
    s_clear(&S);
    s_put(&S, VC(0x14, 0, 0x0070)); s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    s_put(&S, VC(0x07, 0, 0x99)); s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    CHECK_EQ(vif_dma(1, S.w, 2), 0);
    CHECK_EQ(fake_vu1_starts, before);
    fake_vu1_busy = 0;
    CHECK_EQ(vif_dma(1, S.w, 2), 2);
    CHECK_EQ(fake_vu1_starts, before + 1);
    CHECK_EQ(fake_vu1_start_addr[before], 0x70);
    CHECK_EQ(rd(1, R_MARK), 0x99);

    /* VIF0 MSCAL: sets ITOP, does not touch VU1 */
    s_clear(&S);
    s_put(&S, VC(0x04, 0, 0x12));
    s_put(&S, VC(0x14, 0, 0x0010));
    CHECK_EQ(run(0, &S), 1);
    CHECK_EQ(fake_vu1_starts, before + 1);
    CHECK_EQ(rd(0, R_ITOP), 0x12);
    CHECK_EQ(fake_tops_itop[0], 0x12);
}

static void test_direct(void) {
    reset_all();
    /* Aligned DIRECT: code in word 3 */
    s_clear(&S);
    s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    s_put(&S, VC(0x50, 0, 3));
    for (uint32_t i = 0; i < 12; i++) s_put(&S, 0x900 + i);
    s_put(&S, VC(0x51, 0, 1));                         /* DIRECTHL, misaligned data */
    for (uint32_t i = 0; i < 4; i++) s_put(&S, 0x990 + i);
    CHECK_EQ(run(1, &S), S.n / 4);
    CHECK_EQ(fake_gif_qwc, 4);
    for (uint32_t i = 0; i < 12; i++) { uint32_t v; memcpy(&v, fake_gif + 4 * i, 4); CHECK_EQ(v, 0x900 + i); }
    for (uint32_t i = 0; i < 4; i++) { uint32_t v; memcpy(&v, fake_gif + 48 + 4 * i, 4); CHECK_EQ(v, 0x990 + i); }
    CHECK_EQ(fake_gif_misaligned, 0);

    /* Split across many calls and FIFO writes */
    fake_gif_qwc = 0;
    s_clear(&S);
    s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    s_put(&S, VC(0x50, 0, 5));
    for (uint32_t i = 0; i < 20; i++) s_put(&S, 0x700 + i);
    s_align(&S);
    for (uint32_t q = 0; q < S.n / 4; q++) CHECK_EQ(vif_dma(1, S.w + 4 * q, 1), 1);
    CHECK_EQ(fake_gif_qwc, 5);
    for (uint32_t i = 0; i < 20; i++) { uint32_t v; memcpy(&v, fake_gif + 4 * i, 4); CHECK_EQ(v, 0x700 + i); }
    CHECK_EQ(rd(1, R_STAT) & ST_VPS, 0);

    /* GIF refuses: VGW, the rest is retried on the next call */
    fake_gif_qwc = 0;
    fake_gif_limit = 1;
    s_clear(&S);
    s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    s_put(&S, VC(0x50, 0, 3));
    for (uint32_t i = 0; i < 12; i++) s_put(&S, 0x500 + i);
    s_put(&S, VC(0x07, 0, 0x31)); s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    uint32_t total = S.n / 4, q = 0;
    q += vif_dma(1, S.w, total);
    CHECK_EQ(q, 2);                                    /* tag qword + 1 data qword */
    CHECK(rd(1, R_STAT) & ST_VGW);
    CHECK_EQ(rd(1, R_STAT) & ST_VPS, 1);
    fake_gif_limit = 0;
    CHECK_EQ(vif_dma(1, S.w + 4 * q, total - q), 0);
    fake_gif_limit = -1;
    q += vif_dma(1, S.w + 4 * q, total - q);
    CHECK_EQ(q, total);
    CHECK_EQ(fake_gif_qwc, 3);
    for (uint32_t i = 0; i < 12; i++) { uint32_t v; memcpy(&v, fake_gif + 4 * i, 4); CHECK_EQ(v, 0x500 + i); }
    CHECK_EQ(rd(1, R_STAT) & ST_VGW, 0);
    CHECK_EQ(rd(1, R_MARK), 0x31);

    /* DIRECT is VIF1 only: on VIF0 it is an invalid command (ER1 stall) */
    s_clear(&S);
    s_put(&S, VC(0x50, 0, 1));
    s_put(&S, VC(0x07, 0, 0x32));
    CHECK_EQ(run(0, &S), 1);                           /* qword taken, 3 words wait in the FIFO */
    CHECK(rd(0, R_STAT) & ST_ER1);
    CHECK(fake_intc & (1u << INTC_VIF0));
    CHECK_EQ(FQC(rd(0, R_STAT)), 1);
    wr(0, R_FBRST, 8);                                 /* STC: the bad code is skipped, MARK runs */
    CHECK_EQ(rd(0, R_STAT), ST_MRK);
    CHECK_EQ(rd(0, R_MARK), 0x32);
    /* with ERR.ME1 the invalid command is ignored */
    wr(0, R_ERR, 4);
    CHECK_EQ(run(0, &S), 1);
    CHECK_EQ(rd(0, R_STAT) & ST_ER1, 0);
}

static void test_ibit(void) {
    reset_all();
    /* NOP|i, MARK passes, STCYCL stalls */
    s_clear(&S);
    s_put(&S, VC(0x00, 0, 0) | IBIT);
    s_put(&S, VC(0x07, 0, 0x1234));
    s_put(&S, VC(0x01, 0, 0x0404));
    s_put(&S, 0);
    s_put(&S, VC(0x05, 0, 1)); s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    CHECK_EQ(vif_dma(1, S.w, 2), 1);
    CHECK_EQ(fake_intc_count[INTC_VIF1], 1);
    uint32_t st = rd(1, R_STAT);
    CHECK_EQ(st & (ST_VIS | ST_INT | ST_MRK), ST_VIS | ST_INT | ST_MRK);
    CHECK_EQ(FQC(st), 1);
    CHECK_EQ(rd(1, R_MARK), 0x1234);
    CHECK_EQ(rd(1, R_CYCLE), 0);
    CHECK_EQ(vif_dma(1, S.w + 4, 1), 0);               /* still stalled */
    wr(1, R_FBRST, 8);                                 /* STC */
    st = rd(1, R_STAT);
    CHECK_EQ(st & (ST_VIS | ST_INT), 0);
    CHECK_EQ(FQC(st), 0);
    CHECK_EQ(rd(1, R_CYCLE), 0x0404);
    CHECK_EQ(vif_dma(1, S.w + 4, 1), 1);
    CHECK_EQ(rd(1, R_MODE), 1);
    CHECK_EQ(fake_intc_count[INTC_VIF1], 1);

    /* i bit on UNPACK: interrupt only after its last data word; stall at qword boundary */
    reset_all();
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0101));
    s_put(&S, VC(0x6C, 2, 0x000) | IBIT);
    for (uint32_t i = 0; i < 6; i++) s_put(&S, i + 1);
    for (uint32_t i = 0; i < 2; i++) s_put(&S, i + 7);
    s_put(&S, VC(0x60, 1, 0x010)); s_put(&S, 0x77);
    s_align(&S);
    CHECK_EQ(vif_dma(1, S.w, 2), 2);
    CHECK_EQ(fake_intc, 0);
    CHECK_EQ(rd(1, R_NUM), 1);
    CHECK_EQ(rd(1, R_STAT) & ST_VPS, 1);
    /* qword 2 = [7, 8, UNPACK, 0x77]: words 7, 8 finish the UNPACK, then the stall hits */
    CHECK_EQ(vif_dma(1, S.w + 8, 1), 1);
    CHECK_EQ(fake_intc_count[INTC_VIF1], 1);
    CHECK_VEC(1, 1, 5, 6, 7, 8);
    CHECK(rd(1, R_STAT) & ST_VIS);
    CHECK_EQ(FQC(rd(1, R_STAT)), 1);
    CHECK_EQ(mem32(1, 0x10, 0), 0);
    wr(1, R_FBRST, 8);
    CHECK_EQ(mem32(1, 0x10, 0), 0x77);
    CHECK_EQ(rd(1, R_STAT), 0);

    /* STC before the next command arrives cancels the stall */
    reset_all();
    s_clear(&S);
    s_put(&S, 0); s_put(&S, 0); s_put(&S, 0); s_put(&S, IBIT);
    s_put(&S, VC(0x05, 0, 2)); s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    CHECK_EQ(vif_dma(1, S.w, 1), 1);
    CHECK_EQ(rd(1, R_STAT) & (ST_INT | ST_VIS), ST_INT);
    wr(1, R_FBRST, 8);
    CHECK_EQ(vif_dma(1, S.w + 4, 1), 1);
    CHECK_EQ(rd(1, R_MODE), 2);

    /* Stall at a qword boundary: nothing of the next qword is taken */
    reset_all();
    CHECK_EQ(vif_dma(1, S.w, 2), 1);
    CHECK_EQ(FQC(rd(1, R_STAT)), 0);
    CHECK_EQ(rd(1, R_MODE), 0);
    wr(1, R_FBRST, 8);
    CHECK_EQ(vif_dma(1, S.w + 4, 1), 1);
    CHECK_EQ(rd(1, R_MODE), 2);

    /* ERR.MII: no interrupt, no stall */
    reset_all();
    wr(1, R_ERR, 1);
    CHECK_EQ(vif_dma(1, S.w, 2), 2);
    CHECK_EQ(fake_intc, 0);
    CHECK_EQ(rd(1, R_STAT) & (ST_INT | ST_VIS), 0);

    /* i bit through FIFO writes on VIF0, with MSCAL|i */
    reset_all();
    uint32_t q[4] = { VC(0x14, 0, 0) | IBIT, VC(0x05, 0, 3), 0, 0 };
    vif_access(0x10004000, q, 16, 1);
    CHECK_EQ(fake_intc_count[INTC_VIF0], 1);
    CHECK(rd(0, R_STAT) & ST_VIS);
    CHECK_EQ(FQC(rd(0, R_STAT)), 1);
    wr(0, R_FBRST, 8);
    CHECK_EQ(rd(0, R_MODE), 3);
    CHECK_EQ(FQC(rd(0, R_STAT)), 0);

    /* i bit inside a DMA tag (TTE) */
    reset_all();
    _Alignas(16) uint32_t tag[4] = { 0x10000001, 0, VC(0, 0, 0) | IBIT, VC(0x05, 0, 1) };
    CHECK_EQ(vif_dma_tag(1, tag), 1);                  /* second word waits in the FIFO */
    CHECK_EQ(fake_intc_count[INTC_VIF1], 1);
    CHECK_EQ(rd(1, R_MODE), 0);
    _Alignas(16) uint32_t tag2[4] = { 0x10000001, 0, VC(0x05, 0, 2), 0 };
    CHECK_EQ(vif_dma_tag(1, tag2), 0);
    wr(1, R_FBRST, 8);
    CHECK_EQ(rd(1, R_MODE), 1);
    CHECK_EQ(vif_dma_tag(1, tag2), 1);
    CHECK_EQ(rd(1, R_MODE), 2);
}

static void test_stop_and_break(void) {
    reset_all();
    /* STP while idle: stops before the next command */
    wr(1, R_FBRST, 4);
    CHECK(rd(1, R_STAT) & ST_VSS);
    s_clear(&S);
    s_put(&S, VC(0x05, 0, 1)); s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    CHECK_EQ(vif_dma(1, S.w, 1), 0);
    wr(1, R_FBRST, 8);
    CHECK_EQ(vif_dma(1, S.w, 1), 1);
    CHECK_EQ(rd(1, R_MODE), 1);

    /* STP during an UNPACK: finishes the command, then stops */
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0101));
    s_put(&S, VC(0x6C, 2, 0x000));
    for (uint32_t i = 0; i < 8; i++) s_put(&S, 0x30 + i);
    s_put(&S, VC(0x05, 0, 2));
    s_align(&S);
    CHECK_EQ(vif_dma(1, S.w, 1), 1);
    wr(1, R_FBRST, 4);
    CHECK_EQ(rd(1, R_STAT) & ST_VSS, 0);
    CHECK_EQ(vif_dma(1, S.w + 4, 2), 2);
    CHECK(rd(1, R_STAT) & ST_VSS);
    CHECK_VEC(1, 1, 0x34, 0x35, 0x36, 0x37);
    CHECK_EQ(rd(1, R_MODE), 1);
    wr(1, R_FBRST, 8);
    CHECK_EQ(rd(1, R_MODE), 2);

    /* FBK: immediate stall in the middle of an UNPACK, resumes where it left off */
    s_clear(&S);
    s_put(&S, VC(0x05, 0, 0));                         /* STMOD 0 (difference mode is still on) */
    s_put(&S, VC(0x6C, 2, 0x010));
    for (uint32_t i = 0; i < 8; i++) s_put(&S, 0x40 + i);
    s_align(&S);
    CHECK_EQ(vif_dma(1, S.w, 1), 1);
    wr(1, R_FBRST, 2);
    CHECK(rd(1, R_STAT) & ST_VFS);
    CHECK_EQ(vif_dma(1, S.w + 4, 2), 0);
    wr(1, R_FBRST, 8);
    CHECK_EQ(vif_dma(1, S.w + 4, 2), 2);
    CHECK_VEC(1, 0x10, 0x40, 0x41, 0x42, 0x43);
    CHECK_VEC(1, 0x11, 0x44, 0x45, 0x46, 0x47);

    /* RST drops a half-received command */
    s_clear(&S);
    s_put(&S, VC(0x6C, 2, 0x020)); s_put(&S, 1); s_put(&S, 2); s_put(&S, 3);
    CHECK_EQ(vif_dma(1, S.w, 1), 1);
    CHECK_EQ(rd(1, R_STAT) & ST_VPS, 1);
    wr(1, R_FBRST, 1);
    CHECK_EQ(rd(1, R_STAT), 0);
    s_clear(&S);
    s_put(&S, VC(0x05, 0, 3)); s_put(&S, 0); s_put(&S, 0); s_put(&S, 0);
    CHECK_EQ(vif_dma(1, S.w, 1), 1);
    CHECK_EQ(rd(1, R_MODE), 3);
}

static void test_split_everything(void) {
    /* A long mixed stream, delivered one word at a time via FIFO, must match one-shot DMA. */
    reset_all();
    rng_state = 99;
    s_clear(&S2);
    s_put(&S2, VC(0x01, 0, 0x0302));
    s_put(&S2, VC(0x30, 0, 0)); for (int j = 0; j < 4; j++) s_put(&S2, rnd());
    s_put(&S2, VC(0x31, 0, 0)); for (int j = 0; j < 4; j++) s_put(&S2, rnd());
    s_put(&S2, VC(0x20, 0, 0)); s_put(&S2, rnd());
    s_put(&S2, VC(0x03, 0, 0x100));
    s_put(&S2, VC(0x02, 0, 0x180));
    for (int k = 0; k < 40; k++) {
        uint32_t cmd;
        do cmd = 0x60 | (rnd() & 0x1F); while ((cmd & 3) == 3 && (cmd & 0xC) != 0xC);
        uint32_t num = 1 + rnd() % 40, cl = 2, wl = 3, n_in = (num / wl) * cl + (num % wl < cl ? num % wl : cl);
        uint32_t vb = (cmd & 0xF) == 0xF ? 2 : (4u >> (cmd & 3)) * (((cmd >> 2) & 3) + 1);
        s_put(&S2, VC(cmd, num, (rnd() & 0x3FF) | (rnd() & 0xC000)));
        uint32_t words = (n_in * vb + 3) / 4;
        for (uint32_t i = 0; i < words; i++) s_put(&S2, rnd());
        if (k % 7 == 3) s_put(&S2, VC(0x14, 0, k));    /* MSCAL flips the double buffer */
        if (k % 5 == 1) { while ((S2.n & 1) == 0) s_put(&S2, 0); s_put(&S2, VC(0x4A, 2, k * 4)); for (int i = 0; i < 4; i++) s_put(&S2, rnd()); }
        if (k % 9 == 2) { while ((S2.n & 3) != 3) s_put(&S2, 0); s_put(&S2, VC(0x50, 0, 2)); for (int i = 0; i < 8; i++) s_put(&S2, rnd()); }
        if (k % 4 == 0) s_put(&S2, VC(0x05, 0, rnd() % 3));
    }
    s_align(&S2);

    static uint8_t data1[16384], micro1[16384], gif1[4096];
    uint32_t gifq1, starts1;
    CHECK_EQ(vif_dma(1, S2.w, S2.n / 4), S2.n / 4);
    memcpy(data1, fake_vu_data[1], sizeof data1);
    memcpy(micro1, fake_vu_micro[1], sizeof micro1);
    memcpy(gif1, fake_gif, sizeof gif1);
    gifq1 = fake_gif_qwc;
    starts1 = (uint32_t)fake_vu1_starts;
    uint32_t r1[4];
    for (int j = 0; j < 4; j++) r1[j] = rd(1, R_R0 + 16 * j);

    reset_all();
    for (uint32_t i = 0; i < S2.n; i++) { uint32_t w = S2.w[i]; vif_access(0x10005000, &w, 4, 1); }
    CHECK(memcmp(data1, fake_vu_data[1], sizeof data1) == 0);
    CHECK(memcmp(micro1, fake_vu_micro[1], sizeof micro1) == 0);
    CHECK(memcmp(gif1, fake_gif, sizeof gif1) == 0);
    CHECK_EQ(fake_gif_qwc, gifq1);
    CHECK_EQ((uint32_t)fake_vu1_starts, starts1);
    for (int j = 0; j < 4; j++) CHECK_EQ(rd(1, R_R0 + 16 * j), r1[j]);
    CHECK_EQ(rd(1, R_STAT) & ~(uint32_t)ST_DBF, 0);

    reset_all();
    deliver(1, &S2, 1);
    CHECK(memcmp(data1, fake_vu_data[1], sizeof data1) == 0);
    CHECK(memcmp(gif1, fake_gif, sizeof gif1) == 0);
    reset_all();
    deliver(1, &S2, 4);
    CHECK(memcmp(data1, fake_vu_data[1], sizeof data1) == 0);
    CHECK(memcmp(micro1, fake_vu_micro[1], sizeof micro1) == 0);
}

static void test_vif1_read(void) {
    reset_all();
    wr(1, R_STAT, ST_FDR);
    CHECK_EQ(rd(1, R_STAT) & ST_FDR, ST_FDR);
    fake_gs_avail = 40;
    _Alignas(16) uint8_t buf[64];
    memset(buf, 0xCC, sizeof buf);
    CHECK_EQ(vif1_read(buf, 4), 3);                    /* 40 bytes -> 2 full + 1 padded qword */
    for (int i = 0; i < 40; i++) CHECK_EQ(buf[i], (uint8_t)i);
    for (int i = 40; i < 48; i++) CHECK_EQ(buf[i], 0);
    CHECK_EQ(buf[48], 0xCC);
    CHECK_EQ(vif1_read(buf, 4), 0);                    /* nothing left: DMAC waits */
    fake_gs_avail = 16;
    uint32_t q[4] = { 0 };
    vif_access(0x10005000, q, 16, 0);                  /* EE reading the FIFO directly */
    CHECK_EQ(q[0], 0x2B2A2928u);
    wr(1, R_STAT, 0);
    CHECK_EQ(rd(1, R_STAT) & ST_FDR, 0);
}

static void bench(void) {
    /* Rough speed check: 1 MB of V4-32 + masked V3-16 unpacks. */
    reset_all();
    s_clear(&S);
    s_put(&S, VC(0x01, 0, 0x0101));
    s_put(&S, VC(0x20, 0, 0)); s_put(&S, 0xC0C0C0C0);
    for (int k = 0; k < 64; k++) {
        s_put(&S, VC(0x6C, 0, 0)); for (int i = 0; i < 1024; i++) s_put(&S, (uint32_t)i);
        s_put(&S, VC(0x79, 0, 0x100)); for (int i = 0; i < 384; i++) s_put(&S, (uint32_t)i);
    }
    s_align(&S);
    clock_t t0 = clock();
    for (int rep = 0; rep < 20; rep++) CHECK_EQ(vif_dma(1, S.w, S.n / 4), S.n / 4);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: %.1f MB of VIF data in %.3f s (%.0f MB/s)\n", 20.0 * S.n * 4 / 1e6, secs,
           secs > 0 ? 20.0 * S.n * 4 / 1e6 / secs : 0.0);
}

int main(void) {
    test_registers();
    test_unpack_formats();
    test_unpack_mask_mode_cycle();
    test_unpack_address();
    test_unpack_random();
    test_mpg();
    test_mscal_double_buffer();
    test_direct();
    test_ibit();
    test_stop_and_break();
    test_split_everything();
    test_vif1_read();
    bench();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
