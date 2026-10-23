/* GIF: the GS interface unit.
 *
 * Three paths feed the GS through the GIF. Each has its own GIFtag state machine, so a packet may
 * arrive split across any number of calls:
 *   PATH1  VU1 XGKICK        gif_path1_kick: reads VU1 data memory until the packet's EOP tag is done
 *   PATH2  VIF1 DIRECT(HL)   gif_path2
 *   PATH3  DMA channel 2     gif_path3_dma, and EE stores to the GIF FIFO at 0x10006000
 *
 * Synchronous model (hw.h): data handed to a path is turned into gs_write_reg / gs_image_write
 * calls before the call returns. Only PATH3 ever stalls, and only at a packet boundary while it is
 * masked (GIF_MODE.M3R or VIF1 MSKPATH3) or paused (GIF_CTRL.PSE). A packet already in progress
 * always completes first, as on hardware. EE stores to the FIFO while PATH3 is blocked wait in the
 * 16-qword FIFO (GIF_STAT.FQC) until it is released.
 *
 * GIFtag (ps2tek "GIF"; Sony GS User's Manual, GIFtag):
 *   bits 0-14 NLOOP, 15 EOP, 46 PRE, 47-57 PRIM, 58-59 FLG (0 PACKED, 1 REGLIST, 2 IMAGE,
 *   3 disabled = IMAGE), 60-63 NREG (0 = 16), 64-127 REGS (4-bit descriptors, lowest first).
 *   NLOOP = 0: everything but EOP is ignored. PRE/PRIM applies to PACKED tags only. Every GIFtag
 *   resets the packed-mode Q to 1.0f.
 *
 * PACKED descriptors (input bit positions within the 128-bit qword -> GS register):
 *   0 PRIM    0-10                                        -> PRIM
 *   1 RGBAQ   R 0-7, G 32-39, B 64-71, A 96-103; Q from the last packed ST in this tag (else 1.0f)
 *   2 ST      S 0-31, T 32-63 -> ST; Q 64-95 is kept for the next RGBAQ
 *   3 UV      U 0-13, V 32-45                             -> UV (U 0-13, V 16-29)
 *   4 XYZF2   X 0-15, Y 32-47, Z 68-91, F 100-107, ADC 111 -> XYZF2, or XYZF3 when ADC = 1
 *   5 XYZ2    X 0-15, Y 32-47, Z 64-95, ADC 111           -> XYZ2, or XYZ3 when ADC = 1
 *   A FOG     F 100-107                                   -> FOG (F 56-63)
 *   E A+D     data 0-63 to the GS register whose address is in bits 64-71
 *   F NOP     nothing
 *   6-9 (TEX0_1/2, CLAMP_1/2), B, C (XYZF3), D (XYZ3): bits 0-63 as-is to that register number.
 * REGLIST: one 64-bit word per descriptor straight to that register (A+D and NOP output nothing);
 *   when NLOOP*NREG is odd the last qword's upper half is padding.
 * IMAGE: NLOOP qwords to gs_image_write. A+D writes to HWREG (0x54) are image data as well.
 *
 * Debugging: PS2_LOG_GIF=1 prints every GIFtag (first 100000) to stderr.
 */
#include "hw.h"
#include "perf.h"
#include "gfx_async.h"
#include "gif_parse.h"

#include <stdlib.h>

enum { FLG_PACKED = 0, FLG_REGLIST = 1, FLG_IMAGE = 2 };
enum { GS_PRIM = 0x00, GS_RGBAQ = 0x01, GS_ST = 0x02, GS_UV = 0x03, GS_XYZF2 = 0x04, GS_XYZ2 = 0x05,
       GS_FOG = 0x0A, GS_XYZF3 = 0x0C, GS_XYZ3 = 0x0D, GS_HWREG = 0x54 };
enum { FEED_PATH2, FEED_PATH1, FEED_PATH3 };   /* how gif_feed() decides to stop early */

#define Q_ONE     0x3F800000u                  /* 1.0f */
#define VU1_QWC   1024u                        /* VU1 data memory: 16 KB */
#define PATH1_MAX 2048u                        /* a kick longer than this never had an EOP */

