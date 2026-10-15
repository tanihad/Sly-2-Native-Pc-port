/* Widescreen support for the game code: the aspect factors the instruction patches use, and the
 * emission of GS side-band hints into the game's DMA chain. See aspect.h, gs_hint.h and
 * notes/widescreen.md. */
#include "aspect.h"
#include "gs_hint.h"
#include "ps2_runtime.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ps2_aspect_t ps2_aspect = { 0, 4.0f / 3.0f, 1.0f, 1.0f, 1 };

void gs_write_reg(uint32_t r, uint64_t v);      /* hw.h */
uint32_t ps2_vblank_count(void);                /* ps2_runtime.c */

/* A hint at the EE's present moment in the GS stream, for code that draws outside the frame's DMA
 * chain (the movie player sends its pictures itself). */
void aspect_hint_now(uint32_t kind, uint32_t arg) {
    if (!ps2_aspect.wide) return;
    uint32_t value = 0;
    if (kind == GS_HINT_HUD_BEGIN) memcpy(&value, &ps2_aspect.hud_x, 4);
    gs_write_reg(GS_HINT_REG, gs_hint_make(kind, arg, value));
}

void aspect_init(void) {
    ps2_aspect.hud_edges = !ps2_settings.hud_centered;
    int w = ps2_settings.aspect_w, h = ps2_settings.aspect_h;
    if (w <= 0 || h <= 0) { w = 4; h = 3; }
    ps2_aspect.wide = w * 3 != h * 4;
    if (!ps2_aspect.wide) {                      /* exactly the game's own numbers */
        ps2_aspect.ratio = 4.0f / 3.0f;
        ps2_aspect.proj_x = ps2_aspect.hud_x = 1.0f;
        return;
    }
    ps2_aspect.ratio = (float)w / (float)h;
    ps2_aspect.proj_x = (float)(3 * w) / (float)(4 * h);
    ps2_aspect.hud_x = (float)(4 * h) / (float)(3 * w);
}

/* ---- run-time switch -----------------------------------------------------------------------------
 * window.c (F7 / F8, sly2.ini edited while the game runs, PS2_INPUT "aspect"/"hud" tokens) asks for
 * a new aspect or HUD mode on its own thread; the game takes it at the top of its next frame
 * (aspect_apply, instruction patch in the main loop right after the camera's per-frame update), so a
 * frame is built with one aspect throughout. The camera's X range, projection and frustum planes
 * depend on the aspect (ws_projection), so they are recomputed there with the game's own 0x14DB70.
 * Pictures already built are shown with the new display aspect (at most a frame or two). */
#define CM_CAMERA 0x002DDF40u                    /* the camera (CM) */
#define FN_CM_RECALC 0x0014DB70u                 /* recompute its frustum from FOV / aspect */
volatile int ps2_aspect_pending;
static volatile int req_w, req_h, req_hud;

void aspect_request(int w, int h, int hud_centered) {
    int pending = __atomic_load_n(&ps2_aspect_pending, __ATOMIC_ACQUIRE);   /* requests add up */
    int cw = pending ? req_w : ps2_settings.aspect_w, ch = pending ? req_h : ps2_settings.aspect_h;
    int chud = pending ? req_hud : ps2_settings.hud_centered;
    req_w = w > 0 && h > 0 ? w : cw;
    req_h = w > 0 && h > 0 ? h : ch;
    req_hud = hud_centered < 0 ? chud : hud_centered;
    __atomic_store_n(&ps2_aspect_pending, 1, __ATOMIC_RELEASE);
}

/* The aspect / HUD mode the game will have once pending requests are applied (for F7/F8). */
void aspect_target(int* w, int* h, int* hud_centered) {
    int pending = __atomic_load_n(&ps2_aspect_pending, __ATOMIC_ACQUIRE);
    *w = pending ? req_w : ps2_settings.aspect_w;
    *h = pending ? req_h : ps2_settings.aspect_h;
    *hud_centered = pending ? req_hud : ps2_settings.hud_centered;
}

