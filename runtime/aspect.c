/* Widescreen support for the game code: the aspect factors the instruction patches use, and the
 * emission of GS side-band hints into the game's DMA chain. See aspect.h, gs_hint.h and
 * notes/widescreen.md. */
#include "aspect.h"
#include "gs_hint.h"
#include "ps2_runtime.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>

ps2_aspect_t ps2_aspect = { 0, 4.0f / 3.0f, 1.0f, 1.0f };

void gs_write_reg(uint32_t r, uint64_t v);      /* hw.h */

/* A hint at the EE's present moment in the GS stream, for code that draws outside the frame's DMA
 * chain (the movie player sends its pictures itself). */
void aspect_hint_now(uint32_t kind, uint32_t arg) {
    if (!ps2_aspect.wide) return;
    uint32_t value = 0;
    if (kind == GS_HINT_HUD_BEGIN) memcpy(&value, &ps2_aspect.hud_x, 4);
    gs_write_reg(GS_HINT_REG, gs_hint_make(kind, arg, value));
}

void aspect_init(void) {
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
 *            2D content laid out for a 4:3 screen: HUD_BEGIN(centre). */
#define RENDER_LIST 0x003B3980u
#define RENDER_LIST_MAX 0x2000u
enum { UI_DRAW_HUD = 0, UI_DRAW_AS_IS = 1 };
static uint8_t ui_entry_kind[RENDER_LIST_MAX];
static int ui_hud_on;                    /* a HUD_BEGIN has been emitted into this frame's chain */

void aspect_render_entry_alloc(struct ps2_ctx* ctx) {
    uint32_t i = (ctx->r[2].uw[0] - RENDER_LIST) / 16u;
    if (i < RENDER_LIST_MAX) ui_entry_kind[i] = ctx->r[31].uw[0] == 0x00213278u ? UI_DRAW_AS_IS : UI_DRAW_HUD;
}

void aspect_render_entry(struct ps2_ctx* ctx, uint32_t dmas, uint32_t entry, uint32_t rp) {
    if (!ps2_aspect.wide) return;
    uint32_t i = (entry - RENDER_LIST) / 16u;
    int hud = rp == 21 && i < RENDER_LIST_MAX && ui_entry_kind[i] == UI_DRAW_HUD;
    if (hud && !ui_hud_on) aspect_hint(ctx, dmas, GS_HINT_HUD_BEGIN, GS_HINT_ANCHOR_C);
    else if (!hud && ui_hud_on) aspect_hint(ctx, dmas, GS_HINT_HUD_END, 0);
    ui_hud_on = hud;
}

void aspect_render_done(struct ps2_ctx* ctx, uint32_t dmas) {
    if (ui_hud_on) aspect_hint(ctx, dmas, GS_HINT_HUD_END, 0);
    ui_hud_on = 0;
}
