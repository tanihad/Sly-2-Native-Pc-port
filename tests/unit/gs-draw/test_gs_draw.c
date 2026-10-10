/* Unit tests for gs.c / gs_draw*.c: registers, rasterizer, pixel pipeline, texture cache, CSR and
 * display. Scenes are drawn through gs_write_reg exactly as the GIF would, read back through the
 * fake gs_mem and written as PNGs to build/unit/gs-draw/ (argv[1]). */
#include "gs_draw.h"

#include <math.h>
#include <stdlib.h>
#include <windows.h>

/* ---- fakes of neighbouring subsystems ------------------------------------------------------------ */

static int intc_count[16];
void intc_raise(int cause) { intc_count[cause & 15]++; }
void dmac_raise(int ch) { (void)ch; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)data;
    fprintf(stderr, "[%s] unhandled %s%u 0x%08X\n", unit, is_write ? "write" : "read", size * 8, phys);
}
extern int fake_clut_loads;

#ifdef TEST_ASYNC
/* The same scenes through the GS thread and its pixel workers (gs_thread.c, started in main): wait
 * for the queue to drain before looking at VRAM. */
static uint32_t synced_mem_read(int psm, uint32_t bp, uint32_t bw, int x, int y) {
    gs_sync();
    return gs_mem_read(psm, bp, bw, x, y);
}
#define gs_mem_read synced_mem_read
#endif

/* ---- test helpers --------------------------------------------------------------------------------- */

static int fails, checks;
static const char* outdir = ".";
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

enum { PRIM = 0x00, RGBAQ = 0x01, ST = 0x02, UV = 0x03, XYZF2 = 0x04, XYZ2 = 0x05, TEX0_1 = 0x06,
       CLAMP_1 = 0x08, FOG = 0x0A, XYZ3 = 0x0D, TEX1_1 = 0x14, TEX2_1 = 0x16, XYOFFSET_1 = 0x18,
       PRMODECONT = 0x1A, PRMODE = 0x1B, TEXCLUT = 0x1C, SCANMSK = 0x22, MIPTBP1_1 = 0x34,
       TEXA = 0x3B, FOGCOL = 0x3D, TEXFLUSH = 0x3F, SCISSOR_1 = 0x40, ALPHA_1 = 0x42, DIMX = 0x44,
       DTHE = 0x45, COLCLAMP = 0x46, TEST_1 = 0x47, PABE = 0x49, FBA_1 = 0x4A, FRAME_1 = 0x4C,
       ZBUF_1 = 0x4E, BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52, TRXDIR = 0x53, HWREG = 0x54,
       SIGNAL = 0x60, FINISH = 0x61, LABEL = 0x62 };
enum { P_POINT, P_LINE, P_LINESTRIP, P_TRI, P_TRISTRIP, P_TRIFAN, P_SPRITE };
#define IIP (1 << 3)
#define TME (1 << 4)
#define FGE (1 << 5)
#define ABE (1 << 6)
#define FST (1 << 8)
#define CTXT2 (1 << 9)

static void W(uint32_t r, uint64_t v) { gs_write_reg(r, v); }

static uint32_t FBP = 0, FBW = 10;       /* current frame buffer (pages, 64-px units) */
static int FPSM = PSMCT32;

static void frame(uint32_t fbp, uint32_t fbw, int psm, uint32_t fbmsk) {
    FBP = fbp; FBW = fbw; FPSM = psm;
    W(FRAME_1, fbp | ((uint64_t)fbw << 16) | ((uint64_t)psm << 24) | ((uint64_t)fbmsk << 32));
}
static void zbuf(uint32_t zbp, int psm, int zmsk) { W(ZBUF_1, zbp | ((uint64_t)(psm & 0xF) << 24) | ((uint64_t)zmsk << 32)); }
static void test_reg(int ate, int atst, int aref, int afail, int date, int datm, int zte, int ztst) {
    W(TEST_1, (uint64_t)ate | (uint64_t)atst << 1 | (uint64_t)aref << 4 | (uint64_t)afail << 12 |
              (uint64_t)date << 14 | (uint64_t)datm << 15 | (uint64_t)zte << 16 | (uint64_t)ztst << 17);
}
static void alpha_reg(int a, int b, int c, int d, int fix) {
    W(ALPHA_1, (uint64_t)a | (uint64_t)b << 2 | (uint64_t)c << 4 | (uint64_t)d << 6 | (uint64_t)fix << 32);
}
static void scissor(int x0, int x1, int y0, int y1) {
    W(SCISSOR_1, (uint64_t)x0 | (uint64_t)x1 << 16 | (uint64_t)y0 << 32 | (uint64_t)y1 << 48);
}
static void reset_env(void) {
    W(PRMODECONT, 1);
    frame(0, 10, PSMCT32, 0);
    zbuf(160, PSMZ32, 1);
    W(XYOFFSET_1, 0);
    scissor(0, 639, 0, 447);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    alpha_reg(0, 1, 0, 1, 0);
    W(FBA_1, 0); W(PABE, 0); W(COLCLAMP, 1); W(DTHE, 0); W(SCANMSK, 0);
    W(TEXA, 0x80ull << 32 | 0x80); W(FOGCOL, 0); W(TEX1_1, 0); W(CLAMP_1, 0);
}
static void prim(int type, int flags) { W(PRIM, (uint64_t)(type | flags)); }
static void rgba(int r, int g, int b, int a) { W(RGBAQ, (uint64_t)r | (uint64_t)g << 8 | (uint64_t)b << 16 | (uint64_t)a << 24 | (uint64_t)0x3F800000 << 32); }
static void rgbaq(int r, int g, int b, int a, float q) {
    uint32_t qb; memcpy(&qb, &q, 4);
    W(RGBAQ, (uint64_t)r | (uint64_t)g << 8 | (uint64_t)b << 16 | (uint64_t)a << 24 | (uint64_t)qb << 32);
}
static void st(float s, float t) { uint32_t a, b; memcpy(&a, &s, 4); memcpy(&b, &t, 4); W(ST, a | (uint64_t)b << 32); }
static void uv(int u16, int v16) { W(UV, (uint64_t)u16 | (uint64_t)v16 << 16); }
static void xyz(double x, double y, uint32_t z) {      /* pixel coordinates */
    W(XYZ2, (uint64_t)(uint16_t)lround(x * 16) | (uint64_t)(uint16_t)lround(y * 16) << 16 | (uint64_t)z << 32);
}
static void xyzf(double x, double y, uint32_t z, int f) {
    W(XYZF2, (uint64_t)(uint16_t)lround(x * 16) | (uint64_t)(uint16_t)lround(y * 16) << 16 | (uint64_t)(z & 0xFFFFFF) << 32 | (uint64_t)f << 56);
}
static void sprite(double x0, double y0, double x1, double y1, int r, int g, int b, int a) {
    prim(P_SPRITE, 0);
    rgba(r, g, b, a);
    xyz(x0, y0, 0);
    xyz(x1, y1, 0);
}
static void clear(int r, int g, int b, int a) { sprite(0, 0, 640, 448, r, g, b, a); }

static uint32_t px(int x, int y) { return gs_mem_read(FPSM, FBP * 32, FBW, x, y); }
static uint32_t rgb(uint32_t c) { return c & 0xFFFFFF; }
static int chan(uint32_t c, int i) { return (int)((c >> (i * 8)) & 0xFF); }
static int close_to(uint32_t a, uint32_t b, int tol) {
    for (int i = 0; i < 4; i++) if (abs(chan(a, i) - chan(b, i)) > tol) return 0;
    return 1;
}

static void upload(uint32_t dbp, uint32_t dbw, int dpsm, int x, int y, int w, int h, const void* data, uint32_t bytes) {
    W(BITBLTBUF, (uint64_t)dbp << 32 | (uint64_t)dbw << 48 | (uint64_t)dpsm << 56);
    W(TRXPOS, (uint64_t)x << 32 | (uint64_t)y << 48);
    W(TRXREG, (uint64_t)w | (uint64_t)h << 32);
    W(TRXDIR, 0);
    gs_image_write(data, bytes);
}

/* 256-entry CT32 CLUT in the CSM1 arrangement: entries 8-15 and 16-23 of every 32 swapped */
static void upload_clut8(uint32_t cbp, const uint32_t* pal) {
    uint32_t img[256];
    for (int i = 0; i < 256; i++) img[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)] = pal[i];
    upload(cbp, 1, PSMCT32, 0, 0, 16, 16, img, sizeof img);
}

static uint64_t tex0(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t tcc, uint32_t tfx,
                     uint32_t cbp, uint32_t cpsm, uint32_t csa, uint32_t cld) {
    return (uint64_t)tbp | (uint64_t)tbw << 14 | (uint64_t)psm << 20 | (uint64_t)tw << 26 | (uint64_t)th << 30 |
           (uint64_t)tcc << 34 | (uint64_t)tfx << 35 | (uint64_t)cbp << 37 | (uint64_t)cpsm << 51 |
           (uint64_t)csa << 56 | (uint64_t)cld << 61;
}

static void save(const char* name, uint32_t fbp, uint32_t fbw, int psm, int w, int h) {
    static uint32_t img[1024 * 1024];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = gs_mem_read(psm, fbp * 32, fbw, x, y), o;
            if (psm == PSMCT16 || psm == PSMCT16S)
                o = ((v & 0x1F) << 19) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 3);
            else
                o = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
            img[y * w + x] = o;
        }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", outdir, name);
    debug_write_png(path, img, w, h, w);
}

/* ---- tests ---------------------------------------------------------------------------------------- */

static void t_sprite_coverage(void) {
    reset_env();
    clear(0, 0, 0, 0);
    sprite(10, 10, 20, 20, 255, 0, 0, 0x80);
    int n = 0;
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) n += rgb(px(x, y)) == 0x0000FF;
    CHECK(n == 100, "sprite 10x10 covers %d pixels", n);
    CHECK(px(10, 10) == 0x800000FF && px(19, 19) == 0x800000FF, "sprite corners");
    CHECK(rgb(px(20, 20)) == 0 && rgb(px(9, 10)) == 0, "sprite exclusive edges");
    /* half-pixel coordinates: pixels whose sample point (integer) lies in [10.5, 20.5) */
    clear(0, 0, 0, 0);
    sprite(10.5, 10.5, 20.5, 20.5, 0, 255, 0, 0x80);
    CHECK(rgb(px(10, 10)) == 0 && rgb(px(11, 11)) == 0x00FF00 && rgb(px(20, 20)) == 0x00FF00 && rgb(px(21, 20)) == 0,
          "half-pixel sprite");
    /* reversed vertex order */
    clear(0, 0, 0, 0);
    sprite(30, 30, 25, 25, 0, 0, 255, 0x80);
    n = 0;
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) n += rgb(px(x, y)) == 0xFF0000;
    CHECK(n == 25, "reversed sprite covers %d", n);
}

