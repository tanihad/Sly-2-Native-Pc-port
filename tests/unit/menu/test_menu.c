/* Tests of the settings model (runtime/settings.c: the option table, presets, sly2.ini round trips keeping
 * comments, validation) and of the Options menu (runtime/menu.c: pages, navigation, values, mouse, bindings; and
 * runtime/menu_draw.c: the picture at several output sizes). No window, no game: the input subsystem is faked here.
 *
 * Usage: test_menu.exe [--shots <dir>]   (--shots: every page as raw BGRA files <dir>\<name>.bgra for
 *        scripts/menu_shots.py, which makes the PNGs and the contact sheet) */
#include "settings.c"
#include "gfx.c"
#include "menu.c"
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
static int h_mode(void) { return host_mode; }
static void h_set_mode(int m) { host_mode = m; }
static void h_set_size(int w, int h) { host_size_w = w; host_size_h = h; }
static void h_closed(void) { host_closed++; }
static void h_req(void) { host_open_requests++; menu_open(); }
static int h_true(void) { return 1; }
static int h_auto(void) { return 3; }

static void use_host(void) {
    menu_host h;
    memset(&h, 0, sizeof h);
    h.gpu_active = h_true;
    h.gpu_started = h_true;
    h.auto_scale_now = h_auto;
    h.output_size = h_out;
    h.display_size = h_disp;
    h.window_mode = h_mode;
    h.set_window_mode = h_set_mode;
    h.set_client_size = h_set_size;
    h.request_open = h_req;
    h.closed = h_closed;
    h.in_game = h_true;
    menu_set_host(&h);
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
    for (int p = 0; p < SETTINGS_PRESET_CUSTOM; p++) {
        settings_defaults(&s);
        s.aspect_w = 21; s.aspect_h = 9;
        settings_apply_preset(&s, p);
        CHECK(settings_detect_preset(&s) == p, "preset %d detected after applying it (%d)", p, settings_detect_preset(&s));
    }
    settings_defaults(&s);
    settings_apply_preset(&s, SETTINGS_PRESET_ORIGINAL);
    CHECK(s.internal_scale == 1 && s.filters_faithful && !s.outlines_thin && s.aspect_w == 4 && s.aspect_h == 3, "Original: 1x faithful 4:3");
    settings_apply_preset(&s, SETTINGS_PRESET_QUALITY);
    CHECK(s.internal_scale == SETTINGS_SCALE_NATIVE && s.outlines_thin && !s.filters_faithful, "Quality: native, thin");
    CHECK(s.aspect_w == 4, "Quality keeps the aspect (the menu restores widescreen when leaving Original)");
    settings_apply_preset(&s, SETTINGS_PRESET_PERFORMANCE);
    CHECK(s.internal_scale == SETTINGS_SCALE_AUTO && s.auto_max == 2 && !s.outlines_thin, "Performance: auto up to 2x");
    s.outlines_thin = 1;
    CHECK(settings_detect_preset(&s) == SETTINGS_PRESET_CUSTOM, "changing an option -> custom");
    /* a preset in the file wins over its options; custom leaves them */
    settings_defaults(&s);
    settings_parse("[graphics]\ninternal_scale = 5\npreset = quality\n", &s, NULL, NULL);
    CHECK(s.internal_scale == SETTINGS_SCALE_NATIVE && s.preset == SETTINGS_PRESET_QUALITY, "preset = quality wins");
    settings_defaults(&s);
    settings_parse("[graphics]\npreset = custom\ninternal_scale = 5\n", &s, NULL, NULL);
    CHECK(s.internal_scale == 5 && s.preset == SETTINGS_PRESET_CUSTOM, "preset = custom keeps the options");
    settings_defaults(&s);
    settings_parse("[graphics]\ninternal_scale = 1\nfilters = faithful\n[display]\naspect = 4:3\n", &s, NULL, NULL);
    CHECK(s.preset == SETTINGS_PRESET_ORIGINAL, "an old file without preset: derived (Original)");
    CHECK(gfx_native_scale(1440) == 4 && gfx_native_scale(1080) == 3 && gfx_native_scale(2160) == 5 && gfx_native_scale(448) == 1 &&
          gfx_native_scale(0) == 3, "native scale");
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
    CHECK(settings_change_key("graphics", "preset", SETTINGS_PRESET_ORIGINAL, 1), "Original");
    t = read_file(ini);
    CHECK(t && strstr(t, "preset = original") && strstr(t, "internal_scale = 1") && strstr(t, "filters = faithful") &&
          strstr(t, "aspect = 4:3   ; wide") && strstr(t, "outlines = ps2"), "Original's options written:\n%s", t ? t : "");
    CHECK(settings_change_key("graphics", "preset", SETTINGS_PRESET_BALANCED, 1) && ps2_settings.aspect_w == 16, "leaving Original: 16:9");
    settings_load();
    CHECK(ps2_settings.preset == SETTINGS_PRESET_BALANCED && ps2_settings.aspect_w == 16 && ps2_settings.internal_scale == SETTINGS_SCALE_AUTO,
          "persisted across a restart");
    CHECK(!settings_change_key("audio", "master", 150, 1), "invalid value refused");
    _putenv("PS2_GS_SCALE=2");
    CHECK(settings_change_key("graphics", "internal_scale", 6, 1), "env-locked change in memory");
    t = read_file(ini);
    CHECK(t && !strstr(t, "internal_scale = 6"), "env-locked option not written");
    _putenv("PS2_GS_SCALE=");
    settings_change_key("graphics", "internal_scale", SETTINGS_SCALE_AUTO, 1);
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

static void test_menu_nav(void) {
    settings_load();
    use_host();
    CHECK(!menu_is_open(), "closed at start");
    push(INPUT_NAV_MENU, INPUT_SRC_KEYBOARD);        /* Esc with the menu closed: request_open */
    menu_tick();
    CHECK(menu_is_open() && host_open_requests == 1 && fake_menu_active && fake_flushes, "Esc opens (through the host)");
    view();
    CHECK(V.page == MENU_PAGE_GRAPHICS && V.sel == 0 && V.rows[0].id == MENU_ROW_PRESET && !strcmp(V.rows[0].value, "Balanced"),
          "Graphics, Preset = Balanced ('%s')", V.rows[0].value);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    CHECK(ps2_settings.preset == SETTINGS_PRESET_QUALITY && ps2_settings.internal_scale == SETTINGS_SCALE_NATIVE && ps2_settings.outlines_thin,
          "right: Quality");
    go(MENU_ROW_SCALE);
    view();
    CHECK(strstr(V.rows[V.sel].value, "Native - 4x (2560 x 1792)") != NULL, "native at 1440p 16:9: '%s'", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    CHECK(ps2_settings.internal_scale == 8 && ps2_settings.preset == SETTINGS_PRESET_CUSTOM, "left: 8x, Custom");
    view();
    CHECK(!strcmp(V.rows[0].value, "Custom"), "preset row shows Custom");
    CHECK(strstr(V.rows[V.sel].value, "8x (5632 x 3584)") != NULL, "8x 16:9 size (11 columns per pixel) '%s'", V.rows[V.sel].value);
    /* reset the page: the confirm dialog */
    go(MENU_ROW_RESET);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.confirm_reset, "reset asks");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    view();
    CHECK(!V.confirm_reset && menu_is_open() && ps2_settings.internal_scale == 8, "back cancels");
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    CHECK(ps2_settings.preset == SETTINGS_PRESET_BALANCED && ps2_settings.internal_scale == SETTINGS_SCALE_AUTO && !ps2_settings.outlines_thin,
          "reset Graphics: Balanced");
    /* wrap-around and tabs */
    go(MENU_ROW_PRESET);
    menu_nav_event(MENU_NAV_UP, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.sel == V.nrows - 1, "up from the first row: the last (%d)", V.sel);
    menu_nav_event(MENU_NAV_DOWN, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.sel == 0, "down from the last row wraps to the first (%d)", V.sel);
    push(INPUT_NAV_TAB_NEXT, INPUT_SRC_CONTROLLER);
    menu_tick();
    view();
    CHECK(V.page == MENU_PAGE_DISPLAY && V.device == MENU_DEV_PLAYSTATION, "R1: Display, PlayStation hints");
    go(MENU_ROW_ASPECT);
    view();
    CHECK(!strcmp(V.rows[V.sel].value, "16:9 (widescreen)"), "aspect '%s'", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    view();
    CHECK(ps2_settings.aspect_w == 4 && ps2_settings.aspect_h == 3, "aspect 4:3");
    CHECK(!V.rows[find_row(&V, MENU_ROW_HUD)].enabled, "HUD layout disabled at 4:3");
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    CHECK(ps2_settings.aspect_w == 16 && ps2_settings.aspect_h == 9, "back to 16:9");
    go(MENU_ROW_WINSIZE);
    view();
    CHECK(V.rows[V.sel].enabled && V.rows[V.sel].cur < 0 && !strcmp(V.rows[V.sel].value, "2560 x 1440"),
          "window size: the current one ('%s')", V.rows[V.sel].value);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    CHECK(host_size_w == 960 && host_size_h == 540, "first window size 960 x 540 (%d x %d)", host_size_w, host_size_h);
    go(MENU_ROW_MODE);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    CHECK(ps2_settings.win_fullscreen == 1, "display mode: borderless (%d)", ps2_settings.win_fullscreen);
    go(MENU_ROW_PACING);
    view();
    CHECK(!strcmp(V.rows[V.sel].note, "Applies at the next start"), "pacing note (stand-in): '%s'", V.rows[V.sel].note);
    menu_nav_event(MENU_NAV_TAB_NEXT, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_AUDIO && V.rows[0].id == MENU_ROW_MASTER && V.rows[0].kind == MENU_ITEM_SLIDER, "Audio, master slider");
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    CHECK(ps2_settings.vol_master == 90, "master 90 (%d)", ps2_settings.vol_master);
    /* mouse: click on the slider bar at 25 % */
    {
        menu_layout L;
        view();
        menu_compute_layout(host_cw, host_ch, V.nrows, V.scroll, &L);
        int x0 = L.left[0].x + L.left[0].w, x1 = L.right[0].x;
        menu_mouse_button(x0 + (x1 - x0) / 4, L.rows[0].y + L.rows[0].h / 2, 1);
        CHECK(ps2_settings.vol_master == 25, "click on the bar: 25 (%d)", ps2_settings.vol_master);
        menu_mouse_button(L.tabs[MENU_PAGE_CONTROLS].x + 5, L.tabs[MENU_PAGE_CONTROLS].y + 5, 1);
        view();
        CHECK(V.page == MENU_PAGE_CONTROLS, "click on the Controls tab");
        menu_mouse_move(L.rows[2].x + 10, L.rows[2].y + 5);
        view();
        CHECK(V.sel == 2 && V.hover_row == 2, "hover selects row 2");
        menu_mouse_button(L.right[2].x + 2, L.right[2].y + 5, 1);
        CHECK(fake_cfg.invert_camera_x == 1 && fake_saves > 0, "click on > : invert X on (input_config_set saved)");
    }
    go(MENU_ROW_VIB_STRENGTH);
    int r0 = fake_rumbles;
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    CHECK(fake_cfg.vibration_strength == 90 && fake_rumbles == r0 + 1, "strength 90 + test rumble");
    /* bindings */
    go(MENU_ROW_KEYS);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_KEYS && V.nrows == INPUT_ACT_COUNT + 1 && !strcmp(V.rows[0].value, "Space") && !strcmp(V.rows[0].value2, "-"),
          "keyboard bindings page (%d rows, '%s' '%s')", V.nrows, V.rows[0].value, V.rows[0].value2);
    menu_nav_event(MENU_NAV_RIGHT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    CHECK(menu_capturing() && fake_capture == INPUT_CAPTURE_KEYBOARD, "capture begins (second column)");
    menu_capture_result(INPUT_CAPTURE_KEY, 'G');
    CHECK(!menu_capturing() && fake_cfg.key[INPUT_ACT_CROSS][1] == 'G', "G bound as Cross's second key");
    go(MENU_ROW_BIND0 + INPUT_ACT_CIRCLE);
    menu_nav_event(MENU_NAV_DOWN, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_UP, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_LEFT, MENU_DEV_KEYBOARD);
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    menu_capture_result(INPUT_CAPTURE_KEY, VK_SPACE);
    CHECK(fake_cfg.key[INPUT_ACT_CIRCLE][0] == VK_SPACE, "Space bound to Circle (row 1, first column)");
    CHECK(fake_cfg.key[INPUT_ACT_CROSS][0] != VK_SPACE, "a key does one thing: Space taken from Cross");
    menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_KEYBOARD);
    menu_capture_result(INPUT_CAPTURE_CANCELLED, 0);
    CHECK(!menu_capturing(), "cancelled");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    view();
    CHECK(V.page == MENU_PAGE_CONTROLS && V.rows[V.sel].id == MENU_ROW_KEYS && menu_is_open(), "back: Controls, on the Keyboard row");
    menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
    CHECK(!menu_is_open() && host_closed == 1 && !fake_menu_active, "back on a main page closes (host told, input released)");
    menu_open();
    push(INPUT_NAV_MENU, INPUT_SRC_CONTROLLER);       /* Options / PS while open: close */
    menu_tick();
    CHECK(!menu_is_open() && host_closed == 2, "MENU closes");
    settings_reset_section("audio");
    settings_reset_section("display");
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
            for (char* c = name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
            save_raw(name, px, w, h);
            if (page == MENU_PAGE_KEYS || page == MENU_PAGE_PAD) menu_nav_event(MENU_NAV_BACK, MENU_DEV_KEYBOARD);
        }
        if (s == 0) {                            /* the reset dialog and a binding capture */
            LOCK(); set_page(MENU_PAGE_GRAPHICS); UNLOCK();
            menu_nav_event(MENU_NAV_UP, MENU_DEV_PLAYSTATION);
            menu_nav_event(MENU_NAV_CONFIRM, MENU_DEV_PLAYSTATION);
            menu_draw(px, w, h, w);
            save_raw("pc_menu_reset_1440p", px, w, h);
            menu_nav_event(MENU_NAV_BACK, MENU_DEV_PLAYSTATION);
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
    test_table();
    test_presets();
    test_persist();
    test_menu_nav();
    test_draw();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
