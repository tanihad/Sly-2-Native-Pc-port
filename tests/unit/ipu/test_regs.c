/* IPU registers, FIFOs, simple commands (BCLR, FDEC, VDEC, SETTH, SETVQ, CSC, PACK), stalls and
 * interrupts. */
#include "fakes.h"

#include <math.h>

static void reset(void) {
    ipu_wr32(IPU_CTRL, 0x40000000);
    memset(&fake_dma, 0, sizeof fake_dma);
}

static void cmd(uint32_t c) { ipu_wr32(IPU_CMD, c); }
static int busy(void) { return (int)(ipu_rd32(IPU_CTRL) >> 31); }

/* push bytes (a multiple of 16) through the input FIFO register */
static void push_fifo(const uint8_t* p, size_t bytes) { for (size_t i = 0; i < bytes; i += 16) ipu_fifo_write(p + i); }

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void test_reset_and_fifo(void) {
    reset();
    CHECK(ipu_rd32(IPU_CTRL) == 0, "CTRL after reset = %08X", ipu_rd32(IPU_CTRL));
    CHECK(ipu_rd32(IPU_BP) == 0, "BP after reset = %08X", ipu_rd32(IPU_BP));

    /* writable CTRL fields read back; RST clears them */
    ipu_wr32(IPU_CTRL, 0x07F30000);
    CHECK(ipu_rd32(IPU_CTRL) == 0x07F30000, "CTRL fields = %08X", ipu_rd32(IPU_CTRL));
    ipu_wr32(IPU_CTRL, 0x40000000);
    CHECK(ipu_rd32(IPU_CTRL) == 0, "CTRL after second reset = %08X", ipu_rd32(IPU_CTRL));

    uint8_t data[16 * 12];
    for (int i = 0; i < (int)sizeof data; i++) data[i] = (uint8_t)(i * 7 + 3);

    /* no command since reset: IPU_CMD shows the bitstream */
    CHECK(ipu_dma_to(data, 1) == 1, "dma_to refused data");
    CHECK((uint32_t)ipu_rd64(IPU_CMD) == be32(data), "IPU_CMD before any command = %08X", (uint32_t)ipu_rd64(IPU_CMD));

    /* the input side holds 8 FIFO qwords + 2 in the decoder's buffer */
    reset();
    cmd(0x00000000);                                        /* BCLR, BP = 0 */
    CHECK(!busy(), "BCLR still busy");
    CHECK(ipu_dma_to(data, 12) == 10, "idle IPU accepted other than 10 qwords");
    uint32_t bp = ipu_rd32(IPU_BP);
    CHECK(bp == ((8u << 8) | (2u << 16)), "BP with 10 qwords held = %08X", bp);
    CHECK((ipu_rd32(IPU_CTRL) & 15) == 8, "CTRL.IFC = %u", ipu_rd32(IPU_CTRL) & 15);
    uint64_t top = ipu_rd64(IPU_TOP);
    CHECK((uint32_t)top == be32(data) && !(top >> 63), "TOP = %016llX", (unsigned long long)top);

    /* BCLR empties it and sets the start bit position */
    cmd(0x00000000 | 12);
    CHECK(ipu_rd32(IPU_BP) == 12, "BP after BCLR 12 = %08X", ipu_rd32(IPU_BP));
    push_fifo(data, 16);
    bp = ipu_rd32(IPU_BP);
    CHECK(bp == (12u | (1u << 16)), "BP after one qword = %08X", bp);
    uint32_t expect = (uint32_t)(((uint64_t)be32(data) << 12 | be32(data + 4) >> 20) & 0xFFFFFFFF);
    top = ipu_rd64(IPU_TOP);
    CHECK((uint32_t)top == expect && !(top >> 63), "TOP after BCLR 12 = %016llX, expected %08X", (unsigned long long)top, expect);

    /* fewer than 32 bits: TOP is busy and zero-filled */
    cmd(0x00000000 | 120);
    push_fifo(data, 16);
    top = ipu_rd64(IPU_TOP);
    CHECK((top >> 63) && (uint32_t)top == ((uint32_t)data[15] << 24 & 0xFF000000u),
          "TOP with 8 bits = %016llX", (unsigned long long)top);
}

/* 32 stream bits starting at absolute bit `pos` of `data` */
static uint32_t bits_at(const uint8_t* data, unsigned pos) {
    uint64_t v = 0;
    for (int k = 0; k < 5; k++) v = (v << 8) | data[pos / 8 + k];
    return (uint32_t)(v >> (8 - pos % 8));
}

