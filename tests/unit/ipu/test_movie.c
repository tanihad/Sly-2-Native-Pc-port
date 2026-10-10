/* Decode the game's intro movie through the IPU, the way Sony's MPEG library (sceMpeg) drives it.
 *
 * The movie is a Sony PSS file (an MPEG-2 program stream) on the disc image at sector 959782.
 * The video elementary stream (PES stream 0xE0) is demuxed here and fed to the IPU by a fake DMA
 * channel 4; channel 3 receives the output. Like sceMpeg, the "library" below parses headers with
 * FDEC, decodes macroblock headers and motion vectors with VDEC, gets RAW16 residuals with BDEC,
 * does motion compensation itself, and converts finished pictures to RGB32 with CSC, suspending and
 * resuming the bitstream DMA around it from IPU_BP (BP, IFC, FP). I-pictures are also decoded with
 * IDEC and must match the BDEC + CSC result exactly.
 *
 * Usage: test_movie <iso> <output dir>. Writes movie_NNN.png (display order) and idec_NNN.png. */
#include "fakes.h"

#include <time.h>

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);

static uint8_t* es;
static size_t es_len;

static int demux(const char* iso, uint32_t lba) {
    FILE* f = fopen(iso, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", iso); return 0; }
    _fseeki64(f, (long long)lba * 2048, SEEK_SET);
    size_t cap = 32u << 20;
    uint8_t* d = malloc(cap);
    if (!es) es = malloc(cap + 4096);
    es_len = 0;
    size_t n = fread(d, 1, cap, f);
    fclose(f);
    size_t pos = 0;
    int packs = 0;
    while (pos + 6 <= n) {
        if (d[pos] || d[pos + 1] || d[pos + 2] != 1) { fprintf(stderr, "demux: lost sync at %zu\n", pos); break; }
        uint8_t sid = d[pos + 3];
        if (sid == 0xBA) {                                   /* pack header */
            packs++;
            pos += ((d[pos + 4] >> 6) == 1) ? 14u + (d[pos + 13] & 7u) : 12u;
            continue;
        }
        if (sid == 0xB9) break;                              /* MPEG program end */
        size_t len = (size_t)d[pos + 4] << 8 | d[pos + 5];
        if (sid == 0xE0) {                                   /* video PES: MPEG-2 header */
            size_t hdr = 9u + d[pos + 8];
            memcpy(es + es_len, d + pos + hdr, len + 6 - hdr);
            es_len += len + 6 - hdr;
        }
        pos += 6 + len;
    }
    free(d);
    memset(es + es_len, 0, 4096);                            /* padding after the end code */
    printf("  demuxed %d packs, %zu bytes of video\n", packs, es_len);
    return es_len > 0;
}

/* ---- IPU access, as a decoder library does it -------------------------------------------------- */

static int failed;                                           /* stop decoding after an error */

static void ipu_wait(void) {
    for (;;) {
        int busy = (int)(ipu_rd32(IPU_CTRL) >> 31);
        if (!busy && !fake_dma.from_qwc) return;
        uint64_t before = fake_dma.moved;
        dma_service();
        if (fake_dma.moved == before) {
            if (!failed) CHECK(0, "IPU waits forever (busy %d, CTRL %08X, BP %08X, ch4 %u, ch3 %u)", busy,
                               ipu_rd32(IPU_CTRL), ipu_rd32(IPU_BP), fake_dma.to_qwc, fake_dma.from_qwc);
            failed = 1;
            return;
        }
    }
}

static uint32_t command(uint32_t c) {
    ipu_wr32(IPU_CMD, c);
    ipu_wait();
    if ((ipu_rd32(IPU_CTRL) & 0x4000) && !failed) {
        CHECK(0, "ECD after command %08X", c);
        failed = 1;
    }
    return (uint32_t)ipu_rd64(IPU_CMD);
}

static uint32_t show32(void) { return command(0x40000000); }
static uint32_t getbits(int n) {
    uint32_t v = show32() >> (32 - n);
    command(0x40000000u | (uint32_t)n);
    return v;
}
static void next_start_code(void) {
    uint32_t bp = ipu_rd32(IPU_BP) & 7;
    if (bp) command(0x40000000u | (8 - bp));
    for (int i = 0; i < 1 << 20 && !failed && (show32() >> 8) != 1; i++) command(0x40000000 | 8);
}

/* Suspend the bitstream (DMA rewound by the qwords the IPU holds), and resume it. */
typedef struct { const uint8_t* src; uint32_t qwc, bp; } stream_ctx;
static void mark_ctx(stream_ctx* s) {                        /* where the IPU is in the stream */
    ipu_wait();
    uint32_t bp = ipu_rd32(IPU_BP), held = ((bp >> 8) & 15) + ((bp >> 16) & 3);
    s->src = fake_dma.to_src - 16 * held;
    s->qwc = fake_dma.to_qwc + held;
    s->bp = bp & 0x7F;
}
static void save_ctx(stream_ctx* s) {
    mark_ctx(s);
    fake_dma.to_qwc = 0;                                     /* stop channel 4 */
}
static void restore_ctx(const stream_ctx* s) {
    fake_dma.to_qwc = 0;
    command(s->bp);                                          /* BCLR at the saved bit position */
    fake_dma.to_src = s->src;                                /* restart channel 4, rewound */
    fake_dma.to_qwc = s->qwc;
}

/* ---- decoder state ----------------------------------------------------------------------------- */

enum { MB_INTRA = 1, MB_PATTERN = 2, MB_BACKWARD = 4, MB_FORWARD = 8, MB_QUANT = 16 };

typedef struct { uint8_t* p[3]; } frame;

static struct {
    int w, h, mbw, mbh, mpeg2;
    int pct, fcode[2][2], idp, ps, fpfd, cmv, qst, ivf, as, full_pel[2];
    frame buf[3];
    int fwd, bwd, cur;                                       /* buffer indices; -1 = none */
    int pending;                                             /* decoded reference not yet displayed */
    int displayed, decoded, idec_checked, idec_mismatch;
    int dump;                                                /* write PNGs */
    int max_pictures;
    const char* out_dir;
    const char* name;
    uint64_t hash;
    uint8_t* raw8;                                           /* CSC input, RAW8 macroblocks */
    uint8_t* rgb;                                            /* CSC output, RGB32 macroblocks */
    uint8_t* idec;                                           /* IDEC output */
    uint32_t* image;
    uint8_t intra_matrix_custom, nonintra_matrix_custom;
} D;

static const uint8_t default_intra_zigzag[64] = {
    8, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27,
    27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
    35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
};

static void ipu_init_like_sceMpeg(void) {
    ipu_wr32(IPU_CTRL, 0x40000000);                          /* reset */
    command(0);                                              /* BCLR */
    uint8_t m[64];
    for (int i = 0; i < 64; i += 16) ipu_fifo_write(default_intra_zigzag + i);
    memset(m, 16, 64);
    for (int i = 0; i < 64; i += 16) ipu_fifo_write(m + i);
    command(0x50000000);                                     /* SETIQ intra */
    command(0x58000000);                                     /* SETIQ non-intra */
    command(0x90000000);                                     /* SETTH 0, 0 */
    ipu_wr32(IPU_CTRL, 0x40000000);
    command(0);
}

/* reload the default matrices between pictures, around the suspended bitstream */
static void load_default_matrices(void) {
    stream_ctx s;
    save_ctx(&s);
    command(0);
    uint8_t m[64];
    memset(m, 16, 64);
    if (D.intra_matrix_custom) {
        for (int i = 0; i < 64; i += 16) ipu_fifo_write(default_intra_zigzag + i);
        command(0x50000000);
    }
    if (D.nonintra_matrix_custom) {
        for (int i = 0; i < 64; i += 16) ipu_fifo_write(m + i);
        command(0x58000000);
    }
    D.intra_matrix_custom = D.nonintra_matrix_custom = 0;
    restore_ctx(&s);
}

static void sequence_header(void) {
    getbits(32);
    int w = (int)getbits(12), h = (int)getbits(12);
    getbits(4); getbits(4); getbits(18); getbits(1); getbits(10); getbits(1);
    int load_intra = (int)getbits(1);
    if (load_intra) { command(0x50000000); D.intra_matrix_custom = 1; }     /* SETIQ from the stream */
    int load_nonintra = (int)getbits(1);
    if (load_nonintra) { command(0x58000000); D.nonintra_matrix_custom = 1; }
    if ((!load_intra && D.intra_matrix_custom) || (!load_nonintra && D.nonintra_matrix_custom)) {
        next_start_code();
        load_default_matrices();
    }
    if (w != D.w || h != D.h) {
        D.w = w; D.h = h;
        D.mbw = (w + 15) / 16; D.mbh = (h + 15) / 16;
        for (int i = 0; i < 3; i++) {
            free(D.buf[i].p[0]);
            D.buf[i].p[0] = calloc((size_t)D.mbw * D.mbh * 384, 1);
            D.buf[i].p[1] = D.buf[i].p[0] + D.mbw * D.mbh * 256;
            D.buf[i].p[2] = D.buf[i].p[1] + D.mbw * D.mbh * 64;
        }
        free(D.raw8); free(D.rgb); free(D.idec); free(D.image);
        D.raw8 = malloc((size_t)D.mbw * D.mbh * 384);
        D.rgb = malloc((size_t)D.mbw * D.mbh * 1024);
        D.idec = malloc((size_t)D.mbw * D.mbh * 1024);
        D.image = malloc((size_t)D.mbw * D.mbh * 256 * 4);
        D.fwd = D.bwd = D.pending = -1;
        printf("  sequence %dx%d\n", w, h);
    }
}

static void extension(void) {
    getbits(32);
    int id = (int)getbits(4);
    if (id == 1) {                                           /* sequence extension */
        D.mpeg2 = 1;
    } else if (id == 3) {                                    /* quant matrix extension */
        if (getbits(1)) { command(0x50000000); D.intra_matrix_custom = 1; }
        if (getbits(1)) { command(0x58000000); D.nonintra_matrix_custom = 1; }
    } else if (id == 8) {                                    /* picture coding extension */
        D.fcode[0][0] = (int)getbits(4); D.fcode[0][1] = (int)getbits(4);
        D.fcode[1][0] = (int)getbits(4); D.fcode[1][1] = (int)getbits(4);
        D.idp = (int)getbits(2);
        D.ps = (int)getbits(2);
        getbits(1);                                          /* top_field_first */
        D.fpfd = (int)getbits(1);
        D.cmv = (int)getbits(1);
        D.qst = (int)getbits(1);
        D.ivf = (int)getbits(1);
        D.as = (int)getbits(1);
    }
    next_start_code();
}

/* ---- motion compensation (frame pictures, frame prediction) ---------------------------------- */

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int fetch(const uint8_t* p, int w, int h, int x2, int y2) {
    int x = x2 >> 1, y = y2 >> 1, hx = x2 & 1, hy = y2 & 1;
#define P(xx, yy) p[clampi(yy, 0, h - 1) * w + clampi(xx, 0, w - 1)]
    if (!hx && !hy) return P(x, y);
    if (hx && !hy) return (P(x, y) + P(x + 1, y) + 1) >> 1;
    if (!hx) return (P(x, y) + P(x, y + 1) + 1) >> 1;
    return (P(x, y) + P(x + 1, y) + P(x, y + 1) + P(x + 1, y + 1) + 2) >> 2;
#undef P
}

/* motion of one macroblock: frame prediction uses vector [0]; field prediction uses vector [r]
 * for the destination lines of parity r, taken from reference field sel[r] (13818-2 7.6.3, 7.6.4) */
typedef struct { int type, mv[2][2][2], sel[2][2]; } motion;   /* mv[r][s][t], sel[r][s] */
enum { MT_FIELD = 1, MT_FRAME = 2 };

/* field-based fetch: (x2, y2) in half pels of field `field` of the plane */
static inline int fetch_field(const uint8_t* p, int w, int h, int field, int x2, int y2) {
    int x = x2 >> 1, y = y2 >> 1, hx = x2 & 1, hy = y2 & 1, fh = h / 2;
#define P(xx, yy) p[(2 * clampi(yy, 0, fh - 1) + field) * w + clampi(xx, 0, w - 1)]
    if (!hx && !hy) return P(x, y);
    if (hx && !hy) return (P(x, y) + P(x + 1, y) + 1) >> 1;
    if (!hx) return (P(x, y) + P(x, y + 1) + 1) >> 1;
    return (P(x, y) + P(x + 1, y) + P(x, y + 1) + P(x + 1, y + 1) + 2) >> 2;
#undef P
}

/* prediction for one macroblock from the references selected by mbt */
static void predict(int mbx, int mby, int mbt, const motion* m, uint8_t pred[384]) {
    for (int c = 0; c < 3; c++) {
        int size = c ? 8 : 16, pw = c ? D.mbw * 8 : D.mbw * 16, ph = c ? D.mbh * 8 : D.mbh * 16;
        uint8_t* dst = pred + (c ? 256 + (c - 1) * 64 : 0);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                int sum = 0, n = 0, X = mbx * size + x, Y = mby * size + y;
                for (int s = 0; s < 2; s++) {
                    if (!(mbt & (s ? MB_BACKWARD : MB_FORWARD))) continue;
                    const uint8_t* ref = D.buf[s ? D.bwd : D.fwd].p[c];
                    int r = m->type == MT_FIELD ? (Y & 1) : 0;
                    int vx = m->mv[r][s][0], vy = m->mv[r][s][1];
                    if (c) { vx /= 2; vy /= 2; }             /* 4:2:0 chroma vectors (7.6.3.7) */
                    if (m->type == MT_FIELD)
                        sum += fetch_field(ref, pw, ph, m->sel[r][s], 2 * X + vx, 2 * (Y >> 1) + vy);
                    else
                        sum += fetch(ref, pw, ph, 2 * X + vx, 2 * Y + vy);
                    n++;
                }
                dst[y * size + x] = (uint8_t)(n == 2 ? (sum + 1) >> 1 : sum);
            }
    }
}

