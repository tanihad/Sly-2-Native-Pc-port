/* The Direct3D 11 GS backend in the game (gpu_hw.h, notes/d3d11-renderer.md 11 M6). */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include "gpu_hw.h"
#include "gpu.h"
#include "gpu_front.h"
#include "gpu_verify.h"
#include "settings.h"

#include <dxgi1_2.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);   /* debug.c */
void gs_record_request(const char* path, uint32_t frames);                                  /* gs_record.c */

static gpu_hw_config C;
static int render_on;                    /* the render thread runs (hw, or sw pictures after a switch) */
static volatile LONG active;             /* the GPU draws */
static volatile LONG switch_req;
static int losses;
static uint32_t seen_thread_punts, same_thread_until;
static volatile LONG last_punt_vblank;   /* GS thread (stats drain): the last VBlank with a PUNT */
static volatile LONG front_threaded;     /* mirror of gpu_front_threaded() for the GS thread */
static uint32_t lose_at[16];             /* PS2_GPU_LOSE_AT */
static int nlose_at;
static uint32_t switch_at[16];           /* PS2_GPU_SWITCH_AT: backend switches at these VBlanks (tests) */
static int nswitch_at;

static void hlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[gpu] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

/* ---- pictures done (render thread) ------------------------------------------------------------- */
static volatile LONG presents;           /* pictures reported (stats: flips per second) */

static void on_picture_done(uint32_t vblank, void* user) {
    (void)vblank; (void)user;
    InterlockedIncrement(&presents);
    if (C.presented) C.presented();
    gs_picture_presented();
}

/* ---- frame dumps: read-backs completed on a thread of their own ------------------------------------ */
#define NDUMP 4
static struct { gpu_readback rb; uint32_t* px; uint32_t vblank; int content43; volatile LONG busy; } dumps[NDUMP];
static volatile LONG dump_head, dump_tail;   /* queued: [head, tail) */
static HANDLE dump_thread, dump_event;

static DWORD WINAPI dump_main(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(dump_event, INFINITE);
        while (dump_head != dump_tail) {
            int i = (int)(dump_head % NDUMP);
            gpu_wait_readback(&dumps[i].rb);
            int ok = dumps[i].rb.status == 0 && dumps[i].rb.w > 0;   /* (no picture: display off, or lost) */
            if (C.dump) C.dump(ok ? dumps[i].px : NULL, ok ? dumps[i].rb.w : 0, ok ? dumps[i].rb.h : 0, dumps[i].vblank, dumps[i].content43);
            InterlockedExchange(&dumps[i].busy, 0);
            InterlockedIncrement(&dump_head);
        }
    }
}