typedef struct {
    uint64_t regs;          /* REGS of the current tag */
    uint32_t nloop;         /* loops left in the current tag; 0 = the next qword is a GIFtag */
    uint32_t nreg;          /* descriptors per loop, 1-16 */
    uint32_t reg;           /* index of the next descriptor in the current loop */
    uint32_t flg;           /* FLG_*, with 3 folded into FLG_IMAGE */
    uint32_t q;             /* packed-mode Q (float bits) */
    uint32_t tag_lo;        /* bits 0-31 of the current tag (GIF_P3TAG) */
    uint8_t  eop;           /* the current tag ends the packet */
    uint8_t  in_packet;     /* a packet has started and its EOP tag has not completed */
} gif_path;

static gif_path paths[4];                      /* indexed by path number; [0] unused */
static uint32_t mode_m3r, mode_imt;            /* GIF_MODE */
static uint32_t ctrl_pse;                      /* GIF_CTRL.PSE */
static uint32_t vif_m3p;                       /* VIF1 MSKPATH3 */
static uint32_t p3_waiting;                    /* PATH3 DMA was refused at a packet boundary */
static uint8_t  fifo[16][16];                  /* EE stores waiting for PATH3 */
static uint32_t fifo_count;
static uint8_t  fifo_part[16];                 /* a qword being assembled from narrower stores */
static uint64_t last_tag[2];                   /* GIF_TAG0-3 */
static int      last_path = 3;                 /* path of the last tag read (GIF_CNT) */
static uint32_t vu_addr;                       /* PATH1 read address in qwords (GIF_CNT.VUADDR) */
static int      log_tags = -1;                 /* PS2_LOG_GIF; -1 = not read yet */
static uint32_t tags_logged;

static inline int path3_blocked(void) { return (mode_m3r | vif_m3p | ctrl_pse) != 0; }

/* ---- GIFtag ------------------------------------------------------------------------------------ */

static void log_tag(int path, uint64_t lo, uint64_t hi) {
    if (log_tags < 0) log_tags = getenv("PS2_LOG_GIF") != NULL;
    if (!log_tags) return;
    if (tags_logged >= 100000) return;
    static const char* const fmt[4] = { "PACKED", "REGLIST", "IMAGE", "IMAGE(3)" };
    fprintf(stderr, "[gif] PATH%d tag nloop=%u eop=%u %s nreg=%u regs=%016llX pre=%u prim=0x%03X%s\n", path,
            (unsigned)(lo & 0x7FFF), (unsigned)(lo >> 15) & 1, fmt[(lo >> 58) & 3], (unsigned)(lo >> 60),
            (unsigned long long)hi, (unsigned)(lo >> 46) & 1, (unsigned)(lo >> 47) & 0x7FF,
            ++tags_logged == 100000 ? " (log limit reached)" : "");
}

/* (pc-fps2) quiet = 1: a check run on a copy of the path state (PS2_GIF_CHECK), no GIF register side effects */
static inline __attribute__((always_inline)) void read_tag_t(gif_path* p, int path, const uint8_t* qw, gifp_reg_fn out, int quiet) {
    uint64_t lo, hi;
    memcpy(&lo, qw, 8);
    memcpy(&hi, qw + 8, 8);
    uint32_t flg = (uint32_t)(lo >> 58) & 3, nreg = (uint32_t)(lo >> 60);
    p->nloop = (uint32_t)lo & 0x7FFF;
    p->eop = (uint8_t)((lo >> 15) & 1);
    p->flg = flg > FLG_IMAGE ? FLG_IMAGE : flg;
    p->nreg = nreg ? nreg : 16;
    p->regs = hi;
    p->reg = 0;
    p->q = Q_ONE;
    p->tag_lo = (uint32_t)lo;
    p->in_packet = !(p->nloop == 0 && p->eop);
    if (!quiet) {
        last_tag[0] = lo;
        last_tag[1] = hi;
        last_path = path;
        if (log_tags) log_tag(path, lo, hi);
    }
    if (p->nloop && flg == FLG_PACKED && ((lo >> 46) & 1)) out(GS_PRIM, (lo >> 47) & 0x7FF);
}

/* ---- data (conversions: gif_parse.h) ------------------------------------------------------------ */

/* Run qwords through one path's state machine; returns how many were consumed.
 * FEED_PATH1 stops right after the packet's EOP tag completes; FEED_PATH3 stops at a packet
 * boundary while PATH3 is masked or paused; FEED_PATH2 consumes everything. */
