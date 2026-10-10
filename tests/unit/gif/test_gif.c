/* Unit tests for runtime/gif.c. Links gif.c with fakes of the GS entry points it calls. */
#include "hw.h"

#include <stdlib.h>

/* ---- fakes ------------------------------------------------------------------------------------- */

typedef struct { int image; uint32_t reg; uint64_t val; uint32_t off, len; } event;   /* image: bytes at img[off..off+len) */

static event   evs[8192];
static int     nev;
static uint8_t img[1 << 20];
static uint32_t img_len;
static int     unhandled;

void gs_write_reg(uint32_t reg, uint64_t value) {
    if (nev < (int)(sizeof evs / sizeof evs[0])) evs[nev++] = (event){ 0, reg, value, 0, 0 };
}
void gs_image_write(const void* data, uint32_t bytes) {
    if (img_len + bytes > sizeof img) abort();
    memcpy(img + img_len, data, bytes);
    if (nev && evs[nev - 1].image) evs[nev - 1].len += bytes;    /* merge: split points don't matter */
    else evs[nev++] = (event){ 1, 0, 0, img_len, bytes };
    img_len += bytes;
}
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
    unhandled++;
}

/* ---- helpers ----------------------------------------------------------------------------------- */

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); checks++; \
    if (_a != _b) { failures++; fprintf(stderr, "FAIL %s:%d: %s == 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static void clear(void) { nev = 0; img_len = 0; }

typedef struct { uint8_t b[16]; } qword;

static qword qw(uint64_t lo, uint64_t hi) { qword q; memcpy(q.b, &lo, 8); memcpy(q.b + 8, &hi, 8); return q; }

static qword tag(uint32_t nloop, int eop, int pre, uint32_t prim, int flg, uint32_t nreg, uint64_t regs) {
    uint64_t lo = (uint64_t)(nloop & 0x7FFF) | ((uint64_t)eop << 15) | ((uint64_t)pre << 46) |
                  ((uint64_t)(prim & 0x7FF) << 47) | ((uint64_t)flg << 58) | ((uint64_t)(nreg & 15) << 60);
    return qw(lo, regs);
}

static void expect_reg(int i, uint32_t reg, uint64_t val) {
    checks++;
    if (i >= nev || evs[i].image || evs[i].reg != reg || evs[i].val != val) {
        failures++;
        if (i >= nev) fprintf(stderr, "FAIL event %d: missing (wanted reg 0x%02X = 0x%016llX)\n", i, reg, (unsigned long long)val);
        else fprintf(stderr, "FAIL event %d: got %s 0x%02X = 0x%016llX, wanted reg 0x%02X = 0x%016llX\n", i,
                     evs[i].image ? "image" : "reg", evs[i].reg, (unsigned long long)evs[i].val, reg, (unsigned long long)val);
    }
}

static uint32_t reg32(uint32_t phys) { uint32_t v = 0xCCCCCCCCu; gif_access(phys, &v, 4, 0); return v; }
static void wreg32(uint32_t phys, uint32_t v) { gif_access(phys, &v, 4, 1); }
static uint32_t gif_stat_rd(void) { return reg32(0x10003020); }

#define STAT_M3R (1u << 0)
#define STAT_M3P (1u << 1)
#define STAT_IMT (1u << 2)
#define STAT_PSE (1u << 3)
#define STAT_P3Q (1u << 6)
#define STAT_OPH (1u << 9)
#define STAT_APATH(s) (((s) >> 10) & 3)
#define STAT_FQC(s) (((s) >> 24) & 31)

static void reset_gif(void) { wreg32(0x10003000, 1); wreg32(0x10003010, 0); gif_set_path3_mask(0); clear(); }

/* ---- packed conversions ------------------------------------------------------------------------ */

static void test_packed_all_descriptors(void) {
    reset_gif();
    qword s[17];
    s[0] = tag(1, 1, 0, 0, 0, 0 /* = 16 */, 0xFEDCBA9876543210ull);
    s[1]  = qw(0xFFFFFFFFFFFFFDA5ull, 0xFFFFFFFFFFFFFFFFull);           /* 0 PRIM: bits 0-10 */
    s[2]  = qw(0xDEADBE56CAFEBA12ull, 0xF00DF09A0BADC078ull);           /* 1 RGBAQ: R 12 G 56 B 78 A 9A */
    s[3]  = qw(0x4040000040000000ull, 0xFFFFFFFF40800000ull);           /* 2 ST: S 2.0 T 3.0 Q 4.0 */
    s[4]  = qw(0xFFFFC123FFFFC456ull, 0xFFFFFFFFFFFFFFFFull);           /* 3 UV: U 0456 V 0123 */
    s[5]  = qw(0xFFFF5678FFFF1234ull, 0xFFFF79CFFABCDEFFull);           /* 4 XYZF2: Z ABCDEF F 9C ADC 0 */
    s[6]  = qw(0xFFFF5678FFFF1234ull, 0xFFFF7FFF89ABCDEFull);           /* 5 XYZ2: Z 89ABCDEF ADC 0 */
    s[7]  = qw(0x1111111111111111ull, 0xFFFFFFFFFFFFFFFFull);           /* 6 TEX0_1 */
    s[8]  = qw(0x2222222222222222ull, 0xFFFFFFFFFFFFFFFFull);           /* 7 TEX0_2 */
    s[9]  = qw(0x3333333333333333ull, 0xFFFFFFFFFFFFFFFFull);           /* 8 CLAMP_1 */
    s[10] = qw(0x4444444444444444ull, 0xFFFFFFFFFFFFFFFFull);           /* 9 CLAMP_2 */
    s[11] = qw(0xFFFFFFFFFFFFFFFFull, 0xFFFFF5AFFFFFFFFFull);           /* A FOG: F 5A */
    s[12] = qw(0x5555555555555555ull, 0xFFFFFFFFFFFFFFFFull);           /* B: raw to 0x0B */
    s[13] = qw(0x6666666666666666ull, 0xFFFFFFFFFFFFFFFFull);           /* C XYZF3: raw */
    s[14] = qw(0x7777777777777777ull, 0xFFFFFFFFFFFFFFFFull);           /* D XYZ3: raw */
    s[15] = qw(0x0123456789ABCDEFull, 0xFFFFFFFFFFFFFF4Cull);           /* E A+D: FRAME_1 */
    s[16] = qw(0x8888888888888888ull, 0x8888888888888888ull);           /* F NOP */
    CHECK_EQ(gif_path2(s, 17), 17);
    CHECK_EQ(nev, 15);
    expect_reg(0, 0x00, 0x5A5);
    expect_reg(1, 0x01, 0x3F8000009A785612ull);                         /* Q = 1.0 before any ST */
    expect_reg(2, 0x02, 0x4040000040000000ull);
    expect_reg(3, 0x03, 0x01230456ull);
    expect_reg(4, 0x04, 0x9CABCDEF56781234ull);
    expect_reg(5, 0x05, 0x89ABCDEF56781234ull);
    expect_reg(6, 0x06, 0x1111111111111111ull);
    expect_reg(7, 0x07, 0x2222222222222222ull);
    expect_reg(8, 0x08, 0x3333333333333333ull);
    expect_reg(9, 0x09, 0x4444444444444444ull);
    expect_reg(10, 0x0A, 0x5A00000000000000ull);
    expect_reg(11, 0x0B, 0x5555555555555555ull);
    expect_reg(12, 0x0C, 0x6666666666666666ull);
    expect_reg(13, 0x0D, 0x7777777777777777ull);
    expect_reg(14, 0x4C, 0x0123456789ABCDEFull);
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
}

static void test_packed_adc_and_q(void) {
    reset_gif();
    /* vertex loop: ST, RGBAQ, XYZF2 (ADC 1), XYZ2 (ADC 1); NLOOP 2 */
    qword s[10];
    s[0] = tag(2, 1, 1, 0x006, 0, 4, 0x5412);
    s[1] = qw(0x3F0000003E800000ull, 0x0000000041000000ull);            /* ST, Q = 8.0 */
    s[2] = qw(0x0000002000000010ull, 0x0000008000000040ull);            /* RGBA 10 20 40 80 */
    s[3] = qw(0x0000000200000001ull, 0x0000F00000000030ull);            /* XYZF2: X1 Y2 Z3 F0 ADC1 */
    s[4] = qw(0x0000000400000003ull, 0x0000800000000005ull);            /* XYZ2: X3 Y4 Z5 ADC1 */
    s[5] = qw(0x3F8000003F800000ull, 0x0000000040000000ull);            /* ST, Q = 2.0 */
    s[6] = qw(0x000000FF000000EEull, 0x000000CC000000DDull);            /* RGBA EE FF DD CC */
    s[7] = qw(0x0000000200000001ull, 0x0000700000000030ull);            /* XYZF2: ADC 0 */
    s[8] = qw(0x0000000400000003ull, 0x0000000000000005ull);            /* XYZ2: ADC 0 */
    /* next tag: Q resets to 1.0 */
    s[9] = tag(1, 1, 0, 0, 0, 1, 0x1);
    qword rgba = qw(0x0000000200000001ull, 0x0000000400000003ull);
    CHECK_EQ(gif_path2(s, 10), 10);
    CHECK_EQ(gif_path2(&rgba, 1), 1);
    CHECK_EQ(nev, 10);
    expect_reg(0, 0x00, 0x006);                                         /* PRE */
    expect_reg(1, 0x02, 0x3F0000003E800000ull);
    expect_reg(2, 0x01, 0x4100000080402010ull);
    expect_reg(3, 0x0C, 0x0000000300020001ull);
    expect_reg(4, 0x0D, 0x0000000500040003ull);
    expect_reg(5, 0x02, 0x3F8000003F800000ull);
    expect_reg(6, 0x01, 0x40000000CCDDFFEEull);
    expect_reg(7, 0x04, 0x0000000300020001ull);
    expect_reg(8, 0x05, 0x0000000500040003ull);
    expect_reg(9, 0x01, 0x3F80000004030201ull);
}

static void test_packed_ad_hwreg(void) {
    reset_gif();
    qword s[4];
    s[0] = tag(3, 1, 0, 0, 0, 1, 0xE);
    s[1] = qw(0x0000000000000001ull, 0x53);                             /* TRXDIR */
    s[2] = qw(0x0807060504030201ull, 0x54);                             /* HWREG: image data */
    s[3] = qw(0x00000000DEADBEEFull, 0xFFFFFFFFFFFFFF7Full);            /* only bits 64-71 are the address */
    CHECK_EQ(gif_path2(s, 4), 4);
    CHECK_EQ(nev, 3);
    expect_reg(0, 0x53, 1);
    CHECK(nev > 1 && evs[1].image && evs[1].len == 8 && memcmp(img + evs[1].off, "\x01\x02\x03\x04\x05\x06\x07\x08", 8) == 0);
    expect_reg(2, 0x7F, 0xDEADBEEF);
}

/* ---- tag fields -------------------------------------------------------------------------------- */

static void test_pre_rules(void) {
    reset_gif();
    qword s[6];
    s[0] = tag(0, 0, 1, 0x123, 0, 1, 0);           /* NLOOP 0: PRE ignored */
    s[1] = tag(1, 0, 1, 0x7FF, 1, 2, 0x10);        /* REGLIST: PRE ignored */
    s[2] = qw(0xAAAA, 0xBBBB);
    s[3] = tag(1, 0, 1, 0x2AB, 2, 1, 0);           /* IMAGE: PRE ignored */
    s[4] = qw(0, 0);
    s[5] = tag(0, 1, 0, 0, 0, 0, 0);
    CHECK_EQ(gif_path2(s, 6), 6);
    CHECK_EQ(nev, 3);
    expect_reg(0, 0x00, 0xAAAA);
    expect_reg(1, 0x01, 0xBBBB);
    CHECK(nev > 2 && evs[2].image && evs[2].len == 16);
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
}

static void test_nreg_zero_is_16(void) {
    reset_gif();
    qword s[1 + 32];
    s[0] = tag(2, 1, 0, 0, 0, 0, 0x3333333333333333ull);                /* 16 x UV, 2 loops */
    for (int i = 0; i < 32; i++) s[1 + i] = qw((uint64_t)i, 0);
    CHECK_EQ(gif_path2(s, 33), 33);
    CHECK_EQ(nev, 32);
    for (int i = 0; i < 32 && i < nev; i++) expect_reg(i, 0x03, (uint64_t)i);
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
}

/* ---- REGLIST ----------------------------------------------------------------------------------- */

static void test_reglist(void) {
    reset_gif();
    /* 3 registers x 1 loop = 3 doublewords: 2 qwords, the last half is padding */
    qword s[8];
    s[0] = tag(1, 0, 0, 0, 1, 3, 0x0E1);          /* RGBAQ, A+D (outputs nothing), PRIM */
    s[1] = qw(0x1111, 0x2222);
    s[2] = qw(0x3333, 0x9999);                     /* 0x9999: padding */
    /* 3 registers x 2 loops = 6 doublewords: 3 qwords, no padding */
    s[3] = tag(2, 1, 0, 0, 1, 3, 0xF64);          /* XYZF2, TEX0_1, NOP */
    s[4] = qw(0xA1, 0xA2);
    s[5] = qw(0xA3, 0xB1);
    s[6] = qw(0xB2, 0xB3);
    s[7] = tag(0, 1, 0, 0, 0, 0, 0);
    CHECK_EQ(gif_path2(s, 8), 8);
    CHECK_EQ(nev, 6);
    expect_reg(0, 0x01, 0x1111);
    expect_reg(1, 0x00, 0x3333);
    expect_reg(2, 0x04, 0xA1);
    expect_reg(3, 0x06, 0xA2);
    expect_reg(4, 0x04, 0xB1);
    expect_reg(5, 0x06, 0xB2);
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
}

/* ---- IMAGE ------------------------------------------------------------------------------------- */

static void test_image(void) {
    reset_gif();
    qword s[8];
    s[0] = tag(3, 0, 0, 0, 2, 0, 0);
    for (int i = 0; i < 3; i++) for (int b = 0; b < 16; b++) s[1 + i].b[b] = (uint8_t)(i * 16 + b);
    s[4] = tag(2, 1, 0, 0, 3, 0, 0);               /* FLG 3 behaves as IMAGE */
    for (int i = 0; i < 2; i++) for (int b = 0; b < 16; b++) s[5 + i].b[b] = (uint8_t)(0x80 + i * 16 + b);
    s[7] = tag(1, 1, 0, 0, 0, 1, 0x3);             /* a following packet still parses */
    qword uv = qw(0x77, 0);
    CHECK_EQ(gif_path3_dma(s, 8), 8);
    CHECK_EQ(gif_path3_dma(&uv, 1), 1);
    CHECK_EQ(nev, 2);
    CHECK(nev > 0 && evs[0].image && evs[0].len == 80);
    int ok = 1;
    for (int i = 0; i < 48; i++) ok &= img[i] == i;
    for (int i = 0; i < 32; i++) ok &= img[48 + i] == 0x80 + i;
    CHECK(ok);
    expect_reg(1, 0x03, 0x77);
}

/* ---- split packets ----------------------------------------------------------------------------- */

static qword stream[64];
static int stream_len;
static event ref_evs[256];
static int ref_nev;
static uint8_t ref_img[4096];
static uint32_t ref_img_len;

static void build_stream(void) {
    int n = 0;
    stream[n++] = tag(2, 0, 1, 0x00D, 0, 3, 0x412);                     /* ST, RGBAQ, XYZF2 x2 */
    stream[n++] = qw(0x3F0000003E800000ull, 0x41000000);
    stream[n++] = qw(0x0000002000000010ull, 0x0000008000000040ull);
    stream[n++] = qw(0x0000000200000001ull, 0x0000700000000030ull);
    stream[n++] = qw(0x3F8000003F800000ull, 0x40000000);
    stream[n++] = qw(0x000000FF000000EEull, 0x000000CC000000DDull);
    stream[n++] = qw(0x0000000400000003ull, 0x0000F00000000050ull);
    stream[n++] = tag(1, 0, 0, 0, 1, 5, 0x6E321);                       /* REGLIST, odd: 5 dwords */
    stream[n++] = qw(0xD1, 0xD2);
    stream[n++] = qw(0xD3, 0xD4);
    stream[n++] = qw(0xD5, 0xFFFF);
    stream[n++] = tag(4, 0, 0, 0, 2, 0, 0);                             /* IMAGE x4 */
    for (int i = 0; i < 4; i++) { qword q; for (int b = 0; b < 16; b++) q.b[b] = (uint8_t)(i * 16 + b + 1); stream[n++] = q; }
    stream[n++] = tag(0, 0, 0, 0, 0, 0, 0);                             /* empty tag inside the packet */
    stream[n++] = tag(2, 1, 0, 0, 0, 1, 0xE);                           /* A+D x2, EOP */
    stream[n++] = qw(0x1234, 0x50);
    stream[n++] = qw(0x5678, 0x53);
    stream[n++] = tag(0, 1, 0, 0, 0, 0, 0);                             /* empty packet */
    stream[n++] = tag(1, 1, 0, 0, 1, 1, 0x3);                           /* REGLIST 1 dword, EOP */
    stream[n++] = qw(0xEE, 0xFF);
    stream_len = n;
}

static void save_ref(void) {
    ref_nev = nev; memcpy(ref_evs, evs, sizeof(event) * (size_t)nev);
    ref_img_len = img_len; memcpy(ref_img, img, img_len);
}

static int same_as_ref(void) {
    if (nev != ref_nev || img_len != ref_img_len || memcmp(img, ref_img, img_len) != 0) return 0;
    for (int i = 0; i < nev; i++)
        if (evs[i].image != ref_evs[i].image || evs[i].reg != ref_evs[i].reg || evs[i].val != ref_evs[i].val ||
            evs[i].off != ref_evs[i].off || evs[i].len != ref_evs[i].len) return 0;
    return 1;
}

static void test_split_packets(void) {
    build_stream();
    reset_gif();
    CHECK_EQ(gif_path2(stream, (uint32_t)stream_len), (uint32_t)stream_len);
    save_ref();
    CHECK_EQ(ref_nev, 15);                         /* PRIM, 2x(ST RGBAQ XYZF2), 4 reglist, image, 2 A+D, 1 reglist */
    CHECK_EQ(ref_img_len, 64);
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);

    for (int chunk = 1; chunk <= 7; chunk++) {     /* every path, split into fixed-size pieces */
        clear();
        for (int i = 0; i < stream_len; i += chunk) {
            uint32_t n = (uint32_t)(stream_len - i < chunk ? stream_len - i : chunk);
            CHECK_EQ(gif_path2(&stream[i], n), n);
        }
        CHECK(same_as_ref());
        clear();
        for (int i = 0; i < stream_len; i += chunk) {
            uint32_t n = (uint32_t)(stream_len - i < chunk ? stream_len - i : chunk);
            CHECK_EQ(gif_path3_dma(&stream[i], n), n);
        }
        CHECK(same_as_ref());
    }
    srand(1234);                                   /* random split points */
    for (int round = 0; round < 50; round++) {
        clear();
        for (int i = 0; i < stream_len;) {
            uint32_t n = 1 + (uint32_t)(rand() % 9);
            if (n > (uint32_t)(stream_len - i)) n = (uint32_t)(stream_len - i);
            CHECK_EQ(gif_path3_dma(&stream[i], n), n);
            i += (int)n;
        }
        CHECK(same_as_ref());
    }
    /* FIFO stores: 128-bit, and 64-bit halves */
    clear();
    for (int i = 0; i < stream_len; i++) gif_access(0x10006000, stream[i].b, 16, 1);
    CHECK(same_as_ref());
    clear();
    for (int i = 0; i < stream_len; i++) { gif_access(0x10006000, stream[i].b, 8, 1); gif_access(0x10006008, stream[i].b + 8, 8, 1); }
    CHECK(same_as_ref());
    CHECK_EQ(gif_stat_rd(), 0);
}

static void test_paths_independent(void) {
    build_stream();
    reset_gif();
    CHECK_EQ(gif_path2(stream, (uint32_t)stream_len), (uint32_t)stream_len);
    save_ref();
    /* PATH3 stops mid-vertex (inside the first PACKED tag); PATH2 and PATH1 packets run in between */
    clear();
    CHECK_EQ(gif_path3_dma(stream, 5), 5);
    uint32_t st = gif_stat_rd();
    CHECK_EQ(STAT_APATH(st), 3);
    CHECK(st & STAT_OPH);
    int before = nev;
    qword other[3] = { tag(1, 1, 0, 0, 0, 2, 0x12), qw(0x40400000, 0x40A00000), qw(1, 0) };   /* ST (Q 5.0), RGBAQ */
    CHECK_EQ(gif_path2(other, 3), 3);
    static uint8_t vu1[16384];
    memcpy(vu1 + 100 * 16, other, sizeof other);
    gif_path1_kick(vu1, 100);
    int mid = nev;
    CHECK_EQ(mid - before, 4);
    if (mid - before == 4) {
        CHECK_EQ(evs[before + 1].val, 0x40A0000000000001ull);
        CHECK_EQ(evs[before + 3].val, 0x40A0000000000001ull);
    }
    /* drop the foreign events and finish PATH3 */
    nev = before;
    CHECK_EQ(gif_path3_dma(stream + 5, (uint32_t)stream_len - 5), (uint32_t)stream_len - 5);
    CHECK(same_as_ref());   /* PATH3's Q (2.0 from its own ST) was not disturbed by the other paths */
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
}

/* ---- PATH1 ------------------------------------------------------------------------------------- */

static void test_path1_wrap(void) {
    build_stream();
    reset_gif();
    const int first_len = 20;                      /* first packet: up to the A+D x2 tag's data (EOP) */
    clear();
    CHECK_EQ(gif_path2(stream, (uint32_t)first_len), (uint32_t)first_len);
    save_ref();
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);

    static uint8_t vu1[16384];
    int start;
    for (start = 1024 - first_len - 1; start <= 1024; start++) {   /* wrap at every offset in the packet */
        memset(vu1, 0, sizeof vu1);
        for (int i = 0; i < stream_len; i++) memcpy(vu1 + (size_t)((start + i) & 1023) * 16, stream[i].b, 16);
        clear();
        gif_path1_kick(vu1, (uint32_t)start + 1024u * 3);       /* address bits above 10 are ignored */
        CHECK(same_as_ref());                      /* stops at EOP: the following packets are not sent */
        CHECK_EQ((reg32(0x10003080) >> 20) & 0x3FF, (uint32_t)(start + first_len) & 1023);   /* GIF_CNT.VUADDR */
    }
    start = 1024;                                  /* the last layout written: the stream at qword 0 */
    /* the next kicks send the following packets */
    clear();
    gif_path1_kick(vu1, (uint32_t)(start + first_len) & 1023);
    CHECK_EQ(nev, 0);                              /* tag(0, EOP) */
    clear();
    gif_path1_kick(vu1, (uint32_t)(start + first_len + 1) & 1023);
    CHECK_EQ(nev, 1);
    expect_reg(0, 0x03, 0xEE);
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
}

