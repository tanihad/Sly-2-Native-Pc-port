/* Unit tests for the Direct3D 11 GS backend, milestone M0 (runtime/gpu/):
 *  - device and capabilities on the hardware adapter and on WARP (recorded in the log);
 *  - cs_conformance: int64 helpers, exact integer Z formula, depth encoding, SV_Depth round trip
 *    and GREATER/GEQUAL ties, bit identical to C;
 *  - the render thread and command rings under load (both rings wrap many times), gpu_run_sync;
 *  - picture round trip (upload, compose, read-back) for many sizes, display off;
 *  - "both" mode on GS scenes drawn by the software renderer through the GS thread with pixel
 *    workers (the picture callback feeds the hw backend from a worker): 0 differing pixels;
 *  - the flip-model swap chain on a small window (skipped with --no-window).
 *
 *   test_gs_hw [--adapter hw|warp|both] [--no-window] [--vectors N] */
#include "gs_draw.h"
#include "gpu/gpu.h"
#include "gpu/gpu_cmd.h"
#include "gpu/gpu_verify.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

void intc_raise(int cause) { (void)cause; }
void dmac_raise(int ch) { (void)ch; }
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ---- picture round trip --------------------------------------------------------------------------- */

static int pictures_round_trip(void) {
    static const int sizes[][2] = { { 640, 448 }, { 512, 448 }, { 1, 1 }, { 333, 77 }, { 1024, 1024 }, { 640, 224 },
                                    { 512, 448 }, { 7, 1000 } };
    static uint32_t img[1024 * 1024], back[1024 * 1024];
    int bad = 0;
    for (int t = 0; t < (int)(sizeof sizes / sizeof sizes[0]); t++) {
        int w = sizes[t][0], h = sizes[t][1], stride = 1024;
        for (int i = 0; i < 1024 * h; i++) img[i] = rnd();
        gpu_readback rb;
        memset(&rb, 0, sizeof rb);
        rb.dst = back;
        rb.cap_pixels = 1024 * 1024;
        gpu_picture(img, 1, w, h, stride, (uint32_t)t);
        gpu_read_picture(&rb);
        gpu_wait_readback(&rb);
        CHECK(rb.status == 0 && rb.w == w && rb.h == h && rb.vblank == (uint32_t)t, "read-back %dx%d: status %d size %dx%d",
              w, h, rb.status, rb.w, rb.h);
        int diff = 0;
        for (int y = 0; y < h && rb.status == 0; y++)
            for (int x = 0; x < w; x++)
                if ((back[y * w + x] ^ img[y * stride + x]) & 0xFFFFFF) diff++;
        CHECK(diff == 0, "picture %dx%d: %d pixels differ", w, h, diff);
        bad += diff;
    }
    /* display off: black 640x448 */
    gpu_readback rb;
    memset(&rb, 0, sizeof rb);
    rb.dst = back;
    rb.cap_pixels = 1024 * 1024;
    gpu_picture(NULL, 0, 0, 0, 1024, 99);
    gpu_read_picture(&rb);
    gpu_wait_readback(&rb);
    int nonblack = 0;
    for (int i = 0; i < rb.w * rb.h; i++) nonblack += (back[i] & 0xFFFFFF) != 0;
    CHECK(rb.status == 0 && rb.w == 640 && rb.h == 448 && nonblack == 0, "display off: %dx%d, %d non-black", rb.w, rb.h, nonblack);
    return bad;
}

/* ---- command rings under load --------------------------------------------------------------------- */

static volatile long calls;
static void count_fn(void* arg) { (void)arg; InterlockedIncrement(&calls); }

