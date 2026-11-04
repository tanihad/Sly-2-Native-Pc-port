/* VIF0 / VIF1: the VIFcode processor in front of VU0 / VU1.
 *
 * Written from the documented behaviour (ps2tek's VIF chapter, Sony's EE User's Manual, chapter
 * "VIF"). Summary of the model:
 *
 *  - The command stream is parsed 32 bits at a time. A command may be split over any number of
 *    vif_dma / vif_dma_tag / FIFO-write calls, so every bit of parser state lives in vif_state.
 *  - Everything runs synchronously: UNPACK/MPG write VU memory at once, MSCAL runs the VU1
 *    microprogram inside vu1_start(), DIRECT hands quadwords straight to gif_path2().
 *  - Real stalls stop the parser: the i bit (STAT.INT set + INTC_VIF0/1 raised when the command
 *    completes; the next command other than MARK then stalls with STAT.VIS), FBRST.STP (VSS),
 *    FBRST.FBK (VFS), invalid commands (ER1, unless ERR.ME1). FBRST.STC resumes.
 *    Softer waits (GIF refusing PATH2 data = VGW, VU still busy) are retried on the next call.
 *  - When the parser stops inside a quadword the DMAC already delivered, the rest of that quadword
 *    is kept in a small FIFO (the hardware has a 8/16-qword FIFO too) and processed first when the
 *    VIF resumes: on FBRST.STC, or the next vif_dma / vif_dma_tag / FIFO write / register access.
 *  - UNPACK: all V1/V2/V3/V4 x 32/16/8 formats and V4-5, USN, FLG (+TOPS), write mask, MODE
 *    (offset / difference), CYCLE skipping and filling, addresses wrap inside VU data memory.
 *    Unspecified hardware details (ps2tek: "indeterminate") are filled in as follows:
 *      V2 writes (x, y, x, y); V3 writes the next element of the stream into w (0 after the last
 *      vector); a filling cycle (CL < WL) with mask 00 writes the row register.
 *  - VIF1 double buffering: OFFSET clears DBF and sets TOPS = BASE; every microprogram start
 *    (MSCAL/MSCALF/MSCNT) copies TOPS -> TOP and ITOPS -> ITOP, flips DBF and sets
 *    TOPS = BASE (+ OFST when DBF = 1), then tells the VU through vu_set_vif_tops().
 */
#include "hw.h"
#include "perf.h"
#include "gfx_async.h"

#include <stdlib.h>

/* ---- register bits --------------------------------------------------------------------------- */

#define ST_VPS_WAIT 0x1u               /* VPS = 1: waiting for the data of a command */
#define ST_VPS_DECODE 0x2u             /* VPS = 2: decoding (a command waiting for the VU) */
#define ST_VPS_XFER 0x3u               /* VPS = 3: transferring (DIRECT waiting on the GIF) */
#define ST_VEW (1u << 2)
#define ST_VGW (1u << 3)
#define ST_MRK (1u << 6)
#define ST_DBF (1u << 7)
#define ST_VSS (1u << 8)
#define ST_VFS (1u << 9)
#define ST_VIS (1u << 10)
#define ST_INT (1u << 11)
#define ST_ER0 (1u << 12)
#define ST_ER1 (1u << 13)
#define ST_FDR (1u << 23)
#define ST_STALL (ST_VSS | ST_VFS | ST_VIS | ST_ER0 | ST_ER1)

#define ERR_MII 1u
#define ERR_ME0 2u
#define ERR_ME1 4u

enum {
    CMD_NOP = 0x00, CMD_STCYCL = 0x01, CMD_OFFSET = 0x02, CMD_BASE = 0x03, CMD_ITOP = 0x04,
    CMD_STMOD = 0x05, CMD_MSKPATH3 = 0x06, CMD_MARK = 0x07, CMD_FLUSHE = 0x10, CMD_FLUSH = 0x11,
    CMD_FLUSHA = 0x13, CMD_MSCAL = 0x14, CMD_MSCALF = 0x15, CMD_MSCNT = 0x17, CMD_STMASK = 0x20,
    CMD_STROW = 0x30, CMD_STCOL = 0x31, CMD_MPG = 0x4A, CMD_DIRECT = 0x50, CMD_DIRECTHL = 0x51,
    CMD_UNPACK = 0x60
};

/* Kind of multi-word command in progress */
enum { K_NONE, K_STMASK, K_STROW, K_STCOL, K_MPG, K_UNPACK, K_DIRECT };

#define FIFO_WORDS 256                 /* host-side holding buffer (hardware: 32 / 64 words) */

