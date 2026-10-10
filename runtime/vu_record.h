/* VU microprogram recordings: written by runtime/vu.c (PS2_VU_RECORD), replayed by scripts/vu_replay.c. */
#pragma once
#include "vu_internal.h"

/* ---- microprogram recordings (PS2_VU_RECORD, vu.c; replayed by scripts/vu_replay.c) ----------------
 * Per program: vu_rec_head, micro memory (micro_size bytes; 0 = unchanged since this unit's previous
 * record), data memory (data_size bytes), then vu_rec_tail with what the run produced. */
typedef struct {
    char     magic[4];                   /* "VUP1" */
    uint32_t idx, addr, vblank, micro_size, data_size;
    vf128    vf[32], acc;
    uint16_t vi[16];
    uint32_t I, Q, P, R, status, mac, clip, top, itop;
} vu_rec_head;
typedef struct {
    uint32_t pairs, cycles, kicks, pad;
    uint64_t hash;                       /* vu_state_hash after the run */
    uint64_t khash;                      /* XGKICKs: vu_kick_hash over (address, issue cycle) */
} vu_rec_tail;

#define VU_HASH_SEED 0xCBF29CE484222325ull
static inline uint64_t vu_hash_words(uint64_t h, const void* p, size_t bytes) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i + 8 <= bytes; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}
static inline uint64_t vu_kick_hash(uint64_t h, uint32_t qw_addr, uint32_t cyc) {
    return vu_hash_words(h, &(uint64_t){ (uint64_t)(qw_addr & 0x3FF) << 32 | cyc }, 8);
}
/* Architectural state after a program: registers, flags, TPC and data memory. */
static inline uint64_t vu_state_hash(const vu_state* st) {
    uint32_t misc[8] = { st->I, st->Q, st->P, st->R, st->status, st->mac, st->clip, st->tpc };
    uint64_t h = vu_hash_words(VU_HASH_SEED, st->vf, sizeof st->vf);
    h = vu_hash_words(h, &st->acc, sizeof st->acc);
    h = vu_hash_words(h, st->vi, sizeof st->vi);
    h = vu_hash_words(h, misc, sizeof misc);
    return vu_hash_words(h, st->data, (st->dmask + 1) * 16u);
}
