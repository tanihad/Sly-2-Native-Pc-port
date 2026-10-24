/* The Options menu in the game (menu_game.h). */
#include "menu_game.h"
#include "menu.h"
#include "menu_ext.h"
#include "menu_overlay.h"
#include "ps2_runtime.h"
#include "settings.h"
#include "audio_out.h"
#include "input.h"
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
int  window_display_modes(int* wh, int max);
int  window_request_quit(void);
extern void (*window_exit_hook)(int code);
/* ps2_runtime.c: the GPU renderer was started (a live switch to software and back is possible) */
int  ps2_gpu_started(void);

volatile int pcmenu_ee_pending;
volatile int pcmenu_pause_req;

/* ---- the game's pause menu (notes/pc-menu.md: the menu object 0x3D9ED0 = HUD manager 0x3D9E70 + 0x60) ---------- */
#define PM_DEPTH   0x003D9EF4u           /* page depth: 0 closed, 1 the list, 2 a sub-page, 3 memory card pages */
#define PM_ENTRIES 0x003D9EFCu           /* page 1's entries (our PCMENU_LIST while it is the pause list) */
#define PM_STATE   0x003DA0B8u           /* 7 = idle (takes input) */
#define MENU_OBJ   0x003D9ED0u           /* the menu object (the Options setter's first argument) */
#define SND_MGR    0x003DD040u           /* the sound manager: +0x150 music volume, +0x14C effects volume (floats 0..1) */
#define FN_SND_VOLUME 0x001FEA60u        /* (mgr, category, kind, f12 volume): the game's Options page uses it */
#define CARD_WORKER 0x002F633Cu          /* the memory card worker: +4 = -1 when idle (pc_text.c) */
#define ENTRY_SIZE 32u

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, ps2_ram + a, 4); return v; }
static float rdf(uint32_t a) { float v; memcpy(&v, ps2_ram + a, 4); return v; }

static int pause_list_up(void) {
    if (!ps2_ram) return 0;
    int32_t depth = (int32_t)rd32(PM_DEPTH);
    return depth >= 1 && depth <= 3 && rd32(PM_ENTRIES) == PCMENU_LIST;
}

static int pause_idle(void) { return pause_list_up() && rd32(PM_STATE) == 7; }

/* ---- (pc-polish) one menu: the game's pause menu and its Options page as a PC release has them -------------------
 * Pause menu: Keep Playing / Save and Quit / [Exit to Episode Menu] / [Abandon Job] / Save Game... / Load Game... /
 * Options (ours) / Controls... / Job Help / Quit to Desktop (ours). The game's own Options entry (0x2C) is replaced by
 * its two save items (taken from its Options table) and our Options: its other items are PS2 hardware options (hidden)
 * or options our menu has (music / effects volume, speakers, vibration, the two inversions: two-way, below).
 * The game's Options page (the title screen's SELECT menu): its save items, then Options and Quit to Desktop. */
#define OPTIONS_TABLE 0x002BFEC8u        /* the game's static Options items (32 bytes each): [1] Load Game..., [2] Save Game... */
#define TXT_SAVE_GAME 0x3Eu
#define TXT_LOAD_GAME 0x3Du
#define TXT_OPTIONS 0x2Cu
#define TXT_SAVE_AND_QUIT 0x47u         /* "Save and Quit" (back to the title screen, after saving) */
#define FN_IN_TITLE 0x001D9880u          /* (0x3DA530): the Options builder shows Save Game... when it returns 0 */

static void put_text(ps2_ctx* ctx, uint32_t at, const char* text) {
    size_t n = strlen(text) + 1;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        for (size_t b = 0; b < 4 && i + b < n; b++) v |= (uint32_t)(uint8_t)text[i + b] << (8 * b);
        ps2_w32(ctx, at + (uint32_t)i, v);
    }
}

static void put_entry(ps2_ctx* ctx, uint32_t at, uint32_t text_id) {
    for (uint32_t w = 0; w < ENTRY_SIZE; w += 4) ps2_w32(ctx, at + w, 0);
    ps2_w32(ctx, at + 0x00, text_id);    /* text id; no callbacks (pcmenu_select acts on it) */
    ps2_w32(ctx, at + 0x1C, 2);          /* flags: stay open */
}

static void copy_entry(ps2_ctx* ctx, uint32_t dst, uint32_t src) {
    for (uint32_t w = 0; w < ENTRY_SIZE; w += 4) ps2_w32(ctx, dst + w, ps2_r32(ctx, src + w));
}

static void texts(ps2_ctx* ctx) {
    put_text(ctx, PCMENU_STRING, "Options");
    put_text(ctx, PCMENU_STRING_QUIT, "Quit to Desktop");
    put_text(ctx, PCMENU_STRING_SAVEQUIT, "Save and Quit to Title");   /* (the game's 0x47 "Save and Quit": next to Quit to Desktop) */
}