static void t_fill_rule(void) {
    reset_env();
    clear(0, 0, 0, 0x80);
    /* additive: Cs*FIX + Cd with FIX=0x80 -> Cs + Cd; overlap would show as 0x80 */
    alpha_reg(0, 2, 2, 1, 0x80);
    rgba(0x40, 0, 0, 0x80);
    prim(P_TRI, ABE);
    xyz(100, 100, 0); xyz(200, 100, 0); xyz(200, 180, 0);
    xyz(100, 100, 0); xyz(200, 180, 0); xyz(100, 180, 0);
    int n = 0, over = 0;
    for (int y = 90; y < 190; y++)
        for (int x = 90; x < 210; x++) {
            int r = chan(px(x, y), 0);
            n += r == 0x40;
            over += r > 0x40;
        }
    CHECK(n == 100 * 80 && over == 0, "two triangles: %d pixels, %d drawn twice", n, over);

    /* strip and fan of the same quad, offset */
    prim(P_TRISTRIP, ABE);
    xyz(300, 100, 0); xyz(400, 100, 0); xyz(300, 180, 0); xyz(400, 180, 0);
    prim(P_TRIFAN, ABE);
    xyz(500, 140, 0); xyz(450, 100, 0); xyz(550, 100, 0); xyz(550, 180, 0); xyz(450, 180, 0); xyz(450, 100, 0);
    int ns = 0, os = 0, nf = 0, of = 0;
    for (int y = 90; y < 190; y++)
        for (int x = 290; x < 560; x++) {
            int r = chan(px(x, y), 0);
            if (x < 420) { ns += r == 0x40; os += r > 0x40; } else { nf += r == 0x40; of += r > 0x40; }
        }
    CHECK(ns == 100 * 80 && os == 0, "strip: %d pixels, %d twice", ns, os);
    CHECK(nf == 100 * 80 && of == 0, "fan: %d pixels, %d twice", nf, of);

    /* a fan of many thin triangles around a centre with fractional coordinates */
    clear(0, 0, 0, 0x80);
    prim(P_TRIFAN, ABE);
    rgba(0x40, 0, 0, 0x80);
    xyz(320.3, 240.7, 0);
    for (int i = 0; i <= 64; i++) {
        double a = i * 2 * 3.14159265358979 / 64;
        xyz(320.3 + 150.37 * cos(a), 240.7 + 150.37 * sin(a), 0);
    }
    over = 0; n = 0;
    for (int y = 80; y < 400; y++) for (int x = 160; x < 480; x++) { int r = chan(px(x, y), 0); over += r > 0x40; n += r == 0x40; }
    CHECK(over == 0 && n > 69000 && n < 71500, "fan circle: %d pixels, %d drawn twice", n, over);
    save("fill_rule", 0, 10, PSMCT32, 640, 448);
}

static void t_gouraud(void) {
    reset_env();
    clear(0, 0, 0, 0);
    prim(P_TRI, IIP);
    rgba(255, 0, 0, 0x80); xyz(100, 50, 0);
    rgba(0, 255, 0, 0x80); xyz(400, 400, 0);
    rgba(0, 0, 255, 0x80); xyz(20, 400, 0);
    CHECK(close_to(px(100, 52), 0x800000FF, 6), "near red vertex: %08X", px(100, 52));
    CHECK(close_to(px(395, 398), 0x8000FF00, 8), "near green vertex: %08X", px(395, 398));
    CHECK(close_to(px(25, 398), 0x80FF0000, 8), "near blue vertex: %08X", px(25, 398));
    uint32_t c = px((100 + 400 + 20) / 3, (50 + 400 + 400) / 3);
    CHECK(close_to(c, 0x80555555, 3), "centroid: %08X", c);
    /* flat shading takes the last vertex colour */
    prim(P_TRI, 0);
    rgba(255, 0, 0, 0x80); xyz(450, 50, 0);
    rgba(0, 255, 0, 0x80); xyz(630, 50, 0);
    rgba(10, 20, 30, 0x80); xyz(540, 200, 0);
    CHECK(px(540, 80) == 0x801E140A, "flat colour %08X", px(540, 80));
    save("gouraud", 0, 10, PSMCT32, 640, 448);
}

#define TEXBP (300 * 32)                 /* texture area, block units */
#define CLUTBP (290 * 32)

static void t_clut_textures(void) {
    reset_env();
    clear(0, 0, 0, 0);
    /* T8 16x16, index = x + 16 y */
    uint8_t idx[256];
    for (int i = 0; i < 256; i++) idx[i] = (uint8_t)i;
    upload(TEXBP, 2, PSMT8, 0, 0, 16, 16, idx, sizeof idx);
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = (uint32_t)i | (uint32_t)(255 - i) << 8 | (uint32_t)((i * 3) & 255) << 16 | 0x80000000u;
    upload_clut8(CLUTBP, pal);
    W(TEX0_1, tex0(TEXBP, 2, PSMT8, 4, 4, 1, 1 /*DECAL*/, CLUTBP, PSMCT32, 0, 1));
    prim(P_SPRITE, TME | FST);
    rgba(128, 128, 128, 128);
    uv(0, 0); xyz(300, 20, 0);
    uv(16 * 16, 16 * 16); xyz(316, 36, 0);
    int bad = 0;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) bad += px(300 + x, 20 + y) != pal[x + 16 * y];
    CHECK(bad == 0, "T8 1:1 mapping: %d wrong texels", bad);
    /* 2x magnification, nearest */
    uv(0, 0); xyz(340, 20, 0);
    uv(16 * 16, 16 * 16); xyz(372, 52, 0);
    bad = 0;
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) bad += px(340 + x, 20 + y) != pal[x / 2 + 16 * (y / 2)];
    CHECK(bad == 0, "T8 2x magnification: %d wrong", bad);

    /* T4 8x8 with a CT16 CLUT at CSA 2 and MODULATE */
    uint8_t t4[32];
    for (int i = 0; i < 32; i++) t4[i] = (uint8_t)((i * 2) & 15) | (uint8_t)(((i * 2 + 1) & 15) << 4);
    upload(TEXBP + 64, 1, PSMT4, 0, 0, 8, 8, t4, sizeof t4);
    uint16_t pal16[16];
    for (int i = 0; i < 16; i++) pal16[i] = (uint16_t)((i * 2) | ((31 - i * 2) << 5) | (i << 10) | 0x8000);
    upload(CLUTBP + 64, 1, PSMCT16, 0, 0, 8, 2, pal16, sizeof pal16);
    W(TEXA, 0x80ull << 32 | 0x20);       /* TA1 = 0x80 for A=1 texels */
    W(TEX0_1, tex0(TEXBP + 64, 1, PSMT4, 3, 3, 1, 0 /*MODULATE*/, CLUTBP + 64, PSMCT16, 2, 1));
    prim(P_SPRITE, TME | FST);
    rgba(0x80, 0x40, 0x80, 0x80);
    uv(0, 0); xyz(400, 20, 0);
    uv(8 * 16, 8 * 16); xyz(408, 28, 0);
    bad = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int k = (y * 8 + x) & 15;    /* index at (x, y): ((y*8+x)) & 15 by construction */
            uint32_t e = pal16[k];
            int r = (int)(e & 31) << 3, g = (int)((e >> 5) & 31) << 3, b = (int)((e >> 10) & 31) << 3;
            uint32_t want = (uint32_t)r | (uint32_t)((g * 0x40) >> 7) << 8 | (uint32_t)b << 16 | 0x80000000u;
            bad += px(400 + x, 20 + y) != want;
        }
    CHECK(bad == 0, "T4 CSA/CT16 CLUT modulate: %d wrong (e.g. %08X)", bad, px(400, 20));

    /* CLUT reload with new data: same texture must show new colours */
    for (int i = 0; i < 256; i++) pal[i] = 0x80000000u | (uint32_t)(255 - i);
    upload_clut8(CLUTBP, pal);
    W(TEX0_1, tex0(TEXBP, 2, PSMT8, 4, 4, 1, 1, CLUTBP, PSMCT32, 0, 1));
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(300, 60, 0);
    uv(16 * 16, 16 * 16); xyz(316, 76, 0);
    bad = 0;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) bad += px(300 + x, 60 + y) != pal[x + 16 * y];
    CHECK(bad == 0, "T8 after CLUT reload: %d wrong", bad);
    /* CLD=4 with an unchanged CBP: no reload (fake counts loads) */
    gs_sync();                           /* (no-op unless TEST_ASYNC) */
    int loads = fake_clut_loads;
    W(TEX0_1, tex0(TEXBP, 2, PSMT8, 4, 4, 1, 1, CLUTBP, PSMCT32, 0, 4));
    W(TEX0_1, tex0(TEXBP, 2, PSMT8, 4, 4, 1, 1, CLUTBP, PSMCT32, 0, 4));
    gs_sync();
    CHECK(loads < 0 || fake_clut_loads == loads + 1, "gs_clut_on_tex0 called for every TEX0 write");   /* < 0: real gs_mem */
    /* TEX2 changes only PSM/CLUT fields */
    W(TEX2_1, tex0(0, 0, PSMT8, 0, 0, 0, 0, CLUTBP, PSMCT32, 0, 0));
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(340, 60, 0);
    uv(16 * 16, 16 * 16); xyz(356, 76, 0);
    CHECK(px(345, 61) == pal[5 + 16], "TEX2 keeps TBP/TW/TH: %08X", px(345, 61));
    save("clut", 0, 10, PSMCT32, 640, 448);
}

static void t_bilinear(void) {
    reset_env();
    clear(0, 0, 0, 0);
    uint32_t tex[4] = { 0x80000000, 0x80FFFFFF, 0x80FFFFFF, 0x80000000 };
    upload(TEXBP, 1, PSMCT32, 0, 0, 2, 2, tex, sizeof tex);
    W(TEX0_1, tex0(TEXBP, 1, PSMCT32, 1, 1, 1, 1, 0, 0, 0, 0));
    W(CLAMP_1, 1 | 1 << 2);              /* CLAMP both */
    W(TEX1_1, 1 << 5 | 1 << 6);          /* MMAG = MMIN = LINEAR */
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(100, 100, 0);
    uv(32, 32); xyz(164, 164, 0);
    uint32_t c = px(132, 132), tl = px(100, 100), tr = px(163, 100);
    CHECK(close_to(c, 0x80808080, 8), "bilinear centre %08X", c);
    CHECK(rgb(tl) == 0 && rgb(tr) == 0xFFFFFF, "bilinear clamped corners %08X %08X", tl, tr);
    int mono = 1;
    for (int x = 100; x < 132; x++) if (chan(px(x + 1, 108), 0) < chan(px(x, 108), 0)) mono = 0;
    CHECK(mono, "bilinear ramp is monotonic");
    /* repeat wraps: a 4x magnified sprite spanning 2 texture periods */
    W(CLAMP_1, 0);
    uv(0, 0); xyz(200, 100, 0);
    uv(64, 64); xyz(264, 164, 0);
    CHECK(close_to(px(216, 116), px(248, 148), 2), "repeat period");
    save("bilinear", 0, 10, PSMCT32, 640, 448);
    W(TEX1_1, 0);
}