static inline __attribute__((always_inline)) uint32_t feed_t(gif_path* p, int path, const uint8_t* src, uint32_t qwc, int how,
                                                             gifp_reg_fn out, gifp_img_fn img, int quiet) {
    uint32_t i = 0;
    while (i < qwc) {
        const uint8_t* qw = src + (size_t)i * 16;
        if (p->nloop == 0) {                       /* a GIFtag */
            if (how == FEED_PATH3 && !p->in_packet && path3_blocked()) break;
            read_tag_t(p, path, qw, out, quiet);
            i++;
            if (how == FEED_PATH1 && !p->in_packet) break;   /* NLOOP = 0 with EOP */
            continue;
        }
        if (p->flg == FLG_IMAGE) {
            uint32_t n = qwc - i < p->nloop ? qwc - i : p->nloop;
            img(qw, n * 16);
            p->nloop -= n;
            i += n;
        } else {
            uint64_t d0, d1;
            memcpy(&d0, qw, 8);
            memcpy(&d1, qw + 8, 8);
            if (p->flg == FLG_PACKED) {
                gifp_packed(&p->q, (uint32_t)(p->regs >> (p->reg * 4)) & 0xF, d0, d1, out, img);
                if (++p->reg == p->nreg) { p->reg = 0; p->nloop--; }
            } else {                               /* REGLIST: two registers per qword */
                for (int h = 0; h < 2; h++) {
                    uint32_t desc = (uint32_t)(p->regs >> (p->reg * 4)) & 0xF;
                    if (desc < 0xE) out(desc, h ? d1 : d0);
                    if (++p->reg == p->nreg) { p->reg = 0; if (--p->nloop == 0) break; }   /* rest is padding */
                }
            }
            i++;
        }
        if (p->nloop == 0 && p->eop) {             /* packet complete */
            p->in_packet = 0;
            if (how == FEED_PATH1) break;
        }
    }
    return i;
}

static uint32_t gif_feed_body(gif_path* p, int path, const uint8_t* src, uint32_t qwc, int how) {
    return feed_t(p, path, src, qwc, how, gs_write_reg, gs_image_write, 0);
}

/* Everything the GS draws passes through here: charged to GS time in the PS2_PERF report. */
static uint32_t gif_feed(gif_path* p, int path, const uint8_t* src, uint32_t qwc, int how) {
    int perf_prev = perf_enter(PERF_GS);
    uint32_t n = gif_feed_body(p, path, src, qwc, how);
    perf_leave(perf_prev);
    return n;
}

/* ---- PATH3 FIFO -------------------------------------------------------------------------------- */

static void fifo_drain(void) {
    if (!fifo_count) return;
    uint32_t n = gif_feed(&paths[3], 3, fifo[0], fifo_count, FEED_PATH3);
    if (!n) return;
    fifo_count -= n;
    memmove(fifo[0], fifo[n], (size_t)fifo_count * 16);
}

static void fifo_write(uint32_t off, const void* data, unsigned size) {
    if (size > 16 - off) size = 16 - off;
    memcpy(fifo_part + off, data, size);
    if (off + size < 16) return;                   /* wait for the rest of the qword */
    if (fifo_count == 16) {                        /* the EE would stall here forever */
        static int logged;
        if (!logged++) fprintf(stderr, "[gif] FIFO store while PATH3 is blocked and the FIFO is full: dropped\n");
        return;
    }
    memcpy(fifo[fifo_count++], fifo_part, 16);
    fifo_drain();
}

/* ---- paths ------------------------------------------------------------------------------------- */

uint32_t gif_path3_dma(const void* qw, uint32_t qwc) {
    fifo_drain();                                  /* earlier EE stores go first */
    uint32_t n = fifo_count ? 0 : gif_feed(&paths[3], 3, (const uint8_t*)qw, qwc, FEED_PATH3);
    p3_waiting = n < qwc;
    return n;
}

uint32_t gif_path2(const void* qw, uint32_t qwc) {
    return gif_feed(&paths[2], 2, (const uint8_t*)qw, qwc, FEED_PATH2);
}

/* (pc-fps2) A whole PATH1 packet goes to the GS thread as raw qwords and is parsed there (gs_thread.c gs_gif_packet,
 * gif_parse.h): the producer (EE or graphics worker) only finds the packet's end and copies it, instead of a queue slot
 * per register. Same register stream (one parser source; PS2_GIF_CHECK=1 compares a hash of this file's parse of every
 * packet with the GS thread's). PS2_GIF_RAW=0: register by register as before. Not while recording the GS stream or
 * logging GIFtags, and not for packets with SIGNAL / FINISH / LABEL (CSR events stay on the EE's timeline). */