static void test_fdec(void) {
    uint8_t data[80];
    for (int i = 0; i < 80; i++) data[i] = (uint8_t)(0x11 * (i & 15) ^ (i * 3));
    reset();
    cmd(0);
    push_fifo(data, 32);
    int ints = fake_intc[INTC_IPU];
    cmd(0x40000000 | 8);                                    /* FDEC, skip 8 */
    uint64_t r = ipu_rd64(IPU_CMD);
    CHECK(!(r >> 63) && (uint32_t)r == be32(data + 1), "FDEC 8 = %016llX", (unsigned long long)r);
    CHECK(fake_intc[INTC_IPU] == ints + 1, "FDEC raised %d interrupts", fake_intc[INTC_IPU] - ints);
    CHECK((ipu_rd32(IPU_BP) & 0x7F) == 8, "BP after FDEC = %u", ipu_rd32(IPU_BP) & 0x7F);
    CHECK((uint32_t)ipu_rd64(IPU_TOP) == be32(data + 1), "TOP after FDEC");

    unsigned pos = 8;
    for (int i = 0; i < 3; i++) {                            /* to bit 197 of the 256 present */
        cmd(0x40000000 | 63);
        pos += 63;
        CHECK(!busy() && (uint32_t)ipu_rd64(IPU_CMD) == bits_at(data, pos), "FDEC to %u", pos);
    }
    CHECK(ipu_rd32(IPU_BP) == ((pos - 128) | (1u << 16)), "BP at bit %u = %08X", pos, ipu_rd32(IPU_BP));

    /* FDEC needing more data than present stays busy, then completes when data arrives */
    cmd(0x40000000 | 63);                                   /* to bit 260: needs 292 bits */
    CHECK(busy(), "FDEC with too little data is not busy");
    r = ipu_rd64(IPU_CMD);
    CHECK(r >> 63, "IPU_CMD.BUSY clear while busy");
    ints = fake_intc[INTC_IPU];
    push_fifo(data + 32, 16);
    pos += 63;
    CHECK(!busy(), "FDEC still busy after data arrived");
    CHECK(fake_intc[INTC_IPU] == ints + 1, "FDEC completion interrupts: %d", fake_intc[INTC_IPU] - ints);
    CHECK((uint32_t)ipu_rd64(IPU_CMD) == bits_at(data, pos), "FDEC to %u = %08X", pos, (uint32_t)ipu_rd64(IPU_CMD));

    /* reset while busy interrupts */
    cmd(0x40000000 | 63);                                   /* to bit 323 */
    CHECK(!busy(), "FDEC to 323 busy");
    cmd(0x40000000 | 63);                                   /* to bit 386 of 384 */
    CHECK(busy(), "not busy before reset");
    ints = fake_intc[INTC_IPU];
    ipu_wr32(IPU_CTRL, 0x40000000);
    CHECK(fake_intc[INTC_IPU] == ints + 1 && !busy(), "reset during a command");
}

/* run VDEC on a bitstream holding `code` followed by ones */
static uint32_t vdec(int tbl, int pct, const char* code, int fb) {
    uint8_t buf[32];
    memset(buf, 0xFF, sizeof buf);
    bitwriter w = { buf, sizeof buf, 0 };
    for (int i = 0; i < fb; i++) bw_put(&w, i & 1, 1);
    bw_code(&w, code);
    bw_put(&w, 0xFFFFFFFF, 32);
    while (w.bits & 127) bw_put(&w, 1, 1);
    reset();
    ipu_wr32(IPU_CTRL, (uint32_t)pct << 24);
    cmd(0);
    push_fifo(buf, 32);
    cmd(0x30000000u | ((uint32_t)tbl << 26) | (uint32_t)fb);
    CHECK(!busy(), "VDEC busy");
    return (uint32_t)ipu_rd64(IPU_CMD);
}

