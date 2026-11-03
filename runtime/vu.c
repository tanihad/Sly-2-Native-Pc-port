/* VU0/VU1: memory as seen by the EE, micro mode entry points (VIF MSCAL/CMSAR1 for VU1, VCALLMS for
 * VU0), and the VU0 control registers reached through CFC2/CTC2 16-31.
 *
 * The interpreter itself is in vu_exec.c; vu_internal.h describes the timing model. Everything is
 * synchronous: a microprogram runs to its E bit inside the call that starts it, so the VUs are
 * never busy as far as VIF, VPU-STAT or the EE can tell.
 */
#include "vu_record.h"
#include "vu_compiled.h"
#include "perf.h"
#include "gfx_async.h"

#include <stdlib.h>
#include <xmmintrin.h>
#include <windows.h>                    /* (r2-flicker: QueryPerformanceCounter, A/B timers) */

static _Alignas(16) uint8_t vu0_data[4096], vu0_micro[4096], vu1_data[16384], vu1_micro[16384];
static vu_op vu0_cache[512], vu1_cache[2048];
vu_state vu_st[2];

static uint32_t cmsar0, cmsar1, fbrst;
static unsigned host_csr[2];             /* MXCSR of the caller while a microprogram runs (per unit: (pc-fps2) VU1 can run
                                            on the graphics worker while the EE runs VU0) */
static uint32_t micro_gen[2] = { 1, 1 }; /* bumped on every write to a unit's micro memory */

static vu_state* get(int vu) {
    vu_state* st = &vu_st[vu];
    if (__builtin_expect(!st->inited, 0)) {
        st->idx = vu;
        st->data = vu ? vu1_data : vu0_data;
        st->dmask = vu ? 0x3FF : 0xFF;
        st->micro = vu ? vu1_micro : vu0_micro;
        st->pcmask = vu ? 0x7FF : 0x1FF;
        st->cache = vu ? vu1_cache : vu0_cache;
        vu_init_state(st);
    }
    return st;
}

uint8_t* vu_data_mem(int vu) { return vu ? vu1_data : vu0_data; }
/* Callers (VIF MPG, the replay tool, tests) write micro memory through this pointer: the micro
 * memory generation changes, so the next program start checks which recompiled image it holds. The
 * interpreter's decoded-instruction cache needs no notification (it compares every pair). */
uint8_t* vu_micro_mem(int vu) {
    micro_gen[vu ? 1 : 0]++;
    return vu ? vu1_micro : vu0_micro;
}

/* EE view: 0x11000000 VU0 micro, 0x11004000 VU0 data, 0x11008000 VU1 micro, 0x1100C000 VU1 data
 * (VU0's 4 KB areas mirror through their 16 KB windows). */
void vu_mem_access(uint32_t phys, void* data, unsigned size, int is_write) {
    uint32_t off = phys - 0x11000000u;
    uint8_t* mem;
    uint32_t len;
    if (off >= 0x8000) gfx_sync();       /* (pc-fps2) VU1 belongs to the graphics worker while it runs */
    if (off < 0x4000)       { mem = vu0_micro; len = sizeof vu0_micro; if (is_write) micro_gen[0]++; }
    else if (off < 0x8000)  { mem = vu0_data;  len = sizeof vu0_data;  off -= 0x4000; }
    else if (off < 0xC000)  { mem = vu1_micro; len = sizeof vu1_micro; off -= 0x8000; if (is_write) micro_gen[1]++; }
    else                    { mem = vu1_data;  len = sizeof vu1_data;  off -= 0xC000; }
    off &= len - 1;
    if (off + size > len) { if (!is_write) memset(data, 0, size); return; }
    if (is_write) memcpy(mem + off, data, size); else memcpy(data, mem + off, size);
}

/* ---- recompiled microprograms ----------------------------------------------------------------------
 * recomp/vu_recomp.py compiles the game's microcode images (vu_compiled.h); its generated table
 * registers them at startup. A program start at (unit, pc) uses a unit whose entry list has pc and
 * whose whole image matches micro memory; the answer is cached per (unit, pc) until micro memory is
 * written again. Everything else runs in the interpreter (logged once per start address).
 *   PS2_VU_INTERP=1  interpreter only (A/B comparisons)
 *   PS2_VU_VERIFY=1  run every compiled program and the interpreter on a copy of the state, compare
 *                    pairs, cycles, the resulting state and the XGKICKs; print mismatches */