typedef struct {
    int      id;                       /* 0 = VIF0, 1 = VIF1 */
    uint32_t qmask;                    /* VU data memory size in quadwords - 1 */
    uint32_t micro_size;               /* VU micro memory size in bytes */

    /* registers. stat keeps MRK DBF VSS VFS VIS INT ER0 ER1 FDR VGW; VPS, VEW, FQC are computed */
    uint32_t stat, err, mark, cycle, mode, num, mask, code, itops, base, ofst, tops, itop, top;
    uint32_t r[4], c[4];
    uint32_t r2_arm, r2_skip, r2_norun;   /* (r2-flicker) MARK A5A5 (A5A6), MARK n: the next n MSCALs' programs kick nothing
                                          (A5A6: run nothing, the double buffer still flips) */

    /* command in progress */
    int      kind;                     /* K_NONE between commands */
    uint32_t left;                     /* data words the command still expects */
    uint32_t pos;                      /* STROW/STCOL element; MPG byte address */
    int      stop_req;                 /* FBRST.STP arrived during a command */
    int      vu_wait;                  /* CODE holds a command waiting for the VU (STAT.VPS = 2) */

    /* UNPACK */
    uint32_t u_fmt;                    /* CMD bits 0-3: vn << 2 | vl (0xF = V4-5) */
    uint32_t u_vbytes, u_ebytes;       /* bytes per input vector / element */
    uint32_t u_addr;                   /* next write address in quadwords (wrapped on use) */
    uint32_t u_out, u_in;              /* vectors still to write / input vectors still to read */
    uint32_t u_cyc, u_cl, u_wl;        /* write cycle counter; CYCLE latched at the start */
    int      u_masked, u_usn;
    uint32_t u_buflen;                 /* bytes of a vector split across calls */
    uint8_t  u_buf[32];

    /* DIRECT: a partial quadword (or one the GIF has not accepted yet) */
    _Alignas(16) uint32_t dq[4];
    uint32_t dq_n;

    /* FIFO: words received but not processed yet */
    uint32_t fifo_rd, fifo_n;
    uint32_t fifo[FIFO_WORDS];
} vif_state;

static vif_state vifs[2] = {
    { .id = 0, .qmask = 4096 / 16 - 1, .micro_size = 4096 },
    { .id = 1, .qmask = 16384 / 16 - 1, .micro_size = 16384 },
};

