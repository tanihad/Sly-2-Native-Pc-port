/* Replays a VU microprogram recording (PS2_VU_RECORD, runtime/vu.c) with the interpreter of a
 * checkout (scripts/vu_replay.ps1 builds it against any runtime/ directory):
 *  - a benchmark of real game microcode, without the EE, VIF or GS around it;
 *  - an exactness check: pair and cycle counts, the resulting registers/flags/data memory and the
 *    XGKICKs (address and issue cycle) must match what the recording run produced.
 *   vu_replay <file> [passes] [interp|compiled]
 * Prints the time of the fastest pass and the number of programs whose results differ. "compiled"
 * starts programs through vu_execute(), i.e. the recompiled microcode linked in (vu_replay.ps1
 * -Compiled <dir>), falling back to the interpreter where the runtime would. */
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
#define PROF_SLOTS (1u << 24)                 /* image offsets up to 16 MB (recompiled microcode is large) */
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
    for (int i = 0; i < 4000 && prof_hits[idx[i]]; i++)
        printf("  %6.2f%%  0x%llx\n", 100.0 * prof_hits[idx[i]] / (double)total, 0x140000000ull + idx[i]);
}

typedef struct {
    const vu_rec_head* h;
    const uint8_t *micro, *data;
    const vu_rec_tail* t;
} rec;

static void load_state(vu_state* st, const rec* r) {
    memcpy(vu_data_mem((int)r->h->idx), r->data, r->h->data_size);
    memcpy(st->vf, r->h->vf, sizeof st->vf);
    st->acc = r->h->acc;
    memcpy(st->vi, r->h->vi, sizeof st->vi);
    st->I = r->h->I; st->Q = r->h->Q; st->P = r->h->P; st->R = r->h->R;
    st->status = r->h->status; st->mac = r->h->mac; st->clip = r->h->clip;
    st->top = r->h->top; st->itop = r->h->itop;
}