#define VUC_MAX_PROGS 1024
static const vuc_program* const* vuc_progs;
static uint32_t vuc_nprogs;
static int vuc_mode;                     /* 0 not decided, 1 compiled, 2 interpreter only, 3 verify */
typedef struct { uint32_t gen; uint16_t prog, idx; } vuc_slot;   /* prog = index + 1, 0 = none */
static vuc_slot vuc_slots[2][2048];
static uint32_t vuc_checked[VUC_MAX_PROGS];
static uint8_t  vuc_match[VUC_MAX_PROGS];

void vu_register_compiled(const vuc_program* const* progs, uint32_t n) {
    vuc_progs = progs;
    vuc_nprogs = n < VUC_MAX_PROGS ? n : VUC_MAX_PROGS;
}

static int vuc_image_matches(const vu_state* st, const vuc_program* p) {
    const uint32_t* code = p->code;
    for (uint32_t r = 0; r < p->nranges; r++) {
        const vuc_range* g = &p->ranges[r];
        if ((uint32_t)g->start + g->len > st->pcmask + 1u) return 0;
        if (memcmp(st->micro + g->start * 8u, code, g->len * 8u)) return 0;
        code += g->len * 2u;
    }
    return 1;
}

static void vuc_lookup(const vu_state* st, uint32_t pc, vuc_slot* s) {
    const uint32_t u = (uint32_t)st->idx;
    s->gen = micro_gen[u];
    s->prog = 0;
    for (uint32_t k = 0; k < vuc_nprogs; k++) {
        const vuc_program* p = vuc_progs[k];
        if (p->unit != u) continue;
        for (uint32_t i = 0; i < p->nentries; i++) {
            if (p->entries[i] != pc) continue;
            if (vuc_checked[k] != s->gen) { vuc_checked[k] = s->gen; vuc_match[k] = (uint8_t)vuc_image_matches(st, p); }
            if (vuc_match[k]) { s->prog = (uint16_t)(k + 1); s->idx = (uint16_t)i; return; }
            break;
        }
    }
    static uint8_t logged[2][2048];
    if (!logged[u][pc]) {
        logged[u][pc] = 1;
        uint64_t h = vu_hash_words(VU_HASH_SEED, st->micro, (st->pcmask + 1u) * 8u);
        fprintf(stderr, "[vu] VU%u program at 0x%04X: no recompiled code for this micro memory (hash %016llX); interpreted\n",
                u, pc * 8u, (unsigned long long)h);
    }
}

void vuc_unknown_jump(const vu_state* st, uint32_t site, uint32_t target) {
    static uint32_t seen[64], nseen;
    const uint32_t key = (uint32_t)st->idx << 31 | site << 12 | target;
    for (uint32_t i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 64) seen[nseen++] = key;
    else return;
    fprintf(stderr, "[vu] VU%d: jump from 0x%04X to 0x%04X is not in the recompiled code; continuing in the interpreter\n",
            st->idx, site * 8u, target * 8u);
}

/* PS2_VU_VERIFY: the interpreter runs the same program on a snapshot; its XGKICKs only feed a hash.
 * PS2_VU_VERIFY_DUMP=<file>: mismatching programs are written as a recording (vu_record.h, with
 * the interpreter's results), for scripts/vu_replay.ps1 -Compiled. */
static vu_state vuc_shadow;
static _Alignas(16) uint8_t vuc_shadow_data[16384], vuc_before_data[16384];
static int vuc_shadow_active, vuc_hashing;
static uint64_t vuc_khash;
static uint32_t vuc_kicks, vuc_verified, vuc_mismatches;
static FILE* vuc_dump;

