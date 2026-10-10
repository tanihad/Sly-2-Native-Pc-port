/* Render a GS recording (runtime/gs_record.c, PS2_GS_RECORD) with the GS code it is linked with,
 * writing every VBlank's picture as <outdir>/frame_<vblank>.png. Built and run by
 * scripts/gs_replay.ps1 against any checkout's runtime/, so two versions of the renderer can be
 * compared on exactly the same input (scripts/compare_frames.py).
 *
 *   gs_replay <recording> <outdir> [workers]
 *
 * workers > 0: through the GS thread with that many pixel workers (deferred drawing), else
 * synchronously. Uses only the public GS entry points, so it builds with older GS code too. */
#include "gs_draw.h"

#include <stdlib.h>
#include <windows.h>

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);   /* debug.c */

/* neighbours of the GS code in the game */
void intc_raise(int cause) { (void)cause; }
void dmac_raise(int ch) { (void)ch; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}

static const char* outdir;

static void save(const uint32_t* frame, int ok, int w, int h, uint32_t vblank) {
    if (!ok || !strcmp(outdir, "-")) return;     /* "-": no pictures (benchmark) */
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06u.png", outdir, vblank);
    debug_write_png(path, frame, w, h, 1024);
}

static int rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: gs_replay <recording> <outdir> [workers]\n"); return 2; }
    outdir = argv[2];
    int workers = argc > 3 ? atoi(argv[3]) : 0;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    setvbuf(f, NULL, _IOFBF, 1 << 22);
    char magic[8];
    static uint8_t vram[GS_VRAM_SIZE];
    static uint16_t clut[512];
    static uint64_t regs[0x64];
    if (!rd(f, magic, 8) || memcmp(magic, "GSREC001", 8) || !rd(f, vram, sizeof vram) || !rd(f, clut, sizeof clut) ||
        !rd(f, regs, sizeof regs)) {
        fprintf(stderr, "%s: not a GS recording\n", argv[1]);
        return 1;
    }
    GS_INIT();

    /* CLUT buffer: load it from a 16x16 PSMCT32 image (CSM1 order: entries 8-15 and 16-23 of every
     * 32 swapped), which fills all 512 halves (entry i: low half i, high half i + 256) */
    {
        uint32_t img[256];
        for (int i = 0; i < 256; i++)
            img[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)] = clut[i] | (uint32_t)clut[i + 256] << 16;
        const uint32_t cbp = 0;
        gs_exec_write_reg(0x50, (uint64_t)cbp << 32 | 1ull << 48 | 0ull << 56);   /* BITBLTBUF: CT32, width 64 */
        gs_exec_write_reg(0x51, 0);                                               /* TRXPOS */
        gs_exec_write_reg(0x52, 16 | 16ull << 32);                                /* TRXREG 16x16 */
        gs_exec_write_reg(0x53, 0);                                               /* TRXDIR host->local */
        gs_exec_image_write(img, sizeof img);
        /* TEX0_1: PSMT8, CPSM CT32, CSM1, CSA 0, CLD 1 */
        gs_exec_write_reg(0x06, 0x13ull << 20 | (uint64_t)cbp << 37 | 1ull << 61);
    }
    /* general registers as last written, without their side effects (drawing, transfers, CLUT
     * loads); TEX2 is already folded into TEX0 */
    for (uint32_t r = 0; r < 0x64; r++) {
        if (r == 0x04 || r == 0x05 || r == 0x0C || r == 0x0D || r == 0x16 || r == 0x17 ||
            r == 0x53 || r == 0x54 || (r >= 0x60 && r <= 0x62))
            continue;
        uint64_t v = regs[r];
        if (r == 0x06 || r == 0x07) v &= ~(7ull << 61);                          /* CLD 0 */
        gs_exec_write_reg(r, v);
    }
    memcpy(gs_vram, vram, sizeof vram);
    gs_tex_flush_all();                          /* nothing decoded from the CLUT upload area stays */

    if (workers > 0) gs_thread_start(workers, save);
    /* the recording is read into memory first, so the timing below is the GS's alone */
    long start = ftell(f);
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)(ftell(f) - start);
    fseek(f, start, SEEK_SET);
    uint8_t* all = malloc(size + 16);
    if (!all || !rd(f, all, size)) { fprintf(stderr, "cannot read the recording\n"); return 1; }
    fclose(f);
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    uint32_t frames = 0;
    for (size_t pos = 0; pos + 8 <= size;) {
        uint32_t h[2];
        memcpy(h, all + pos, 8);
        pos += 8;
        if (h[0] == 1) {
            uint64_t v;
            memcpy(&v, all + pos, 8);
            pos += 8;
            gs_write_reg(h[1], v);
        } else if (h[0] == 2) {
            if (pos + h[1] > size) break;
            gs_image_write(all + pos, h[1]);
            pos += (h[1] + 3) & ~3u;
        } else if (h[0] == 3) {
            uint64_t snap[GS_PRIV_SNAPSHOT];
            if (pos + sizeof snap > size) break;
            memcpy(snap, all + pos, sizeof snap);
            pos += sizeof snap;
            for (int i = 0; i < 15; i++) gs_priv_access(0x12000000u + 16u * (uint32_t)i, &snap[i], 8, 1);
            gs_vsync();
            if (workers > 0) gs_queue_vsync(h[1]);
            else {
                static uint32_t frame[1024 * 1024];
                int w = 0, hh = 0;
                int ok = gs_display(frame, 1024, &w, &hh);
                save(frame, ok, w, hh, h[1]);
            }
            frames++;
        } else {
            fprintf(stderr, "bad record tag %u\n", h[0]);
            return 1;
        }
    }
    if (workers > 0) gs_sync();
    QueryPerformanceCounter(&q1);
    double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    fprintf(stderr, "replayed %u VBlanks in %.1f ms (%.2f ms per VBlank, %d pixel workers)\n", frames, ms,
            frames ? ms / frames : 0.0, workers);
    return 0;
}