static void rings_stress(void) {
    static uint32_t img[1024 * 448], back[1024 * 448];
    uint64_t cmd0 = gpu_cmd_written();
    /* 400 pictures of 640x448 = 460 MB through the 64 MB data ring; a read-back every 7th checks
       that the right picture comes out while the ring wraps */
    int bad = 0, checked = 0;
    for (int n = 0; n < 400; n++) {
        for (int i = 0; i < 1024 * 448; i += 97) img[i] = (uint32_t)n * 2654435761u + (uint32_t)i;
        img[0] = (uint32_t)n;
        gpu_picture(img, 1, 640, 448, 1024, (uint32_t)n);
        if (n % 7 == 3) {
            gpu_readback rb;
            memset(&rb, 0, sizeof rb);
            rb.dst = back;
            rb.cap_pixels = 1024 * 448;
            gpu_read_picture(&rb);
            gpu_wait_readback(&rb);
            checked++;
            if (rb.status || rb.vblank != (uint32_t)n || (back[0] & 0xFFFFFF) != ((uint32_t)n & 0xFFFFFF)) bad++;
            for (int i = 0; i < 640 * 448 && !rb.status; i += 97) {
                int x = i % 640, y = i / 640;
                if ((back[y * 640 + x] ^ img[y * 1024 + x]) & 0xFFFFFF) { bad++; break; }
            }
        }
    }
    CHECK(bad == 0, "ring stress: %d of %d read-backs wrong", bad, checked);
    /* many small commands: wrap the 4 MB command ring with synchronous calls mixed in */
    calls = 0;
    for (int n = 0; n < 20000; n++) {
        gpu_picture(NULL, 0, 0, 0, 1024, (uint32_t)n);
        if (n % 1000 == 0) gpu_run_sync(count_fn, NULL);
        /* NOPs of every size, so commands end at every alignment and SKIP padding happens */
        for (int k = 0; k < 4; k++) {
            gpu_cmd_alloc(GPU_OP_NOP, (rnd() % 4096) & ~3u);
            gpu_cmd_commit();
        }
    }
    gpu_finish();
    CHECK(calls == 20, "gpu_run_sync ran %ld times, want 20", calls);
    uint64_t wrote = gpu_cmd_written() - cmd0;
    CHECK(wrote > 2ull * GPU_CMD_RING, "command ring wrapped (%llu bytes written)", (unsigned long long)wrote);
    fprintf(stderr, "  rings: %llu command bytes, %llu commands, %llu publishes, waits cmd %llu data %llu\n",
            (unsigned long long)wrote, (unsigned long long)gpu_cmd_counters.commands,
            (unsigned long long)gpu_cmd_counters.publishes, (unsigned long long)gpu_cmd_counters.cmd_waits,
            (unsigned long long)gpu_cmd_counters.data_waits);
    gpu_frame_stats st[4096];
    int total = 0, n;
    while ((n = gpu_take_frame_stats(st, 4096)) > 0) total += n;
    CHECK(total > 0, "frame statistics delivered (%d)", total);
}

/* ---- GS scenes through the software renderer, verified by "both" --------------------------------- */

enum { PRIM = 0x00, RGBAQ = 0x01, XYZ2 = 0x05, XYOFFSET_1 = 0x18, PRMODECONT = 0x1A, SCISSOR_1 = 0x40, ALPHA_1 = 0x42,
       COLCLAMP = 0x46, TEST_1 = 0x47, FRAME_1 = 0x4C, ZBUF_1 = 0x4E };
static void W(uint32_t r, uint64_t v) { gs_write_reg(r, v); }
static void rgba(int r, int g, int b, int a) { W(RGBAQ, (uint64_t)r | (uint64_t)g << 8 | (uint64_t)b << 16 | (uint64_t)a << 24 | (uint64_t)0x3F800000 << 32); }
static void xyz(int x16, int y16, uint32_t z) { W(XYZ2, (uint64_t)(uint16_t)x16 | (uint64_t)(uint16_t)y16 << 16 | (uint64_t)z << 32); }

static int both_frames, both_diff_frames;
static uint64_t both_diff_px;
static void both_cb(const gpu_pic_diff* d, const uint32_t* sw, const uint32_t* hw, void* user) {
    (void)sw; (void)hw; (void)user;
    both_frames++;
    if (d->diff_px || d->size_mismatch || d->readback_error) {
        both_diff_frames++;
        both_diff_px += d->diff_px;
        if (both_diff_frames <= 3)
            fprintf(stderr, "  vblank %u: %u pixels differ (size %dx%d vs %dx%d, error %d)\n", d->vblank, d->diff_px,
                    d->w, d->h, d->hw_w, d->hw_h, d->readback_error);
    }
}

