/* Render a GS recording (runtime/gs_record.c, PS2_GS_RECORD) with the GS code it is linked with,
 * writing every VBlank's picture as <outdir>/frame_<vblank>.png. Built and run by
 * scripts/gs_replay.ps1 against any checkout's runtime/, so two versions of the renderer can be
 * compared on exactly the same input (scripts/compare_frames.py).
 *
 *   gs_replay <recording> <outdir> [workers] [--backend sw|hw|both] [--adapter hw|warp] [--metrics file]
 *
 * workers > 0: through the GS thread with that many pixel workers (deferred drawing), else
 * synchronously. outdir "-": no pictures (benchmark).
 *
 * Backends (notes/d3d11-renderer.md 10.1; built with GS_REPLAY_GPU when runtime/gpu exists):
 *   sw    the software renderer's pictures (default; the only mode without runtime/gpu)
 *   hw    the GPU backend's pictures, read back from the GPU (M0: the all-fallback backend, i.e. the
 *         sw picture uploaded, composed and presented by the render thread)
 *   both  both, compared per VBlank; differing frames are written as frame_N_sw/_hw/_diff.png (at
 *         most 20) and the exit code is 3 if any pixel differs
 * Every mode prints "sw-hash": a 64-bit FNV-1a over the size and RGB of every sw picture in
 * order, so renderer changes that must not change the output can be checked without PNGs.
 * --metrics writes per-VBlank JSON: differing pixel counts per channel above |d| 0/2/16, PSNR,
 * bbox, D3D calls, front/render/GPU ms. Uses only the public GS entry points, so it builds with
 * older GS code too. */
#include "gs_draw.h"

#include <stdlib.h>
#include <windows.h>

#ifdef GS_REPLAY_GPU
#include "gpu/gpu.h"
#include "gpu/gpu_verify.h"
#endif

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);   /* debug.c */

/* neighbours of the GS code in the game */
void intc_raise(int cause) { (void)cause; }
void dmac_raise(int ch) { (void)ch; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}

enum { B_SW, B_HW, B_BOTH };
static const char* outdir;
static int backend = B_SW;
static uint64_t sw_hash = 1469598103934665603ull;
static uint32_t pictures;

static void hash_picture(const uint32_t* frame, int ok, int w, int h, int stride) {
    uint64_t hsh = sw_hash;
#define MIX(v) do { hsh ^= (uint64_t)(v); hsh *= 1099511628211ull; } while (0)
    if (!ok) { MIX(0xDEADu); sw_hash = hsh; return; }
    MIX((uint32_t)w); MIX((uint32_t)h);
    for (int y = 0; y < h; y++) {
        const uint32_t* r = frame + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++) MIX(r[x] & 0xFFFFFF);
    }
#undef MIX
    sw_hash = hsh;
}

static void save_png(const char* suffix, const uint32_t* frame, int w, int h, int stride, uint32_t vblank) {
    if (!strcmp(outdir, "-")) return;     /* "-": no pictures (benchmark) */
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06u%s.png", outdir, vblank, suffix);
    debug_write_png(path, frame, w, h, stride);
}

#ifdef GS_REPLAY_GPU
/* ---- hw / both: per-VBlank results ---- */
typedef struct { gpu_pic_diff d; } frame_rec;
static frame_rec* recs;
static size_t nrecs, caprecs;
static gpu_frame_stats* sts;                     /* per picture, in the same order as recs */
static size_t nsts, capsts;
static uint32_t diff_frames, png_diffs;
static uint64_t diff_pixels;
static double min_psnr = 999.0;

static void take_stats(void) {
    gpu_frame_stats st[256];
    int n;
    while ((n = gpu_take_frame_stats(st, 256)) > 0)
        for (int i = 0; i < n; i++) {
            if (nsts == capsts) {
                capsts = capsts ? capsts * 2 : 4096;
                sts = realloc(sts, capsts * sizeof *sts);
                if (!sts) { fprintf(stderr, "out of memory\n"); exit(1); }
            }
            sts[nsts++] = st[i];
        }
}

