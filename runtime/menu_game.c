/* The Options menu in the game (menu_game.h). */
#include "menu_game.h"
#include "menu.h"
#include "menu_ext.h"
#include "menu_overlay.h"
#include "ps2_runtime.h"
#include "settings.h"
#include "audio_out.h"
#include "window_gpu.h"

/* the GPU renderer (not linked into every test program: weak, as in ps2_runtime.c) */
extern int  gpu_hw_active(void) __attribute__((weak));
extern void gpu_hw_request_switch(void) __attribute__((weak));
extern int  gpu_hw_auto_scale(void) __attribute__((weak));

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* window.c */
int  window_mode(void);
void window_set_client_size(int w, int h);
void window_display_size(int* w, int* h, int* hz);
int  window_monitor_count(void);
void window_monitor_info(int k, int* w, int* h, int* hz, int* primary);
/* ps2_runtime.c: the GPU renderer was started (a live switch to software and back is possible) */
int  ps2_gpu_started(void);

volatile int pcmenu_ee_pending;
volatile int pcmenu_pause_req;

/* ---- the game's pause menu (notes/pc-menu.md: the menu object 0x3D9ED0 = HUD manager 0x3D9E70 + 0x60) ---------- */
#define PM_DEPTH   0x003D9EF4u           /* page depth: 0 closed, 1 the list, 2 a sub-page, 3 memory card pages */
#define PM_ENTRIES 0x003D9EFCu           /* page 1's entries (our PCMENU_LIST while it is the pause list) */
#define PM_STATE   0x003DA0B8u           /* 7 = idle (takes input) */
#define SND_MGR    0x003DD040u           /* the sound manager: +0x150 music volume, +0x14C effects volume (floats 0..1) */
#define FN_SND_VOLUME 0x001FEA60u        /* (mgr, category, kind, f12 volume): the game's Options page uses it */
#define ENTRY_SIZE 32u

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, ps2_ram + a, 4); return v; }
static float rdf(uint32_t a) { float v; memcpy(&v, ps2_ram + a, 4); return v; }

static int pause_list_up(void) {
    if (!ps2_ram) return 0;
    int32_t depth = (int32_t)rd32(PM_DEPTH);
    return depth >= 1 && depth <= 3 && rd32(PM_ENTRIES) == PCMENU_LIST;
}

static int pause_idle(void) { return pause_list_up() && rd32(PM_STATE) == 7; }

/* f_001B0290, before its jal f_001AF4D8 (push page 1): $a1 = count, $a2 = the list (0x2F65D0, full at 7 entries).
 * The list is copied to PCMENU_LIST with "PC Settings" after "Options" (text 0x2C; else at the end) and the call
 * gets the copy: nothing else reads 0x2F65D0 (the page record keeps the pointer). */
void pcmenu_pause_list(ps2_ctx* ctx) {
    uint32_t n = ctx->r[5].uw[0], src = ctx->r[6].uw[0];
    if (n == 0 || n > 12) return;
    uint32_t dst = PCMENU_LIST, k = 0, placed = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t w = 0; w < ENTRY_SIZE; w += 4) ps2_w32(ctx, dst + k * ENTRY_SIZE + w, ps2_r32(ctx, src + i * ENTRY_SIZE + w));
        k++;
        if (!placed && ps2_r32(ctx, src + i * ENTRY_SIZE) == 0x2C) {      /* after "Options" */
            for (uint32_t w = 0; w < ENTRY_SIZE; w += 4) ps2_w32(ctx, dst + k * ENTRY_SIZE + w, 0);
            ps2_w32(ctx, dst + k * ENTRY_SIZE + 0x00, PCMENU_TEXT_ID);   /* text id; no callbacks */
            ps2_w32(ctx, dst + k * ENTRY_SIZE + 0x1C, 2);                /* flags: stay open */
            k++;
            placed = 1;
        }
    }
    if (!placed) {
        for (uint32_t w = 0; w < ENTRY_SIZE; w += 4) ps2_w32(ctx, dst + k * ENTRY_SIZE + w, 0);
        ps2_w32(ctx, dst + k * ENTRY_SIZE, PCMENU_TEXT_ID);
        ps2_w32(ctx, dst + k * ENTRY_SIZE + 0x1C, 2);
        k++;
    }
    static const char text[] = "PC Settings";
    for (uint32_t i = 0; i < sizeof text; i += 4) {
        uint32_t v = 0;
        for (uint32_t b = 0; b < 4 && i + b < sizeof text; b++) v |= (uint32_t)(uint8_t)text[i + b] << (8 * b);
        ps2_w32(ctx, PCMENU_STRING + i, v);
    }
    ctx->r[5].ud[0] = k;
    ctx->r[6].ud[0] = dst;
}

/* f_0013E368 (text id -> string, a leaf): our id */
int pcmenu_text(ps2_ctx* ctx) {
    if (ctx->r[5].uw[0] != PCMENU_TEXT_ID) return 0;
    ctx->r[2].ud[0] = PCMENU_STRING;
    return 1;
}

