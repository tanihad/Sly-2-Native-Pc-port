/* (pc-fps2) The frame's VIF1 DMA chain on a worker thread (gfx_async.c, notes/frame-rate.md "High frame rate").
 *
 * On a PS2 the VIF1 DMA (VIF unpacking, VU1 microprograms, GIF -> GS) runs in parallel with the EE: the game kicks a
 * frame's chain, its render thread waits for the GS FINISH interrupt, and the main loop builds the next frame meanwhile
 * (the frame buffers are double-buffered). The port used to run the whole chain inside the CHCR write on the EE thread.
 * With the worker, a VIF1 source-chain transfer is handed to a host thread that runs exactly the same code (dma.c ->
 * vif.c -> vu.c -> gif.c -> gs_thread.c's producer side); the EE thread goes on with the game.
 *
 * Ownership: while gfx_owned is set the worker owns DMA channel 1, VIF1, VU1, the GIF and the GS queue's producer side.
 * The EE calls gfx_sync() before it touches any of them (register / memory accesses, GIF DMA, VBlank pictures): that
 * waits until the worker is done and applies what it deferred. Everything the EE can observe that the worker causes
 * (INTC / DMAC interrupts, GS SIGNAL / FINISH / LABEL) is deferred in order and applied on the EE when the job ends -
 * where the synchronous model raised them too (after the chain), so the GS stream and the game's view are the same.
 *
 * This header is self-contained (selectany state, function pointers): files linked into unit tests without
 * gfx_async.c see a worker that never exists. */
#pragma once
#include <stdint.h>

__attribute__((selectany)) volatile uint32_t gfx_worker_tid = 0;   /* the worker's OS thread id (0: no worker) */
__attribute__((selectany)) volatile int gfx_owned = 0;            /* EE: a job is out (set and cleared by the EE) */
__attribute__((selectany)) void (*gfx_wait_fn)(void) = 0;         /* EE: wait for the job, apply its results */
/* worker: an event the EE must see, deferred (kind GFX_EV_*) */
__attribute__((selectany)) void (*gfx_defer_fn)(int kind, uint32_t a, uint64_t b) = 0;
enum { GFX_EV_INTC = 1, GFX_EV_DMAC, GFX_EV_GSREG };
/* (pc-fps2, frame interpolation prototype interp.c) chains re-run by the runtime raise no GS / VIF1 / DMA 1-2 interrupts */
__attribute__((selectany)) volatile int gfx_quiet = 0;

static inline uint32_t gfx_tid(void) {                            /* TEB ClientId.UniqueThread (x64) */
    uint32_t v;
    __asm__ volatile("movl %%gs:0x48, %0" : "=r"(v));
    return v;
}
static inline int gfx_on_worker(void) { return gfx_worker_tid && gfx_tid() == gfx_worker_tid; }
static inline void gfx_sync(void) { if (gfx_owned) gfx_wait_fn(); }   /* EE only */

/* dma.c pump, EE: 1 = channel ch belongs to the worker now (or still); NULL without a worker */
__attribute__((selectany)) int (*gfx_take_fn)(int ch) = 0;
/* dma.c, worker: run channel ch as pump() would, without the other channels and the IOP */
void dma_run_owned(int ch);

/* gfx_async.c (game build) */
int  gfx_async_init(void);             /* PS2_GFX_ASYNC (default on): starts the worker; 1 = on */
void gfx_poll(void);                   /* EE safe points: a finished job's results are applied */
