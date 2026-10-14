/* Widescreen: the display aspect the game is played at, for the instruction patches in the game's
 * code (recomp/patches.py) and the presenter. notes/widescreen.md explains every patch.
 *
 * The game renders a 512x448 picture meant for a 4:3 screen. At aspect A (16:9 by default,
 * settings.h) the picture is shown stretched to A, and the patches make the game draw for that:
 * the 3D projection covers A/(4/3) more horizontally (the vertical field of view is unchanged),
 * and 2D content (HUD, menus) is drawn at 4:3 proportions, anchored to the left, centre or right
 * of the screen, through side-band hints to the renderer (gs_hint.h). At 4:3 nothing changes:
 * every patch tests ps2_aspect.wide first. */
#pragma once
#include <stdint.h>

typedef struct {
    int wide;            /* the display aspect is not 4:3 */
    float ratio;         /* A = width / height of the displayed picture */
    float proj_x;        /* A / (4/3): how much wider the 3D view is */
    float hud_x;         /* (4/3) / A: X scale that keeps 4:3 content's proportions */
} ps2_aspect_t;

extern ps2_aspect_t ps2_aspect;

struct ps2_ctx;
void aspect_init(void);                      /* from ps2_settings (settings_load first) */
/* Append a GS side-band hint (gs_hint.h) to the DMA chain the game's frame builder (DMAS object at
 * guest address `dmas`) is writing, so it reaches the GS between the draws around it. */
void aspect_hint(struct ps2_ctx* ctx, uint32_t dmas, uint32_t kind, uint32_t arg);
void aspect_hint_value(struct ps2_ctx* ctx, uint32_t dmas, uint32_t kind, uint32_t arg, uint32_t value);
void aspect_debug_trace(struct ps2_ctx* ctx, uint32_t tag);   /* TEMP */
/* The frame's render list (aspect.c): an entry was allocated (at the return of 0x1CD998), an entry
 * is about to be turned into DMA CALLs (in 0x157FA0's loop), the list is done. */
void aspect_render_entry_alloc(struct ps2_ctx* ctx);
void aspect_render_entry(struct ps2_ctx* ctx, uint32_t dmas, uint32_t entry, uint32_t rp);
void aspect_render_done(struct ps2_ctx* ctx, uint32_t dmas);