static void store_mb(int mbx, int mby, const uint8_t* pred, const int16_t* res, int intra) {
    frame* f = &D.buf[D.cur];
    for (int c = 0; c < 3; c++) {
        int size = c ? 8 : 16, pw = c ? D.mbw * 8 : D.mbw * 16, off = c ? 256 + (c - 1) * 64 : 0;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                int v = res[off + y * size + x] + (intra ? 0 : pred[off + y * size + x]);
                f->p[c][(mby * size + y) * pw + mbx * size + x] = (uint8_t)clampi(v, 0, 255);
            }
    }
}

/* one motion vector component (7.6.3.1): VDEC motion_code, FDEC motion_residual */
static int motion_component(int s, int t, int prediction) {
    uint32_t r = command(0x38000000);                         /* VDEC motion_code */
    if (!r) { CHECK(failed, "invalid motion_code"); failed = 1; return 0; }
    int code = (int16_t)(r & 0xFFFF);
    int fcode = D.mpeg2 ? D.fcode[s][t] : D.fcode[s][0];
    int r_size = fcode - 1, f = 1 << r_size, residual = 0;
    if (r_size && code) residual = (int)getbits(r_size);
    int delta = (f == 1 || code == 0) ? code : (((abs(code) - 1) * f) + residual + 1) * (code < 0 ? -1 : 1);
    int v = prediction + delta, low = -16 * f, high = 16 * f - 1;
    if (v < low) v += 32 * f;
    if (v > high) v -= 32 * f;
    return v;
}