void aspect_apply(struct ps2_ctx* ctx) {
    if (!__atomic_exchange_n(&ps2_aspect_pending, 0, __ATOMIC_ACQUIRE)) return;
    int w = req_w, h = req_h, hud = req_hud;
    int same_aspect = w * ps2_settings.aspect_h == h * ps2_settings.aspect_w;
    ps2_settings.aspect_w = w;
    ps2_settings.aspect_h = h;
    ps2_settings.hud_centered = hud;
    aspect_init();
    fprintf(stderr, "[aspect] v%u: now %d:%d, hud %s\n", ps2_vblank_count(), w, h, hud ? "centered" : "edges");
    if (same_aspect) return;
    static ps2_ctx saved;                        /* the game's registers stay as they were */
    saved = *ctx;
    ctx->r[29].ud[0] = (uint64_t)(int64_t)(int32_t)((ctx->r[29].uw[0] - 0x400u) & ~15u);
    ctx->r[4].ud[0] = CM_CAMERA;
    ctx->r[31].ud[0] = saved.r[31].ud[0];
    ps2_call(ctx, FN_CM_RECALC);
    *ctx = saved;
}

/* The game's frame builder (a "DMAS" object; Sly 1 names) as far as the hint needs it:
 *   +0x00 end of the buffer, +0x04 start, +0x08 the open DMA CNT tag (0: none), +0x0C write
 *   position, +0x10 non-zero while VIF codes are written word by word, +0x14 non-zero while a GIF
 *   primitive is open (closed by the object's own method before anything else may follow).
 * A hint is three quadwords: VIF NOP x3 + DIRECT 2, a PACKED GIFtag (NLOOP 1, EOP, one A+D
 * register) and the A+D quadword for register 0x7F; inside a CNT of its own when no CNT is open
 * (the CNT the game opens later computes its own QWC from the write position). */
void aspect_hint(struct ps2_ctx* ctx, uint32_t dmas, uint32_t kind, uint32_t arg) {
    uint32_t value = 0;
    if (kind == GS_HINT_HUD_BEGIN) memcpy(&value, &ps2_aspect.hud_x, 4);
    aspect_hint_value(ctx, dmas, kind, arg, value);
}

void aspect_hint_value(struct ps2_ctx* ctx, uint32_t dmas, uint32_t kind, uint32_t arg, uint32_t value) {
    if (!ps2_aspect.wide) return;
    uint32_t end = ps2_r32(ctx, dmas + 0x00), cnt = ps2_r32(ctx, dmas + 0x08), cur = ps2_r32(ctx, dmas + 0x0C);
    uint32_t vif_words = ps2_r32(ctx, dmas + 0x10), gif_open = ps2_r32(ctx, dmas + 0x14);
    static int logged;
    if (gif_open || ((cur & 15) && !vif_words)) {
        if (logged++ < 8)
            fprintf(stderr, "[aspect] hint %u not emitted: the frame builder at 0x%08X is inside a %s\n", kind,
                    dmas, gif_open ? "GIF primitive" : "quadword");
        return;
    }
    uint32_t pad = (16 - (cur & 15)) & 15;
    uint32_t need = pad + (cnt ? 3u : 4u) * 16u;
    if (cur + need > end) {
        if (logged++ < 8) fprintf(stderr, "[aspect] hint %u not emitted: frame builder 0x%08X is full\n", kind, dmas);
        return;
    }
    for (; cur & 15; cur += 4) ps2_w32(ctx, cur, 0);       /* VIF NOPs up to the quadword */
    if (!cnt) {                                            /* DMA CNT, QWC 3 (VIF NOP NOP) */
        ps2_w64(ctx, cur, 0x10000003ull);
        ps2_w64(ctx, cur + 8, 0);
        cur += 16;
    }
    ps2_w64(ctx, cur, 0);                                  /* VIF NOP, NOP, NOP, DIRECT 2 */
    ps2_w64(ctx, cur + 8, (uint64_t)0x50000002u << 32);
    ps2_w64(ctx, cur + 16, 1ull | (1ull << 15) | (1ull << 60));   /* NLOOP 1, EOP, PACKED, NREG 1 */
    ps2_w64(ctx, cur + 24, 0xE);                           /* A+D */
    ps2_w64(ctx, cur + 32, gs_hint_make(kind, arg, value));
    ps2_w64(ctx, cur + 40, GS_HINT_REG);
    ps2_w32(ctx, dmas + 0x0C, cur + 48);
}

