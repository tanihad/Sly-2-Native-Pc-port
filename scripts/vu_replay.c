/* Replays a VU microprogram recording (PS2_VU_RECORD, runtime/vu.c) with the interpreter of a
 * checkout (scripts/vu_replay.ps1 builds it against any runtime/ directory):
 *  - a benchmark of real game microcode, without the EE, VIF or GS around it;
 *  - an exactness check: pair and cycle counts, the resulting registers/flags/data memory and the
 *    XGKICKs (address and issue cycle) must match what the recording run produced.
 *   vu_replay <file> [passes]
 * Prints the time of the fastest pass and the number of programs whose results differ. */
#include "vu_internal.h"
#include "vu_record.h"                 /* this checkout's, if -Runtime is an older one */

#include <stdlib.h>
#include <windows.h>
#include <xmmintrin.h>

static uint64_t khash;
static uint32_t kicks;
void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) {
    (void)vu1_data;
    khash = vu_kick_hash(khash, qw_addr, vu_st[1].cyc);
    kicks++;
}
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}
uint32_t ps2_vblank_count(void) { return 0; }

/* VU_REPLAY_PROFILE=1: sample the replaying thread's instruction pointer every ~50 us and print the
 * hottest addresses as image-relative offsets (match them with llvm-objdump -d of the exe). */
#define PROF_SLOTS (1u << 20)
static uint32_t* prof_hits;
static volatile LONG prof_stop;
static DWORD WINAPI prof_thread(LPVOID arg) {
    HANDLE th = (HANDLE)arg;
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    while (!prof_stop) {
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &c)) {
                uintptr_t off = (uintptr_t)c.Rip - base;
                prof_hits[off < PROF_SLOTS ? off : PROF_SLOTS - 1]++;
            }
            ResumeThread(th);
        }
        for (volatile int spin = 0; spin < 20000; spin++) {}
    }
    return 0;
}
static int prof_cmp(const void* a, const void* b) {
    uint32_t x = prof_hits[*(const uint32_t*)a], y = prof_hits[*(const uint32_t*)b];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void prof_report(void) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < PROF_SLOTS; i++) total += prof_hits[i];
    uint32_t* idx = malloc(PROF_SLOTS * sizeof *idx);
    for (uint32_t i = 0; i < PROF_SLOTS; i++) idx[i] = i;
    qsort(idx, PROF_SLOTS, sizeof *idx, prof_cmp);
    printf("profile: %llu samples; image base 0x140000000 + offset:\n", (unsigned long long)total);
    for (int i = 0; i < 600 && prof_hits[idx[i]]; i++)
        printf("  %6.2f%%  0x%llx\n", 100.0 * prof_hits[idx[i]] / (double)total, 0x140000000ull + idx[i]);
}