static void t_blend(void) {
    reset_env();
    clear(0, 0, 255, 0x80);
    alpha_reg(0, 1, 0, 1, 0);            /* (Cs - Cd) * As + Cd */
    prim(P_SPRITE, ABE);
    rgba(255, 0, 0, 0x40);
    xyz(10, 10, 0); xyz(30, 30, 0);
    uint32_t c = px(15, 15);
    CHECK(chan(c, 0) == 127 && chan(c, 2) == 127 && chan(c, 3) == 0x40, "50%% blend %08X", c);
    /* FIX = 0x80 and Cd * Ad style: A=Cd, B=0, C=FIX, D=0 -> Cd */
    alpha_reg(1, 2, 2, 2, 0x40);         /* Cd * FIX(0.5) */
    prim(P_SPRITE, ABE);
    rgba(0, 255, 0, 0x80);
    xyz(40, 10, 0); xyz(60, 30, 0);
    c = px(45, 15);
    CHECK(rgb(c) == 0x7F0000, "Cd*FIX %08X", c);
    /* COLCLAMP = 0 wraps: Cs + Cd with 255 + 255 */
    W(COLCLAMP, 0);
    alpha_reg(0, 2, 2, 1, 0x80);
    prim(P_SPRITE, ABE);
    rgba(10, 0, 255, 0x80);
    xyz(70, 10, 0); xyz(90, 30, 0);
    c = px(75, 15);
    CHECK(chan(c, 2) == ((255 + 255) & 255) && chan(c, 0) == 10, "COLCLAMP=0 wraps %08X", c);
    W(COLCLAMP, 1);
    prim(P_SPRITE, ABE);
    xyz(100, 10, 0); xyz(120, 30, 0);
    c = px(105, 15);
    CHECK(chan(c, 2) == 255, "COLCLAMP=1 clamps %08X", c);
    /* PABE: only As >= 0x80 blends */
    W(PABE, 1);
    alpha_reg(0, 1, 0, 1, 0);
    prim(P_SPRITE, ABE);
    rgba(255, 255, 255, 0x40);
    xyz(130, 10, 0); xyz(150, 30, 0);
    CHECK(rgb(px(135, 15)) == 0xFFFFFF, "PABE skips blending for As < 0x80: %08X", px(135, 15));
    W(PABE, 0);
    /* FBA forces alpha MSB */
    W(FBA_1, 1);
    sprite(160, 10, 180, 30, 1, 2, 3, 0x10);
    CHECK(px(165, 15) == 0x90030201, "FBA %08X", px(165, 15));
    W(FBA_1, 0);
    /* FBMSK: keep green/blue */
    frame(0, 10, PSMCT32, 0xFFFF00);
    sprite(190, 10, 210, 30, 0x11, 0x22, 0x33, 0x44);
    CHECK(px(195, 15) == 0x44FF0011, "FBMSK %08X", px(195, 15));
    frame(0, 10, PSMCT32, 0);
    save("blend", 0, 10, PSMCT32, 640, 448);
}

