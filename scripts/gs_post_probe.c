/* Trace what Sly 2's post passes do to the two 512x224 PSMCT32 frame buffers (FBP 000 / 038), by
 * replaying a GS recording through this checkout's software GS (synchronously) and snapshotting both
 * buffers every time FRAME_1/FRAME_2 changes inside a range of VBlanks. For each step it prints which
 * channels of which buffer changed and, for every changed channel, the best exact source among the
 * channels of both buffers before the step (a "byte move" detector for the channel shuffles).
 * Built by scripts/gs_post_probe.ps1 (same sources as gs_replay).
 *
 *   gs_post_probe <recording> <first VBlank index> <last VBlank index> */
#include "gs_draw.h"

#include <stdlib.h>
#include <windows.h>

void intc_raise(int cause) { (void)cause; }
void dmac_raise(int ch) { (void)ch; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}

#define W 512
#define H 224
static uint32_t prev[2][W * H], cur[2][W * H];
static const uint32_t fbp_of[2] = { 0x000, 0x038 };

static void grab(uint32_t out[2][W * H]) {
    gs_draw_sync();
    const gs_fmt* f = gs_fmt_of(0);
    for (int b = 0; b < 2; b++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t a = gs_swz_addr(f, fbp_of[b], 8, x, y);
                memcpy(&out[b][y * W + x], gs_vram + a, 4);
            }
}

static int ch(uint32_t p, int c) { return (int)((p >> (8 * c)) & 0xFF); }

static void report(const char* why) {
    static const char* cn = "RGBA";
    grab(cur);
    printf("%s\n", why);
    for (int b = 0; b < 2; b++) {
        for (int c = 0; c < 4; c++) {
            int changed = 0;
            for (int i = 0; i < W * H; i++) changed += ch(cur[b][i], c) != ch(prev[b][i], c);
            if (!changed) continue;
            /* best source: same pixel, any buffer/channel before the step; also bit-subset matches */
            int best = -1, bestn = -1;
            for (int sb = 0; sb < 2; sb++)
                for (int sc = 0; sc < 4; sc++) {
                    int n = 0;
                    for (int i = 0; i < W * H; i++)
                        if (ch(cur[b][i], c) != ch(prev[b][i], c)) n += ch(cur[b][i], c) == ch(prev[sb][i], sc);
                    if (n > bestn) { bestn = n; best = sb * 4 + sc; }
                }
            int dmin = 999, dmax = -999;
            long long dsum = 0;
            for (int i = 0; i < W * H; i++)
                if (ch(cur[b][i], c) != ch(prev[b][i], c)) {
                    int d = ch(cur[b][i], c) - ch(prev[b][i], c);
                    if (d < dmin) dmin = d;
                    if (d > dmax) dmax = d;
                    dsum += d;
                }
            printf("   buf %03X %c: %6d px changed (delta %d..%d avg %.1f); best exact source %03X.%c %d/%d\n", fbp_of[b],
                   cn[c], changed, dmin, dmax, (double)dsum / changed, fbp_of[best / 4], cn[best % 4], bestn, changed);
        }
        /* alpha histogram summary of each buffer */
        int hist[8] = { 0 };
        for (int i = 0; i < W * H; i++) hist[ch(cur[b][i], 3) >> 5]++;
        printf("   buf %03X alpha /32 histogram:", fbp_of[b]);
        for (int k = 0; k < 8; k++) printf(" %d", hist[k]);
        printf("\n");
    }
    memcpy(prev, cur, sizeof prev);
}

static int rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: gs_post_probe <recording> <first vblank idx> <last vblank idx>\n"); return 2; }
    int first = atoi(argv[2]), last = atoi(argv[3]);
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
    {   /* CLUT buffer, as in gs_replay.c */
        uint32_t img[256];
        for (int i = 0; i < 256; i++)
            img[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)] = clut[i] | (uint32_t)clut[i + 256] << 16;
        gs_exec_write_reg(0x50, 1ull << 48);
        gs_exec_write_reg(0x51, 0);
        gs_exec_write_reg(0x52, 16 | 16ull << 32);
        gs_exec_write_reg(0x53, 0);
        gs_exec_image_write(img, sizeof img);
        gs_exec_write_reg(0x06, 0x13ull << 20 | 1ull << 61);
    }
    for (uint32_t r = 0; r < 0x64; r++) {
        if (r == 0x04 || r == 0x05 || r == 0x0C || r == 0x0D || r == 0x16 || r == 0x17 || r == 0x53 || r == 0x54 ||
            (r >= 0x60 && r <= 0x62))
            continue;
        uint64_t v = regs[r];
        if (r == 0x06 || r == 0x07) v &= ~(7ull << 61);
        gs_exec_write_reg(r, v);
    }
    memcpy(gs_vram, vram, sizeof vram);
    gs_tex_flush_all();

    uint64_t frame1 = regs[0x4C], frame2 = regs[0x4D], tex0 = regs[0x06], alpha = regs[0x42], test = regs[0x47], zbuf = regs[0x4E];
    int vb = 0, armed = 0;
    for (;;) {
        uint32_t h[2];
        if (!rd(f, h, 8)) break;
        if (h[0] == 1) {
            uint64_t v;
            if (!rd(f, &v, 8)) break;
            int inr = vb >= first && vb <= last;
            if (inr && !armed) { grab(prev); armed = 1; printf("== VBlank index %d: start\n", vb); }
            if (inr && (h[1] == 0x4C || h[1] == 0x4D)) {
                uint64_t* fr = h[1] == 0x4C ? &frame1 : &frame2;
                if (v != *fr) {
                    char why[256];
                    snprintf(why, sizeof why, "-- before FRAME_%d <- FBP %03X FBW %u PSM %02X FBMSK %08X | previous step: TEX0 %016llX ALPHA %010llX TEST %05llX ZBUF %08llX",
                             h[1] == 0x4C ? 1 : 2, (unsigned)(v & 0x1FF), (unsigned)((v >> 16) & 0x3F), (unsigned)((v >> 24) & 0x3F),
                             (unsigned)(v >> 32), (unsigned long long)tex0, (unsigned long long)alpha, (unsigned long long)test,
                             (unsigned long long)zbuf);
                    report(why);
                }
                *fr = v;
            }
            if (h[1] == 0x06) tex0 = v;
            if (h[1] == 0x42) alpha = v;
            if (h[1] == 0x47) test = v;
            if (h[1] == 0x4E) zbuf = v;
            gs_write_reg(h[1], v);
        } else if (h[0] == 2) {
            uint32_t n = (h[1] + 3) & ~3u;
            uint8_t* buf = malloc(n ? n : 4);
            if (!rd(f, buf, n)) break;
            gs_image_write(buf, h[1]);
            free(buf);
        } else if (h[0] == 3) {
            uint64_t snap[GS_PRIV_SNAPSHOT];
            if (!rd(f, snap, sizeof snap)) break;
            if (vb >= first && vb <= last) {
                char why[64];
                snprintf(why, sizeof why, "-- VBlank index %d ends (DISPFB2 FBP %03X)", vb, (unsigned)(snap[9] & 0x1FF));
                report(why);
            }
            for (int i = 0; i < 15; i++) gs_priv_access(0x12000000u + 16u * (uint32_t)i, &snap[i], 8, 1);
            gs_vsync();
            vb++;
            if (vb > last) break;
        } else {
            fprintf(stderr, "bad record tag %u\n", h[0]);
            return 1;
        }
    }
    return 0;
}