static void vuc_dump_program(const vu_state* before, uint32_t pc, uint32_t pairs, uint32_t cycles,
                             uint32_t kicks, uint64_t hash, uint64_t khash) {
    static int tried;
    if (!tried) {
        tried = 1;
        const char* path = getenv("PS2_VU_VERIFY_DUMP");
        if (path && *path && !(vuc_dump = fopen(path, "wb"))) fprintf(stderr, "[vu] cannot write %s\n", path);
    }
    if (!vuc_dump || vuc_mismatches > 200) return;
    const uint32_t msize = (before->pcmask + 1) * 8u, dsize = (before->dmask + 1) * 16u;
    vu_rec_head h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "VUP1", 4);
    h.idx = (uint32_t)before->idx; h.addr = pc; h.vblank = ps2_vblank_count();
    h.micro_size = msize; h.data_size = dsize;
    memcpy(h.vf, before->vf, sizeof h.vf); h.acc = before->acc; memcpy(h.vi, before->vi, sizeof h.vi);
    h.I = before->I; h.Q = before->Q; h.P = before->P; h.R = before->R;
    h.status = before->status; h.mac = before->mac; h.clip = before->clip; h.top = before->top; h.itop = before->itop;
    vu_rec_tail t = { pairs, cycles, kicks, 0, hash, khash };
    fwrite(&h, sizeof h, 1, vuc_dump);
    fwrite(before->micro, msize, 1, vuc_dump);
    fwrite(vuc_before_data, dsize, 1, vuc_dump);
    fwrite(&t, sizeof t, 1, vuc_dump);
    fflush(vuc_dump);
}

static uint32_t vuc_verify(vu_state* st, uint32_t pc, const vuc_program* p, uint32_t idx) {
    vuc_shadow = *st;
    memcpy(vuc_shadow_data, st->data, (st->dmask + 1u) * 16u);
    memcpy(vuc_before_data, st->data, (st->dmask + 1u) * 16u);
    vuc_shadow.data = vuc_shadow_data;
    const uint64_t c0 = st->cycles;
    vuc_khash = VU_HASH_SEED; vuc_kicks = 0; vuc_hashing = 1;
    const uint32_t n = p->fn(st, idx);
    const uint64_t kh = vuc_khash;
    const uint32_t kn = vuc_kicks;
    vu_state before = vuc_shadow;
    vuc_khash = VU_HASH_SEED; vuc_kicks = 0; vuc_shadow_active = 1;
    const uint32_t n2 = vu_run(&vuc_shadow, pc);
    vuc_shadow_active = 0; vuc_hashing = 0;
    vuc_verified++;
    const uint32_t cyc = (uint32_t)(st->cycles - c0), cyc2 = (uint32_t)(vuc_shadow.cycles - c0);
    const uint64_t h = vu_state_hash(st), h2 = vu_state_hash(&vuc_shadow);
    if (n != n2 || cyc != cyc2 || h != h2 || kh != vuc_khash || kn != vuc_kicks) {
        vuc_dump_program(&before, pc, n2, cyc2, vuc_kicks, h2, vuc_khash);
        if (vuc_mismatches++ < 20)
            fprintf(stderr, "[vu] verify: VU%d %s at 0x%04X differs from the interpreter: pairs %u/%u cycles %u/%u kicks %u/%u%s%s "
                    "(%u programs verified)\n", st->idx, p->name, pc * 8u, n, n2, cyc, cyc2, kn, vuc_kicks,
                    h != h2 ? " state differs" : "", kh != vuc_khash ? " xgkick cycles/addresses differ" : "", vuc_verified);
    } else if ((vuc_verified & 0xFFFFF) == 0) {
        fprintf(stderr, "[vu] verify: %u programs verified, %u mismatches\n", vuc_verified, vuc_mismatches);
    }
    return n;
}

uint32_t vu_execute(vu_state* st, uint32_t pc) {
    if (!vuc_mode) {
        const char* e = getenv("PS2_VU_INTERP");
        const char* v = getenv("PS2_VU_VERIFY");
        vuc_mode = (e && *e && *e != '0') ? 2 : (v && *v && *v != '0') ? 3 : 1;
        if (vuc_nprogs)
            fprintf(stderr, "[vu] %u recompiled microcode units%s\n", vuc_nprogs,
                    vuc_mode == 2 ? " (not used: PS2_VU_INTERP)" : vuc_mode == 3 ? " (verified against the interpreter: PS2_VU_VERIFY)" : "");
    }
    pc &= st->pcmask;
    if (vuc_mode != 2 && vuc_nprogs) {
        vuc_slot* s = &vuc_slots[st->idx][pc];
        if (s->gen != micro_gen[st->idx]) vuc_lookup(st, pc, s);
        if (s->prog) {
            const vuc_program* p = vuc_progs[s->prog - 1];
            const uint32_t n = vuc_mode == 3 ? vuc_verify(st, pc, p, s->idx) : p->fn(st, s->idx);
            if (n != VUC_NOT_COMPILED) {
                st->cprograms++;
                st->cpairs += n;
                return n;
            }
        }
    }
    return vu_run(st, pc);
}