/* a call of a game function with the registers kept (aspect.c aspect_apply does the same) */
static ps2_ctx call_saved;
static void call_begin(ps2_ctx* ctx) {
    call_saved = *ctx;
    ctx->r[29].ud[0] = (uint64_t)(int64_t)(int32_t)((ctx->r[29].uw[0] - 0x400u) & ~15u);
    ctx->r[31].ud[0] = call_saved.r[31].ud[0];
}
static void call_end(ps2_ctx* ctx) { *ctx = call_saved; }

static int call_in_title(ps2_ctx* ctx) {
    call_begin(ctx);
    ctx->r[4].ud[0] = 0x003DA530u;
    ps2_call(ctx, FN_IN_TITLE);
    int r = ctx->r[2].uw[0] != 0;
    call_end(ctx);
    return r;
}

/* f_001B0290, before its jal f_001AF4D8 (push page 1): $a1 = count, $a2 = the list (0x2F65D0, full at 7 entries).
 * The list is copied to PCMENU_LIST (as above) and the call gets the copy: nothing else reads 0x2F65D0 (the page
 * record keeps the pointer). */
void pcmenu_pause_list(ps2_ctx* ctx) {
    uint32_t n = ctx->r[5].uw[0], src = ctx->r[6].uw[0];
    if (n == 0 || n > 12) return;
    uint32_t dst = PCMENU_LIST, k = 0, placed = 0;
    int save_ok = !call_in_title(ctx);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = src + i * ENTRY_SIZE;
        if (!placed && ps2_r32(ctx, e) == TXT_OPTIONS) {      /* the game's Options -> Save Game... / Load Game... / Options */
            if (save_ok && ps2_r32(ctx, OPTIONS_TABLE + 2 * ENTRY_SIZE) == TXT_SAVE_GAME)
                copy_entry(ctx, dst + k++ * ENTRY_SIZE, OPTIONS_TABLE + 2 * ENTRY_SIZE);
            if (ps2_r32(ctx, OPTIONS_TABLE + 1 * ENTRY_SIZE) == TXT_LOAD_GAME)
                copy_entry(ctx, dst + k++ * ENTRY_SIZE, OPTIONS_TABLE + 1 * ENTRY_SIZE);
            put_entry(ctx, dst + k++ * ENTRY_SIZE, PCMENU_TEXT_ID);
            placed = 1;
            continue;
        }
        copy_entry(ctx, dst + k++ * ENTRY_SIZE, e);
    }
    if (!placed) put_entry(ctx, dst + k++ * ENTRY_SIZE, PCMENU_TEXT_ID);
    put_entry(ctx, dst + k++ * ENTRY_SIZE, PCMENU_TEXT_QUIT);
    texts(ctx);
    ctx->r[5].ud[0] = k;
    ctx->r[6].ud[0] = dst;
}

/* the Options page's items our menu has (f_001B04E0's filter at 0x1B058C, beside pc_options_hidden): Speakers 0x4C,
 * Music / Effects Volume 0x67 / 0x68, Invert Binocucom 0x51, Invert Turret 0x53, Vibrations 0x48 */
int pcmenu_option_hidden(uint32_t kind) {
    return kind == 0x4Cu || kind == 0x67u || kind == 0x68u || kind == 0x51u || kind == 0x53u || kind == 0x48u;
}

/* f_001B04E0 before 0x1B05E4 (the items are copied: $s1 = the end of the list at 0x2F6470, 11 entries' room): our
 * Options and Quit to Desktop after the game's save items */
void pcmenu_options_list(ps2_ctx* ctx) {
    uint32_t end = ctx->r[17].uw[0];
    if (end < 0x002F6470u || end > 0x002F6470u + 9 * ENTRY_SIZE) return;
    put_entry(ctx, end, PCMENU_TEXT_ID);
    put_entry(ctx, end + ENTRY_SIZE, PCMENU_TEXT_QUIT);
    texts(ctx);
    ctx->r[17].ud[0] = end + 2 * ENTRY_SIZE;
}

/* f_0013E368 (text id -> string, a leaf): our ids */
int pcmenu_text(ps2_ctx* ctx) {
    uint32_t id = ctx->r[5].uw[0];
    if (id == PCMENU_TEXT_ID) ctx->r[2].ud[0] = PCMENU_STRING;
    else if (id == PCMENU_TEXT_QUIT) ctx->r[2].ud[0] = PCMENU_STRING_QUIT;
    else if (id == TXT_SAVE_AND_QUIT && pc_text_enabled() && ps2_r32(ctx, PCMENU_STRING_SAVEQUIT)) ctx->r[2].ud[0] = PCMENU_STRING_SAVEQUIT;
    else return 0;
    return 1;
}