static void t_ztest(int zpsm) {
    reset_env();
    clear(0, 0, 0, 0);
    uint32_t zbp = 160;
    zbuf(zbp, zpsm, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);    /* ALWAYS, write z = 0 */
    prim(P_SPRITE, 0); rgba(0, 0, 0, 0); xyz(0, 0, 0); xyz(640, 448, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 2);    /* GEQUAL */
    prim(P_TRI, 0);
    rgba(255, 0, 0, 0x80); xyz(100, 100, 1000); xyz(300, 100, 1000); xyz(200, 300, 1000);
    rgba(0, 255, 0, 0x80); xyz(150, 150, 500); xyz(350, 150, 500); xyz(250, 350, 500);
    CHECK(rgb(px(200, 160)) == 0x0000FF, "nearer red stays (Z %X): %08X", zpsm, px(200, 160));
    CHECK(rgb(px(300, 200)) == 0x00FF00, "green outside overlap: %08X", px(300, 200));
    rgba(0, 0, 255, 0x80); xyz(150, 150, 2000); xyz(350, 150, 2000); xyz(250, 350, 2000);
    CHECK(rgb(px(200, 160)) == 0xFF0000, "nearer blue wins: %08X", px(200, 160));
    uint32_t zv = gs_mem_read(zpsm, zbp * 32, 10, 200, 160);
    CHECK(zv == 2000, "Z written: %u", zv);
    /* GREATER with equal Z fails */
    test_reg(0, 1, 0, 0, 0, 0, 1, 3);
    rgba(255, 255, 0, 0x80); xyz(150, 150, 2000); xyz(350, 150, 2000); xyz(250, 350, 2000);
    CHECK(rgb(px(200, 160)) == 0xFF0000, "GREATER equal fails: %08X", px(200, 160));
    /* ZMSK: colour written, Z kept */
    zbuf(zbp, zpsm, 1);
    test_reg(0, 1, 0, 0, 0, 0, 1, 2);
    rgba(255, 0, 255, 0x80); xyz(150, 150, 5000); xyz(350, 150, 5000); xyz(250, 350, 5000);
    CHECK(rgb(px(200, 160)) == 0xFF00FF && gs_mem_read(zpsm, zbp * 32, 10, 200, 160) == 2000, "ZMSK");
    /* interpolated Z: a slanted triangle intersecting a flat one */
    zbuf(zbp, zpsm, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    prim(P_SPRITE, 0); rgba(0, 0, 0, 0); xyz(400, 0, 0); xyz(640, 200, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 2);
    prim(P_TRI, 0);
    rgba(255, 0, 0, 0x80); xyz(400, 20, 3000); xyz(640, 20, 3000); xyz(520, 200, 3000);
    rgba(0, 255, 0, 0x80); xyz(400, 20, 1000); xyz(640, 20, 5000); xyz(400, 200, 1000);
    CHECK(rgb(px(450, 50)) == 0x0000FF && rgb(px(590, 50)) == 0x00FF00, "slanted Z intersection");
    char name[32];
    snprintf(name, sizeof name, "ztest_%02X", zpsm);
    save(name, 0, 10, PSMCT32, 640, 448);
    zbuf(160, PSMZ32, 1);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
}

static void t_scissor_offset(void) {
    reset_env();
    clear(0, 0, 0, 0);
    scissor(50, 99, 60, 89);
    sprite(0, 0, 200, 200, 255, 255, 255, 0x80);
    int n = 0;
    for (int y = 0; y < 200; y++) for (int x = 0; x < 200; x++) n += rgb(px(x, y)) == 0xFFFFFF;
    CHECK(n == 50 * 30 && rgb(px(50, 60)) && rgb(px(99, 89)) && !rgb(px(100, 89)) && !rgb(px(50, 90)), "scissor: %d", n);
    /* triangle clipped by scissor */
    prim(P_TRI, 0); rgba(0, 255, 0, 0x80);
    xyz(0, 0, 0); xyz(300, 70, 0); xyz(70, 300, 0);
    n = 0;
    for (int y = 0; y < 200; y++) for (int x = 0; x < 200; x++) n += rgb(px(x, y)) == 0x00FF00;
    CHECK(n > 0 && rgb(px(49, 70)) == 0, "clipped triangle: %d", n);
    /* typical XYOFFSET: (2048 - 320, 2048 - 224) and vertices around 2048 */
    scissor(0, 639, 0, 447);
    W(XYOFFSET_1, (uint64_t)((2048 - 320) * 16) | (uint64_t)((2048 - 224) * 16) << 32);
    prim(P_SPRITE, 0); rgba(0, 0, 255, 0x80);
    xyz(2048 - 10, 2048 - 10, 0); xyz(2048 + 10, 2048 + 10, 0);
    CHECK(rgb(px(310, 214)) == 0xFF0000 && rgb(px(329, 233)) == 0xFF0000 && rgb(px(330, 234)) == 0, "XYOFFSET");
    W(XYOFFSET_1, 0);
    /* SCANMSK: odd lines only (2 = even prohibited) */
    W(SCANMSK, 2);
    sprite(400, 0, 420, 20, 255, 255, 0, 0x80);
    CHECK(rgb(px(405, 4)) == 0 && rgb(px(405, 5)) == 0x00FFFF, "SCANMSK");
    W(SCANMSK, 0);
    save("scissor", 0, 10, PSMCT32, 640, 448);
}

static void t_alpha_tests(void) {
    reset_env();
    clear(0, 0, 0, 0);
    zbuf(160, PSMZ32, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    prim(P_SPRITE, 0); rgba(0, 0, 0, 0); xyz(0, 0, 0); xyz(640, 448, 0);
    /* ATST GEQUAL 0x40, KEEP */
    test_reg(1, 5, 0x40, 0, 0, 0, 1, 1);
    for (int i = 0; i < 8; i++) {
        prim(P_SPRITE, 0); rgba(255, 255, 255, i * 0x10); xyz(10 + i * 10, 10, 77); xyz(20 + i * 10, 20, 77);
    }
    int ok = 1;
    for (int i = 0; i < 8; i++) ok &= (rgb(px(15 + i * 10, 15)) != 0) == (i * 0x10 >= 0x40);
    CHECK(ok, "alpha test GEQUAL/KEEP");
    CHECK(gs_mem_read(PSMZ32, 160 * 32, 10, 15, 15) == 0 && gs_mem_read(PSMZ32, 160 * 32, 10, 75, 15) == 77, "KEEP leaves Z");
    /* FB_ONLY: colour written, Z not */
    test_reg(1, 0, 0, 1, 0, 0, 1, 1);
    prim(P_SPRITE, 0); rgba(0, 255, 0, 0x80); xyz(100, 10, 55); xyz(110, 20, 55);
    CHECK(rgb(px(105, 15)) == 0x00FF00 && gs_mem_read(PSMZ32, 160 * 32, 10, 105, 15) == 0, "FB_ONLY");
    /* ZB_ONLY: Z written, colour not */
    test_reg(1, 0, 0, 2, 0, 0, 1, 1);
    prim(P_SPRITE, 0); rgba(0, 0, 255, 0x80); xyz(120, 10, 99); xyz(130, 20, 99);
    CHECK(rgb(px(125, 15)) == 0 && gs_mem_read(PSMZ32, 160 * 32, 10, 125, 15) == 99, "ZB_ONLY");
    /* RGB_ONLY: alpha kept */
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    sprite(140, 10, 150, 20, 1, 1, 1, 0x33);
    test_reg(1, 0, 0, 3, 0, 0, 1, 1);
    sprite(140, 10, 150, 20, 9, 8, 7, 0x80);
    CHECK(px(145, 15) == 0x33070809, "RGB_ONLY %08X", px(145, 15));
    /* DATE: only pixels whose destination alpha MSB == DATM pass */
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    sprite(200, 10, 210, 20, 0, 0, 0, 0x80);
    sprite(210, 10, 220, 20, 0, 0, 0, 0x00);
    test_reg(0, 1, 0, 0, 1, 0, 1, 1);    /* DATM 0: pass where MSB is 0 */
    sprite(200, 10, 220, 20, 255, 0, 0, 0x80);
    CHECK(rgb(px(205, 15)) == 0 && rgb(px(215, 15)) == 0xFF, "DATE DATM=0 %08X %08X", px(205, 15), px(215, 15));
    test_reg(0, 1, 0, 0, 1, 1, 1, 1);    /* DATM 1 */
    sprite(200, 10, 205, 20, 0, 255, 0, 0x80);
    CHECK(rgb(px(202, 15)) == 0xFF00, "DATE DATM=1 %08X", px(202, 15));
    zbuf(160, PSMZ32, 1);
    save("alphatest", 0, 10, PSMCT32, 640, 448);
}

static void t_fog_tfx(void) {
    reset_env();
    clear(0, 0, 0, 0);
    W(FOGCOL, 0x00FF00);                 /* green fog */
    prim(P_SPRITE, FGE);
    rgba(255, 0, 0, 0x80);
    xyzf(10, 10, 0, 0); xyzf(20, 20, 0, 0);
    CHECK(rgb(px(15, 15)) == 0x00FE00 || rgb(px(15, 15)) == 0x00FF00, "fog F=0 -> FOGCOL %08X", px(15, 15));
    xyzf(30, 10, 0, 255); xyzf(40, 20, 0, 255);
    CHECK(rgb(px(35, 15)) == 0x0000FE || rgb(px(35, 15)) == 0x0000FF, "fog F=255 -> colour %08X", px(35, 15));
    xyzf(50, 10, 0, 128); xyzf(60, 20, 0, 128);
    CHECK(close_to(px(55, 15), 0x80007F80, 2), "fog F=128 %08X", px(55, 15));
    /* fog interpolated over a triangle via the FOG register path */
    prim(P_TRI, FGE | IIP);
    rgba(255, 0, 0, 0x80);
    W(FOG, 255ull << 56); xyz(100, 10, 0);
    W(FOG, 0ull << 56); xyz(300, 10, 0);
    W(FOG, 255ull << 56); xyz(100, 100, 0);
    CHECK(chan(px(105, 15), 0) > 230 && chan(px(280, 12), 1) > 200, "interpolated fog");

    /* texture functions with a solid texel (0x40, 0x80, 0xFF, A=0x40) */
    uint32_t t = 0x40FF8040;
    uint32_t tex[16];
    for (int i = 0; i < 16; i++) tex[i] = t;
    upload(TEXBP, 1, PSMCT32, 0, 0, 4, 4, tex, sizeof tex);
    struct { int tfx, tcc; uint32_t want; } cases[] = {
        { 0, 1, 0x20FF4020 },            /* MODULATE: (0x80,0x80,0xFF? ) see below */
        { 1, 1, 0x40FF8040 },            /* DECAL */
        { 1, 0, 0x60FF8040 },            /* DECAL, TCC=0 keeps vertex alpha */
        { 2, 1, 0xA0FFE0A0 },            /* HIGHLIGHT */
        { 3, 1, 0x40FFE0A0 },            /* HIGHLIGHT2 */
    };
    /* vertex colour (0x80, 0x80, 0x80, 0x60) for every case; MODULATE: c*t>>7 = t; a = 0x60*0x40>>7 = 0x30 */
    cases[0].want = 0x30FF8040;
    for (int i = 0; i < 5; i++) {
        W(TEX0_1, tex0(TEXBP, 1, PSMCT32, 2, 2, (uint32_t)cases[i].tcc, (uint32_t)cases[i].tfx, 0, 0, 0, 0));
        prim(P_SPRITE, TME | FST);
        rgba(0x80, 0x80, 0x80, 0x60);
        uv(0, 0); xyz(10 + i * 20, 200, 0);
        uv(64, 64); xyz(20 + i * 20, 210, 0);
        CHECK(px(15 + i * 20, 205) == cases[i].want, "TFX %d TCC %d: %08X want %08X", cases[i].tfx, cases[i].tcc,
              px(15 + i * 20, 205), cases[i].want);
    }
    save("fog_tfx", 0, 10, PSMCT32, 640, 448);
}

static void t_frame16(void) {
    reset_env();
    uint32_t fbp = 400, zbp = 470;       /* 640x448 at 16 bits = 70 pages each */
    frame(fbp, 10, PSMCT16, 0);
    zbuf(zbp, PSMZ16, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    clear(0, 0, 0, 0);
    sprite(10, 10, 20, 20, 0xF8, 0x80, 0x08, 0x80);
    CHECK(px(15, 15) == (0x1F | 0x10 << 5 | 0x01 << 10 | 0x8000), "CT16 conversion %04X", px(15, 15));
    /* Gouraud ramp with dithering */
    W(DTHE, 1);
    W(DIMX, 0x4 | 0x1 << 4 | 0x5 << 8 | 0x0 << 12 | 0x2ull << 16 | 0x6ull << 20 | 0x3ull << 24 | 0x7ull << 28 |
            0x5ull << 32 | 0x0ull << 36 | 0x4ull << 40 | 0x1ull << 44 | 0x3ull << 48 | 0x7ull << 52 | 0x2ull << 56 | 0x6ull << 60);
    prim(P_SPRITE, 0); rgba(102, 102, 102, 0x80); xyz(30, 10, 0); xyz(46, 26, 0);
    int differ = 0;
    for (int y = 10; y < 14; y++) for (int x = 30; x < 34; x++) differ |= px(x, y) != px(30, 10);
    CHECK(differ, "dither pattern varies");
    /* DIMX entry (row y & 3, column x & 3) is added before the 8 -> 5 bit truncation:
     * (32, 10): row 2 col 0 = 5 -> -3: 99 -> 12; (34, 13): row 1 col 2 = 3: 105 -> 13 */
    CHECK((px(32, 10) & 31) == 12 && (px(34, 13) & 31) == 13, "dither values %04X %04X", px(32, 10), px(34, 13));
    prim(P_SPRITE, 0); rgba(7, 7, 7, 0x80); xyz(50, 10, 0); xyz(66, 26, 0);
    /* 7 + dither: (50, 11): row 3 col 2 = 2 -> 9 -> 1; (50, 10): row 2 col 2 = 4 -> -4: 3 -> 0 */
    CHECK((px(50, 11) & 31) == 1 && (px(50, 10) & 31) == 0, "dither shifts the 5-bit value %04X %04X", px(50, 11), px(50, 10));
    W(DTHE, 0);
    /* Z16 test */
    test_reg(0, 1, 0, 0, 0, 0, 1, 2);
    prim(P_TRI, 0);
    rgba(255, 0, 0, 0x80); xyz(100, 100, 0x8000); xyz(200, 100, 0x8000); xyz(150, 200, 0x8000);
    rgba(0, 255, 0, 0x80); xyz(100, 100, 0x12345); xyz(200, 100, 0x12345); xyz(150, 200, 0x12345);   /* clamps to 0xFFFF */
    CHECK(gs_mem_read(PSMZ16, zbp * 32, 10, 150, 120) == 0xFFFF, "Z16 clamp");
    CHECK((px(150, 120) & 0x3E0) == 0x3E0, "Z16 nearer wins");
    /* 16-bit blending */
    alpha_reg(0, 1, 0, 1, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    zbuf(zbp, PSMZ16, 1);
    sprite(300, 10, 320, 30, 0, 0, 0xF8, 0x80);
    prim(P_SPRITE, ABE); rgba(0xF8, 0, 0, 0x40); xyz(300, 10, 0); xyz(320, 30, 0);
    uint32_t c = px(305, 15);
    CHECK((c & 31) == 15 && ((c >> 10) & 31) == 15, "CT16 blend %04X", c);
    save("frame16", fbp, 10, PSMCT16, 640, 448);
    frame(0, 10, PSMCT32, 0);
    zbuf(160, PSMZ32, 1);
}

static void t_perspective(void) {
    reset_env();
    clear(0x20, 0x20, 0x20, 0x80);
    /* 64x64 checkerboard, 8x8 squares */
    static uint32_t tex[64 * 64];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) tex[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0x80FFFFFF : 0x80C02000;
    upload(TEXBP, 1, PSMCT32, 0, 0, 64, 64, tex, sizeof tex);
    W(TEX0_1, tex0(TEXBP, 1, PSMCT32, 6, 6, 1, 1, 0, 0, 0, 0));
    W(TEX1_1, 1 << 5 | 1 << 6);
    /* floor quad: far edge (q = 0.25) at the top, near edge (q = 1) at the bottom */
    prim(P_TRISTRIP, TME);
    float qf = 0.25f, qn = 1.0f;
    rgbaq(128, 128, 128, 128, qf); st(0 * qf, 0 * qf); xyz(250, 50, 0);
    rgbaq(128, 128, 128, 128, qf); st(4 * qf, 0 * qf); xyz(390, 50, 0);
    rgbaq(128, 128, 128, 128, qn); st(0 * qn, 4 * qn); xyz(20, 400, 0);
    rgbaq(128, 128, 128, 128, qn); st(4 * qn, 4 * qn); xyz(620, 400, 0);
    /* at the vertical middle of the left edge, v = t/q interpolates hyperbolically: at screen y = 225 */
    /* (halfway), 1/q = linear: q = (0.25 + 1) / 2 = 0.625, t/q = (0 + 4)/2 / 0.625 = 3.2 (tiled) */
    save("perspective", 0, 10, PSMCT32, 640, 448);
    W(TEX1_1, 0);
    int n_white = 0;
    for (int x = 20; x < 620; x++) n_white += rgb(px(x, 390)) == 0xFFFFFF;
    CHECK(n_white > 100 && n_white < 500, "checker near row: %d white", n_white);
}

static void t_mipmap(void) {
    reset_env();
    clear(0, 0, 0, 0);
    static uint32_t l0[16 * 16], l1[8 * 8], l2[4 * 4];
    for (int i = 0; i < 256; i++) l0[i] = 0x800000FF;
    for (int i = 0; i < 64; i++) l1[i] = 0x8000FF00;
    for (int i = 0; i < 16; i++) l2[i] = 0x80FF0000;
    upload(TEXBP, 1, PSMCT32, 0, 0, 16, 16, l0, sizeof l0);
    upload(TEXBP + 32, 1, PSMCT32, 0, 0, 8, 8, l1, sizeof l1);
    upload(TEXBP + 64, 1, PSMCT32, 0, 0, 4, 4, l2, sizeof l2);
    W(TEX0_1, tex0(TEXBP, 1, PSMCT32, 4, 4, 1, 1, 0, 0, 0, 0));
    W(MIPTBP1_1, (uint64_t)(TEXBP + 32) | 1ull << 14 | (uint64_t)(TEXBP + 64) << 20 | 1ull << 34);
    struct { int lcm, mmin, k; float q; uint32_t want; } c[] = {
        { 1, 2, 16, 1.0f, 0x00FF00 },    /* K = 1.0, NEAREST_MIPMAP_NEAREST -> level 1 */
        { 1, 2, 32, 1.0f, 0xFF0000 },    /* K = 2.0 -> level 2 */
        { 1, 2, -16, 1.0f, 0x0000FF },   /* K < 0 -> magnification, level 0 */
        { 0, 2, 0, 0.25f, 0xFF0000 },    /* LOD = log2(1/0.25) = 2 */
        { 0, 4, 0, 0.5f, 0x00FF00 },     /* LOD 1, LINEAR_MIPMAP_NEAREST */
        { 0, 2, 0, 1.0f, 0x0000FF },     /* LOD 0 */
        { 1, 3, 24, 1.0f, 0x7F7F00 },    /* K = 1.5, NEAREST_MIPMAP_LINEAR: halfway green/blue */
    };
    for (int i = 0; i < 7; i++) {
        uint64_t k = (uint64_t)(c[i].k & 0xFFF);
        W(TEX1_1, (uint64_t)c[i].lcm | 2ull << 2 | (uint64_t)c[i].mmin << 6 | k << 32);
        prim(P_SPRITE, TME);
        rgbaq(128, 128, 128, 128, c[i].q); st(0, 0); xyz(10 + i * 20, 10, 0);
        rgbaq(128, 128, 128, 128, c[i].q); st(c[i].q, c[i].q); xyz(26 + i * 20, 26, 0);
        uint32_t got = rgb(px(18 + i * 20, 18));
        CHECK(close_to(got, c[i].want, 2), "mipmap case %d: %06X want %06X", i, got, c[i].want);
    }
    /* per-pixel LOD in a perspective triangle: far part uses smaller levels */
    W(TEX1_1, 0ull | 2ull << 2 | 2ull << 6);
    prim(P_TRISTRIP, TME);
    rgbaq(128, 128, 128, 128, 0.2f); st(0, 0); xyz(100, 100, 0);
    rgbaq(128, 128, 128, 128, 0.2f); st(0.2f, 0); xyz(300, 100, 0);
    rgbaq(128, 128, 128, 128, 1.0f); st(0, 1); xyz(100, 300, 0);
    rgbaq(128, 128, 128, 128, 1.0f); st(1, 1); xyz(300, 300, 0);
    CHECK(rgb(px(200, 102)) == 0xFF0000 && rgb(px(200, 298)) == 0x0000FF, "per-pixel LOD %06X %06X",
          rgb(px(200, 102)), rgb(px(200, 298)));
    save("mipmap", 0, 10, PSMCT32, 640, 448);
    W(TEX1_1, 0);
}

static void t_texture_cache(void) {
    reset_env();
    clear(0, 0, 0, 0);
    uint32_t tex[64];
    for (int i = 0; i < 64; i++) tex[i] = 0x800000FF;
    upload(TEXBP, 1, PSMCT32, 0, 0, 8, 8, tex, sizeof tex);
    W(TEX0_1, tex0(TEXBP, 1, PSMCT32, 3, 3, 1, 1, 0, 0, 0, 0));
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(10, 10, 0); uv(128, 128); xyz(18, 18, 0);
    CHECK(rgb(px(12, 12)) == 0x0000FF, "texture first use");
    for (int i = 0; i < 64; i++) tex[i] = 0x8000FF00;
    upload(TEXBP, 1, PSMCT32, 0, 0, 8, 8, tex, sizeof tex);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(20, 10, 0); uv(128, 128); xyz(28, 18, 0);
    CHECK(rgb(px(22, 12)) == 0x00FF00, "texture re-uploaded: %08X", px(22, 12));
    /* HWREG upload path */
    W(BITBLTBUF, (uint64_t)TEXBP << 32 | 1ull << 48 | (uint64_t)PSMCT32 << 56);
    W(TRXPOS, 0); W(TRXREG, 8 | 8ull << 32); W(TRXDIR, 0);
    for (int i = 0; i < 32; i++) W(HWREG, 0x80FF000080FF0000ull);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(30, 10, 0); uv(128, 128); xyz(38, 18, 0);
    CHECK(rgb(px(32, 12)) == 0xFF0000, "HWREG upload: %08X", px(32, 12));
    /* render to texture: draw yellow into the texture's page, then sample it */
    frame(TEXBP / 32, 1, PSMCT32, 0);
    sprite(0, 0, 8, 8, 255, 255, 0, 0x80);
    frame(0, 10, PSMCT32, 0);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(40, 10, 0); uv(128, 128); xyz(48, 18, 0);
    CHECK(rgb(px(42, 12)) == 0x00FFFF, "render to texture: %08X", px(42, 12));
    /* local -> local copy into the texture */
    for (int i = 0; i < 64; i++) tex[i] = 0x80FF00FF;
    upload(TEXBP + 32, 1, PSMCT32, 0, 0, 8, 8, tex, sizeof tex);
    W(BITBLTBUF, (uint64_t)(TEXBP + 32) | 1ull << 16 | (uint64_t)TEXBP << 32 | 1ull << 48);
    W(TRXPOS, 0); W(TRXREG, 8 | 8ull << 32); W(TRXDIR, 2);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(50, 10, 0); uv(128, 128); xyz(58, 18, 0);
    CHECK(rgb(px(52, 12)) == 0xFF00FF, "local->local copy: %08X", px(52, 12));
    /* 24-bit texture with TEXA */
    uint8_t t24[16 * 3];
    for (int i = 0; i < 16; i++) { t24[i * 3] = 10; t24[i * 3 + 1] = 20; t24[i * 3 + 2] = 30; }
    upload(TEXBP + 64, 1, PSMCT24, 0, 0, 4, 4, t24, sizeof t24);
    W(TEXA, 0x40);
    W(TEX0_1, tex0(TEXBP + 64, 1, PSMCT24, 2, 2, 1, 1, 0, 0, 0, 0));
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(60, 10, 0); uv(64, 64); xyz(64, 14, 0);
    CHECK(px(61, 11) == 0x401E140A, "CT24 + TEXA.TA0: %08X", px(61, 11));
    W(TEXA, 0x70);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(66, 10, 0); uv(64, 64); xyz(70, 14, 0);
    CHECK(px(67, 11) == 0x701E140A, "TEXA change: %08X", px(67, 11));
    /* readback through gs_image_read (local -> host) */
    W(BITBLTBUF, 0ull | 10ull << 16 | (uint64_t)PSMCT32 << 24);
    W(TRXPOS, 60 | 10ull << 16); W(TRXREG, 2 | 1ull << 32); W(TRXDIR, 1);
    uint32_t rb[2];
    gs_image_read(rb, 8);
    CHECK(rb[0] == 0x401E140A && rb[1] == 0x401E140A, "local->host readback %08X", rb[0]);
}

static void t_lines_points(void) {
    reset_env();
    clear(0, 0, 0, 0);
    prim(P_LINE, 0); rgba(255, 255, 255, 0x80);
    xyz(10, 300, 0); xyz(110, 300, 0);
    int n = 0;
    for (int x = 0; x < 200; x++) n += rgb(px(x, 300)) != 0;
    CHECK(n == 100, "horizontal line: %d pixels", n);
    prim(P_LINESTRIP, IIP);
    rgba(255, 0, 0, 0x80); xyz(200, 300, 0);
    rgba(0, 255, 0, 0x80); xyz(300, 350, 0);
    rgba(0, 0, 255, 0x80); xyz(250, 420, 0);
    CHECK(rgb(px(200, 300)) == 0x0000FF && rgb(px(250, 325)) != 0, "line strip");
    prim(P_POINT, 0); rgba(1, 2, 3, 0x80); xyz(400, 300, 0);
    CHECK(px(400, 300) == 0x80030201, "point");
    /* XYZ3 queues without drawing */
    prim(P_SPRITE, 0); rgba(9, 9, 9, 0x80);
    W(XYZ3, (uint64_t)(450 * 16) | (uint64_t)(300 * 16) << 16);
    W(XYZ3, (uint64_t)(460 * 16) | (uint64_t)(310 * 16) << 16);
    CHECK(rgb(px(455, 305)) == 0, "XYZ3 does not kick");
    save("lines", 0, 10, PSMCT32, 640, 448);
}

static void t_csr(void) {
    uint64_t v = 0;
    gs_priv_access(0x12001000, &v, 8, 0);
    CHECK(((v >> 14) & 3) == 1 && ((v >> 16) & 0xFF) == 0x1B && ((v >> 24) & 0xFF) == 0x55, "CSR FIFO/REV/ID %llX",
          (unsigned long long)v);
    uint32_t field0 = (uint32_t)(v >> 13) & 1;
    int before = intc_count[INTC_GS];
    gs_vsync();
    gs_priv_access(0x12001000, &v, 8, 0);
    CHECK(((v >> 13) & 1) != field0 && (v & 8), "vsync: FIELD toggles, VSINT set");
    CHECK(intc_count[INTC_GS] == before, "masked VSINT raises nothing");
    uint32_t w = 8;
    gs_priv_access(0x12001000, &w, 4, 1);
    gs_priv_access(0x12001000, &v, 8, 0);
    CHECK(!(v & 8), "VSINT write-1-to-clear");
    /* SIGNAL with SIGMSK clear */
    uint64_t imr = 0x7E00;               /* unmask SIGNAL (bit 8) */
    gs_priv_access(0x12001010, &imr, 8, 1);
    W(SIGNAL, 0x12345678ull | 0xFFFF0000ull << 32);
    gs_priv_access(0x12001000, &v, 8, 0);
    uint64_t sl = 0;
    gs_priv_access(0x12001080, &sl, 8, 0);
    CHECK((v & 1) && intc_count[INTC_GS] == before + 1, "SIGNAL event + interrupt");
    CHECK((uint32_t)sl == 0x12340000, "SIGLBLID.SIGID %llX", (unsigned long long)sl);
    W(LABEL, 0xABCDull | 0xFFFFull << 32);
    gs_priv_access(0x12001080, &sl, 8, 0);
    CHECK((sl >> 32) == 0xABCD, "LBLID %llX", (unsigned long long)sl);
    W(FINISH, 0);
    gs_priv_access(0x12001000, &v, 8, 0);
    CHECK((v & 2) && intc_count[INTC_GS] == before + 1, "FINISH masked: event only");
    imr = 0x7C00;                        /* unmask FINISH too: pending -> interrupt */
    gs_priv_access(0x12001010, &imr, 8, 1);
    CHECK(intc_count[INTC_GS] == before + 2, "unmasking a pending event interrupts");
    w = 3;
    gs_priv_access(0x12001000, &w, 4, 1);
    gs_priv_access(0x12001000, &v, 8, 0);
    CHECK(!(v & 3), "SIGNAL/FINISH cleared");
    imr = 0x7F00;
    gs_priv_access(0x12001010, &imr, 8, 1);
    uint64_t rd = 0;
    gs_priv_access(0x12001010, &rd, 8, 0);
    CHECK(rd == 0x7F00, "IMR readback");
    /* partial writes to a privileged register (sdl/sdr style) */
    uint64_t d = 0x1122334455667788ull;
    gs_priv_access(0x120000A0, &d, 8, 1);
    uint32_t hi = 0xAABBCCDD;
    gs_priv_access(0x120000A4, &hi, 4, 1);
    gs_priv_access(0x120000A0, &rd, 8, 0);
    CHECK(rd == 0xAABBCCDD55667788ull, "partial write %llX", (unsigned long long)rd);
}

static void t_display(void) {
    /* Sly 2's mode: NTSC interlaced field mode, circuit 2, 512x448 from a 512x224 buffer at page 56 */
    reset_env();
    frame(56, 8, PSMCT32, 0);
    scissor(0, 511, 0, 223);
    prim(P_SPRITE, 0); rgba(0, 0, 0, 0x80); xyz(0, 0, 0); xyz(512, 224, 0);
    prim(P_TRI, IIP);
    rgba(255, 0, 0, 0x80); xyz(256, 10, 0);
    rgba(0, 255, 0, 0x80); xyz(500, 214, 0);
    rgba(0, 0, 255, 0x80); xyz(12, 214, 0);
    sprite(0, 0, 4, 4, 255, 255, 255, 0x80);
    uint64_t v;
    v = 0x66; gs_priv_access(0x12000000, &v, 8, 1);      /* PMODE: EN2, CRTMD=1, MMOD, AMOD */
    v = 3; gs_priv_access(0x12000020, &v, 8, 1);         /* SMODE2: INT | FFMD */
    v = 0x1038; gs_priv_access(0x12000090, &v, 8, 1);    /* DISPFB2 */
    v = 0x1BF9FF0203227Cull; gs_priv_access(0x120000A0, &v, 8, 1);   /* DISPLAY2 */
    static uint32_t out[1024 * 1024];
    int w = 0, h = 0;
    int on = gs_display(out, 1024, &w, &h);
    CHECK(on && w == 512 && h == 448, "display size %dx%d", w, h);
    CHECK(out[0] == 0xFFFFFF && out[1024 * 1 + 1] == 0xFFFFFF && out[1024 * 7 + 3] == 0xFFFFFF && out[1024 * 8 + 3] == 0,
          "field mode line doubling");
    uint32_t want = px(256, 100);
    CHECK(out[1024 * 200 + 256] == (((want & 0xFF) << 16) | (want & 0xFF00) | ((want >> 16) & 0xFF)) &&
          out[1024 * 201 + 256] == out[1024 * 200 + 256], "BGRX conversion");
    char path[512];
    snprintf(path, sizeof path, "%s/display.png", outdir);
    debug_write_png(path, out, w, h, 1024);
    /* frame mode shows the whole buffer: line y comes from buffer line y */
    v = 1; gs_priv_access(0x12000020, &v, 8, 1);
    gs_display(out, 1024, &w, &h);
    CHECK(out[1024 * 3 + 3] == 0xFFFFFF && out[1024 * 4 + 3] == 0 && h == 448, "frame mode");
    /* both circuits, 50% merge with ALP = 0x80 (PMODE.MMOD = 1: the ALP register) over a white
     * circuit-1 image */
    frame(200, 8, PSMCT32, 0);
    prim(P_SPRITE, 0); rgba(255, 255, 255, 0x00); xyz(0, 0, 0); xyz(512, 224, 0);   /* pixel alpha unused */
    v = 0x8027; gs_priv_access(0x12000000, &v, 8, 1);    /* EN1 | EN2, MMOD = 1, ALP = 0x80 */
    v = 0x1000 | 200; gs_priv_access(0x12000070, &v, 8, 1);   /* DISPFB1 at page 200 */
    v = 0x1BF9FF0203227Cull; gs_priv_access(0x12000080, &v, 8, 1);
    v = 3; gs_priv_access(0x12000020, &v, 8, 1);
    gs_display(out, 1024, &w, &h);
    CHECK(close_to(out[1024 * 1 + 1], 0xFFFFFF & 0xFFFFFF, 0) && close_to(out[1024 * 300 + 2], 0x808080, 1),
          "merge MMOD=1 (ALP) %06X %06X", out[1024 + 1], out[1024 * 300 + 2]);
    /* MMOD = 0: the blend factor is circuit 1's pixel alpha (GS alpha, 0x80 = 1.0), ALP is unused:
     * A = 0x80 shows circuit 1 alone, A = 0x40 is a 50% merge, A = 0 shows circuit 2 */
    v = 0x0007; gs_priv_access(0x12000000, &v, 8, 1);    /* EN1 | EN2, MMOD = 0, ALP = 0 */
    prim(P_SPRITE, 0); rgba(255, 255, 255, 0x80); xyz(0, 0, 0); xyz(512, 224, 0);
    gs_display(out, 1024, &w, &h);
    CHECK(out[1024 * 300 + 2] == 0xFFFFFF, "merge MMOD=0, circuit-1 alpha 0x80: %06X", out[1024 * 300 + 2]);
    prim(P_SPRITE, 0); rgba(255, 255, 255, 0x40); xyz(0, 0, 0); xyz(512, 224, 0);
    gs_display(out, 1024, &w, &h);
    CHECK(close_to(out[1024 * 300 + 2], 0x808080, 1), "merge MMOD=0, circuit-1 alpha 0x40: %06X", out[1024 * 300 + 2]);
    prim(P_SPRITE, 0); rgba(255, 255, 255, 0x00); xyz(0, 0, 0); xyz(512, 224, 0);
    v = 0xFF07; gs_priv_access(0x12000000, &v, 8, 1);    /* MMOD = 0 with ALP = 0xFF: still per-pixel alpha */
    gs_display(out, 1024, &w, &h);
    uint32_t c2 = gs_mem_read(PSMCT32, 56 * 32, 8, 256, 150);   /* circuit 2 behind output (256, 300) */
    CHECK((c2 & 0xFFFFFF) && out[1024 * 300 + 256] == (((c2 & 0xFF) << 16) | (c2 & 0xFF00) | ((c2 >> 16) & 0xFF)),
          "merge MMOD=0, circuit-1 alpha 0: circuit 2 shows, %06X", out[1024 * 300 + 256]);
    v = 0; gs_priv_access(0x12000000, &v, 8, 1);
    CHECK(gs_display(out, 1024, &w, &h) == 0, "display off");
}

/* Render-to-texture chains with nothing read back in between: in the async variant every step is
 * still being drawn (deferred) when the next one samples it, so this checks the hazard handling of
 * gs_draw_pixel.c / gs_draw_tex.c. The texture is declared 1024x1024 over a 64x64 buffer, as games
 * do with frame buffers, so it also covers pages that are written without touching its texels. */
static void t_deferred_hazards(void) {
    reset_env();
    const uint32_t A = 200;                        /* 64x64 source buffer at page 200 (FBW 1) */
    const uint64_t tA = tex0(A * 32, 1, PSMCT32, 10, 10, 1, 1, 0, 0, 0, 0);
    frame(0, 10, PSMCT32, 0);
    clear(0, 0, 0, 0);
    frame(A, 1, PSMCT32, 0);
    sprite(0, 0, 64, 64, 255, 0, 0, 0x80);         /* A: red */
    frame(0, 10, PSMCT32, 0);
    W(TEX0_1, tA);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(0, 0, 0); uv(64 * 16, 64 * 16); xyz(64, 64, 0);          /* red */
    frame(A, 1, PSMCT32, 0);
    sprite(0, 0, 64, 32, 0, 255, 0, 0x80);         /* A: green top half */
    frame(0, 10, PSMCT32, 0);
    W(TEX0_1, tA);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(100, 0, 0); uv(64 * 16, 64 * 16); xyz(164, 64, 0);      /* green / red */
    frame(A + 5, 1, PSMCT32, 0);                   /* texture rows 160-191: not sampled below */
    sprite(0, 0, 64, 32, 0, 0, 255, 0x80);
    frame(0, 10, PSMCT32, 0);
    W(TEX0_1, tA);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(200, 0, 0); uv(64 * 16, 64 * 16); xyz(264, 64, 0);      /* green / red */
    frame(A, 1, PSMCT32, 0);                       /* feedback: A's top half copied onto its bottom half */
    W(TEX0_1, tA);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(0, 32, 0); uv(64 * 16, 32 * 16); xyz(64, 64, 0);
    frame(0, 10, PSMCT32, 0);
    W(TEX0_1, tA);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(300, 0, 0); uv(64 * 16, 64 * 16); xyz(364, 64, 0);      /* green / green */
    CHECK(rgb(px(10, 10)) == 0x0000FF && rgb(px(10, 50)) == 0x0000FF, "sampled red: %08X %08X", px(10, 10), px(10, 50));
    CHECK(rgb(px(110, 10)) == 0x00FF00 && rgb(px(110, 50)) == 0x0000FF, "after drawing into it: %08X %08X", px(110, 10), px(110, 50));
    CHECK(rgb(px(210, 10)) == 0x00FF00 && rgb(px(210, 50)) == 0x0000FF, "after drawing elsewhere in its pages: %08X %08X", px(210, 10), px(210, 50));
    CHECK(rgb(px(310, 10)) == 0x00FF00 && rgb(px(310, 50)) == 0x00FF00, "after feedback drawing: %08X %08X", px(310, 10), px(310, 50));
}

/* An upload into one page of an indexed texture drops only that page's tiles; a palette change
 * afterwards must still reach the tiles of the other page that hold an index whose probe position
 * was in the dropped page. */
static void t_partial_invalidation_clut(void) {
    reset_env();
    clear(0, 0, 0, 0);
    static uint8_t idx[256 * 64];
    memset(idx, 7, sizeof idx);
    upload(TEXBP, 4, PSMT8, 0, 0, 256, 64, idx, sizeof idx);       /* two T8 pages side by side */
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0x80000000u | (uint32_t)i;
    pal[7] = 0x800000FFu; pal[9] = 0x8000FF00u;
    upload_clut8(CLUTBP, pal);
    const uint64_t t = tex0(TEXBP, 4, PSMT8, 8, 6, 1, 1 /*DECAL*/, CLUTBP, PSMCT32, 0, 1);
    W(TEX0_1, t);
    prim(P_SPRITE, TME | FST);
    rgba(128, 128, 128, 128);
    uv(0, 0); xyz(0, 100, 0); uv(256 * 16, 64 * 16); xyz(256, 164, 0);
    memset(idx, 9, 128 * 64);
    upload(TEXBP, 4, PSMT8, 0, 0, 128, 64, idx, 128 * 64);           /* left page only */
    W(TEX0_1, t);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(0, 200, 0); uv(256 * 16, 64 * 16); xyz(256, 264, 0);
    pal[7] = 0x80FF0000u;
    upload_clut8(CLUTBP, pal);
    W(TEX0_1, t);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(0, 300, 0); uv(256 * 16, 64 * 16); xyz(256, 364, 0);
    CHECK(rgb(px(10, 110)) == 0x0000FF && rgb(px(200, 110)) == 0x0000FF, "before: %08X %08X", px(10, 110), px(200, 110));
    CHECK(rgb(px(10, 210)) == 0x00FF00 && rgb(px(200, 210)) == 0x0000FF, "after the upload: %08X %08X", px(10, 210), px(200, 210));
    CHECK(rgb(px(10, 310)) == 0x00FF00 && rgb(px(200, 310)) == 0xFF0000, "after the CLUT change: %08X %08X", px(10, 310), px(200, 310));
}

/* ---- benchmark --------------------------------------------------------------------------------- */

static double now(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void bench(void) {
    reset_env();
    frame(0, 10, PSMCT32, 0);
    static uint8_t idx[256 * 256];
    for (int i = 0; i < 256 * 256; i++) idx[i] = (uint8_t)(i * 7 + (i >> 8) * 3);
    upload(TEXBP, 4, PSMT8, 0, 0, 256, 256, idx, sizeof idx);
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0x80000000u | (uint32_t)i * 0x010203u;
    upload_clut8(CLUTBP, pal);
    W(TEX0_1, tex0(TEXBP, 4, PSMT8, 8, 8, 1, 0, CLUTBP, PSMCT32, 0, 1));
    alpha_reg(0, 1, 0, 1, 0);
    zbuf(160, PSMZ32, 0);
    sprite(0, 0, 640, 448, 0, 0, 0, 0);  /* Z = 0 */

    static const struct { const char* name; int flags, tex1, z; } cfg[] = {
        { "flat fill            ", 0, 0, 0 },
        { "T8 point             ", TME | FST, 0, 0 },
        { "T8 bilinear          ", TME | FST, 1 << 5 | 1 << 6, 0 },
        { "T8 bilinear + blend  ", TME | FST | ABE, 1 << 5 | 1 << 6, 0 },
        { "T8 bilinear+blend+Z  ", TME | FST | ABE, 1 << 5 | 1 << 6, 1 },
        { "T8 STQ bilinear      ", TME, 1 << 5 | 1 << 6, 0 },
    };
    for (unsigned c = 0; c < sizeof cfg / sizeof cfg[0]; c++) {
        W(TEX1_1, (uint64_t)cfg[c].tex1);
        test_reg(0, 1, 0, 0, 0, 0, 1, cfg[c].z ? 2 : 1);
        zbuf(160, PSMZ32, !cfg[c].z);
        int N = 40;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            prim(P_SPRITE, cfg[c].flags);
            rgbaq(128, 128, 128, 0x60, 1.0f);
            uv(0, 0); st(0, 0); xyz(0, 0, 1);
            uv(256 * 16, 256 * 16); st(1, 1); xyz(640, 448, 1);
        }
        gs_sync();
        double t1 = now();
        fprintf(stderr, "bench: full-screen sprites, %s %5.2f ns/pixel\n", cfg[c].name, (t1 - t0) * 1e9 / (N * 640.0 * 448.0));
    }

    /* many small Gouraud textured Z triangles as one triangle list (about 200 pixels each) */
    zbuf(160, PSMZ32, 0);
    test_reg(0, 1, 0, 0, 0, 0, 1, 2);
    W(TEX1_1, 1 << 5 | 1 << 6);
    double t0 = now();
    int tris = 0;
    srand(1);
    prim(P_TRI, IIP | TME | FST);
    for (int i = 0; i < 20000; i++) {
        double cx = 20 + (i % 30) * 20, cy = 20 + (i / 30 % 20) * 20;
        for (int k = 0; k < 3; k++) {
            double x = cx + (k == 1 ? 20 : 0) + (rand() % 5), y = cy + (k == 2 ? 20 : 0) + (rand() % 5);
            rgba(rand() & 255, rand() & 255, rand() & 255, 0x80);
            uv((int)(x * 4) % 4096, (int)(y * 4) % 4096);
            xyz(x, y, (uint32_t)(i + 1));
        }
        tris++;
    }
    gs_sync();
    double t1 = now();
    fprintf(stderr, "bench: %d small textured Z triangles: %.2f us each\n", tris, (t1 - t0) * 1e6 / tris);
    /* per-triangle overhead: the same triangles shrunk to about 2 pixels */
    t0 = now();
    prim(P_TRI, IIP | TME | FST);
    for (int i = 0; i < 20000; i++) {
        double cx = 20 + (i % 30) * 20, cy = 20 + (i / 30 % 20) * 20;
        for (int k = 0; k < 3; k++) {
            rgba(k * 50, 0, 0, 0x80);
            uv((int)(cx * 4) % 4096, (int)(cy * 4) % 4096);
            xyz(cx + (k == 1 ? 2 : 0), cy + (k == 2 ? 2 : 0), (uint32_t)(i + 1));
        }
    }
    gs_sync();
    t1 = now();
    fprintf(stderr, "bench: 20000 tiny (2-pixel) textured Z triangles: %.2f us each\n", (t1 - t0) * 1e6 / 20000);

    /* texture re-upload + decode: a 256x256 T8 texture uploaded again before every 64x64 sprite */
    zbuf(160, PSMZ32, 1);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    W(TEX1_1, 0);
    double tu = 0, td = 0;
    for (int i = 0; i < 20; i++) {
        double a = now();
        upload(TEXBP, 4, PSMT8, 0, 0, 256, 256, idx, sizeof idx);
        W(TEX0_1, tex0(TEXBP, 4, PSMT8, 8, 8, 1, 0, CLUTBP, PSMCT32, 0, 1));
        gs_sync();
        double b = now();
        prim(P_SPRITE, TME | FST);
        rgba(128, 128, 128, 0x80);
        uv(0, 0); xyz(0, 0, 0); uv(256 * 16, 256 * 16); xyz(64, 64, 0);
        gs_sync();
        double c = now();
        tu += b - a; td += c - b;
    }
    fprintf(stderr, "bench: 256x256 T8 re-upload %.0f us (gs_mem), first draw incl. decode %.0f us\n", tu * 1e6 / 20, td * 1e6 / 20);

    /* display readout as at every VBlank: Sly 2's mode (512x448 from a field buffer) */
    uint64_t v;
    v = 0x8066; gs_priv_access(0x12000000, &v, 8, 1);
    v = 3; gs_priv_access(0x12000020, &v, 8, 1);
    v = 0x1038; gs_priv_access(0x12000090, &v, 8, 1);
    v = 0x1BF9FF0203227Cull; gs_priv_access(0x120000A0, &v, 8, 1);
    static uint32_t outimg[1024 * 1024];
    int w, h;
    t0 = now();
    for (int i = 0; i < 50; i++) gs_display(outimg, 1024, &w, &h);
    t1 = now();
    fprintf(stderr, "bench: gs_display %dx%d: %.0f us per frame\n", w, h, (t1 - t0) * 1e6 / 50);
    test_reg(0, 1, 0, 0, 0, 0, 1, 1);
    zbuf(160, PSMZ32, 1);
    W(TEX1_1, 0);
    save("bench", 0, 10, PSMCT32, 640, 448);
}

/* REGION_CLAMP / REGION_REPEAT with point sampling on a texture whose texel encodes its position */
static void t_region_clamp(void) {
    reset_env();
    clear(0, 0, 0, 0);
    static uint32_t tex[16 * 16];
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) tex[y * 16 + x] = 0x80000000u | (uint32_t)x | (uint32_t)y << 8;
    upload(TEXBP, 1, PSMCT32, 0, 0, 16, 16, tex, sizeof tex);
    W(TEX0_1, tex0(TEXBP, 1, PSMCT32, 4, 4, 1, 1, 0, 0, 0, 0));
    /* REGION_CLAMP u in [4, 7], v in [2, 5] */
    W(CLAMP_1, 2 | 2 << 2 | 4ull << 4 | 7ull << 14 | 2ull << 24 | 5ull << 34);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(10, 10, 0); uv(256, 256); xyz(26, 26, 0);
    int bad = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int u = x < 4 ? 4 : x > 7 ? 7 : x, v = y < 2 ? 2 : y > 5 ? 5 : y;
            bad += rgb(px(10 + x, 10 + y)) != (uint32_t)(u | v << 8);
        }
    CHECK(bad == 0, "REGION_CLAMP: %d wrong", bad);
    /* REGION_REPEAT u = (u & 3) | 8, v = (v & 1) | 4 over a 32x32 sprite (u up to 31: wraps) */
    W(CLAMP_1, 3 | 3 << 2 | 3ull << 4 | 8ull << 14 | 1ull << 24 | 4ull << 34);
    prim(P_SPRITE, TME | FST);
    uv(0, 0); xyz(40, 10, 0); uv(512, 512); xyz(72, 42, 0);
    bad = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) bad += rgb(px(40 + x, 10 + y)) != (uint32_t)((((x & 15) & 3) | 8) | (((y & 15) & 1) | 4) << 8);
    CHECK(bad == 0, "REGION_REPEAT: %d wrong (%06X)", bad, rgb(px(41, 10)));
    /* CLAMP vs REPEAT outside [0, 16) */
    W(CLAMP_1, 1 | 0 << 2);
    prim(P_SPRITE, TME | FST);
    uv(-0, 0); xyz(80, 10, 0); uv(512, 256); xyz(112, 26, 0);
    CHECK(rgb(px(80 + 20, 12)) == (15u | 2u << 8) && rgb(px(80 + 3, 12)) == (3u | 2u << 8), "CLAMP u beyond the texture");
    W(CLAMP_1, 0);
    save("region", 0, 10, PSMCT32, 640, 448);
}

static void t_formats_contexts(void) {
    reset_env();
    clear(0, 0, 0, 0xAB);
    /* PSMCT24 frame: RGB written, the top byte (alpha/T8H data) untouched */
    frame(0, 10, PSMCT24, 0);
    sprite(10, 10, 20, 20, 1, 2, 3, 0x40);
    frame(0, 10, PSMCT32, 0);
    CHECK(px(15, 15) == 0xAB030201, "CT24 keeps the top byte: %08X", px(15, 15));
    /* CT24 frame with blending: destination alpha reads as 1.0 */
    frame(0, 10, PSMCT24, 0);
    alpha_reg(1, 2, 1, 2, 0);            /* Cd * Ad -> Cd */
    prim(P_SPRITE, ABE); rgba(200, 200, 200, 0x80); xyz(10, 10, 0); xyz(20, 20, 0);
    frame(0, 10, PSMCT32, 0);
    CHECK(px(15, 15) == 0xAB030201, "CT24 Ad = 1.0: %08X", px(15, 15));
    /* PSMCT16S frame */
    frame(400, 10, PSMCT16S, 0);
    sprite(0, 0, 64, 64, 0, 0, 0, 0);
    sprite(5, 6, 9, 9, 0xF8, 0, 0xF8, 0x80);
    CHECK(px(5, 6) == (0x1F | 0x1F << 10 | 0x8000) && px(4, 6) == 0 && px(9, 8) == 0, "CT16S frame %04X", px(5, 6));
    frame(0, 10, PSMCT32, 0);
    /* context 2: its own FRAME/TEST/XYOFFSET/SCISSOR */
    W(FRAME_1 + 1, 200 | 10ull << 16);
    W(ZBUF_1 + 1, 1ull << 32);
    W(XYOFFSET_1 + 1, (uint64_t)(100 * 16) | (uint64_t)(100 * 16) << 32);
    W(SCISSOR_1 + 1, 639ull << 16 | 447ull << 48);
    W(TEST_1 + 1, 1ull << 16 | 1ull << 17);
    W(ALPHA_1 + 1, 0); W(FBA_1 + 1, 0); W(TEX0_1 + 1, 0);
    prim(P_SPRITE, CTXT2); rgba(9, 8, 7, 0x80);
    xyz(110, 110, 0); xyz(120, 120, 0);
    CHECK(gs_mem_read(PSMCT32, 200 * 32, 10, 15, 15) == 0x80070809 && rgb(px(15, 15)) == 0x030201, "context 2");
    /* PRMODECONT = 0: attributes come from PRMODE (ABE here), PRIM only gives the type */
    clear(0, 0, 0, 0x80);
    alpha_reg(0, 2, 2, 1, 0x80);         /* Cs + Cd */
    W(PRMODECONT, 0);
    W(PRMODE, ABE);
    sprite(30, 10, 40, 20, 0x10, 0, 0, 0x80);
    sprite(30, 10, 40, 20, 0x10, 0, 0, 0x80);
    W(PRMODECONT, 1);
    CHECK(chan(px(35, 15), 0) == 0x20, "PRMODE ABE: %08X", px(35, 15));
    /* CSR RESET drops a half-sent primitive */
    prim(P_SPRITE, 0); rgba(255, 255, 255, 0x80);
    xyz(50, 10, 0);
    uint32_t reset = 1u << 9;
    gs_priv_access(0x12001000, &reset, 4, 1);
    xyz(60, 20, 0);
    CHECK(rgb(px(55, 15)) == 0, "CSR RESET clears the vertex queue");
    /* CT16 display */
    reset_env();
    frame(400, 8, PSMCT16, 0);
    sprite(0, 0, 8, 8, 0xF8, 0x80, 0x00, 0x80);
    uint64_t v;
    v = 0x62; gs_priv_access(0x12000000, &v, 8, 1);
    v = 0; gs_priv_access(0x12000020, &v, 8, 1);
    v = 400 | 8 << 9 | (uint64_t)PSMCT16 << 15; gs_priv_access(0x12000090, &v, 8, 1);
    v = 0x1BF9FF0203227Cull; gs_priv_access(0x120000A0, &v, 8, 1);
    static uint32_t out[1024 * 1024];
    int w, h;
    gs_display(out, 1024, &w, &h);
    CHECK(out[1024 * 3 + 3] == 0xF88000, "CT16 display %06X", out[1024 * 3 + 3]);
    frame(0, 10, PSMCT32, 0);
}

/* Random triangles (fractional and integer vertices, XYOFFSET, scissor) against a per-pixel
 * evaluation of the three edge functions with the top-left rule. */
/* STQ texture coordinates must be floored (not truncated toward zero) when converted to texels:
 * with REPEAT, sampling is periodic across u = 0. A 16-texel-wide texel ramp mapped 1:1 onto a
 * triangle row with u = x - 100.03 (q = 1): pixel x samples texel floor(x - 100.03) & 15. */
static void t_stq_negative_coords(void) {
    reset_env();
    clear(0, 0, 0, 0);
    static uint32_t tex[16 * 16];
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) tex[y * 16 + x] = 0x80000000u | (uint32_t)x | (uint32_t)y << 8;
    upload(TEXBP, 1, PSMCT32, 0, 0, 16, 16, tex, sizeof tex);
    W(TEX0_1, tex0(TEXBP, 1, PSMCT32, 4, 4, 1, 1, 0, 0, 0, 0));
    W(CLAMP_1, 0);                       /* REPEAT */
    W(TEX1_1, 0);                        /* point sampling */
    for (int pass = 0; pass < 2; pass++) {
        /* pass 0: triangle; pass 1: the same coordinates on a line along row 30 */
        double u0 = -100.03, u1 = 299.97;     /* u at x = 0 and x = 400 */
        float tv = 0.5f / 16;                 /* v = 0.5: texel row 0 */
        if (pass == 0) {
            prim(P_TRI, TME);
            rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f); st((float)(u0 / 16), tv); xyz(0, 0, 0);
            rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f); st((float)(u1 / 16), tv); xyz(400, 0, 0);
            rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f); st((float)(u0 / 16), tv); xyz(0, 40, 0);
        } else {
            prim(P_LINE, TME);
            rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f); st((float)(u0 / 16), tv); xyz(0, 30, 0);
            rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f); st((float)(u1 / 16), tv); xyz(400, 30, 0);
        }
        int y = pass == 0 ? 5 : 30, bad = 0, first = -1;
        for (int x = 20; x < 300; x++) {
            int want = (int)floor(x - 100.03) & 15;
            if (rgb(px(x, y)) != (uint32_t)want) { bad++; if (first < 0) first = x; }
        }
        CHECK(bad == 0, "STQ %s, negative u: %d pixels sample the wrong texel (first at x=%d: %06X, want %02X)",
              pass ? "line" : "triangle", bad, first, first < 0 ? 0 : rgb(px(first, y)),
              first < 0 ? 0 : (int)floor(first - 100.03) & 15);
    }
    save("stq_negative", 0, 10, PSMCT32, 640, 448);
}