/* ---- recording (PS2_VU_RECORD) ---------------------------------------------------------------------
 * PS2_VU_RECORD=<file> records PS2_VU_RECORD_COUNT (default 2000) microprograms from VBlank
 * PS2_VU_RECORD_FROM (default 0) on: registers and memory before each run, and its pair/cycle
 * counts, resulting state hash and XGKICKs after it (format in vu_internal.h). scripts/vu_replay.ps1
 * runs a recording with any version of the interpreter: a benchmark on real game microcode and an
 * exactness check of interpreter changes. */
static struct {
    int      state;                      /* 0: not set up, 1: off, 2: armed, 3: recording a program */
    FILE*    f;
    uint32_t from, left, kicks;
    uint64_t khash, cyc0;
    int      have_micro[2];
    uint8_t  micro[2][16384];            /* micro memory as last written to the file */
} vrec;

static void vrec_begin(vu_state* st, uint32_t addr) {
    if (!vrec.state) {
        const char* path = getenv("PS2_VU_RECORD");
        vrec.state = 1;
        if (!path || !*path) return;
        if (!(vrec.f = fopen(path, "wb"))) { fprintf(stderr, "[vu] cannot write %s\n", path); return; }
        const char* s = getenv("PS2_VU_RECORD_FROM");
        vrec.from = s ? (uint32_t)strtoul(s, NULL, 0) : 0;
        s = getenv("PS2_VU_RECORD_COUNT");
        vrec.left = s ? (uint32_t)strtoul(s, NULL, 0) : 2000;
        if (!vrec.left) vrec.left = 1;
        vrec.state = 2;
        fprintf(stderr, "[vu] recording %u microprograms from VBlank %u to %s\n", vrec.left, vrec.from, path);
    }
    if (vrec.state != 2 || ps2_vblank_count() < vrec.from) return;
    const int u = st->idx;
    const uint32_t msize = (st->pcmask + 1) * 8u, dsize = (st->dmask + 1) * 16u;
    const int same = vrec.have_micro[u] && !memcmp(vrec.micro[u], st->micro, msize);
    vu_rec_head h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "VUP1", 4);
    h.idx = (uint32_t)u; h.addr = addr; h.vblank = ps2_vblank_count();
    h.micro_size = same ? 0 : msize; h.data_size = dsize;
    memcpy(h.vf, st->vf, sizeof h.vf); h.acc = st->acc; memcpy(h.vi, st->vi, sizeof h.vi);
    h.I = st->I; h.Q = st->Q; h.P = st->P; h.R = st->R;
    h.status = st->status; h.mac = st->mac; h.clip = st->clip; h.top = st->top; h.itop = st->itop;
    fwrite(&h, sizeof h, 1, vrec.f);
    if (!same) {
        fwrite(st->micro, msize, 1, vrec.f);
        memcpy(vrec.micro[u], st->micro, msize);
        vrec.have_micro[u] = 1;
    }
    fwrite(st->data, dsize, 1, vrec.f);
    vrec.kicks = 0;
    vrec.khash = VU_HASH_SEED;
    vrec.cyc0 = st->cycles;
    vrec.state = 3;
}

static void vrec_end(vu_state* st, uint32_t pairs) {
    vu_rec_tail t = { pairs, (uint32_t)(st->cycles - vrec.cyc0), vrec.kicks, 0, vu_state_hash(st), vrec.khash };
    fwrite(&t, sizeof t, 1, vrec.f);
    vrec.state = 2;
    if (!--vrec.left) {
        fclose(vrec.f);
        vrec.f = NULL;
        vrec.state = 1;
        fprintf(stderr, "[vu] recording complete\n");
    }
}

/* ---- running microprograms ---------------------------------------------------------------------- */

/* (r2-flicker, diagnostics) PS2_VU_SNAP=<vblank> PS2_VU_SNAP_DIR=<dir> [PS2_VU_SNAP_NAME=<unit>] [PS2_VU_SNAP_COUNT=n]:
 * before each of the first n (default 40) VU1 runs of the recompiled unit (default 188054619, the shadow volumes) from
 * VBlank <vblank> on, the VU state
 * (vi, vf, acc, I/Q/P, top/itop), VIF1's registers and VU1 data memory into <dir>\snap_<vblank>_<n>_<addr>.bin */