extern int gs_gif_packet(const uint8_t* ring, uint32_t addr, uint32_t ring_qw, uint32_t qwc, uint64_t check) __attribute__((weak));
static int raw_path1 = -1, gif_check;
static uint64_t chk_acc;
static void chk_reg(uint32_t r, uint64_t v) { chk_acc = gifp_hash_reg(chk_acc, r, v); }
static void chk_img(const void* d, uint32_t n) { chk_acc = gifp_hash_bytes(chk_acc, d, n); }

static uint64_t path1_check_hash(const uint8_t* vu1_data, uint32_t addr, uint32_t n) {
    static _Alignas(16) uint8_t buf[PATH1_MAX * 16];
    for (uint32_t done = 0; done < n;) {
        uint32_t a = (addr + done) & (VU1_QWC - 1), c = VU1_QWC - a;
        if (c > n - done) c = n - done;
        memcpy(buf + (size_t)done * 16, vu1_data + (size_t)a * 16, (size_t)c * 16);
        done += c;
    }
    gif_path copy = paths[1];
    chk_acc = GIFP_HASH_SEED;
    feed_t(&copy, 1, buf, n, FEED_PATH1, chk_reg, chk_img, 1);
    return chk_acc;
}

static int path1_raw(const uint8_t* vu1_data, uint32_t addr) {
    gif_path* p = &paths[1];
    if (raw_path1 < 0) {
        const char* e = getenv("PS2_GIF_RAW");
        raw_path1 = !(e && *e == '0');
        gif_check = getenv("PS2_GIF_CHECK") && *getenv("PS2_GIF_CHECK") != '0';
        if (log_tags < 0) log_tags = getenv("PS2_LOG_GIF") != NULL;
    }
    if (!raw_path1 || log_tags || !gs_gif_packet || p->nloop || p->in_packet) return 0;
    uint64_t lt[2];
    uint32_t n = gifp_packet_len(vu1_data, addr, VU1_QWC, PATH1_MAX, lt);
    if (!n || !gs_gif_packet(vu1_data, addr, VU1_QWC, n, gif_check ? path1_check_hash(vu1_data, addr, n) : 0)) return 0;
    /* the path's state after the packet, as the register-by-register parse leaves it */
    uint32_t nreg = (uint32_t)(lt[0] >> 60), flg = (uint32_t)(lt[0] >> 58) & 3;
    p->nloop = 0;
    p->eop = 1;
    p->in_packet = 0;
    p->reg = 0;
    p->regs = lt[1];
    p->nreg = nreg ? nreg : 16;
    p->flg = flg > FLG_IMAGE ? FLG_IMAGE : flg;
    p->tag_lo = (uint32_t)lt[0];
    last_tag[0] = lt[0];
    last_tag[1] = lt[1];
    last_path = 1;
    vu_addr = (addr + n) & (VU1_QWC - 1);
    return 1;
}

void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) {
    gif_path* p = &paths[1];
    uint32_t addr = qw_addr & (VU1_QWC - 1), total = 0;
    if (!vu1_data) return;
    if (raw_path1) {
        int perf_prev = perf_enter(PERF_GS);
        int done = path1_raw(vu1_data, addr);
        perf_leave(perf_prev);
        if (done) return;
    }
    for (;;) {
        uint32_t len = VU1_QWC - addr;             /* up to the end of VU1 memory, and no further than the limit */
        if (len > PATH1_MAX - total) len = PATH1_MAX - total;
        uint32_t n = gif_feed(p, 1, vu1_data + (size_t)addr * 16, len, FEED_PATH1);
        addr = (addr + n) & (VU1_QWC - 1);         /* the packet wraps at the end of VU1 memory */
        total += n;
        if (!p->in_packet) break;
        if (total >= PATH1_MAX) {
            static int logged;
            if (!logged++)
                fprintf(stderr, "[gif] PATH1 packet from VU1 qword 0x%X has no EOP after %u qwords: dropped\n",
                        qw_addr & (VU1_QWC - 1), total);
            memset(p, 0, sizeof *p);
            break;
        }
    }
    vu_addr = addr;
}

void gif_set_path3_mask(int masked) {
    vif_m3p = masked != 0;
    if (!masked) fifo_drain();
}

