/* Tests of the settings model (runtime/settings.c: the option table, presets, sly2.ini round trips keeping
 * comments, validation) and of the Options menu (runtime/menu.c: pages, navigation, values, mouse, bindings; and
 * runtime/menu_draw.c: the picture at several output sizes). No window, no game: the input subsystem is faked here.
 *
 * Usage: test_menu.exe [--shots <dir>]   (--shots: every page as raw BGRA files <dir>\<name>.bgra for
 *        scripts/menu_shots.py, which makes the PNGs and the contact sheet) */
#include "settings.c"
#include "gfx.c"
#include "menu.c"
#include "sly_font.c"
#include "menu_draw.c"
#include "menu_stubs.c"

#include <stdlib.h>

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

/* ---- a fake input subsystem (runtime/input.h) ------------------------------------------------------------ */
static input_config fake_cfg;
static int fake_cfg_init, fake_saves, fake_menu_active, fake_capture, fake_flushes, fake_rumbles;
static input_nav_event fake_q[64];
static int fake_qn;
void input_config_defaults(input_config* c) {
    memset(c, 0, sizeof *c);
    memset(c->pad, INPUT_BTN_NONE, sizeof c->pad);
    for (int a = 0; a < INPUT_ACT_PAD_COUNT; a++) c->pad[a][0] = (uint8_t)a;   /* (any distinct defaults) */
    c->key[INPUT_ACT_CROSS][0] = VK_SPACE;
    c->key[INPUT_ACT_CIRCLE][0] = VK_LCONTROL;
    c->vibration = 1;
    c->vibration_strength = 100;
    c->deadzone_left = c->deadzone_right = 12;
    c->lightbar_on = 1;
}
void input_config_get(input_config* c) {
    if (!fake_cfg_init) { input_config_defaults(&fake_cfg); fake_cfg_init = 1; }
    *c = fake_cfg;
}
int input_config_set(const input_config* c, int save) { fake_cfg = *c; fake_saves += save != 0; return 1; }
int input_devices(input_device_info* out, int max) {
    if (max < 1) return 0;
    memset(out, 0, sizeof *out);
    out->id = 7; out->kind = INPUT_DEV_DUALSENSE; out->can_rumble = 1;
    return 1;
}
void input_test_rumble(int id, uint8_t s, uint8_t l, int ms) { (void)id; (void)s; (void)l; (void)ms; fake_rumbles++; }
void input_capture_begin(int what) { fake_capture = what; }
void input_capture_cancel(void) { fake_capture = 0; }
int input_capture_poll(int* code) { (void)code; return INPUT_CAPTURE_PENDING; }
void input_set_menu_active(int on) { fake_menu_active = on; }
void input_nav_flush(void) { fake_flushes++; fake_qn = 0; }
int input_nav_poll(input_nav_event* out) {
    if (!fake_qn) return 0;
    *out = fake_q[0];
    memmove(fake_q, fake_q + 1, sizeof fake_q[0] * (size_t)--fake_qn);
    return 1;
}
static void push(int nav, int src) {
    memset(&fake_q[fake_qn], 0, sizeof fake_q[0]);
    fake_q[fake_qn].nav = nav;
    fake_q[fake_qn].source = src;
    fake_q[fake_qn++].device_id = 7;
}
const char* input_action_label(int act) { static char b[32]; snprintf(b, sizeof b, "Action %d", act); return b; }
const char* input_key_label(int vk) { return vk == VK_SPACE ? "Space" : vk == VK_LCONTROL ? "Left Ctrl" : vk == 'G' ? "G" : "Key"; }
const char* input_button_label(int btn, int xbox) { static const char* n[] = { "Cross", "Circle" }; return xbox ? "A" : btn < 2 ? n[btn] : "Btn"; }

/* ---- a fake host --------------------------------------------------------------------------------------------- */
static int host_mode, host_closed, host_cw = 2560, host_ch = 1440, host_size_w, host_size_h, host_open_requests;
static void h_out(int* w, int* h) { *w = host_cw; *h = host_ch; }
static void h_disp(int* w, int* h, int* hz) { *w = 2560; *h = 1440; *hz = 165; }
static void h_set_mode(int m) { host_mode = m; }
static void h_set_size(int w, int h) { host_size_w = w; host_size_h = h; }
static void h_closed(void) { host_closed++; }
static void h_req(void) { host_open_requests++; menu_open(); }
static int h_true(void) { return 1; }
static int h_auto(void) { return 3; }
static unsigned long long fake_now = 1000000;
static unsigned long long h_now(void) { return fake_now; }
static int host_quits;
static void h_quit(void) { host_quits++; }
static int h_modes(int* wh, int max) {
    static const int m[] = { 2560 << 16 | 1440, 1920 << 16 | 1080, 1280 << 16 | 720 };
    int n = 0;
    for (; n < 3 && n < max; n++) wh[n] = m[n];
    return n;
}
static int h_mode_now(void) { return ps2_settings.win_fullscreen; }   /* (the window does what the setting says) */
static int host_monitors = 1;
static int h_mon_count(void) { return host_monitors; }
static void h_mon_info(int k, int* w, int* h, int* hz, int* pri) { *w = 2560; *h = 1440; *hz = 165; *pri = k == 0; }

static void use_host(void) {
    menu_host h;
    memset(&h, 0, sizeof h);
    h.gpu_active = h_true;
    h.gpu_started = h_true;
    h.auto_scale_now = h_auto;
    h.output_size = h_out;
    h.display_size = h_disp;
    h.window_mode = h_mode_now;
    h.set_window_mode = h_set_mode;
    h.set_client_size = h_set_size;
    h.request_open = h_req;
    h.closed = h_closed;
    h.in_game = h_true;
    h.now_ms = h_now;
    h.quit = h_quit;
    h.display_modes = h_modes;
    h.monitor_count = h_mon_count;
    h.monitor_info = h_mon_info;
    menu_set_host(&h);
    gfx_output_size = h_out;                     /* (the render scale follows the output, as window.c sets it) */
}

/* ---- settings model ------------------------------------------------------------------------------------- */
static int warns;
static void count_warn(void* u, const char* m) { (void)u; (void)m; warns++; }

