/* DMA controller (DMAC): ten channels with normal, source chain, destination chain and interleave
 * modes, MFIFO and stall control.
 *
 * Synchronous model (see hw.h): a channel runs as soon as CHCR.STR is set (DMAC enabled, not
 * suspended) until it finishes or its device accepts/produces less than offered. A stalled channel
 * keeps STR = 1 with MADR/QWC/TADR describing what is left, and is retried by dma_service() and
 * whenever the game touches the DMAC (a CPU polling a DMA register is time passing for the DMAC).
 * Completion clears STR and raises D_STAT.CIS for the channel.
 *
 * Reference: EE User's Manual (DMAC chapter), ps2tek "EE DMAC". SIF0/1/2 keep their registers but
 * transfer nothing: the IOP side is emulated at a higher level (sif.c).
 */
#include "hw.h"
#include "gfx_async.h"

enum { NCH = 10 };

#define CHCR_DIR  0x00000001u
#define CHCR_ASP  0x00000030u
#define CHCR_TTE  0x00000040u
#define CHCR_TIE  0x00000080u
#define CHCR_STR  0x00000100u
#define CHCR_RW   0xFFFF01FDu          /* DIR, MOD, ASP, TTE, TIE, STR, TAG */

#define CTRL_DMAE 0x001u
#define CTRL_RW   0x7FFu               /* DMAE, RELE, MFD, STS, STD, RCYC */
#define PCR_PCE   0x80000000u
#define PCR_RW    0x83FF03FFu          /* CPC, CDE, PCE */
#define ENABLE_CPND 0x00010000u        /* D_ENABLER/W: all transfers suspended */

enum { STAT_SIS = 13, STAT_MEIS = 14, STAT_BEIS = 15 };   /* D_STAT bits besides CIS 0-9 */

enum { TAG_REFE, TAG_CNT, TAG_NEXT, TAG_REF, TAG_REFS, TAG_CALL, TAG_RET, TAG_END };   /* source chain */
enum { DTAG_CNT = 0, DTAG_CNTS = 1, DTAG_END = 7 };                                   /* destination chain */

enum { M_NORMAL, M_SRC_CHAIN, M_DST_CHAIN, M_INTERLEAVE };

/* Channel registers, at base + 0x10 * index. Bit n of chan_regs = register n exists. */
enum { R_CHCR = 0, R_MADR = 1, R_QWC = 2, R_TADR = 3, R_ASR0 = 4, R_ASR1 = 5, R_SADR = 8 };
static const uint16_t chan_regs[NCH] = { 0x3F, 0x3F, 0x3F, 0x07, 0x0F, 0x07, 0x0F, 0x07, 0x107, 0x10F };
static const uint32_t reg_mask[9] = { CHCR_RW, 0xFFFFFFF0u, 0xFFFFu, 0xFFFFFFF0u, 0xFFFFFFF0u, 0xFFFFFFF0u,
                                      0, 0, 0x3FF0u };

typedef struct {
    uint32_t chcr, madr, qwc, tadr, asr[2], sadr;
    /* transfer state behind the registers */
    uint32_t il_pos;           /* interleave: qwords done in the current TQWC block */
    uint32_t budget;           /* tags this channel may still read in the current pump */
    uint8_t  tag_end;          /* chain: the transfer ends when QWC reaches 0 */
    uint8_t  ret_end;          /* the last tag was a ret with ASP 0 (it ends the chain) */
    uint8_t  refs;             /* source chain: the current data is under stall control */
    uint8_t  cnts;             /* destination chain: the current data updates D_STADR */
    uint8_t  in_ring;          /* MFIFO drain: the current data follows its tag inside the ring */
    uint8_t  tte_pend;         /* CHCR.TTE: tag_qw is still waiting for the device */
    uint8_t  waiting;          /* SIS/MEIS already raised for the current wait */
    uint8_t  tag_qw[16];
} chan;