/* f_001AEEE0, Cross on an entry ($s0) of an idle page */
void pcmenu_select(ps2_ctx* ctx) {
    if (ps2_r32(ctx, ctx->r[16].uw[0]) == PCMENU_TEXT_ID) menu_open();
}

/* f_001D9648 after the game asked whether START was pressed ($v0): Esc / PS button pause the game as START does
 * (only where the game itself would take START for its pause menu) */
void pcmenu_start(ps2_ctx* ctx) {
    if (pcmenu_pause_req) {
        pcmenu_pause_req = 0;
        ctx->r[2].ud[0] = 1;
    }
}

/* ---- opening ------------------------------------------------------------------------------------------------ */
volatile LONG pcmenu_open_pending;
static volatile ULONGLONG open_deadline;

/* Esc / PS button / "menu" token: the game's pause menu first (the game is paused while the menu is open), then the
 * menu; where the game cannot pause (title, movies, loading) the menu opens over it after 0.5 s */
static void request_open(void) {
    if (menu_is_open()) return;
    if (pause_list_up()) { menu_open(); return; }
    open_deadline = GetTickCount64() + 500;
    InterlockedExchange(&pcmenu_open_pending, 1);
    pcmenu_pause_req = 1;
}

/* both the window thread (menu_tick) and the EE (main loop) */
static void tick(void) {
    if (!pcmenu_open_pending) return;
    if (pause_idle() || GetTickCount64() > open_deadline) {
        if (!InterlockedExchange(&pcmenu_open_pending, 0)) return;
        pcmenu_pause_req = 0;
        menu_open();
    }
}

static void closed(void) { InterlockedExchange(&pcmenu_open_pending, 0); }

/* ---- audio ---------------------------------------------------------------------------------------------------- */
static volatile int focused = 1;
static int ee_applied;                   /* the game's volumes have been set once (its sound manager is up) */

static void update_gain(void) {
    double g = ps2_settings.vol_master / 100.0;
    g = g * g;                           /* (a perceptual curve: 50% = -12 dB) */
    if (!focused && !ps2_settings.audio_background) g = 0;
    audio_set_gain((float)g);
}

void menu_focus(int active) {
    focused = active != 0;
    update_gain();
}

/* The game's music / effects volumes (its sound manager: +0x150 = kind 1 category 1, +0x14C = kind 1 category 0)
 * changed after we set them. On its own Options page (pause menu depth >= 2, page 2 = the options list 0x2F6470)
 * that is the player: the menu takes the new values over (sly2.ini too). Anywhere else (a loaded save sets the
 * volumes it was saved with): ours again. */
#define PM_PAGE2_ENTRIES 0x003D9F18u
#define GAME_OPTIONS_LIST 0x002F6470u
static float applied_mu = -1.0f, applied_ef = -1.0f;
static int adopting;

static int game_options_open(void) {
    int32_t depth = (int32_t)rd32(PM_DEPTH);
    return depth >= 2 && depth <= 3 && rd32(PM_PAGE2_ENTRIES) == GAME_OPTIONS_LIST;
}

static void watch_volumes(void) {
    float mu = rdf(SND_MGR + 0x150), ef = rdf(SND_MGR + 0x14C);
    if (!(mu >= 0.0f && mu <= 1.0f) || !(ef >= 0.0f && ef <= 1.0f)) return;
    if (fabsf(mu - applied_mu) < 0.002f && fabsf(ef - applied_ef) < 0.002f) return;
    if (!game_options_open()) { pcmenu_ee_pending = 1; return; }
    applied_mu = mu;
    applied_ef = ef;
    adopting = 1;                        /* (no write back into the game: it has them) */
    settings_change_key("audio", "music", (int)lrintf(mu * 100.0f), 1);
    settings_change_key("audio", "effects", (int)lrintf(ef * 100.0f), 1);
    adopting = 0;
}

static void call_volume(ps2_ctx* ctx, int category, float v) {
    static ps2_ctx saved;                /* the game's registers stay as they were (aspect.c aspect_apply does the same) */
    saved = *ctx;
    ctx->r[29].ud[0] = (uint64_t)(int64_t)(int32_t)((ctx->r[29].uw[0] - 0x400u) & ~15u);
    ctx->r[4].ud[0] = SND_MGR;
    ctx->r[5].ud[0] = (uint64_t)category;
    ctx->r[6].ud[0] = 1;
    ctx->f[12].f = v;
    ctx->r[31].ud[0] = saved.r[31].ud[0];
    ps2_call(ctx, FN_SND_VOLUME);
    *ctx = saved;
}

/* main loop (EE): the volumes into the game's sound manager - as its Options page sets them: music = category 1,
 * effects = categories 0 and 3 */