static void present_cb(const uint32_t* frame, int ok, int w, int h, uint32_t vblank) {
    gpu_verify_picture(frame, ok, w, h, 1024, vblank);
}

static void both_scenes(void) {
    both_frames = both_diff_frames = 0;
    both_diff_px = 0;
    CHECK(gpu_verify_init(4, both_cb, NULL), "verifier init");
    uint64_t v;
    v = 0x66; gs_priv_access(0x12000000, &v, 8, 1);                 /* PMODE: circuit 2 */
    v = 1; gs_priv_access(0x12000020, &v, 8, 1);                    /* SMODE2: INT, frame mode */
    v = 10u << 9; gs_priv_access(0x12000090, &v, 8, 1);             /* DISPFB2: FBP 0, FBW 10, CT32 */
    v = 0x1BF9FF0203227Cull; gs_priv_access(0x120000A0, &v, 8, 1);  /* DISPLAY2 */
    W(PRMODECONT, 1);
    W(FRAME_1, 0 | (10ull << 16));
    W(ZBUF_1, 160 | (0ull << 24));
    W(XYOFFSET_1, 0);
    W(SCISSOR_1, 0 | 639ull << 16 | 0ull << 32 | 447ull << 48);
    W(COLCLAMP, 1);
    W(ALPHA_1, 0 | 1 << 2 | 0 << 4 | 1 << 6 | 0x40ull << 32);      /* (Cs - Cd) * As + Cd */
    for (int f = 0; f < 120; f++) {
        if (f == 60) { v = 0; gs_priv_access(0x12000000, &v, 8, 1); }          /* display off for a frame */
        if (f == 61) { v = 0x66; gs_priv_access(0x12000000, &v, 8, 1); }
        W(TEST_1, 0x30000ull | 1 << 16);                                        /* ZTE, ALWAYS */
        W(PRIM, 6);                                                              /* clear sprite */
        rgba(f * 2, 40, 80, 0x80);
        xyz(0, 0, 0);
        xyz(640 * 16, 448 * 16, 0);
        W(TEST_1, 1 << 16 | 2ull << 17);                                         /* ZTE, GEQUAL */
        for (int t = 0; t < 40; t++) {
            W(PRIM, 3 | 1 << 3 | (t & 1) << 6);                                  /* triangle, Gouraud, ABE */
            for (int k = 0; k < 3; k++) {
                rgba((int)(rnd() & 255), (int)(rnd() & 255), (int)(rnd() & 255), (int)(rnd() & 255));
                xyz((int)(rnd() % (700 * 16)) - 30 * 16, (int)(rnd() % (500 * 16)) - 26 * 16, rnd() & 0xFFFFFF);
            }
        }
        gs_vsync();
        gs_queue_vsync((uint32_t)f);
    }
    gs_sync();
    gpu_verify_flush();
    gpu_verify_free();
    CHECK(both_frames == 120, "both: %d pictures compared, want 120", both_frames);
    CHECK(both_diff_frames == 0, "both: %d pictures differ (%llu pixels)", both_diff_frames, (unsigned long long)both_diff_px);
    fprintf(stderr, "  both: %d GS pictures compared, %d differing\n", both_frames, both_diff_frames);
}

