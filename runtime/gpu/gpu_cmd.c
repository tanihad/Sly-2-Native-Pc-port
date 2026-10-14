/* Command chunks and data arena to the render thread (see gpu_cmd.h). */
#include "gpu_cmd.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define SPINS 4000

gpu_cmd_stats gpu_cmd_counters;

static uint8_t *cmd_buf, *data_buf;

/* producer side (one producer at a time) */
static uint64_t cmd_wr;                  /* committed commands end here */
static uint64_t cmd_cur_size;            /* size of the command being written (0: none) */
static uint64_t unpublished;
static uint64_t data_wr;                 /* allocated data ends here */
/* consumer side */
static uint64_t cons_rd;
/* shared */
static _Atomic uint64_t cmd_pub;         /* published by the producer */
static _Atomic uint64_t cmd_rd;          /* consumer: commands before this are free */
static _Atomic uint64_t data_rd;         /* consumer: data before this is free */
static _Atomic int cons_sleeping, prod_waiting;

int gpu_cmd_init(void) {
    if (cmd_buf) return 1;
    cmd_buf = VirtualAlloc(NULL, GPU_CMD_RING, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    data_buf = VirtualAlloc(NULL, GPU_DATA_RING, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!cmd_buf || !data_buf) { gpu_cmd_free(); return 0; }
    gpu_cmd_reset();
    return 1;
}

void gpu_cmd_free(void) {
    if (cmd_buf) VirtualFree(cmd_buf, 0, MEM_RELEASE);
    if (data_buf) VirtualFree(data_buf, 0, MEM_RELEASE);
    cmd_buf = data_buf = NULL;
}

void gpu_cmd_reset(void) {
    cmd_wr = cmd_cur_size = unpublished = data_wr = cons_rd = 0;
    atomic_store(&cmd_pub, 0);
    atomic_store(&cmd_rd, 0);
    atomic_store(&data_rd, 0);
    atomic_store(&cons_sleeping, 0);
    atomic_store(&prod_waiting, 0);
    memset(&gpu_cmd_counters, 0, sizeof gpu_cmd_counters);
}

/* ---- producer ---- */

void gpu_cmd_publish(void) {
    unpublished = 0;
    if (atomic_load_explicit(&cmd_pub, memory_order_relaxed) == cmd_wr) return;
    gpu_cmd_counters.publishes++;
    atomic_store_explicit(&cmd_pub, cmd_wr, memory_order_seq_cst);
    if (atomic_load_explicit(&cons_sleeping, memory_order_seq_cst)) WakeByAddressSingle((PVOID)&cmd_pub);
}

static void prod_wait_change(_Atomic uint64_t* a, uint64_t old) {
    for (int i = 0; i < SPINS; i++) {
        if (atomic_load_explicit(a, memory_order_acquire) != old) return;
        _mm_pause();
    }
    atomic_store_explicit(&prod_waiting, 1, memory_order_seq_cst);
    while (atomic_load_explicit(a, memory_order_seq_cst) == old) WaitOnAddress((volatile VOID*)a, &old, sizeof old, 10);
    atomic_store_explicit(&prod_waiting, 0, memory_order_relaxed);
}

void* gpu_data_alloc(size_t bytes) {
    uint64_t n = ((uint64_t)bytes + 15) & ~15ull;
    if (n > GPU_DATA_RING) return NULL;
    uint64_t pos = data_wr % GPU_DATA_RING;
    uint64_t pad = pos + n > GPU_DATA_RING ? GPU_DATA_RING - pos : 0;
    for (;;) {
        uint64_t rd = atomic_load_explicit(&data_rd, memory_order_acquire);
        if (data_wr + pad + n - rd <= GPU_DATA_RING) break;
        gpu_cmd_publish();                      /* the consumer must see what frees the space */
        gpu_cmd_counters.data_waits++;
        prod_wait_change(&data_rd, rd);
    }
    data_wr += pad;
    void* p = data_buf + data_wr % GPU_DATA_RING;
    data_wr += n;
    return p;
}

void* gpu_cmd_alloc(uint32_t op, uint32_t payload_bytes) {
    uint64_t size = (sizeof(gpu_cmd) + (uint64_t)payload_bytes + 15) & ~15ull;
    uint64_t pos = cmd_wr % GPU_CMD_RING;
    uint64_t pad = pos + size > GPU_CMD_RING ? GPU_CMD_RING - pos : 0;
    for (;;) {
        uint64_t rd = atomic_load_explicit(&cmd_rd, memory_order_acquire);
        if (cmd_wr + pad + size - rd <= GPU_CMD_RING) break;
        gpu_cmd_publish();
        gpu_cmd_counters.cmd_waits++;
        prod_wait_change(&cmd_rd, rd);
    }
    if (pad) {                                  /* SKIP to the end of the ring, part of the stream */
        gpu_cmd* s = (gpu_cmd*)(cmd_buf + pos);
        s->op = GPU_OP_SKIP;
        s->size = (uint32_t)pad;
        s->data_end = 0;
        cmd_wr += pad;
        unpublished += pad;
    }
    gpu_cmd* c = (gpu_cmd*)(cmd_buf + cmd_wr % GPU_CMD_RING);
    c->op = op;
    c->size = (uint32_t)size;
    c->data_end = data_wr;                      /* every data allocated so far belongs to <= this command */
    cmd_cur_size = size;
    return c + 1;
}

void gpu_cmd_commit(void) {
    cmd_wr += cmd_cur_size;
    unpublished += cmd_cur_size;
    cmd_cur_size = 0;
    gpu_cmd_counters.commands++;
    if (unpublished >= GPU_CMD_CHUNK) gpu_cmd_publish();
}

uint64_t gpu_cmd_written(void) { return cmd_wr; }

void gpu_cmd_debug(char* buf, size_t n) {
    snprintf(buf, n, "cmd wr %llu pub %llu rd %llu cons %llu | data wr %llu rd %llu | sleeping %d waiting %d",
             (unsigned long long)cmd_wr, (unsigned long long)atomic_load(&cmd_pub), (unsigned long long)atomic_load(&cmd_rd),
             (unsigned long long)cons_rd, (unsigned long long)data_wr, (unsigned long long)atomic_load(&data_rd),
             atomic_load(&cons_sleeping), atomic_load(&prod_waiting));
}

/* ---- consumer ---- */

const gpu_cmd* gpu_cmd_next(int wait_ms) {
    for (;;) {
        uint64_t pub = atomic_load_explicit(&cmd_pub, memory_order_acquire);
        if (pub == cons_rd) {
            for (int i = 0; i < SPINS && (pub = atomic_load_explicit(&cmd_pub, memory_order_acquire)) == cons_rd; i++)
                _mm_pause();
            if (pub == cons_rd) {
                if (wait_ms == 0) return NULL;
                atomic_store_explicit(&cons_sleeping, 1, memory_order_seq_cst);
                if ((pub = atomic_load_explicit(&cmd_pub, memory_order_seq_cst)) == cons_rd) {
                    uint64_t seen = cons_rd;
                    WaitOnAddress((volatile VOID*)&cmd_pub, &seen, sizeof seen, wait_ms < 0 ? INFINITE : (DWORD)wait_ms);
                }
                atomic_store_explicit(&cons_sleeping, 0, memory_order_relaxed);
                pub = atomic_load_explicit(&cmd_pub, memory_order_acquire);
                if (pub == cons_rd) {
                    if (wait_ms < 0) continue;
                    return NULL;
                }
            }
        }
        const gpu_cmd* c = (const gpu_cmd*)(cmd_buf + cons_rd % GPU_CMD_RING);
        if (c->op == GPU_OP_SKIP) {
            cons_rd += c->size;
            atomic_store_explicit(&cmd_rd, cons_rd, memory_order_release);
            continue;
        }
        return c;
    }
}

void gpu_cmd_done(const gpu_cmd* c) {
    uint64_t data_end = c->data_end;
    cons_rd += c->size;
    if (data_end > atomic_load_explicit(&data_rd, memory_order_relaxed))
        atomic_store_explicit(&data_rd, data_end, memory_order_release);
    atomic_store_explicit(&cmd_rd, cons_rd, memory_order_seq_cst);
    if (atomic_load_explicit(&prod_waiting, memory_order_seq_cst)) {
        WakeByAddressAll((PVOID)&cmd_rd);
        WakeByAddressAll((PVOID)&data_rd);
    }
}