static uint32_t rng_state;
static int rnd(int n) { rng_state = rng_state * 1664525u + 1013904223u; return (int)((rng_state >> 9) % (uint32_t)n); }
static double rndf(void) { return rnd(65536) / 65536.0; }

static void t_coverage_reference(void) {
    reset_env();
    rng_state = 99;
    int bad = 0;
    for (int it = 0; it < 300; it++) {
        int ox = (100 + rnd(64)) * 16, oy = (100 + rnd(64)) * 16;
        ox += rnd(16); oy += rnd(16);
        int sx0 = rnd(40), sx1 = 200 + rnd(40), sy0 = rnd(40), sy1 = 160 + rnd(40);
        W(XYOFFSET_1, 0);
        scissor(0, 639, 0, 447);
        clear(0, 0, 0, 0);
        W(XYOFFSET_1, (uint64_t)ox | (uint64_t)oy << 32);
        scissor(sx0, sx1, sy0, sy1);
        int64_t x[3], y[3];
        int kind = rnd(4);
        for (int k = 0; k < 3; k++) {
            if (kind == 0) { x[k] = rnd(280 * 16) - 20 * 16; y[k] = rnd(230 * 16) - 15 * 16; }
            else if (kind == 1) { x[k] = 100 * 16 + rnd(64); y[k] = 100 * 16 + rnd(64); }     /* tiny */
            else { x[k] = rnd(26) * 160; y[k] = rnd(22) * 160; }                             /* many ties */
        }
        if (kind == 3) {                 /* axis-aligned right triangle, any orientation, any fraction */
            int fx = rnd(16), fy = rnd(16), w = (rnd(2) ? 1 : -1) * (16 + rnd(1600)), h = (rnd(2) ? 1 : -1) * (16 + rnd(1600));
            x[0] = 1200 + fx; y[0] = 1200 + fy;
            x[1] = x[0] + w; y[1] = y[0];
            x[2] = x[0]; y[2] = y[0] + h;
        }
        prim(P_TRI, 0); rgba(255, 255, 255, 0x80);
        for (int k = 0; k < 3; k++) W(XYZ2, (uint64_t)(x[k] + ox) | (uint64_t)(y[k] + oy) << 16);
        int64_t area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
        for (int py = 0; py < 240; py++)
            for (int pxx = 0; pxx < 280; pxx++) {
                int in = area != 0 && pxx >= sx0 && pxx <= sx1 && py >= sy0 && py <= sy1;
                for (int k = 0; k < 3 && in; k++) {
                    int64_t ex = x[(k + 1) % 3] - x[k], ey = y[(k + 1) % 3] - y[k];
                    if (area < 0) { ex = -ex; ey = -ey; }
                    int64_t e = ex * (16 * py - y[k]) - ey * (16 * pxx - x[k]);
                    int top_left = ey < 0 || (ey == 0 && ex > 0);
                    if (e < 0 || (e == 0 && !top_left)) in = 0;
                }
                if (in != (rgb(px(pxx, py)) != 0)) bad++;
            }
    }
    CHECK(bad == 0, "triangle coverage vs edge-function reference: %d pixels differ", bad);
    W(XYOFFSET_1, 0);
    scissor(0, 639, 0, 447);
}