/* ---- the 2D interface: render priority 21 -------------------------------------------------------
 * The frame's render list (0x3B3980, entries of 16 bytes {render priority, sort key, DMA CALL
 * address, second CALL}) is sorted and turned into DMA CALLs by f_00157FA0; everything the game
 * draws in 2D - HUD, menus, text, the vector-art screens - is in priority 21 (render class 5,
 * begin 0x211A80 / end 0x211C10, the 2D microprogram on VU1). Entries are allocated by 0x1CD998;
 * the allocation's caller tells what an entry is:
 *   0x2130C0 (return 0x213278): the set-up of a 2D scene - GS context and 16 sprites that clear the
 *            Z buffer over the whole screen (the 2D shapes are layered with ZTST GREATER): drawn as
 *            is, so it still covers the whole widened picture;
 *   every other creator (shapes 0x2174C8/0x2177E0, passes 0x217298, text 0x216750/0x2163B0):
 *            2D content laid out for a 4:3 screen: HUD_BEGIN(anchor), the anchor of the scene's
 *            widget the entry belongs to (below; C for everything outside scenes). */
#define RENDER_LIST 0x003B3980u
#define RENDER_LIST_MAX 0x2000u
enum { UI_DRAW_HUD = 0, UI_DRAW_AS_IS = 1 };
static uint8_t ui_entry_kind[RENDER_LIST_MAX];
static uint32_t ui_entry_scene[RENDER_LIST_MAX], ui_entry_ra[RENDER_LIST_MAX], ui_entry_root[RENDER_LIST_MAX];
static int ui_hud_on;                    /* a HUD_BEGIN has been emitted into this frame's chain */
static uint32_t ui_scene;                /* the 2D scene being drawn (0x213300), 0 outside */
static int ui_dl_depth;                  /* display-list nesting while the scene queues its items */
static uint32_t ui_root, ui_item_root;   /* root-level object being queued / of the item being drawn */
#define ITEM_MAP 2048u
static uint32_t ui_item_key[ITEM_MAP], ui_item_val[ITEM_MAP], ui_item_gen[ITEM_MAP], ui_gen;
static int hud_log = -1;                 /* PS2_HUD_LOG=1: diagnostics (PHASE hints, scene list) */
static uint32_t ui_label;

/* ---- HUD anchors (hud = edges) -------------------------------------------------------------------
 * Every 2D entry gets an anchor; with hud = edges the HUD_BEGIN hint carries it. A scene's policy
 * comes from its object id (+0x18; the HUD manager 0x1D9520 and the other owners find their scenes
 * by these ids with 0x15E938): a fixed anchor, or POLICY_BY_ROOT - each root-level object of the
 * scene's display list (one widget) is anchored by the third of the screen its origin is in. Scenes
 * not listed (menus, the pause screen, ThiefNet, briefings, the Binocucom, the ticker, level-specific
 * scenes) are centred layouts and stay centred. */