__attribute__((weak)) void vif_debug_regs(int vif, uint32_t out[18]) { (void)vif; memset(out, 0, 72); }   /* (vif.c's when linked) */
__attribute__((weak)) int vu1_trk;
__attribute__((weak)) uint8_t vu1_trk_w[1024], vu1_trk_r[1024];
static void vu_snap(vu_state* st, uint32_t addr_dw) {
    static int init, left;
    static uint32_t from, seq;
    static char dir[300], name[40];
    if (!init) {
        init = 1;
        const char* e = getenv("PS2_VU_SNAP"), *d = getenv("PS2_VU_SNAP_DIR"), *n = getenv("PS2_VU_SNAP_NAME"), *c = getenv("PS2_VU_SNAP_COUNT");
        if (!e || !*e || !d || !*d) return;
        from = (uint32_t)strtoul(e, NULL, 0);
        snprintf(dir, sizeof dir, "%s", d);
        snprintf(name, sizeof name, "%s", n && *n ? n : "188054619");
        left = c ? atoi(c) : 40;
        fprintf(stderr, "[vu] snapshots of %s from VBlank %u into %s (%d)\n", name, from, dir, left);
    }
    if (left <= 0 || st->idx != 1 || ps2_vblank_count() < from || !vuc_nprogs) return;
    uint32_t pc = addr_dw & st->pcmask;
    vuc_slot* s = &vuc_slots[1][pc];
    if (s->gen != micro_gen[1]) vuc_lookup(st, pc, s);
    int mine = s->prog && !strcmp(vuc_progs[s->prog - 1]->name, name);
    /* PS2_VU_SNAP_TRACK=1 (with PS2_VU_INTERP=1): from each class begin (0x1AA0) to the next other program, the VU1 data
     * lanes the unit reads before anything (VIF or VU) wrote them: lmap_<n>.bin (1024 bytes, lane bits) */
    static int track = -1;
    if (track < 0) { const char* e = getenv("PS2_VU_SNAP_TRACK"); track = e && *e == '1'; }
    if (track && vu1_trk && !mine) {
        char lp[400];
        snprintf(lp, sizeof lp, "%s\\lmap_%06u_%03u.bin", dir, ps2_vblank_count(), seq);
        FILE* lf = fopen(lp, "wb");
        if (lf) { fwrite(vu1_trk_r, 1024, 1, lf); fclose(lf); }
        vu1_trk = 0;
    }
    if (!mine) return;
    if (track && pc * 8u == 0x1AA0u) { memset(vu1_trk_w, 0, 1024); memset(vu1_trk_r, 0, 1024); vu1_trk = 1; }
    left--;
    char path[400];
    snprintf(path, sizeof path, "%s\\snap_%06u_%03u_%04X.bin", dir, ps2_vblank_count(), seq++, pc * 8u);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t vr[18];
    vif_debug_regs(1, vr);
    fwrite("VUS1", 4, 1, f);
    fwrite(st->vi, sizeof st->vi, 1, f);
    fwrite(st->vf, sizeof st->vf, 1, f);
    fwrite(&st->acc, sizeof st->acc, 1, f);
    fwrite(&st->I, 4, 1, f); fwrite(&st->Q, 4, 1, f); fwrite(&st->P, 4, 1, f);
    fwrite(&st->top, 4, 1, f); fwrite(&st->itop, 4, 1, f);
    fwrite(vr, sizeof vr, 1, f);
    fwrite(st->data, (st->dmask + 1) * 16u, 1, f);
    fclose(f);
}

/* (r2-flicker) CPU time per PS2_GPU_AB phase (r2_capture.c sets vu_ab_phase; -1 off): [phase][0] VU1 programs,
 * [1] VIF1 (vif.c, its programs' time included) */
volatile int vu_ab_phase = -1;
double vu_ab_ms[2][2];
static double qpc_ms_now(void) { static double f; LARGE_INTEGER t; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = 1000.0 / (double)q.QuadPart; } QueryPerformanceCounter(&t); return (double)t.QuadPart * f; }
double vu_ab_now(void) { return qpc_ms_now(); }

static void run(vu_state* st, uint32_t addr_dw) {
    int perf_prev = perf_enter(PERF_VU);
    int abp = st->idx == 1 ? vu_ab_phase : -1;
    double ab0 = abp >= 0 ? qpc_ms_now() : 0;
    vu_snap(st, addr_dw);
    if (vrec.state != 1) vrec_begin(st, addr_dw);
    unsigned csr = _mm_getcsr();
    host_csr[st->idx & 1] = csr;
    _mm_setcsr(VU_MXCSR);
    uint32_t pairs = vu_execute(st, addr_dw);
    _mm_setcsr(csr);
    if (vrec.state == 3) vrec_end(st, pairs);
    if (abp >= 0) vu_ab_ms[abp & 1][0] += qpc_ms_now() - ab0;
    perf_leave(perf_prev);
}

