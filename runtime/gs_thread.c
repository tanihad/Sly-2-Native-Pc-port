/* Asynchronous GS: the GS register/image stream executes on its own host thread.
 *
 * On the PS2 the GS draws in parallel with the EE and the VUs; here the EE thread (which also runs
 * VIF and the VU microprograms) only queues what reaches the GS - register writes and IMAGE data
 * in GIF order - and this thread executes it with gs_exec_* (gs_draw.c), large primitives shared
 * with the pixel workers (gs_draw_pixel.c). Every GS-side effect the EE can observe stays in order:
 *  - SIGNAL / FINISH / LABEL only change CSR / SIGLBLID and raise INTC_GS. They are executed on the
 *    EE thread at the moment they are queued, exactly when the synchronous GS executed them.
 *  - Local->host transfers (gs_image_read) and CSR.RESET wait until the queue is empty (gs_sync).
 *  - The picture for a VBlank is composed by this thread when it reaches the VBlank's place in the
 *    stream (gs_queue_vsync, with a snapshot of the display registers), so it shows exactly what a
 *    synchronous GS would have shown at that VBlank. The EE thread waits at a VBlank while the
 *    previous one has not been presented yet: the GS never falls more than one frame behind.
 *
 * The queue is a single-producer/single-consumer ring of 16-byte slots: a register write is one
 * slot, IMAGE data is a header slot followed by the data (never wrapping: a SKIP slot pads to the
 * end of the ring first). Positions are free-running slot counters. The producer publishes in
 * batches (at primitive kicks and every PUBLISH_EVERY slots); both sides spin briefly before they
 * block in WaitOnAddress. */
#include "gs_draw.h"
#include "perf.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <windows.h>

#define Q_SLOTS        (1u << 18)       /* 4 MB of 16-byte slots */
#define Q_MASK         (Q_SLOTS - 1)
#define IMAGE_CHUNK    (16384u * 16u)   /* bytes per IMAGE record (a larger write is split) */
#define PUBLISH_EVERY  64
#define SPINS          4000             /* pause iterations before blocking */

enum { K_IMAGE = 0x100, K_SKIP, K_VSYNC, K_SYNC };
enum { R_XYZF2 = 0x04, R_XYZ2 = 0x05, R_SIGNAL = 0x60, R_FINISH = 0x61, R_LABEL = 0x62 };

typedef struct { uint32_t kind, len; uint64_t v; } slot;   /* kind: register number or K_* */

static slot* q;
static int async_on;
static uint32_t wr_local, unpublished;             /* producer */
static _Atomic uint32_t q_wr;                      /* published by the producer */
static _Atomic uint32_t q_rd;                      /* consumer: everything before this has executed */
static _Atomic int gs_sleeping, ee_waiting;
static _Atomic uint32_t vsync_queued, vsync_done;  /* VBlank pictures queued / presented */
static gs_present_fn present;
static HANDLE thread;

int gs_async(void) { return async_on; }
void* gs_thread_os_handle(void) { return async_on ? thread : NULL; }   /* for the profiler (perf.c) */

/* ---- producer (EE thread) ------------------------------------------------------------------------ */

static void publish(void) {
    unpublished = 0;
    atomic_store_explicit(&q_wr, wr_local, memory_order_seq_cst);
    if (atomic_load_explicit(&gs_sleeping, memory_order_seq_cst)) WakeByAddressSingle((PVOID)&q_wr);
}

/* Block until `*addr` != `val`, spinning first. The other side wakes us when `ee_waiting` is set. */
static void ee_wait_change(_Atomic uint32_t* addr, uint32_t val) {
    for (int i = 0; i < SPINS; i++) {
        if (atomic_load_explicit(addr, memory_order_acquire) != val) return;
        _mm_pause();
    }
    atomic_store_explicit(&ee_waiting, 1, memory_order_seq_cst);
    while (atomic_load_explicit(addr, memory_order_seq_cst) == val)
        WaitOnAddress((volatile VOID*)addr, &val, sizeof val, 10);
    atomic_store_explicit(&ee_waiting, 0, memory_order_relaxed);
}

