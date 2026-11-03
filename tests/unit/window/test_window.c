/* Tests for runtime/window.c.
 *
 * 1. Unit checks (no window): 4:3 letterbox maths (controller and keyboard mapping: tests/unit/input),
 *    and the triple-buffer frame
 *    hand-off (ordering, cropping, bad input, and a two-thread stress test for torn frames).
 * 2. Presenter checks (small test windows of their own, shown without activation): the GDI
 *    presenter paints every letterbox bar black, including one-pixel bars; the Direct3D 11
 *    presenter recovers from a failed ResizeBuffers when the window returns to its old size.
 * 3. Interactive run: opens the window, presents moving test patterns at 60 fps for ~3 s while the
 *    frame size changes, toggles borderless fullscreen (F11) on and off, checks the presented
 *    picture through PrintWindow (letterbox bars black, image colour exact), checks the title's
 *    FPS value (also the first one, with frames presented while the window was coming up),
 *    prints input state and timing, then exits.
 *
 * The implementation is #included so its internal helpers can be tested directly.
 *
 * Usage: test_window.exe [--unit-only] [--no-fullscreen] [--seconds N]
 *        (PS2_WINDOW_GDI=1 in the environment tests the GDI presenter)
 */
#include "window.c"
#include "settings.c"
#include "gfx.c"
#include "gpu/gpu_pace.c"                  /* (pc-pacing: gfx.c's picture sizes) */
#include "display_mode.c"                  /* (pc-pacing: fullscreen refresh rate) */
#include "pacing_menu.c"                   /* (pc-pacing: menu_ext.h) */

#include <stdlib.h>

#define FPS_TITLE     WINDOW_TITLE " - "                /* with show_fps on */
#define FPS_TITLE_LEN (sizeof FPS_TITLE - 1)

static int checks, failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                 \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static int is_cleared(const pad_input* p) {
    pad_input c;
    pad_clear(&c);
    return memcmp(&c, p, sizeof c) == 0;
}

/* ---- unit checks ---------------------------------------------------------------------------- */

static void test_letterbox(void) {
    wrect r = letterbox(1280, 960);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1280 && r.h == 960, "%d %d %d %d", r.x, r.y, r.w, r.h);
    r = letterbox(1920, 1080);
    CHECK(r.x == 240 && r.y == 0 && r.w == 1440 && r.h == 1080, "%d %d %d %d", r.x, r.y, r.w, r.h);
    r = letterbox(800, 1000);
    CHECK(r.x == 0 && r.y == 200 && r.w == 800 && r.h == 600, "%d %d %d %d", r.x, r.y, r.w, r.h);
    r = letterbox(0, 100);
    CHECK(r.w == 0 && r.h == 0, "%d %d", r.w, r.h);
    int bad = 0;
    for (int cw = 1; cw < 4000 && !bad; cw += 7)
        for (int ch = 1; ch < 3000 && !bad; ch += 11) {
            r = letterbox(cw, ch);
            int fits = r.x >= 0 && r.y >= 0 && r.x + r.w <= cw && r.y + r.h <= ch;
            int full = r.w == cw || r.h == ch;
            int centred = abs(r.x - (cw - r.x - r.w)) <= 1 && abs(r.y - (ch - r.y - r.h)) <= 1;
            int aspect = llabs((long long)r.w * 3 - (long long)r.h * 4) <= 4;
            if (!(fits && full && centred && aspect)) {
                bad = 1;
                CHECK(0, "letterbox(%d,%d) = %d %d %d %d", cw, ch, r.x, r.y, r.w, r.h);
            }
        }
}

/* Widescreen presentation: any aspect fits (16:9 fills a 16:9 window, 4:3 content is pillarboxed
 * in it), and frames carry their aspect through the triple buffer. */
static void test_fit_aspect(void) {
    wrect r = fit_aspect(1920, 1080, 16, 9);
    CHECK(r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080, "%d %d %d %d", r.x, r.y, r.w, r.h);
    r = fit_aspect(1920, 1080, 4, 3);
    CHECK(r.x == 240 && r.w == 1440 && r.h == 1080, "pillarbox %d %d %d %d", r.x, r.y, r.w, r.h);
    r = fit_aspect(1280, 960, 16, 9);
    CHECK(r.x == 0 && r.y == 120 && r.w == 1280 && r.h == 720, "letterbox %d %d %d %d", r.x, r.y, r.w, r.h);
    r = fit_aspect(3440, 1440, 21, 9);
    CHECK(r.w == 3360 && r.h == 1440 && r.x == 40, "21:9 %d %d %d %d", r.x, r.y, r.w, r.h);
    int bad = 0;
    for (int cw = 1; cw < 4000 && !bad; cw += 13)
        for (int ch = 1; ch < 3000 && !bad; ch += 17) {
            wrect a = fit_aspect(cw, ch, 4, 3), b = letterbox(cw, ch);
            if (memcmp(&a, &b, sizeof a)) { bad = 1; CHECK(0, "fit_aspect 4:3 != letterbox at %dx%d", cw, ch); }
            r = fit_aspect(cw, ch, 16, 9);
            int fits = r.x >= 0 && r.y >= 0 && r.x + r.w <= cw && r.y + r.h <= ch && (r.w == cw || r.h == ch);
            if (!fits || llabs((long long)r.w * 9 - (long long)r.h * 16) > 16) {
                bad = 1;
                CHECK(0, "fit_aspect(%d,%d,16,9) = %d %d %d %d", cw, ch, r.x, r.y, r.w, r.h);
            }
        }
    static uint32_t img[16];
    win_accepting = 1;
    window_present_aspect(img, 4, 4, 4, 16, 9);
    fb_take();
    CHECK(fb_aw[fb_read] == 16 && fb_ah[fb_read] == 9, "aspect travels with the frame (%d:%d)", fb_aw[fb_read], fb_ah[fb_read]);
    window_present(img, 4, 4, 4);
    fb_take();
    CHECK(fb_aw[fb_read] == 4 && fb_ah[fb_read] == 3, "window_present is 4:3 (%d:%d)", fb_aw[fb_read], fb_ah[fb_read]);
    win_accepting = 0;
}

/* (M7) gfx.c: the graphics settings -> the GPU renderer's bits, the keys (not persisted here) */
static void test_gfx(void) {
    ps2_settings_t keep = ps2_settings;
    settings_defaults(&ps2_settings);
    uint32_t g = gfx_bits();
    CHECK(gfx_scale() == 3 && (g & GPU_GFX_PROGRESSIVE) && !(g & GPU_GFX_FIELDS) && (g & GPU_GFX_FLICKER_OFF) && !(g & GPU_GFX_THIN),
          "defaults: 3x, Modern (progressive, flicker filter off), ps2 outlines (%X)", g);
    gfx_apply();
    CHECK(!ps2_motion_blur_off, "Modern: motion blur on");
    CHECK(gfx_key(VK_F4, 0, 0), "F4 handled");
    g = gfx_bits();
    CHECK(ps2_settings.filters_faithful && (g & GPU_GFX_FIELDS) && !(g & GPU_GFX_PROGRESSIVE) && !(g & GPU_GFX_FLICKER_OFF) &&
          !ps2_motion_blur_off, "F4: Faithful (fields, flicker filter, motion blur) %X", g);
    gfx_key(VK_F5, 0, 0);
    CHECK(ps2_settings.motion_blur == 0 && ps2_motion_blur_off, "F5: motion blur off");
    gfx_key(VK_F6, 0, 0);
    CHECK(ps2_settings.flicker_filter == 0 && (gfx_bits() & GPU_GFX_FLICKER_OFF), "F6: flicker filter off");
    gfx_key(VK_F6, 1, 0);
    CHECK(ps2_settings.interlacing == 0 && (gfx_bits() & GPU_GFX_PROGRESSIVE), "Shift+F6: progressive");
    gfx_key(VK_F4, 0, 0);
    CHECK(!ps2_settings.filters_faithful && ps2_settings.motion_blur == -1 && ps2_settings.flicker_filter == -1 &&
          ps2_settings.interlacing == -1 && !ps2_motion_blur_off, "F4 again: Modern, the toggles back to the preset");
    gfx_key(VK_F3, 0, 0);
    CHECK(ps2_settings.outlines_thin && (gfx_bits() & GPU_GFX_THIN), "F3: thin outlines");
    gfx_key(VK_F2, 0, 0);
    CHECK(gfx_scale() == 4, "F2: scale up (%d)", gfx_scale());
    ps2_settings.render_scale = 8;
    gfx_key(VK_F2, 0, 0);
    CHECK(gfx_scale() == 1, "F2 at 8: back to 1 (%d)", gfx_scale());
    gfx_key(VK_F2, 1, 0);
    CHECK(gfx_scale() == 8, "Shift+F2 at 1: 8 (%d)", gfx_scale());
    gfx_key(VK_F2, 2, 0);                /* (M11) Ctrl+F2: render_scale = auto (3x at 1080p until the GPU renderer lowers it) */
    CHECK(ps2_settings.internal_scale == SETTINGS_SCALE_AUTO && gfx_scale() == SETTINGS_SCALE_AUTO_MAX, "Ctrl+F2: auto (%d, %d)",
          ps2_settings.internal_scale, gfx_scale());
    gfx_key(VK_F2, 1, 0);
    CHECK(ps2_settings.render_scale == 2 && gfx_scale() == 2, "Shift+F2 from auto at 3x: fixed 2x (%d)", gfx_scale());
    /* (pc-pacing) pacing / vsync through the settings table (live: pacing_apply_mode), the scale status */
    CHECK(settings_change_key("graphics", "pacing", SETTINGS_PACING_IMMEDIATE, 0) && ps2_settings.pacing == SETTINGS_PACING_IMMEDIATE,
          "pacing = immediate through the table");
    CHECK(settings_change_key("graphics", "vsync", SETTINGS_VSYNC_ADAPTIVE, 0) && ps2_settings.vsync == SETTINGS_VSYNC_ADAPTIVE,
          "vsync = adaptive through the table");
    CHECK(!settings_change_key("graphics", "vsync", 7, 0) && ps2_settings.vsync == SETTINGS_VSYNC_ADAPTIVE, "a bad vsync is refused");
    CHECK(pacing_apply_mode(-1) == 1, "pacing applies while the game runs");
    CHECK(gfx_apply_pacing() == 0, "no presenter in this test: nothing to apply");
    settings_change_key("graphics", "pacing", SETTINGS_PACING_AUTO, 0);
    settings_change_key("graphics", "vsync", SETTINGS_VSYNC_ON, 0);
    settings_change_key("graphics", "render_scale", SETTINGS_RS_AUTO, 0);
    {
        gfx_scale_info si;
        gfx_scale_status(&si);
        CHECK(si.automatic && si.drawn == 3 && si.w == 2048 && si.h == 1344 && !si.limited, "status without the renderer: 3x 2048 x 1344 (%d %dx%d)",
              si.drawn, si.w, si.h);
    }
    gfx_key(VK_F2, 2, 0);
    _putenv("PS2_GS_SCALE=5");
    CHECK(gfx_scale() == 5, "PS2_GS_SCALE wins over auto (%d)", gfx_scale());
    _putenv("PS2_GS_SCALE=");
    ps2_settings.scaler = SETTINGS_SCALER_POINT;
    CHECK(((gfx_bits() >> GPU_GFX_SCALER_SHIFT) & 15) == GPU_SCALER_POINT, "scaler bits");
    CHECK(!gfx_key(VK_F7, 0, 0), "F7 is not a graphics key");
    ps2_settings = keep;
    gfx_apply();
}