/* (r2-m5b) a private VU1 on another thread (runtime/r2/r2_clip.c: the main program's clipper for the modern renderer):
 * its own data / micro memory and op cache, its XGKICKs handed to a callback instead of the GIF. The recompiled unit
 * whose image matches the private micro memory runs it (the interpreter otherwise), exactly as on the real VU1. */
static _Thread_local vu_private_kick_fn tl_kick;
static _Thread_local void* tl_kick_user;
static _Thread_local const vu_state* tl_kick_st;

void vu_private_init(vu_state* st, uint8_t* data, uint8_t* micro, vu_op* cache) {
    memset(st, 0, sizeof *st);
    st->idx = 1;
    st->data = data;
    st->dmask = 0x3FF;
    st->micro = micro;
    st->pcmask = 0x7FF;
    st->cache = cache;
    vu_init_state(st);
    st->inited = 1;
}

int vu_compiled_image(const char* name, uint8_t* micro, uint32_t size) {
    for (uint32_t k = 0; k < vuc_nprogs; k++) {
        const vuc_program* p = vuc_progs[k];
        if (p->unit != 1 || strcmp(p->name, name)) continue;
        const uint32_t* code = p->code;
        for (uint32_t r = 0; r < p->nranges; r++) {
            const vuc_range* g = &p->ranges[r];
            if ((uint32_t)(g->start + g->len) * 8u > size) return 0;
            memcpy(micro + g->start * 8u, code, g->len * 8u);
            code += g->len * 2u;
        }
        return 1;
    }
    return 0;
}

uint32_t vu_private_run(vu_state* st, uint32_t pc, vu_private_kick_fn kick, void* user) {
    static _Thread_local const vu_state* cst;
    static _Thread_local const vuc_program* cprog[2048];
    static _Thread_local uint16_t cidx[2048];
    static _Thread_local uint8_t cknown[2048];
    pc &= st->pcmask;
    if (cst != st) { cst = st; memset(cknown, 0, sizeof cknown); }   /* (the private micro memory never changes) */
    if (!cknown[pc]) {
        cknown[pc] = 1;
        cprog[pc] = NULL;
        const char* e = getenv("PS2_VU_INTERP");
        if (!(e && *e && *e != '0'))
            for (uint32_t k = 0; k < vuc_nprogs && !cprog[pc]; k++) {
                const vuc_program* p = vuc_progs[k];
                if (p->unit != 1) continue;
                for (uint32_t i = 0; i < p->nentries; i++)
                    if (p->entries[i] == pc) { if (vuc_image_matches(st, p)) { cprog[pc] = p; cidx[pc] = (uint16_t)i; } break; }
            }
    }
    const vu_private_kick_fn ok = tl_kick;
    void* ou = tl_kick_user;
    const vu_state* os = tl_kick_st;
    tl_kick = kick; tl_kick_user = user; tl_kick_st = st;
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(VU_MXCSR);
    uint32_t n = VUC_NOT_COMPILED;
    if (cprog[pc]) n = cprog[pc]->fn(st, cidx[pc]);
    if (n == VUC_NOT_COMPILED) n = vu_run(st, pc);
    _mm_setcsr(csr);
    tl_kick = ok; tl_kick_user = ou; tl_kick_st = os;
    return n;
}

volatile int vu1_drop_kicks;           /* (r2-flicker) set by VIF1 for the runs of a kept native chain */
void vu_xgkick(uint32_t qw_addr) {
    if (tl_kick) { tl_kick(tl_kick_user, tl_kick_st->data, qw_addr & 0x3FF); return; }   /* (r2-m5b) a private VU1 */
    if (vu1_drop_kicks) return;          /* (r2-flicker) a native mesh's program: its packets are R2's (vif.c) */
    if (vuc_hashing) {                   /* PS2_VU_VERIFY: hash both runs' kicks; the shadow's go no further */
        const vu_state* cur = vuc_shadow_active ? &vuc_shadow : &vu_st[1];
        vuc_khash = vu_kick_hash(vuc_khash, qw_addr, cur->cyc);
        /* and the whole data memory at the kick: what the GS receives is byte-identical, so are the
         * pictures (cross-run frame comparisons cannot show this: VBlank timing is wall-clock) */
        vuc_khash = vu_hash_words(vuc_khash, cur->data, (cur->dmask + 1u) * 16u);
        vuc_kicks++;
        if (vuc_shadow_active) return;
    }
    if (vrec.state == 3) { vrec.khash = vu_kick_hash(vrec.khash, qw_addr, vu_st[1].cyc); vrec.kicks++; }
    _mm_setcsr(host_csr[1]);             /* the GIF/GS code runs in the host's FP mode */
    gif_path1_kick(vu1_data, qw_addr & 0x3FF);
    _mm_setcsr(VU_MXCSR);
}