/* Room for n contiguous slots at the write position (padding to the end of the ring if needed). */
static slot* reserve(uint32_t n) {
    uint32_t pos = wr_local & Q_MASK;
    uint32_t pad = pos + n > Q_SLOTS ? Q_SLOTS - pos : 0;
    for (;;) {
        uint32_t rd = atomic_load_explicit(&q_rd, memory_order_acquire);
        if (wr_local + pad + n - rd <= Q_SLOTS) break;
        publish();                                      /* full: let the GS thread catch up */
        int prev = perf_enter(PERF_GSWAIT);
        ee_wait_change(&q_rd, rd);
        perf_leave(prev);
    }
    if (pad) {
        q[pos].kind = K_SKIP;
        wr_local += pad;
    }
    return &q[wr_local & Q_MASK];
}

void gs_record_reg(uint32_t r, uint64_t v);       /* gs_record.c */
void gs_record_image(const void* data, uint32_t bytes);
void gs_record_vsync(uint32_t vblank);

void gs_write_reg(uint32_t r, uint64_t v) {
    gs_record_reg(r, v);
    if (!async_on) { gs_exec_write_reg(r, v); return; }
    if (r == R_SIGNAL || r == R_FINISH || r == R_LABEL) {   /* CSR events: on the EE's timeline */
        gs_exec_write_reg(r, v);
        return;
    }
    slot* s = reserve(1);
    s->kind = r;
    s->v = v;
    wr_local++;
    if (r == R_XYZ2 || r == R_XYZF2 || ++unpublished >= PUBLISH_EVERY) publish();
}

void gs_image_write(const void* data, uint32_t bytes) {
    gs_record_image(data, bytes);
    if (!async_on) { gs_exec_image_write(data, bytes); return; }
    const uint8_t* p = data;
    while (bytes) {
        uint32_t n = bytes < IMAGE_CHUNK ? bytes : IMAGE_CHUNK;
        uint32_t slots = 1 + (n + 15) / 16;
        slot* s = reserve(slots);
        s->kind = K_IMAGE;
        s->len = n;
        memcpy(s + 1, p, n);
        wr_local += slots;
        p += n;
        bytes -= n;
    }
    publish();
}

void gs_sync(void) {
    static uint32_t synced_at = ~0u;                    /* wr_local after the last completed sync */
    if (!async_on || wr_local == synced_at) return;     /* nothing sent since: still in sync */
    slot* s = reserve(1);                               /* the GS thread finishes deferred drawing too */
    s->kind = K_SYNC;
    wr_local++;
    publish();
    uint32_t rd = atomic_load_explicit(&q_rd, memory_order_acquire);
    int prev = perf_enter(PERF_GSWAIT);
    while (rd != wr_local) {
        ee_wait_change(&q_rd, rd);
        rd = atomic_load_explicit(&q_rd, memory_order_acquire);
    }
    perf_leave(prev);
    synced_at = wr_local;
}

uint32_t gs_image_read(void* data, uint32_t bytes) {
    gs_sync();
    return gs_exec_image_read(data, bytes);
}

void gs_queue_vsync(uint32_t vblank) {
    gs_record_vsync(vblank);                            /* PS2_GS_RECORD (gs_record.c) */
    /* at most one picture in flight: wait for the previous VBlank's to be presented */
    uint32_t done = atomic_load_explicit(&vsync_done, memory_order_acquire);
    if (vsync_queued != done) {
        publish();
        int prev = perf_enter(PERF_GSWAIT);
        while ((done = atomic_load_explicit(&vsync_done, memory_order_acquire)) != vsync_queued)
            ee_wait_change(&vsync_done, done);
        perf_leave(prev);
    }
    uint32_t slots = 1 + GS_PRIV_SNAPSHOT * 8 / 16;
    slot* s = reserve(slots);
    s->kind = K_VSYNC;
    s->len = vblank;
    gs_priv_snapshot((uint64_t*)(void*)(s + 1));
    wr_local += slots;
    vsync_queued++;
    publish();
}