/* a read-back slot for this VBlank's picture, or NULL (dumps disabled, or all slots still busy) */
static gpu_readback* dump_slot(uint32_t vblank, int content43) {
    if (!C.dump || !C.dump_wanted || !C.dump_wanted(vblank)) return NULL;
    if (!dump_thread) {
        for (int i = 0; i < NDUMP; i++)
            if (!(dumps[i].px = malloc(1024 * 1024 * 4))) return NULL;
        dump_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        dump_thread = CreateThread(NULL, 0, dump_main, NULL, 0, NULL);
        if (!dump_thread) return NULL;
        SetThreadPriority(dump_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    int i = (int)(dump_tail % NDUMP);
    if (dump_tail - dump_head >= NDUMP || dumps[i].busy) {
        static int logged;
        if (!logged++) hlog("frame dump of VBlank %u skipped: the dump thread is behind", vblank);
        return NULL;
    }
    dumps[i].busy = 1;
    dumps[i].vblank = vblank;
    dumps[i].content43 = content43;
    memset(&dumps[i].rb, 0, sizeof dumps[i].rb);
    dumps[i].rb.dst = dumps[i].px;
    dumps[i].rb.cap_pixels = 1024 * 1024;
    return &dumps[i].rb;
}

static void dump_queued(void) {
    InterlockedIncrement(&dump_tail);
    SetEvent(dump_event);
}

/* ---- PS2_GS_HW_STATS ----------------------------------------------------------------------------- */
#define SW 256                           /* samples per report */
static struct {
    double gs[SW], front[SW], rcpu[SW], rwall[SW], gpu[SW], pres[SW];
    int ngs, nfront, nr, ngpu;
    uint64_t draws, ff, rov, rov_runs, punts, downloads, loads, display_mem, kicks;
    uint32_t frames, targets;
    LONG presents0;
    LARGE_INTEGER t0;
    uint64_t gs_cyc;
    double tsc_per_ms;
} S;

static int dcmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static double med(double* v, int n) { if (!n) return 0; qsort(v, (size_t)n, sizeof *v, dcmp); return v[n / 2]; }
static double vmax(const double* v, int n) { double m = 0; for (int i = 0; i < n; i++) if (v[i] > m) m = v[i]; return m; }

static double tsc_ms(void) {
    if (!S.tsc_per_ms) {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&b); while (b.QuadPart - a.QuadPart < f.QuadPart / 50);
        S.tsc_per_ms = (double)(__rdtsc() - t0) / ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    }
    return S.tsc_per_ms;
}

/* GS thread, every VBlank: the frontend's and the render thread's per-frame records are taken (they
 * pile up otherwise) and, with PS2_GS_HW_STATS, summarised once a second */
static void stats_vblank(uint32_t vblank) {
    static gpu_front_frame ff[16];
    static gpu_frame_stats fs[64];
    int n;
    while ((n = gpu_front_take_frames(ff, 16)) > 0)
        for (int i = 0; i < n; i++) {
            const gpu_front_frame* f = &ff[i];
            uint32_t punts = f->tier[GPU_TIER_PUNT];
            if (punts) InterlockedExchange(&last_punt_vblank, (LONG)f->vblank);
            if (C.stats > 1 && (f->downloads || f->loads || f->xcopies || punts))   /* PS2_GS_HW_STATS=2: per frame */
                hlog("frame v%u: downloads %u (%u blocks), loads %u (%u blocks), cross copies %u, PUNT %u, targets created %u",
                     f->vblank, f->downloads, f->download_blocks, f->loads, f->load_blocks, f->xcopies, punts, f->targets_created);
            if (!C.stats) continue;
            if (S.nfront < SW) S.front[S.nfront++] = f->thread_ms;
            S.draws += f->d3d_draws; S.ff += f->ff_draws; S.rov += f->rov_draws; S.rov_runs += f->rov_runs;
            S.punts += punts; S.downloads += f->downloads; S.loads += f->loads; S.display_mem += f->display_mem;
            S.kicks += f->kicks;
            S.targets = f->targets_live;
            S.frames++;
        }
    while ((n = gpu_take_frame_stats(fs, 64)) > 0)
        for (int i = 0; i < n && C.stats; i++) {
            if (fs[i].partial) continue;
            if (S.nr < SW) { S.rcpu[S.nr] = fs[i].render_cpu_ms; S.rwall[S.nr] = fs[i].render_ms; S.pres[S.nr] = fs[i].present_ms; S.nr++; }
            if (fs[i].gpu_ms >= 0 && S.ngpu < SW) S.gpu[S.ngpu++] = fs[i].gpu_ms;
        }
    if (!C.stats) return;
    ULONG64 cyc = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    if (S.gs_cyc && S.ngs < SW) S.gs[S.ngs++] = (double)(cyc - S.gs_cyc) / tsc_ms();
    S.gs_cyc = cyc;
    if (vblank % 60) return;
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    LONG p = presents;
    double secs = S.t0.QuadPart ? (double)(t.QuadPart - S.t0.QuadPart) / (double)f.QuadPart : 0;
    if (secs > 0) {
        double fps = (double)(p - S.presents0) / secs, k = S.frames ? 1.0 / S.frames : 0;
        double gsm = vmax(S.gs, S.ngs), frm = vmax(S.front, S.nfront), rcm = vmax(S.rcpu, S.nr), rwm = vmax(S.rwall, S.nr), gpm = vmax(S.gpu, S.ngpu);
        hlog("stats v%u: %.1f flips/s | GS thread %.2f / %.2f ms | frontend %s %.2f / %.2f | render CPU %.2f / %.2f, wall %.2f / %.2f, "
             "present %.2f | GPU %.2f / %.2f | per VBlank: %.0f prims, %.0f draws (FF %.0f, ROV %.0f in %.1f runs), PUNT %llu, "
             "downloads %.1f, loads %.1f, display from memory %.2f, targets %u  (median / max ms per VBlank)",
             vblank, fps, med(S.gs, S.ngs), gsm, front_threaded ? "thread" : "(GS thread)", med(S.front, S.nfront), frm,
             med(S.rcpu, S.nr), rcm, med(S.rwall, S.nr), rwm, med(S.pres, S.nr), med(S.gpu, S.ngpu), gpm, (double)S.kicks * k,
             (double)S.draws * k, (double)S.ff * k, (double)S.rov * k, (double)S.rov_runs * k, (unsigned long long)S.punts,
             (double)S.downloads * k, (double)S.loads * k, (double)S.display_mem * k, S.targets);
    }
    S.ngs = S.nfront = S.nr = S.ngpu = 0;
    S.draws = S.ff = S.rov = S.rov_runs = S.punts = S.downloads = S.loads = S.display_mem = S.kicks = 0;
    S.frames = 0;
    S.t0 = t;
    S.presents0 = p;
}

/* ---- render thread and frontend ---------------------------------------------------------------- */
static int render_start(void) {
    gpu_config gc;
    memset(&gc, 0, sizeof gc);
    gc.adapter = C.adapter;
    gc.hwnd = C.hwnd;
    gc.present_w = 1280;
    gc.present_h = 960;
    gc.tearing = C.pacing == SETTINGS_PACING_VRR;
    gc.timestamps = C.stats != 0;
    gc.client_size = C.client_size;
    if (!gpu_start(&gc)) return 0;
    if (!gpu_get_caps()->full) {
        hlog("this GPU lacks what the GPU renderer needs (feature level 11_1, rasterizer-ordered views, typed UAV loads)");
        gpu_stop();
        return 0;
    }
    return 1;
}

static int punt_reader(uint8_t* out, void* user) { (void)user; return gpu_exec_read_memory(NULL, NULL, 0, out); }

static void front_start(void) {
    gpu_front_config fc;
    memset(&fc, 0, sizeof fc);
    fc.mixed = 1;                        /* FF + ROV frames, the M5 default mode */
    fc.scale = 1;                        /* (M7: scaling) */
    fc.display_ky2 = 0;
    fc.sw = C.verify > 0;                /* verify: the software renderer draws too (same thread) */
    fc.thread = !fc.sw && !C.same_thread;
    gpu_exec_reset();                    /* (a restart: the render thread forgets the old targets) */
    gpu_front_start(&fc);
    gpu_front_set_sink(gpu_submit_sink, NULL);
    gpu_front_set_mem_reader(punt_reader, NULL);
    InterlockedExchange(&front_threaded, fc.thread);
    seen_thread_punts = gpu_front_thread_punts();
}

/* ---- display-driven VBlanks (pacing = display) ----------------------------------------------------- */
static HANDLE clock_thread;

static IDXGIOutput* find_output(void) {
    static HMODULE m;
    if (!m) m = LoadLibraryW(L"dxgi.dll");
    typedef HRESULT (WINAPI * create_fn)(REFIID, void**);
    create_fn create = m ? (create_fn)(void*)GetProcAddress(m, "CreateDXGIFactory1") : NULL;
    static const IID iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    IDXGIFactory1* f = NULL;
    if (!create || FAILED(create(&iid_factory1, (void**)&f))) return NULL;
    HMONITOR mon = MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY);
    IDXGIOutput* found = NULL;
    IDXGIAdapter1* ad = NULL;
    for (UINT a = 0; !found && SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, a, &ad)); a++) {
        IDXGIOutput* o = NULL;
        for (UINT k = 0; !found && SUCCEEDED(IDXGIAdapter1_EnumOutputs(ad, k, &o)); k++) {
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(IDXGIOutput_GetDesc(o, &d)) && d.Monitor == mon) found = o;
            else IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(f);
    return found;
}