static void diff_png(const gpu_pic_diff* d, const uint32_t* sw, const uint32_t* hw) {
    static uint32_t heat[1024 * 1024];
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++) {
            uint32_t a = sw[y * d->w + x], b = hw[y * d->w + x];
            int m = 0;
            for (int s = 0; s < 24; s += 8) {
                int e = abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF));
                if (e > m) m = e;
            }
            uint32_t g = (a >> 8 & 0xFF) / 4;          /* dim sw picture underneath */
            heat[y * d->w + x] = m ? 0xFF0000u | (uint32_t)(m * 8 > 255 ? 255 : m * 8) << 8 : g << 16 | g << 8 | g;
        }
    save_png("_diff", heat, d->w, d->h, d->w, d->vblank);
}

static void verify_cb(const gpu_pic_diff* d, const uint32_t* sw, const uint32_t* hw, void* user) {
    (void)user;
    if (nrecs == caprecs) {
        caprecs = caprecs ? caprecs * 2 : 4096;
        recs = realloc(recs, caprecs * sizeof *recs);
        if (!recs) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    memset(&recs[nrecs], 0, sizeof recs[nrecs]);
    recs[nrecs++].d = *d;
    if (d->diff_px) {
        diff_frames++;
        diff_pixels += d->diff_px;
        if (diff_frames <= 5)
            fprintf(stderr, "vblank %u: %u pixels differ (bbox %d,%d-%d,%d, PSNR %.2f%s%s)\n", d->vblank, d->diff_px,
                    d->bx0, d->by0, d->bx1, d->by1, d->psnr, d->size_mismatch ? ", size mismatch" : "",
                    d->readback_error ? ", read-back error" : "");
    }
    if (d->psnr < min_psnr) min_psnr = d->psnr;
    if (backend == B_HW && hw) save_png("", hw, d->w, d->h, d->w, d->vblank);
    if (backend == B_BOTH && d->diff_px && hw && png_diffs < 20) {
        png_diffs++;
        save_png("_sw", sw, d->w, d->h, d->w, d->vblank);
        save_png("_hw", hw, d->w, d->h, d->w, d->vblank);
        diff_png(d, sw, hw);
    }
    if ((nrecs & 63) == 0) take_stats();
}

static int cmp_dbl(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double median(double* v, size_t n) {
    if (!n) return -1;
    qsort(v, n, sizeof *v, cmp_dbl);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static void write_metrics(const char* path, const char* rec, const char* adapter, double replay_ms) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    const gpu_caps* c = gpu_get_caps();
    const char* json_rec = rec;
    fprintf(f, "{\n  \"recording\": \"");
    for (const char* p = json_rec; *p; p++) fprintf(f, *p == '\\' ? "\\\\" : *p == '"' ? "\\\"" : "%c", *p);
    fprintf(f, "\",\n  \"backend\": \"%s\",\n  \"adapter\": \"%s\",\n", backend == B_HW ? "hw" : "both", adapter);
    fprintf(f, "  \"caps\": {\"name\": \"%s\", \"feature_level\": \"%x\", \"rov\": %d, \"typed_uav_load\": %d, "
               "\"dual_source\": %d, \"cb_offsets\": %d, \"full\": %d},\n",
            c->adapter, c->feature_level, c->rovs, c->typed_uav_load, c->dual_source, c->cb_offsets, c->full);
    double* rm = malloc((nrecs + 1) * sizeof *rm);
    double* gm = malloc((nrecs + 1) * sizeof *gm);
    size_t nr = 0, ng = 0;
    fprintf(f, "  \"frames\": [\n");
    for (size_t i = 0; i < nrecs; i++) {
        const gpu_pic_diff* d = &recs[i].d;
        const gpu_frame_stats* s = i < nsts ? &sts[i] : NULL;
        fprintf(f, "    {\"vblank\": %u, \"w\": %d, \"h\": %d, \"diff_px\": %u, \"r\": [%u, %u, %u], \"g\": [%u, %u, %u], "
                   "\"b\": [%u, %u, %u], \"psnr\": %.3f, \"bbox\": [%d, %d, %d, %d]",
                d->vblank, d->w, d->h, d->diff_px, d->diff[0][0], d->diff[0][1], d->diff[0][2], d->diff[1][0],
                d->diff[1][1], d->diff[1][2], d->diff[2][0], d->diff[2][1], d->diff[2][2], d->psnr, d->bx0, d->by0,
                d->bx1, d->by1);
        if (d->size_mismatch || d->readback_error)
            fprintf(f, ", \"hw_w\": %d, \"hw_h\": %d, \"readback_error\": %d", d->hw_w, d->hw_h, d->readback_error);
        if (s) {
            fprintf(f, ", \"draws\": %u, \"dispatches\": %u, \"copies\": %u, \"uploads\": %u, \"upload_bytes\": %u, "
                       "\"front_ms\": %.4f, \"render_ms\": %.4f, \"gpu_ms\": %.4f",
                    s->draws, s->dispatches, s->copies, s->uploads, s->upload_bytes, s->front_ms, s->render_ms, s->gpu_ms);
            rm[nr++] = s->render_ms;
            if (s->gpu_ms >= 0) gm[ng++] = s->gpu_ms;
        }
        fprintf(f, "}%s\n", i + 1 < nrecs ? "," : "");
    }
    fprintf(f, "  ],\n");
    double rmed = median(rm, nr), gmed = median(gm, ng);
    fprintf(f, "  \"summary\": {\"frames\": %zu, \"diff_frames\": %u, \"diff_px\": %llu, \"min_psnr\": %.3f, "
               "\"sw_hash\": \"%016llx\", \"replay_ms\": %.1f, \"render_ms_median\": %.4f, \"gpu_ms_median\": %.4f}\n}\n",
            nrecs, diff_frames, (unsigned long long)diff_pixels, min_psnr, (unsigned long long)sw_hash, replay_ms, rmed, gmed);
    free(rm);
    free(gm);
    fclose(f);
}
#endif

/* the picture of a VBlank (on the GS thread's pixel worker with workers, else on this thread) */
static void picture(const uint32_t* frame, int ok, int w, int h, uint32_t vblank) {
    pictures++;
    hash_picture(frame, ok, w, h, 1024);
#ifdef GS_REPLAY_GPU
    if (backend != B_SW) { gpu_verify_picture(frame, ok, w, h, 1024, vblank); return; }
#endif
    if (ok) save_png("", frame, w, h, 1024, vblank);
}

static int rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: gs_replay <recording> <outdir> [workers] [--backend sw|hw|both] [--adapter hw|warp] "
                        "[--metrics file]\n");
        return 2;
    }
    outdir = argv[2];
    int workers = 0;
    const char* adapter = "hw";
    const char* metrics = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            const char* b = argv[++i];
            backend = !strcmp(b, "hw") ? B_HW : !strcmp(b, "both") ? B_BOTH : B_SW;
            if (strcmp(b, "sw") && backend == B_SW) { fprintf(stderr, "unknown backend %s\n", b); return 2; }
        } else if (!strcmp(argv[i], "--adapter") && i + 1 < argc) adapter = argv[++i];
        else if (!strcmp(argv[i], "--metrics") && i + 1 < argc) metrics = argv[++i];
        else workers = atoi(argv[i]);
    }