/* ---- registers --------------------------------------------------------------------------------- */

/* GIF_STAT. Always 0 here: IP3 (nothing interrupts PATH3), P2Q/P1Q (PATH1/2 never wait) and DIR
 * (GS->EE downloads go through VIF1 and gs_image_read, not through these paths). */
static uint32_t gif_stat(void) {
    uint32_t s = mode_m3r | (vif_m3p << 1) | (mode_imt << 2) | (ctrl_pse << 3);
    if (fifo_count || p3_waiting) s |= 1u << 6;                       /* P3Q */
    /* APATH/OPH: a path whose packet has started but not ended still owns the GIF (on hardware
     * the others queue behind it). Everything else has already been transferred. */
    uint32_t apath = paths[3].in_packet ? 3 : paths[2].in_packet ? 2 : paths[1].in_packet ? 1 : 0;
    if (apath) {
        static int logged;
        if (!logged++) fprintf(stderr, "[gif] GIF_STAT read while PATH%u is inside a packet\n", apath);
        s |= (1u << 9) | (apath << 10);
    }
    s |= fifo_count << 24;                                             /* FQC */
    return s;
}

static void gif_reset(void) {
    memset(paths, 0, sizeof paths);
    fifo_count = 0;
    p3_waiting = 0;
    memset(fifo_part, 0, sizeof fifo_part);
    vu_addr = 0;
}

/* Returns 0 for an offset that is not a GIF register. */
static int reg_read(uint32_t off, uint32_t* v) {
    const gif_path* lp = &paths[last_path];
    switch (off) {
    case 0x00: case 0x10: *v = 0; return 1;                           /* GIF_CTRL, GIF_MODE: write-only */
    case 0x20: *v = gif_stat(); return 1;
    case 0x40: *v = (uint32_t)last_tag[0]; return 1;                  /* GIF_TAG0-3 */
    case 0x50: *v = (uint32_t)(last_tag[0] >> 32); return 1;
    case 0x60: *v = (uint32_t)last_tag[1]; return 1;
    case 0x70: *v = (uint32_t)(last_tag[1] >> 32); return 1;
    case 0x80: *v = (lp->nloop & 0x7FFF) | ((lp->reg & 0xF) << 16) | ((vu_addr & 0x3FF) << 20); return 1;   /* GIF_CNT */
    case 0x90: *v = paths[3].nloop & 0x7FFF; return 1;                /* GIF_P3CNT */
    case 0xA0: *v = paths[3].tag_lo; return 1;                        /* GIF_P3TAG */
    default: return 0;
    }
}

void gif_access(uint32_t phys, void* data, unsigned size, int is_write) {
    gfx_sync();                                    /* (pc-fps2) the GIF belongs to the worker while it runs */
    if (phys >= 0x10006000u) {                     /* GIF FIFO (mirrored through the 4 KB page) */
        if (is_write) { fifo_write(phys & 15, data, size); return; }
        memset(data, 0, size);                     /* downloads go through VIF1, not here */
        hw_log_unhandled("gif", phys, data, size, is_write);
        return;
    }
    uint32_t off = (phys - 0x10003000u) & ~15u, sub = phys & 15;
    uint8_t buf[16] = { 0 };
    unsigned n = size < 16 - sub ? size : 16 - sub;
    if (!is_write) {
        uint32_t v;
        memset(data, 0, size);
        if (!reg_read(off, &v)) { hw_log_unhandled("gif", phys, data, size, is_write); return; }
        memcpy(buf, &v, 4);
        memcpy(data, buf + sub, n);
        return;
    }
    memcpy(buf + sub, data, n);
    uint32_t v;
    memcpy(&v, buf, 4);
    if ((off == 0x00 || off == 0x10) && sub >= 4) return;   /* GIF_CTRL/GIF_MODE: only bits 0-31 exist */
    if (off == 0x00) {                             /* GIF_CTRL: RST, PSE */
        if (v & 1) gif_reset();
        ctrl_pse = (v >> 3) & 1;
        if (!ctrl_pse) fifo_drain();
    } else if (off == 0x10) {                      /* GIF_MODE: M3R, IMT */
        mode_m3r = v & 1;
        mode_imt = (v >> 2) & 1;
        if (!mode_m3r) fifo_drain();
    } else {
        hw_log_unhandled("gif", phys, data, size, is_write);   /* read-only or reserved */
    }
}