static DWORD WINAPI clock_main(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    IDXGIOutput* o = find_output();
    if (!o) { hlog("display pacing: no DXGI output for the window: the 59.94 Hz timer stays"); return 0; }
    /* the display's rate, measured: display pacing only on a 59.94 / 60 Hz display (within 0.5%) */
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    IDXGIOutput_WaitForVBlank(o);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 30; i++) IDXGIOutput_WaitForVBlank(o);
    QueryPerformanceCounter(&b);
    double hz = 30.0 * (double)f.QuadPart / (double)(b.QuadPart - a.QuadPart);
    if (hz < 59.94 * 0.995 || hz > 59.94 * 1.005) {
        hlog("display pacing: the display runs at %.2f Hz, not 59.94/60: the 59.94 Hz timer stays", hz);
        IDXGIOutput_Release(o);
        return 0;
    }
    hlog("display pacing: VBlanks from the display (%.3f Hz)", hz);
    C.vblank_external(1);
    HMONITOR mon = MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY);
    for (uint32_t n = 0;; n++) {
        if (FAILED(IDXGIOutput_WaitForVBlank(o))) { Sleep(16); }
        else C.display_vblank();
        if ((n & 127) == 127 && MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY) != mon) {   /* the window moved */
            IDXGIOutput_Release(o);
            if (!(o = find_output())) { C.vblank_external(0); hlog("display pacing: output lost: the 59.94 Hz timer again"); return 0; }
            mon = MonitorFromWindow((HWND)C.hwnd, MONITOR_DEFAULTTOPRIMARY);
        }
    }
}