/* settings.c: the sly2.ini parser and its aspect values */
static int warn_count;
static void count_warn(void* user, const char* msg) { (void)user; (void)msg; warn_count++; }

static void test_settings(void) {
    ps2_settings_t s;
    settings_defaults(&s);
    CHECK(s.aspect_w == 16 && s.aspect_h == 9 && s.internal_scale == SETTINGS_SCALE_AUTO && !s.outlines_thin && !s.filters_faithful &&
          s.renderer_gpu == 1, "defaults (M11: gpu renderer, internal_scale auto)");
    CHECK(settings_motion_blur(&s) == 1 && settings_flicker_filter(&s) == 0 && settings_interlacing(&s) == 0, "modern preset");
    int w, h;
    CHECK(settings_parse_aspect("4:3", &w, &h) && w == 4 && h == 3, "4:3");
    CHECK(settings_parse_aspect(" 32:18 ", &w, &h) && w == 16 && h == 9, "reduced");
    CHECK(settings_parse_aspect("21/9", &w, &h) && w == 7 && h == 3, "21/9 -> 7:3");
    CHECK(settings_parse_aspect("1.7778", &w, &h) && w == 889 && h == 500, "decimal ratio %d:%d", w, h);
    CHECK(!settings_parse_aspect("wide", &w, &h) && !settings_parse_aspect("16:", &w, &h) &&
          !settings_parse_aspect("1:3", &w, &h) && !settings_parse_aspect("", &w, &h), "rejects garbage");

    warn_count = 0;
    int n = settings_parse("\xEF\xBB\xBF; comment\r\n[display]\r\naspect = 4:3   ; old TV\r\n\r\n[graphics]\n"
                           "internal_scale=2\noutlines = Thin\nfilters = faithful\nmotion_blur = off\n"
                           "interlacing = preset\n", &s, count_warn, NULL);
    CHECK(n == 0 && warn_count == 0, "clean file: %d problems", n);
    CHECK(s.aspect_w == 4 && s.aspect_h == 3, "aspect %d:%d", s.aspect_w, s.aspect_h);
    CHECK(s.internal_scale == 2 && s.outlines_thin == 1 && s.filters_faithful == 1, "graphics keys");
    CHECK(settings_motion_blur(&s) == 0 && settings_flicker_filter(&s) == 1 && settings_interlacing(&s) == 1,
          "faithful preset with motion blur off");

    settings_defaults(&s);
    warn_count = 0;
    n = settings_parse("aspect = 21:9\nbogus line\n[graphics]\ninternal_scale = 99\ncolour = red\n[broken\n", &s,
                       count_warn, NULL);
    CHECK(n == 4 && warn_count == 4, "four problems reported (%d, %d)", n, warn_count);
    CHECK(s.aspect_w == 7 && s.aspect_h == 3 && s.internal_scale == SETTINGS_SCALE_AUTO, "good keys still apply, bad ones don't");
    /* (M11) internal_scale = auto */
    n = settings_parse("[graphics]\ninternal_scale = 4\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.internal_scale == 4, "internal_scale = 4");
    n = settings_parse("[graphics]\ninternal_scale = Auto ; let the game choose\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.internal_scale == SETTINGS_SCALE_AUTO, "internal_scale = auto (%d)", s.internal_scale);
    n = settings_parse("[graphics]\ninternal_scale = 0\n", &s, count_warn, NULL);
    CHECK(n == 1 && s.internal_scale == SETTINGS_SCALE_AUTO, "internal_scale = 0 is not a value (auto is)");

    /* (M7) graphics.scaler; the template has every graphics key and reads back */
    settings_defaults(&s);
    CHECK(s.scaler == SETTINGS_SCALER_AUTO, "scaler defaults to auto");
    warn_count = 0;
    n = settings_parse("[graphics]\nscaler = Catmull-Rom\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.scaler == SETTINGS_SCALER_CATMULL, "scaler = catmull-rom (%d)", s.scaler);
    n = settings_parse("[graphics]\nscaler = lanczos\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.scaler == SETTINGS_SCALER_LANCZOS, "scaler = lanczos");
    n = settings_parse("[graphics]\nscaler = sharp\n", &s, count_warn, NULL);
    CHECK(n == 1 && s.scaler == SETTINGS_SCALER_LANCZOS, "bad scaler reported and ignored");
    {
        static char tpl[8192];
        ps2_settings_t a, b;
        settings_defaults(&a);
        a.render_scale = 5; a.outlines_thin = 1; a.filters_faithful = 1; a.flicker_filter = 0; a.scaler = SETTINGS_SCALER_BOX;
        int len = settings_format(&a, tpl, sizeof tpl);
        settings_defaults(&b);
        warn_count = 0;
        n = settings_parse(tpl, &b, count_warn, NULL);
        CHECK(len > 0 && n == 0 && b.render_scale == 5 && b.outlines_thin == 1 && b.filters_faithful == 1 && b.flicker_filter == 0 &&
              b.motion_blur == -1 && b.scaler == SETTINGS_SCALER_BOX, "graphics keys round trip through the template (%d problems)", n);
    }

    /* [display] hud = edges (default) | centered */
    settings_defaults(&s);
    CHECK(s.hud_centered == 0, "hud defaults to edges");
    warn_count = 0;
    n = settings_parse("[display]\nhud = Centered ; keep it in the middle\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.hud_centered == 1, "hud = centered (%d, %d)", n, s.hud_centered);
    n = settings_parse("hud = edges\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.hud_centered == 0, "hud = edges");
    n = settings_parse("hud = centred\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.hud_centered == 1, "hud = centred (British spelling)");
    n = settings_parse("hud = left\n", &s, count_warn, NULL);
    CHECK(n == 1 && warn_count == 1 && s.hud_centered == 1, "bad hud value reported and ignored");
    char tmpl[4096];
    settings_defaults(&s);
    s.hud_centered = 1;
    settings_format(&s, tmpl, sizeof tmpl);
    CHECK(strstr(tmpl, "\nhud = centered\n") != NULL, "template writes the hud key");

    /* [graphics] renderer = gpu (default since M11) | software, pacing = vsync (default) | vrr | display (M6) */
    settings_defaults(&s);
    CHECK(s.renderer_gpu == 1 && s.pacing == SETTINGS_PACING_VSYNC, "renderer / pacing defaults");
    CHECK(strstr(tmpl, "\nrenderer = gpu\n") && strstr(tmpl, "\npacing = auto\n") && strstr(tmpl, "\nvsync = on\n") && strstr(tmpl, "\nrender_scale = 100%\n") &&
          strstr(tmpl, "\nmsaa = 4x\n") && strstr(tmpl, "\nrenderer_mode = modern\n") &&   /* (r2-m8: Balanced) */
          !strstr(tmpl, "internal_scale =") && strstr(tmpl, "\nmode = borderless\n"), "template writes renderer / pacing / render_scale / mode");
    warn_count = 0;
    n = settings_parse("[graphics]\nrenderer = GPU\npacing = vrr\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.renderer_gpu == 1 && s.pacing == SETTINGS_PACING_VRR, "renderer = gpu, pacing = vrr (%d)", n);
    n = settings_parse("[graphics]\nrenderer = sw\npacing = display\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.renderer_gpu == 0 && s.pacing == SETTINGS_PACING_DISPLAY, "renderer = sw, pacing = display");
    n = settings_parse("[graphics]\nrenderer = vulkan\npacing = fast\n", &s, count_warn, NULL);
    CHECK(n == 2 && warn_count == 2 && s.renderer_gpu == 0 && s.pacing == SETTINGS_PACING_DISPLAY, "bad renderer / pacing ignored");
    s.renderer_gpu = 0;
    s.pacing = SETTINGS_PACING_DISPLAY;
    settings_format(&s, tmpl, sizeof tmpl);
    CHECK(strstr(tmpl, "\nrenderer = software\n") && strstr(tmpl, "\npacing = display\n"), "template writes renderer = software");
    /* (pc-pacing) [display] fullscreen_refresh and its mode choice */
    settings_defaults(&s);
    CHECK(s.fullscreen_refresh == 0, "fullscreen_refresh = desktop by default");
    n = settings_parse("[display]\nfullscreen_refresh = auto\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.fullscreen_refresh == -1, "fullscreen_refresh = auto");
    n = settings_parse("[display]\nfullscreen_refresh = 120\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.fullscreen_refresh == 120, "fullscreen_refresh = 120");
    warn_count = 0;
    n = settings_parse("[display]\nfullscreen_refresh = fast\n", &s, count_warn, NULL);
    CHECK(n == 1 && warn_count == 1 && s.fullscreen_refresh == 120, "bad fullscreen_refresh ignored");
    settings_format(&s, tmpl, sizeof tmpl);
    CHECK(strstr(tmpl, "\nfullscreen_refresh = 120\n") != NULL, "template writes fullscreen_refresh");
    {
        const int dell[] = { 165, 144, 119, 59 }, hz120[] = { 120, 60 }, odd[] = { 165, 144, 100 };
        CHECK(display_mode_pick(dell, 4, 165, -1) == 119, "auto on a 165 Hz display offering 119 (119.88) Hz: 119");
        CHECK(display_mode_pick(dell, 4, 165, 0) == 0, "desktop: never changed");
        CHECK(display_mode_pick(dell, 4, 119, -1) == 0, "auto when the desktop is 119 Hz already: no change");
        CHECK(display_mode_pick(hz120, 2, 120, -1) == 0, "auto at 120 Hz: no change");
        CHECK(display_mode_pick(odd, 3, 165, -1) == 0, "auto without a multiple: no change");
        CHECK(display_mode_pick(dell, 4, 165, 59) == 59 && display_mode_pick(dell, 4, 165, 75) == 0, "a chosen rate if offered");
        CHECK(display_mode_pick(dell, 4, 144, -1) == 119, "auto at 144 Hz: 119");
        /* (pc-polish) another resolution (Fullscreen's): never "no change" */
        static const int at1080[] = { 60, 100, 120, 144 }, at720[] = { 60, 50 };
        CHECK(display_mode_rate_at(at1080, 4, 165, -1) == 120 && display_mode_rate_at(at1080, 4, 165, 0) == 144 &&
              display_mode_rate_at(at1080, 4, 120, 0) == 120 && display_mode_rate_at(at720, 2, 165, 75) == 60 &&
              display_mode_rate_at(at1080, 4, 50, 0) == 144 && display_mode_rate_at(at1080, 0, 60, 0) == 0, "display_mode_rate_at");
    }
    /* (pc-pacing) pacing = auto | paced | immediate | display | vrr (vsync = the old default's name = auto), vsync = on | off |
     * adaptive */
    settings_defaults(&s);
    CHECK(s.pacing == SETTINGS_PACING_AUTO && s.vsync == SETTINGS_VSYNC_ON, "pacing auto, vsync on by default");
    n = settings_parse("[graphics]\npacing = paced\nvsync = adaptive\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.pacing == SETTINGS_PACING_PACED && s.vsync == SETTINGS_VSYNC_ADAPTIVE, "pacing = paced, vsync = adaptive (%d)", n);
    n = settings_parse("[graphics]\npacing = immediate\nvsync = off\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.pacing == SETTINGS_PACING_IMMEDIATE && s.vsync == SETTINGS_VSYNC_OFF, "pacing = immediate, vsync = off");
    n = settings_parse("[graphics]\npacing = vsync\n", &s, count_warn, NULL);
    CHECK(n == 0 && s.pacing == SETTINGS_PACING_AUTO, "pacing = vsync (old files) = auto");
    warn_count = 0;
    n = settings_parse("[graphics]\nvsync = sometimes\n", &s, count_warn, NULL);
    CHECK(n == 1 && warn_count == 1 && s.vsync == SETTINGS_VSYNC_OFF, "bad vsync ignored (%d)", n);
    s.pacing = SETTINGS_PACING_IMMEDIATE;
    settings_format(&s, tmpl, sizeof tmpl);
    CHECK(strstr(tmpl, "\npacing = immediate\n") && strstr(tmpl, "\nvsync = off\n"), "template writes immediate / vsync off");

    /* the template written next to the exe parses back to the defaults */
    char text[4096];
    ps2_settings_t d, back;
    settings_defaults(&d);
    settings_format(&d, text, sizeof text);
    memset(&back, 0x55, sizeof back);
    settings_defaults(&back);
    back.aspect_w = 1;
    warn_count = 0;
    CHECK(settings_parse(text, &back, count_warn, NULL) == 0 && !memcmp(&back, &d, sizeof d), "template round trip");

    /* [game] disc (quoted: ; and # inside), [window] keys */
    settings_defaults(&s);
    warn_count = 0;
    n = settings_parse("[game]\ndisc = \"D:\\Games #2\\Sly 2; USA.iso\"   ; my disc\n[window]\nx = -1200\ny = 40\n"
                       "width = 1600\nheight = 900\nmaximized = on\nfullscreen = yes\nshow_fps = 1\n", &s, count_warn, NULL);
    CHECK(n == 0, "game/window keys: %d problems", n);
    CHECK(!strcmp(s.disc, "D:\\Games #2\\Sly 2; USA.iso"), "quoted disc path '%s'", s.disc);
    CHECK(s.win_x == -1200 && s.win_y == 40 && s.win_w == 1600 && s.win_h == 900 && s.win_maximized &&
          s.win_fullscreen && s.show_fps, "window keys");
    n = settings_parse("[window]\nwidth = 5\nfullscreen = maybe\n", &s, count_warn, NULL);
    CHECK(n == 2 && s.win_w == 1600 && s.win_fullscreen == 1, "bad window values ignored");

    /* key-by-key updates keep the rest of the file */
    char out[8192];
    const char* user = "; mine\r\n[display]\r\naspect = 4:3 ; old TV\r\n\r\n[window]\r\nfullscreen = off\r\n\r\n; later\r\n[graphics]\r\n";
    int len = settings_update_text(user, "display", "aspect", "21:9", out, sizeof out);
    CHECK(len > 0 && strstr(out, "aspect = 21:9   ; old TV\r\n") && strstr(out, "; mine\r\n") && strstr(out, "; later\r\n[graphics]"),
          "rewrite keeps the comment and the other lines:\n%s", out);
    len = settings_update_text(user, "window", "width", "1707", out, sizeof out);
    CHECK(len > 0 && strstr(out, "fullscreen = off\r\nwidth = 1707\r\n\r\n; later"), "new key at the end of its section:\n%s", out);
    len = settings_update_text(user, "game", "disc", "C:\\a;b.iso", out, sizeof out);
    CHECK(len > 0 && strstr(out, "[graphics]\r\n\r\n[game]\r\ndisc = \"C:\\a;b.iso\"\r\n"), "new section, quoted value:\n%s", out);
    settings_defaults(&s);
    settings_parse(out, &s, NULL, NULL);
    CHECK(!strcmp(s.disc, "C:\\a;b.iso") && s.aspect_w == 4, "updated text parses back");
    len = settings_update_text("[game]\r\ndisc = a.iso\r\n\r\n", "window", "x", "5", out, sizeof out);
    CHECK(len > 0 && !strcmp(out, "[game]\r\ndisc = a.iso\r\n\r\n[window]\r\nx = 5\r\n"), "one blank line between sections: '%s'", out);
    len = settings_update_text("", "window", "x", "5", out, sizeof out);
    CHECK(len > 0 && !strcmp(out, "[window]\nx = 5\n"), "empty file: '%s'", out);
    len = settings_update_text("aspect = 4:3", "display", "aspect", "16:9", out, sizeof out);
    CHECK(len > 0 && !strcmp(out, "aspect = 16:9"), "keys before any header are [display]: '%s'", out);
    CHECK(settings_update_text(user, "window", "width", "1707", out, 20) < 0, "overflow reported");

    /* the user data folder (PS2_USER_DIR, set by main) and a write + read back through the file */
    const wchar_t* dir = settings_user_dir();
    CHECK(dir && settings_portable() && GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES, "user dir %ls", dir);
    const char* const kv[][3] = { { "window", "width", "1500" }, { "window", "height", "844" }, { "game", "disc", "x.iso" } };
    CHECK(settings_set(kv, 3), "settings_set");
    settings_load();
    CHECK(ps2_settings.win_w == 1500 && ps2_settings.win_h == 844 && !strcmp(ps2_settings.disc, "x.iso") &&
          ps2_settings.aspect_w == 16, "stored keys load back (%d x %d, '%s')", ps2_settings.win_w, ps2_settings.win_h,
          ps2_settings.disc);
    /* [display] read again while the game runs (sly2.ini edited meanwhile); environment overrides win */
    const char* const kd[][3] = { { "display", "aspect", "4:3" }, { "display", "hud", "centered" } };
    CHECK(settings_set(kd, 2), "settings_set display");
    int rw = 0, rh = 0, rhud = -1;
    CHECK(settings_read_display(&rw, &rh, &rhud) && rw == 4 && rh == 3 && rhud == 1, "read back %d:%d hud %d", rw, rh, rhud);
    _putenv("PS2_ASPECT=21:9");
    _putenv("PS2_HUD=edges");
    settings_load();
    CHECK(ps2_settings.aspect_w == 7 && ps2_settings.aspect_h == 3 && !ps2_settings.hud_centered, "env overrides at load");
    CHECK(settings_read_display(&rw, &rh, &rhud) && rw == 7 && rh == 3 && rhud == 0, "env keys keep their values (%d:%d %d)",
          rw, rh, rhud);
    _putenv("PS2_ASPECT=");
    _putenv("PS2_HUD=");
    _putenv("PS2_GS_BACKEND=hw");
    _putenv("PS2_VBLANK=display");
    settings_load();
    CHECK(ps2_settings.renderer_gpu == 1 && ps2_settings.pacing == SETTINGS_PACING_DISPLAY, "PS2_GS_BACKEND=hw, PS2_VBLANK=display");
    _putenv("PS2_GS_BACKEND=sw");
    _putenv("PS2_VBLANK=");
    _putenv("PS2_GS_PACING=vrr");
    settings_load();
    CHECK(ps2_settings.renderer_gpu == 0 && ps2_settings.pacing == SETTINGS_PACING_VRR, "PS2_GS_BACKEND=sw, PS2_GS_PACING=vrr");
    _putenv("PS2_GS_BACKEND=");
    _putenv("PS2_GS_PACING=");
    wchar_t ini[MAX_PATH + 80];
    settings_user_path(L"sly2.ini", ini, MAX_PATH + 80);
    DeleteFileW(ini);                                    /* the interactive run starts at the default place */
    settings_load();
    CHECK(!settings_read_display(&rw, &rh, &rhud), "no file: nothing to read");
}

/* F7 / F8 (window.c): what they ask aspect.c for. aspect.c is not linked here: these stand-ins apply a
 * request at once, as the game does at its next frame. */
static int stub_requests;
void aspect_request(int w, int h, int hud_centered) {
    stub_requests++;
    if (w > 0 && h > 0) { ps2_settings.aspect_w = w; ps2_settings.aspect_h = h; }
    if (hud_centered >= 0) ps2_settings.hud_centered = hud_centered;
}
void aspect_target(int* w, int* h, int* hud_centered) {
    *w = ps2_settings.aspect_w; *h = ps2_settings.aspect_h; *hud_centered = ps2_settings.hud_centered;
}

static void test_display_switch(void) {
    ps2_settings.aspect_w = 16; ps2_settings.aspect_h = 9; ps2_settings.hud_centered = 0;
    switch_aspect();
    CHECK(ps2_settings.aspect_w == 4 && ps2_settings.aspect_h == 3, "F7: 16:9 -> 4:3");
    switch_aspect();
    CHECK(ps2_settings.aspect_w == 16 && ps2_settings.aspect_h == 9, "F7: 4:3 -> 16:9");
    ps2_settings.aspect_w = 7; ps2_settings.aspect_h = 3;
    switch_aspect();
    switch_aspect();
    CHECK(ps2_settings.aspect_w == 7 && ps2_settings.aspect_h == 3, "F7 twice from 21:9 comes back to 21:9");
    switch_hud();
    CHECK(ps2_settings.hud_centered == 1 && ps2_settings.aspect_w == 7, "F8: edges -> centered, aspect kept");
    switch_hud();
    CHECK(ps2_settings.hud_centered == 0, "F8: centered -> edges");
    CHECK(stub_requests == 6, "one request per key (%d)", stub_requests);
    ps2_settings.aspect_w = 16; ps2_settings.aspect_h = 9;
}

static uint32_t src[FRAME_MAX * 2 * 16];

static void test_frame_handoff_basic(void) {
    CHECK(fb_setup(), "fb_setup");
    InterlockedExchange(&win_accepting, 1);
    while (fb_take()) {}
    CHECK(!fb_take(), "spurious frame");

    for (int i = 0; i < 8 * 3; i++) src[i] = 0xA0000000u | (uint32_t)i;
    window_present(src, 4, 3, 8);                        /* stride > width */
    CHECK(fb_take(), "frame A not delivered");
    CHECK(fb_w[fb_read] == 4 && fb_h[fb_read] == 3, "A size %dx%d", fb_w[fb_read], fb_h[fb_read]);
    int ok = 1;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++) ok &= fb_pix[fb_read][y * 4 + x] == (0xA0000000u | (uint32_t)(y * 8 + x));
    CHECK(ok, "frame A rows copied wrongly");
    CHECK(!fb_take(), "frame A delivered twice");

    for (int i = 0; i < 16; i++) src[i] = 0xB0000000u | (uint32_t)i;
    window_present(src, 5, 2, 5);
    for (int i = 0; i < 16; i++) src[i] = 0xC0000000u | (uint32_t)i;
    window_present(src, 6, 1, 6);
    CHECK(fb_take(), "frame C not delivered");
    CHECK(fb_w[fb_read] == 6 && fb_h[fb_read] == 1 && fb_pix[fb_read][5] == 0xC0000005u, "not the newest frame");
    CHECK(!fb_take(), "stale frame B delivered after C");

    printf("(two 'bad frame' / 'cropped' log lines are expected next)\n");
    fflush(stdout);
    window_present(NULL, 4, 4, 4);
    window_present(src, 8, 1, 4);                        /* stride < width */
    window_present(src, 0, 1, 4);
    CHECK(!fb_take(), "invalid frame delivered");

    for (int i = 0; i < FRAME_MAX * 2 * 2; i++) src[i] = (uint32_t)i;
    window_present(src, FRAME_MAX * 2, 2, FRAME_MAX * 2);  /* too wide: cropped */
    CHECK(fb_take() && fb_w[fb_read] == FRAME_MAX && fb_h[fb_read] == 2 &&
          fb_pix[fb_read][FRAME_MAX] == (uint32_t)(FRAME_MAX * 2), "crop: %dx%d", fb_w[fb_read], fb_h[fb_read]);
    InterlockedExchange(&win_accepting, 0);
    window_present(src, 4, 4, 4);
    CHECK(!fb_take(), "frame accepted while not initialised");
}

/* Producer thread presents frames whose every pixel holds the frame number; the consumer checks
 * that no taken frame is torn and that frame numbers never go backwards. */
#define STRESS_W 64
#define STRESS_H 48
static volatile LONG stress_stop;
static DWORD WINAPI stress_producer(LPVOID arg) {
    (void)arg;
    static uint32_t frame[STRESS_W * STRESS_H];
    for (uint32_t n = 1; !stress_stop; n++) {
        for (int i = 0; i < STRESS_W * STRESS_H; i++) frame[i] = n;
        window_present(frame, STRESS_W, STRESS_H, STRESS_W);
    }
    return 0;
}

static void test_frame_handoff_stress(void) {
    InterlockedExchange(&win_accepting, 1);
    while (fb_take()) {}
    stress_stop = 0;
    HANDLE t = CreateThread(NULL, 0, stress_producer, NULL, 0, NULL);
    uint32_t last = 0;
    int taken = 0, torn = 0, backwards = 0;
    ULONGLONG end = GetTickCount64() + 300;
    while (GetTickCount64() < end) {
        if (!fb_take()) { YieldProcessor(); continue; }
        const uint32_t* px = fb_pix[fb_read];
        uint32_t n = px[0];
        for (int i = 1; i < STRESS_W * STRESS_H; i++) if (px[i] != n) { torn++; break; }
        if (n <= last) backwards++;
        last = n;
        taken++;
    }
    stress_stop = 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    while (fb_take()) {}
    InterlockedExchange(&win_accepting, 0);
    printf("hand-off stress: %d frames taken, %u presented\n", taken, last);
    CHECK(taken > 100 && torn == 0 && backwards == 0, "taken %d torn %d backwards %d", taken, torn, backwards);
}

static void test_headless_input(void) {
    pad_input p;
    window_get_input(2, &p);
    CHECK(is_cleared(&p), "port 2 not cleared");
    window_get_input(-1, &p);
    CHECK(is_cleared(&p), "port -1 not cleared");
    window_get_input(1, &p);                             /* no window yet: keyboard ignored */
    input_device_info devs[INPUT_MAX_DEVICES];
    int n = input_devices(devs, INPUT_MAX_DEVICES), port1 = 0;
    for (int i = 0; i < n; i++) port1 |= devs[i].port == 1;
    printf("controllers: %d connected\n", n);
    if (!port1) CHECK(is_cleared(&p), "absent port 1 not cleared");
}

/* ---- PS2_INPUT scripts: absolute, relative (+n) and function-anchored (@addr+n) tokens --------- */

static uint32_t fake_anchor_calls;
uint32_t ps2_func_calls(uint32_t addr, uint32_t ra) {
    return addr == 0x187D80 && ra == 0x1DD96C ? fake_anchor_calls : 0;
}
uint32_t ps2_vblank_count(void) { return 0; }
static int ram_dumps, calls_requests, watch_requests;
static uint32_t calls_frames, watch_frames;
void ps2_request_ram_dump(void) { ram_dumps++; }
static int rec_starts;
void gs_record_start_now(void) { rec_starts++; }
void ps2_request_calls_window(uint32_t frames) { calls_requests++; calls_frames = frames; }
void ps2_request_watch_window(uint32_t frames) { watch_requests++; watch_frames = frames; }

static void test_input_script(void) {
    script_n = 0;
    script_frame = 0;
    script_parse("10:cross:2,+5:square:1,@187d80/1dd96c+3:start:2,+1:lup:1,7:bogus");
    CHECK(script_n == 4, "parsed %d tokens", script_n);
    uint16_t buttons[40];
    uint8_t ly[40];
    for (int f = 0; f < 40; f++) {
        if (f == 25) fake_anchor_calls++;                 /* the anchor function runs before frame 25 */
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        buttons[f] = p.buttons;
        ly[f] = p.ly;
    }
    for (int f = 0; f < 40; f++) {
        CHECK(!!(buttons[f] & PAD_CROSS) == (f == 10 || f == 11), "cross at frame %d", f);
        CHECK(!!(buttons[f] & PAD_SQUARE) == (f == 15), "square at frame %d", f);
        CHECK(!!(buttons[f] & PAD_START) == (f == 28 || f == 29), "start at frame %d", f);
        CHECK((ly[f] == 0) == (f == 29), "left stick up at frame %d (ly %02X)", f, ly[f]);
    }
    /* quiet anchor: fires once the function was called and then stayed silent for 5 frames */
    script_n = 0;
    script_frame = 0;
    fake_anchor_calls = 0;
    script_parse("@!187d80/1dd96c+5:none,+2:circle:1");
    CHECK(script_n == 2, "parsed %d tokens", script_n);
    for (int f = 0; f < 40; f++) {
        if (f >= 10 && f < 20) fake_anchor_calls++;       /* called on frames 10..19, silent after */
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        CHECK(!!(p.buttons & PAD_CIRCLE) == (f == 26), "circle at frame %d", f);
    }
    /* partial stick deflection and the RAM dump request */
    script_n = 0;
    script_frame = 0;
    ram_dumps = 0;
    rec_starts = 0;
    script_parse("2:lx40:3,+1:ryC0:1,+0:RAM,+2:rec");   /* upper case works too; + the GS recording start */
    CHECK(script_n == 4, "parsed %d tokens", script_n);
    for (int f = 0; f < 8; f++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        CHECK((p.lx == 0x40) == (f >= 2 && f < 5), "lx at frame %d: %02X", f, p.lx);
        CHECK((p.ry == 0xC0) == (f == 3), "ry at frame %d: %02X", f, p.ry);
    }
    CHECK(ram_dumps == 1, "%d RAM dumps", ram_dumps);
    CHECK(rec_starts == 1, "%d GS recording starts", rec_starts);
    /* "release" ends the holds in progress (r1, lx) but not a press that starts with it */
    script_n = 0;
    script_frame = 0;
    script_parse("1:r1:1000,1:lx20:1000,6:release,6:cross:2,+3:r1:2");
    CHECK(script_n == 5, "parsed %d tokens", script_n);
    for (int f = 0; f < 14; f++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        CHECK(!!(p.buttons & PAD_R1) == ((f >= 1 && f < 6) || f == 9 || f == 10), "r1 at frame %d", f);
        CHECK((p.lx == 0x20) == (f >= 1 && f < 6), "lx at frame %d: %02X", f, p.lx);
        CHECK(!!(p.buttons & PAD_CROSS) == (f == 6 || f == 7), "cross at frame %d", f);
    }
    /* debug windows: "calls:<n>" and "watch:<n>" ask once, at their start frame, for n VBlanks */
    script_n = 0;
    script_frame = 0;
    calls_requests = watch_requests = 0;
    script_parse("1:calls:120,+2:watch:30");
    CHECK(script_n == 2, "parsed %d tokens", script_n);
    for (int f = 0; f < 40; f++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        CHECK(!p.buttons, "no buttons from debug tokens at frame %d", f);
        CHECK(calls_requests == (f >= 1), "calls requests at frame %d: %d", f, calls_requests);
        CHECK(watch_requests == (f >= 3), "watch requests at frame %d: %d", f, watch_requests);
    }
    CHECK(calls_frames == 120 && watch_frames == 30, "window lengths %u %u", calls_frames, watch_frames);
    script_n = 0;                                         /* leave scripting off for the other tests */
}

/* PS2_INPUT_FILE live input: a line longer than any fixed buffer (24 KB), two lines appended at once
 * (each starts at the moment it is read), and more tokens over a session than the script holds at a
 * time (finished ones are dropped when a line arrives). */
static void append_line(const char* path, const char* pre, int fillers, const char* post) {
    FILE* f = fopen(path, "ab");
    if (!f) return;
    fputs(pre, f);
    for (int i = 0; i < fillers; i++) fputs("+0:none,", f);
    fprintf(f, "%s\r\n", post);
    fclose(f);
}

static void test_live_input(void) {
    char path[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, path);
    snprintf(path + n, sizeof path - n, "sly2_live_input_%lu.txt", GetCurrentProcessId());
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL, "cannot create %s", path);
    if (!f) return;
    fclose(f);
    script_path = path;
    script_offset = 0;
    script_n = 0;
    script_frame = 0;
    append_line(path, "", 3000, "+5:circle:1");           /* read at frame 0 (lines are read every 8) */
    int circle_at = -1, pad_frames = 0;
    for (int f = 0; f < 16; f++, pad_frames++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        if (p.buttons & PAD_CIRCLE) { CHECK(circle_at < 0, "circle twice"); circle_at = f; }
    }
    CHECK(circle_at == 5, "long line: circle at frame %d (want 5)", circle_at);
    append_line(path, "+2:cross:1,", 0, "+0:none");       /* two lines at once: read at frame 16 */
    append_line(path, "+3:square:1,", 0, "+0:none");
    int cross_at = -1, square_at = -1;
    for (int f = 16; f < 24; f++, pad_frames++) {
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        if (p.buttons & PAD_CROSS) cross_at = f;
        if (p.buttons & PAD_SQUARE) square_at = f;
    }
    CHECK(cross_at == 18 && square_at == 19, "two lines: cross at %d (want 18), square at %d (want 19)",
          cross_at, square_at);
    int sent = 0, seen = 0;                               /* 10 lines of 3000 tokens: > SCRIPT_MAX in all */
    for (int k = 0; k < 10; k++) {
        append_line(path, "", 3000, "+1:triangle:1");
        sent += 3002;
        for (int f = 0; f < 8; f++, pad_frames++) {
            pad_input p;
            pad_clear(&p);
            script_apply(0, &p);
            if (p.buttons & PAD_TRIANGLE) seen++;
        }
    }
    CHECK(sent > SCRIPT_MAX && seen == 10, "%d tokens sent, triangle seen %d times (want 10), %d pending",
          sent, seen, script_n);
    CHECK(script_n < 3100, "%d tokens still held", script_n);
    script_path = NULL;                                   /* leave scripting off for the other tests */
    script_n = 0;
    remove(path);
}

/* "pause": the pad read after the pause token blocks until the next line arrives, and that line's
 * "+0" tokens apply in the same pad frame. */
static volatile LONG line_appended, held_while_waiting;
static volatile LONG timer_held, timer_holds;
void ps2_debug_hold(int on) { InterlockedExchange(&timer_held, on); if (on) InterlockedIncrement(&timer_holds); }
static DWORD WINAPI append_later(LPVOID path) {
    Sleep(150);
    InterlockedExchange(&held_while_waiting, timer_held);  /* EE time stands still during the wait */
    append_line((const char*)path, "+0:cross:1,", 0, "+0:none");
    InterlockedExchange(&line_appended, 1);
    return 0;
}

static void test_live_pause(void) {
    char path[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, path);
    snprintf(path + n, sizeof path - n, "sly2_live_pause_%lu.txt", GetCurrentProcessId());
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL, "cannot create %s", path);
    if (!f) return;
    fclose(f);
    script_path = path;
    script_offset = 0;
    script_n = 0;
    script_frame = 0;
    append_line(path, "+2:pause,", 0, "+0:none");       /* read at frame 0, pause from frame 3 */
    int blocked_at = -1, cross_at = -1;
    HANDLE t = NULL;
    line_appended = 0;
    for (int fr = 0; fr < 8; fr++) {
        if (fr == 3) t = CreateThread(NULL, 0, append_later, path, 0, NULL);
        LONG before = line_appended;                      /* a call that waited for the line */
        pad_input p;
        pad_clear(&p);
        script_apply(0, &p);
        if (!before && line_appended) blocked_at = fr;
        if (p.buttons & PAD_CROSS) cross_at = fr;
    }
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
    CHECK(blocked_at == 3 && cross_at == 3, "pause: blocked at frame %d, cross at %d (want 3, 3)",
          blocked_at, cross_at);
    CHECK(held_while_waiting == 1 && timer_held == 0 && timer_holds == 1,
          "pause: PS2 time held while waiting %ld, held after %ld, holds %ld (want 1, 0, 1)",
          (long)held_while_waiting, (long)timer_held, (long)timer_holds);
    script_path = NULL;
    script_n = 0;
    remove(path);
}

/* ---- presenter checks: the GDI and Direct3D 11 presenters drawing into a test window of their own
 * (shown without activation, before window_init, so the window thread is not involved) ---------- */

static LRESULT CALLBACK test_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND test_window(int cw, int ch) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = test_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"Sly2WindowTest";
    RegisterClassW(&wc);
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Sly2WindowTest", L"window test",
                             WS_OVERLAPPEDWINDOW, 40, 40, r.right - r.left, r.bottom - r.top, NULL, NULL,
                             wc.hInstance, NULL);
    if (h) ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

static void set_client_size(HWND h, int cw, int ch);
static int near_rgb(uint32_t a, uint32_t b, int tol);

/* Copy the client area into a top-down 32-bit buffer. GDI content is read through the window DC;
 * flip-model Direct3D content only through PrintWindow(PW_RENDERFULLCONTENT). */
static int read_client(HWND h, uint32_t* buf, int cw, int ch, int print) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC wdc = GetDC(h), mem = CreateCompatibleDC(wdc);
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int ok = 0;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        memset(bits, 0x5A, (size_t)cw * (size_t)ch * 4);
        ok = print ? PrintWindow(h, mem, 1 | 2) : BitBlt(mem, 0, 0, cw, ch, wdc, 0, 0, SRCCOPY);
        GdiFlush();
        for (size_t i = 0; i < (size_t)cw * (size_t)ch; i++) buf[i] = ((const uint32_t*)bits)[i] & 0xFFFFFFu;
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(h, wdc);
    return ok;
}

static uint32_t shot[1200 * 1000];
static uint32_t solid[640 * 480];

/* GDI: every client pixel outside the 4:3 picture must be painted black, including a bar only one
 * pixel wide (the client is first filled white so an unpainted bar shows). */
static void test_gdi_bars(void) {
    static const int sizes[][2] = { { 700, 480 }, { 640, 600 }, { 641, 480 }, { 642, 481 }, { 640, 481 }, { 801, 600 } };
    HWND h = test_window(700, 600);
    CHECK(h != NULL, "test window");
    if (!h) return;
    for (int i = 0; i < 320 * 224; i++) solid[i] = 0x7F2060C0u;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        set_client_size(h, sizes[k][0], sizes[k][1]);
        RECT rc;
        GetClientRect(h, &rc);
        int cw = rc.right, ch = rc.bottom;
        HDC dc = GetDC(h);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        ReleaseDC(h, dc);
        gdi_render(h, solid, 320, 224, 4, 3);
        if (!read_client(h, shot, cw, ch, 0)) { CHECK(0, "cannot read the client area"); break; }
        wrect r = letterbox(cw, ch);
        int unpainted = 0, fx = -1, fy = -1;
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                int in = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
                if (!in && shot[(size_t)y * cw + x] != 0 && unpainted++ == 0) { fx = x; fy = y; }
            }
        uint32_t mid = shot[(size_t)(r.y + r.h / 2) * cw + r.x + r.w / 2];
        CHECK(unpainted == 0, "GDI client %dx%d (picture %d,%d %dx%d): %d bar pixels not black, first at %d,%d = %06X",
              cw, ch, r.x, r.y, r.w, r.h, unpainted, fx, fy, unpainted ? shot[(size_t)fy * cw + fx] : 0);
        CHECK(near_rgb(mid, 0x2060C0, 2), "GDI client %dx%d: picture centre %06X", cw, ch, mid);
    }
    DestroyWindow(h);
}

/* Direct3D 11: a failed ResizeBuffers (forced here by holding a back-buffer reference, as the
 * runtime refuses to resize then) must not leave the presenter without a render target. When the
 * window goes back to the old size, the next frame must be drawn and shown. */
static void test_d3d_resize_failure(void) {
    HWND h = test_window(640, 480);
    CHECK(h != NULL, "test window");
    if (!h) return;
    if (!d3d_init(h)) {
        printf("Direct3D resize check: SKIPPED (no Direct3D 11)\n");
        DestroyWindow(h);
        return;
    }
    for (int i = 0; i < 640 * 480; i++) solid[i] = 0x7FFF0000u;             /* red */
    HRESULT hr = d3d_render(h, solid, 640, 480, 1, 4, 3);
    CHECK(SUCCEEDED(hr) && d3d.bb_w == 640 && d3d.bb_h == 480, "first frame: hr %08lX, back buffer %dx%d",
          (unsigned long)hr, d3d.bb_w, d3d.bb_h);
    ID3D11Texture2D* held = NULL;
    IDXGISwapChain_GetBuffer(d3d.sc, 0, &IID_ID3D11Texture2D_, (void**)&held);
    set_client_size(h, 700, 500);
    hr = d3d_render(h, solid, 640, 480, 1, 4, 3);
    CHECK(FAILED(hr), "resize with a back buffer still referenced succeeded (hr %08lX)", (unsigned long)hr);
    RELEASE(held);
    set_client_size(h, 640, 480);                                           /* back to the old size */
    for (int i = 0; i < 640 * 480; i++) solid[i] = 0x7F00FF00u;             /* green */
    hr = d3d_render(h, solid, 640, 480, 2, 4, 3);
    CHECK(SUCCEEDED(hr) && d3d.rtv != NULL && d3d.bb_w == 640 && d3d.bb_h == 480,
          "frame after a failed resize: hr %08lX, render target %p, back buffer %dx%d", (unsigned long)hr,
          (void*)d3d.rtv, d3d.bb_w, d3d.bb_h);
    hr = d3d_render(h, solid, 640, 480, 2, 4, 3);                                 /* flip model: show it twice */
    Sleep(100);
    if (read_client(h, shot, 640, 480, 1) && shot[240 * 640 + 320] != 0x5A5A5A) {
        uint32_t c = shot[240 * 640 + 320];
        printf("Direct3D resize check: centre after recovery %06X (want 00FF00)\n", c);
        CHECK(near_rgb(c, 0x00FF00, 2), "frame after a failed resize not shown: centre %06X", c);
    } else {
        printf("Direct3D resize check: picture capture unavailable\n");
    }
    d3d_shutdown();
    DestroyWindow(h);
}

/* ---- interactive run -------------------------------------------------------------------------- */

static uint32_t frame[FRAME_MAX * FRAME_MAX];

/* Gradient + checkerboard, a white 1-pixel border, a moving white column and yellow row, and a ring
 * that looks circular when the picture is shown at 4:3. The X byte holds junk on purpose. */
static void pattern(uint32_t* px, int stride, int w, int h, int t) {
    int bx = (t * 6) % w, by = (t * 3) % h;
    float sx = (4.0f / 3.0f) / (float)w, sy = 1.0f / (float)h;   /* pixel -> 4:3 display units */
    for (int y = 0; y < h; y++) {
        uint32_t* row = px + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint32_t c = (uint32_t)(x * 255 / w) << 16 | (uint32_t)(y * 255 / h) << 8 | (uint32_t)((t * 4) & 255);
            if (((x >> 4) ^ (y >> 4)) & 1) c = (c >> 1) & 0x7F7F7Fu;
            float dx = (x - w / 2) * sx, dy = (y - h / 2) * sy, d = dx * dx + dy * dy;
            if (d > 0.30f * 0.30f && d < 0.32f * 0.32f) c = 0x00FF40u;
            if (x >= bx && x < bx + 8) c = 0xFFFFFFu;
            if (y >= by && y < by + 4) c = 0xFFFF00u;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) c = 0xFFFFFFu;
            row[x] = c | 0xAB000000u;
        }
    }
}

static void print_input(int n) {
    pad_input a, b;
    window_get_input(0, &a);
    window_get_input(1, &b);
    printf("[input] frame %3d focus=%d | port0 buttons=%04X L=(%02X,%02X) R=(%02X,%02X) L2p=%02X R2p=%02X"
           " | port1 buttons=%04X L=(%02X,%02X) R=(%02X,%02X)\n",
           n, GetForegroundWindow() == g_hwnd, a.buttons, a.lx, a.ly, a.rx, a.ry, a.pressure[10], a.pressure[11],
           b.buttons, b.lx, b.ly, b.rx, b.ry);
}

static int near_rgb(uint32_t a, uint32_t b, int tol) {
    for (int s = 0; s < 24; s += 8)
        if (abs((int)((a >> s) & 255) - (int)((b >> s) & 255)) > tol) return 0;
    return 1;
}

/* Capture the client area (PrintWindow with PW_RENDERFULLCONTENT also captures Direct3D content)
 * and compare the four quadrants of the letterboxed picture (top-left, top-right, bottom-left,
 * bottom-right: catches channel swaps and flips), a pixel just inside the top-left corner, and a
 * letterbox bar (must be black). Returns 1 = pass, 0 = fail, -1 = capture unavailable. */
static int check_picture(HWND h, const uint32_t quad[4]) {
    RECT rc;
    GetClientRect(h, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return -1;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int result = -1;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        memset(bits, 0x5A, (size_t)cw * (size_t)ch * 4);
        if (PrintWindow(h, mem, 1 /* PW_CLIENTONLY */ | 2 /* PW_RENDERFULLCONTENT */)) {
            GdiFlush();
            const uint32_t* px = bits;
            wrect r = letterbox(cw, ch);
#define AT(x, y) (px[(size_t)(y) * cw + (x)] & 0xFFFFFFu)
            uint32_t got[4] = { AT(r.x + r.w / 4, r.y + r.h / 4), AT(r.x + r.w * 3 / 4, r.y + r.h / 4),
                                AT(r.x + r.w / 4, r.y + r.h * 3 / 4), AT(r.x + r.w * 3 / 4, r.y + r.h * 3 / 4) };
            uint32_t inner = AT(r.x + 1, r.y + 1);
            uint32_t bar = r.x > 0 ? AT(r.x / 2, ch / 2) : AT(cw / 2, r.y / 2);
#undef AT
            printf("picture check: client %dx%d, picture %d,%d %dx%d: quadrants %06X %06X %06X %06X"
                   " (want %06X %06X %06X %06X), corner %06X, bar %06X\n", cw, ch, r.x, r.y, r.w, r.h,
                   got[0], got[1], got[2], got[3], quad[0], quad[1], quad[2], quad[3], inner, bar);
            if (got[0] == 0x5A5A5A && got[3] == 0x5A5A5A) result = -1;   /* nothing captured */
            else {
                result = near_rgb(inner, quad[0], 2) && near_rgb(bar, 0, 2);
                for (int q = 0; q < 4; q++) result &= near_rgb(got[q], quad[q], 2);
            }
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return result;
}

static LARGE_INTEGER qf;
static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qf.QuadPart;
}

static void set_client_size(HWND h, int cw, int ch) {
    RECT wr, cr;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    SetWindowPos(h, NULL, 0, 0, cw + (wr.right - wr.left) - cr.right, ch + (wr.bottom - wr.top) - cr.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void interactive(double seconds, int do_fullscreen) {
    /* Same DPI context as the window thread, so sizes and captures are in real pixels. */
    typedef HANDLE (WINAPI* set_dpi_fn)(HANDLE);
    set_dpi_fn set_dpi = (set_dpi_fn)dll_proc(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (set_dpi && !set_dpi((HANDLE)(LONG_PTR)-4)) set_dpi((HANDLE)(LONG_PTR)-3);

    /* Frames handed over while the window and renderer are still coming up (0.35-0.9 s on a cold
     * start, i.e. 20-55 frames from the emulator) must not inflate the first FPS reading in the
     * title. Present 60 at once right away (the window takes far longer than that to come up), then
     * 60 per second. Calls made by the unit checks above don't count either. */
    InterlockedExchange(&fb_presented, 0);
    double t0 = now_ms(), early_due = t0;
    ps2_settings.show_fps = 1;                           /* the FPS checks below read it from the title */
    /* The default display mode is borderless (settings.c, pc-menu2): the window would open fullscreen and
     * the F11 / Alt+Enter checks below (which post the keys to the window itself, so they do not depend on
     * which application has the foreground) would find the opposite state. Start windowed. */
    ps2_settings.win_fullscreen = SETTINGS_FULLSCREEN_OFF;
    window_init();
    pattern(frame, FRAME_MAX, 640, 448, 0);
    for (int n = 0; n < 60; n++) window_present(frame, 640, 448, FRAME_MAX);
    int early_burst_done = !g_hwnd;
    for (int n = 0; !g_hwnd && now_ms() - t0 < 5000; n++) {
        pattern(frame, FRAME_MAX, 640, 448, n);
        window_present(frame, 640, 448, FRAME_MAX);
        early_due += 1000.0 / 60.0;
        while (now_ms() < early_due) Sleep(1);
    }
    if (!early_burst_done) printf("note: the window came up before the 60-frame burst ended\n");
    HWND h = g_hwnd;
    CHECK(h != NULL, "window did not come up");
    if (!h) return;
    printf("window up after %.0f ms, presenter: %s\n", now_ms() - t0, use_d3d ? "Direct3D 11" : "GDI");
    RECT cr;
    GetClientRect(h, &cr);
    printf("client area %ldx%ld\n", cr.right, cr.bottom);
    int dw, dh;
    display_aspect(&dw, &dh);                            /* the settings' aspect (16:9 by default) */
    CHECK(labs(cr.right * dh - cr.bottom * dw) <= dw + dh, "default client area is not %d:%d", dw, dh);

    static const int sizes[][2] = { { 640, 448 }, { 512, 448 }, { 640, 480 }, { 320, 240 }, { 720, 480 }, { 256, 224 } };
    int frames = (int)(seconds * 60.0), shown0 = frames_shown;
    double present_total = 0, present_max = 0, input_total = 0;
    int input_calls = 0, fs_seen = 0, fs_restored = 0;
    double first_fps = -1;
    RECT before_fs;
    GetWindowRect(h, &before_fs);
    CHECK(!window_fullscreen(), "the window did not start windowed");
    double start = now_ms();
    for (int i = 0; i < frames; i++) {
        const int* s = sizes[(i / 30) % (sizeof sizes / sizeof sizes[0])];
        pattern(frame, FRAME_MAX, s[0], s[1], i);
        double a = now_ms();
        window_present(frame, s[0], s[1], FRAME_MAX);
        double d = now_ms() - a;
        present_total += d;
        if (d > present_max) present_max = d;
        if (i % 30 == 0) print_input(i);
        if (first_fps < 0 && i % 6 == 3) {               /* the first FPS value the title shows */
            char t[128];
            GetWindowTextA(h, t, sizeof t);
            if (!strncmp(t, FPS_TITLE, FPS_TITLE_LEN) && strncmp(t + FPS_TITLE_LEN, "0.0 ", 4)) {
                first_fps = atof(t + FPS_TITLE_LEN);
                printf("first title FPS reading: \"%s\" (frame %d)\n", t, i);
            }
        }
        if (i % 30 == 15) {                              /* time the emulation-thread call alone */
            pad_input p;
            a = now_ms();
            for (int k = 0; k < 100; k++) window_get_input(k & 1, &p);
            input_total += now_ms() - a;
            input_calls += 100;
        }
        if (do_fullscreen) {
            if (i == 60) PostMessageW(h, WM_KEYDOWN, VK_F11, 0);
            if (i == 80) {
                MONITORINFO mi;
                mi.cbSize = sizeof mi;
                GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
                RECT wr;
                GetWindowRect(h, &wr);
                fs_seen = (GetWindowLongPtrW(h, GWL_STYLE) & WS_POPUP) && EqualRect(&wr, &mi.rcMonitor);
                printf("fullscreen: %s (window %ld,%ld-%ld,%ld)\n", fs_seen ? "on" : "NOT ON", wr.left, wr.top, wr.right, wr.bottom);
            }
            if (i == 90) PostMessageW(h, WM_SYSKEYDOWN, VK_RETURN, (LPARAM)1 << 29);   /* Alt+Enter */
            if (i == 110) {
                RECT wr;
                GetWindowRect(h, &wr);
                fs_restored = (GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION) == WS_CAPTION && EqualRect(&wr, &before_fs);
                printf("fullscreen off: %s\n", fs_restored ? "restored" : "NOT RESTORED");
            }
        }
        double due = start + (i + 1) * (1000.0 / 60.0);
        while (now_ms() < due - 1.5) Sleep(1);
        while (now_ms() < due) YieldProcessor();
    }
    double elapsed = now_ms() - start;
    int shown = frames_shown - shown0;
    if (do_fullscreen) {
        CHECK(fs_seen, "F11 did not enter borderless fullscreen");
        CHECK(fs_restored, "Alt+Enter did not restore the window");
    }

    char title[128];
    GetWindowTextA(h, title, sizeof title);
    printf("title: \"%s\"\n", title);
    CHECK(strncmp(title, FPS_TITLE, FPS_TITLE_LEN) == 0 && strstr(title, " fps"), "title has no FPS counter");
    double fps = atof(title + FPS_TITLE_LEN);
    CHECK(fps > 40 && fps < 75, "title FPS %.1f while presenting at 60", fps);
    CHECK(first_fps > 54 && first_fps < 66, "first title FPS %.1f while presenting at 60 from the start", first_fps);
    ps2_settings.show_fps = 0;                           /* the default: just the game's name */
    set_title(h, 60.0);
    GetWindowTextA(h, title, sizeof title);
    CHECK(!strcmp(title, WINDOW_TITLE), "plain title \"%s\"", title);
    ps2_settings.show_fps = 1;

    /* Picture check at a wide client size: letterbox bars must be black, the image exact. */
    set_client_size(h, 900, 450);
    static const uint32_t quad[4] = { 0x20C060, 0xE04010, 0x1030F0, 0xF0F0F0 };
    for (int i = 0; i < 15; i++) {
        for (int y = 0; y < 448; y++)
            for (int x = 0; x < 640; x++) frame[y * 640 + x] = quad[(y >= 224) * 2 + (x >= 320)] | 0x7F000000u;
        window_present(frame, 640, 448, 640);
        Sleep(16);
    }
    Sleep(50);
    int pic = check_picture(h, quad);
    if (pic < 0) printf("picture check: SKIPPED (capture unavailable)\n");
    else CHECK(pic == 1, "presented picture wrong (see 'picture check' line)");
    set_client_size(h, DEFAULT_CLIENT_W, DEFAULT_CLIENT_H);

    printf("presented %d frames in %.0f ms (%.1f fps); window thread showed %d new frames (%.0f%%)\n", frames,
           elapsed, frames * 1000.0 / elapsed, shown, 100.0 * shown / frames);
    printf("window_present: avg %.3f ms, max %.3f ms; window_get_input: avg %.1f us\n", present_total / frames,
           present_max, input_total * 1000.0 / input_calls);
    CHECK(shown > frames / 4, "window thread showed only %d of %d frames", shown, frames);
    CHECK(present_total / frames < 2.0, "window_present too slow");
}

/* ---- --external: the window with an external presenter (window_gpu.h; the GPU renderer's render thread) ----
 * A thread of the test plays the render thread: its own device and flip-model swap chain (FLIP_DISCARD,
 * waitable object, as runtime/gpu/gpu_present.c) on window_hwnd(), resized to window_client_size() before
 * each present, filled with one colour. Checks: the window creates no presenter of its own; pictures
 * given to window_present go to the external function; the client size and resize serial follow size
 * changes and borderless fullscreen (F11 / Alt+Enter) while the external swap chain presents; the
 * captured client area shows the external colour; the debug keys need PS2_GS_DEBUG_KEYS; switching back
 * (window_set_external_presenter(0)) brings the window's own presenter back once the swap chain is gone. */
static volatile LONG ext_calls, ext_stop, ext_presents, ext_resizes;
static void ext_present_fn(const uint32_t* px, int w, int h, int stride, int aw, int ah) {
    (void)px; (void)w; (void)h; (void)stride; (void)aw; (void)ah;
    InterlockedIncrement(&ext_calls);
}

static DWORD WINAPI ext_render(LPVOID arg) {
    HWND hwnd = (HWND)arg;
    PFN_D3D11_CREATE_DEVICE create = (PFN_D3D11_CREATE_DEVICE)dll_proc(LoadLibraryW(L"d3d11.dll"), "D3D11CreateDevice");
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL got;
    static const D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0 };
    if (!create || FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, lv, 1, D3D11_SDK_VERSION, &dev, &got, &ctx))) return 1;
    IDXGIDevice* dxdev = NULL;
    IDXGIAdapter* ad = NULL;
    IDXGIFactory2* f2 = NULL;
    IDXGISwapChain1* sc = NULL;
    HRESULT hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice_, (void**)&dxdev);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxdev, &ad);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2_, (void**)&f2);
    DXGI_SWAP_CHAIN_DESC1 d;
    memset(&d, 0, sizeof d);
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (SUCCEEDED(hr)) hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)dev, hwnd, &d, NULL, NULL, &sc);
    if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(f2, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    int bw = -1, bh = -1;
    ID3D11RenderTargetView* rtv = NULL;
    while (SUCCEEDED(hr) && !ext_stop) {
        int cw, ch;
        window_client_size(&cw, &ch);
        if (cw <= 0 || ch <= 0) { Sleep(5); continue; }
        if (cw != bw || ch != bh) {
            RELEASE(rtv);
            ID3D11DeviceContext_ClearState(ctx);
            if (FAILED(IDXGISwapChain1_ResizeBuffers(sc, 0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN, d.Flags))) { Sleep(5); continue; }
            ID3D11Texture2D* bb = NULL;
            IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D_, (void**)&bb);
            if (bb) { ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)bb, NULL, &rtv); RELEASE(bb); }
            bw = cw; bh = ch;
            InterlockedIncrement(&ext_resizes);
        }
        static const float col[4] = { 0.25f, 0.5f, 0.75f, 1.0f };   /* 40 80 BF */
        if (rtv) ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
        IDXGISwapChain1_Present(sc, 1, 0);
        InterlockedIncrement(&ext_presents);
    }
    RELEASE(rtv);
    if (ctx) { ID3D11DeviceContext_ClearState(ctx); ID3D11DeviceContext_Flush(ctx); }
    RELEASE(sc); RELEASE(f2); RELEASE(ad); RELEASE(dxdev); RELEASE(ctx); RELEASE(dev);
    return 0;
}

