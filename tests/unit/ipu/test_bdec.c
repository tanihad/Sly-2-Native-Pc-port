/* BDEC and IDEC on synthetic bitstreams: random macroblocks are encoded with the Annex B code
 * lists, decoded by the IPU (fed in random-sized DMA bursts so that commands stall and resume at
 * arbitrary bit positions), and compared with a reference reconstruction written straight from the
 * dequantisation rules of ISO/IEC 13818-2 7.4 and ISO/IEC 11172-2 2.4.4. */
#include "fakes.h"

static uint64_t seed = 0xC0FFEE;
static int rnd(int n) { return (int)(test_rand(&seed) % (uint32_t)n); }

static const uint8_t nonlinear[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32,
                                       36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112 };

typedef struct {
    int mp1, as, ivf, qst, idp;          /* IPU_CTRL fields */
    uint8_t w[2][64];                    /* intra / non-intra matrices, raster order */
} config;

typedef struct {
    int intra, dt, dcr, qsc, cbp;
    int dc[6];                           /* intra: QF[0], i.e. predictor + differential */
    int n[6];                            /* run/level pairs per block (AC only for intra) */
    int run[6][64], level[6][64];
} mb;

static const char* find_code(int table, int val) {
    for (int i = 0; i < ipu_vlc_code_count[table]; i++)
        if (ipu_vlc_codes[table][i].val == val) return ipu_vlc_codes[table][i].code;
    return NULL;
}

/* ---- encoder ------------------------------------------------------------------------------- */

static void put_coef(bitwriter* w, const config* c, int table, int first, int run, int level, int force_escape) {
    int a = abs(level);
    if (first && table == 0 && run == 0 && a == 1 && !force_escape) {   /* "1s" */
        bw_put(w, 1, 1);
        bw_put(w, level < 0, 1);
        return;
    }
    const char* code = (a <= 40 && !force_escape) ? find_code(IPU_VLC_DCT0 + table, run | (a << 8)) : NULL;
    if (code) { bw_code(w, code); bw_put(w, level < 0, 1); return; }
    bw_code(w, "000001");
    bw_put(w, (uint32_t)run, 6);
    if (!c->mp1) bw_put(w, (uint32_t)level & 0xFFF, 12);
    else if (level >= -127 && level <= 127) bw_put(w, (uint32_t)level & 0xFF, 8);
    else if (level > 0) { bw_put(w, 0, 8); bw_put(w, (uint32_t)level, 8); }
    else { bw_put(w, 0x80, 8); bw_put(w, (uint32_t)(level + 256), 8); }
}

static void put_dc(bitwriter* w, int chroma, int diff) {
    int size = 0;
    for (int t = abs(diff); t; t >>= 1) size++;
    bw_code(w, find_code(chroma ? IPU_VLC_DC_CHROMA : IPU_VLC_DC_LUMA, size));
    if (size) bw_put(w, (uint32_t)(diff > 0 ? diff : diff + (1 << size) - 1), size);
}

/* The blocks of a macroblock; DC differentials against `pred` (the decoder's predictors), updated. */
static void put_blocks(bitwriter* w, const config* c, const mb* m, int pred[3]) {
    for (int b = 0; b < 6; b++) {
        if (!(m->cbp & (32 >> b))) continue;
        if (m->intra) {
            int comp = b < 4 ? 0 : b - 3;
            put_dc(w, b >= 4, m->dc[b] - pred[comp]);
            pred[comp] = m->dc[b];
        }
        int table = m->intra ? c->ivf : 0;
        for (int k = 0; k < m->n[b]; k++)
            put_coef(w, c, table, !m->intra && k == 0, m->run[b][k], m->level[b][k], rnd(16) == 0);
        bw_code(w, table ? "0110" : "10");                   /* end of block */
    }
}