/* ---- start / stop -------------------------------------------------------------------------------- */
int gpu_hw_start(const gpu_hw_config* cfg) {
    C = *cfg;
    if (C.verify) C.same_thread = 1;
    const char* e = getenv("PS2_GPU_LOSE_AT");
    for (const char* s = e; s && *s && nlose_at < 16; s = strchr(s, ',') ? strchr(s, ',') + 1 : "")
        lose_at[nlose_at++] = (uint32_t)strtoul(s, NULL, 10);
    e = getenv("PS2_GPU_SWITCH_AT");
    for (const char* s = e; s && *s && nswitch_at < 16; s = strchr(s, ',') ? strchr(s, ',') + 1 : "")
        switch_at[nswitch_at++] = (uint32_t)strtoul(s, NULL, 10);
    gpu_set_picture_done(on_picture_done, NULL);
    if (!render_start()) return 0;
    render_on = 1;
    gpu_caps_print(gpu_get_caps());
    front_start();
    InterlockedExchange(&active, 1);
    hlog("GPU renderer on (%s, frontend on %s%s, pacing %s)", C.hwnd ? "window" : "headless",
         front_threaded ? "its own thread" : "the GS thread", C.verify ? ", verify" : "",
         C.pacing == SETTINGS_PACING_VRR ? "vrr" : C.pacing == SETTINGS_PACING_DISPLAY ? "display" : "vsync");
    if (C.pacing == SETTINGS_PACING_DISPLAY && C.hwnd && C.vblank_external && C.display_vblank)
        clock_thread = CreateThread(NULL, 0, clock_main, NULL, 0, NULL);
    return 1;
}

void gpu_hw_attach_gs(void) {
    gs_set_vblank_hook(gpu_hw_vblank);
    gs_set_read_hook(gpu_hw_sync_memory);
}

int gpu_hw_active(void) { return active != 0; }

void gpu_hw_stop(void) {
    if (active) gpu_front_stop();
    InterlockedExchange(&active, 0);
    if (render_on) gpu_stop();
    render_on = 0;
}

/* ---- GS memory read-back ------------------------------------------------------------------------- */
static gpu_dl_entry* rb_e;
static uint16_t* rb_w;
static uint8_t* rb_mem;

static int read_gpu_memory(int* n_out) {   /* (frontend thread, or the GS side with the frontend idle) */
    if (!rb_mem) {
        rb_mem = malloc(4u << 20);
        rb_e = malloc(16384 * sizeof *rb_e);
        rb_w = malloc(16384 * sizeof *rb_w);
        if (!rb_mem || !rb_e || !rb_w) { hlog("out of memory"); exit(1); }
    }
    gpu_front_checkpoint();              /* everything so far to the render thread */
    int n = gpu_front_gpu_blocks(rb_e, rb_w, 16384);
    *n_out = n;
    return gpu_exec_read_memory(rb_e, rb_w, n, rb_mem);
}

static void sync_fn(void* arg) {
    int* ok = arg;
    int n = 0;
    *ok = read_gpu_memory(&n);
    if (!*ok) return;
    for (int i = 0; i < n; i++) {        /* the GPU words of each block into gs_vram */
        uint32_t b = rb_e[i].abs;
        uint32_t* d = (uint32_t*)(gs_vram + (size_t)b * 256);
        const uint32_t* s = (const uint32_t*)(rb_mem + (size_t)b * 256);
        for (int k = 0; k < 64; k++) if (rb_e[i].words >> k & 1) d[k] = s[k];
    }
}