static chan ch_[NCH];
static uint32_t d_ctrl = CTRL_DMAE;    /* the BIOS leaves the DMAC enabled */
static uint32_t d_pcr, d_sqwc, d_rbsr, d_rbor, d_stadr;
static uint32_t d_enable = 0x1201;     /* D_ENABLER value after boot */
static int pumping;
#define TAG_BUDGET (1u << 16)          /* tags per channel per pump: bounds runaway (looping) chains */

/* ---- logging (first occurrence of each event only) ---------------------------------------------- */

enum { LOG_MODE, LOG_TTE, LOG_QWC0, LOG_CALL, LOG_BUDGET, LOG_TQWC0, LOG_STS_SIF0, LOG_DTAG, LOG_BUS = 16 };

static void note(int id, const char* what, uint32_t a, uint32_t b) {
    static uint32_t seen;
    if (seen & (1u << id)) return;
    seen |= 1u << id;
    fprintf(stderr, "[dma] %s (0x%X, 0x%08X)\n", what, a, b);
}

/* ---- channel properties ------------------------------------------------------------------------- */

static int is_sif(int ch) { return ch == DMA_SIF0 || ch == DMA_SIF1 || ch == DMA_SIF2; }

static int to_device(int ch) {           /* memory -> peripheral */
    switch (ch) {
    case DMA_VIF0: case DMA_GIF: case DMA_IPU_TO: case DMA_SIF1: case DMA_SPR_TO: return 1;
    case DMA_VIF1: case DMA_SIF2: return ch_[ch].chcr & CHCR_DIR;
    default: return 0;
    }
}

static int mode_of(int ch) {
    unsigned mod = (ch_[ch].chcr >> 2) & 3;
    if (mod == 0) return M_NORMAL;
    if (mod == 1) {
        if (to_device(ch) && ch != DMA_SIF2) return M_SRC_CHAIN;
        if (ch == DMA_SPR_FROM || ch == DMA_SIF0) return M_DST_CHAIN;
    } else if (mod == 2 && (ch == DMA_SPR_FROM || ch == DMA_SPR_TO)) {
        return M_INTERLEAVE;
    }
    note(LOG_MODE, "CHCR.MOD not supported by this channel/direction, using normal mode", (uint32_t)ch, ch_[ch].chcr);
    return M_NORMAL;
}

static int runnable(int ch) {
    if (!(ch_[ch].chcr & CHCR_STR)) return 0;
    if (!(d_ctrl & CTRL_DMAE) || (d_enable & ENABLE_CPND)) return 0;
    if ((d_pcr & PCR_PCE) && !(d_pcr & (1u << (16 + ch)))) return 0;   /* priority control: CDE */
    return 1;
}

/* MFIFO: drain channel (or -1) and ring buffer addressing */
static int mfifo_drain(void) {
    unsigned mfd = (d_ctrl >> 2) & 3;
    return mfd == 2 ? DMA_VIF1 : mfd == 3 ? DMA_GIF : -1;
}
static uint32_t ring(uint32_t a) { return d_rbor | (a & d_rbsr); }
static uint32_t ring_room(uint32_t a) { return (d_rbsr + 16 - (a & d_rbsr)) >> 4; }   /* qwords to the end */
static int ring_empty_at(uint32_t a) { return (a & d_rbsr) == (ch_[DMA_SPR_FROM].madr & d_rbsr); }
static uint32_t ring_avail(uint32_t rd) {            /* qwords SPR_FROM has written ahead of rd, up to the end */
    uint32_t r = rd & d_rbsr, w = ch_[DMA_SPR_FROM].madr & d_rbsr;
    return (w >= r ? w - r : d_rbsr + 16 - r) >> 4;
}

