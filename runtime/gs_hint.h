/* Side-band hints in the GS stream: GS general register 0x7F.
 *
 * PS2 software never writes register 0x7F (the general registers end at 0x63, and gs_record.c's
 * register mirror stops there), so the PC port uses it to tell the renderers about the content of
 * what follows. The EE side emits the hints (runtime/aspect.c, from instruction patches in the
 * game's frame builder: recomp/patches.py); they travel inside the game's own DMA chain, so they
 * arrive in exactly the right place of the GS stream, are recorded by gs_record.c as ordinary
 * register writes and replay with scripts/gs_replay. Hints are only emitted while the display
 * aspect is not 4:3: at 4:3 the stream is the game's own, bit for bit.
 *
 * Payload (the 64-bit register value): bits 0-7 kind, 8-15 arg, 16-31 reserved (0), 32-63 value.
 *
 *   HUD_BEGIN   arg = anchor (L/C/R), value = float bits of the X scale (4/3)/A (0: the renderer's
 *               own aspect). Until HUD_END, every vertex's X becomes c + (x - c) * scale, where c is
 *               the anchor's X on the drawing buffer (left edge, centre, right edge of the FRAME
 *               width): 2D content drawn for a 4:3 screen keeps its proportions when the 512-wide
 *               picture is shown stretched to A.
 *   HUD_END     back to untransformed drawing.
 *   ASPECT_4_3_BEGIN / ASPECT_4_3_END
 *               everything shown between them (movies, full-screen menus, loading and legal
 *               screens) is 4:3 content: the presenter pillarboxes those pictures to 4:3 instead of
 *               stretching them. The state at a VBlank decides how that VBlank's picture is shown.
 *   PHASE       value = label (diagnostics only: renderers ignore it).
 *
 * See notes/widescreen.md; the GPU renderer's contract is notes/d3d11-renderer.md section 8.3. */
#pragma once
#include <stdint.h>

#define GS_HINT_REG 0x7Fu

enum {
    GS_HINT_HUD_BEGIN = 1,
    GS_HINT_HUD_END = 2,
    GS_HINT_ASPECT_4_3_BEGIN = 3,
    GS_HINT_ASPECT_4_3_END = 4,
    GS_HINT_PHASE = 5,
};

enum { GS_HINT_ANCHOR_L = 0, GS_HINT_ANCHOR_C = 1, GS_HINT_ANCHOR_R = 2 };

static inline uint64_t gs_hint_make(uint32_t kind, uint32_t arg, uint32_t value) {
    return (uint64_t)(kind & 0xFF) | ((uint64_t)(arg & 0xFF) << 8) | ((uint64_t)value << 32);
}
static inline uint32_t gs_hint_kind(uint64_t v) { return (uint32_t)(v & 0xFF); }
static inline uint32_t gs_hint_arg(uint64_t v) { return (uint32_t)((v >> 8) & 0xFF); }
static inline uint32_t gs_hint_value(uint64_t v) { return (uint32_t)(v >> 32); }