static void test_vdec(void) {
    static const struct { int tbl, pct; const char* code; uint32_t result; } v[] = {
        { 0, 0, "1", 0x00010001 },
        { 0, 0, "0000 0011 000", 0x000B0021 },               /* increment 33 */
        { 0, 0, "0000 0001 000", 0x000B0023 },               /* macroblock_escape */
        { 0, 0, "0000 0001 111", 0x000B0022 },               /* macroblock_stuffing */
        { 0, 0, "0000 0101 10", 0x000A0011 },                /* 17 */
        { 1, 1, "1", 0x00010001 },                           /* I: intra */
        { 1, 1, "01", 0x00020011 },                          /* I: intra + quant */
        { 1, 2, "1", 0x0001000A },                           /* P: forward + pattern */
        { 1, 2, "001", 0x00030008 },                         /* P: forward */
        { 1, 2, "0000 01", 0x00060011 },                     /* P: intra + quant */
        { 1, 3, "10", 0x0002000C },                          /* B: interpolated, not coded */
        { 1, 3, "0001 0", 0x0005001E },                      /* B: interpolated, coded, quant */
        { 1, 3, "0000 10", 0x00060016 },                     /* B: backward, coded, quant */
        { 1, 4, "1", 0x00010001 },                           /* D */
        { 2, 0, "1", 0x00010000 },                           /* motion 0 */
        { 2, 0, "011", 0x0003FFFF },                         /* -1 */
        { 2, 0, "0000 0011 001", 0x000BFFF0 },               /* -16 */
        { 2, 0, "0000 0011 000", 0x000B0010 },               /* +16 */
        { 2, 0, "0000 0101 10", 0x000A0008 },                /* +8 */
        { 3, 0, "0", 0x00010000 },
        { 3, 0, "10", 0x00020001 },
        { 3, 0, "11", 0x0002FFFF },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint32_t r = vdec(v[i].tbl, v[i].pct, v[i].code, (int)(i % 5));
        CHECK(r == v[i].result, "VDEC tbl %d pct %d '%s' = %08X, expected %08X", v[i].tbl, v[i].pct, v[i].code, r, v[i].result);
        CHECK((ipu_rd32(IPU_BP) & 0x7F) == (v[i].result >> 16) + i % 5, "VDEC advanced BP to %u", ipu_rd32(IPU_BP) & 0x7F);
        CHECK(!(ipu_rd32(IPU_CTRL) & 0x4000), "VDEC set ECD");
    }
    /* an invalid code: result 0 and ECD */
    printf("  (an '[ipu] decode error' line from the invalid-code test is expected)\n");
    fflush(stdout);
    uint8_t zeros[32] = { 0 };
    reset();
    cmd(0);
    push_fifo(zeros, 32);
    cmd(0x30000000);
    CHECK(!busy() && (uint32_t)ipu_rd64(IPU_CMD) == 0 && (ipu_rd32(IPU_CTRL) & 0x4000), "VDEC of zeros");
    /* a new command clears ECD */
    cmd(0x40000000);
    CHECK(!(ipu_rd32(IPU_CTRL) & 0x4000), "ECD survived a new command");
}

static void test_vdec_split(void) {
    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    bitwriter w = { buf, sizeof buf, 0 };
    bw_put(&w, 0, 30);
    bw_code(&w, "0000 0011 001");                            /* bits 30..40 */
    bw_code(&w, "1");
    while (w.bits < 512) bw_put(&w, 1, 1);
    for (int split = 0; split < 2; split++) {
        reset();
        cmd(0 | 0);
        cmd(0x38000000 | 30);                               /* VDEC motion code after 30 bits */
        CHECK(busy(), "VDEC without data not busy");
        ipu_dma_to(buf, 1);                                  /* the whole code is in qword 0 */
        CHECK(!busy() && (uint32_t)ipu_rd64(IPU_CMD) == 0x000BFFF0, "VDEC after data: %08X", (uint32_t)ipu_rd64(IPU_CMD));
    }
    /* a code crossing a qword boundary */
    memset(buf, 0, sizeof buf);
    w.bits = 0;
    bw_put(&w, 0, 120);
    bw_code(&w, "0000 0011 001");                            /* bits 120..130 */
    while (w.bits < 512) bw_put(&w, 1, 1);
    reset();
    cmd(0);
    ipu_dma_to(buf, 1);
    cmd(0x40000000 | 60);
    cmd(0x38000000 | 60);                                   /* at bit 120 */
    CHECK(busy(), "VDEC with a partial code not busy");
    ipu_dma_to(buf + 16, 1);
    CHECK(!busy() && (uint32_t)ipu_rd64(IPU_CMD) == 0x000BFFF0, "VDEC across qwords: %08X", (uint32_t)ipu_rd64(IPU_CMD));
    CHECK(ipu_rd32(IPU_BP) == (3u | (1u << 16)), "BP after crossing = %08X", ipu_rd32(IPU_BP));
}