static char* read_file(const wchar_t* path) {
    static char buf[65536];
    FILE* f = _wfopen(path, L"rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void test_table(void) {
    ps2_settings_t s;
    settings_defaults(&s);
    CHECK(s.preset == SETTINGS_PRESET_BALANCED && settings_detect_preset(&s) == SETTINGS_PRESET_BALANCED, "defaults are Balanced");
    CHECK(s.vol_master == 100 && s.audio_background == 1 && s.auto_max == 3, "new defaults");
    /* every option: default valid, its text parses back to itself */
    for (int i = 0; i < settings_count(); i++) {
        const settings_opt* o = settings_opt_at(i);
        if (o->type == SET_STRING || (o->flags & SET_STATE)) continue;
        char t[64];
        int v = -99;
        CHECK(settings_valid(i, o->def), "%s.%s default valid", o->section, o->key);
        CHECK(settings_parse_value(i, settings_value_text(i, o->def, t, sizeof t), &v) && v == o->def, "%s.%s '%s' round trip (%d)",
              o->section, o->key, t, v);
    }
    int i = settings_find("graphics", "internal_scale"), v;
    CHECK(settings_parse_value(i, "native", &v) && v == SETTINGS_SCALE_NATIVE, "native");
    CHECK(!settings_parse_value(i, "0", &v) && !settings_parse_value(i, "9", &v) && !settings_valid(i, 9), "0 and 9 are not scales");
    i = settings_find("window", "fullscreen");
    CHECK(settings_parse_value(i, "on", &v) && v == 1 && settings_parse_value(i, "exclusive", &v) && v == 2 &&
          settings_parse_value(i, "borderless", &v) && v == 1 && settings_parse_value(i, "no", &v) && v == 0, "fullscreen values");
    i = settings_find("audio", "master");
    CHECK(!settings_valid(i, 101) && !settings_valid(i, -1) && settings_valid(i, 0), "volume range");
    CHECK(settings_find("controls", "vibration") < 0, "[controls] is input.c's");
    warns = 0;
    settings_defaults(&s);
    int n = settings_parse("[controls]\nkey_cross = Space\nvibration = off\n[audio]\nmaster = 40\nmusic = 300\n", &s, count_warn, NULL);
    CHECK(n == 1 && warns == 1 && s.vol_master == 40 && s.vol_music == 100, "[controls] ignored, bad volume reported (%d)", n);
}

static void test_presets(void) {
    ps2_settings_t s;
    for (int p = SETTINGS_PRESET_LOW; p < SETTINGS_PRESET_CUSTOM; p++) {
        settings_defaults(&s);
        s.aspect_w = 21; s.aspect_h = 9;
        settings_apply_preset(&s, p);
        CHECK(settings_detect_preset(&s) == p, "preset %d detected after applying it (%d)", p, settings_detect_preset(&s));
    }
    settings_defaults(&s);
    CHECK(s.look == SETTINGS_LOOK_FAITHFUL && s.preset == SETTINGS_PRESET_MEDIUM, "defaults: Look Faithful, Quality Medium");
    settings_apply_preset(&s, SETTINGS_PRESET_ULTRA);
    CHECK(s.render_scale == SETTINGS_PRESET_ULTRA_PCT && s.msaa == SETTINGS_MSAA_8X && s.outlines_thin && s.lighting_enhanced &&
          s.textures_enhanced && s.renderer_modern && settings_detect_preset(&s) == SETTINGS_PRESET_ULTRA, "Ultra: %d%%, 8x, thin, enhanced",
          SETTINGS_PRESET_ULTRA_PCT);
    /* (menu) Look = Retro: the original values over the player's own, which come back */
    settings_defaults(&s);
    settings_apply_preset(&s, SETTINGS_PRESET_HIGH);
    settings_set_look(&s, SETTINGS_LOOK_RETRO);
    CHECK(s.render_scale == SETTINGS_RS_AUTO && !s.renderer_modern && s.filters_faithful && !s.outlines_thin && s.aspect_w == 4 &&
          s.aspect_h == 3 && !s.lighting_enhanced && !s.textures_enhanced && settings_detect_preset(&s) == SETTINGS_PRESET_ORIGINAL,
          "Retro: Reference on the integer grid, TV effects, 4:3, original outlines, lighting and textures");
    CHECK(settings_own(&s, settings_find("graphics", "msaa")) == SETTINGS_MSAA_8X && settings_own(&s, settings_find("display", "aspect")) == (16 << 16 | 9) &&
          settings_own(&s, settings_find("graphics", "preset")) == SETTINGS_PRESET_HIGH, "the player's own values kept");
    settings_set_look(&s, SETTINGS_LOOK_FAITHFUL);
    CHECK(s.preset == SETTINGS_PRESET_HIGH && s.renderer_modern && s.outlines_thin && s.aspect_w == 16 && !s.filters_faithful, "Faithful again: High");
    settings_set_look(&s, SETTINGS_LOOK_REIMAGINED);
    CHECK(s.look == SETTINGS_LOOK_REIMAGINED && s.preset == SETTINGS_PRESET_HIGH && settings_look(&s) == SETTINGS_LOOK_REIMAGINED,
          "Reimagined: the Quality stays (renders as Faithful for now)");
    /* old preset names */
    static const struct { const char* name; int p; } old_names[] = { { "performance", SETTINGS_PRESET_LOW }, { "balanced", SETTINGS_PRESET_MEDIUM },
                                                                       { "quality", SETTINGS_PRESET_HIGH }, { "ultra", SETTINGS_PRESET_ULTRA } };
    for (size_t k = 0; k < sizeof old_names / sizeof old_names[0]; k++) {
        char txt[64];
        snprintf(txt, sizeof txt, "[graphics]\npreset = %s\n", old_names[k].name);
        settings_defaults(&s);
        settings_parse(txt, &s, NULL, NULL);
        CHECK(s.preset == old_names[k].p && s.look == SETTINGS_LOOK_FAITHFUL, "preset = %s read (%d)", old_names[k].name, s.preset);
    }
    settings_defaults(&s);
    settings_parse("[display]\naspect = 4:3\n[graphics]\npreset = ps2\n", &s, NULL, NULL);
    CHECK(s.look == SETTINGS_LOOK_RETRO && !s.renderer_modern && s.aspect_w == 4 && settings_own(&s, settings_find("graphics", "preset")) == SETTINGS_PRESET_MEDIUM &&
          settings_own(&s, settings_find("display", "aspect")) == (16 << 16 | 9), "preset = ps2: Look Retro over Medium (16:9 when left)");
    settings_defaults(&s);
    settings_apply_preset(&s, SETTINGS_PRESET_ORIGINAL);         /* (the raw values, the look untouched) */
    CHECK(s.look == SETTINGS_LOOK_FAITHFUL && settings_detect_preset(&s) == SETTINGS_PRESET_CUSTOM, "the original values without Retro: Custom");
    settings_apply_preset(&s, SETTINGS_PRESET_QUALITY);
    CHECK(s.render_scale == 100 && s.outlines_thin && !s.filters_faithful && s.lighting_enhanced && s.textures_enhanced, "Quality: 100%%, thin, enhanced lighting and textures");
    CHECK(s.aspect_w == 4, "Quality keeps the aspect (the menu restores widescreen when leaving Original)");
    settings_apply_preset(&s, SETTINGS_PRESET_PERFORMANCE);
    CHECK(s.render_scale == 75 && s.msaa == SETTINGS_MSAA_OFF && s.renderer_modern && !s.outlines_thin && !s.lighting_enhanced && !s.textures_enhanced,
          "Performance: Modern 75%%, no MSAA, original lighting");
    settings_apply_preset(&s, SETTINGS_PRESET_BALANCED);
    CHECK(s.render_scale == 100 && s.msaa == SETTINGS_MSAA_4X && s.renderer_modern && s.lighting_enhanced && s.textures_enhanced, "Balanced: Modern 100%%, MSAA 4x, enhanced lighting and textures");
    s.lighting_enhanced = 0;
    CHECK(settings_detect_preset(&s) == SETTINGS_PRESET_CUSTOM, "Balanced with original lighting -> custom");
    s.lighting_enhanced = 1;
    s.textures_enhanced = 0;
    CHECK(settings_detect_preset(&s) == SETTINGS_PRESET_CUSTOM, "Balanced with original textures -> custom");
    s.textures_enhanced = 1;
    s.outlines_thin = 1;
    CHECK(settings_detect_preset(&s) == SETTINGS_PRESET_CUSTOM, "changing an option -> custom");
    /* a preset in the file wins over its options; custom leaves them */
    settings_defaults(&s);
    settings_parse("[graphics]\nrender_scale = 5x\npreset = quality\n", &s, NULL, NULL);
    CHECK(s.render_scale == 100 && s.preset == SETTINGS_PRESET_QUALITY, "preset = quality wins");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = custom\nrender_scale = 5x\n", &s, NULL, NULL);
    CHECK(s.render_scale == 5 && s.preset == SETTINGS_PRESET_CUSTOM, "preset = custom keeps the options");
    /* (light) lighting: a default of Balanced; an explicit preset decides it, a custom file without the key gets Enhanced */
    settings_defaults(&s);
    CHECK(s.lighting_enhanced == 1, "lighting: enhanced by default");
    settings_parse("[graphics]\npreset = performance\nlighting = enhanced\n", &s, NULL, NULL);
    CHECK(!s.lighting_enhanced && s.preset == SETTINGS_PRESET_PERFORMANCE, "preset = performance: original lighting");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = balanced\nlighting = original\n", &s, NULL, NULL);
    CHECK(s.lighting_enhanced && s.preset == SETTINGS_PRESET_BALANCED, "preset = balanced: enhanced lighting (the preset wins)");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = custom\nrender_scale = 90%\n", &s, NULL, NULL);
    CHECK(s.lighting_enhanced && s.preset == SETTINGS_PRESET_CUSTOM, "custom without the key: enhanced");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = custom\nlighting = original\n", &s, NULL, NULL);
    CHECK(!s.lighting_enhanced && s.preset == SETTINGS_PRESET_CUSTOM, "custom, lighting = original");
    {
        int li = settings_find("graphics", "lighting"), lv;
        CHECK(li >= 0 && settings_parse_value(li, "Original", &lv) && lv == 0 && settings_parse_value(li, "enhanced", &lv) && lv == 1 &&
              settings_parse_value(li, "off", &lv) && lv == 0 && !settings_parse_value(li, "bright", &lv), "lighting values");
    }
    /* (tex) textures: the same rules as lighting */
    settings_defaults(&s);
    CHECK(s.textures_enhanced == 1, "textures: enhanced by default");
    settings_parse("[graphics]\npreset = performance\ntextures = enhanced\n", &s, NULL, NULL);
    CHECK(!s.textures_enhanced && s.preset == SETTINGS_PRESET_PERFORMANCE, "preset = performance: original textures");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = original\n", &s, NULL, NULL);
    CHECK(!s.textures_enhanced && s.preset == SETTINGS_PRESET_ORIGINAL, "preset = original (PS2): original textures");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = quality\ntextures = original\n", &s, NULL, NULL);
    CHECK(s.textures_enhanced && s.preset == SETTINGS_PRESET_QUALITY, "preset = quality: enhanced textures (the preset wins)");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = custom\nlighting = original\n", &s, NULL, NULL);
    CHECK(s.textures_enhanced && s.preset == SETTINGS_PRESET_CUSTOM, "custom without the textures key: enhanced");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = custom\ntextures = original\n", &s, NULL, NULL);
    CHECK(!s.textures_enhanced && s.lighting_enhanced && s.preset == SETTINGS_PRESET_CUSTOM, "custom, textures = original");
    {
        int ti = settings_find("graphics", "textures"), tv;
        CHECK(ti >= 0 && settings_parse_value(ti, "Original", &tv) && tv == 0 && settings_parse_value(ti, "enhanced", &tv) && tv == 1 &&
              settings_parse_value(ti, "off", &tv) && tv == 0 && !settings_parse_value(ti, "sharp", &tv), "textures values");
        static char buf[16384];
        settings_defaults(&s);
        s.preset = SETTINGS_PRESET_CUSTOM;
        s.textures_enhanced = 0;
        settings_format(&s, buf, sizeof buf);
        CHECK(strstr(buf, "\ntextures = original\n") != NULL, "sly2.ini writes textures");
    }
    settings_defaults(&s);
    settings_parse("[graphics]\ninternal_scale = 1\nfilters = faithful\n[display]\naspect = 4:3\n", &s, NULL, NULL);
    CHECK(s.preset == SETTINGS_PRESET_CUSTOM && s.render_scale == SETTINGS_RS_ORIGINAL,
          "an old file without preset (internal_scale = 1): derived (Custom: Rendering is Modern by default)");
    CHECK(gfx_native_scale(1440) == 4 && gfx_native_scale(1080) == 3 && gfx_native_scale(2160) == 5 && gfx_native_scale(448) == 1 &&
          gfx_native_scale(0) == 3, "native scale");
}

/* (pc-polish) render_scale: values, the mapping onto the renderer's integer scales */
static void test_render_scale(void) {
    int i = settings_find("graphics", "render_scale"), v;
    char t[32];
    CHECK(i >= 0 && settings_parse_value(i, "75%", &v) && v == 75 && settings_parse_value(i, "75", &v) && v == 75 &&
          settings_parse_value(i, "3x", &v) && v == 3 && settings_parse_value(i, "original", &v) && v == SETTINGS_RS_ORIGINAL &&
          settings_parse_value(i, "1x", &v) && v == SETTINGS_RS_ORIGINAL && settings_parse_value(i, "native", &v) && v == 100 &&
          settings_parse_value(i, "Auto", &v) && v == SETTINGS_RS_AUTO, "render_scale values");
    CHECK(!settings_parse_value(i, "20%", &v) && !settings_parse_value(i, "9x", &v) && !settings_parse_value(i, "401", &v) &&
          !settings_parse_value(i, "big", &v) && !settings_valid(i, 9) && !settings_valid(i, 24), "render_scale rejects");
    CHECK(!strcmp(settings_value_text(i, 75, t, sizeof t), "75%") && !strcmp(settings_value_text(i, 4, t, sizeof t), "4x") &&
          !strcmp(settings_value_text(i, 1, t, sizeof t), "original"), "render_scale texts");
    /* N(P, rows) = the smallest N whose 448 N rows reach P %% of the output (10 %% slack) */
    static const struct { int rs, h, n; } m[] = {
        { 100, 1440, 4 }, { 100, 1080, 3 }, { 100, 2160, 5 }, { 100, 720, 2 }, { 50, 1440, 2 }, { 50, 1080, 2 }, { 67, 1440, 3 },
        { 75, 1440, 3 }, { 75, 1080, 2 }, { 125, 1440, 4 }, { 150, 1440, 5 }, { 200, 1440, 7 }, { 200, 2160, 8 }, { 50, 448, 1 },
        { SETTINGS_RS_AUTO, 1440, 4 }, { SETTINGS_RS_AUTO, 1080, 3 }, { SETTINGS_RS_AUTO, 0, 3 }, { SETTINGS_RS_ORIGINAL, 2160, 1 },
        { 6, 720, 6 }, { 400, 2160, 8 }, { 25, 1080, 1 },
    };
    for (size_t k = 0; k < sizeof m / sizeof m[0]; k++)
        CHECK(settings_scale_for(m[k].rs, m[k].h) == m[k].n, "scale for %d at %d rows: %d (got %d)", m[k].rs, m[k].h, m[k].n,
              settings_scale_for(m[k].rs, m[k].h));
    CHECK(settings_rs_from_internal(SETTINGS_SCALE_AUTO) == SETTINGS_RS_AUTO && settings_rs_from_internal(SETTINGS_SCALE_NATIVE) == 100 &&
          settings_rs_from_internal(1) == SETTINGS_RS_ORIGINAL && settings_rs_from_internal(5) == 5, "old internal_scale -> render_scale");
    i = settings_find("display", "resolution");
    CHECK(settings_parse_value(i, "1920x1080", &v) && v == (1920 << 16 | 1080) && settings_parse_value(i, "1280 x 720", &v) &&
          v == (1280 << 16 | 720) && settings_parse_value(i, "desktop", &v) && v == 0 && !settings_parse_value(i, "10x10", &v) &&
          !strcmp(settings_value_text(i, 2560 << 16 | 1440, t, sizeof t), "2560x1440"), "display.resolution values");
    i = settings_find("display", "mode");
    CHECK(settings_parse_value(i, "fullscreen", &v) && v == SETTINGS_FULLSCREEN_EXCLUSIVE && settings_parse_value(i, "borderless", &v) &&
          v == 1 && settings_parse_value(i, "exclusive", &v) && v == 2 && settings_parse_value(i, "on", &v) && v == 1 &&
          !strcmp(settings_value_text(i, 0, t, sizeof t), "windowed"), "display.mode values");
}

/* (pc-polish) old keys: read, the new key wins, files rewritten */
static void test_migration(void) {
    ps2_settings_t s;
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = custom\ninternal_scale = native\n[window]\nfullscreen = exclusive\n", &s, NULL, NULL);
    CHECK(s.render_scale == 100 && s.win_fullscreen == SETTINGS_FULLSCREEN_EXCLUSIVE, "old keys read (%d, %d)", s.render_scale, s.win_fullscreen);
    settings_defaults(&s);
    settings_parse("[display]\nmode = windowed\n[graphics]\npreset = custom\nrender_scale = 75%\ninternal_scale = 6\n[window]\nfullscreen = on\n",
                   &s, NULL, NULL);
    CHECK(s.render_scale == 75 && s.win_fullscreen == 0, "the new keys win over old ones (%d, %d)", s.render_scale, s.win_fullscreen);
    static char out[8192];
    const char* old = "; mine\r\n[display]\r\naspect = 16:9\r\n[graphics]\r\npreset = custom\r\ninternal_scale = 5 ; sharp\r\nauto_max = 2\r\n"
                      "outlines = thin\r\n[window]\r\nfullscreen = on\r\nshow_fps = on\r\nwidth = 1280\r\n";
    int n = settings_migrate_text(old, out, sizeof out);
    CHECK(n > 0 && strstr(out, "; mine\r\n") && strstr(out, "render_scale = 5x\r\n") && !strstr(out, "internal_scale") && !strstr(out, "auto_max") &&
          strstr(out, "[display]\r\naspect = 16:9\r\nmode = borderless\r\n") && !strstr(out, "fullscreen") && strstr(out, "show_fps = on\r\n") &&
          strstr(out, "width = 1280\r\n") && strstr(out, "outlines = thin\r\n"), "migrated text (%d):\n%s", n, out);
    CHECK(settings_migrate_text(out, out + 4096, 4096) == 0, "a migrated file needs nothing more");
    CHECK(settings_migrate_text("[graphics]\npreset = quality\ninternal_scale = 2\n", out, sizeof out) > 0 && strstr(out, "render_scale = 100%") &&
          strstr(out, "preset = high"), "a preset decides the migrated render scale: %s", out);
    /* (menu) the preset's old names; original / ps2 -> Look = Retro over Medium */
    CHECK(settings_migrate_text("[graphics]\r\npreset = performance ; mine\r\n", out, sizeof out) > 0 && strstr(out, "preset = low ") && strstr(out, "; mine\r\n"),
          "performance -> low:\n%s", out);
    CHECK(settings_migrate_text("[graphics]\npreset = balanced\n", out, sizeof out) > 0 && strstr(out, "preset = medium\n"), "balanced -> medium");
    CHECK(settings_migrate_text("[display]\naspect = 4:3\n[graphics]\npreset = ps2\nrenderer_mode = reference\nfilters = faithful\n", out, sizeof out) > 0 &&
          strstr(out, "preset = medium\n") && strstr(out, "look = retro\n") && strstr(out, "aspect = 16:9\n") &&
          strstr(out, "renderer_mode = modern\n") && strstr(out, "filters = modern\n"),
          "ps2 -> Look Retro (Original's option lines -> Medium's: no stale reference / faithful under Custom later):\n%s", out);
    {
        ps2_settings_t s;
        settings_defaults(&s);
        settings_parse(out, &s, NULL, NULL);
        CHECK(s.look == SETTINGS_LOOK_RETRO && !s.renderer_modern && s.aspect_w == 4 && s.filters_faithful, "the migrated file: Retro in effect");
        settings_set_look(&s, SETTINGS_LOOK_FAITHFUL);
        CHECK(s.preset == SETTINGS_PRESET_MEDIUM && s.renderer_modern && s.aspect_w == 16 && !s.filters_faithful, "leaving Retro: Medium, 16:9");
        CHECK(settings_migrate_text(out, out + 4096, 4096) == 0, "migrated once");
    }
    {   /* the owner's own file (custom: thin outlines, modern filters, render scale auto): nothing changes, no surprises */
        const char* own = "; Sly 2 PC settings.\r\n[display]\r\nmode = borderless\r\nresolution = desktop\r\naspect = 16:9\r\nhud = edges\r\n"
                          "monitor = 0\r\nfullscreen_refresh = desktop\r\nframe_rate = 60\r\n\r\n[graphics]\r\npreset = custom\r\nrenderer = gpu\r\n"
                          "pacing = auto\r\nvsync = on\r\nrenderer_mode = modern\r\nrender_scale = auto\r\nmsaa = 4x\r\nlighting = enhanced\r\n"
                          "textures = enhanced\r\noutlines = thin\r\nfilters = modern\r\nmotion_blur = preset\r\nflicker_filter = preset\r\n"
                          "interlacing = preset\r\nscaler = auto\r\n\r\n[settings]\r\nversion = 2\r\n";
        CHECK(settings_migrate_text(own, out, sizeof out) == 0 && settings_migrate_defaults(own, out, sizeof out) == 0, "the owner's file: unchanged");
        ps2_settings_t s;
        settings_defaults(&s);
        int probs = settings_parse(own, &s, count_warn, NULL);
        CHECK(probs == 0 && s.preset == SETTINGS_PRESET_CUSTOM && s.look == SETTINGS_LOOK_FAITHFUL && s.outlines_thin && !s.filters_faithful &&
              s.render_scale == SETTINGS_RS_AUTO && s.renderer_modern && s.msaa == SETTINGS_MSAA_4X && s.motion_blur == -1,
              "the owner's file: Custom, Faithful, thin, auto (%d problems)", probs);
    }
    /* on load: the file is rewritten */
    wchar_t ini[MAX_PATH + 80];
    settings_user_path(L"sly2.ini", ini, MAX_PATH + 80);
    FILE* f = _wfopen(ini, L"wb");
    fputs("[graphics]\r\npreset = custom\r\ninternal_scale = auto\r\nauto_max = 3\r\n[window]\r\nfullscreen = exclusive\r\n", f);
    fclose(f);
    settings_load();
    char* t = read_file(ini);
    CHECK(ps2_settings.render_scale == SETTINGS_RS_AUTO && ps2_settings.win_fullscreen == SETTINGS_FULLSCREEN_EXCLUSIVE && t &&
          strstr(t, "render_scale = auto") && strstr(t, "mode = fullscreen") && !strstr(t, "internal_scale") && !strstr(t, "fullscreen = "),
          "settings_load migrates the file:\n%s", t ? t : "(none)");
    /* old callers of the old keys: redirected to the new ones */
    CHECK(settings_change_key("window", "fullscreen", 0, 1) && ps2_settings.win_fullscreen == 0 && settings_change_key("graphics", "internal_scale", 4, 1) &&
          ps2_settings.render_scale == 4, "settings_change of an old key");
    t = read_file(ini);
    CHECK(t && strstr(t, "mode = windowed") && strstr(t, "render_scale = 4x") && !strstr(t, "internal_scale"), "written as the new keys:\n%s", t ? t : "");
    DeleteFileW(ini);
    settings_load();
}

/* (pc-menu2) the new launch defaults: borderless; older files move to it unless the player chose otherwise */
static void test_defaults_migration(void) {
    ps2_settings_t s;
    settings_defaults(&s);
    CHECK(s.win_fullscreen == SETTINGS_FULLSCREEN_BORDERLESS && s.frame_rate == 60 && s.pacing == SETTINGS_PACING_AUTO,
          "defaults: borderless, 60 fps, pacing auto (%d %d %d)", s.win_fullscreen, s.frame_rate, s.pacing);
    static char tmpl[8192], out[16384];
    settings_format(&s, tmpl, sizeof tmpl);
    CHECK(strstr(tmpl, "mode = borderless\n") && strstr(tmpl, "[settings]") && strstr(tmpl, "version = 2\n"), "the template:\n%s", tmpl);
    CHECK(settings_migrate_defaults(tmpl, out, sizeof out) == 0, "a new file needs nothing");
    /* an old file with the old defaults: windowed -> borderless, version 2, everything else kept */
    const char* old = "; mine\r\n[display]\r\nmode = windowed ; old default\r\nframe_rate = 60\r\n[graphics]\r\npacing = auto\r\n";
    int n = settings_migrate_defaults(old, out, sizeof out);
    CHECK(n > 0 && strstr(out, "; mine\r\n") && strstr(out, "mode = borderless") && strstr(out, "; old default\r\n") && strstr(out, "frame_rate = 60\r\n") &&
          strstr(out, "pacing = auto\r\n") && strstr(out, "[settings]\r\nversion = 2\r\n"), "old defaults migrated (%d):\n%s", n, out);
    CHECK(settings_migrate_defaults(out, out + 8192, 8192) == 0, "once");
    /* the player chose windowed: kept */
    const char* chosen = "[display]\r\nmode = windowed\r\n[chosen]\r\ndisplay.mode = on\r\n";
    n = settings_migrate_defaults(chosen, out, sizeof out);
    CHECK(n > 0 && strstr(out, "mode = windowed\r\n") && strstr(out, "version = 2"), "a chosen value stays:\n%s", out);
    CHECK(settings_chosen_in(chosen, "display", "mode") && !settings_chosen_in(chosen, "display", "frame_rate"), "[chosen] read");
    /* fullscreen / 120 fps in an old file: not defaults, kept */
    n = settings_migrate_defaults("[display]\nmode = fullscreen\nframe_rate = 120\n", out, sizeof out);
    CHECK(n > 0 && strstr(out, "mode = fullscreen\n") && strstr(out, "frame_rate = 120\n"), "non-default values stay:\n%s", out);
    /* a file without a mode line (the oldest): borderless */
    n = settings_migrate_defaults("[display]\naspect = 4:3\n", out, sizeof out);
    settings_defaults(&s);
    settings_parse(out, &s, NULL, NULL);
    CHECK(n > 0 && s.win_fullscreen == SETTINGS_FULLSCREEN_BORDERLESS && s.aspect_w == 4 && s.ini_version == 2, "no mode line: borderless\n%s", out);
    /* on load, and the menu records the player's choice */
    wchar_t ini[MAX_PATH + 80];
    settings_user_path(L"sly2.ini", ini, MAX_PATH + 80);
    FILE* f = _wfopen(ini, L"wb");
    fputs("[display]\r\nmode = windowed\r\n", f);
    fclose(f);
    settings_load();
    char* t = read_file(ini);
    CHECK(ps2_settings.win_fullscreen == SETTINGS_FULLSCREEN_BORDERLESS && t && strstr(t, "mode = borderless") && strstr(t, "version = 2"),
          "settings_load moves an old file to the new defaults:\n%s", t ? t : "");
    m_change("display", "frame_rate", 120);
    t = read_file(ini);
    CHECK(t && strstr(t, "[chosen]") && strstr(t, "display.frame_rate = on"), "a menu change is recorded as chosen:\n%s", t ? t : "");
    m_change("display", "frame_rate", 60);
    DeleteFileW(ini);
    settings_load();
}

/* (pc-menu2) the game's font format (sly_font.c), built here in a fake EE RAM: 'A' = a square fan (outline) with a counter
 * (a flag-1 fan wound the other way), 'B' = a strip with an ADC vertex, '0' a fan; everything else empty */
static void w32(uint8_t* ram, uint32_t a, uint32_t v) { memcpy(ram + a, &v, 4); }
static uint32_t put_glyph_packet(uint8_t* ram, uint32_t at, int nseg, const int* kinds, const int* counts, const int* flags,
                                 const int16_t (*pts)[2], const uint8_t* adc) {
    uint32_t p = at + 8;                         /* (the DMA tag's first qword holds two VIF NOPs) */
    uint8_t* b = ram;
    #define VIF(code) do { w32(b, p, (code)); p += 4; } while (0)
    VIF(0x30000000u); for (int k = 0; k < 4; k++) VIF(0);                       /* STROW 0 */
    VIF(0x6401C007u); VIF((uint32_t)nseg); VIF(0);                              /* V2-32 @7: count */
    VIF(0x01000102u);                                                           /* STCYCL 2,1 */
    VIF(0x6A00C008u | (uint32_t)nseg << 16);                                    /* V3-8 @8 */
    uint8_t info[96] = { 0 };
    for (int s = 0; s < nseg; s++) { info[3 * s] = (uint8_t)kinds[s]; info[3 * s + 1] = (uint8_t)counts[s]; info[3 * s + 2] = (uint8_t)flags[s]; }
    for (int k = 0; k < (3 * nseg + 3) / 4; k++) { uint32_t v; memcpy(&v, info + 4 * k, 4); VIF(v); }
    VIF(0x6E00C009u | (uint32_t)nseg << 16); for (int s = 0; s < nseg; s++) VIF(flags[s] ? 0 : 0x80000000u);   /* V4-8 colours */
    VIF(0x01000101u);                                                           /* STCYCL 1,1 */
    VIF(0x20000000u); VIF(0x50505050u);                                         /* STMASK */
    int v = 0;
    uint32_t addr = 0x20;
    for (int s = 0; s < nseg; s++) {
        VIF(0x75008000u | (uint32_t)counts[s] << 16 | addr);                    /* V2-16 masked */
        for (int k = 0; k < counts[s]; k++) VIF((uint32_t)(uint16_t)pts[v + k][0] | (uint32_t)(uint16_t)pts[v + k][1] << 16);
        if (adc) {
            VIF(0x20000000u); VIF(0x3F3F3F3Fu);
            VIF(0x72008000u | (uint32_t)counts[s] << 16 | addr);                /* V1-8 masked: w = ADC */
            uint8_t f[32] = { 0 };
            for (int k = 0; k < counts[s]; k++) f[k] = adc[v + k];
            for (int k = 0; k < (counts[s] + 3) / 4; k++) { uint32_t x; memcpy(&x, f + 4 * k, 4); VIF(x); }
            VIF(0x20000000u); VIF(0x50505050u);
        }
        addr += (uint32_t)counts[s];
        v += counts[s];
    }
    VIF(0x14000028u);                                                           /* MSCAL 40 */
    while ((p - at) % 16) VIF(0);
    #undef VIF
    uint32_t qwc = (p - at) / 16 - 1;
    w32(ram, at, 0x60000000u | qwc);                                            /* RET tag */
    w32(ram, at + 4, 0);
    return p;
}

static void test_font(void) {
    const uint32_t size = 0x400000u, font = 0x200000u, chars = 0x200100u, adv = 0x200500u, glyphs = 0x201000u;
    uint8_t* ram = calloc(size, 1);
    w32(ram, font + 0x10, 3); w32(ram, font + 0x14, glyphs); w32(ram, font + 0x18, 3); w32(ram, font + 0x1C, adv);
    w32(ram, font + 0x20, 24); w32(ram, font + 0x24, chars);
    uint32_t cs[24];                             /* 0..9, A..H, a..f: the glyph of table entry i at glyphs + 0x40 * (i + 1) */
    for (int i = 0; i < 24; i++) cs[i] = i < 10 ? (uint32_t)('0' + i) : i < 18 ? (uint32_t)('A' + i - 10) : (uint32_t)('a' + i - 18);
    for (int i = 0; i < 24; i++) { w32(ram, chars + 4 * i, cs[i]); float a = 25.0f + i; memcpy(ram + adv + 4 * i, &a, 4); }
    uint32_t pk = 0x210000u;
    /* '0': a fan square 0..400 x -600..0 (units of 1/16) */
    { static const int16_t p[][2] = { { 0, 0 }, { 400, 0 }, { 400, -600 }, { 0, -600 } }; int k[] = { 1 }, n[] = { 4 }, f[] = { 0 };
      w32(ram, glyphs + 0x40 * 1 + 0x1C, pk); pk = (put_glyph_packet(ram, pk, 1, k, n, f, p, NULL) + 15) & ~15u; }
    /* 'A': square 0..640 x -640..0 and a counter 160..480 wound the other way, flagged */
    { static const int16_t p[][2] = { { 0, 0 }, { 640, 0 }, { 640, -640 }, { 0, -640 }, { 160, -160 }, { 160, -480 }, { 480, -480 }, { 480, -160 } };
      int k[] = { 1, 1 }, n[] = { 4, 4 }, f[] = { 0, 1 };
      w32(ram, glyphs + 0x40 * 11 + 0x1C, pk); pk = (put_glyph_packet(ram, pk, 2, k, n, f, p, NULL) + 15) & ~15u; }
    /* 'B': a strip of two triangles, then an ADC vertex and one more triangle far away */
    { static const int16_t p[][2] = { { 0, 0 }, { 320, 0 }, { 0, -320 }, { 320, -320 }, { 0, -640 }, { 320, -640 } };
      static const uint8_t adc[] = { 0, 0, 0, 0, 255, 0 };
      int k[] = { 2 }, n[] = { 6 }, f[] = { 0 };
      w32(ram, glyphs + 0x40 * 12 + 0x1C, pk); pk = (put_glyph_packet(ram, pk, 1, k, n, f, p, adc) + 15) & ~15u; }
    uint32_t at = 0;
    CHECK(slyf_find(ram, size, &at) && at == font, "font found at 0x%08X", at);
    slyf_mesh m = { 0 };
    m.tri = malloc(sizeof *m.tri * SLYF_MAX_TRIS);
    CHECK(slyf_extract(ram, size, font, &m) && m.has['A'] && m.has['a'] && fabsf(m.adv['B'] - 36.0f) < 1e-4f,
          "extracted (%d triangles)", m.ntri);
    int nA = 0, nB = 0, neg = 0;
    for (int i = 0; i < m.ntri; i++) { nA += m.tri[i].ch == 'A'; nB += m.tri[i].ch == 'B'; neg += m.tri[i].w < 0; }
    CHECK(nA == 4 && nB == 3 && neg == 2, "triangles: A %d (4), B %d (3: ADC skipped one), counter %d (2)", nA, nB, neg);
    slyf_atlas a;
    memset(&a, 0, sizeof a);
    int rok = slyf_raster(&m, 2.0f, &a);
    if (!rok || !a.px[0]) { CHECK(0, "no atlas"); free(m.tri); free(ram); return; }
    CHECK(rok &&a.levels > 3 && a.g['A'].present && !a.g['a'].present && a.g['a'].adv > 0, "atlas");
    /* 'A' at 2 px per unit: 80 x 80 px square, the counter 20..60 px: inside the counter 0, the ring 255 */
    const slyf_glyph* g = &a.g['A'];
    int cx = g->ax + (int)((20.0f - g->ox) * 2.0f), cy = g->ay + (int)((-20.0f - g->oy) * 2.0f);   /* (font units -> pixels) */
    int rx = g->ax + (int)((5.0f - g->ox) * 2.0f);
    uint8_t hole = a.px[0][(size_t)cy * a.w[0] + cx], ring = a.px[0][(size_t)cy * a.w[0] + rx];
    CHECK(hole == 0 && ring == 255, "the counter is a hole (%d), the ring filled (%d)", hole, ring);
    const slyf_glyph* gb = &a.g['B'];
    int bx = gb->ax + (int)((10.0f - gb->ox) * 2.0f), by_lo = gb->ay + (int)((-10.0f - gb->oy) * 2.0f), by_hi = gb->ay + (int)((-35.0f - gb->oy) * 2.0f), bx2 = gb->ax + (int)((15.0f - gb->ox) * 2.0f);
    CHECK(a.px[0][(size_t)by_lo * a.w[0] + bx] == 255 && a.px[0][(size_t)by_hi * a.w[0] + bx2] == 255, "strip filled");
    CHECK(fabsf(slyf_text_width(&a, "AB", 60.0f) - (35.0f + 36.0f)) < 1e-3f, "text width (one em = 60 units = 60 px)");
    char t[16];
    slyf_cp1252("\xC3\xA9x\xE2\x86\x92", t, sizeof t);
    CHECK((uint8_t)t[0] == 0xE9 && t[1] == 'x' && t[2] == '>', "UTF-8 -> cp1252");
    /* a damaged glyph packet: not used */
    w32(ram, glyphs + 0x40 * 11 + 0x1C, 0x00300000u);
    w32(ram, 0x300000u, 0x30000000u | 2);        /* (an unknown DMA tag id) */
    CHECK(!slyf_extract(ram, size, font, &m), "a damaged glyph is refused");
    slyf_free(&a);
    free(m.tri);
    free(ram);
}

static void test_persist(void) {
    wchar_t ini[MAX_PATH + 80];
    settings_user_path(L"sly2.ini", ini, MAX_PATH + 80);
    FILE* f = _wfopen(ini, L"wb");
    fputs("; my own notes\r\n[display]\r\naspect = 16:9   ; wide\r\nunknown_key = 1\r\n\r\n[controls]\r\nkey_cross = Space ; mine\r\n"
          "[graphics]\r\n; keep me\r\noutlines = ps2\r\n", f);
    fclose(f);
    settings_load();
    CHECK(ps2_settings.preset == SETTINGS_PRESET_BALANCED, "loaded preset %d", ps2_settings.preset);
    CHECK(settings_change_key("graphics", "outlines", 1, 1), "change outlines");
    char* t = read_file(ini);
    CHECK(t && strstr(t, "; my own notes\r\n") && strstr(t, "aspect = 16:9   ; wide\r\n") && strstr(t, "unknown_key = 1\r\n") &&
          strstr(t, "key_cross = Space ; mine\r\n") && strstr(t, "; keep me\r\noutlines = thin\r\n") && strstr(t, "preset = custom"),
          "comments, unknown keys and [controls] kept; outlines + preset written:\n%s", t ? t : "(none)");
    CHECK(ps2_settings.preset == SETTINGS_PRESET_CUSTOM, "now custom");
    /* (menu) the original look is Look = Retro: in effect at once, the player's own options stay in the file */
    CHECK(settings_change_key("graphics", "preset", SETTINGS_PRESET_ORIGINAL, 1) && ps2_settings.look == SETTINGS_LOOK_RETRO &&
          !ps2_settings.renderer_modern && ps2_settings.filters_faithful && ps2_settings.aspect_w == 4 && !ps2_settings.outlines_thin &&
          ps2_settings.preset == SETTINGS_PRESET_ORIGINAL, "preset original = Look Retro");
    t = read_file(ini);
    CHECK(t && strstr(t, "look = retro") && strstr(t, "outlines = thin") && strstr(t, "aspect = 16:9   ; wide") && strstr(t, "preset = custom") &&
          !strstr(t, "renderer_mode = reference") && !strstr(t, "filters = faithful"), "Retro: only the look written:\n%s", t ? t : "");
    CHECK(settings_own(&ps2_settings, settings_find("graphics", "outlines")) == 1, "the player's outlines kept aside");
    settings_load();
    CHECK(ps2_settings.look == SETTINGS_LOOK_RETRO && !ps2_settings.renderer_modern && ps2_settings.aspect_w == 4 &&
          settings_own(&ps2_settings, settings_find("graphics", "outlines")) == 1, "Retro across a restart");
    CHECK(settings_change_key("display", "aspect", 16 << 16 | 9, 1) && ps2_settings.look == SETTINGS_LOOK_FAITHFUL && ps2_settings.outlines_thin &&
          ps2_settings.renderer_modern && ps2_settings.aspect_w == 16, "an option Retro sets (F7): Retro ends, the player's options back");
    t = read_file(ini);
    CHECK(t && strstr(t, "look = faithful"), "written:\n%s", t ? t : "");
    CHECK(settings_change_key("graphics", "look", SETTINGS_LOOK_RETRO, 1) && settings_change_key("graphics", "look", SETTINGS_LOOK_FAITHFUL, 1) &&
          ps2_settings.outlines_thin && ps2_settings.aspect_w == 16 && ps2_settings.preset == SETTINGS_PRESET_CUSTOM, "Retro and back: as before");
    CHECK(settings_change_key("graphics", "preset", SETTINGS_PRESET_BALANCED, 1) && ps2_settings.aspect_w == 16, "Medium");
    settings_load();
    CHECK(ps2_settings.preset == SETTINGS_PRESET_BALANCED && ps2_settings.aspect_w == 16 && ps2_settings.render_scale == 100 &&
          ps2_settings.renderer_modern && ps2_settings.msaa == SETTINGS_MSAA_4X, "persisted across a restart");
    CHECK(!settings_change_key("audio", "master", 150, 1), "invalid value refused");
    _putenv("PS2_GS_SCALE=2");
    CHECK(settings_change_key("graphics", "render_scale", 6, 1), "env-locked change in memory");
    t = read_file(ini);
    CHECK(t && !strstr(t, "render_scale = 6x"), "env-locked option not written");
    _putenv("PS2_GS_SCALE=");
    settings_change_key("graphics", "render_scale", 100, 1);
    /* F-keys go through the table too */
    gfx_key(0x72, 0, 1);                         /* F3 */
    t = read_file(ini);
    CHECK(ps2_settings.outlines_thin && t && strstr(t, "outlines = thin") && strstr(t, "preset = custom"), "F3 writes outlines + preset");
    gfx_key(0x72, 0, 1);
    CHECK(ps2_settings.preset == SETTINGS_PRESET_BALANCED, "F3 back: Balanced again");
    settings_reset_section("audio");
    DeleteFileW(ini);
}

/* ---- the menu -------------------------------------------------------------------------------------------------- */
static int find_row(const menu_view* v, int id) {
    for (int k = 0; k < v->nrows; k++) if (v->rows[k].id == id) return k;
    return -1;
}
static menu_view V;
static void view(void) { menu_get_view(&V, host_cw, host_ch); }
static void go(int id) {                          /* select row id on the current page with up / down */
    view();
    int k = find_row(&V, id);
    for (int i = 0; i < 64 && V.sel != k; i++) { menu_nav_event(V.sel < k ? MENU_NAV_DOWN : MENU_NAV_UP, MENU_DEV_KEYBOARD); view(); }
}

static int changed_row(int id) { view(); int k = find_row(&V, id); return k >= 0 && V.rows[k].changed; }
static void apply_now(void) { menu_nav_event(MENU_NAV_APPLY, MENU_DEV_KEYBOARD); view(); }

static void test_menu_nav(void) {
    settings_load();
    use_host();
    CHECK(!menu_is_open(), "closed at start");
    push(INPUT_NAV_MENU, INPUT_SRC_KEYBOARD);        /* Esc with the menu closed: request_open */
    menu_tick();
    CHECK(menu_is_open() && host_open_requests == 1 && fake_menu_active && fake_flushes, "Esc opens (through the host)");
    view();
    CHECK(V.page == MENU_PAGE_GRAPHICS && V.sel == 0 && V.rows[0].id == MENU_ROW_GLOOK && !strcmp(V.rows[0].value, "Faithful") &&
          V.rows[1].id == MENU_ROW_PRESET && !strcmp(V.rows[1].value, "Medium") && V.rows[0].nchoices == 3,
          "Graphics: Look = Faithful (Retro/Faithful/Reimagined), Quality = Medium ('%s', '%s')", V.rows[0].value, V.rows[1].value);
    {   /* the page is short: Look, Quality, Resolution scale, Anti-aliasing, Outlines, Advanced graphics, Reset, Quit */
        static const int ids[] = { MENU_ROW_GLOOK, MENU_ROW_PRESET, MENU_ROW_SCALE, MENU_ROW_MSAA, MENU_ROW_OUTLINES, MENU_ROW_GFX_ADV,
                                   MENU_ROW_RESET, MENU_ROW_QUIT };
        int ok = V.nrows == 8;
        for (int k = 0; k < 8 && ok; k++) ok = V.rows[k].id == ids[k];
        CHECK(ok, "Graphics rows (%d)", V.nrows);
        for (int k = 0; k < V.nrows; k++) CHECK(!strstr(V.rows[k].label, "PS2") && !strstr(V.rows[k].value, "PS2") && !strstr(V.rows[k].desc, "PS2"),
                                                "no PS2 wording: '%s'", V.rows[k].label);
    }
    /* Quality High: staged (the rows below restaged), applied with Apply */
    go(MENU_ROW_PRESET);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.preset == SETTINGS_PRESET_MEDIUM && !ps2_settings.outlines_thin && !strcmp(V.rows[V.sel].value, "High") &&
          V.rows[V.sel].changed && V.pending == 2 && menu_pending() == 2, "High staged, not applied (%d changes)", V.pending);
    CHECK(changed_row(MENU_ROW_MSAA) && changed_row(MENU_ROW_OUTLINES) && !changed_row(MENU_ROW_SCALE) &&
          !strcmp(V.rows[find_row(&V, MENU_ROW_MSAA)].value, "High") && !strcmp(V.rows[find_row(&V, MENU_ROW_OUTLINES)].value, "Thin"),
          "the rows below show High's values, marked");
    apply_now();
    CHECK(ps2_settings.preset == SETTINGS_PRESET_HIGH && ps2_settings.msaa == SETTINGS_MSAA_8X && ps2_settings.outlines_thin && !V.pending &&
          !V.rows[V.sel].changed, "Apply: High in effect");
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);         /* Ultra, then Discard */
    view();
    CHECK(!strcmp(V.rows[V.sel].value, "Ultra") && V.pending == 1, "Ultra staged (%d)", V.pending);
    menu_nav_event(MENU_NAV_DISCARD, MENU_DEV_KEYBOARD);
    view();
    CHECK(!V.pending && !strcmp(V.rows[V.sel].value, "High") && ps2_settings.preset == SETTINGS_PRESET_HIGH, "Discard: High");
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    apply_now();
    CHECK(ps2_settings.preset == SETTINGS_PRESET_ULTRA && ps2_settings.render_scale == SETTINGS_PRESET_ULTRA_PCT, "Ultra applied");
    go(MENU_ROW_SCALE);
    CHECK(!strcmp(V.rows[V.sel].label, "Resolution scale") && strstr(V.rows[V.sel].value, "% (3200 x 1800)") &&
          strstr(V.rows[V.sel].desc, "Rendering at 3200 x 1800 -> shown at 2560 x 1440") != NULL, "Ultra at 1440p: '%s' / '%s'",
          V.rows[V.sel].value, V.rows[V.sel].desc);
    for (int k = 0; k < 2; k++) menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(!strcmp(V.rows[V.sel].value, "75% (1920 x 1080)") && ps2_settings.render_scale == SETTINGS_PRESET_ULTRA_PCT && V.rows[V.sel].changed &&
          !strcmp(V.rows[find_row(&V, MENU_ROW_PRESET)].value, "Custom"), "75%% staged: Quality shows Custom ('%s')", V.rows[V.sel].value);
    for (int k = 0; k < 10; k++) menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(!strcmp(V.rows[V.sel].value, "Auto (2560 x 1440)"), "leftmost: Auto ('%s')", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);         /* 75 % */
    /* Look Retro: the rows it sets show the original values, locked; applied with the staged 75 % as the player's own */
    go(MENU_ROW_GLOOK);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(!strcmp(V.rows[0].value, "Retro") && ps2_settings.look == SETTINGS_LOOK_FAITHFUL, "Retro staged");
    {
        int q = find_row(&V, MENU_ROW_PRESET), sc = find_row(&V, MENU_ROW_SCALE), o = find_row(&V, MENU_ROW_OUTLINES);
        CHECK(!strcmp(V.rows[q].value, "Original") && !V.rows[q].enabled && !V.rows[sc].enabled && !strcmp(V.rows[sc].value, "Auto (up to 100%)") &&
              !V.rows[o].enabled && !strcmp(V.rows[o].value, "Original") && strstr(V.rows[q].note, "Retro"),
              "Retro: Quality 'Original' locked ('%s'), scale '%s', outlines '%s'", V.rows[q].value, V.rows[sc].value, V.rows[o].value);
    }
    apply_now();
    CHECK(ps2_settings.look == SETTINGS_LOOK_RETRO && !ps2_settings.renderer_modern && ps2_settings.aspect_w == 4 &&
          ps2_settings.render_scale == SETTINGS_RS_AUTO && settings_own(&ps2_settings, settings_find("graphics", "render_scale")) == 75,
          "Retro applied, the player's 75%% kept aside");
    {
        wchar_t ini[MAX_PATH + 80];
        settings_user_path(L"sly2.ini", ini, MAX_PATH + 80);
        char* t = read_file(ini);
        CHECK(t && strstr(t, "look = retro") && strstr(t, "render_scale = 75%") && strstr(t, "outlines = thin") && !strstr(t, "renderer_mode = reference"),
              "sly2.ini: the look, the player's options:\n%s", t ? t : "");
    }
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);         /* Faithful again */
    apply_now();
    CHECK(ps2_settings.look == SETTINGS_LOOK_FAITHFUL && ps2_settings.render_scale == 75 && ps2_settings.outlines_thin &&
          ps2_settings.renderer_modern && ps2_settings.aspect_w == 16, "Faithful: the player's options back");
    /* Advanced graphics: Lighting, Textures, Renderer, Rendering ... */
    go(MENU_ROW_GFX_ADV);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_GFX_ADV && V.rows[0].id == MENU_ROW_LIGHTING && !strcmp(V.rows[0].value, "Enhanced") &&
          !strcmp(V.page_title, "Advanced graphics") && find_row(&V, MENU_ROW_TVFX) >= 0 && find_row(&V, MENU_ROW_HUD) >= 0 &&
          find_row(&V, MENU_ROW_QUIT) < 0, "Advanced graphics ('%s')", V.rows[0].value);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.lighting_enhanced && !strcmp(V.rows[0].value, "Original") && V.rows[0].changed, "Lighting Original staged");
    go(MENU_ROW_RENDERER);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.renderer_gpu == 1 && !strcmp(V.rows[V.sel].value, "Software (CPU)") && V.rows[V.sel].changed && V.pending == 2,
          "renderer staged ('%s', %d)", V.rows[V.sel].value, V.pending);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.pending == 1 && !V.rows[V.sel].changed, "back to the value in effect: not a change");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_GRAPHICS && V.rows[V.sel].id == MENU_ROW_GFX_ADV && V.rows[V.sel].changed && menu_is_open(),
          "Back: Graphics, on Advanced graphics (marked)");
    apply_now();
    CHECK(!ps2_settings.lighting_enhanced, "Lighting applied");
    /* reset Graphics: staged (Faithful + Medium) */
    go(MENU_ROW_RESET);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.dialog == MENU_DLG_NONE && V.pending > 0 && !strcmp(V.rows[find_row(&V, MENU_ROW_PRESET)].value, "Medium") &&
          ps2_settings.render_scale == 75, "Reset stages Medium (%d changes)", V.pending);
    apply_now();
    CHECK(ps2_settings.preset == SETTINGS_PRESET_MEDIUM && ps2_settings.render_scale == 100 && !ps2_settings.outlines_thin &&
          ps2_settings.lighting_enhanced && ps2_settings.textures_enhanced && ps2_settings.msaa == SETTINGS_MSAA_4X, "Medium applied");
    /* wrap-around and tabs */
    go(MENU_ROW_GLOOK);
    menu_nav_event(MENU_NAV_UP, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.sel == V.nrows - 1 && V.rows[V.sel].id == MENU_ROW_QUIT, "up from the first row: the last, Quit (%d)", V.sel);
    menu_nav_event(MENU_NAV_DOWN, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.sel == 0, "down from the last row wraps to the first (%d)", V.sel);
    /* leaving with changes: Apply / Discard / Cancel */
    go(MENU_ROW_OUTLINES);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    view();
    CHECK(menu_is_open() && V.dialog == MENU_DLG_PENDING && V.dlg_buttons == 3 && !strcmp(V.dlg_yes, "Apply") && !strcmp(V.dlg_alt, "Discard") &&
          !strcmp(V.dlg_no, "Cancel") && V.dlg_focus == 0 && strstr(V.dlg_text, "not applied"), "close: Apply / Discard / Cancel ('%s')", V.dlg_text);
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    view();
    CHECK(menu_is_open() && V.dialog == MENU_DLG_NONE && V.pending == 1, "Cancel: back to the menu, still staged");
    push(INPUT_NAV_MENU, INPUT_SRC_CONTROLLER);                /* the PS button: the same question */
    menu_tick();
    view();
    CHECK(V.dialog == MENU_DLG_PENDING, "PS button with changes: asks");
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_PLAYSTATION);
    view();
    CHECK(V.dlg_focus == 1, "right: Discard focused");
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_PLAYSTATION);
    CHECK(!menu_is_open() && !ps2_settings.outlines_thin && host_closed == 1, "Discard: closed, nothing applied");
    menu_open();
    view();
    CHECK(!V.pending, "reopened: nothing staged");
    go(MENU_ROW_OUTLINES);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);       /* Apply (focused) */
    CHECK(!menu_is_open() && ps2_settings.outlines_thin && host_closed == 2, "Apply: applied and closed");
    menu_open();
    go(MENU_ROW_OUTLINES);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CLOSE, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_APPLY, MENU_DEV_KEYBOARD);         /* (Square / F on the question: Apply) */
    CHECK(!menu_is_open() && !ps2_settings.outlines_thin, "Apply by its shortcut");
    menu_open();
    push(INPUT_NAV_TAB_NEXT, INPUT_SRC_CONTROLLER);
    menu_tick();
    view();
    CHECK(V.page == MENU_PAGE_DISPLAY && V.device == MENU_DEV_PLAYSTATION, "R1: Display, PlayStation hints");
    CHECK(find_row(&V, MENU_ROW_MONITOR) < 0 && find_row(&V, MENU_ROW_PACING) < 0 && find_row(&V, MENU_ROW_VRR) < 0 &&
          find_row(&V, MENU_ROW_FRAME_RATE) >= 0 && !strcmp(V.rows[find_row(&V, MENU_ROW_FRAME_RATE)].label, "Frame rate limit"),
          "Display: no Monitor with one monitor, pacing / VRR automatic");
    host_monitors = 2;
    view();
    CHECK(find_row(&V, MENU_ROW_MONITOR) >= 0, "two monitors: Monitor");
    host_monitors = 1;
    go(MENU_ROW_ASPECT);
    CHECK(!strcmp(V.rows[V.sel].value, "16:9 (widescreen)"), "aspect '%s'", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.aspect_w == 16 && !strcmp(V.rows[V.sel].value, "4:3 (original)") && V.rows[V.sel].changed, "4:3 staged");
    apply_now();
    CHECK(ps2_settings.aspect_w == 4 && ps2_settings.aspect_h == 3 && V.dialog == MENU_DLG_NONE, "4:3 applied (no Keep question)");
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    apply_now();
    CHECK(ps2_settings.aspect_w == 16 && ps2_settings.aspect_h == 9, "back to 16:9");
    /* resolution (windowed: the window size), staged - (pc-menu2: borderless is the default now; these steps start windowed) */
    ps2_settings.win_fullscreen = SETTINGS_FULLSCREEN_OFF;
    go(MENU_ROW_RESOLUTION);
    CHECK(V.rows[V.sel].enabled && V.rows[V.sel].cur < 0 && !strcmp(V.rows[V.sel].value, "2560 x 1440") && !strcmp(V.rows[V.sel].label, "Resolution"),
          "resolution: the window's ('%s')", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    view();
    CHECK(host_size_w == 0 && !strcmp(V.rows[V.sel].value, "960 x 540") && V.pending == 1 && V.rows[V.sel].changed, "window size staged ('%s')", V.rows[V.sel].value);
    /* display mode: staged; Fullscreen offers the monitor's modes */
    go(MENU_ROW_MODE);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.win_fullscreen == 0 && !strcmp(V.rows[V.sel].value, "Fullscreen") && V.rows[V.sel].changed, "Fullscreen staged ('%s')",
          V.rows[V.sel].value);
    go(MENU_ROW_RESOLUTION);
    CHECK(!strcmp(V.rows[V.sel].value, "Desktop (2560 x 1440)") && V.rows[V.sel].nchoices == 3, "fullscreen resolutions ('%s', %d)",
          V.rows[V.sel].value, V.rows[V.sel].nchoices);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    view();
    CHECK(!strcmp(V.rows[V.sel].value, "1920 x 1080") && ps2_settings.fullscreen_res == 0, "1920 x 1080 staged ('%s')", V.rows[V.sel].value);
    go(MENU_ROW_MODE);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(!strcmp(V.rows[find_row(&V, MENU_ROW_RESOLUTION)].value, "2560 x 1440 (desktop)") && !V.rows[find_row(&V, MENU_ROW_RESOLUTION)].enabled,
          "borderless: the desktop's resolution ('%s')", V.rows[find_row(&V, MENU_ROW_RESOLUTION)].value);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);         /* Fullscreen again, 1920 x 1080 */
    /* Apply -> Keep these display settings? (fake clock) */
    apply_now();
    CHECK(ps2_settings.win_fullscreen == SETTINGS_FULLSCREEN_EXCLUSIVE && ps2_settings.fullscreen_res == (1920 << 16 | 1080) && !V.pending &&
          V.dialog == MENU_DLG_KEEP && strstr(V.dlg_text, "15 s"), "applied, Keep? ('%s')", V.dlg_text);
    fake_now += 5200;
    menu_keep_tick();
    view();
    CHECK(V.dialog == MENU_DLG_KEEP && strstr(V.dlg_text, "10 s"), "counting down ('%s')", V.dlg_text);
    fake_now += 10000;
    menu_keep_tick();
    view();
    CHECK(V.dialog == MENU_DLG_NONE && ps2_settings.win_fullscreen == 0 && ps2_settings.fullscreen_res == 0, "no answer in 15 s: reverted");
    go(MENU_ROW_MODE);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);         /* borderless, Apply, Keep */
    apply_now();
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    fake_now += 20000;
    menu_keep_tick();
    view();
    CHECK(V.dialog == MENU_DLG_NONE && ps2_settings.win_fullscreen == SETTINGS_FULLSCREEN_BORDERLESS, "Keep: borderless stays");
    go(MENU_ROW_MODE);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);          /* windowed, Apply, Revert */
    apply_now();
    CHECK(ps2_settings.win_fullscreen == 0, "windowed applied");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    CHECK(ps2_settings.win_fullscreen == SETTINGS_FULLSCREEN_BORDERLESS && menu_is_open(), "Revert: borderless again");
    /* leaving with a display change: Apply -> Keep? -> Keep closes */
    go(MENU_ROW_MODE);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(menu_is_open() && V.dialog == MENU_DLG_KEEP && ps2_settings.win_fullscreen == 0, "leave + Apply: Keep these display settings?");
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    CHECK(!menu_is_open() && ps2_settings.win_fullscreen == 0, "Keep: closed");
    menu_open();
    menu_nav_event(MENU_NAV_TAB_NEXT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_TAB_PREV, MENU_DEV_KEYBOARD);
    m_change("display", "mode", 0);
    host_size_w = host_size_h = 0;
    go(MENU_ROW_RESOLUTION);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    apply_now();
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);       /* Keep */
    CHECK(host_size_w == 960 && host_size_h == 540, "window size applied: 960 x 540 (%d x %d)", host_size_w, host_size_h);
    /* (pc-fps) Frame rate: staged; Apply writes [display] frame_rate */
    go(MENU_ROW_FRAME_RATE);
    CHECK(V.rows[V.sel].id == MENU_ROW_FRAME_RATE && !strcmp(V.rows[V.sel].value, "60 fps (original)"), "frame rate '%s'", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.frame_rate == 60 && !strcmp(V.rows[V.sel].value, "120 fps"), "frame rate 120 staged (%d, '%s')",
          ps2_settings.frame_rate, V.rows[V.sel].value);
    for (int k = 0; k < 4; k++) menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    view();
    CHECK(!strcmp(V.rows[V.sel].value, "Display refresh rate"), "frame rate display ('%s')", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    apply_now();
    CHECK(ps2_settings.frame_rate == -1, "frame rate unlimited applied (%d)", ps2_settings.frame_rate);
    {
        char t[64];
        int fi = settings_find("display", "frame_rate");
        CHECK(!strcmp(settings_value_text(fi, -1, t, sizeof t), "unlimited") && !strcmp(settings_value_text(fi, 165, t, sizeof t), "165"),
              "frame_rate ini values");
        int v = 0;
        CHECK(settings_parse_value(fi, "display", &v) && v == -2 && settings_parse_value(fi, "144", &v) && v == 144 &&
              !settings_parse_value(fi, "20", &v), "frame_rate parse");
    }
    m_change("display", "frame_rate", 60);
    menu_nav_event(MENU_NAV_TAB_NEXT, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_AUDIO && V.rows[0].id == MENU_ROW_MASTER && V.rows[0].kind == MENU_ITEM_SLIDER, "Audio, master slider");
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.vol_master == 100 && V.rows[0].val == 90 && V.rows[0].changed, "master 90 staged (%d)", V.rows[0].val);
    /* mouse: click on the slider bar at 25 %, the footer's Apply */
    {
        menu_layout L;
        menu_compute_layout(host_cw, host_ch, V.nrows, V.scroll, &L);
        int x0 = L.left[0].x + L.left[0].w, x1 = L.right[0].x;
        int tx0 = x0 + (int)(10 * L.u), tx1 = x1 - (int)(96 * L.u) - (int)(10 * L.u);   /* (the track: menu.c slider_drag_to) */
        menu_mouse_button(tx0 + (tx1 - tx0) / 4, L.rows[0].y + L.rows[0].h / 2, 1);
        view();
        CHECK(V.rows[0].val == 25 && ps2_settings.vol_master == 100, "click on the bar: 25 staged (%d)", V.rows[0].val);
        /* (pc-menu2) the button held: dragging moves the value, the button up ends the drag */
        menu_mouse_move(tx0 + (tx1 - tx0) * 3 / 4, L.rows[5].y);         /* (anywhere vertically while dragging) */
        view();
        CHECK(V.rows[0].val == 75, "drag to 75 (%d)", V.rows[0].val);
        menu_mouse_up(tx0, L.rows[0].y);
        menu_mouse_move(tx0, L.rows[0].y + L.rows[0].h / 2);
        view();
        CHECK(V.rows[0].val == 75, "released: no more dragging (%d)", V.rows[0].val);
        menu_mouse_move(L.btn[2].x + 5, L.btn[2].y + 5);
        view();
        CHECK(V.hover_btn == 2, "hover on Apply");
        menu_mouse_button(L.btn[2].x + 5, L.btn[2].y + 5, 1);
        CHECK(ps2_settings.vol_master == 75 && !menu_pending(), "click on Apply: 75 (%d)", ps2_settings.vol_master);
        menu_mouse_button(tx0 + (tx1 - tx0) / 4, L.rows[0].y + L.rows[0].h / 2, 1);
        menu_mouse_up(0, 0);
        menu_mouse_button(L.btn[1].x + 5, L.btn[1].y + 5, 1);
        CHECK(ps2_settings.vol_master == 75 && !menu_pending(), "click on Discard");
        menu_mouse_button(L.tabs[MENU_PAGE_CONTROLS].x + 5, L.tabs[MENU_PAGE_CONTROLS].y + 5, 1);
        view();
        CHECK(V.page == MENU_PAGE_CONTROLS && V.rows[0].id == MENU_ROW_INVERT && !strcmp(V.rows[0].value, "Off"), "click on the Controls tab");
        menu_mouse_move(L.rows[0].x + 10, L.rows[0].y + 5);
        menu_mouse_button(L.right[0].x + 2, L.right[0].y + 5, 1);
        view();
        int saves = fake_saves;
        CHECK(!strcmp(V.rows[0].value, "Vertical") && !fake_cfg.invert_camera_y && V.rows[0].changed, "click on > : invert Vertical staged ('%s')", V.rows[0].value);
        apply_now();
        CHECK(fake_cfg.invert_camera_y == 1 && !fake_cfg.invert_camera_x && fake_saves == saves + 1, "applied: input_config_set saved");
    }
    go(MENU_ROW_INVERT);
    for (int k = 0; k < 2; k++) menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    apply_now();
    CHECK(fake_cfg.invert_camera_x && fake_cfg.invert_camera_y && !strcmp(V.rows[V.sel].value, "Both"), "Both ('%s')", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    apply_now();
    CHECK(!fake_cfg.invert_camera_x && !fake_cfg.invert_camera_y, "Off");
    go(MENU_ROW_CTL_ADV);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_CTL_ADV && V.rows[0].id == MENU_ROW_VIB_STRENGTH && find_row(&V, MENU_ROW_INV_BINO) >= 0 &&
          find_row(&V, MENU_ROW_DEADZONE) >= 0, "Advanced controls");
    int r0 = fake_rumbles;
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    CHECK(fake_cfg.vibration_strength == 100 && fake_rumbles == r0, "strength 90 staged, no rumble yet");
    apply_now();
    CHECK(fake_cfg.vibration_strength == 90 && fake_rumbles == r0 + 1, "applied: strength 90 + a test rumble");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    /* bindings: staged too */
    go(MENU_ROW_KEYS);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_KEYS && V.nrows == INPUT_ACT_COUNT + 1 && !strcmp(V.rows[0].value, "Space") && !strcmp(V.rows[0].value2, "-"),
          "keyboard bindings page (%d rows, '%s' '%s')", V.nrows, V.rows[0].value, V.rows[0].value2);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    CHECK(menu_capturing() && fake_capture == INPUT_CAPTURE_KEYBOARD, "capture begins (second column)");
    menu_capture_result(INPUT_CAPTURE_KEY, 'G');
    view();
    CHECK(!menu_capturing() && fake_cfg.key[INPUT_ACT_CROSS][1] != 'G' && !strcmp(V.rows[0].value2, "G") && V.rows[0].changed,
          "G staged as Cross's second key");
    go(MENU_ROW_BIND0 + INPUT_ACT_CIRCLE);
    menu_nav_event(MENU_NAV_DOWN, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_UP, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    menu_capture_result(INPUT_CAPTURE_KEY, VK_SPACE);
    apply_now();
    CHECK(fake_cfg.key[INPUT_ACT_CROSS][1] == 'G' && fake_cfg.key[INPUT_ACT_CIRCLE][0] == VK_SPACE, "applied: G on Cross, Space on Circle");
    CHECK(fake_cfg.key[INPUT_ACT_CROSS][0] != VK_SPACE, "a key does one thing: Space taken from Cross");
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    menu_capture_result(INPUT_CAPTURE_CANCELLED, 0);
    CHECK(!menu_capturing(), "cancelled");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_CONTROLS && V.rows[V.sel].id == MENU_ROW_KEYS && menu_is_open(), "back: Controls, on the Keyboard row");
    int closed0 = host_closed;
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    CHECK(!menu_is_open() && host_closed == closed0 + 1 && !fake_menu_active, "back on a main page closes (host told, input released)");
    menu_open();
    push(INPUT_NAV_MENU, INPUT_SRC_CONTROLLER);       /* Options / PS while open: close */
    menu_tick();
    CHECK(!menu_is_open() && host_closed == closed0 + 2, "MENU closes");
    /* (pc-polish) Quit to desktop: alone over the game (the pause menu's entry, the close button) */
    CHECK(menu_quit_request() && menu_is_open() && menu_quit_showing() && fake_menu_active, "quit asks");
    view();
    CHECK(V.dialog_only && V.dialog == MENU_DLG_QUIT && !strcmp(V.dlg_title, "Quit to desktop?") && V.dlg_buttons == 2 &&
          !strcmp(V.dlg_text, "Unsaved progress since the last save will be lost.") && !strcmp(V.dlg_yes, "Quit"), "the quit question");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    CHECK(!menu_is_open() && host_quits == 0 && host_closed == closed0 + 3, "Cancel: back to the game");
    menu_quit_request();
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_PLAYSTATION);
    CHECK(host_quits == 1 && !menu_is_open(), "Quit: host.quit");
    /* from the Options menu: the Quit row; Cancel returns to the page; right + Confirm = Cancel too */
    menu_open();
    go(MENU_ROW_QUIT);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.dialog == MENU_DLG_QUIT && !V.dialog_only, "Quit row asks");
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(menu_is_open() && V.dialog == MENU_DLG_NONE && host_quits == 1, "Cancel focused + Confirm: still in Options");
    {                                                 /* the mouse: the dialog's buttons */
        menu_layout L;
        menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
        view();
        menu_compute_layout(host_cw, host_ch, V.nrows, V.scroll, &L);
        menu_mouse_button(L.dlg.x + 3, L.dlg.y + 3, 1);
        CHECK(menu_quit_showing(), "a click beside the buttons does nothing");
        menu_mouse_button(L.dlg_yes.x + 5, L.dlg_yes.y + 5, 1);
        CHECK(host_quits == 2 && !menu_is_open(), "click on Quit");
        /* the leave question's three buttons */
        menu_open();
        menu_nav_event(MENU_NAV_TAB_NEXT, MENU_DEV_KEYBOARD);
        menu_nav_event(MENU_NAV_TAB_NEXT, MENU_DEV_KEYBOARD);
        go(MENU_ROW_FRAME_RATE);
        menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
        menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
        view();
        menu_mouse_move(L.dlg3[1].x + 5, L.dlg3[1].y + 5);
        view();
        CHECK(V.dialog == MENU_DLG_PENDING && V.hover_btn == 7, "hover on Discard (%d)", V.hover_btn);
        menu_mouse_button(L.dlg3[2].x + 5, L.dlg3[2].y + 5, 1);
        CHECK(menu_is_open() && menu_pending() == 1, "click on Cancel");
        menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
        menu_mouse_button(L.dlg3[1].x + 5, L.dlg3[1].y + 5, 1);
        CHECK(!menu_is_open() && ps2_settings.vol_master == 75, "click on Discard: closed, unchanged");
    }
    settings_reset_section("audio");
    settings_reset_section("display");
    m_change("display", "mode", 0);
    CHECK(host_mode == 0, "the display mode goes through the settings (host.set_window_mode unused)");
}