void gpu_hw_sync_memory(void) {
    if (!active) return;
    int ok = 0;
    gpu_front_call(sync_fn, &ok);
    if (!ok) hlog("GS memory read-back failed (device lost?): gs_vram keeps what it has");
}

typedef struct { uint8_t* out; int ok; } readmem_arg;
static void readmem_fn(void* arg) {
    readmem_arg* a = arg;
    int n = 0;
    a->ok = read_gpu_memory(&n);
    if (a->ok) memcpy(a->out, rb_mem, 4u << 20);
}

int gpu_hw_read_memory(uint8_t* out) {
    if (!active) return 0;
    readmem_arg a = { out, 0 };
    gpu_front_call(readmem_fn, &a);
    return a.ok;
}

/* ---- the VBlank (GS thread) ---------------------------------------------------------------------- */
static void verify_vblank(const gs_display_snap* snap, uint32_t vblank);

void gpu_hw_vblank(const gs_display_snap* snap, uint32_t vblank, int content43) {
    if (!active) return;                 /* (the software renderer draws: its own pictures) */
    gpu_display_info di;
    memset(&di, 0, sizeof di);
    memcpy(di.priv, snap->priv, sizeof di.priv);
    di.vblank = vblank;
    gs_display_layout L;
    di.field_code = (uint8_t)(gs_display_layout_of(snap->priv, &L) && L.field_mode ? gs_display_field_code(snap, &L) : 0xFF);
    di.content43 = (uint8_t)content43;
    int aw = 4, ah = 3;
    if (C.aspect) C.aspect(content43, &aw, &ah);
    di.aspect_w = (uint16_t)aw;
    di.aspect_h = (uint16_t)ah;
    di.flags = GPU_DI_SHOW | GPU_DI_SIGNAL;
    gpu_readback* rb = dump_slot(vblank, content43);
    di.rb = rb;
    gpu_front_vblank_display(&di);
    if (rb) dump_queued();
    if (C.verify > 0 && vblank % (uint32_t)C.verify == 0) verify_vblank(snap, vblank);
    stats_vblank(vblank);
}

/* ---- PS2_GS_VERIFY (GS thread; the frontend runs on it, the software renderer draws too) ----------- */
static int verify_kind(uint32_t id) { return gpu_front_target_kind(id); }

static void verify_vblank(const gs_display_snap* snap, uint32_t vblank) {
    static uint16_t writer[16384];
    static uint32_t checks, bad, tolerated;
    gs_draw_sync();                      /* the software renderer has drawn everything up to this VBlank */
    int n = 0;
    if (!read_gpu_memory(&n)) { hlog("verify v%u: GPU memory read-back failed", vblank); return; }
    memset(writer, 0, sizeof writer);
    for (int i = 0; i < n; i++) writer[rb_e[i].abs] = rb_w[i];
    gpu_mem_diff d;
    gpu_mem_compare(rb_mem, gs_vram, writer, verify_kind, 0, &d);
    checks++;
    if (d.blocks && !d.over) tolerated++;
    if (d.over) {
        bad++;
        hlog("verify v%u: GPU memory differs beyond the tolerance: %u blocks (first %04X, writer %u); colour words +-1 %llu, "
             "+-2 %llu, 3..16 %llu, > 16 %llu of %llu; Z %llu of %llu; CPU %llu", vblank, d.blocks, d.first_block,
             d.first_block >= 0 ? writer[d.first_block] : 0, (unsigned long long)d.c1, (unsigned long long)d.c2,
             (unsigned long long)d.c16, (unsigned long long)d.cbig, (unsigned long long)d.nc, (unsigned long long)d.z,
             (unsigned long long)d.nz, (unsigned long long)d.cpu);
        if (bad <= 8) {                  /* both pictures, and the stream from here for offline bisection */
            static gs_compose_hist *hs, *hh;
            static uint32_t sw[1024 * 1024], hw[1024 * 1024];
            if (!hs) { hs = gs_compose_hist_new(); hh = gs_compose_hist_new(); }
            const char* dir = C.out_dir && *C.out_dir ? C.out_dir : ".";
            char path[600];
            int w = 0, h = 0;
            if (hs && gs_compose_from(snap, gs_vram, hs, sw, 1024, &w, &h)) {
                snprintf(path, sizeof path, "%s/verify_%06u_sw.png", dir, vblank);
                debug_write_png(path, sw, w, h, 1024);
            }
            if (hh && gs_compose_from(snap, rb_mem, hh, hw, 1024, &w, &h)) {
                snprintf(path, sizeof path, "%s/verify_%06u_hw.png", dir, vblank);
                debug_write_png(path, hw, w, h, 1024);
            }
            snprintf(path, sizeof path, "%s/verify_%06u.gsrec", dir, vblank);
            gs_record_request(path, 60);
            hlog("verify v%u: pictures and the next 60 VBlanks' GS stream in %s/verify_%06u*", vblank, dir, vblank);
        }
        gpu_front_resync();              /* compare the next frames from equal memory */
    }
    if (checks % 60 == 0 || d.over)
        hlog("verify v%u: %u comparisons, %u beyond the tolerance, %u with tolerated differences", vblank, checks, bad, tolerated);
}