/* Random macroblock content. DC values stay near `near` (updated) like real pictures, with jumps. */
static void random_mb(const config* c, mb* m, int intra, int near[3]) {
    memset(m, 0, sizeof *m);
    m->intra = intra;
    m->dt = rnd(2);
    m->qsc = 1 + rnd(31);
    m->cbp = intra ? 63 : (c->mp1 ? 1 + rnd(63) : rnd(64));
    int maxlevel = c->mp1 ? 255 : 2047;
    for (int b = 0; b < 6; b++) {
        if (!(m->cbp & (32 >> b))) continue;
        if (intra) {
            int comp = b < 4 ? 0 : b - 3, range = 1 << (8 + c->idp);
            int v = rnd(4) ? near[comp] + rnd(41) - 20 : rnd(range);
            v = v < 0 ? 0 : v >= range ? range - 1 : v;
            m->dc[b] = near[comp] = v;
        }
        int i = intra, n = 0, want = rnd(5) == 0 ? rnd(64) : rnd(8);
        if (!intra && want == 0) want = 1;                   /* a coded non-intra block has a coefficient */
        while (n < want && i < 64) {
            int run = rnd(3) ? 0 : rnd(5) == 0 ? rnd(64 - i) : rnd(4);
            if (i + run > 63) run = 63 - i;
            int level = rnd(10) ? 1 + rnd(3) : rnd(4) ? 1 + rnd(40) : 1 + rnd(maxlevel);
            m->run[b][n] = run;
            m->level[b][n] = rnd(2) ? -level : level;
            n++;
            i += run + 1;
        }
        m->n[b] = n;
    }
}

/* ---- reference decoder ----------------------------------------------------------------------- */

static int sgn(int v) { return (v > 0) - (v < 0); }
static int sat(int v) { return v < -2048 ? -2048 : v > 2047 ? 2047 : v; }

/* RAW16 macroblock (Y 16x16, Cb 8x8, Cr 8x8) for quantiser_scale_code qsc */
static void reference_mb(const config* c, const mb* m, int qsc, int16_t out[384]) {
    memset(out, 0, 384 * sizeof out[0]);
    int qs = (c->qst && !c->mp1) ? nonlinear[qsc] : 2 * qsc;
    for (int b = 0; b < 6; b++) {
        if (!(m->cbp & (32 >> b))) continue;
        int QF[64] = { 0 }, F[64] = { 0 };
        int i = m->intra;
        for (int k = 0; k < m->n[b]; k++) {
            i += m->run[b][k];
            QF[ipu_scan[c->as][i]] = m->level[b][k];
            i++;
        }
        const uint8_t* W = c->w[!m->intra];
        int sum = 0;
        for (int p = 0; p < 64; p++) {
            int v;
            if (m->intra && p == 0) {
                static const int intra_dc_mult[4] = { 8, 4, 2, 1 };
                v = m->dc[b] * (c->mp1 ? 8 : intra_dc_mult[c->idp]);
            } else if (!QF[p]) {
                v = 0;
            } else if (!c->mp1) {                            /* 13818-2 7.4.2.3 */
                int k = m->intra ? 0 : sgn(QF[p]);
                v = ((2 * QF[p] + k) * W[p] * qs) / 32;
            } else {                                         /* 11172-2 2.4.4: quantizer_scale = qsc */
                if (m->intra) v = (2 * QF[p] * qsc * W[p]) / 16;
                else v = ((2 * QF[p] + sgn(QF[p])) * qsc * W[p]) / 16;
                if ((v & 1) == 0) v -= sgn(v);
            }
            F[p] = sat(v);
            sum += F[p];
        }
        if (!c->mp1 && (sum & 1) == 0) F[63] += (F[63] & 1) ? -1 : 1;   /* 7.4.4 mismatch control */
        int16_t coef[64], blk[64];
        for (int p = 0; p < 64; p++) coef[p] = (int16_t)F[p];
        ipu_idct(coef, blk, 8);
        for (int r = 0; r < 8; r++)
            for (int x = 0; x < 8; x++) {
                int16_t v = blk[r * 8 + x];
                if (b >= 4) { out[256 + (b - 4) * 64 + r * 8 + x] = v; continue; }
                int row = m->dt ? 2 * r + (b >> 1) : (b >> 1) * 8 + r;   /* field DCT interleaves */
                out[row * 16 + (b & 1) * 8 + x] = v;
            }
    }
}

/* ---- driving the IPU ------------------------------------------------------------------------ */

