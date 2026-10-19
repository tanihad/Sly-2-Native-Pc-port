/* Unit tests for the GPU frontend (runtime/gpu/gpu_front.c, gpu_own.c, gpu_clut.c; design M1):
 * synthetic GS scenes go through gs_write_reg with the frontend hooked next to the software
 * rasterizer, and the per-frame counters must show the classification of notes/d3d11-renderer.md
 * 4-6 (tiers, ROV reasons, ALIAS16 batching, clears, pass-A split, uploads, targets, feedback).
 * Also the shared setup code: gs_llround against the C library. No GPU is needed. */
#include "gs_setup.h"
#include "gpu/gpu_front.h"

#include <math.h>
#include <stdlib.h>
#include <windows.h>

void intc_raise(int cause) { (void)cause; }
void dmac_raise(int ch) { (void)ch; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

enum { PRIM = 0x00, RGBAQ = 0x01, UV = 0x03, XYZ2 = 0x05, TEX0_1 = 0x06, CLAMP_1 = 0x08, TEX1_1 = 0x14,
       XYOFFSET_1 = 0x18, PRMODECONT = 0x1A, TEXA = 0x3B, SCISSOR_1 = 0x40, ALPHA_1 = 0x42, COLCLAMP = 0x46,
       TEST_1 = 0x47, FBA_1 = 0x4A, FRAME_1 = 0x4C, ZBUF_1 = 0x4E, BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52,
       TRXDIR = 0x53 };
enum { P_POINT, P_LINE, P_LINESTRIP, P_TRI, P_TRISTRIP, P_TRIFAN, P_SPRITE };
#define IIP (1 << 3)
#define TME (1 << 4)
#define ABE (1 << 6)
#define AA1 (1 << 7)
#define FST (1 << 8)

static void W(uint32_t r, uint64_t v) { gs_write_reg(r, v); }
static void frame(uint32_t fbp, uint32_t fbw, int psm, uint32_t fbmsk) {
    W(FRAME_1, fbp | ((uint64_t)fbw << 16) | ((uint64_t)psm << 24) | ((uint64_t)fbmsk << 32));
}
static void zbuf(uint32_t zbp, int psm, int zmsk) { W(ZBUF_1, zbp | ((uint64_t)(psm & 0xF) << 24) | ((uint64_t)zmsk << 32)); }
static void test_reg(int ate, int atst, int aref, int afail, int date, int zte, int ztst) {
    W(TEST_1, (uint64_t)ate | (uint64_t)atst << 1 | (uint64_t)aref << 4 | (uint64_t)afail << 12 |
              (uint64_t)date << 14 | (uint64_t)zte << 16 | (uint64_t)ztst << 17);
}
static void alpha_reg(int a, int b, int c, int d, int fix) {
    W(ALPHA_1, (uint64_t)a | (uint64_t)b << 2 | (uint64_t)c << 4 | (uint64_t)d << 6 | (uint64_t)fix << 32);
}
static void scissor(int x0, int x1, int y0, int y1) {
    W(SCISSOR_1, (uint64_t)x0 | (uint64_t)x1 << 16 | (uint64_t)y0 << 32 | (uint64_t)y1 << 48);
}
static void rgba(int r, int g, int b, int a) { W(RGBAQ, (uint64_t)r | (uint64_t)g << 8 | (uint64_t)b << 16 | (uint64_t)a << 24 | 0x3F800000ull << 32); }
static void xyz(int x, int y, uint32_t z) { W(XYZ2, (uint64_t)(x * 16) | (uint64_t)(y * 16) << 16 | (uint64_t)z << 32); }
static void uv(int u, int v) { W(UV, (uint64_t)(u * 16) | (uint64_t)(v * 16) << 16); }
static void tex0(uint32_t tbp, uint32_t tbw, int psm, int tw, int th, int tcc, int tfx, uint32_t cbp, int cld) {
    W(TEX0_1, (uint64_t)tbp | (uint64_t)tbw << 14 | (uint64_t)psm << 20 | (uint64_t)tw << 26 | (uint64_t)th << 30 |
              (uint64_t)tcc << 34 | (uint64_t)tfx << 35 | (uint64_t)cbp << 37 | (uint64_t)cld << 61);
}
static void sprite(int x0, int y0, int x1, int y1, uint32_t z) { xyz(x0, y0, z); xyz(x1, y1, z); }
static void sprite_uv(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1) {
    uv(u0, v0); xyz(x0, y0, 0); uv(u1, v1); xyz(x1, y1, 0);
}
static void upload(uint32_t dbp, uint32_t dbw, int psm, int x, int y, int w, int h, const void* data, uint32_t bytes) {
    W(BITBLTBUF, (uint64_t)dbp << 32 | (uint64_t)dbw << 48 | (uint64_t)psm << 56);
    W(TRXPOS, (uint64_t)x << 32 | (uint64_t)y << 48);
    W(TRXREG, (uint64_t)w | (uint64_t)h << 32);
    W(TRXDIR, 0);
    gs_image_write(data, bytes);
}

static uint32_t vb, selfbad;
static gpu_front_frame endf(void) {      /* close the frame, return its counters */
    gpu_front_vblank(NULL, ++vb);
    gpu_front_frame f;
    int n = 0;
    gpu_front_frame tmp[8];
    while (gpu_front_take_frames(tmp, 1) == 1) { f = tmp[0]; n++; }
    if (!n) memset(&f, 0, sizeof f);
    selfbad += f.selfcheck_bad;
    return f;
}

static void base_env(void) {
    W(PRMODECONT, 1);
    frame(0, 8, PSMCT32, 0);
    zbuf(0x70, PSMZ32, 1);
    W(XYOFFSET_1, 0);
    scissor(0, 511, 0, 223);
    test_reg(0, 1, 0, 0, 0, 1, 1);
    alpha_reg(0, 1, 0, 1, 0);
    W(COLCLAMP, 1);
    W(FBA_1, 0);
    W(TEX1_1, 0);
    W(CLAMP_1, 0);
    W(TEXA, 0x80ull << 32);
}

static void clear_frame(uint32_t fbp, uint32_t fbw, int psm) {   /* 16 opaque 32x224 sprites */
    frame(fbp, fbw, psm, 0);
    test_reg(0, 1, 0, 0, 0, 1, 1);
    W(PRIM, P_SPRITE);
    rgba(0, 0, 0, 0x80);
    for (int x = 0; x < 512; x += 32) sprite(x, 0, x + 32, 224, 0);
}

static void test_llround(void) {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    long bad = 0;
    for (long k = 0; k < 4000000; k++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        double v = (k & 1) ? ((int64_t)(s >> 11) - (1ll << 52)) * 0.5 : (double)(int64_t)s / 65536.0 / 4096.0;
        if (gs_llround(v) != llround(v)) bad++;
    }
    static const double edge[] = { 0.5, -0.5, 1.5, -2.5, 0.49999999999999994, -0.49999999999999994, 4503599627370495.5 };
    for (size_t i = 0; i < sizeof edge / sizeof *edge; i++) bad += gs_llround(edge[i]) != llround(edge[i]);
    CHECK(bad == 0, "gs_llround differs from llround on %ld values", bad);
}

int main(void) {
    gpu_front_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sw = 1;
    cfg.selfcheck = 1;
    test_llround();
    gpu_front_start(&cfg);
    base_env();
    endf();

    /* 1. ZB_ONLY Z clear (16 sprites, ATST NEVER AFAIL ZB_ONLY, ZTST ALWAYS) = a full Z clear */
    frame(0, 8, PSMCT32, 0);
    zbuf(0x70, PSMZ32, 0);
    test_reg(1, 0, 0, 2, 0, 1, 1);
    W(PRIM, P_SPRITE);
    rgba(0, 0, 0, 0);
    for (int x = 0; x < 512; x += 32) sprite(x, 0, x + 32, 224, 0);
    gpu_front_frame f = endf();
    CHECK(f.clears_z == 1 && f.clears_c == 0, "Z-only clear: clears_z %u clears_c %u", f.clears_z, f.clears_c);
    /* (M5: the fill's FF draw is dropped, the clear does it) */
    CHECK(f.tier[GPU_TIER_FF] == 16 && f.d3d_draws == 0 && f.fill_draws_dropped == 1, "Z clear: FF %u, D3D draws %u, dropped %u",
          f.tier[GPU_TIER_FF], f.d3d_draws, f.fill_draws_dropped);
    CHECK(f.loads == 0, "a full overwrite needs no load (%u)", f.loads);

    /* 2. a colour clear, then a shadow volume: G-channel wrapping subtract with DATE -> ROV */
    clear_frame(0, 8, PSMCT32);
    frame(0, 8, PSMCT32, 0xFFFF00FFu);
    zbuf(0x70, PSMZ32, 1);
    alpha_reg(2, 0, 2, 1, 128);                          /* (0 - Cs) * FIX + Cd */
    W(COLCLAMP, 0);
    test_reg(1, 0, 0, 1, 1, 1, 3);                       /* ATST NEVER FB_ONLY, DATE, ZTST GREATER */
    W(PRIM, P_TRISTRIP | ABE);
    rgba(0, 2, 0, 0x80);
    xyz(10, 10, 100); xyz(100, 10, 100); xyz(10, 100, 100); xyz(100, 100, 100);
    W(COLCLAMP, 1);
    f = endf();
    CHECK(f.clears_c == 1, "colour clear recognised (%u)", f.clears_c);
    CHECK(f.tier[GPU_TIER_ROV] == 2 && f.rov_why[GPU_ROV_COLCLAMP] == 2, "shadow volume on ROV: ROV %u (colclamp %u)",
          f.tier[GPU_TIER_ROV], f.rov_why[GPU_ROV_COLCLAMP]);
    CHECK(f.rov_runs == 1 && f.rov_draws == 1 && f.zresolves == 0, "one ROV run, one draw, no Z resolve: %u %u %u",
          f.rov_runs, f.rov_draws, f.zresolves);

    /* 3. ALIAS16: B (038) drawn, then 32 CT16 strips over A (000) reading B as CT16 (shuffle 1) */
    clear_frame(0x38, 8, PSMCT32);
    alpha_reg(2, 2, 2, 0, 0);                            /* (0-0)*FIX+Cs: copy */
    frame(0, 8, PSMCT16, 0x00003FFFu);
    scissor(0, 511, 0, 447);
    test_reg(0, 1, 0, 0, 0, 1, 1);
    tex0(0x38 * 32, 8, PSMCT16, 10, 10, 1, 1, 0, 0);
    W(PRIM, P_SPRITE | TME | FST | ABE);
    for (int x = 0; x < 512; x += 16) sprite_uv(x, 0, x + 16, 448, x + 8, 0, x + 24, 448);
    /* in place on A, halfword-disjoint rectangles: still one more dispatch only when a sprite reads
     * what an earlier one wrote */
    tex0(0, 8, PSMCT16, 10, 10, 1, 1, 0, 0);
    sprite_uv(0, 0, 16, 448, 16, 0, 32, 448);            /* reads x 16..31 of A: written by strip 2 above */
    f = endf();
    CHECK(f.tier[GPU_TIER_ALIAS16] == 33 && f.alias16_sprites == 33, "ALIAS16 sprites %u", f.alias16_sprites);
    CHECK(f.alias16_dispatches == 2, "ALIAS16 dispatches %u (expected 2: the 33rd reads an earlier write)", f.alias16_dispatches);
    CHECK(f.tier[GPU_TIER_PUNT] == 0, "no PUNT (%u)", f.tier[GPU_TIER_PUNT]);
    scissor(0, 511, 0, 223);

    /* 4. pass A on a T8 texture: CLUT alphas decide one draw (all pass) or the two-draw split */
    {
        uint32_t clut[256], tex[16 * 16 / 4];
        for (int i = 0; i < 256; i++) clut[i] = 0x80000000u | (uint32_t)i;
        memset(tex, 0, sizeof tex);
        ((uint8_t*)tex)[17] = 7;                         /* (M9: the bounds come from the palette entries the texels use:
                                                            entry 7 must be one of them) */
        upload(0x3000, 1, PSMCT32, 0, 0, 16, 16, clut, sizeof clut);
        upload(0x3100, 1, PSMT8, 0, 0, 16, 16, tex, sizeof tex);
        frame(0, 8, PSMCT32, 0);
        zbuf(0x70, PSMZ32, 0);
        alpha_reg(0, 1, 0, 1, 0);                        /* (Cs-Cd)*As+Cd */
        test_reg(1, 5, 126, 1, 0, 1, 3);                 /* ATST GEQUAL 126 FB_ONLY, ZTST GREATER */
        tex0(0x3100, 1, PSMT8, 4, 4, 1, 0, 0x3000, 1);
        W(PRIM, P_TRI | TME | FST | ABE);
        rgba(128, 128, 128, 128);
        uv(0, 0); xyz(0, 0, 50); uv(15, 0); xyz(40, 0, 50); uv(0, 15); xyz(0, 40, 50);
        f = endf();
        CHECK(f.atst_kicks == 1 && f.atst_pass == 1, "all CLUT alphas 0x80 with vertex 0x80: all pass (%u/%u)", f.atst_kicks, f.atst_pass);
        CHECK(f.patches == 1, "two uploads before the draw: one vram_buf patch (%u)", f.patches);
        clut[7] = 0x10000007u;                           /* one entry far below AREF -> straddle */
        upload(0x3000, 1, PSMCT32, 0, 0, 16, 16, clut, sizeof clut);
        tex0(0x3100, 1, PSMT8, 4, 4, 1, 0, 0x3000, 1);
        uv(0, 0); xyz(0, 0, 50); uv(15, 0); xyz(40, 0, 50); uv(0, 15); xyz(0, 40, 50);
        f = endf();
        CHECK(f.atst_straddle == 1 && f.split_extra == 1, "straddle -> split: %u %u", f.atst_straddle, f.split_extra);
        clut[7] = 0xF0000007u;                           /* As up to 0xF0 * 0x80 >> 7 = 240 > 128 with a lerp blend */
        upload(0x3000, 1, PSMCT32, 0, 0, 16, 16, clut, sizeof clut);
        tex0(0x3100, 1, PSMT8, 4, 4, 1, 0, 0x3000, 1);
        uv(0, 0); xyz(0, 0, 50); uv(15, 0); xyz(40, 0, 50); uv(0, 15); xyz(0, 40, 50);
        f = endf();
        CHECK(f.as_over128 == 1 && f.rov_why[GPU_ROV_C_OVER128] == 1 && f.zresolves == 1,
              "As > 128 with (Cs-Cd)*As+Cd -> ROV (+ Z resolve): %u %u %u", f.as_over128, f.rov_why[GPU_ROV_C_OVER128], f.zresolves);
    }

    /* 5. exact Z: Z32 vertices above 0x3F000000 go to the ROV tier */
    alpha_reg(0, 2, 2, 1, 128);
    test_reg(0, 1, 0, 0, 0, 1, 2);
    W(PRIM, P_TRI);
    xyz(0, 0, 0x3F000001u); xyz(30, 0, 0x3F000001u); xyz(0, 30, 0x3F000001u);
    f = endf();
    CHECK(f.z32_over == 1 && f.rov_why[GPU_ROV_Z32_HIGH] == 1, "Z32 above 0x3F000000: %u %u", f.z32_over, f.rov_why[GPU_ROV_Z32_HIGH]);
    clear_frame(0, 8, PSMCT32);                          /* (a later full Z clear drops zpatch-as-depth) */
    zbuf(0x70, PSMZ32, 0);
    test_reg(1, 0, 0, 2, 0, 1, 1);
    W(PRIM, P_SPRITE);
    for (int x = 0; x < 512; x += 32) sprite(x, 0, x + 32, 224, 0);
    endf();

    /* 6. FMV-like: 40 16x16 CT32 macroblock uploads into a TBW 12 texture, one DECAL sprite */
    {
        static uint32_t mb[16 * 16];
        for (int i = 0; i < 256; i++) mb[i] = 0xFF000000u | (uint32_t)i * 0x010101u;
        for (int k = 0; k < 40; k++) upload(0x1800, 12, PSMCT32, (k % 8) * 16, (k / 8) * 16, 16, 16, mb, sizeof mb);
        frame(0, 8, PSMCT32, 0);
        zbuf(0x70, PSMZ32, 1);
        test_reg(0, 1, 0, 0, 0, 1, 1);
        alpha_reg(0, 1, 0, 1, 0);
        W(CLAMP_1, 1 | 1 << 2);
        tex0(0x1800, 12, PSMCT32, 10, 10, 0, 1, 0, 0);
        W(PRIM, P_SPRITE | TME | FST);
        sprite_uv(0, 0, 128, 80, 0, 0, 128, 80);
        f = endf();
        CHECK(f.xfers == 40 && f.patches == 1, "40 uploads -> 1 patch (%u, %u)", f.xfers, f.patches);
        CHECK(f.tbw_npot == 1 && f.tex_vram == 1 && f.tier[GPU_TIER_FF] == 1, "TBW 12 sprite: %u %u %u", f.tbw_npot, f.tex_vram,
              f.tier[GPU_TIER_FF]);
        W(CLAMP_1, 0);
    }

    /* 7. the ep2 photo: a CT32 target at FBP 070 (the Z base) sampled with REGION_CLAMP for 200 frames */
    frame(0x70, 8, PSMCT32, 0);
    zbuf(0x70, PSMZ32, 1);
    test_reg(0, 1, 0, 0, 0, 1, 1);
    alpha_reg(0, 1, 0, 1, 0);
    W(PRIM, P_SPRITE);
    rgba(10, 20, 30, 0x80);
    sprite(0, 0, 512, 224, 0);
    endf();
    W(CLAMP_1, 2 | 2 << 2 | (uint64_t)511 << 14 | (uint64_t)223 << 34);   /* REGION_CLAMP 0..511 x 0..223 */
    frame(0, 8, PSMCT32, 0);
    tex0(0x70 * 32, 8, PSMCT32, 10, 10, 1, 1, 0, 0);
    W(PRIM, P_TRI | TME | FST);
    uint32_t views = 0, rc = 0, punts = 0;
    for (int k = 0; k < 200; k++) {
        uv(0, 0); xyz(100, 50, 0); uv(511, 0); xyz(300, 50, 0); uv(0, 223); xyz(100, 150, 0);
        f = endf();
        views += f.tex_view_c32; rc += f.region_clamp; punts += f.tier[GPU_TIER_PUNT];
    }
    CHECK(views == 200 && rc == 200 && punts == 0, "photo: %u views, %u REGION_CLAMP, %u PUNT over 200 frames", views, rc, punts);
    CHECK(f.targets_evicted == 0 && f.downloads == 0, "the sampled target stays GPU-owned (evicted %u, downloads %u)",
          f.targets_evicted, f.downloads);
    W(CLAMP_1, 0);

    /* 8. T8H darken of the bound target (alpha written too): one feedback copy for 16 strips */
    {
        uint32_t clut[256];
        for (int i = 0; i < 256; i++) clut[i] = 0x40000000u;
        upload(0x3000, 1, PSMCT32, 0, 0, 16, 16, clut, sizeof clut);
        clear_frame(0x38, 8, PSMCT32);
        frame(0x38, 8, PSMCT32, 0);
        alpha_reg(0, 1, 0, 1, 0);
        tex0(0x38 * 32, 8, PSMT8H, 10, 10, 1, 1, 0x3000, 1);
        W(PRIM, P_SPRITE | TME | FST | ABE);
        for (int x = 0; x < 512; x += 32) sprite_uv(x, 0, x + 32, 224, x, 0, x + 32, 224);
        f = endf();
        CHECK(f.tex_view_alpha == 16 && f.shadow_copies == 1, "darken: %u alpha views, %u shadow copies", f.tex_view_alpha,
              f.shadow_copies);
        /* the same with RGB-only writes (FBMSK FF000000): lanes disjoint - read in place on the ROV tier, but
         * this lerp is an FF draw (M5), which cannot sample its render target: one shadow copy */
        clear_frame(0x38, 8, PSMCT32);
        frame(0x38, 8, PSMCT32, 0xFF000000u);
        tex0(0x38 * 32, 8, PSMT8H, 10, 10, 1, 1, 0x3000, 1);
        W(PRIM, P_SPRITE | TME | FST | ABE);
        for (int x = 0; x < 512; x += 32) sprite_uv(x, 0, x + 32, 224, x, 0, x + 32, 224);
        f = endf();
        CHECK(f.shadow_copies == 1 && f.tier[GPU_TIER_ROV] == 0, "darken RGB only (FF): %u shadow copies, ROV %u", f.shadow_copies,
              f.tier[GPU_TIER_ROV]);
    }

    /* 9. AA1 outline pair: ZB_ONLY line then AA1 RGB_ONLY line, line pixel records */
    frame(0, 8, PSMCT32, 0);
    zbuf(0x70, PSMZ32, 0);
    test_reg(1, 0, 0, 2, 0, 1, 3);
    W(PRIM, P_LINE);
    rgba(0, 0, 0, 0x80);
    xyz(10, 10, 1000); xyz(60, 30, 1000);
    test_reg(1, 0, 0, 3, 0, 1, 3);
    W(PRIM, P_LINE | AA1 | ABE);
    xyz(10, 10, 1000); xyz(60, 30, 1000);
    f = endf();
    CHECK(f.cls[GPU_PC_LINEPIX] == 2 && f.linepix == 50 + 90, "outline pair: %u lines, %u line pixels (50 + 90 expected)",
          f.cls[GPU_PC_LINEPIX], f.linepix);
    CHECK(f.d3d_draws == 2 && f.tier[GPU_TIER_FF] == 2, "outline pair: %u D3D draws", f.d3d_draws);

    gpu_front_stop();
    CHECK(selfbad == 0, "selfcheck: %u Z records / alpha-test counts differ from the sw code", selfbad);
    fprintf(stderr, "gs-front: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