void pcmenu_ee_frame(ps2_ctx* ctx) {
    tick();
    if (ee_applied && !pcmenu_ee_pending) watch_volumes();
    if (!pcmenu_ee_pending) return;
    pcmenu_ee_pending = 0;
    float mu = ps2_settings.vol_music / 100.0f, ef = ps2_settings.vol_effects / 100.0f;
    call_volume(ctx, 1, mu);
    call_volume(ctx, 0, ef);
    call_volume(ctx, 3, ef);
    applied_mu = rdf(SND_MGR + 0x150);   /* (what the game stored: the values watched from now on) */
    applied_ef = rdf(SND_MGR + 0x14C);
    if (!ee_applied) fprintf(stderr, "[menu] game volumes: music %d%%, effects %d%%\n", ps2_settings.vol_music, ps2_settings.vol_effects);
    ee_applied = 1;
}

static void audio_applier(int option) {
    (void)option;
    update_gain();
    if (!adopting) pcmenu_ee_pending = 1;
}

/* ---- renderer, pacing ------------------------------------------------------------------------------------------ */
static void renderer_applier(int option) {
    (void)option;
    if (!ps2_gpu_started()) return;      /* started on the software renderer: the GPU one at the next start */
    if (gpu_hw_active && gpu_hw_request_switch && (ps2_settings.renderer_gpu != 0) != (gpu_hw_active() != 0)) {
        fprintf(stderr, "[menu] renderer: switching to the %s renderer\n", ps2_settings.renderer_gpu ? "GPU" : "software");
        gpu_hw_request_switch();
    }
}

static void pacing_applier(int option) {
    (void)option;
    if (!pacing_apply_mode(ps2_settings.pacing)) fprintf(stderr, "[menu] frame pacing %d applies at the next start\n", ps2_settings.pacing);
}

/* [display] aspect / hud without a window (headless runs; window.c registers its own, which also remembers the wide
 * aspect for F7): the game takes it at its next frame */
static void display_applier(int option) {
    (void)option;
    aspect_request(ps2_settings.aspect_w, ps2_settings.aspect_h, ps2_settings.hud_centered);
}

/* ---- host ------------------------------------------------------------------------------------------------------ */
static int h_gpu_active(void) { return ps2_gpu_started() && gpu_hw_active && gpu_hw_active(); }
static int h_auto_now(void) { return gpu_hw_auto_scale ? gpu_hw_auto_scale() : 0; }
static int h_in_game(void) { return ee_applied; }
static void h_set_mode(int m) { settings_change_key("window", "fullscreen", m, 1); }

void menu_game_init(int headless) {
    menu_host h;
    memset(&h, 0, sizeof h);
    h.gpu_active = h_gpu_active;
    h.gpu_started = ps2_gpu_started;
    h.auto_scale_now = h_auto_now;
    h.output_size = headless ? NULL : window_client_size;
    h.display_size = headless ? NULL : window_display_size;
    h.monitor_count = headless ? NULL : window_monitor_count;
    h.monitor_info = headless ? NULL : window_monitor_info;
    h.window_mode = headless ? NULL : window_mode;
    h.set_window_mode = h_set_mode;
    h.set_client_size = headless ? NULL : window_set_client_size;
    h.in_game = h_in_game;
    h.request_open = request_open;
    h.closed = closed;
    h.tick = tick;
    menu_set_host(&h);
    settings_on_apply(SET_APPLY_AUDIO, audio_applier);
    settings_on_apply(SET_APPLY_RENDERER, renderer_applier);
    settings_on_apply(SET_APPLY_PACING, pacing_applier);
    if (headless) settings_on_apply(SET_APPLY_DISPLAY, display_applier);
    update_gain();
    pcmenu_ee_pending = 1;               /* the [audio] volumes into the game once its main loop runs */
}

/* PS2_INPUT tokens (window.c): menu mup mdown mleft mright mok mback mtabn mtabp menushot */
void menu_script(int k) {
    static const menu_nav navs[] = { MENU_NAV_NONE, MENU_NAV_UP, MENU_NAV_DOWN, MENU_NAV_LEFT, MENU_NAV_RIGHT,
                                     MENU_NAV_CONFIRM, MENU_NAV_BACK, MENU_NAV_TAB_NEXT, MENU_NAV_TAB_PREV };
    if (k == 0) {
        if (menu_is_open()) menu_close();
        else request_open();
    } else if (k > 0 && k < (int)(sizeof navs / sizeof navs[0])) {
        menu_nav_event(navs[k], MENU_DEV_PLAYSTATION);
    } else if (k == 9) {
        static int n;
        const char* dir = getenv("PS2_MENU_SHOTS");
        if (!dir || !*dir) dir = getenv("PS2_DUMP_FRAMES");
        char path[600];
        snprintf(path, sizeof path, "%s/menu_%02d.png", dir && *dir ? dir : ".", n++);
        overlay_request_shot(path);
    }
}