/* Stall control: source (writes D_STADR) and drain (stops at D_STADR) channels, or -1 */
static int stall_source(void) {
    static const int8_t src[4] = { -1, DMA_SIF0, DMA_SPR_FROM, DMA_IPU_FROM };
    return src[(d_ctrl >> 4) & 3];
}
static int stall_drain(void) {
    static const int8_t drn[4] = { -1, DMA_VIF1, DMA_GIF, DMA_SIF1 };
    return drn[(d_ctrl >> 6) & 3];
}
static uint32_t stall_room(uint32_t addr) {          /* qwords below D_STADR the drain may read */
    uint32_t m = addr & 0x0FFFFFF0u, s = d_stadr & 0x0FFFFFF0u;
    return m < s ? (s - m) >> 4 : 0;
}

/* ---- memory ------------------------------------------------------------------------------------- */

/* Host pointer for up to `want` (> 0) qwords at a DMA address; *got = how many are contiguous
 * (the scratchpad wraps at 16 KB). NULL = bus error. */
static uint8_t* mem_span(uint32_t addr, uint32_t want, uint32_t* got) {
    uint32_t room;
    if (addr & 0x80000000u) {
        room = (PS2_SPR_SIZE - (addr & (PS2_SPR_SIZE - 16))) >> 4;
    } else {
        uint32_t a = addr & 0x0FFFFFF0u;
        if (a >= PS2_RAM_SIZE) return NULL;
        room = (PS2_RAM_SIZE - a) >> 4;
    }
    *got = want < room ? want : room;
    return hw_dma_ptr(addr, *got * 16);
}

/* Copy n qwords between memory and the scratchpad at *sadr (wrapping at 16 KB), advancing *sadr. */
static void spr_copy(uint32_t* sadr, uint8_t* mem, uint32_t n, int to_spr) {
    while (n) {
        uint32_t off = *sadr & 0x3FF0u, k = (PS2_SPR_SIZE - off) >> 4;
        if (k > n) k = n;
        uint8_t* s = hw_dma_ptr(0x80000000u | off, k * 16);
        if (s) {
            if (to_spr) memmove(s, mem, k * 16);
            else memmove(mem, s, k * 16);
        }
        mem += k * 16;
        n -= k;
        *sadr = (off + k * 16) & 0x3FF0u;
    }
}

/* ---- devices ------------------------------------------------------------------------------------ */

static uint32_t dev_out(int ch, uint8_t* qw, uint32_t n) {   /* returns qwords consumed */
    switch (ch) {
    case DMA_VIF0:   return vif_dma(0, qw, n);
    case DMA_VIF1:   return vif_dma(1, qw, n);
    case DMA_GIF:    return gif_path3_dma(qw, n);
    case DMA_IPU_TO: return ipu_dma_to(qw, n);
    case DMA_SPR_TO: spr_copy(&ch_[ch].sadr, qw, n, 1); return n;
    default:         return n;
    }
}

static uint32_t dev_in(int ch, uint8_t* qw, uint32_t n) {    /* returns qwords produced */
    switch (ch) {
    case DMA_VIF1:     return vif1_read(qw, n);
    case DMA_IPU_FROM: return ipu_dma_from(qw, n);
    case DMA_SPR_FROM: spr_copy(&ch_[ch].sadr, qw, n, 0); return n;
    default:           return 0;
    }
}

/* CHCR.TTE: the tag qword goes to the device before the tag's data. 1 = accepted. */
static int send_tag(int ch) {
    chan* c = &ch_[ch];
    if (ch == DMA_VIF0 || ch == DMA_VIF1) return vif_dma_tag(ch == DMA_VIF1, c->tag_qw);
    if (ch != DMA_SPR_TO) note(LOG_TTE, "CHCR.TTE on a non-VIF channel: tag sent as data", (uint32_t)ch, c->chcr);
    uint32_t n = dev_out(ch, c->tag_qw, 1);
    return n != 0;
}

/* ---- transfer state changes --------------------------------------------------------------------- */

static void finish(int ch) {
    chan* c = &ch_[ch];
    c->chcr &= ~CHCR_STR;
    c->tte_pend = 0;
    c->waiting = 0;
    dmac_raise(ch);
}