static void log_once(int* flag, const char* fmt, uint32_t a, uint32_t b) {
    if (*flag) return;
    *flag = 1;
    fprintf(stderr, "[vif] ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, "\n");
}

static void raise_irq(const vif_state* v) { intc_raise(v->id ? INTC_VIF1 : INTC_VIF0); }

static int vu_running(const vif_state* v) { return v->id == 1 && vu1_busy(); }

/* End of a command: the i bit raises the interrupt and arms the stall; a pending STP stops here. */
static void cmd_done(vif_state* v) {
    v->kind = K_NONE;
    v->left = 0;
    if ((v->code & 0x80000000u) && !(v->err & ERR_MII)) {
        v->stat |= ST_INT;
        raise_irq(v);
    }
    if (v->stop_req) { v->stop_req = 0; v->stat |= ST_VSS; }
}

/* ---- microprogram start (MSCAL / MSCALF / MSCNT) ----------------------------------------------- */

static void micro_start(vif_state* v, uint32_t cmd, uint32_t addr_dw) {
    v->itop = v->itops;
    if (v->id == 0) {
        /* VU0's registers are the EE's macro-mode registers, which live in the running thread. */
        vu_set_vif_tops(0, 0, v->itop);
        vu0_callms(thread_ctx(), cmd == CMD_MSCNT ? vu_get_tpc(0) : addr_dw);
        return;
    }
    v->top = v->tops;
    v->stat ^= ST_DBF;
    v->tops = (v->base + ((v->stat & ST_DBF) ? v->ofst : 0)) & 0x3FF;
    vu_set_vif_tops(1, v->top, v->itop);
    vu1_start(cmd == CMD_MSCNT ? vu_get_tpc(1) : addr_dw);
}

/* ---- UNPACK ------------------------------------------------------------------------------------ */

static inline uint32_t ld_elem(const uint8_t* p, uint32_t vl, int usn) {
    if (vl == 0) { uint32_t x; memcpy(&x, p, 4); return x; }
    if (vl == 1) { uint16_t h; memcpy(&h, p, 2); return usn ? h : (uint32_t)(int32_t)(int16_t)h; }
    return usn ? p[0] : (uint32_t)(int32_t)(int8_t)p[0];
}

static void upk_decode(const vif_state* v, const uint8_t* p, uint32_t d[4]) {
    uint32_t vl = v->u_fmt & 3, eb = v->u_ebytes;
    int usn = v->u_usn;
    switch (v->u_fmt >> 2) {
    case 0:                                                  /* S: x = y = z = w */
        d[0] = d[1] = d[2] = d[3] = ld_elem(p, vl, usn);
        break;
    case 1:                                                  /* V2: z, w indeterminate -> x, y */
        d[0] = d[2] = ld_elem(p, vl, usn);
        d[1] = d[3] = ld_elem(p + eb, vl, usn);
        break;
    case 2:                                                  /* V3: w indeterminate -> next element */
        d[0] = ld_elem(p, vl, usn);
        d[1] = ld_elem(p + eb, vl, usn);
        d[2] = ld_elem(p + 2 * eb, vl, usn);
        d[3] = v->u_in > 1 ? ld_elem(p + 3 * eb, vl, usn) : 0;
        break;
    default:
        if (vl == 3) {                                       /* V4-5: RGBA 5:5:5:1 */
            uint16_t h;
            memcpy(&h, p, 2);
            d[0] = (h & 0x1Fu) << 3;
            d[1] = ((h >> 5) & 0x1Fu) << 3;
            d[2] = ((h >> 10) & 0x1Fu) << 3;
            d[3] = ((uint32_t)h >> 15) << 7;
        } else {
            d[0] = ld_elem(p, vl, usn);
            d[1] = ld_elem(p + eb, vl, usn);
            d[2] = ld_elem(p + 2 * eb, vl, usn);
            d[3] = ld_elem(p + 3 * eb, vl, usn);
        }
        break;
    }
}

__attribute__((weak)) int vu1_trk;      /* (r2-flicker, diagnostics: vu_exec.c defines them) lanes written into VU1 data */
__attribute__((weak)) uint8_t vu1_trk_w[1024];
__attribute__((weak)) volatile int vu1_drop_kicks;   /* (r2-flicker: vu.c's) */
static inline void trk_vif(const vif_state* v, uint32_t addr, uint32_t m) {
    if (!vu1_trk || v->id != 1) return;
    uint8_t l = 0;
    for (int j = 0; j < 4; j++) if (((m >> (j * 2)) & 3) != 3) l |= (uint8_t)(1u << j);
    vu1_trk_w[addr & 0x3FF] |= l;
}

/* Write one vector (d = unpacked input, or NULL for a filling cycle) and advance the address. */
static void upk_write(vif_state* v, const uint32_t* d) {
    uint32_t* dst = (uint32_t*)(vu_data_mem(v->id) + ((v->u_addr & v->qmask) << 4));
    uint32_t row = v->u_cyc < 3 ? v->u_cyc : 3;
    uint32_t m = v->u_masked ? (v->mask >> (row * 8)) & 0xFFu : 0;
    uint32_t mode = v->mode & 3;
    trk_vif(v, v->u_addr, m);
    if (d && !m && (mode == 0 || mode == 3)) {
        memcpy(dst, d, 16);
    } else {
        for (int j = 0; j < 4; j++) {
            switch ((m >> (j * 2)) & 3) {
            case 0:
                if (!d) dst[j] = v->r[j];                    /* filling cycle */
                else if (mode == 1) dst[j] = d[j] + v->r[j]; /* offset */
                else if (mode == 2) dst[j] = v->r[j] += d[j];/* difference */
                else dst[j] = d[j];
                break;
            case 1: dst[j] = v->r[j]; break;
            case 2: dst[j] = v->c[row]; break;
            default: break;                                  /* write protected */
            }
        }
    }
    v->u_addr++;
    v->u_out--;
    if (++v->u_cyc >= v->u_wl) {
        v->u_cyc = 0;
        if (v->u_cl > v->u_wl) v->u_addr += v->u_cl - v->u_wl;   /* skipping write */
    }
    v->num = v->u_out & 0xFF;
}

/* Filling cycles (CL < WL) that come before the next input vector need no data. */
static void upk_fills(vif_state* v) {
    while (v->u_out && v->u_wl > v->u_cl && v->u_cyc >= v->u_cl) upk_write(v, NULL);
}

static void upk_begin(vif_state* v, uint32_t code) {
    uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF;
    uint32_t num = (code >> 16) & 0xFF;
    if (!num) num = 256;
    v->u_fmt = cmd & 0xF;
    v->u_masked = (cmd >> 4) & 1;
    v->u_usn = (imm >> 14) & 1;
    if (v->u_fmt == 0xF) { v->u_ebytes = 2; v->u_vbytes = 2; }
    else { v->u_ebytes = 4u >> (cmd & 3); v->u_vbytes = v->u_ebytes * (((cmd >> 2) & 3) + 1); }
    v->u_addr = imm & 0x3FF;
    if (v->id == 1 && (imm & 0x8000)) v->u_addr += v->tops;
    uint32_t cl = v->cycle & 0xFF, wl = (v->cycle >> 8) & 0xFF;
    if (wl == 0) {                                           /* undefined: treat as a plain write */
        static int logged;
        log_once(&logged, "UNPACK with CYCLE.WL = 0 (CL = %u, code 0x%08X): treated as CL = WL", cl, code);
        cl = wl = 1;
    }
    v->u_cl = cl;
    v->u_wl = wl;
    v->u_in = wl <= cl ? num : (num / wl) * cl + (num % wl < cl ? num % wl : cl);
    v->u_out = num;
    v->u_cyc = 0;
    v->u_buflen = 0;
    v->left = (v->u_in * v->u_vbytes + 3) / 4;
    v->num = num & 0xFF;
    v->kind = K_UNPACK;
}

/* (pc-fps2) Whole vectors of one format, the format and the mask flag known at compile time (Sly 2's chains: V4-5, V1-8
 * masked, V2-16 with CL > WL, V4-16, V4-8, V3-16 ...): the same writes as upk_decode + upk_write, without their per-vector
 * dispatch. No filling cycles (CL < WL: the generic loop). Returns the bytes consumed. */
static inline __attribute__((always_inline)) uint32_t upk_loop(vif_state* v, const uint8_t* src, uint32_t avail,
                                                               const uint32_t vn, const uint32_t vl, const int masked) {
    const uint32_t eb = vl == 3 ? 2 : 4u >> vl, vb = vl == 3 ? 2 : eb * (vn + 1);
    uint32_t* mem = (uint32_t*)vu_data_mem(v->id);
    const uint32_t qmask = v->qmask, mode = v->mode & 3, wl = v->u_wl, skipq = v->u_cl > wl ? v->u_cl - wl : 0;
    const int usn = v->u_usn;
    uint32_t pos = 0, addr = v->u_addr, cyc = v->u_cyc, in = v->u_in, out = v->u_out;
    while (in) {
        uint32_t need = vb + (vn == 2 && in > 1 ? eb : 0);
        if (avail - pos < need) break;
        const uint8_t* s = src + pos;
        uint32_t d[4];
        if (vl == 3) {                                       /* V4-5 */
            uint16_t h;
            memcpy(&h, s, 2);
            d[0] = (h & 0x1Fu) << 3; d[1] = ((h >> 5) & 0x1Fu) << 3; d[2] = ((h >> 10) & 0x1Fu) << 3; d[3] = ((uint32_t)h >> 15) << 7;
        } else if (vn == 0) {
            d[0] = d[1] = d[2] = d[3] = ld_elem(s, vl, usn);
        } else if (vn == 1) {
            d[0] = d[2] = ld_elem(s, vl, usn);
            d[1] = d[3] = ld_elem(s + eb, vl, usn);
        } else if (vn == 2) {
            d[0] = ld_elem(s, vl, usn); d[1] = ld_elem(s + eb, vl, usn); d[2] = ld_elem(s + 2 * eb, vl, usn);
            d[3] = in > 1 ? ld_elem(s + 3 * eb, vl, usn) : 0;
        } else {
            d[0] = ld_elem(s, vl, usn); d[1] = ld_elem(s + eb, vl, usn); d[2] = ld_elem(s + 2 * eb, vl, usn); d[3] = ld_elem(s + 3 * eb, vl, usn);
        }
        uint32_t* dst = mem + ((addr & qmask) << 2);
        uint32_t m = masked ? (v->mask >> ((cyc < 3 ? cyc : 3) * 8)) & 0xFFu : 0;
        trk_vif(v, addr, m);
        if (!m && (mode == 0 || mode == 3)) {
            memcpy(dst, d, 16);
        } else {
            uint32_t row = cyc < 3 ? cyc : 3;
            for (int j = 0; j < 4; j++) {
                switch ((m >> (j * 2)) & 3) {
                case 0:
                    if (mode == 1) dst[j] = d[j] + v->r[j];
                    else if (mode == 2) dst[j] = v->r[j] += d[j];
                    else dst[j] = d[j];
                    break;
                case 1: dst[j] = v->r[j]; break;
                case 2: dst[j] = v->c[row]; break;
                default: break;
                }
            }
        }
        addr++;
        out--;
        if (++cyc >= wl) { cyc = 0; addr += skipq; }
        in--;
        pos += vb;
    }
    v->u_addr = addr;
    v->u_cyc = cyc;
    v->u_in = in;
    v->u_out = out;
    v->num = out & 0xFF;
    return pos;
}

static uint32_t upk_fast(vif_state* v, const uint8_t* src, uint32_t avail) {
    switch ((v->u_fmt << 1) | (uint32_t)v->u_masked) {
#define UPK_CASE(VN, VL) \
    case ((VN << 2 | VL) << 1): return upk_loop(v, src, avail, VN, VL, 0); \
    case ((VN << 2 | VL) << 1) | 1: return upk_loop(v, src, avail, VN, VL, 1);
    UPK_CASE(0, 0) UPK_CASE(0, 1) UPK_CASE(0, 2)
    UPK_CASE(1, 0) UPK_CASE(1, 1) UPK_CASE(1, 2)
    UPK_CASE(2, 0) UPK_CASE(2, 1) UPK_CASE(2, 2)
    UPK_CASE(3, 0) UPK_CASE(3, 1) UPK_CASE(3, 2) UPK_CASE(3, 3)
#undef UPK_CASE
    default: return 0;
    }
}

static int upk_fast_off = -1;            /* PS2_VIF_UPK_FAST=0: the generic loop only; =check: both, compared (slow) */
static int upk_check;

static uint32_t upk_data_body(vif_state* v, const uint32_t* w, uint32_t n, int fast);

static uint32_t upk_data(vif_state* v, const uint32_t* w, uint32_t n) {
    if (upk_fast_off < 0) {
        const char* e = getenv("PS2_VIF_UPK_FAST");
        upk_fast_off = e && *e == '0';
        upk_check = e && !strcmp(e, "check");
    }
    if (!upk_check) return upk_data_body(v, w, n, !upk_fast_off);
    /* the generic loop on copies of the state and the VU memory, then the fast one: everything must agree */
    static vif_state ref;
    static _Alignas(16) uint8_t mem_before[16384], mem_ref[16384];
    static uint64_t checked, bad;
    uint8_t* mem = vu_data_mem(v->id);
    uint32_t msize = (v->qmask + 1) * 16;
    memcpy(mem_before, mem, msize);
    ref = *v;
    uint32_t r1 = upk_data_body(&ref, w, n, 0);
    memcpy(mem_ref, mem, msize);
    memcpy(mem, mem_before, msize);
    int ref_done = ref.kind == K_NONE;
    uint32_t r2 = upk_data_body(v, w, n, 1);
    checked++;
    if (r1 != r2 || memcmp(mem, mem_ref, msize) || ref.u_addr != v->u_addr || ref.u_cyc != v->u_cyc || ref.u_in != v->u_in ||
        ref.u_out != v->u_out || ref.num != v->num || ref.u_buflen != v->u_buflen || memcmp(ref.r, v->r, 16) ||
        ref_done != (v->kind == K_NONE)) {
        if (bad++ < 10) fprintf(stderr, "[vif] PS2_VIF_UPK_FAST=check: unpack fmt 0x%X masked %d mode %u cycle 0x%04X differs\n",
                                v->u_fmt, v->u_masked, v->mode & 3, v->cycle);
    }
    if ((checked & 0xFFFFF) == 0) fprintf(stderr, "[vif] PS2_VIF_UPK_FAST=check: %llu unpack calls, %llu differ\n",
                                          (unsigned long long)checked, (unsigned long long)bad);
    return r2;
}

static uint32_t upk_data_body(vif_state* v, const uint32_t* w, uint32_t n, int fast) {
    uint32_t take = n < v->left ? n : v->left;
    const uint8_t* p = (const uint8_t*)w;
    uint32_t bytes = take * 4, pos = 0, vb = v->u_vbytes;
    v->left -= take;
    /* (pc-fps2) whole vectors without filling cycles: the per-format loops */
    if (fast && !v->u_buflen && v->u_wl <= v->u_cl && v->u_in && !(v->u_fmt == 0xC && !v->u_masked))
        pos = upk_fast(v, p, bytes);

    /* Fast path: V4-32, no mask, no addition, no skipping/filling. */
    uint32_t mode = v->mode & 3;
    if (v->u_fmt == 0xC && !v->u_masked && (mode == 0 || mode == 3) && v->u_cl == v->u_wl && !v->u_buflen) {
        uint32_t nv = bytes / 16;
        if (nv > v->u_in) nv = v->u_in;
        uint8_t* mem = vu_data_mem(v->id);
        for (uint32_t done = 0; done < nv;) {
            uint32_t a = v->u_addr & v->qmask, c = v->qmask + 1 - a;
            if (c > nv - done) c = nv - done;
            memcpy(mem + a * 16, p + pos, c * 16);
            for (uint32_t k = 0; k < c; k++) trk_vif(v, a + k, 0);
            pos += c * 16;
            done += c;
            v->u_addr += c;
        }
        v->u_in -= nv;
        v->u_out -= nv;
        v->u_cyc = (v->u_cyc + nv) % v->u_wl;
        v->num = v->u_out & 0xFF;
    }

    while (v->u_in) {
        upk_fills(v);
        uint32_t need = vb + ((v->u_fmt >> 2) == 2 && v->u_in > 1 ? v->u_ebytes : 0);
        const uint8_t* src;
        if (v->u_buflen) {                                   /* complete a vector split across calls */
            uint32_t cp = need - v->u_buflen;
            if (cp > bytes - pos) cp = bytes - pos;
            memcpy(v->u_buf + v->u_buflen, p + pos, cp);
            v->u_buflen += cp;
            pos += cp;
            if (v->u_buflen < need) break;
            src = v->u_buf;
        } else {
            if (bytes - pos < need) {                        /* keep the start of a split vector */
                memcpy(v->u_buf, p + pos, bytes - pos);
                v->u_buflen = bytes - pos;
                pos = bytes;
                break;
            }
            src = p + pos;
        }
        uint32_t d[4];
        upk_decode(v, src, d);
        upk_write(v, d);
        v->u_in--;
        if (src == v->u_buf) {                               /* keep V3 look-ahead bytes */
            v->u_buflen -= vb;
            memmove(v->u_buf, v->u_buf + vb, v->u_buflen);
        } else {
            pos += vb;
        }
    }
    if (!v->u_in) {
        v->u_buflen = 0;                                     /* rest of the last word is padding */
        upk_fills(v);
        if (!v->left) cmd_done(v);
    }
    return take;
}

/* ---- MPG ----------------------------------------------------------------------------------------- */

static uint32_t mpg_data(vif_state* v, const uint32_t* w, uint32_t n) {
    uint32_t take = n < v->left ? n : v->left;
    uint8_t* mem = vu_micro_mem(v->id);
    const uint8_t* src = (const uint8_t*)w;
    for (uint32_t bytes = take * 4; bytes;) {
        uint32_t off = v->pos & (v->micro_size - 1), c = v->micro_size - off;
        if (c > bytes) c = bytes;
        memcpy(mem + off, src, c);
        v->pos += c;
        src += c;
        bytes -= c;
    }
    v->left -= take;
    v->num = ((v->left + 1) / 2) & 0xFF;
    if (!v->left) cmd_done(v);
    return take;
}

/* ---- DIRECT / DIRECTHL ---------------------------------------------------------------------------- */

static uint32_t path2_send(const uint32_t* w, uint32_t qwc) {
    if (((uintptr_t)w & 15) == 0) return gif_path2(w, qwc);
    static _Alignas(16) uint32_t bounce[4 * 64];             /* misaligned DIRECT data */
    uint32_t done = 0;
    while (done < qwc) {
        uint32_t c = qwc - done > 64 ? 64 : qwc - done;
        memcpy(bounce, w + done * 4, c * 16);
        uint32_t a = gif_path2(bounce, c);
        done += a;
        if (a < c) break;
    }
    return done;
}

static uint32_t direct_data(vif_state* v, const uint32_t* w, uint32_t n) {
    uint32_t used = 0;
    v->stat &= ~ST_VGW;
    for (;;) {
        if (v->dq_n == 4) {
            if (!gif_path2(v->dq, 1)) { v->stat |= ST_VGW; return used; }
            v->dq_n = 0;
        }
        if (!v->left) break;
        uint32_t avail = n - used;
        if (avail > v->left) avail = v->left;
        if (!avail) return used;                             /* waiting for data */
        if (v->dq_n || avail < 4) {                          /* gather a quadword */
            while (v->dq_n < 4 && avail) { v->dq[v->dq_n++] = w[used++]; v->left--; avail--; }
            continue;
        }
        uint32_t q = avail / 4, a = path2_send(w + used, q);
        used += a * 4;
        v->left -= a * 4;
        if (a < q) { v->stat |= ST_VGW; return used; }
    }
    cmd_done(v);
    return used;
}

/* ---- command decoder ------------------------------------------------------------------------------ */

/* Data words for the command in progress; returns words consumed. */
static uint32_t cmd_data(vif_state* v, const uint32_t* w, uint32_t n) {
    switch (v->kind) {
    case K_STMASK:
        if (!n) return 0;
        v->mask = w[0];
        cmd_done(v);
        return 1;
    case K_STROW:
    case K_STCOL: {
        uint32_t* dst = v->kind == K_STROW ? v->r : v->c, k = 0;
        while (k < n && v->left) { dst[v->pos++ & 3] = w[k++]; v->left--; }
        if (!v->left) cmd_done(v);
        return k;
    }
    case K_MPG: return mpg_data(v, w, n);
    case K_UNPACK: return upk_data(v, w, n);
    case K_DIRECT: return direct_data(v, w, n);
    default: return 0;
    }
}

/* Start the command `code`. Returns 0 if it has to wait (VU busy) and was not taken. */
static int cmd_begin(vif_state* v, uint32_t code) {
    uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    int vif1 = v->id == 1;

    if (cmd >= CMD_UNPACK) {
        if ((cmd & 3) == 3 && (cmd & 0xC) != 0xC) goto invalid;   /* only V4-5 uses vl = 3 */
        v->code = code;
        upk_begin(v, code);
        return 1;
    }
    switch (cmd) {
    case CMD_NOP: break;
    case CMD_STCYCL: v->cycle = imm; break;
    case CMD_OFFSET:
        if (!vif1) goto invalid;
        v->ofst = imm & 0x3FF;
        v->stat &= ~ST_DBF;
        v->tops = v->base;
        break;
    case CMD_BASE:
        if (!vif1) goto invalid;
        v->base = imm & 0x3FF;
        break;
    case CMD_ITOP: v->itops = imm & 0x3FF; break;
    case CMD_STMOD: v->mode = imm & 3; break;
    case CMD_MSKPATH3:
        if (!vif1) goto invalid;
        gif_set_path3_mask((imm >> 15) & 1);
        break;
    case CMD_MARK:
        /* (r2-flicker) the modern renderer keeps a native mesh's chain (its uploads, VIF registers and double-buffer
         * flips as in an all-exact frame) but not its programs: MARK 0xA5A5 then MARK n (not visible to the EE) */
        if (vif1 && v->r2_arm) { v->r2_norun = v->r2_arm == 2; v->r2_arm = 0; v->r2_skip = imm; break; }
        if (vif1 && (imm == 0xA5A5u || imm == 0xA5A6u)) { v->r2_arm = imm == 0xA5A6u ? 2 : 1; break; }
        v->mark = imm; v->stat |= ST_MRK; break;
    case CMD_FLUSH:
    case CMD_FLUSHA:
        if (!vif1) goto invalid;
        /* fall through: PATH1/2/3 are idle between calls in the synchronous model */
    case CMD_FLUSHE:
        if (vu_running(v)) return 0;
        break;
    case CMD_MSCALF:
        if (!vif1) goto invalid;
        /* fall through */
    case CMD_MSCAL:
    case CMD_MSCNT:
        if (vu_running(v)) return 0;
        v->code = code;
        if (vif1) {                                          /* (r2-flicker) a kept native chain's runs: no packets */
            vu1_drop_kicks = v->r2_skip != 0;
            if (v->r2_skip && v->r2_norun) {                 /* (no program: the flip, TOP / ITOP as micro_start) */
                v->r2_skip--;
                v->itop = v->itops;
                v->top = v->tops;
                v->stat ^= ST_DBF;
                v->tops = (v->base + ((v->stat & ST_DBF) ? v->ofst : 0)) & 0x3FF;
                vu_set_vif_tops(1, v->top, v->itop);
                break;
            }
            if (v->r2_skip) v->r2_skip--;
        }
        micro_start(v, cmd, imm);
        break;
    case CMD_STMASK: v->kind = K_STMASK; v->left = 1; break;
    case CMD_STROW: v->kind = K_STROW; v->left = 4; v->pos = 0; break;
    case CMD_STCOL: v->kind = K_STCOL; v->left = 4; v->pos = 0; break;
    case CMD_MPG:
        if (vu_running(v)) return 0;                         /* MPG waits for the VU to end */
        v->kind = K_MPG;
        v->left = (num ? num : 256) * 2;
        v->pos = imm * 8;
        v->num = num;
        break;
    case CMD_DIRECT:
    case CMD_DIRECTHL:
        if (!vif1) goto invalid;
        v->kind = K_DIRECT;
        v->left = (imm ? imm : 65536) * 4;
        v->dq_n = 0;
        break;
    default: goto invalid;
    }
    v->code = code;
    if (v->kind == K_NONE) cmd_done(v);
    return 1;

invalid:
    v->code = code;
    {
        static int logged[2];
        log_once(&logged[v->id], "invalid VIFcode 0x%08X on VIF%u (further ones not logged)", code, (uint32_t)v->id);
    }
    if (!(v->err & ERR_ME1)) {                               /* ER1: stall until STC */
        v->stat |= ST_ER1;
        raise_irq(v);
    }
    return 1;
}

/* Run the command processor over n words; returns words consumed (stops early at a stall). */
static uint32_t feed(vif_state* v, const uint32_t* w, uint32_t n) {
    int perf_prev = perf_enter(PERF_VIF);                    /* PS2_PERF: VIF time (VU/GS nest inside) */
    uint32_t i = 0;
    for (;;) {
        if (v->stat & ST_STALL) break;
        if (v->kind != K_NONE) {
            i += cmd_data(v, w + i, n - i);
            if (v->kind != K_NONE) break;                    /* needs more data, or the GIF is busy */
            continue;
        }
        if (i == n) break;
        uint32_t code = w[i];
        if ((v->stat & ST_INT) && ((code >> 24) & 0x7F) != CMD_MARK) {   /* i bit: stall before it */
            v->stat |= ST_VIS;
            break;
        }
        if (!cmd_begin(v, code)) {                           /* waits for the VU: it is the command */
            v->code = code;                                  /* being processed (CODE, VPS = 2) */
            v->vu_wait = 1;
            break;
        }
        v->vu_wait = 0;
        i++;
    }
    perf_leave(perf_prev);
    return i;
}

/* ---- FIFO ------------------------------------------------------------------------------------------ */

static void fifo_push(vif_state* v, const uint32_t* w, uint32_t n) {
    if (v->fifo_rd + v->fifo_n + n > FIFO_WORDS) {
        memmove(v->fifo, v->fifo + v->fifo_rd, v->fifo_n * 4);
        v->fifo_rd = 0;
    }
    if (v->fifo_n + n > FIFO_WORDS) {
        static int logged;
        log_once(&logged, "VIF%u FIFO overflow while stalled (STAT 0x%08X): data dropped", (uint32_t)v->id, v->stat);
        n = FIFO_WORDS - v->fifo_n;
    }
    memcpy(v->fifo + v->fifo_rd + v->fifo_n, w, n * 4);
    v->fifo_n += n;
}

static void fifo_drain(vif_state* v) {
    if (!v->fifo_n) return;
    uint32_t k = feed(v, v->fifo + v->fifo_rd, v->fifo_n);
    v->fifo_rd += k;
    v->fifo_n -= k;
    if (!v->fifo_n) v->fifo_rd = 0;
}

/* Retry work that may be waiting on another unit (GIF, VU) and leftover FIFO words. */
static void resume(vif_state* v) {
    static const uint32_t none[1];
    if (v->stat & ST_STALL) return;
    if (v->kind == K_DIRECT && v->dq_n == 4) feed(v, none, 0);   /* a quadword the GIF refused */
    fifo_drain(v);
}

/* ---- DMA entry points ------------------------------------------------------------------------------ */

__attribute__((weak)) volatile int vu_ab_phase = -1;   /* (r2-flicker: vu.c's) */
__attribute__((weak)) double vu_ab_ms[2][2];
__attribute__((weak)) double vu_ab_now(void) { return 0; }
static uint32_t vif_dma_impl(int vif, const void* qw, uint32_t qwc);
uint32_t vif_dma(int vif, const void* qw, uint32_t qwc) {
    int abp = vif == 1 ? vu_ab_phase : -1;
    if (abp < 0) return vif_dma_impl(vif, qw, qwc);
    double t0 = vu_ab_now();
    uint32_t r = vif_dma_impl(vif, qw, qwc);
    vu_ab_ms[abp & 1][1] += vu_ab_now() - t0;
    return r;
}
static uint32_t vif_dma_impl(int vif, const void* qw, uint32_t qwc) {
    vif_state* v = &vifs[vif & 1];
    resume(v);
    if (v->fifo_n) return 0;
    const uint32_t* w = (const uint32_t*)qw;
    uint32_t n = qwc * 4, k = feed(v, w, n);
    if (k == n) return qwc;
    if (k & 3) {                                             /* rest of the quadword waits in the FIFO */
        fifo_push(v, w + k, 4 - (k & 3));
        return k / 4 + 1;
    }
    return k / 4;
}

int vif_dma_tag(int vif, const void* tag_qw) {
    vif_state* v = &vifs[vif & 1];
    resume(v);
    if (v->fifo_n) return 0;
    const uint32_t* w = (const uint32_t*)tag_qw + 2;         /* bits 64-127 go to the VIF */
    uint32_t k = feed(v, w, 2);
    if (k == 0) return 0;
    if (k == 1) fifo_push(v, w + 1, 1);
    return 1;
}

/* (r2-flicker, diagnostics) VIF registers: stat cycle mode mask base ofst tops itop top itops, then ROW[4], COL[4] */
void vif_debug_regs(int vif, uint32_t out[18]) {
    const vif_state* v = &vifs[vif & 1];
    uint32_t a[10] = { v->stat, v->cycle, v->mode, v->mask, v->base, v->ofst, v->tops, v->itop, v->top, v->itops };
    memcpy(out, a, sizeof a);
    memcpy(out + 10, v->r, 16);
    memcpy(out + 14, v->c, 16);
}

void vif_debug_state(int vif) {                              /* (pc-fps2) diagnostics */
    const vif_state* v = &vifs[vif & 1];
    fprintf(stderr, "[vif] VIF%d: stat %08X kind %d left %u code %08X fifo %u vu_wait %d dq_n %u\n", vif, v->stat, v->kind,
            v->left, v->code, v->fifo_n, v->vu_wait, v->dq_n);
}

uint32_t vif1_read(void* qw, uint32_t qwc) {
    uint32_t bytes = gs_image_read(qw, qwc * 16), q = bytes / 16;
    if (bytes & 15) {                                        /* pad a final partial quadword */
        memset((uint8_t*)qw + bytes, 0, 16 - (bytes & 15));
        q++;
    }
    if (!q && qwc) {
        static int logged;
        log_once(&logged, "VIF1 FIFO read (%u qwords, STAT.FDR=%u): GS has no data to send", qwc,
                 (vifs[1].stat & ST_FDR) ? 1u : 0u);
    }
    return q;
}

/* ---- registers ------------------------------------------------------------------------------------- */

static void vif_reset(vif_state* v) {
    int id = v->id;
    uint32_t qmask = v->qmask, micro = v->micro_size;
    memset(v, 0, sizeof *v);
    v->id = id;
    v->qmask = qmask;
    v->micro_size = micro;
    if (id == 1) gif_set_path3_mask(0);
}

static uint32_t stat_read(vif_state* v) {
    uint32_t s = v->stat;
    if (v->kind != K_NONE) s |= (v->left ? ST_VPS_WAIT : ST_VPS_XFER);
    if (vu_running(v)) {
        s |= ST_VEW;
        if (v->kind == K_NONE && v->vu_wait) s |= ST_VPS_DECODE;
    }
    uint32_t fqc = (v->fifo_n + 3) / 4, cap = v->id ? 16 : 8;
    s |= (fqc < cap ? fqc : cap) << 24;
    return s;
}

static uint32_t* reg_ptr(vif_state* v, uint32_t off) {
    switch (off) {
    case 0x20: return &v->err;
    case 0x30: return &v->mark;
    case 0x40: return &v->cycle;
    case 0x50: return &v->mode;
    case 0x60: return &v->num;
    case 0x70: return &v->mask;
    case 0x80: return &v->code;
    case 0x90: return &v->itops;
    case 0xA0: return v->id ? &v->base : NULL;
    case 0xB0: return v->id ? &v->ofst : NULL;
    case 0xC0: return v->id ? &v->tops : NULL;
    case 0xD0: return &v->itop;
    case 0xE0: return v->id ? &v->top : NULL;
    case 0x100: case 0x110: case 0x120: case 0x130: return &v->r[(off - 0x100) >> 4];
    case 0x140: case 0x150: case 0x160: case 0x170: return &v->c[(off - 0x140) >> 4];
    default: return NULL;
    }
}

static void fifo_access(vif_state* v, uint32_t phys, void* data, unsigned size, int is_write) {
    if (!is_write) {
        memset(data, 0, size);
        if (v->id == 1) {                                    /* EE reading GS download data */
            uint8_t q[16];
            if (vif1_read(q, 1)) memcpy(data, q, size < 16 ? size : 16);
            if (size != 16) hw_log_unhandled("vif", phys, data, size, 0);
        } else {
            hw_log_unhandled("vif", phys, data, size, 0);
        }
        return;
    }
    if (size < 4) { hw_log_unhandled("vif", phys, data, size, 1); return; }
    uint32_t w[4];
    memcpy(w, data, size);
    fifo_push(v, w, size / 4);
    resume(v);
}

void vif_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if ((phys >= 0x10004000u ? (phys >> 12) : (phys >> 10)) & 1) gfx_sync();   /* (pc-fps2) VIF1: the worker's */
    if (phys >= 0x10004000u) {                               /* FIFOs: 0x10004000 VIF0, 0x10005000 VIF1 */
        fifo_access(&vifs[(phys >> 12) & 1], phys, data, size, is_write);
        return;
    }
    vif_state* v = &vifs[(phys >> 10) & 1];                  /* 0x10003800 VIF0, 0x10003C00 VIF1 */
    uint32_t off = phys & 0x3F0, sub = phys & 0xF;

    if (!is_write) {
        uint32_t val = 0;
        resume(v);                                           /* a polling game lets waiting work progress */
        if (off == 0x00) val = stat_read(v);
        else if (off == 0x10) val = 0;                       /* FBRST is write-only */
        else {
            uint32_t* r = reg_ptr(v, off);
            if (r) val = *r;
            else hw_log_unhandled("vif", phys, data, size, 0);
        }
        uint8_t q[16] = { 0 };
        memcpy(q, &val, 4);
        memset(data, 0, size);
        memcpy(data, q + sub, sub + size <= 16 ? size : 16 - sub);
        return;
    }

    if (sub) { hw_log_unhandled("vif", phys, data, size, 1); return; }
    uint32_t val = 0;
    memcpy(&val, data, size < 4 ? size : 4);
    switch (off) {
    case 0x00:                                               /* STAT: only FDR (VIF1) is writable */
        if (v->id) v->stat = (v->stat & ~ST_FDR) | (val & ST_FDR);
        break;
    case 0x10:                                               /* FBRST */
        if (val & 1) vif_reset(v);
        if (val & 2) v->stat |= ST_VFS;
        if (val & 4) {
            if (v->kind != K_NONE) v->stop_req = 1;
            else v->stat |= ST_VSS;
        }
        if (val & 8) {
            v->stat &= ~(ST_VSS | ST_VFS | ST_VIS | ST_INT | ST_ER0 | ST_ER1);
            v->stop_req = 0;
            resume(v);
        }
        break;
    case 0x20: v->err = val & 7; break;
    case 0x30: v->mark = val & 0xFFFF; v->stat &= ~ST_MRK; break;
    case 0x60: case 0x80:                                    /* NUM, CODE: read-only */
        hw_log_unhandled("vif", phys, data, size, 1);
        break;
    default: {
        /* Read-only for the EE on hardware except while the VIF is stopped (debug use); accept. */
        uint32_t* r = reg_ptr(v, off);
        if (!r) { hw_log_unhandled("vif", phys, data, size, 1); break; }
        if (off == 0x40) val &= 0xFFFF;
        else if (off == 0x50) val &= 3;
        else if (off >= 0x90 && off <= 0xE0) val &= 0x3FF;
        *r = val;
        break;
    }
    }
}