enum { POLICY_BY_ROOT = 3 };
#define BINOCUCOM_NAVY 0x00190000u               /* the Binocucom frame's colour (0, 0, 25) */
#define SIDE_FILL_ON 0x01000000u
#define SCENE_VT_JOB_COUNTER 0x002DAD90u        /* vtable of the job counter scene class */
static const struct { uint16_t oid; uint8_t policy; uint32_t side_fill; const char* what; } scene_policies[] = {
    { 0x36B, POLICY_BY_ROOT, 0, "player's health meter and gadget gauge (top left; HUD manager 0x3D9E80, 0x1D9968)" },
    { 0x36D, POLICY_BY_ROOT, 0, "coin counter (world +0x3BAC, shown by 0x161868 when the coin count changes; L3)" },
    { 0x36E, POLICY_BY_ROOT, 0, "clue bottle counter (world +0x3BB8, shown by 0x1618B0 from the pick-ups 0x1C7EE8, 0x1C8AF0; L3)" },
    { 0x36F, POLICY_BY_ROOT, 0, "timed counter (world +0x3BC4, shown by 0x1618F8 for a time, hidden by 0x161958)" },
    /* The Binocucom frame (R3 view, briefings), one per character (0x19D540): a full-screen 4:3
     * overlay whose navy border fills the 4:3 screen's corners. Centred, and the sides of the wide
     * picture are filled in its colour after it (SIDE_FILL), so it covers the display as on a TV and
     * the view shows the 4:3 area the game's photo / on-screen tests use (ws_screen_project). */
    { 0x35F, GS_HINT_ANCHOR_C, BINOCUCOM_NAVY | SIDE_FILL_ON, "Binocucom frame (player class 0x35, Sly)" },
    { 0x360, GS_HINT_ANCHOR_C, BINOCUCOM_NAVY | SIDE_FILL_ON, "Binocucom frame (player class 0x37)" },
    { 0x361, GS_HINT_ANCHOR_C, BINOCUCOM_NAVY | SIDE_FILL_ON, "Binocucom frame (player class 0x36)" },
};
static uint32_t ui_scene_fill;           /* SIDE_FILL_ON | colour: the scene being drawn wants a side fill */
static uint32_t ui_entry_fill[RENDER_LIST_MAX];
static uint32_t ui_fill_pending;         /* the 2D entries just emitted belong to a side-fill scene */
static uint8_t ui_entry_anchor[RENDER_LIST_MAX];
static int ui_scene_policy = GS_HINT_ANCHOR_C;
static float ui_scene_w = 640.0f;        /* the scene's width in its own units (+0x94) */
static uint8_t ui_item_anchor = GS_HINT_ANCHOR_C, ui_text_anchor = GS_HINT_ANCHOR_C;
static int ui_hud_anchor = GS_HINT_ANCHOR_C;

/* A HUD meter: a scene with a "meter fill" sprite instance, id 0x34F or 0x350 - what the game's
 * 0x212E40 (set a sprite's position in its timeline by a fraction) is called with by the health
 * meter (0x1D9968, 0x1D9DF8) and by every boss's health meter (0x180100, 0x1D4C88, script command
 * 0x10DD08). Sprite instances: scene +0x80 count (u16), +0x84 array of 0x30 bytes {u16 states,
 * +4 states (0x18 bytes, id at +0x14)}, as 0x212E40 walks them. Cached by (scene, object id). */
static int scene_is_meter(struct ps2_ctx* ctx, uint32_t scene, uint32_t oid) {
    static struct { uint32_t scene, oid; int meter; } cache[64];
    static unsigned next;
    for (int k = 0; k < 64; k++)
        if (cache[k].scene == scene && cache[k].oid == oid) return cache[k].meter;
    int meter = 0;
    uint32_t n = ps2_r32(ctx, scene + 0x80) & 0xFFFF, spr = ps2_r32(ctx, scene + 0x84);
    for (uint32_t i = 0; i < n && i < 1024 && !meter && spr; i++) {
        uint32_t s = spr + i * 0x30, cnt = ps2_r32(ctx, s) & 0xFFFF, arr = ps2_r32(ctx, s + 4);
        for (uint32_t j = 0; j < cnt && j < 1024 && arr; j++) {
            uint32_t id = ps2_r32(ctx, arr + j * 0x18 + 0x14);
            if (id == 0x34F || id == 0x350) { meter = 1; break; }
        }
    }
    cache[next].scene = scene; cache[next].oid = oid; cache[next].meter = meter;
    next = (next + 1) & 63;
    return meter;
}

static void hud_log_init(void) {
    if (hud_log < 0) { const char* e = getenv("PS2_HUD_LOG"); hud_log = e && *e && *e != '0'; }
}

