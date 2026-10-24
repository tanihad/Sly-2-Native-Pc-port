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
 *   SIDE_FILL   value = colour 0x00BBGGRR. Fill the parts of the FRAME width outside the centre 4:3
 *               area - W * (1 - s) / 2 pixels at each side, s = the scale of the last HUD_BEGIN -
 *               over the scissor's height, opaque, untextured, Z neither tested nor written, now (it
 *               is drawn between the primitives around it). Used after a full-screen 4:3 overlay
 *               (the Binocucom frame) so it covers the wide picture like a pillarbox in its colour.
 *
 *   MOVIE_BEGIN / MOVIE_END
 *               (pc-small) the movie player runs: the GS thread copies the movie sprite's source picture
 *               for the presenters (movie_layer.h); drawing is unchanged. Sent at every aspect
 *               (PS2_MOVIE_LAYER=0: never).
 *
 * See notes/widescreen.md, notes/movies.md; the GPU renderer's contract is notes/d3d11-renderer.md section 8.3. */
#pragma once
#include <stdint.h>

#define GS_HINT_REG 0x7Fu

enum {
    GS_HINT_HUD_BEGIN = 1,
    GS_HINT_HUD_END = 2,
    GS_HINT_ASPECT_4_3_BEGIN = 3,
    GS_HINT_ASPECT_4_3_END = 4,
    GS_HINT_PHASE = 5,
    GS_HINT_SIDE_FILL = 6,
    GS_HINT_MOVIE_BEGIN = 7,
    GS_HINT_MOVIE_END = 8,
};

enum { GS_HINT_ANCHOR_L = 0, GS_HINT_ANCHOR_C = 1, GS_HINT_ANCHOR_R = 2 };

static inline uint64_t gs_hint_make(uint32_t kind, uint32_t arg, uint32_t value) {
    return (uint64_t)(kind & 0xFF) | ((uint64_t)(arg & 0xFF) << 8) | ((uint64_t)value << 32);
}
static inline uint32_t gs_hint_kind(uint64_t v) { return (uint32_t)(v & 0xFF); }
static inline uint32_t gs_hint_arg(uint64_t v) { return (uint32_t)((v >> 8) & 0xFF); }
static inline uint32_t gs_hint_value(uint64_t v) { return (uint32_t)(v >> 32); }