/* ---- swap chain on a window ----------------------------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static void pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static void swapchain_test(int adapter) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"gs_hw_test";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"gs_hw_test", L"gs-hw swap chain test", WS_OVERLAPPEDWINDOW, 40, 40, 480, 400,
                                NULL, NULL, wc.hInstance, NULL);
    CHECK(hwnd != NULL, "test window");
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    pump();
    gpu_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.adapter = adapter;
    cfg.hwnd = hwnd;
    cfg.timestamps = 1;
    int ok = gpu_start(&cfg);
    CHECK(ok, "gpu_start with a window");
    if (ok) {
        const gpu_caps* c = gpu_get_caps();
        CHECK(c->swapchain, "swap chain created");
        fprintf(stderr, "  window: swap chain waitable=%d tearing=%d\n", c->waitable, c->tearing);
        static uint32_t img[1024 * 448], back[1024 * 448];
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        for (int n = 0; n < 90; n++) {
            for (int y = 0; y < 448; y++)
                for (int x = 0; x < 640; x++)
                    img[y * 1024 + x] = (uint32_t)((x + n * 4) & 255) << 16 | (uint32_t)((y + n) & 255) << 8 | (uint32_t)(n * 3 & 255);
            if (n == 45) SetWindowPos(hwnd, NULL, 0, 0, 640, 520, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            gpu_picture(img, 1, 640, 448, 1024, (uint32_t)n);
            pump();
            if (n % 10 == 0) gpu_finish();          /* keep the producer within a few frames */
        }
        gpu_readback rb;
        memset(&rb, 0, sizeof rb);
        rb.dst = back;
        rb.cap_pixels = 1024 * 448;
        gpu_read_picture(&rb);
        while (!rb.done) { pump(); Sleep(1); }
        int diff = 0;
        for (int y = 0; y < 448 && rb.status == 0; y++)
            for (int x = 0; x < 640; x++) diff += ((back[y * 640 + x] ^ img[y * 1024 + x]) & 0xFFFFFF) != 0;
        CHECK(rb.status == 0 && diff == 0, "windowed picture read-back: status %d, %d pixels differ", rb.status, diff);
        gpu_finish();
        QueryPerformanceCounter(&t1);
        gpu_frame_stats st[256];
        int n = gpu_take_frame_stats(st, 256);
        double gms = 0;
        int gn = 0;
        for (int i = 0; i < n; i++) if (st[i].gpu_ms >= 0) { gms += st[i].gpu_ms; gn++; }
        fprintf(stderr, "  window: 90 pictures presented in %.0f ms; %d stats, mean GPU %.3f ms\n",
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, n, gn ? gms / gn : -1.0);
        CHECK(n == 90 && gn == 90, "per-picture stats with GPU time: %d / %d", n, gn);
        gpu_stop();
    }
    DestroyWindow(hwnd);
    pump();
}

/* ---- (M11) start-up checks of the game's renderer -----------------------------------------------------------------
 * cfg.warmup (the game): the device must have what the renderer needs and every game shader is created at start. Each
 * forced failure (PS2_GPU_FAIL) makes gpu_start return 0 with its reason and leaves nothing running - the game then
 * uses the software renderer - and a start after a failure works. Feature level 11_0 (PS2_GPU_FL) and a device without
 * rasterizer-ordered views (PS2_GPU_NO_ROV: the no-ROV shader set) are accepted. */
static void startup_tests(int adapter) {
    gpu_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.adapter = adapter;
    cfg.warmup = 1;
    static const char* const fail[][2] = { { "device", "no Direct3D 11" }, { "caps", "the GPU lacks" },
                                           { "swapchain", "presentation resources" }, { "shader", "cannot create shader" } };
    for (int i = 0; i < 4; i++) {
        _putenv_s("PS2_GPU_FAIL", fail[i][0]);
        int ok = gpu_start(&cfg);
        CHECK(!ok && !gpu_running(), "PS2_GPU_FAIL=%s: gpu_start fails cleanly (%d)", fail[i][0], ok);
        CHECK(strstr(gpu_start_error(), fail[i][1]) != NULL, "PS2_GPU_FAIL=%s: reason '%s'", fail[i][0], gpu_start_error());
        if (ok) gpu_stop();
    }
    _putenv_s("PS2_GPU_FAIL", "");
    int ok = gpu_start(&cfg);
    CHECK(ok && gpu_get_caps()->full && !gpu_no_rov(), "start after the failures, with ROVs (%d)", ok);
    if (ok) { CHECK(pictures_round_trip() == 0, "pictures after the failures"); gpu_stop(); }
    _putenv_s("PS2_GPU_FL", "11_0");
    _putenv_s("PS2_GPU_NO_ROV", "1");
    ok = gpu_start(&cfg);
    CHECK(ok && gpu_get_caps()->feature_level == 0xb000 && !gpu_get_caps()->rovs && gpu_no_rov(),
          "feature level 11_0 without ROVs: started (%d), level %x, no-ROV %d", ok, gpu_get_caps()->feature_level, gpu_no_rov());
    if (ok) { CHECK(pictures_round_trip() == 0, "pictures at 11_0 without ROVs"); gpu_stop(); }
    _putenv_s("PS2_GPU_FL", "");
    _putenv_s("PS2_GPU_NO_ROV", "");
    CHECK(!gpu_no_rov(), "no device: ROVs assumed again");
}