static void test_path1_no_eop(void) {
    reset_gif();
    static uint8_t vu1[16384];
    memset(vu1, 0, sizeof vu1);                    /* endless NLOOP=0 tags without EOP */
    gif_path1_kick(vu1, 5);                        /* must return */
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
    qword s[2] = { tag(1, 1, 0, 0, 0, 1, 0x3), qw(0x42, 0) };
    memcpy(vu1, s, sizeof s);
    clear();
    gif_path1_kick(vu1, 0);                        /* PATH1 recovers */
    CHECK_EQ(nev, 1);
    expect_reg(0, 0x03, 0x42);
}

/* ---- PATH3 masking ----------------------------------------------------------------------------- */

static void test_mask_vif(void) {
    reset_gif();
    qword s[8];
    s[0] = tag(2, 1, 0, 0, 0, 1, 0x3); s[1] = qw(1, 0); s[2] = qw(2, 0);
    s[3] = tag(1, 1, 0, 0, 0, 1, 0x3); s[4] = qw(3, 0);
    s[5] = tag(1, 1, 0, 0, 0, 1, 0x3); s[6] = qw(4, 0);

    gif_set_path3_mask(1);                         /* masked at a boundary: nothing is taken */
    CHECK_EQ(gif_path3_dma(s, 7), 0);
    CHECK_EQ(nev, 0);
    uint32_t st = gif_stat_rd();
    CHECK(st & STAT_M3P);
    CHECK(st & STAT_P3Q);
    CHECK_EQ(STAT_APATH(st), 0);
    gif_set_path3_mask(0);
    CHECK_EQ(gif_path3_dma(s, 2), 2);              /* tag + 1 qword: packet in progress */
    CHECK_EQ(gif_stat_rd() & STAT_P3Q, 0);
    gif_set_path3_mask(1);                         /* the packet in progress completes first */
    CHECK_EQ(gif_path3_dma(s + 2, 5), 1);
    CHECK_EQ(nev, 2);
    CHECK(gif_stat_rd() & STAT_P3Q);
    CHECK_EQ(STAT_APATH(gif_stat_rd()), 0);
    CHECK_EQ(gif_path3_dma(s + 3, 4), 0);          /* DMAC retry while still masked */
    gif_set_path3_mask(0);
    CHECK_EQ(gif_path3_dma(s + 3, 4), 4);
    CHECK_EQ(nev, 4);
    for (int i = 0; i < 4 && i < nev; i++) expect_reg(i, 0x03, (uint64_t)i + 1);
    CHECK_EQ(gif_stat_rd(), 0);
    /* PATH2 and PATH1 ignore the PATH3 mask */
    gif_set_path3_mask(1);
    clear();
    CHECK_EQ(gif_path2(s, 3), 3);
    CHECK_EQ(nev, 2);
    gif_set_path3_mask(0);

    /* a mask set between two GIFtags of one packet waits for the EOP tag */
    reset_gif();
    qword m[7];
    m[0] = tag(1, 0, 0, 0, 0, 1, 0x3); m[1] = qw(1, 0);     /* no EOP */
    m[2] = tag(0, 0, 0, 0, 0, 0, 0);                          /* empty, no EOP */
    m[3] = tag(1, 1, 0, 0, 0, 1, 0x3); m[4] = qw(2, 0);     /* EOP */
    m[5] = tag(1, 1, 0, 0, 0, 1, 0x3); m[6] = qw(3, 0);     /* next packet */
    CHECK_EQ(gif_path3_dma(m, 2), 2);
    CHECK_EQ(STAT_APATH(gif_stat_rd()), 3);
    gif_set_path3_mask(1);
    CHECK_EQ(gif_path3_dma(m + 2, 5), 3);
    CHECK_EQ(nev, 2);
    gif_set_path3_mask(0);
    CHECK_EQ(gif_path3_dma(m + 5, 2), 2);
    CHECK_EQ(nev, 3);
    for (int i = 0; i < 3 && i < nev; i++) expect_reg(i, 0x03, (uint64_t)i + 1);
}