void vu1_start(uint32_t addr_dw) {
    vu_state* st = get(1);
    static int logged;
    if (!logged++) fprintf(stderr, "[vu] first VU1 microprogram at 0x%04X\n", (addr_dw & st->pcmask) * 8);
    run(st, addr_dw);
}

int vu1_busy(void) { return 0; }

void vu_set_vif_tops(int vu, uint32_t top, uint32_t itop) {
    vu_state* st = get(vu ? 1 : 0);
    st->top = top;
    st->itop = itop;
}

/* VU0 micro mode shares its registers with macro mode, whose copies live in the EE context. */
void vu0_callms(ps2_ctx* ctx, uint32_t addr_dw) {
    vu_state* st = get(0);
    static int logged;
    if (!logged++) fprintf(stderr, "[vu] first VU0 microprogram at 0x%04X\n", (addr_dw & st->pcmask) * 8);
    memcpy(&st->vf[1], &ctx->vf[1], 31 * sizeof(vf128));
    st->acc = ctx->vacc;
    memcpy(&st->vi[1], &ctx->vi[1], 15 * sizeof(uint16_t));
    memcpy(&st->I, &ctx->vi_reg, 4);
    memcpy(&st->Q, &ctx->vq, 4);
    memcpy(&st->P, &ctx->vp, 4);
    st->R = 0x3F800000u | (ctx->vr & 0x7FFFFFu);   /* 23 bits; the EE-side copy may hold any value */
    run(st, addr_dw);
    memcpy(&ctx->vf[1], &st->vf[1], 31 * sizeof(vf128));
    ctx->vacc = st->acc;
    memcpy(&ctx->vi[1], &st->vi[1], 15 * sizeof(uint16_t));
    memcpy(&ctx->vi_reg, &st->I, 4);
    memcpy(&ctx->vq, &st->Q, 4);
    memcpy(&ctx->vp, &st->P, 4);
    ctx->vr = st->R;
}

/* ---- VU1 registers in VU0's data space (0x4000 + 16 * n) ------------------------------------------ */

void vu1_reg_read(uint32_t idx, uint32_t out[4]) {
    gfx_sync();                          /* (pc-fps2) */
    vu_state* v = get(1);
    memset(out, 0, 16);
    idx &= 0x3F;
    if (idx < 32) { memcpy(out, &v->vf[idx], 16); return; }
    idx -= 32;
    if (idx < 16) { out[0] = v->vi[idx]; return; }
    switch (idx) {
    case 16: out[0] = v->status & 0xFFF; break;
    case 17: out[0] = v->mac & 0xFFFF; break;
    case 18: out[0] = v->clip & 0xFFFFFF; break;
    case 20: out[0] = v->R; break;
    case 21: out[0] = v->I; break;
    case 22: out[0] = v->Q; break;
    case 23: out[0] = v->P; break;
    case 26: out[0] = v->tpc; break;
    default: break;
    }
}

void vu1_reg_write(uint32_t idx, const uint32_t in[4], unsigned lanes) {
    gfx_sync();                          /* (pc-fps2) */
    vu_state* v = get(1);
    idx &= 0x3F;
    if (idx < 32) {
        if (idx)
            for (int k = 0; k < 4; k++) if (lanes & (1u << k)) v->vf[idx].u[k] = in[k];
        return;
    }
    if (!(lanes & 1)) return;            /* integer and control registers live in the x word */
    idx -= 32;
    if (idx < 16) { if (idx) v->vi[idx] = (uint16_t)in[0]; return; }
    switch (idx) {
    case 16: v->status = (v->status & 0x3F) | (in[0] & 0xFC0); break;
    case 18: v->clip = in[0] & 0xFFFFFF; break;
    case 20: v->R = 0x3F800000u | (in[0] & 0x7FFFFF); break;
    case 21: v->I = in[0]; break;
    case 22: v->Q = in[0]; break;
    default: break;                      /* MAC, P, TPC: read-only */
    }
}