/* f_001AEEE0, Cross on an entry ($s0) of an idle page */
void pcmenu_select(ps2_ctx* ctx) {
    uint32_t id = ps2_r32(ctx, ctx->r[16].uw[0]);
    if (id == PCMENU_TEXT_ID) menu_open();
    else if (id == PCMENU_TEXT_QUIT) pcmenu_quit_request();
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
static volatile ULONGLONG open_deadline, pause_deadline;

/* Esc / PS button / "menu" token: the game's pause menu first (the game is paused while the menu is open), then the
 * menu; where the game cannot pause (title, movies, loading) the menu opens over it after 0.5 s */
static void request_open(void) {
    if (menu_is_open()) return;
    if (pause_list_up()) { menu_open(); return; }
    open_deadline = GetTickCount64() + 500;
    InterlockedExchange(&pcmenu_open_pending, 1);
    pcmenu_pause_req = 1;
}

/* (pc-polish) Quit to desktop? - the game pauses behind the question where it can (as Esc does); the question shows at once */
void pcmenu_quit_request(void) {
    if (!menu_is_open() && !pause_list_up() && ps2_ram) {
        pause_deadline = GetTickCount64() + 500;
        pcmenu_pause_req = 1;
    }
    menu_quit_request();
}

/* both the window thread (menu_tick) and the EE (main loop) */
static void tick(void) {
    if (pause_deadline && GetTickCount64() > pause_deadline) {   /* (a quit question where the game cannot pause) */
        pause_deadline = 0;
        if (!pcmenu_open_pending) pcmenu_pause_req = 0;
    }
    if (!pcmenu_open_pending) return;
    if (pause_idle() || GetTickCount64() > open_deadline) {
        if (!InterlockedExchange(&pcmenu_open_pending, 0)) return;
        pcmenu_pause_req = 0;
        menu_open();
    }
}

static void closed(void) { InterlockedExchange(&pcmenu_open_pending, 0); }

/* ---- Quit to desktop (pc-polish) -------------------------------------------------------------------------------
 * After the question: a save the game is writing finishes first (its card worker idle, then mc_quiesce in launch_exit),
 * the motors stop and the light bar goes dark (input_shutdown), the window's state is saved (window.c close_window; every
 * setting is in sly2.ini already, written when it changed), and the process ends with exit code 0. On its own thread: the
 * window thread and the EE keep running meanwhile (the card worker is the game's). */
static DWORD WINAPI quit_thread(LPVOID arg) {
    (void)arg;
    fprintf(stderr, "[menu] quit to desktop\n");
    ULONGLONG t0 = GetTickCount64();
    while (ps2_ram && rd32(CARD_WORKER + 4) != 0xFFFFFFFFu && GetTickCount64() - t0 < 5000) Sleep(10);
    if (ps2_ram && rd32(CARD_WORKER + 4) != 0xFFFFFFFFu) fprintf(stderr, "[menu] the save in progress did not finish in 5 s\n");
    else if (GetTickCount64() - t0 >= 20) fprintf(stderr, "[menu] waited %llu ms for the save in progress\n", GetTickCount64() - t0);
    if (window_request_quit()) return 0;                     /* (the window thread closes it: close_window) */
    input_shutdown();
    if (window_exit_hook) window_exit_hook(0);
    ExitProcess(0);
}

void menu_game_quit(void) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    HANDLE t = CreateThread(NULL, 0, quit_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else quit_thread(NULL);
}

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
 * volumes it was saved with): ours again. (pc-polish: the page no longer has its volume items, so this is the
 * loaded-save case.) */
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
    call_begin(ctx);
    ctx->r[4].ud[0] = SND_MGR;
    ctx->r[5].ud[0] = (uint64_t)category;
    ctx->r[6].ud[0] = 1;
    ctx->f[12].f = v;
    ps2_call(ctx, FN_SND_VOLUME);
    call_end(ctx);
}

/* ---- the game's own options (pc-polish) -------------------------------------------------------------------------
 * Its Options page's items, now in our menu: read with its getter f_001AF9C8(item) (writes the value's text id into
 * item word 0) and set with its setter f_001AFB88(menu object, item with the new value's text id), which store them where
 * the game keeps them (a loaded save brings its own: ours are put back, as for the volumes). The values' text ids:
 * Vibrations On 0x48 / Off 0x49, Speakers Mono 0x4C / Stereo 0x4D / Surround 0x4E, Invert Binocucom Off 0x51 / On 0x52,
 * Invert Turret Off 0x53 / On 0x54. The getters read the game's variables through the pointers at 0x3E0FC0..0x3E0FE0
 * (zero before the game has set them up: nothing is done then). */
#define FN_OPT_GET 0x001AF9C8u
#define FN_OPT_SET 0x001AFB88u
static volatile int game_opts_now;       /* a setting changed: compare at the next frame */