static void cmd(uint32_t c) { ipu_wr32(IPU_CMD, c); }
static int busy(void) { return (int)(ipu_rd32(IPU_CTRL) >> 31); }

/* Service the fake DMAC (random burst sizes) until the command and the output transfer finish. */
static void wait_done(const char* what) {
    for (int i = 0; i < 10000000; i++) {
        if (!busy() && !fake_dma.from_qwc) return;
        uint64_t before = fake_dma.moved;
        fake_dma.burst = 1 + (uint32_t)rnd(12);
        dma_service();
        if (fake_dma.moved == before && busy()) { CHECK(0, "%s: IPU stuck (BP %08X)", what, ipu_rd32(IPU_BP)); return; }
        if (fake_dma.moved == before && !busy()) { CHECK(0, "%s: output short by %u qwords", what, fake_dma.from_qwc); return; }
    }
}

static void load_matrices(config* c, int random) {
    for (int q = 0; q < 2; q++) {
        for (int p = 0; p < 64; p++) c->w[q][p] = (uint8_t)(random ? 1 + rnd(255) : q ? 16 : 8 + p / 2);
        uint8_t zz[64];
        for (int i = 0; i < 64; i++) zz[i] = c->w[q][ipu_scan[0][i]];   /* SETIQ takes zigzag order */
        cmd(0);
        for (int i = 0; i < 64; i += 16) ipu_fifo_write(zz + i);
        cmd(0x50000000u | ((uint32_t)q << 27));
        CHECK(!busy(), "SETIQ busy");
    }
}

static void random_config(config* c) {
    c->mp1 = rnd(4) == 0;
    c->as = c->mp1 ? 0 : rnd(2);
    c->ivf = c->mp1 ? 0 : rnd(2);
    c->qst = c->mp1 ? 0 : rnd(2);
    c->idp = c->mp1 ? 0 : rnd(4);
    load_matrices(c, rnd(3) != 0);
    ipu_wr32(IPU_CTRL, ((uint32_t)c->idp << 16) | ((uint32_t)c->as << 20) | ((uint32_t)c->ivf << 21) |
                       ((uint32_t)c->qst << 22) | ((uint32_t)c->mp1 << 23));
}

static uint32_t stream_bits_at(const uint8_t* s, size_t pos) {
    uint64_t v = 0;
    for (int k = 0; k < 5; k++) v = (v << 8) | s[pos / 8 + k];
    return (uint32_t)(v >> (8 - pos % 8));
}

static uint8_t stream[1 << 20];