static void bus_error(int ch, uint32_t addr) {
    note(LOG_BUS + ch, "bus error: transfer address outside RAM/scratchpad; channel stopped", (uint32_t)ch, addr);
    ch_[ch].chcr &= ~CHCR_STR;
    ch_[ch].tte_pend = 0;
    dmac_raise(STAT_BEIS);
}

static void wait_on(chan* c, int stat_bit) {         /* stall/MFIFO-empty: raise once per wait */
    if (!c->waiting) { c->waiting = 1; dmac_raise(stat_bit); }
}

static void set_pce(uint64_t tag) {                  /* DMAtag PCE field: 2 = PCE off, 3 = PCE on */
    unsigned pce = (unsigned)(tag >> 26) & 3;
    if (pce == 2) d_pcr &= ~PCR_PCE;
    else if (pce == 3) d_pcr |= PCR_PCE;
}

/* STR 0 -> 1. Starting a chain with QWC > 0 first finishes that data as belonging to the tag held
 * in CHCR.TAG (how a stopped chain is resumed; movie players rely on it). */
static void chan_start(int ch) {
    chan* c = &ch_[ch];
    int m = mode_of(ch);
    c->tte_pend = c->waiting = 0;
    c->il_pos = 0;
    c->tag_end = c->refs = c->cnts = 0;
    c->in_ring = 1;
    if ((m == M_SRC_CHAIN || m == M_DST_CHAIN) && c->qwc) {
        unsigned id = (c->chcr >> 28) & 7;
        int irq = (c->chcr >> 31) && (c->chcr & CHCR_TIE);
        if (m == M_SRC_CHAIN) {
            c->tag_end = id == TAG_REFE || id == TAG_END || (id == TAG_RET && c->ret_end) || irq;
            c->refs = id == TAG_REFS;
            c->in_ring = !(id == TAG_REFE || id == TAG_REF || id == TAG_REFS);
        } else {
            c->tag_end = id == DTAG_END || irq;
            c->cnts = id == DTAG_CNTS;
        }
    }
    if ((m == M_NORMAL || m == M_INTERLEAVE) && !c->qwc)
        note(LOG_QWC0, "normal/interleave transfer started with QWC = 0: ends at once", (uint32_t)ch, c->madr);
}

/* ---- data transfer ------------------------------------------------------------------------------ */

static uint32_t il_limit(chan* c, uint32_t want) {   /* interleave: stay inside the TQWC block */
    uint32_t tq = (d_sqwc >> 16) & 0xFF;
    if (!tq) { note(LOG_TQWC0, "interleave with D_SQWC.TQWC = 0: no skipping", 0, d_sqwc); return want; }
    return want < tq - c->il_pos ? want : tq - c->il_pos;
}

static void il_advance(chan* c, uint32_t n) {
    uint32_t tq = (d_sqwc >> 16) & 0xFF;
    if (!tq) return;
    c->il_pos += n;
    if (c->il_pos >= tq) { c->il_pos = 0; c->madr += (d_sqwc & 0xFF) * 16; }
}

/* Memory -> device from MADR. Returns qwords moved, or -1 after a bus error. */
static int xfer_out(int ch, int m) {
    chan* c = &ch_[ch];
    int drain = m == M_SRC_CHAIN && c->in_ring && mfifo_drain() == ch;
    int stall = 0;
    if (m == M_SRC_CHAIN && c->refs && stall_drain() == ch) {
        int src = stall_source();
        if (src == DMA_SPR_FROM || src == DMA_IPU_FROM) stall = 1;
        else if (src == DMA_SIF0) note(LOG_STS_SIF0, "stall control with SIF0 as source is not emulated", (uint32_t)ch, c->madr);
    }
    uint32_t moved = 0;
    while (c->qwc) {
        uint32_t want = c->qwc, addr = c->madr, got;
        if (drain) {
            addr = c->madr = ring(addr);
            uint32_t avail = ring_avail(addr);
            if (!avail) { wait_on(c, STAT_MEIS); break; }
            if (want > avail) want = avail;
        }
        if (stall) {
            uint32_t room = stall_room(addr);
            if (!room) { wait_on(c, STAT_SIS); break; }
            if (want > room) want = room;
        }
        if (m == M_INTERLEAVE) want = il_limit(c, want);
        uint8_t* p = mem_span(addr, want, &got);
        if (!p) { bus_error(ch, addr); return -1; }
        uint32_t n = dev_out(ch, p, got);
        if (n > got) n = got;
        c->madr = addr + n * 16;
        c->qwc -= n;
        moved += n;
        if (n) c->waiting = 0;
        if (m == M_INTERLEAVE) il_advance(c, n);
        if (drain) c->madr = ring(c->madr);
        if (n < got) break;                          /* device busy: retried later */
    }
    return (int)moved;
}