static int game_vars_ready(void) {
    static const uint32_t ptrs[] = { 0x003E0FC0u, 0x003E0FD8u, 0x003E0FDCu, 0x003E0FE0u };
    for (size_t k = 0; k < sizeof ptrs / sizeof ptrs[0]; k++) {
        uint32_t p = rd32(ptrs[k]);
        if (p < 0x00100000u || p >= 0x02000000u) return 0;
    }
    return 1;
}

static uint32_t opt_get(ps2_ctx* ctx, uint32_t kind) {
    for (uint32_t w = 0; w < ENTRY_SIZE; w += 4) ps2_w32(ctx, PCMENU_ITEM + w, 0);
    ps2_w32(ctx, PCMENU_ITEM, kind);
    call_begin(ctx);
    ctx->r[4].ud[0] = PCMENU_ITEM;
    ps2_call(ctx, FN_OPT_GET);
    call_end(ctx);
    return ps2_r32(ctx, PCMENU_ITEM);
}

static void opt_set(ps2_ctx* ctx, uint32_t value_id) {
    for (uint32_t w = 0; w < ENTRY_SIZE; w += 4) ps2_w32(ctx, PCMENU_ITEM + w, 0);
    ps2_w32(ctx, PCMENU_ITEM, value_id);
    call_begin(ctx);
    ctx->r[4].ud[0] = MENU_OBJ;
    ctx->r[5].ud[0] = PCMENU_ITEM;
    ps2_call(ctx, FN_OPT_SET);
    call_end(ctx);
}

static void sync_game_options(ps2_ctx* ctx) {
    if (!game_vars_ready()) return;
    input_config c;
    input_config_get(&c);
    static const uint32_t speakers[] = { 0x4Du, 0x4Cu, 0x4Eu };   /* SETTINGS_SPEAKERS_*: stereo, mono, surround */
    int sp = ps2_settings.game_speakers >= 0 && ps2_settings.game_speakers < 3 ? ps2_settings.game_speakers : 0;
    const struct { uint32_t kind, want; const char* name; } o[] = {
        { 0x48u, c.vibration ? 0x48u : 0x49u, "vibrations" },
        { 0x51u, ps2_settings.game_invert_binocucom ? 0x52u : 0x51u, "invert binocucom" },
        { 0x53u, ps2_settings.game_invert_turret ? 0x54u : 0x53u, "invert turret" },
        { 0x4Cu, speakers[sp], "speakers" },
    };
    for (size_t k = 0; k < sizeof o / sizeof o[0]; k++) {
        uint32_t now = opt_get(ctx, o[k].kind);
        if (now == o[k].want) continue;
        opt_set(ctx, o[k].want);
        fprintf(stderr, "[menu] game option %s: text %#x -> %#x%s\n", o[k].name, (unsigned)now, (unsigned)o[k].want,
                opt_get(ctx, o[k].kind) == o[k].want ? "" : " (did not take)");
    }
}

/* main loop (EE): the volumes into the game's sound manager - as its Options page sets them: music = category 1,
 * effects = categories 0 and 3; the game's own options (twice a second, and at once after a change) */
void pcmenu_ee_frame(ps2_ctx* ctx) {
    tick();
    menu_keep_tick();                    /* (the Keep / Revert countdown also without a window thread: headless runs) */
    static unsigned frames;
    if (ee_applied && (game_opts_now || ++frames % 30 == 0)) {
        game_opts_now = 0;
        sync_game_options(ctx);
    }
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

static void game_applier(int option) { (void)option; game_opts_now = 1; }

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
static void h_set_mode(int m) { settings_change_key("display", "mode", m, 1); }

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
    h.display_modes = headless ? NULL : window_display_modes;
    h.in_game = h_in_game;
    h.request_open = request_open;
    h.closed = closed;
    h.tick = tick;
    h.quit = menu_game_quit;
    menu_set_host(&h);
    settings_on_apply(SET_APPLY_AUDIO, audio_applier);
    settings_on_apply(SET_APPLY_RENDERER, renderer_applier);
    settings_on_apply(SET_APPLY_PACING, pacing_applier);
    settings_on_apply(SET_APPLY_GAME, game_applier);
    if (headless) settings_on_apply(SET_APPLY_DISPLAY, display_applier);
    update_gain();
    pcmenu_ee_pending = 1;               /* the [audio] volumes into the game once its main loop runs */
}

/* PS2_INPUT tokens (window.c): menu mup mdown mleft mright mok mback mtabn mtabp menushot, (pc-polish) mquit = the window's
 * close button */
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
    } else if (k == 10) {
        if (menu_quit_showing()) menu_game_quit();          /* (a second press: at once, as the close button) */
        else pcmenu_quit_request();
    }
}