/* ---- EE thread: switches, device loss, PUNT fall-back --------------------------------------------- */
void gpu_hw_request_switch(void) { InterlockedExchange(&switch_req, 1); }

void gpu_hw_present_sw(const uint32_t* bgrx, int w, int h, int stride, int aspect_w, int aspect_h) {
    static uint32_t n;
    if (render_on) gpu_picture_aspect(bgrx, 1, w, h, stride, ++n, aspect_w, aspect_h);
}

static void to_sw(int lost) {
    if (!lost) gpu_hw_sync_memory();     /* the GPU's drawing becomes the software renderer's memory */
    gs_set_vblank_hook(NULL);
    gs_set_read_hook(NULL);
    gpu_front_stop();
    gs_draw_set_sw(1);
    InterlockedExchange(&active, 0);
}

static void to_hw(void) {
    front_start();                       /* gs_vram (the software renderer's drawing) is the frontend's memory */
    gpu_hw_attach_gs();
    InterlockedExchange(&active, 1);
}

void gpu_hw_ee_vblank(uint32_t vblank) {
    if (!render_on) return;
    for (int i = 0; i < nlose_at; i++)
        if (lose_at[i] == vblank) { hlog("simulating a device loss at VBlank %u (PS2_GPU_LOSE_AT)", vblank); gpu_simulate_device_lost(); }
    for (int i = 0; i < nswitch_at; i++)
        if (switch_at[i] == vblank) InterlockedExchange(&switch_req, 1);
    if (gpu_device_lost()) {
        gs_sync();                       /* (pictures still queued are reported by the render thread as it skips them) */
        losses++;
        int was = active != 0;
        if (was) to_sw(1);
        gpu_stop();
        render_on = 0;
        if (losses < 3 && render_start()) {
            render_on = 1;
            if (was) to_hw();
            hlog("device lost (%d of 3): device and swap chain recreated, GS memory reloaded from the CPU side", losses);
        } else {
            hlog("device lost %d times: the software renderer takes over", losses);
            if (C.window_external) C.window_external(0);
        }
        return;
    }
    if (switch_req) {
        InterlockedExchange(&switch_req, 0);
        gs_sync();
        if (active) { to_sw(0); hlog("switched to the software renderer (VBlank %u)", vblank); }
        else { to_hw(); hlog("switched to the GPU renderer (VBlank %u)", vblank); }
        return;
    }
    if (!active) return;
    /* PUNT on the frontend thread (5.4): it cannot make the GS thread draw the primitive; the frontend
     * moves to the GS thread, where PUNTs are exact, and back after 600 VBlanks without one */
    uint32_t tp = gpu_front_thread_punts();
    if (front_threaded && tp != seen_thread_punts) {
        gs_sync();
        gpu_front_set_thread(0);
        InterlockedExchange(&front_threaded, 0);
        seen_thread_punts = gpu_front_thread_punts();
        same_thread_until = vblank + 600;
        hlog("PUNT on the frontend thread (%u so far, not drawn): the frontend runs on the GS thread for now", tp);
    } else if (!front_threaded && !C.same_thread && vblank >= same_thread_until &&
               vblank - (uint32_t)last_punt_vblank >= 600) {
        gs_sync();
        gpu_front_set_thread(1);
        InterlockedExchange(&front_threaded, 1);
        seen_thread_punts = gpu_front_thread_punts();
        hlog("no PUNT for 600 VBlanks: the frontend is on its own thread again");
    }
}
