/* VU0/VU1: memory as seen by the EE, micro mode entry points (VIF MSCAL/CMSAR1 for VU1, VCALLMS for
 * VU0), and the VU0 control registers reached through CFC2/CTC2 16-31.
 *
 * The interpreter itself is in vu_exec.c; vu_internal.h describes the timing model. Everything is
 * synchronous: a microprogram runs to its E bit inside the call that starts it, so the VUs are
 * never busy as far as VIF, VPU-STAT or the EE can tell.
 */
#include "vu_record.h"
#include "perf.h"

#include <stdlib.h>
#include <xmmintrin.h>

static _Alignas(16) uint8_t vu0_data[4096], vu0_micro[4096], vu1_data[16384], vu1_micro[16384];
static vu_op vu0_cache[512], vu1_cache[2048];
vu_state vu_st[2];

static uint32_t cmsar0, cmsar1, fbrst;
static unsigned host_csr;                /* MXCSR of the caller while a microprogram runs */

static vu_state* get(int vu) {
    vu_state* st = &vu_st[vu];
    if (!st->inited) {
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
uint8_t* vu_micro_mem(int vu) { return vu ? vu1_micro : vu0_micro; }

/* EE view: 0x11000000 VU0 micro, 0x11004000 VU0 data, 0x11008000 VU1 micro, 0x1100C000 VU1 data
 * (VU0's 4 KB areas mirror through their 16 KB windows). Writes to micro memory need no
 * notification: the decoded-instruction cache checks every pair against memory before using it. */
void vu_mem_access(uint32_t phys, void* data, unsigned size, int is_write) {
    uint32_t off = phys - 0x11000000u;
    uint8_t* mem;
    uint32_t len;
    if (off < 0x4000)       { mem = vu0_micro; len = sizeof vu0_micro; }
    else if (off < 0x8000)  { mem = vu0_data;  len = sizeof vu0_data;  off -= 0x4000; }
    else if (off < 0xC000)  { mem = vu1_micro; len = sizeof vu1_micro; off -= 0x8000; }
    else                    { mem = vu1_data;  len = sizeof vu1_data;  off -= 0xC000; }
    off &= len - 1;
    if (off + size > len) { if (!is_write) memset(data, 0, size); return; }
    if (is_write) memcpy(mem + off, data, size); else memcpy(data, mem + off, size);
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

static void run(vu_state* st, uint32_t addr_dw) {
    int perf_prev = perf_enter(PERF_VU);
    if (vrec.state != 1) vrec_begin(st, addr_dw);
    unsigned csr = _mm_getcsr();
    host_csr = csr;
    _mm_setcsr(VU_MXCSR);
    uint32_t pairs = vu_run(st, addr_dw);
    _mm_setcsr(csr);
    if (vrec.state == 3) vrec_end(st, pairs);
    perf_leave(perf_prev);
}

void vu_xgkick(uint32_t qw_addr) {
    if (vrec.state == 3) { vrec.khash = vu_kick_hash(vrec.khash, qw_addr, vu_st[1].cyc); vrec.kicks++; }
    _mm_setcsr(host_csr);                /* the GIF/GS code runs in the host's FP mode */
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
    unsigned z = 0, s = 0, o = 0;
    for (int k = 0; k < 4; k++) {
        unsigned bit = 8u >> k;
        if (!(dest_xyzw & bit)) continue;
        uint32_t u = r->u[k], a = u & 0x7FFFFFFFu;
        if (!a || (a & 0x7F800000u) == 0) z |= bit;
        if (u >> 31) s |= bit;
        if (a >= 0x7F7FFFFFu) o |= bit;
    }
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
        cmsar1 = v & 0xFFFF;
        vu1_start(cmsar1);
        break;
    default: break;                      /* MAC (17), P (23), TPC (26), VPU-STAT (29): read-only */
    }
}