/* motion_vectors(s) of a frame picture; pmv[r][s][t] updated */
static void motion_vectors(int s, motion* m, int pmv[2][2][2]) {
    if (m->type == MT_FRAME) {
        for (int t = 0; t < 2; t++)
            m->mv[0][s][t] = pmv[0][s][t] = pmv[1][s][t] = motion_component(s, t, pmv[0][s][t]);
        return;
    }
    for (int r = 0; r < 2; r++) {                            /* field vectors: vertical in field units */
        m->sel[r][s] = (int)getbits(1);
        m->mv[r][s][0] = pmv[r][s][0] = motion_component(s, 0, pmv[r][s][0]);
        int v = motion_component(s, 1, pmv[r][s][1] >> 1);
        m->mv[r][s][1] = v;
        pmv[r][s][1] = v * 2;
    }
}

/* ---- slices ----------------------------------------------------------------------------------- */

static void slice(void) {
    uint32_t code = getbits(32) & 0xFF;
    int mb_row = (int)code - 1;
    int qsc = (int)getbits(5);
    while (getbits(1)) getbits(8);
    int pmv[2][2][2] = { { { 0 } } }, last_mbt = 0;
    motion m = { MT_FRAME, { { { 0 } } }, { { 0 } } };
    int dc_reset = 1, first = 1, addr = 0;
    static int16_t res[384];
    static uint8_t pred[384];
    while (!failed) {
        int inc = 0;
        for (;;) {                                           /* macroblock_address_increment */
            uint32_t r = command(0x30000000);
            int v = (int16_t)(r & 0xFFFF);
            if (!r || failed) { CHECK(failed, "invalid macroblock_address_increment"); failed = 1; return; }
            if (v == 0x22) continue;                         /* stuffing */
            if (v == 0x23) { inc += 33; continue; }          /* escape */
            inc += v;
            break;
        }
        if (first) {
            addr = mb_row * D.mbw + inc - 1;
            first = 0;
        } else {
            for (int k = 1; k < inc; k++) {                  /* skipped macroblocks */
                int a = addr + k;
                if (D.pct == 2) {                            /* P: zero vector, frame prediction */
                    memset(&m, 0, sizeof m);
                    m.type = MT_FRAME;
                    memset(pmv, 0, sizeof pmv);
                    last_mbt = MB_FORWARD;
                }                                            /* B: as the previous macroblock */
                memset(res, 0, sizeof res);
                predict(a % D.mbw, a / D.mbw, last_mbt, &m, pred);
                store_mb(a % D.mbw, a / D.mbw, pred, res, 0);
                dc_reset = 1;
            }
            addr += inc;
        }
        int mbx = addr % D.mbw, mby = addr / D.mbw;
        if (mby >= D.mbh) { CHECK(0, "macroblock address %d out of the picture", addr); failed = 1; return; }

        uint32_t r = command(0x34000000);                    /* VDEC macroblock_type */
        int mbt = (int16_t)(r & 0xFFFF);
        if (!r) {
            CHECK(0, "invalid macroblock_type at macroblock %d (picture type %d, slice %u, inc %d): next bits %08X, BP %08X",
                  addr, D.pct, code, inc, (uint32_t)ipu_rd64(IPU_TOP), ipu_rd32(IPU_BP));
            failed = 1;
            return;
        }
        m.type = MT_FRAME;
        if ((mbt & (MB_FORWARD | MB_BACKWARD)) && !D.fpfd && D.mpeg2) {
            m.type = (int)getbits(2);                        /* frame_motion_type */
            if (m.type != MT_FIELD && m.type != MT_FRAME) { CHECK(0, "dual-prime prediction is not implemented"); failed = 1; return; }
        }
        int dct_type = 0;
        if (D.mpeg2 && D.ps == 3 && !D.fpfd && (mbt & (MB_INTRA | MB_PATTERN))) dct_type = (int)getbits(1);
        if (mbt & MB_QUANT) qsc = (int)getbits(5);
        if ((mbt & MB_FORWARD) || ((mbt & MB_INTRA) && D.cmv)) motion_vectors(0, &m, pmv);
        if (mbt & MB_BACKWARD) motion_vectors(1, &m, pmv);
        if ((mbt & MB_INTRA) && D.cmv) getbits(1);
        if (mbt & MB_INTRA) {
            if (!D.cmv) memset(pmv, 0, sizeof pmv);
        } else if (D.pct == 2 && !(mbt & MB_FORWARD)) {      /* P picture, no motion: zero vector */
            memset(&m, 0, sizeof m);
            m.type = MT_FRAME;
            memset(pmv, 0, sizeof pmv);
            mbt |= MB_FORWARD;
        }
        if (mbt & (MB_INTRA | MB_PATTERN)) {                 /* BDEC: RAW16 residual via channel 3 */
            fake_dma.from_dst = (uint8_t*)res;
            fake_dma.from_qwc = 48;
            command(0x20000000u | ((uint32_t)(mbt & MB_INTRA) << 27) | ((uint32_t)dc_reset << 26) |
                    ((uint32_t)dct_type << 25) | ((uint32_t)qsc << 16));
        } else {
            memset(res, 0, sizeof res);
        }
        dc_reset = !(mbt & MB_INTRA);
        if (!(mbt & MB_INTRA)) predict(mbx, mby, mbt, &m, pred);
        store_mb(mbx, mby, pred, res, mbt & MB_INTRA);
        last_mbt = mbt & (MB_FORWARD | MB_BACKWARD);
        if ((show32() >> 9) == 0) break;                     /* end of slice */
    }
}