typedef struct {
    const vu_rec_head* h;
    const uint8_t *micro, *data;
    const vu_rec_tail* t;
} rec;

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: vu_replay <recording> [passes]\n"); return 2; }
    int passes = argc > 2 ? atoi(argv[2]) : 5;
    if (passes < 1) passes = 1;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "read failed\n"); return 2; }
    fclose(f);

    size_t cap = 4096, n = 0;
    rec* recs = malloc(cap * sizeof *recs);
    const uint8_t* last_micro[2] = { NULL, NULL };
    for (long pos = 0; pos + (long)sizeof(vu_rec_head) <= size;) {
        rec r;
        r.h = (const vu_rec_head*)(buf + pos);
        if (memcmp(r.h->magic, "VUP1", 4) || r.h->idx > 1) { fprintf(stderr, "bad record at %ld\n", pos); return 2; }
        pos += sizeof(vu_rec_head);
        if (r.h->micro_size) { last_micro[r.h->idx] = buf + pos; pos += r.h->micro_size; }
        r.micro = last_micro[r.h->idx];
        r.data = buf + pos;
        pos += r.h->data_size;
        r.t = (const vu_rec_tail*)(buf + pos);
        pos += sizeof(vu_rec_tail);
        if (pos > size || !r.micro) break;                    /* truncated recording */
        if (n == cap) recs = realloc(recs, (cap *= 2) * sizeof *recs);
        recs[n++] = r;
    }
    if (!n) { fprintf(stderr, "no complete records\n"); return 2; }

    vu_set_vif_tops(0, 0, 0);                                 /* initialises both units */
    vu_set_vif_tops(1, 0, 0);
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    double best = 1e30;
    uint64_t pairs = 0, cycles = 0;
    uint32_t bad = 0, bad_timing = 0, vb0 = recs[0].h->vblank, vb1 = recs[n - 1].h->vblank;
    const unsigned host_csr = _mm_getcsr();
    HANDLE prof = NULL;
    if (getenv("VU_REPLAY_PROFILE")) {
        HANDLE self;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        prof_hits = calloc(PROF_SLOTS, sizeof *prof_hits);
        prof = CreateThread(NULL, 0, prof_thread, self, 0, NULL);
    }
    for (int pass = 0; pass < passes; pass++) {
        const uint8_t* loaded[2] = { NULL, NULL };
        int64_t ticks = 0;
        for (size_t i = 0; i < n; i++) {
            const rec* r = &recs[i];
            const int u = (int)r->h->idx;
            vu_state* st = &vu_st[u];
            if (loaded[u] != r->micro) { memcpy(vu_micro_mem(u), r->micro, (st->pcmask + 1) * 8u); loaded[u] = r->micro; }
            memcpy(vu_data_mem(u), r->data, r->h->data_size);
            memcpy(st->vf, r->h->vf, sizeof st->vf);
            st->acc = r->h->acc;
            memcpy(st->vi, r->h->vi, sizeof st->vi);
            st->I = r->h->I; st->Q = r->h->Q; st->P = r->h->P; st->R = r->h->R;
            st->status = r->h->status; st->mac = r->h->mac; st->clip = r->h->clip;
            st->top = r->h->top; st->itop = r->h->itop;
            khash = VU_HASH_SEED;
            kicks = 0;
            const uint64_t c0 = st->cycles;
            LARGE_INTEGER t0, t1;
            _mm_setcsr(VU_MXCSR);
            QueryPerformanceCounter(&t0);
            uint32_t np = vu_run(st, r->h->addr);
            QueryPerformanceCounter(&t1);
            _mm_setcsr(host_csr);
            ticks += t1.QuadPart - t0.QuadPart;
            if (pass) continue;
            const uint32_t nc = (uint32_t)(st->cycles - c0);
            pairs += np;
            cycles += nc;
            const int timing = np != r->t->pairs || nc != r->t->cycles;
            const int result = vu_state_hash(st) != r->t->hash || khash != r->t->khash || kicks != r->t->kicks;
            if (timing) bad_timing++;
            if (timing || result) {
                if (bad++ < 10)
                    fprintf(stderr, "program %zu (VU%d at 0x%04X, VBlank %u): pairs %u/%u cycles %u/%u kicks %u/%u%s\n",
                            i, u, r->h->addr * 8u, r->h->vblank, np, r->t->pairs, nc, r->t->cycles, kicks,
                            r->t->kicks, result ? " results differ" : "");
            }
        }
        double ms = (double)ticks * 1e3 / (double)fq.QuadPart;
        if (ms < best) best = ms;
    }
    if (prof) {
        prof_stop = 1;
        WaitForSingleObject(prof, INFINITE);
        prof_report();
    }
    printf("%zu programs (VBlanks %u-%u), %llu pairs, %llu cycles: %.3f ms (fastest of %d passes) = %.2f ns/pair; "
           "%u programs differ (%u in pair/cycle counts)\n",
           n, vb0, vb1, (unsigned long long)pairs, (unsigned long long)cycles, best, passes,
           best * 1e6 / (double)pairs, bad, bad_timing);
    return bad ? 1 : 0;
}