/* ---- consumer (GS thread) ------------------------------------------------------------------------ */

static void compose(const slot* s) {
    static uint32_t frame[1024 * 1024];
    int w = 0, h = 0;
    int ok = gs_display_regs((const uint64_t*)(const void*)(s + 1), frame, 1024, &w, &h);
    if (present) present(frame, ok, w, h, s->len);
}

static DWORD WINAPI gs_thread_main(LPVOID arg) {
    (void)arg;
    uint32_t rd = 0;
    for (;;) {
        uint32_t wr = atomic_load_explicit(&q_wr, memory_order_acquire);
        if (wr == rd) {
            for (int i = 0; i < SPINS && (wr = atomic_load_explicit(&q_wr, memory_order_acquire)) == rd; i++) _mm_pause();
            if (wr == rd) {
                /* nothing more for a while: let the workers draw what has been recorded. Not at
                 * every catch-up: the EE publishes every primitive, and a stream of tiny batches
                 * costs more (each submit waits for the previous batch) than it gains. */
                gs_draw_kick();
                if ((wr = atomic_load_explicit(&q_wr, memory_order_acquire)) != rd) continue;
                atomic_store_explicit(&gs_sleeping, 1, memory_order_seq_cst);
                if ((wr = atomic_load_explicit(&q_wr, memory_order_seq_cst)) == rd)
                    WaitOnAddress((volatile VOID*)&q_wr, &rd, sizeof rd, INFINITE);
                atomic_store_explicit(&gs_sleeping, 0, memory_order_relaxed);
                continue;
            }
        }
        uint64_t t0 = __rdtsc();
        while (rd != wr) {
            const slot* s = &q[rd & Q_MASK];
            switch (s->kind) {
            case K_SKIP:
                rd += Q_SLOTS - (rd & Q_MASK);
                break;
            case K_IMAGE:
                gs_exec_image_write(s + 1, s->len);
                rd += 1 + (s->len + 15) / 16;
                break;
            case K_SYNC:
                gs_draw_sync_why(GS_SYNC_EE);
                rd++;
                break;
            case K_VSYNC:
                compose(s);
                rd += 1 + GS_PRIV_SNAPSHOT * 8 / 16;
                atomic_store_explicit(&q_rd, rd, memory_order_release);
                atomic_fetch_add_explicit(&vsync_done, 1, memory_order_seq_cst);
                if (atomic_load_explicit(&ee_waiting, memory_order_seq_cst)) WakeByAddressAll((PVOID)&vsync_done);
                break;
            default:
                gs_exec_write_reg(s->kind, s->v);
                rd++;
                break;
            }
            if ((rd & 1023) == 0) atomic_store_explicit(&q_rd, rd, memory_order_release);   /* free space */
        }
        perf_state.gs_busy += __rdtsc() - t0;                 /* only this thread writes it */
        atomic_store_explicit(&q_rd, rd, memory_order_seq_cst);
        if (atomic_load_explicit(&ee_waiting, memory_order_seq_cst)) {
            WakeByAddressAll((PVOID)&q_rd);
            WakeByAddressAll((PVOID)&vsync_done);
        }
    }
    return 0;
}

void gs_thread_start(int pixel_workers, gs_present_fn fn) {
    if (async_on) return;
    GS_INIT();                                     /* VRAM self-checks before anyone else draws */
    q = VirtualAlloc(NULL, Q_SLOTS * sizeof(slot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!q) return;
    present = fn;
    gs_draw_set_workers(pixel_workers);
    thread = CreateThread(NULL, 0, gs_thread_main, NULL, 0, NULL);
    if (!thread) { VirtualFree(q, 0, MEM_RELEASE); q = NULL; return; }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    async_on = 1;
}