/* Address (64-bit units) where a unit's last microprogram stopped, for VIF MSCNT. */
uint32_t vu_get_tpc(int vu) { return get(vu ? 1 : 0)->tpc; }

/* Macro mode FMAC flags. COP2 ops share VU0's MAC/status registers with micro mode; the
 * recompiler can call this after a flag-setting macro op (VADD/VSUB/VMUL/VMADD/VMSUB/VOPMULA/
 * VOPMSUB and their ACC/bc/q/i forms) with the result and the instruction's dest field (x = 8).
 * Underflow cannot be told apart from an exact zero after the fact, so U is never set. */
void vu0_macro_flags(const vf128* r, unsigned dest_xyzw) {
    vu_state* st = get(0);
    /* (P6: lanes in parallel, no branches; ~2% of the EE thread in a hub) per lane k (dest bit 8 >> k): Z = exponent 0
     * (zero or denormal), S = sign, O = magnitude >= FLT_MAX's bits */
    __m128i v = _mm_loadu_si128((const __m128i*)r), a = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    unsigned zl = (unsigned)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(a, _mm_set1_epi32(0x7F800000)),
                                                                            _mm_setzero_si128())));
    unsigned sl = (unsigned)_mm_movemask_ps(_mm_castsi128_ps(v));
    unsigned ol = (unsigned)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(a, _mm_set1_epi32(0x7F7FFFFE))));
    unsigned d = dest_xyzw & 15;                 /* lane bit k -> dest bit 8 >> k */
    unsigned z = VU_REV4[zl] & d, s = VU_REV4[sl] & d, o = VU_REV4[ol] & d;
    unsigned zsuo = (z ? 1u : 0u) | (s ? 2u : 0u) | (o ? 8u : 0u);
    st->mac = o << 12 | s << 4 | z;
    st->status = (st->status & ~0xFu) | zsuo | zsuo << 6;
}

/* ---- VU0 control registers (CFC2/CTC2 16-31) ---------------------------------------------------------- */

static void reset_vu(vu_state* st) {
    st->status = st->mac = st->clip = st->clip_pipe = 0;
    st->tpc = 0;
}

uint32_t vu0_cfc2_ctrl(ps2_ctx* ctx, int reg) {
    vu_state* st = get(0);
    uint32_t u;
    switch (reg) {
    case 16: return st->status & 0xFFF;
    case 17: return st->mac & 0xFFFF;
    case 18: return st->clip & 0xFFFFFF;
    case 20: return ctx->vr;
    case 21: memcpy(&u, &ctx->vi_reg, 4); return u;
    case 22: memcpy(&u, &ctx->vq, 4); return u;
    case 23: memcpy(&u, &ctx->vp, 4); return u;
    case 26: return st->tpc;
    case 27: return cmsar0;
    case 28: return fbrst;               /* DE/TE bits; FB/RS read as 0 */
    case 29: return 0;                   /* VPU-STAT: neither VU busy (synchronous), no DIV/EFU busy */
    case 31: return cmsar1;
    default: return 0;
    }
}

void vu0_ctc2_ctrl(ps2_ctx* ctx, int reg, uint32_t v) {
    vu_state* st = get(0);
    switch (reg) {
    case 16: st->status = (st->status & 0x3F) | (v & 0xFC0); break;   /* only the sticky bits */
    case 18: st->clip = st->clip_pipe = v & 0xFFFFFF; break;
    case 20: ctx->vr = 0x3F800000u | (v & 0x7FFFFF); break;
    case 21: memcpy(&ctx->vi_reg, &v, 4); break;
    case 22: memcpy(&ctx->vq, &v, 4); break;
    case 27: cmsar0 = v & 0xFFFF; break;
    case 28:                                                          /* FBRST */
        if (v & 0x002) reset_vu(st);
        if (v & 0x200) reset_vu(get(1));
        fbrst = v & 0x0C0C;
        break;
    case 31:                                                          /* CMSAR1: start VU1 */
        gfx_sync();                                                   /* (pc-fps2) */
        cmsar1 = v & 0xFFFF;
        vu1_start(cmsar1);
        break;
    default: break;                      /* MAC (17), P (23), TPC (26), VPU-STAT (29): read-only */
    }
}