/* ---- output ----------------------------------------------------------------------------------- */

/* Convert a frame to RGB32 with CSC, around the suspended bitstream; result in D.rgb (MB order). */
static void frame_to_rgb(const frame* f) {
    int n = D.mbw * D.mbh;
    for (int m = 0; m < n; m++) {
        int mbx = m % D.mbw, mby = m / D.mbw;
        uint8_t* o = D.raw8 + m * 384;
        for (int y = 0; y < 16; y++) memcpy(o + y * 16, f->p[0] + (mby * 16 + y) * D.mbw * 16 + mbx * 16, 16);
        for (int c = 1; c < 3; c++)
            for (int y = 0; y < 8; y++)
                memcpy(o + 256 + (c - 1) * 64 + y * 8, f->p[c] + (mby * 8 + y) * D.mbw * 8 + mbx * 8, 8);
    }
    stream_ctx s;
    save_ctx(&s);
    command(0);                                              /* BCLR */
    for (int done = 0; done < n && !failed;) {
        int k = n - done < 1023 ? n - done : 1023;
        fake_dma.to_src = D.raw8 + done * 384;
        fake_dma.to_qwc = (uint32_t)k * 24;
        fake_dma.from_dst = D.rgb + done * 1024;
        fake_dma.from_qwc = (uint32_t)k * 64;
        command(0x70000000u | (uint32_t)k);                  /* CSC to RGB32 */
        CHECK(fake_dma.to_qwc == 0, "CSC left %u qwords of input", fake_dma.to_qwc);
        done += k;
    }
    restore_ctx(&s);
}