#ifndef GS_REPLAY_GPU
    if (backend != B_SW) { fprintf(stderr, "this runtime has no GPU backend (runtime/gpu): only --backend sw\n"); return 2; }
    (void)adapter; (void)metrics;
#else
    if (backend != B_SW) {
        gpu_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.adapter = !strcmp(adapter, "warp") ? GPU_ADAPTER_WARP : GPU_ADAPTER_HW;
        cfg.timestamps = 1;
        if (!gpu_start(&cfg)) { fprintf(stderr, "GPU backend unavailable\n"); return 1; }
        gpu_caps_print(gpu_get_caps());
        if (!gpu_verify_init(4, verify_cb, NULL)) { fprintf(stderr, "out of memory\n"); return 1; }
    }
#endif
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

    if (workers > 0) gs_thread_start(workers, picture);
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
                picture(frame, ok, w, hh, h[1]);
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
    fprintf(stderr, "sw-hash %016llx over %u pictures\n", (unsigned long long)sw_hash, pictures);
    int rc = 0;
#ifdef GS_REPLAY_GPU
    if (backend != B_SW) {
        gpu_verify_flush();                      /* the GS thread is idle: this thread is the producer now */
        gpu_finish();
        take_stats();
        fprintf(stderr, "%s: %zu pictures compared, %u differ (%llu pixels), min PSNR %.2f dB\n",
                backend == B_HW ? "hw" : "both", nrecs, diff_frames, (unsigned long long)diff_pixels, min_psnr);
        if (metrics) write_metrics(metrics, argv[1], adapter, ms);
        gpu_verify_free();
        gpu_stop();
        if (backend == B_BOTH && diff_frames) rc = 3;
    }
#endif
    return rc;
}
