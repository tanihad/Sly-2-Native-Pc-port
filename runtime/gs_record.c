/* GS stream recorder, for debugging and for checking renderer changes against each other.
 *
 *   PS2_GS_RECORD=<file>        record what the EE sends to the GS ...
 *   PS2_GS_RECORD_FROM=<vblank> ... from this VBlank on (default 600) ...
 *   PS2_GS_RECORD_FRAMES=<n>    ... for n VBlanks (default 60).
 *
 * File: "GSREC001", then the GS state at the first VBlank (4 MB of VRAM, the 1 KB CLUT buffer, the
 * 0x64 general registers as last written), then records { u32 tag, u32 len, payload }:
 *   1 register write   len = register number, payload u64 value
 *   2 IMAGE data       len = bytes, payload the data (padded to 4 bytes)
 *   3 VBlank           len = VBlank number, payload GS_PRIV_SNAPSHOT u64 (display registers)
 * SIGNAL/FINISH/LABEL are not recorded (they only change CSR). scripts/gs_replay.c renders a recording
 * with any version of the GS code (scripts/gs_replay.ps1), so two renderers can be compared on
 * exactly the same input. Recording happens on the EE side of the GS queue (gs_thread.c). */
#include "gs_draw.h"

#include <stdlib.h>

const uint16_t* gs_clut_buffer(void);    /* gs_mem_tex.c */

static FILE* rec;
static int rec_init, rec_on;
static uint32_t rec_from = 600, rec_frames = 60, rec_seen;
static uint64_t mirror[0x64];             /* every general register as last written */

static void put(uint32_t tag, uint32_t len, const void* p, size_t n) {
    uint32_t h[2] = { tag, len };
    fwrite(h, sizeof h, 1, rec);
    if (n) fwrite(p, 1, n, rec);
}

void gs_record_reg(uint32_t r, uint64_t v) {
    if (r < 0x64) mirror[r] = v;
    if (rec_on && (r < 0x60 || r > 0x62)) put(1, r, &v, 8);
}

void gs_record_image(const void* data, uint32_t bytes) {
    if (!rec_on) return;
    static const uint8_t pad[4];
    put(2, bytes, data, bytes);
    if (bytes & 3) fwrite(pad, 1, 4 - (bytes & 3), rec);
}

void gs_record_vsync(uint32_t vblank) {
    if (!rec_init) {
        rec_init = 1;
        const char* f = getenv("PS2_GS_RECORD");
        if (!f) return;
        if (getenv("PS2_GS_RECORD_FROM")) rec_from = (uint32_t)strtoul(getenv("PS2_GS_RECORD_FROM"), NULL, 0);
        if (getenv("PS2_GS_RECORD_FRAMES")) rec_frames = (uint32_t)strtoul(getenv("PS2_GS_RECORD_FRAMES"), NULL, 0);
        rec = fopen(f, "wb");
        if (!rec) { fprintf(stderr, "[gs] cannot write the GS recording %s\n", f); return; }
        setvbuf(rec, NULL, _IOFBF, 1 << 22);
    }
    if (!rec) return;
    if (!rec_on) {
        if (vblank < rec_from) return;
        gs_sync();                               /* the state below is the GS's at this point */
        fwrite("GSREC001", 8, 1, rec);
        fwrite(gs_vram, GS_VRAM_SIZE, 1, rec);
        static uint16_t clut[512];
        const uint16_t* c = gs_clut_buffer();
        if (c) memcpy(clut, c, sizeof clut);
        fwrite(clut, sizeof clut, 1, rec);
        fwrite(mirror, sizeof mirror, 1, rec);
        rec_on = 1;
        fprintf(stderr, "[gs] recording the GS stream from VBlank %u for %u VBlanks\n", vblank, rec_frames);
    }
    uint64_t snap[GS_PRIV_SNAPSHOT];
    gs_priv_snapshot(snap);
    put(3, vblank, snap, sizeof snap);
    if (++rec_seen >= rec_frames) {
        fclose(rec);
        rec = NULL;
        rec_on = 0;
        fprintf(stderr, "[gs] GS recording complete\n");
    }
}