/* BT.601 reference conversion */
static void ref_rgb(int Y, int cb, int cr, int out[3]) {
    double y = 1.164383 * (Y - 16), u = cb - 128, v = cr - 128;
    double r = y + 1.596027 * v, g = y - 0.391762 * u - 0.812968 * v, b = y + 2.017232 * u;
    double c[3] = { r, g, b };
    for (int i = 0; i < 3; i++) { long q = lround(c[i]); out[i] = q < 0 ? 0 : q > 255 ? 255 : (int)q; }
}

static void test_csc(void) {
    enum { MBS = 3 };
    static uint8_t raw[MBS][384], rgb32[MBS][1024], rgb16[MBS][512], viafifo[1024];
    uint64_t seed = 99;
    for (int m = 0; m < MBS; m++)
        for (int i = 0; i < 384; i++) raw[m][i] = (uint8_t)(m == 0 ? (i * 5) : test_rand(&seed));
    raw[0][0] = 0; raw[0][256] = 128; raw[0][320] = 128;     /* a black pixel */

    /* RGB32 through DMA, with the conversion started before any data arrived */
    reset();
    cmd(0);
    cmd(0x70000000 | MBS);
    CHECK(busy(), "CSC without data not busy");
    fake_dma.to_src = raw[0]; fake_dma.to_qwc = MBS * 24;
    fake_dma.from_dst = rgb32[0]; fake_dma.from_qwc = MBS * 64;
    fake_dma.burst = 5;
    for (int i = 0; i < 1000 && busy(); i++) dma_service();
    dma_service();
    CHECK(!busy() && fake_dma.to_qwc == 0 && fake_dma.from_qwc == 0, "CSC did not complete (%u in, %u out left)",
          fake_dma.to_qwc, fake_dma.from_qwc);
    int bad = 0;
    for (int m = 0; m < MBS; m++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int c = (y / 2) * 8 + x / 2, ref[3];
                ref_rgb(raw[m][y * 16 + x], raw[m][256 + c], raw[m][320 + c], ref);
                const uint8_t* p = &rgb32[m][4 * (y * 16 + x)];
                for (int k = 0; k < 3; k++) if (abs(p[k] - ref[k]) > 1) bad++;
                if (p[3] != 0x80) bad++;
            }
    CHECK(bad == 0, "CSC RGB32: %d components off by more than 1 from BT.601", bad);

    /* alpha thresholds: SETTH(TH0 = 1, TH1 = 0x100): the black pixel is transparent, others 0x40 */
    reset();
    cmd(0x90000000 | 1 | (0x100 << 16));
    cmd(0);
    cmd(0x70000001);
    push_fifo(raw[0], 384);
    for (int q = 0; q < 64; q++) ipu_fifo_read(viafifo + 16 * q);
    CHECK(!busy(), "CSC via FIFO registers busy");
    CHECK(viafifo[3] == 0 && viafifo[0] == 0 && viafifo[1] == 0 && viafifo[2] == 0, "transparent pixel %02X%02X%02X%02X",
          viafifo[0], viafifo[1], viafifo[2], viafifo[3]);
    int not40 = 0;
    for (int i = 1; i < 256; i++) {
        const uint8_t* p = viafifo + 4 * i;
        int expect = (p[0] < 1 && p[1] < 1 && p[2] < 1) ? 0 : 0x40;
        if (p[3] != expect) not40++;
        if (memcmp(p, &rgb32[0][4 * i], 3)) not40++;
    }
    CHECK(not40 == 0, "alpha/colour mismatches with thresholds: %d", not40);
    cmd(0x90000000);                                         /* thresholds back to 0 */

    /* RGB16 with dither: R/G/B = (component + dither) >> 3 of the RGB32 result */
    static const int dither[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
    for (int dte = 0; dte < 2; dte++) {
        reset();
        cmd(0);
        cmd(0x78000000u | ((uint32_t)dte << 26) | MBS);
        fake_dma.to_src = raw[0]; fake_dma.to_qwc = MBS * 24;
        fake_dma.from_dst = rgb16[0]; fake_dma.from_qwc = MBS * 32;
        for (int i = 0; i < 1000 && (busy() || fake_dma.from_qwc); i++) dma_service();
        bad = 0;
        for (int m = 0; m < MBS; m++)
            for (int i = 0; i < 256; i++) {
                int x = i & 15, y = i >> 4, d = dte ? dither[y & 3][x & 3] : 0;
                uint16_t got = (uint16_t)(rgb16[m][2 * i] | rgb16[m][2 * i + 1] << 8), want = 0;
                for (int k = 0; k < 3; k++) {
                    int c = rgb32[m][4 * i + k] + d;
                    c = c < 0 ? 0 : c > 255 ? 255 : c;
                    want |= (uint16_t)((c >> 3) << (5 * k));
                }
                if (got != want) bad++;
            }
        CHECK(bad == 0, "CSC RGB16 (dither %d): %d pixels differ", dte, bad);
    }
}

static void test_pack(void) {
    /* SETVQ with the default table sceMpeg loads (a grey ramp and the primaries) */
    static const uint8_t clut[32] = {
        0x00, 0x00, 0x21, 0x04, 0x42, 0x08, 0xe0, 0x03, 0x84, 0x10, 0xa5, 0x14, 0xc6, 0x18, 0xe7, 0x1c,
        0x1f, 0x00, 0x29, 0x25, 0x4a, 0x29, 0x00, 0x7c, 0x8c, 0x31, 0xad, 0x35, 0xff, 0x7f, 0xce, 0x39,
    };
    static uint8_t rgb32[1024], out[512];
    for (int i = 0; i < 256; i++) {
        uint8_t* p = rgb32 + 4 * i;
        switch (i & 3) {
        case 0: p[0] = 255; p[1] = 0; p[2] = 0; break;       /* red -> entry 8 */
        case 1: p[0] = 0; p[1] = 255; p[2] = 0; break;       /* green -> entry 3 */
        case 2: p[0] = 0; p[1] = 0; p[2] = 250; break;       /* blue -> entry 11 */
        default: p[0] = p[1] = p[2] = 255; break;            /* white -> entry 14 */
        }
        p[3] = 0x80;
    }
    reset();
    cmd(0);
    push_fifo(clut, 32);
    cmd(0x60000000);                                         /* SETVQ */
    CHECK(!busy(), "SETVQ busy");
    cmd(0x80000001);                                         /* PACK 1 MB to INDX4 */
    fake_dma.to_src = rgb32; fake_dma.to_qwc = 64;
    fake_dma.from_dst = out; fake_dma.from_qwc = 8;
    for (int i = 0; i < 100 && (busy() || fake_dma.from_qwc); i++) dma_service();
    int bad = 0;
    for (int i = 0; i < 128; i++) if (out[i] != (i & 1 ? 0xEB : 0x38)) bad++;
    CHECK(!busy() && bad == 0, "PACK INDX4: %d bytes wrong (first %02X %02X)", bad, out[0], out[1]);

    cmd(0x88000001);                                         /* PACK to RGB16 */
    fake_dma.to_src = rgb32; fake_dma.to_qwc = 64;
    fake_dma.from_dst = out; fake_dma.from_qwc = 32;
    for (int i = 0; i < 100 && (busy() || fake_dma.from_qwc); i++) dma_service();
    uint16_t px0 = (uint16_t)(out[0] | out[1] << 8), px2 = (uint16_t)(out[4] | out[5] << 8);
    CHECK(!busy() && px0 == 0x001F && px2 == (31u << 10), "PACK RGB16 = %04X %04X", px0, px2);
}

static void test_output_fifo(void) {
    /* OFC reports at most 8; reads drain in order */
    static uint8_t raw[384], a[1024], b[1024];
    for (int i = 0; i < 384; i++) raw[i] = (uint8_t)(i * 3);
    reset();
    cmd(0);
    cmd(0x70000001);
    push_fifo(raw, 384);
    CHECK(!busy(), "CSC busy");
    CHECK(((ipu_rd32(IPU_CTRL) >> 4) & 15) == 8, "OFC = %u", (ipu_rd32(IPU_CTRL) >> 4) & 15);
    for (int q = 0; q < 60; q++) ipu_fifo_read(a + 16 * q);
    CHECK(((ipu_rd32(IPU_CTRL) >> 4) & 15) == 4, "OFC after 60 reads = %u", (ipu_rd32(IPU_CTRL) >> 4) & 15);
    CHECK(ipu_dma_from(a + 960, 10) == 4, "dma_from returned more than was left");
    ipu_csc_mb(raw, b, 0, 0, 0, 0, 0);
    CHECK(memcmp(a, b, 1024) == 0, "FIFO output differs from the conversion");
}

int main(void) {
    test_reset_and_fifo();
    test_fdec();
    test_vdec();
    test_vdec_split();
    test_csc();
    test_pack();
    test_output_fifo();
    CHECK(fake_unhandled == 0, "%d unhandled accesses logged", fake_unhandled);
    printf("test_regs: %s (%d failures)\n", test_failures ? "FAILED" : "ok", test_failures);
    return test_failures != 0;
}