/* Device -> memory at MADR. Returns qwords moved, or -1 after a bus error. */
static int xfer_in(int ch, int m) {
    chan* c = &ch_[ch];
    int mfifo = ch == DMA_SPR_FROM && mfifo_drain() >= 0;
    int stadr = stall_source() == ch && (m == M_NORMAL || (m == M_DST_CHAIN && c->cnts));
    uint32_t moved = 0;
    while (c->qwc) {
        uint32_t want = c->qwc, addr = c->madr, got;
        if (mfifo) {
            addr = ring(addr);
            uint32_t room = ring_room(addr);
            if (want > room) want = room;
        }
        if (m == M_INTERLEAVE) want = il_limit(c, want);
        uint8_t* p = mem_span(addr, want, &got);
        if (!p) { bus_error(ch, addr); return -1; }
        uint32_t n = dev_in(ch, p, got);
        if (n > got) n = got;
        c->madr = addr + n * 16;
        c->qwc -= n;
        moved += n;
        if (m == M_INTERLEAVE) il_advance(c, n);
        if (mfifo) c->madr = ring(c->madr);
        if (stadr && n) d_stadr = c->madr & 0x7FFFFFF0u;
        if (n < got) break;
    }
    return (int)moved;
}

/* ---- chains ------------------------------------------------------------------------------------- */

/* Read the source-chain tag at TADR. 1 = read, 0 = waiting (MFIFO empty), -1 = bus error. */
static int read_src_tag(int ch) {
    chan* c = &ch_[ch];
    int drain = mfifo_drain() == ch;
    uint32_t ta = c->tadr, got;
    if (drain) {
        ta = c->tadr = ring(ta);
        if (ring_empty_at(ta)) { wait_on(c, STAT_MEIS); return 0; }
    }
    const uint8_t* p = mem_span(ta, 1, &got);
    if (!p) { bus_error(ch, ta); return -1; }
    c->budget--;

    uint64_t t;
    memcpy(&t, p, 8);
    uint32_t qwc = (uint32_t)t & 0xFFFFu, id = (uint32_t)(t >> 28) & 7, addr = (uint32_t)(t >> 32) & 0xFFFFFFF0u;
    uint32_t next = ta + 16, asp = (c->chcr >> 4) & 3;
    set_pce(t);
    c->chcr = (c->chcr & 0xFFFFu) | ((uint32_t)t & 0xFFFF0000u);
    c->qwc = qwc;
    c->tag_end = c->ret_end = c->refs = 0;
    c->in_ring = 1;
    c->waiting = 0;
    switch (id) {
    case TAG_REFE: c->madr = addr; c->tadr = next; c->tag_end = 1; c->in_ring = 0; break;
    case TAG_CNT:  c->madr = next; c->tadr = next + qwc * 16; break;
    case TAG_NEXT: c->madr = next; c->tadr = addr; break;
    case TAG_REF:  c->madr = addr; c->tadr = next; c->in_ring = 0; break;
    case TAG_REFS: c->madr = addr; c->tadr = next; c->in_ring = 0; c->refs = 1; break;
    case TAG_CALL:
        c->madr = next;
        if (asp < 2) { c->asr[asp++] = next + qwc * 16; c->tadr = addr; }
        else { note(LOG_CALL, "call tag with a full address stack: chain ended", (uint32_t)ch, ta); c->tag_end = 1; }
        break;
    case TAG_RET:                                    /* ASP 2 pops ASR1, 1 pops ASR0; 0 (or 3) ends */
        c->madr = next;
        if (asp == 1 || asp == 2) c->tadr = c->asr[--asp];
        else c->tag_end = c->ret_end = 1;
        break;
    default:       c->madr = next; c->tag_end = 1; break;   /* end: TADR stays on the tag */
    }
    c->chcr = (c->chcr & ~CHCR_ASP) | (asp << 4);
    if ((t >> 31) & 1 && (c->chcr & CHCR_TIE)) c->tag_end = 1;
    if (drain) {                                     /* tags (and data following them) live in the ring */
        c->tadr = ring(c->tadr);
        if (c->in_ring) c->madr = ring(c->madr);
        else if (qwc && ring_empty_at(c->tadr)) dmac_raise(STAT_MEIS);   /* empty, but REF data still flows */
    }
    if (c->chcr & CHCR_TTE) { memcpy(c->tag_qw, p, 16); c->tte_pend = 1; }
    return 1;
}