/* ---- drawing ------------------------------------------------------------------------------------------------ */
static const char* shots_dir;

static void save_raw(const char* name, const uint32_t* px, int w, int h) {
    if (!shots_dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s\\%s.bgra", shots_dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[2] = { (uint32_t)w, (uint32_t)h };
    fwrite(hdr, 4, 2, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

static void test_draw(void) {
    static const struct { int w, h; const char* tag; } sizes[] = { { 2560, 1440, "1440p" }, { 1280, 720, "720p" },
                                                                    { 1024, 768, "4x3" }, { 800, 600, "small" } };
    use_host();
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        int w = sizes[s].w, h = sizes[s].h;
        host_cw = w; host_ch = h;
        uint32_t* px = malloc((size_t)w * h * 4);
        menu_close();
        CHECK(!menu_draw(px, w, h, w) && px[0] == 0 && px[(size_t)w * h - 1] == 0, "closed: transparent");
        menu_open();
        for (int page = 0; page < MENU_PAGE_ALL; page++) {
            if (page == MENU_PAGE_KEYS || page == MENU_PAGE_PAD) {
                set_page(MENU_PAGE_CONTROLS);
                go(page == MENU_PAGE_KEYS ? MENU_ROW_KEYS : MENU_ROW_PAD);
                menu_nav_event(MENU_NAV_CONFIRM, page == MENU_PAGE_PAD ? MENU_DEV_PLAYSTATION : MENU_DEV_KEYBOARD);
            } else if (page >= MENU_PAGE_N) {    /* (menu) Advanced graphics / controls */
                LOCK(); set_page(menu_page_parent(page)); UNLOCK();
                go(page == MENU_PAGE_GFX_ADV ? MENU_ROW_GFX_ADV : MENU_ROW_CTL_ADV);
                menu_nav_event(MENU_NAV_CONFIRM, page == MENU_PAGE_CTL_ADV ? MENU_DEV_XBOX : MENU_DEV_KEYBOARD);
                view();
                CHECK(V.page == page, "sub-page %d opened", page);
            } else {
                LOCK(); set_page(page); UNLOCK();
                menu_nav_event(MENU_NAV_DOWN, page == MENU_PAGE_CONTROLS ? MENU_DEV_XBOX : page == MENU_PAGE_AUDIO ? MENU_DEV_PLAYSTATION
                                                                                                                    : MENU_DEV_KEYBOARD);
            }
            CHECK(menu_draw(px, w, h, w), "open: drawn");
            menu_layout L;
            view();
            menu_compute_layout(w, h, V.nrows, V.scroll, &L);
            CHECK((px[0] >> 24) == 150, "dimmed corner (alpha %u)", px[0] >> 24);
            uint32_t mid = px[(size_t)(L.panel.y + L.panel.h / 2) * w + L.panel.x + 3];
            CHECK((mid >> 24) >= 240, "panel opaque at %dx%d", w, h);
            CHECK(L.panel.x >= 0 && L.panel.y >= 0 && L.panel.x + L.panel.w <= w && L.panel.y + L.panel.h <= h && L.visible >= 6,
                  "panel inside the output, %d rows visible at %dx%d", L.visible, w, h);
            char name[64];
            snprintf(name, sizeof name, "pc_menu_%s_%s", menu_page_name(page), sizes[s].tag);
            for (char* c = name; *c; c++) { if (*c >= 'A' && *c <= 'Z') *c += 32; if (*c == ' ') *c = '_'; }
            save_raw(name, px, w, h);
            if (page >= MENU_PAGE_N) menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
        }
        if (s == 0) {                            /* (menu) staged changes, the leave question, the Retro look */
            LOCK(); set_page(MENU_PAGE_GRAPHICS); UNLOCK();
            settings_change_key("graphics", "preset", SETTINGS_PRESET_MEDIUM, 0);
            go(MENU_ROW_PRESET);
            menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_PLAYSTATION);    /* High: Quality, Anti-aliasing, Outlines marked */
            menu_draw(px, w, h, w);
            save_raw("pc_menu_changes_1440p", px, w, h);
            view();
            CHECK(V.pending == 2, "the changes picture: 2 changes (%d)", V.pending);
            menu_nav_event(MENU_NAV_BACK, MENU_DEV_PLAYSTATION);     /* leave: Apply / Discard / Cancel */
            menu_draw(px, w, h, w);
            save_raw("pc_menu_leave_1440p", px, w, h);
            menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
            menu_draw(px, w, h, w);
            save_raw("pc_menu_leave_keys_1440p", px, w, h);
            menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);        /* Cancel */
            menu_nav_event(MENU_NAV_DISCARD, MENU_DEV_KEYBOARD);
            go(MENU_ROW_GLOOK);
            menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);        /* Retro: the rows it sets locked */
            menu_draw(px, w, h, w);
            save_raw("pc_menu_retro_1440p", px, w, h);
            menu_nav_event(MENU_NAV_DISCARD, MENU_DEV_KEYBOARD);
            /* (pc-polish) a staged display mode, Apply on close?, Keep these display settings?, Quit */
            LOCK(); set_page(MENU_PAGE_DISPLAY); UNLOCK();
            go(MENU_ROW_MODE);
            menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
            menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
            go(MENU_ROW_RESOLUTION);
            menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
            menu_draw(px, w, h, w);
            save_raw("pc_menu_staged_1440p", px, w, h);
            menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
            menu_draw(px, w, h, w);
            save_raw("pc_menu_pending_1440p", px, w, h);
            menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);         /* Apply -> Keep? */
            fake_now += 3000;
            menu_keep_tick();
            menu_draw(px, w, h, w);
            save_raw("pc_menu_keep_1440p", px, w, h);
            menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);            /* Revert */
            CHECK(ps2_settings.win_fullscreen == 0 && ps2_settings.fullscreen_res == 0 && menu_is_open(), "reverted after the pictures");
            menu_close();
            menu_quit_request();
            menu_nav_event(MENU_NAV_NONE, MENU_DEV_PLAYSTATION);
            menu_draw(px, w, h, w);
            save_raw("pc_menu_quit_1440p", px, w, h);
            menu_nav_event(MENU_NAV_BACK, MENU_DEV_PLAYSTATION);
            menu_open();
        }
        menu_close();
        uint32_t* fps = malloc(200 * 60 * 4);
        menu_draw_fps(fps, 200, 60, 200, 59.9, 1.0f);
        CHECK((fps[30 * 200 + 100] >> 24) > 100, "fps box drawn");
        free(fps);
        free(px);
    }
    host_cw = 2560; host_ch = 1440;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots_dir = argv[++i];
    /* (pc-menu2) PS2_MENU_FONT_RAM=<EE RAM dump of a running game>: the pictures in the game's own font (a dump is the
     * player's data: never committed; without it the Windows stand-in draws the text) */
    const char* ram_path = getenv("PS2_MENU_FONT_RAM");
    if (ram_path && *ram_path) {
        FILE* f = fopen(ram_path, "rb");
        uint8_t* ram = malloc(32u << 20);
        size_t n = f && ram ? fread(ram, 1, 32u << 20, f) : 0;
        if (f) fclose(f);
        uint32_t at = 0;
        slyf_mesh m = { 0 };
        m.tri = malloc(sizeof *m.tri * SLYF_MAX_TRIS);
        slyf_atlas* a = calloc(1, sizeof *a);
        if (n && m.tri && a && slyf_find(ram, (uint32_t)n, &at) && slyf_extract(ram, (uint32_t)n, at, &m) && slyf_raster(&m, 2.5f, a)) {
            slyf_publish(a);
            printf("the game's font from %s (0x%08X, %d triangles)\n", ram_path, at, m.ntri);
        } else {
            printf("no font in %s\n", ram_path);
        }
        free(m.tri);
        free(ram);
    }
    test_table();
    test_presets();
    test_render_scale();
    test_migration();
    test_defaults_migration();
    test_font();
    test_persist();
    test_menu_nav();
    test_draw();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