static void test_mask_gif_mode(void) {
    reset_gif();
    qword s[2] = { tag(1, 1, 0, 0, 0, 1, 0x3), qw(9, 0) };
    wreg32(0x10003010, 5);                         /* M3R + IMT */
    uint32_t st = gif_stat_rd();
    CHECK(st & STAT_M3R);
    CHECK(st & STAT_IMT);
    CHECK_EQ(st & STAT_M3P, 0);
    CHECK_EQ(gif_path3_dma(s, 2), 0);
    CHECK_EQ(nev, 0);
    gif_set_path3_mask(1);
    wreg32(0x10003010, 0);                         /* still masked by VIF1 */
    CHECK_EQ(gif_path3_dma(s, 2), 0);
    gif_set_path3_mask(0);
    CHECK_EQ(gif_stat_rd() & (STAT_M3R | STAT_IMT | STAT_M3P), 0);
    CHECK_EQ(gif_path3_dma(s, 2), 2);
    expect_reg(0, 0x03, 9);
}

static void test_fifo_masked(void) {
    reset_gif();
    qword s[6] = { tag(2, 1, 0, 0, 0, 1, 0x3), qw(1, 0), qw(2, 0), tag(1, 1, 0, 0, 2, 0, 0), qw(0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull),
                   tag(1, 1, 0, 0, 0, 1, 0x3) };
    gif_set_path3_mask(1);
    for (int i = 0; i < 5; i++) gif_access(0x10006000, s[i].b, 16, 1);
    CHECK_EQ(nev, 0);
    uint32_t st = gif_stat_rd();
    CHECK_EQ(STAT_FQC(st), 5);
    CHECK(st & STAT_P3Q);
    CHECK_EQ(gif_path3_dma(&s[5], 1), 0);          /* DMA queues behind the FIFO */
    gif_set_path3_mask(0);                         /* release: the FIFO drains in order */
    CHECK_EQ(STAT_FQC(gif_stat_rd()), 0);
    CHECK_EQ(nev, 3);
    expect_reg(0, 0x03, 1);
    expect_reg(1, 0x03, 2);
    CHECK(nev > 2 && evs[2].image && evs[2].len == 16 && img[15] == 0x0F);

    /* FIFO holds 16 qwords; a 17th store is dropped (the EE would stall forever) */
    reset_gif();
    gif_set_path3_mask(1);
    qword img_tag = tag(15, 1, 0, 0, 2, 0, 0), data = qw(0x1111111111111111ull, 0x2222222222222222ull);
    gif_access(0x10006000, img_tag.b, 16, 1);
    for (int i = 0; i < 16; i++) gif_access(0x10006000, data.b, 16, 1);
    CHECK_EQ(STAT_FQC(gif_stat_rd()), 16);
    gif_set_path3_mask(0);
    CHECK_EQ(STAT_FQC(gif_stat_rd()), 0);
    CHECK(nev == 1 && evs[0].image && evs[0].len == 15 * 16);
    CHECK_EQ(gif_stat_rd() & 0xE00, 0);
}