/* Read the destination-chain tag from the device (SPR_FROM: the scratchpad at SADR). */
static int read_dst_tag(int ch) {
    chan* c = &ch_[ch];
    const uint8_t* p = hw_dma_ptr(0x80000000u | (c->sadr & 0x3FF0u), 16);
    if (!p) { bus_error(ch, c->sadr); return -1; }
    c->budget--;
    uint64_t t;
    memcpy(&t, p, 8);
    c->sadr = (c->sadr + 16) & 0x3FF0u;
    uint32_t id = (uint32_t)(t >> 28) & 7;
    if (id != DTAG_CNT && id != DTAG_CNTS && id != DTAG_END)
        note(LOG_DTAG, "undefined destination chain tag ID, treated as cnt", id, (uint32_t)t);
    set_pce(t);
    c->chcr = (c->chcr & 0xFFFFu) | ((uint32_t)t & 0xFFFF0000u);
    c->qwc = (uint32_t)t & 0xFFFFu;
    c->madr = (uint32_t)(t >> 32) & 0xFFFFFFF0u;
    c->cnts = id == DTAG_CNTS;
    c->tag_end = id == DTAG_END || (((t >> 31) & 1) && (c->chcr & CHCR_TIE));
    return 1;
}

static int run_src_chain(int ch) {
    chan* c = &ch_[ch];
    int progress = 0;
    for (;;) {
        if (c->tte_pend) {
            if (!send_tag(ch)) return progress;
            c->tte_pend = 0;
            progress = 1;
        }
        if (c->qwc) {
            int r = xfer_out(ch, M_SRC_CHAIN);
            if (r < 0) return 1;
            if (r) progress = 1;
            if (c->qwc) return progress;
        }
        if (c->tag_end) { finish(ch); return 1; }
        if (!c->budget) { note(LOG_BUDGET, "very long or looping DMA chain: continuing later", (uint32_t)ch, c->tadr); return progress; }
        ps2_poll_iop();                          /* a frame's chain can take 100+ ms: let the IOP run */
        int t = read_src_tag(ch);
        if (t < 0) return 1;
        if (!t) return progress;
        progress = 1;
    }
}

static int run_dst_chain(int ch) {
    chan* c = &ch_[ch];
    int progress = 0;
    for (;;) {
        if (c->qwc) {
            int r = xfer_in(ch, M_DST_CHAIN);
            if (r < 0) return 1;
            if (r) progress = 1;
            if (c->qwc) return progress;
        }
        if (c->tag_end) { finish(ch); return 1; }
        if (!c->budget) { note(LOG_BUDGET, "very long or looping DMA chain: continuing later", (uint32_t)ch, c->sadr); return progress; }
        ps2_poll_iop();
        if (read_dst_tag(ch) < 0) return 1;
        progress = 1;
    }
}