void aspect_scene_begin(struct ps2_ctx* ctx) {
    ui_scene = ctx->r[4].uw[0];
    ui_dl_depth = 0;
    ui_root = ui_item_root = 0;
    ui_gen++;
    uint32_t oid = ps2_r32(ctx, ui_scene + 0x18);
    int w = (int)ps2_r32(ctx, ui_scene + 0x94);
    ui_scene_w = w > 0 && w < 4096 ? (float)w : 640.0f;
    ui_scene_policy = -1;
    ui_scene_fill = 0;
    for (size_t k = 0; k < sizeof scene_policies / sizeof scene_policies[0]; k++)
        if (scene_policies[k].oid == oid) { ui_scene_policy = scene_policies[k].policy; ui_scene_fill = scene_policies[k].side_fill; }
    /* the job counter class ("0/3": counts its tasks' states, 0x211F90; vtable 0x2DAD90, a scene
     * subclass): every job's counter, placed in a corner by its level */
    if (ui_scene_policy < 0 && ps2_r32(ctx, ui_scene + 0x0C) == SCENE_VT_JOB_COUNTER) ui_scene_policy = POLICY_BY_ROOT;
    if (ui_scene_policy < 0) ui_scene_policy = scene_is_meter(ctx, ui_scene, oid) ? POLICY_BY_ROOT : GS_HINT_ANCHOR_C;
    ui_item_anchor = ui_scene_policy == POLICY_BY_ROOT ? GS_HINT_ANCHOR_C : (uint8_t)ui_scene_policy;
    hud_log_init();
    if (hud_log) {
        static uint32_t seen[256];
        static int nseen;
        for (int k = 0; k < nseen; k++) if (seen[k] == ui_scene) return;
        if (nseen < 256) seen[nseen++] = ui_scene;
        fprintf(stderr, "[hud] scene %08X vt %08X", ui_scene, ps2_r32(ctx, ui_scene));
        for (uint32_t o = 0; o < 0x120; o += 4) {
            if (!(o & 0x1F)) fprintf(stderr, "\n[hud]   +%03X:", o);
            fprintf(stderr, " %08X", ps2_r32(ctx, ui_scene + o));
        }
        fprintf(stderr, "\n");
    }
}

void aspect_scene_end(struct ps2_ctx* ctx) {
    (void)ctx;
    ui_scene = 0;
    ui_dl_depth = 0;
    ui_item_root = 0;
    ui_item_anchor = GS_HINT_ANCHOR_C;
}

/* Items queued while the scene's root display list is walked: which root-level object they belong to. */
static uint32_t item_hash(uint32_t a) { return (a * 2654435761u) >> (32 - 11); }

void aspect_dl_begin(struct ps2_ctx* ctx) { (void)ctx; if (ui_scene) ui_dl_depth++; }
void aspect_dl_end(struct ps2_ctx* ctx) { (void)ctx; if (ui_dl_depth) ui_dl_depth--; }
void aspect_dl_object(struct ps2_ctx* ctx) {
    if (!ui_scene || ui_dl_depth != 1) return;
    ui_root = ctx->r[16].uw[0];
    if (hud_log > 0) {
        static uint32_t seen[1024];
        static int nseen;
        for (int k = 0; k < nseen; k++) if (seen[k] == ui_root) return;
        if (nseen < 1024) seen[nseen++] = ui_root;
        fprintf(stderr, "[hud] root %08X scene %08X oid %X:", ui_root, ui_scene, ps2_r32(ctx, ui_scene + 0x18));
        for (uint32_t o = 0; o < 0x70; o += 4) {
            uint32_t w = ps2_r32(ctx, ui_root + o);
            float f;
            memcpy(&f, &w, 4);
            if (o >= 0x10 && o < 0x30) fprintf(stderr, " %g", f); else fprintf(stderr, " %X", w);
        }
        fprintf(stderr, "\n");
    }
}

void aspect_item_queued(struct ps2_ctx* ctx) {
    if (!ui_scene) return;
    uint32_t item = ctx->r[5].uw[0];
    for (uint32_t h = item_hash(item), n = 0; n < ITEM_MAP; n++, h = (h + 1) & (ITEM_MAP - 1))
        if (ui_item_gen[h] != ui_gen || ui_item_key[h] == item) {
            ui_item_gen[h] = ui_gen; ui_item_key[h] = item; ui_item_val[h] = ui_root;
            return;
        }
}

/* The anchor of a root-level object of an edge-anchored scene: the third of the scene's width its
 * origin (the matrix translation, +0x20) is in. */
static uint8_t root_anchor(struct ps2_ctx* ctx, uint32_t root) {
    if (!root) return GS_HINT_ANCHOR_C;
    uint32_t w = ps2_r32(ctx, root + 0x20);
    float tx;
    memcpy(&tx, &w, 4);
    if (!(tx == tx)) return GS_HINT_ANCHOR_C;
    return tx < ui_scene_w / 3.0f ? GS_HINT_ANCHOR_L : tx > ui_scene_w * 2.0f / 3.0f ? GS_HINT_ANCHOR_R : GS_HINT_ANCHOR_C;
}