static void test_bdec(void) {
    int bad_mbs = 0, total = 0;
    for (int group = 0; group < 500; group++) {
        config c;
        random_config(&c);
        int bp0 = rnd(128), junk = rnd(64);
        bitwriter w = { stream, sizeof stream, 0 };
        for (int k = 0; k < bp0 + junk; k++) bw_put(&w, (uint32_t)rnd(2), 1);   /* skipped by BCLR's BP and FB */
        enum { NMB = 8 };
        static mb mbs[NMB];
        int reset_value = 1 << (7 + c.idp), near[3] = { reset_value, reset_value, reset_value };
        int pred[3] = { reset_value, reset_value, reset_value };
        int nmb = 1 + rnd(NMB);
        for (int k = 0; k < nmb; k++) {
            mb* m = &mbs[k];
            random_mb(&c, m, rnd(10) < 7, near);
            m->dcr = k == 0 || rnd(3) == 0;
            if (m->dcr) pred[0] = pred[1] = pred[2] = reset_value;
            if (!m->intra) bw_code(&w, find_code(IPU_VLC_CBP, m->cbp));
            put_blocks(&w, &c, m, pred);
        }
        size_t end = w.bits, bytes = (end + 7) / 8;
        for (int k = 0; k < 64; k++) stream[bytes + k] = (uint8_t)rnd(256);   /* data after the last block */

        cmd((uint32_t)bp0);                                  /* BCLR: decoding starts at bit bp0 */
        fake_dma.to_src = stream;
        fake_dma.to_qwc = (uint32_t)((bytes + 15) / 16) + 3;
        static int16_t got[384];
        for (int k = 0; k < nmb; k++) {
            const mb* m = &mbs[k];
            fake_dma.from_dst = (uint8_t*)got;
            fake_dma.from_qwc = 48;
            cmd(0x20000000u | ((uint32_t)m->intra << 27) | ((uint32_t)m->dcr << 26) | ((uint32_t)m->dt << 25) |
                ((uint32_t)m->qsc << 16) | (uint32_t)(k == 0 ? junk : 0));
            wait_done("BDEC");
            int16_t want[384];
            reference_mb(&c, m, m->qsc, want);
            uint32_t ctrl = ipu_rd32(IPU_CTRL);
            total++;
            if (memcmp(got, want, sizeof want) || (ctrl & 0x4000) || ((ctrl >> 8) & 63) != (uint32_t)m->cbp) {
                if (bad_mbs++ < 5) {
                    int first = 0;
                    while (first < 383 && got[first] == want[first]) first++;
                    CHECK(0, "group %d mb %d (intra %d mp1 %d as %d ivf %d qst %d idp %d dt %d qsc %d cbp %02X): "
                          "first difference at %d (%d vs %d), CTRL %08X", group, k, m->intra, c.mp1, c.as, c.ivf,
                          c.qst, c.idp, m->dt, m->qsc, m->cbp, first, got[first], want[first], ctrl);
                }
            }
        }
        /* the bit position after the last macroblock */
        cmd(0x40000000);
        wait_done("FDEC");
        CHECK((uint32_t)ipu_rd64(IPU_CMD) == stream_bits_at(stream, end), "group %d: position after BDEC: %08X vs %08X",
              group, (uint32_t)ipu_rd64(IPU_CMD), stream_bits_at(stream, end));
        fake_dma.to_qwc = 0;
    }
    printf("  BDEC: %d macroblocks, %d wrong\n", total, bad_mbs);
    CHECK(bad_mbs == 0, "%d of %d BDEC macroblocks wrong", bad_mbs, total);
}