static int run_block(int ch, int m) {            /* normal and interleave modes */
    chan* c = &ch_[ch];
    if (c->qwc) {
        int r = to_device(ch) ? xfer_out(ch, m) : xfer_in(ch, m);
        if (r < 0) return 1;
        if (c->qwc) return r > 0;
    }
    finish(ch);
    return 1;
}

static int run(int ch) {                         /* 1 = something changed */
    int m = mode_of(ch);
    if (m == M_SRC_CHAIN) return run_src_chain(ch);
    if (m == M_DST_CHAIN) return run_dst_chain(ch);
    return run_block(ch, m);
}

/* Run every active channel until none can make progress (channels feed each other: SPR_FROM fills
 * the MFIFO ring a drain is waiting on, IPU_TO input lets IPU_FROM produce, ...). No round limit:
 * progress means a tag read (at most TAG_BUDGET per channel), a qword moved (QWC only grows again
 * through a tag read) or a channel stopping, so the loop always ends. A cap would leave channels
 * that feed each other through a small device FIFO stalled until the next retry.
 * The devices do their work (VU1 microprograms, GS drawing) inside these transfers, so a frame's
 * chain can run for 100+ ms; ps2_poll_iop() between rounds and tags keeps the IOP (sound) on time. */
static void pump(void) {
    if (pumping) return;
    pumping = 1;
    for (int ch = 0; ch < NCH; ch++)
        if (ch != DMA_VIF1 || !gfx_owned) ch_[ch].budget = TAG_BUDGET;
    for (;;) {
        int progress = 0;
        for (int ch = 0; ch < NCH; ch++) {
            /* (pc-fps2) a VIF1 chain runs on the graphics worker (gfx_async.h); the GIF waits for it (GS order) */
            if (ch == DMA_VIF1 && gfx_take_fn) {
                if (gfx_owned) continue;
                if (runnable(ch) && mode_of(ch) == M_SRC_CHAIN && gfx_take_fn(ch)) continue;
            }
            if (ch == DMA_GIF && gfx_owned && runnable(ch)) gfx_sync();
            if (runnable(ch)) progress |= run(ch);
        }
        if (!progress) break;
        ps2_poll_iop();
    }
    pumping = 0;
}

/* (pc-fps2) the graphics worker: channel ch alone, as pump() would run it (no IOP polls: ps2_poll_iop is the EE's) */
void dma_run_owned(int ch) {
    ch_[ch].budget = TAG_BUDGET;
    while (runnable(ch) && run(ch)) {}
}

void dma_service(void) { pump(); }

/* ---- register access ---------------------------------------------------------------------------- */

static uint32_t merge(uint32_t old, uint32_t phys, const void* data, unsigned size) {
    uint32_t v = 0;
    if (size >= 4) { memcpy(&v, data, 4); return v; }
    unsigned sh = (phys & 3) * 8;
    uint32_t m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
    memcpy(&v, data, size);
    return (old & ~m) | ((v << sh) & m);
}

static void put(void* data, unsigned size, uint32_t phys, uint32_t v) {
    memset(data, 0, size);
    v >>= (phys & 3) * 8;
    memcpy(data, &v, size < 4 ? size : 4);
}

static void chcr_write(int ch, uint32_t v) {
    chan* c = &ch_[ch];
    if (c->chcr & CHCR_STR) {                    /* busy: only STR can change (stop = keep the state) */
        c->chcr = (c->chcr & ~CHCR_STR) | (v & CHCR_STR);
        return;
    }
    c->chcr = v & CHCR_RW;
    if (!(c->chcr & CHCR_STR)) return;
    if (is_sif(ch)) { c->chcr &= ~CHCR_STR; return; }   /* IOP link emulated at a higher level */
    chan_start(ch);
}