void aspect_item_draw(struct ps2_ctx* ctx) {
    uint32_t item = ctx->r[16].uw[0];
    ui_item_root = 0;
    for (uint32_t h = item_hash(item), n = 0; n < ITEM_MAP && ui_item_gen[h] == ui_gen; n++, h = (h + 1) & (ITEM_MAP - 1))
        if (ui_item_key[h] == item) { ui_item_root = ui_item_val[h]; break; }
    ui_item_anchor = ui_scene_policy == POLICY_BY_ROOT ? root_anchor(ctx, ui_item_root) : (uint8_t)ui_scene_policy;
}

void aspect_render_entry_alloc(struct ps2_ctx* ctx) {
    uint32_t i = (ctx->r[2].uw[0] - RENDER_LIST) / 16u;
    if (i >= RENDER_LIST_MAX) return;
    ui_entry_kind[i] = ctx->r[31].uw[0] == 0x00213278u ? UI_DRAW_AS_IS : UI_DRAW_HUD;
    ui_entry_scene[i] = ui_entry_kind[i] == UI_DRAW_HUD ? ui_scene : 0;
    ui_entry_root[i] = ui_entry_kind[i] == UI_DRAW_HUD && ui_scene ? ui_item_root : 0;
    ui_entry_ra[i] = ctx->r[31].uw[0];
    ui_entry_anchor[i] = ui_scene ? ui_item_anchor : ui_text_anchor;
    ui_entry_fill[i] = ui_scene && ui_entry_kind[i] == UI_DRAW_HUD ? ui_scene_fill : 0;
}

void aspect_render_entry(struct ps2_ctx* ctx, uint32_t dmas, uint32_t entry, uint32_t rp) {
    if (!ps2_aspect.wide) return;
    uint32_t i = (entry - RENDER_LIST) / 16u;
    int hud = rp == 21 && i < RENDER_LIST_MAX && ui_entry_kind[i] == UI_DRAW_HUD;
    if (hud_log > 0) {
        uint32_t label = rp != 21 || i >= RENDER_LIST_MAX ? 0 : ui_entry_root[i] ? ui_entry_root[i]
                       : ui_entry_scene[i] ? ui_entry_scene[i] | 0x40000000u : ui_entry_ra[i] | 0x80000000u;
        if (label != ui_label) { aspect_hint_value(ctx, dmas, GS_HINT_PHASE, 0, label); ui_label = label; }
    }
    uint32_t fill = hud ? ui_entry_fill[i] : 0;
    if (ui_fill_pending && fill != ui_fill_pending && ui_hud_on)      /* a side-fill scene's entries end */
        aspect_hint_value(ctx, dmas, GS_HINT_SIDE_FILL, 0, ui_fill_pending & 0xFFFFFFu);
    ui_fill_pending = fill;
    int anchor = hud && ps2_aspect.hud_edges ? ui_entry_anchor[i] : GS_HINT_ANCHOR_C;
    if (hud && (!ui_hud_on || anchor != ui_hud_anchor)) aspect_hint(ctx, dmas, GS_HINT_HUD_BEGIN, (uint32_t)anchor);
    else if (!hud && ui_hud_on) aspect_hint(ctx, dmas, GS_HINT_HUD_END, 0);
    ui_hud_on = hud;
    ui_hud_anchor = anchor;
}

void aspect_render_done(struct ps2_ctx* ctx, uint32_t dmas) {
    if (ui_fill_pending && ui_hud_on) aspect_hint_value(ctx, dmas, GS_HINT_SIDE_FILL, 0, ui_fill_pending & 0xFFFFFFu);
    ui_fill_pending = 0;
    if (ui_hud_on) aspect_hint(ctx, dmas, GS_HINT_HUD_END, 0);
    ui_hud_on = 0;
    if (hud_log > 0) { aspect_hint_value(ctx, dmas, GS_HINT_PHASE, 0, 0xFFFFFFFFu); ui_label = 0; }
}