static void test_pause(void) {
    reset_gif();
    qword s[3] = { tag(2, 1, 0, 0, 0, 1, 0x3), qw(1, 0), qw(2, 0) };
    wreg32(0x10003000, 8);                         /* PSE */
    CHECK(gif_stat_rd() & STAT_PSE);
    CHECK_EQ(gif_path3_dma(s, 3), 0);
    wreg32(0x10003000, 0);
    CHECK_EQ(gif_stat_rd() & STAT_PSE, 0);
    CHECK_EQ(gif_path3_dma(s, 3), 3);
    CHECK_EQ(nev, 2);
}

/* ---- registers --------------------------------------------------------------------------------- */

static void test_registers(void) {
    reset_gif();
    qword s[3] = { tag(3, 1, 1, 0x155, 0, 2, 0x31), qw(0x10, 0), qw(0x20, 0) };   /* RGBAQ, UV x3 */
    CHECK_EQ(gif_path3_dma(s, 2), 2);              /* stopped after one register of loop 1 */
    uint64_t lo, hi;
    memcpy(&lo, s[0].b, 8);
    memcpy(&hi, s[0].b + 8, 8);
    CHECK_EQ(reg32(0x10003040), (uint32_t)lo);
    CHECK_EQ(reg32(0x10003050), (uint32_t)(lo >> 32));
    CHECK_EQ(reg32(0x10003060), (uint32_t)hi);
    CHECK_EQ(reg32(0x10003070), (uint32_t)(hi >> 32));
    uint32_t cnt = reg32(0x10003080);
    CHECK_EQ(cnt & 0x7FFF, 3);                     /* loops left */
    CHECK_EQ((cnt >> 16) & 15, 1);                 /* next descriptor */
    CHECK_EQ(reg32(0x10003090), 3);                /* GIF_P3CNT */
    CHECK_EQ(reg32(0x100030A0), (uint32_t)lo);     /* GIF_P3TAG */
    uint32_t st = gif_stat_rd();
    CHECK_EQ(STAT_APATH(st), 3);
    CHECK(st & STAT_OPH);
    /* wide and narrow reads */
    uint8_t wide[16];
    memset(wide, 0xCC, sizeof wide);
    gif_access(0x10003020, wide, 16, 0);
    uint32_t w0; memcpy(&w0, wide, 4);
    CHECK_EQ(w0, st);
    int zero = 1; for (int i = 4; i < 16; i++) zero &= wide[i] == 0;
    CHECK(zero);
    uint8_t b1 = 0xCC;
    gif_access(0x10003021, &b1, 1, 0);
    CHECK_EQ(b1, (st >> 8) & 0xFF);
    /* GIF_CTRL.RST abandons the packet: the next qword is a GIFtag again */
    wreg32(0x10003000, 1);
    CHECK_EQ(gif_stat_rd(), 0);
    CHECK_EQ(reg32(0x10003090), 0);
    clear();
    qword t[2] = { tag(1, 1, 0, 0, 0, 1, 0x3), qw(0x55, 0) };
    CHECK_EQ(gif_path3_dma(t, 2), 2);
    CHECK_EQ(nev, 1);
    expect_reg(0, 0x03, 0x55);
    /* PATH2 inside a packet shows as APATH 2 */
    CHECK_EQ(gif_path2(s, 1), 1);
    CHECK_EQ(STAT_APATH(gif_stat_rd()), 2);
    wreg32(0x10003000, 1);
    /* write-only and reserved registers */
    int before = unhandled;
    CHECK_EQ(reg32(0x10003000), 0);
    CHECK_EQ(reg32(0x10003010), 0);
    CHECK_EQ(before, unhandled);
    reg32(0x10003030);
    CHECK_EQ(unhandled, before + 1);
    wreg32(0x10003020, 0xFFFFFFFF);                /* GIF_STAT is read-only */
    CHECK_EQ(gif_stat_rd(), 0);
}

int main(void) {
    test_packed_all_descriptors();
    test_packed_adc_and_q();
    test_packed_ad_hwreg();
    test_pre_rules();
    test_nreg_zero_is_16();
    test_reglist();
    test_image();
    test_split_packets();
    test_paths_independent();
    test_path1_wrap();
    test_path1_no_eop();
    test_mask_vif();
    test_mask_gif_mode();
    test_fifo_masked();
    test_pause();
    test_registers();
    printf("gif: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