static void chan_access(uint32_t phys, void* data, unsigned size, int is_write) {
    static const int8_t slot_ch[24] = { 0, -1, -1, -1, 1, -1, -1, -1, 2, -1, -1, -1,
                                        3, 4, -1, -1, 5, 6, 7, -1, 8, 9, -1, -1 };
    int ch = slot_ch[(phys - 0x10008000u) >> 10];
    uint32_t off = phys & 0x3FFu, reg = off >> 4;
    if (ch < 0 || reg > R_SADR || !((0x13Fu >> reg) & 1)) {
        if (!is_write) memset(data, 0, size);
        hw_log_unhandled("dma", phys, data, size, is_write);
        return;
    }
    if ((off & 0xF) >= 4 || !((chan_regs[ch] >> reg) & 1)) {   /* padding, or a register this channel lacks */
        if (!is_write) memset(data, 0, size);
        return;
    }
    if (ch == DMA_VIF1) gfx_sync();              /* (pc-fps2) the worker's channel: wait for it */
    chan* c = &ch_[ch];
    uint32_t* r = reg == R_CHCR ? &c->chcr : reg == R_MADR ? &c->madr : reg == R_QWC ? &c->qwc :
                  reg == R_TADR ? &c->tadr : reg == R_ASR0 ? &c->asr[0] : reg == R_ASR1 ? &c->asr[1] : &c->sadr;
    if (!is_write) {
        if (reg <= R_TADR && (c->chcr & CHCR_STR)) pump();   /* polling a busy channel: let it move */
        put(data, size, phys, *r);
        return;
    }
    uint32_t v = merge(*r, phys, data, size);
    if (reg == R_CHCR) chcr_write(ch, v);
    else *r = v & reg_mask[reg];
    pump();
}

void dma_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (phys >= 0x10008000u && phys < 0x1000E000u) { chan_access(phys, data, size, is_write); return; }

    uint32_t base = phys & ~0xFu, sub = phys & 0xFu;
    uint32_t* r = NULL;
    uint32_t mask = 0;
    switch (base) {
    case 0x1000E010u:                             /* D_STAT lives in hw.c */
        if (sub >= 4) { if (!is_write) memset(data, 0, size); return; }
        if (is_write) dmac_stat_write(merge(0, phys, data, size));   /* unwritten bytes: no effect */
        else { pump(); put(data, size, phys, dmac_stat_read()); }
        return;
    case 0x1000E000u: r = &d_ctrl;  mask = CTRL_RW;     break;
    case 0x1000E020u: r = &d_pcr;   mask = PCR_RW;      break;
    case 0x1000E030u: r = &d_sqwc;  mask = 0x00FF00FFu; break;
    case 0x1000E040u: r = &d_rbsr;  mask = 0x7FFFFFF0u; break;
    case 0x1000E050u: r = &d_rbor;  mask = 0x7FFFFFF0u; break;
    case 0x1000E060u: r = &d_stadr; mask = 0x7FFFFFF0u; break;
    case 0x1000F520u:                             /* D_ENABLER (read only) */
    case 0x1000F590u:                             /* D_ENABLEW (write only); bit 16 = suspend */
        if (sub >= 4) { if (!is_write) memset(data, 0, size); return; }
        if (!is_write) { put(data, size, phys, d_enable); return; }
        if (base == 0x1000F590u) { gfx_sync(); d_enable = merge(d_enable, phys, data, size); pump(); }
        return;
    default:
        if (!is_write) memset(data, 0, size);
        hw_log_unhandled("dma", phys, data, size, is_write);
        return;
    }
    if (sub >= 4) { if (!is_write) memset(data, 0, size); return; }
    if (!is_write) { put(data, size, phys, *r); return; }
    gfx_sync();                                   /* (pc-fps2) global DMAC state the worker's channel uses */
    *r = merge(*r, phys, data, size) & mask;
    pump();
}