/* Texture cache invalidation under random primitives: every re-upload must make all later samples
 * see the new texels (an undecoded tile would still hold the previous colour). */
static void t_texture_cache_random(void) {
    reset_env();
    static uint32_t img[256 * 128];
    rng_state = 5;
    int bad = 0, drawn = 0;
    for (int it = 0; it < 300; it++) {
        uint32_t cur = (it & 1) ? 0x8000FF00u : 0x800000FFu;
        for (int i = 0; i < 256 * 128; i++) img[i] = cur;
        upload(TEXBP, 4, PSMCT32, 0, 0, 256, 128, img, sizeof img);
        W(TEX0_1, tex0(TEXBP, 4, PSMCT32, 8, 7, 1, 1, 0, 0, 0, 0));
        uint64_t clamp = (uint64_t)rnd(4);
        clamp |= (uint64_t)rnd(4) << 2;
        clamp |= (uint64_t)rnd(256) << 4;
        clamp |= (uint64_t)rnd(256) << 14;
        clamp |= (uint64_t)rnd(128) << 24;
        clamp |= (uint64_t)rnd(128) << 34;
        W(CLAMP_1, clamp);
        int mmag = rnd(2), mmin = rnd(2);
        W(TEX1_1, (uint64_t)mmag << 5 | (uint64_t)mmin << 6);
        clear(0, 0, 0, 0);
        int type = rnd(4), fst = rnd(2), nv = type == 0 ? 3 : type == 3 ? 1 : 2;
        prim(type == 0 ? P_TRI : type == 1 ? P_SPRITE : type == 2 ? P_LINE : P_POINT, TME | (fst ? FST : 0));
        for (int v = 0; v < nv; v++) {
            float q = (float)(0.05 + rndf() * 2);
            double u = rndf() * 600 - 100, t = rndf() * 400 - 100;
            double sx = rndf() * 640, sy = rndf() * 448;
            rgbaq(0x80, 0x80, 0x80, 0x80, q);
            st((float)(u / 256 * q), (float)(t / 128 * q));
            uv(u < 0 ? 0 : (int)(u * 16), t < 0 ? 0 : (int)(t * 16));
            xyz(sx, sy, 0);
        }
        for (int y = 0; y < 448; y++)
            for (int x = 0; x < 640; x++) {
                uint32_t c = px(x, y);
                bad += c != 0 && c != cur;
                drawn += c == cur;
            }
    }
    CHECK(bad == 0 && drawn > 100000, "texture re-uploads: %d stale texels (%d textured pixels)", bad, drawn);
    W(TEX1_1, 0); W(CLAMP_1, 0);
}