static void write_png(const uint8_t* rgb_mbs, const char* kind, int index) {
    for (int m = 0; m < D.mbw * D.mbh; m++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const uint8_t* p = rgb_mbs + m * 1024 + (y * 16 + x) * 4;
                D.image[((m / D.mbw) * 16 + y) * D.mbw * 16 + (m % D.mbw) * 16 + x] =
                    (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%s_%03d.png", D.out_dir, D.name, kind, index);
    debug_write_png(path, D.image, D.w, D.h, D.mbw * 16);
}

static void display(int buf) {
    int i = D.displayed++;
    if (D.dump && (i % D.dump == 0 || i < 3)) {
        frame_to_rgb(&D.buf[buf]);
        write_png(D.rgb, "frame", i);
    }
}

static void picture(void) {
    getbits(32);
    getbits(10);
    D.pct = (int)getbits(3);
    getbits(16);
    if (D.pct == 2 || D.pct == 3) { D.full_pel[0] = (int)getbits(1); D.fcode[0][0] = D.fcode[0][1] = (int)getbits(3); }
    if (D.pct == 3) { D.full_pel[1] = (int)getbits(1); D.fcode[1][0] = D.fcode[1][1] = (int)getbits(3); }
    while (getbits(1)) getbits(8);
    next_start_code();
    for (uint32_t code; !failed && ((code = show32() & 0xFF) == 0xB5 || code == 0xB2);) {
        if (code == 0xB5) extension();
        else { getbits(32); next_start_code(); }
    }
    if (D.mpeg2 && D.ps != 3) { CHECK(0, "field pictures are not implemented"); failed = 1; return; }
    if (D.pct < 1 || D.pct > 3 || (D.pct >= 2 && D.fwd < 0 && D.bwd < 0)) { CHECK(D.pct == 2 || D.pct == 3, "picture type %d", D.pct); return; }
    ipu_wr32(IPU_CTRL, ((uint32_t)D.idp << 16) | ((uint32_t)D.as << 20) | ((uint32_t)D.ivf << 21) |
                       ((uint32_t)D.qst << 22) | ((uint32_t)!D.mpeg2 << 23) | ((uint32_t)D.pct << 24));

    /* references: fwd = older, bwd = newer; B pictures use both, P pictures predict from the newer */
    int older = D.fwd, newer = D.bwd;
    int cur = 0;
    while (cur == older || cur == newer) cur++;
    D.cur = cur;
    if (D.pct == 2) D.fwd = newer;

    /* I pictures: decode with IDEC first, then rewind the stream and decode again with BDEC */
    int idec_done = 0;
    if (D.pct == 1 && (show32() & 0xFF) >= 1 && (show32() & 0xFF) <= 0xAF) {
        stream_ctx s;
        mark_ctx(&s);                                        /* at the first slice start code */
        getbits(32);
        int qsc = (int)getbits(5);
        while (getbits(1)) getbits(8);
        int dtd = D.mpeg2 && D.ps == 3 && !D.fpfd;
        fake_dma.from_dst = D.idec;
        fake_dma.from_qwc = (uint32_t)(D.mbw * D.mbh * 64);
        command(0x10000000u | ((uint32_t)dtd << 24) | ((uint32_t)qsc << 16));   /* IDEC to RGB32 */
        uint32_t ctrl = ipu_rd32(IPU_CTRL);
        CHECK(!fake_dma.from_qwc && (ctrl & 0x8000) && !(ctrl & 0x4000), "IDEC: %u qwords missing, CTRL %08X",
              fake_dma.from_qwc, ctrl);
        fake_dma.from_qwc = 0;
        restore_ctx(&s);                                     /* back to the first slice */
        idec_done = 1;
    }

    while (!failed) {
        next_start_code();
        uint32_t code = show32() & 0xFF;
        if (code < 1 || code > 0xAF) break;
        slice();
    }
    D.decoded++;
    for (int c = 0; c < 3; c++) {                            /* hash of the decoded picture */
        size_t len = (size_t)D.mbw * D.mbh * (c ? 64 : 256);
        for (size_t i = 0; i < len; i++) D.hash = (D.hash ^ D.buf[cur].p[c][i]) * 0x100000001B3ull;
    }

    if (idec_done && !failed) {                              /* IDEC == BDEC + CSC */
        frame_to_rgb(&D.buf[cur]);
        int n = D.mbw * D.mbh * 1024, diff = 0, first_mb = -1, bad_mbs = 0;
        for (int i = 0; i < n; i += 1024) {
            int d = 0;
            for (int k = 0; k < 1024; k++) d += D.rgb[i + k] != D.idec[i + k];
            if (d && first_mb < 0) first_mb = i / 1024;
            bad_mbs += d != 0;
            diff += d;
        }
        D.idec_checked++;
        if (diff) D.idec_mismatch++;
        CHECK(diff == 0, "I picture %d: IDEC and BDEC+CSC differ in %d bytes, %d macroblocks, first %d",
              D.decoded - 1, diff, bad_mbs, first_mb);
        if (diff && D.idec_mismatch == 1 && D.dump) {
            write_png(D.idec, "mismatch_idec", D.decoded - 1);
            write_png(D.rgb, "mismatch_bdec", D.decoded - 1);
        }
        if (D.dump && D.idec_checked <= 3) write_png(D.idec, "idec", D.idec_checked - 1);
    }

    if (D.pct == 3) {                                        /* B: displayed at once */
        display(cur);
    } else {                                                 /* I/P: the previous reference is displayed now */
        if (D.pending >= 0) display(D.pending);
        D.fwd = newer;
        D.bwd = cur;
        D.pending = cur;
    }
}

/* dump: write every dump-th displayed frame as PNG (0 = none) */
static int decode_movie(const char* out_dir, const char* name, int dump, int max_pictures, uint32_t jitter) {
    memset(&D, 0, sizeof D);
    D.fwd = D.bwd = D.pending = -1;
    D.out_dir = out_dir;
    D.name = name;
    D.dump = dump;
    D.max_pictures = max_pictures;
    D.hash = 0xCBF29CE484222325ull;
    failed = 0;
    memset(&fake_dma, 0, sizeof fake_dma);
    fake_dma.jitter = jitter;
    ipu_init_like_sceMpeg();
    fake_dma.to_src = es;
    fake_dma.to_qwc = (uint32_t)((es_len + 15) / 16 + 64);
    while (!failed) {
        next_start_code();
        if (failed) break;
        uint32_t code = show32() & 0xFF;
        if (code == 0xB3) sequence_header();
        else if (code == 0xB5) extension();
        else if (code == 0x00) { if (D.max_pictures && D.decoded >= D.max_pictures) break; picture(); }
        else if (code == 0xB7) break;                        /* sequence end */
        else getbits(32);
    }
    if (D.pending >= 0 && !failed) display(D.pending);
    for (int i = 0; i < 3; i++) free(D.buf[i].p[0]);
    free(D.raw8); free(D.rgb); free(D.idec); free(D.image);
    return !failed;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_movie <iso> <output dir>\n"); return 2; }

    /* movie 0, the first thing the game plays (sector 959782): all of it */
    printf("movie at sector 959782:\n");
    if (!demux(argv[1], 959782)) { printf("test_movie: no video found\n"); return 1; }
    clock_t t0 = clock();
    int ok = decode_movie(argv[2], "intro", 10, 0, 0);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    uint64_t hash = D.hash;
    int decoded = D.decoded, displayed = D.displayed;
    printf("  decoded %d pictures (%d displayed) in %.2f s with PNG output; IDEC checked on %d I pictures, %d mismatched\n",
           decoded, displayed, secs, D.idec_checked, D.idec_mismatch);
    CHECK(ok && decoded == 132, "movie decode stopped after %d pictures", decoded);

    /* the same decode with DMA transfers of 1..7 qwords must give identical pictures */
    t0 = clock();
    ok = decode_movie(argv[2], "intro", 0, 0, 7);
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("  again with 1..7-qword DMA bursts: %d pictures in %.2f s, %s\n", D.decoded, secs,
           D.hash == hash ? "identical" : "DIFFERENT");
    CHECK(ok && D.decoded == decoded && D.hash == hash, "decode differs with small DMA bursts");

    /* movie 1, which follows it on the disc: the first 300 pictures */
    printf("movie at sector 961247:\n");
    if (demux(argv[1], 961247)) {
        t0 = clock();
        ok = decode_movie(argv[2], "movie1", 25, 300, 0);
        secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  decoded %d pictures (%d displayed) in %.2f s; IDEC checked on %d I pictures, %d mismatched\n",
               D.decoded, D.displayed, secs, D.idec_checked, D.idec_mismatch);
        CHECK(ok && D.decoded == 300, "movie 1 decode stopped after %d pictures", D.decoded);
    }

    CHECK(fake_unhandled == 0, "%d unhandled accesses logged", fake_unhandled);
    printf("test_movie: %s (%d failures)\n", test_failures ? "FAILED" : "ok", test_failures);
    return test_failures != 0;
}