/* ---- per adapter ---------------------------------------------------------------------------------- */

static void run_adapter(int adapter, int window, int vectors) {
    const char* name = adapter == GPU_ADAPTER_WARP ? "WARP" : "hardware";
    fprintf(stderr, "== %s adapter ==\n", name);
    gpu_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.adapter = adapter;
    cfg.timestamps = 1;
    int ok = gpu_start(&cfg);
    CHECK(ok, "%s device", name);
    if (!ok) return;
    const gpu_caps* c = gpu_get_caps();
    gpu_caps_print(c);
    CHECK(c->ok && c->feature_level >= 0xb100, "%s: feature level 11_1 (got %x)", name, c->feature_level);
    CHECK(c->dual_source && c->cb_offsets, "%s: dual-source blending and constant buffer offsets", name);
    if (adapter == GPU_ADAPTER_HW) CHECK(c->full, "hardware adapter supports the exact tiers (ROV, typed UAV loads)");
    fprintf(stderr, "CAPS %s: adapter=\"%s\" fl=%x rov=%d typed_uav_load=%d dual_source=%d cb_offsets=%d doubles=%d full=%d\n",
            name, c->adapter, c->feature_level, c->rovs, c->typed_uav_load, c->dual_source, c->cb_offsets, c->doubles, c->full);

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int conf = gpu_conformance(vectors, 1, 1);
    QueryPerformanceCounter(&t1);
    CHECK(conf == 0, "%s: conformance kernel bit identical to C (%d mismatches)", name, conf);
    fprintf(stderr, "  conformance: %d vectors + 65536 depth texels, %d mismatches, %.0f ms\n", vectors, conf,
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
    /* a second seed */
    conf = gpu_conformance(vectors, 0xC0FFEE, 1);
    CHECK(conf == 0, "%s: conformance (seed 2): %d mismatches", name, conf);

    int pr = pictures_round_trip();
    fprintf(stderr, "  picture round trip: %d differing pixels\n", pr);
    rings_stress();
    both_scenes();
    gpu_stop();
    CHECK(!gpu_running(), "stopped");
    /* restart works */
    ok = gpu_start(&cfg);
    CHECK(ok, "%s: restart", name);
    if (ok) {
        int pr2 = pictures_round_trip();
        CHECK(pr2 == 0, "after restart: %d", pr2);
        gpu_stop();
    }
    if (window) swapchain_test(adapter);
    startup_tests(adapter);
}

int main(int argc, char** argv) {
    int want_hw = 1, want_warp = 1, window = 1, vectors = 1 << 17;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--adapter") && i + 1 < argc) {
            const char* a = argv[++i];
            want_hw = !strcmp(a, "hw") || !strcmp(a, "both");
            want_warp = !strcmp(a, "warp") || !strcmp(a, "both");
        } else if (!strcmp(argv[i], "--no-window")) window = 0;
        else if (!strcmp(argv[i], "--vectors") && i + 1 < argc) vectors = atoi(argv[++i]);
    }
    GS_INIT();
    gs_thread_start(3, present_cb);           /* the scenes run through the GS thread */
    if (want_hw) run_adapter(GPU_ADAPTER_HW, window, vectors);
    if (want_warp) run_adapter(GPU_ADAPTER_WARP, window, vectors);
    fprintf(stderr, "%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