/* IDEC: pictures of several slices of intra macroblocks, decoded and converted to RGB. */
static void test_idec(void) {
    int bad = 0, total = 0;
    enum { MAXMB = 48 };
    static mb mbs[MAXMB];
    static int qsc_of[MAXMB];
    static uint8_t got[MAXMB * 1024], want[MAXMB * 1024];
    for (int pic = 0; pic < 150; pic++) {
        config c;
        random_config(&c);
        int dtd = c.mp1 ? 0 : rnd(2), ofm = rnd(2), dte = rnd(2), sgn_ = rnd(4) == 0;
        int th0 = rnd(3) ? 0 : rnd(40), th1 = rnd(3) ? 0 : rnd(300);
        cmd(0x90000000u | (uint32_t)th0 | ((uint32_t)th1 << 16));   /* SETTH */

        bitwriter w = { stream, sizeof stream, 0 };
        int junk = rnd(64);
        for (int k = 0; k < junk; k++) bw_put(&w, (uint32_t)rnd(2), 1);
        int reset_value = 1 << (7 + c.idp), near[3] = { reset_value, reset_value, reset_value };
        int nslices = 1 + rnd(4), n = 0, qsc = 1 + rnd(31), first_qsc = qsc;
        for (int s = 0; s < nslices && n < MAXMB; s++) {
            if (s > 0) {                                     /* end of slice, next slice header */
                while (w.bits & 7) bw_put(&w, 0, 1);
                for (int z = rnd(3); z > 0; z--) bw_put(&w, 0, 8);
                bw_put(&w, 0x000001, 24);
                bw_put(&w, (uint32_t)(1 + s + rnd(100)), 8);   /* slice_start_code 0x01..0xAF */
                qsc = 1 + rnd(31);
                bw_put(&w, (uint32_t)qsc, 5);
                for (int e = rnd(3); e > 0; e--) { bw_put(&w, 1, 1); bw_put(&w, (uint32_t)rnd(256), 8); }
                bw_put(&w, 0, 1);
            }
            int pred[3] = { reset_value, reset_value, reset_value };
            int count = 1 + rnd(MAXMB / 4);
            for (int k = 0; k < count && n < MAXMB; k++, n++) {
                mb* m = &mbs[n];
                random_mb(&c, m, 1, near);
                if (c.mp1 && rnd(4) == 0) bw_code(&w, "0000 0001 111");          /* stuffing */
                if (k == 0 && rnd(3) == 0) bw_code(&w, "0000 0001 000");         /* escape */
                bw_code(&w, k == 0 ? find_code(IPU_VLC_MBAI, 1 + rnd(33)) : "1");
                int quant = rnd(3) == 0;
                bw_code(&w, quant ? "01" : "1");                                 /* intra (+ quant) */
                m->dt = dtd ? rnd(2) : 0;
                if (dtd) bw_put(&w, (uint32_t)m->dt, 1);                         /* dct_type */
                if (quant) {                                                     /* quantiser_scale_code */
                    qsc = 1 + rnd(31);
                    bw_put(&w, (uint32_t)qsc, 5);
                }
                put_blocks(&w, &c, m, pred);
                qsc_of[n] = qsc;
            }
        }
        while (w.bits & 7) bw_put(&w, 0, 1);                 /* then a picture start code */
        size_t end = w.bits;
        bw_put(&w, 0x00000100, 32);
        for (int k = 0; k < 8; k++) bw_put(&w, (uint32_t)rnd(256), 8);

        /* expected RGB */
        int oqw = ofm ? 32 : 64;
        for (int k = 0; k < n; k++) {
            int16_t raw16[384];
            uint8_t raw8[384];
            reference_mb(&c, &mbs[k], qsc_of[k], raw16);
            for (int i = 0; i < 384; i++) raw8[i] = (uint8_t)(raw16[i] < 0 ? 0 : raw16[i] > 255 ? 255 : raw16[i]);
            ipu_csc_mb(raw8, want + k * oqw * 16, ofm, dte, sgn_, th0, th1);
        }

        cmd(0);                                              /* BCLR */
        fake_dma.to_src = stream;
        fake_dma.to_qwc = (uint32_t)((w.bits + 7) / 8 + 15) / 16;
        fake_dma.from_dst = got;
        fake_dma.from_qwc = (uint32_t)(n * oqw);
        cmd(0x10000000u | ((uint32_t)ofm << 27) | ((uint32_t)dte << 26) | ((uint32_t)sgn_ << 25) |
            ((uint32_t)dtd << 24) | ((uint32_t)first_qsc << 16) | (uint32_t)junk);
        wait_done("IDEC");
        uint32_t ctrl = ipu_rd32(IPU_CTRL);
        total++;
        int ok = !memcmp(got, want, (size_t)n * oqw * 16) && !(ctrl & 0x4000) && (ctrl & 0x8000) && !fake_dma.from_qwc;
        uint32_t extra = ipu_dma_from(got, 1);               /* nothing more should come out */
        cmd(0x40000000);
        wait_done("FDEC");
        uint32_t next = (uint32_t)ipu_rd64(IPU_CMD);
        if (!ok || extra || next != 0x00000100 || (ipu_rd32(IPU_BP) & 7) != (end & 7)) {
            if (bad++ < 5) {
                int first = 0;
                while (first < n * oqw * 16 && got[first] == want[first]) first++;
                CHECK(0, "IDEC picture %d (%d MBs, mp1 %d as %d ivf %d qst %d idp %d dtd %d ofm %d dte %d sgn %d): "
                      "first byte difference at %d, CTRL %08X, %u qwords missing, %u extra, next bits %08X",
                      pic, n, c.mp1, c.as, c.ivf, c.qst, c.idp, dtd, ofm, dte, sgn_, first, ctrl, fake_dma.from_qwc,
                      extra, next);
            }
        }
        fake_dma.to_qwc = 0;
        fake_dma.from_qwc = 0;
    }
    printf("  IDEC: %d pictures, %d wrong\n", total, bad);
    CHECK(bad == 0, "%d of %d IDEC pictures wrong", bad, total);
}

int main(void) {
    ipu_wr32(IPU_CTRL, 0x40000000);
    test_bdec();
    test_idec();
    printf("test_bdec: %s (%d failures)\n", test_failures ? "FAILED" : "ok", test_failures);
    return test_failures != 0;
}