int main(int argc, char** argv) {
    if (argc > 1) outdir = argv[1];
    gs_swz_init();
#ifdef TEST_ASYNC
    gs_thread_start(3, NULL);
    CHECK(gs_async() && gs_draw_workers() == 3, "GS thread and 3 pixel workers running");
#endif
#ifdef FAKE_LINEAR
    CHECK(!gs_swz_fast, "linear fake layout must disable the fast path");
#else
    CHECK(gs_swz_fast, "documented layout must enable the fast path");
#endif
    t_sprite_coverage();
    t_fill_rule();
    t_gouraud();
    t_clut_textures();
    t_bilinear();
    t_blend();
    t_ztest(PSMZ32);
    t_ztest(PSMZ24);
    t_ztest(PSMZ16);
    t_ztest(PSMZ16S);
    t_region_clamp();
    t_formats_contexts();
    t_scissor_offset();
    t_alpha_tests();
    t_fog_tfx();
    t_frame16();
    t_perspective();
    t_mipmap();
    t_texture_cache();
    t_lines_points();
    t_coverage_reference();
    t_texture_cache_random();
    t_stq_negative_coords();
    t_csr();
    t_display();
    t_deferred_hazards();
    t_partial_invalidation_clut();
    bench();
    fprintf(stderr, "%s: %d/%d checks passed\n", argv[0], checks - fails, checks);
    return fails ? 1 : 0;
}