/* Compiled mode: what exactly differs from the interpreter on this program (registers, flags, data). */
static void diff_with_interpreter(vu_state* st, const rec* r) {
    static vu_state got;
    static uint8_t got_data[16384];
    got = *st;
    memcpy(got_data, st->data, r->h->data_size);
    load_state(st, r);
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(VU_MXCSR);
    vu_run(st, r->h->addr);
    _mm_setcsr(csr);
    for (int k = 0; k < 32; k++)
        if (memcmp(&got.vf[k], &st->vf[k], 16))
            fprintf(stderr, "    vf%02d compiled %08X %08X %08X %08X interpreter %08X %08X %08X %08X\n", k, got.vf[k].u[0],
                    got.vf[k].u[1], got.vf[k].u[2], got.vf[k].u[3], st->vf[k].u[0], st->vf[k].u[1], st->vf[k].u[2], st->vf[k].u[3]);
    if (memcmp(&got.acc, &st->acc, 16)) fprintf(stderr, "    acc differs\n");
    for (int k = 0; k < 16; k++)
        if (got.vi[k] != st->vi[k]) fprintf(stderr, "    vi%02d compiled %04X interpreter %04X\n", k, got.vi[k], st->vi[k]);
    const char* nm[8] = { "I", "Q", "P", "R", "status", "mac", "clip", "tpc" };
    const uint32_t a[8] = { got.I, got.Q, got.P, got.R, got.status, got.mac, got.clip, got.tpc };
    const uint32_t b[8] = { st->I, st->Q, st->P, st->R, st->status, st->mac, st->clip, st->tpc };
    for (int k = 0; k < 8; k++) if (a[k] != b[k]) fprintf(stderr, "    %s compiled %08X interpreter %08X\n", nm[k], a[k], b[k]);
    int shown = 0;
    for (uint32_t q = 0; q < r->h->data_size / 16; q++)
        if (memcmp(got_data + q * 16, st->data + q * 16, 16) && shown++ < 8)
            fprintf(stderr, "    data qw 0x%03X differs\n", q);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: vu_replay <recording> [passes]\n"); return 2; }
    int passes = argc > 2 ? atoi(argv[2]) : 5;
    if (passes < 1) passes = 1;
    const int compiled = argc > 3 && !strcmp(argv[3], "compiled");
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

    /* (pc-research) VU_REPLAY_LIVEIN=1: does a program's output depend on the registers it inherits from earlier programs
     * (VF1-31, ACC, VI1-15, I/Q/P), or only on VU memory? Each program runs twice - as recorded and with those registers
     * scrambled - and the data memory (where its GIF packets are built) and the XGKICK addresses are compared. Programs
     * whose output does not depend on inherited registers can run on any thread given their memory. */
    if (getenv("VU_REPLAY_LIVEIN")) {
        const unsigned csr0 = _mm_getcsr();
        uint32_t dep = 0, dep_kick = 0;
        uint64_t pairs_dep = 0, pairs_all = 0;
        static uint8_t mem_a[16384];
        const uint8_t* loaded[2] = { NULL, NULL };
        const int compiled_l = argc > 3 && !strcmp(argv[3], "compiled");
        for (size_t i = 0; i < n; i++) {
            const rec* r = &recs[i];
            const int u = (int)r->h->idx;
            vu_state* st = &vu_st[u];
            if (loaded[u] != r->micro) { memcpy(vu_micro_mem(u), r->micro, (st->pcmask + 1) * 8u); loaded[u] = r->micro; }
            uint64_t kh[2];
            uint32_t np = 0, np0 = 0;
            for (int pass = 0; pass < 2; pass++) {
                load_state(st, r);
                if (pass) {
                    uint32_t s = 0x9E3779B9u ^ (uint32_t)i;
                    for (int k = 1; k < 32; k++)
                        for (int c = 0; c < 4; c++) { s = s * 1664525u + 1013904223u; st->vf[k].u[c] = (s >> 1) & 0x3FFFFFFFu; }
                    for (int k = 1; k < 16; k++) { s = s * 1664525u + 1013904223u; st->vi[k] = (uint16_t)(s >> 8); }
                    memcpy(&st->acc, &st->vf[7], sizeof st->acc);
                    s = s * 1664525u + 1013904223u;
                    st->I = s >> 2; st->Q = s >> 3; st->P = s >> 4;
                }
                khash = VU_HASH_SEED;
                kicks = 0;
                _mm_setcsr(VU_MXCSR);
                np = compiled_l ? vu_execute(st, r->h->addr) : vu_run(st, r->h->addr);
                _mm_setcsr(csr0);
                kh[pass] = khash;
                if (!pass) { memcpy(mem_a, st->data, r->h->data_size); np0 = np; }
                else {
                    int md = memcmp(mem_a, st->data, r->h->data_size) != 0, kd = kh[0] != kh[1];
                    if (md || kd) { dep++; pairs_dep += np0; }
                    if (kd) dep_kick++;
                }
            }
            pairs_all += np0;
        }
        printf("live-in test: %zu programs, %u (%.1f%%) depend on inherited registers (%u in their XGKICK addresses/cycles); "
               "%.1f%% of pairs\n", n, dep, 100.0 * dep / (double)n, dep_kick,
               100.0 * (double)pairs_dep / (double)(pairs_all ? pairs_all : 1));
        return 0;
    }
    /* VU_REPLAY_CPU=<n>: run on that logical processor at high priority (benchmarks on a hybrid CPU:
     * pick a P-core; 0-15 on the i7-14700F) */
    const char* cpu = getenv("VU_REPLAY_CPU");
    if (cpu && *cpu) {
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << atoi(cpu));
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
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
            load_state(st, r);
            khash = VU_HASH_SEED;
            kicks = 0;
            const uint64_t c0 = st->cycles;
            LARGE_INTEGER t0, t1;
            _mm_setcsr(VU_MXCSR);
            QueryPerformanceCounter(&t0);
            uint32_t np = compiled ? vu_execute(st, r->h->addr) : vu_run(st, r->h->addr);
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
                if (compiled && bad <= 3) diff_with_interpreter(st, r);
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
    const uint64_t cp = vu_st[0].cprograms + vu_st[1].cprograms, ap = vu_st[0].programs + vu_st[1].programs;
    printf("%zu programs (VBlanks %u-%u), %llu pairs, %llu cycles: %.3f ms (fastest of %d passes) = %.2f ns/pair; "
           "%u programs differ (%u in pair/cycle counts)%s",
           n, vb0, vb1, (unsigned long long)pairs, (unsigned long long)cycles, best, passes,
           best * 1e6 / (double)pairs, bad, bad_timing, compiled ? "; " : "\n");
    if (compiled) printf("%.1f%% of programs started in recompiled code\n", ap ? 100.0 * (double)cp / (double)ap : 0.0);
    return bad ? 1 : 0;
}