static int external_test(void) {
    typedef HANDLE (WINAPI* set_dpi_fn)(HANDLE);
    set_dpi_fn set_dpi = (set_dpi_fn)dll_proc(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (set_dpi && !set_dpi((HANDLE)(LONG_PTR)-4)) set_dpi((HANDLE)(LONG_PTR)-3);
    window_set_external_presenter(1, ext_present_fn);
    ps2_settings.win_fullscreen = SETTINGS_FULLSCREEN_OFF;   /* the default is borderless: start windowed, see interactive() */
    window_init();
    HWND h = (HWND)window_hwnd(5000);
    CHECK(h != NULL, "window did not come up");
    if (!h) return 1;
    CHECK(window_external_presenter() && !use_d3d && !d3d.dev, "the window made a presenter of its own (use_d3d %d)", use_d3d);
    pattern(frame, FRAME_MAX, 640, 448, 0);
    for (int i = 0; i < 5; i++) window_present(frame, 640, 448, FRAME_MAX);
    CHECK(ext_calls == 5, "window_present -> external presenter: %ld calls of 5", (long)ext_calls);
    HANDLE rt = CreateThread(NULL, 0, ext_render, h, 0, NULL);
    for (int i = 0; i < 300 && ext_presents <= 5; i++) Sleep(10);   /* device + swap chain creation time varies */
    CHECK(ext_presents > 5,"the external swap chain presents (%ld)", (long)ext_presents);
    int cw, ch;
    uint32_t s0 = window_resize_serial();
    set_client_size(h, 700, 500);
    Sleep(200);
    window_client_size(&cw, &ch);
    CHECK(cw == 700 && ch == 500 && window_resize_serial() != s0, "client size %dx%d after a resize to 700x500 (serial %u -> %u)", cw, ch,
          s0, window_resize_serial());
    RECT rc;
    GetClientRect(h, &rc);
    if (read_client(h, shot, rc.right, rc.bottom, 1) && shot[(size_t)(rc.bottom / 2) * rc.right + rc.right / 2] != 0x5A5A5A) {
        uint32_t c = shot[(size_t)(rc.bottom / 2) * rc.right + rc.right / 2];
        printf("external presenter: centre %06X (want 4080BF)\n", c);
        CHECK(near_rgb(c, 0x4080BF, 3), "external swap chain picture not shown: centre %06X", c);
    } else printf("external presenter: picture capture unavailable\n");
    LONG r0 = ext_resizes;
    PostMessageW(h, WM_KEYDOWN, VK_F11, 0);              /* borderless fullscreen under the external swap chain */
    Sleep(400);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    window_client_size(&cw, &ch);
    CHECK(window_fullscreen() && cw == mi.rcMonitor.right - mi.rcMonitor.left && ch == mi.rcMonitor.bottom - mi.rcMonitor.top,
          "fullscreen %d, client %dx%d", window_fullscreen(), cw, ch);
    PostMessageW(h, WM_SYSKEYDOWN, VK_RETURN, (LPARAM)1 << 29);   /* Alt+Enter: back */
    Sleep(400);
    window_client_size(&cw, &ch);
    CHECK(!window_fullscreen() && cw == 700 && ch == 500, "after Alt+Enter: fullscreen %d, client %dx%d", window_fullscreen(), cw, ch);
    CHECK(ext_resizes >= r0 + 2, "the external swap chain followed the size changes (%ld resizes)", (long)(ext_resizes - r0));
    LONG p0 = ext_presents;
    Sleep(200);
    CHECK(ext_presents > p0 + 3, "still presenting after the fullscreen round trip");
    /* debug keys: off without PS2_GS_DEBUG_KEYS, on with it */
    window_take_debug_keys();
    debug_keys_on = -1;
    _putenv("PS2_GS_DEBUG_KEYS=");
    PostMessageW(h, WM_KEYDOWN, VK_F9, 0);
    Sleep(100);
    CHECK(window_take_debug_keys() == 0, "F9 without PS2_GS_DEBUG_KEYS");
    debug_keys_on = -1;
    _putenv("PS2_GS_DEBUG_KEYS=1");
    PostMessageW(h, WM_KEYDOWN, VK_F9, 0);
    Sleep(100);
    CHECK(window_take_debug_keys() == WINDOW_KEY_SWITCH, "F9 with PS2_GS_DEBUG_KEYS=1");
    /* the external renderer goes away: the window's own presenter comes back */
    InterlockedExchange(&ext_stop, 1);
    WaitForSingleObject(rt, 5000);
    CloseHandle(rt);
    window_set_external_presenter(0, NULL);
    for (int i = 0; i < 100 && !use_d3d; i++) Sleep(10);
    for (int i = 0; i < 10; i++) {
        for (int y = 0; y < 448; y++)
            for (int x = 0; x < 640; x++) frame[y * 640 + x] = 0x7F00C000u;   /* green */
        window_present(frame, 640, 448, 640);
        Sleep(16);
    }
    Sleep(100);
    CHECK(use_d3d || dwm_flush, "own presenter after the external one (use_d3d %d)", use_d3d);
    GetClientRect(h, &rc);
    wrect lb = fit_aspect(rc.right, rc.bottom, 4, 3);
    if (read_client(h, shot, rc.right, rc.bottom, 1) && shot[(size_t)(lb.y + lb.h / 2) * rc.right + lb.x + lb.w / 2] != 0x5A5A5A) {
        uint32_t c = shot[(size_t)(lb.y + lb.h / 2) * rc.right + lb.x + lb.w / 2];
        printf("own presenter again: centre %06X (want 00C000)\n", c);
        CHECK(near_rgb(c, 0x00C000, 3), "own presenter's picture not shown: centre %06X", c);
    }
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}

/* --close-test: closing the window must end the process with exit code 0 (run.ps1 checks it). */
static int close_test(void) {
    window_init();
    for (int i = 0; i < 500 && !g_hwnd; i++) Sleep(10);
    if (!g_hwnd) return 2;
    pattern(frame, FRAME_MAX, 640, 448, 0);
    window_present(frame, 640, 448, FRAME_MAX);
    Sleep(100);
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    Sleep(3000);
    printf("window still open 3 s after WM_CLOSE\n");
    return 3;
}

int main(int argc, char** argv) {
    /* sly2.ini and the window state go to a folder of the test's own (never the player's
     * %LOCALAPPDATA%\Sly2), starting without a file: the window opens at its default place. */
    wchar_t dir[MAX_PATH + 32], env[MAX_PATH + 64];
    DWORD dn = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* slash = dn ? wcsrchr(dir, L'\\') : NULL;
    if (slash) wcscpy(slash, L"\\userdata"); else wcscpy(dir, L"userdata");
    swprintf(env, MAX_PATH + 64, L"PS2_USER_DIR=%ls", dir);
    _wputenv(env);
    wchar_t ini[MAX_PATH + 48];
    settings_user_path(L"sly2.ini", ini, MAX_PATH + 48);
    DeleteFileW(ini);
    settings_load();

    int unit_only = 0, fullscreen = 1;
    double seconds = 3.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--close-test")) return close_test();
        if (!strcmp(argv[i], "--external")) { setvbuf(stdout, NULL, _IONBF, 0); return external_test(); }
        if (!strcmp(argv[i], "--unit-only")) unit_only = 1;
        else if (!strcmp(argv[i], "--no-fullscreen")) fullscreen = 0;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&qf);
    timeBeginPeriod(1);

    test_letterbox();
    test_fit_aspect();
    test_settings();
    test_gfx();
    test_display_switch();
    test_frame_handoff_basic();
    test_frame_handoff_stress();
    test_headless_input();
    test_input_script();
    test_live_input();
    test_live_pause();
    printf("unit checks: %d, failures: %d\n", checks, failures);
    if (!unit_only) {
        typedef HANDLE (WINAPI* set_dpi_fn)(HANDLE);      /* real pixels, like the window thread */
        set_dpi_fn set_dpi = (set_dpi_fn)dll_proc(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
        if (set_dpi && !set_dpi((HANDLE)(LONG_PTR)-4)) set_dpi((HANDLE)(LONG_PTR)-3);
        int before = failures;
        test_gdi_bars();
        test_d3d_resize_failure();
        printf("presenter checks: %s\n", failures == before ? "passed" : "FAILED");
        interactive(seconds < 2.5 ? 2.5 : seconds, fullscreen);
    }

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
