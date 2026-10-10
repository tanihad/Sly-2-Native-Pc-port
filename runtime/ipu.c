/* IPU: the Emotion Engine's MPEG-1/MPEG-2 macroblock decoder.
 *
 * Registers (0x10002000): IPU_CMD (write: start a command; read: FDEC/VDEC result + BUSY in bit 63),
 * IPU_CTRL, IPU_BP, IPU_TOP. FIFOs: output at 0x10007000 (read), input at 0x10007010 (write);
 * DMA channel 4 (IPU_TO) feeds the input FIFO through ipu_dma_to(), channel 3 (IPU_FROM) drains the
 * output FIFO through ipu_dma_from().
 *
 * Input model: 8-qword FIFO plus the decoder's 2-qword internal buffer (IPU_BP.FP), i.e. up to 10
 * qwords held. The bitstream is read in memory byte order, MSB first within each byte. IPU_BP.BP
 * is the bit position inside the oldest held qword; FP + IFC is the number of held qwords, which
 * is what libraries use to rewind their DMA source when they suspend decoding.
 *
 * Commands execute synchronously as far as the data allows. A command that needs more input bits
 * (or output space) stays BUSY and resumes when ipu_dma_to()/FIFO writes add data or the output is
 * drained; each decoding step is atomic (a symbol is only consumed once all its bits are present),
 * so a command can stop and resume at any input boundary. Completion raises INTC_IPU.
 *
 * The output FIFO is modelled deeper than the hardware's 8 qwords (IPU_CTRL.OFC still reports at
 * most 8) so that a whole macroblock can be produced in one step. When the IPU produces output or
 * waits for input after a register write, it lets the DMAC retry its stalled IPU channels at once
 * (dma_service), as the hardware's DMA request lines would, instead of waiting for the next
 * periodic retry. */
#include "hw.h"
#include "ipu_internal.h"
#include "perf.h"

enum { CMD_BCLR, CMD_IDEC, CMD_BDEC, CMD_VDEC, CMD_FDEC, CMD_SETIQ, CMD_SETVQ, CMD_CSC, CMD_PACK, CMD_SETTH };

#define CTRL_RW   0x07F30000u            /* IDP 16-17, AS 20, IVF 21, QST 22, MP1 23, PCT 24-26 */
#define CTRL_RST  0x40000000u

#define IN_QW  10                        /* 8-qword FIFO + 2-qword internal buffer */
#define OUT_QW 256                       /* ring size (power of two); >= one RGB32 macroblock */

enum { STALL, DONE, FAIL };              /* result of running a command step */

static const uint8_t nonlinear_qscale[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64,
    72, 80, 88, 96, 104, 112,
};

/* default MPEG quantiser matrices (raster order), the IQ RAM's contents until SETIQ */
static const uint8_t default_intra_matrix[64] = {
     8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

static struct {
    int      ready;
    /* registers */
    uint32_t ctrl;                       /* the writable IPU_CTRL fields */
    uint32_t cbp;
    int      ecd, scd;
    uint32_t result;                     /* IPU_CMD data */
    uint32_t cmd;                        /* command being executed */
    int      busy;
    int      started;                    /* a command was written since reset */
    /* input: ring of 16 qwords, of which up to IN_QW are held */
    uint8_t  in[256];
    uint32_t in_rd, in_wr;               /* qword counters */
    uint32_t bp;                         /* bit position in qword in_rd */
    /* output */
    uint8_t  out[OUT_QW][16];
    uint32_t out_rd, out_wr;
    /* IQ / VQ RAM, thresholds (kept across IPU_CTRL.RST) */
    uint8_t  iq[2][64];                  /* intra, non-intra; raster order */
    uint16_t vq[16];
    int      th0, th1;
    int      dc_pred[3];
    uint64_t dma_qw;                     /* qwords moved by the DMA hooks (progress detection) */
    uint64_t bits_used;
} ipu;

/* state of the running command; everything before `coef` is cleared when a command starts */
typedef struct {
    int      phase;
    /* decoding parameters, latched at command start */
    int      idp, as, ivf, mp1, qst;
    int      intra, dt, qscale, cbp;
    int      dtd, sgn, dte, ofm, mbc, quant;
    /* block decoder */
    int      blk, blk_state, i, sum;
    /* byte input (SETIQ, SETVQ, CSC, PACK) and staged output */
    int      have, oqw;
    int16_t  coef[64];
    int16_t  mb16[384];                  /* RAW16 macroblock: Y 16x16, Cb 8x8, Cr 8x8 */
    uint8_t  bytes[1024];
    uint8_t  obuf[1024];
} cmd_state;
static cmd_state st;

static void log_once(const char* what, uint32_t v) {
    static const char* seen[16];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == what) return;
    if (n < 16) seen[n++] = what;
    fprintf(stderr, "[ipu] %s (0x%08X)\n", what, v);
}

/* ---- input FIFO / bitstream ------------------------------------------------------------------- */

static inline uint32_t in_count(void) { return ipu.in_wr - ipu.in_rd; }

static inline uint32_t avail(void) {
    uint32_t n = in_count();
    return n ? n * 128 - ipu.bp : 0;
}

/* Next 32 bits of the stream, first bit in bit 31; bits past the available data read as 0. */
static inline uint32_t peek(void) {
    uint32_t at = ipu.in_rd * 16 + (ipu.bp >> 3);
    uint64_t v = 0;
    for (int k = 0; k < 5; k++) v = (v << 8) | ipu.in[(at + k) & 255];
    uint32_t r = (uint32_t)(v >> (8 - (ipu.bp & 7)));
    uint32_t a = avail();
    if (a < 32) r = a ? r & ~(0xFFFFFFFFu >> a) : 0;
    return r;
}

static inline void skip(uint32_t n) {
    ipu.bp += n;
    ipu.bits_used += n;
    while (ipu.bp >= 128 && ipu.in_rd != ipu.in_wr) { ipu.in_rd++; ipu.bp -= 128; }
}

static void in_push(const uint8_t* qw) {
    memcpy(&ipu.in[(ipu.in_wr & 15) * 16], qw, 16);
    ipu.in_wr++;
}

/* Collect whole bytes into st.bytes until `need` are there; returns 0 while short of data. */
static int collect(int need) {
    uint32_t n = avail() / 8;
    while (st.have < need && n) {
        if (ipu.bp & 7) {
            st.bytes[st.have++] = (uint8_t)(peek() >> 24);
            skip(8);
        } else {
            st.bytes[st.have++] = ipu.in[(ipu.in_rd * 16 + (ipu.bp >> 3)) & 255];
            skip(8);
        }
        n--;
    }
    return st.have == need;
}

/* ---- output FIFO ------------------------------------------------------------------------------- */

static inline uint32_t out_count(void) { return ipu.out_wr - ipu.out_rd; }
static inline uint32_t out_space(void) { return OUT_QW - out_count(); }

static void out_push(const void* data, uint32_t qwc) {
    const uint8_t* p = data;
    for (uint32_t i = 0; i < qwc; i++, p += 16) memcpy(ipu.out[ipu.out_wr++ & (OUT_QW - 1)], p, 16);
}

static uint32_t out_pop(uint8_t* dst, uint32_t qwc) {
    uint32_t n = out_count() < qwc ? out_count() : qwc;
    for (uint32_t i = 0; i < n; i++, dst += 16) memcpy(dst, ipu.out[ipu.out_rd++ & (OUT_QW - 1)], 16);
    return n;
}

/* ---- state ------------------------------------------------------------------------------------ */

static void reset_dc(void) {
    int v = 1 << (7 + ((ipu.ctrl >> 16) & 3));
    ipu.dc_pred[0] = ipu.dc_pred[1] = ipu.dc_pred[2] = v;
}

static void ipu_reset(void) {
    ipu.ctrl = 0;
    ipu.cbp = 0;
    ipu.ecd = ipu.scd = 0;
    ipu.result = 0;
    ipu.busy = 0;
    ipu.started = 0;
    ipu.in_rd = ipu.in_wr = 0;
    ipu.bp = 0;
    ipu.out_rd = ipu.out_wr = 0;
    reset_dc();
    memset(&st, 0, sizeof st);
}

static void ipu_init(void) {
    ipu_vlc_init();
    memcpy(ipu.iq[0], default_intra_matrix, 64);
    memset(ipu.iq[1], 16, 64);
    ipu_reset();
    ipu.ready = 1;
}

static void latch_ctrl(void) {
    st.idp = (ipu.ctrl >> 16) & 3;
    st.as  = (ipu.ctrl >> 20) & 1;
    st.ivf = (ipu.ctrl >> 21) & 1;
    st.qst = (ipu.ctrl >> 22) & 1;
    st.mp1 = (ipu.ctrl >> 23) & 1;
}

static int qscale(int code) { return (st.qst && !st.mp1) ? nonlinear_qscale[code & 31] : 2 * (code & 31); }

/* Skip the command's FB field (bits 0-5) once enough bits are present. */
static int skip_fb(void) {
    uint32_t fb = ipu.cmd & 0x3F;
    if (avail() < fb) return 0;
    skip(fb);
    return 1;
}

/* Decode one VLC; returns STALL, FAIL or DONE (with *val and the bits consumed). */
static int get_vlc(int table, int* val) {
    const ipu_vlc_table* t = &ipu_vlc_tables[table];
    uint32_t a = avail();
    ipu_vlc_entry e = ipu_vlc_get(t, peek());
    if (!e.len) return a < (uint32_t)t->max_len ? STALL : FAIL;
    if (e.len > a) return STALL;
    skip(e.len);
    *val = e.val;
    return DONE;
}

/* Read n (<= 24) fixed bits. */
static int get_bits(int n, int* val) {
    if (avail() < (uint32_t)n) return STALL;
    *val = n ? (int)(peek() >> (32 - n)) : 0;
    skip((uint32_t)n);
    return DONE;
}

/* ---- block decoding (BDEC, IDEC) ---------------------------------------------------------------- */

enum { BLK_NEXT, BLK_DC, BLK_AC };

static int16_t* block_dst(int b, int* stride) {
    if (b >= 4) { *stride = 8; return st.mb16 + 256 + (b - 4) * 64; }
    if (st.dt) { *stride = 32; return st.mb16 + (b >> 1) * 16 + (b & 1) * 8; }   /* field DCT */
    *stride = 16;
    return st.mb16 + (b >> 1) * 128 + (b & 1) * 8;
}

static inline int sat2048(int v) { return v < -2048 ? -2048 : v > 2047 ? 2047 : v; }

/* Decode the blocks selected by st.cbp into st.mb16 (zeroed beforehand). */
static int decode_blocks(void) {
    for (;;) {
        if (st.blk_state == BLK_NEXT) {
            while (st.blk < 6 && !(st.cbp & (32 >> st.blk))) st.blk++;
            if (st.blk == 6) return DONE;
            memset(st.coef, 0, sizeof st.coef);
            st.sum = 0;
            st.i = 0;
            st.blk_state = st.intra ? BLK_DC : BLK_AC;
        }
        if (st.blk_state == BLK_DC) {
            uint32_t bits = peek(), a = avail();
            const ipu_vlc_table* t = &ipu_vlc_tables[st.blk < 4 ? IPU_VLC_DC_LUMA : IPU_VLC_DC_CHROMA];
            ipu_vlc_entry e = ipu_vlc_get(t, bits);
            if (!e.len) return a < 32 ? STALL : FAIL;
            int size = e.val;
            if ((uint32_t)(e.len + size) > a) return STALL;
            int diff = 0;
            if (size) {
                int v = (int)((bits << e.len) >> (32 - size));
                diff = (v >> (size - 1)) ? v : v - (1 << size) + 1;
            }
            skip((uint32_t)(e.len + size));
            int c = st.blk < 4 ? 0 : st.blk - 3;
            ipu.dc_pred[c] += diff;
            int dc = sat2048(ipu.dc_pred[c] * (st.mp1 ? 8 : 8 >> st.idp));
            st.coef[0] = (int16_t)dc;
            st.sum = dc;
            st.i = 1;
            st.blk_state = BLK_AC;
        }
        /* AC coefficients (all coefficients of a non-intra block) up to end of block */
        const uint8_t* scan = ipu_scan[st.as];
        const uint8_t* w = ipu.iq[!st.intra];
        int table = st.intra ? st.ivf : 0;
        for (;;) {
            uint32_t bits = peek(), a = avail();
            int run, level;
            int len = ipu_dct_symbol(bits, table, !st.intra && st.i == 0, st.mp1, &run, &level);
            if (!len) return a < 32 ? STALL : FAIL;
            if ((uint32_t)len > a) return STALL;
            skip((uint32_t)len);
            if (run < 0) break;                          /* end of block */
            st.i += run;
            if (st.i > 63) return FAIL;
            int pos = scan[st.i++];
            int k = st.intra ? 0 : level > 0 ? 1 : -1;
            int v = (2 * level + k) * w[pos] * st.qscale / 32;
            if (st.mp1 && v && !(v & 1)) v -= v > 0 ? 1 : -1;   /* MPEG-1 oddification */
            v = sat2048(v);
            st.coef[pos] = (int16_t)v;
            st.sum += v;
        }
        if (!st.mp1 && !(st.sum & 1)) st.coef[63] ^= 1;   /* MPEG-2 mismatch control */
        int stride;
        int16_t* dst = block_dst(st.blk, &stride);
        ipu_idct(st.coef, dst, stride);
        st.blk++;
        st.blk_state = BLK_NEXT;
    }
}

static void start_blocks(void) {
    memset(st.mb16, 0, sizeof st.mb16);
    st.blk = 0;
    st.blk_state = BLK_NEXT;
}

/* ---- commands ---------------------------------------------------------------------------------- */

static int cmd_fdec(void) {
    if (st.phase == 0) { if (!skip_fb()) return STALL; st.phase = 1; }
    if (avail() < 32) return STALL;
    ipu.result = peek();
    return DONE;
}

static int cmd_vdec(void) {
    if (st.phase == 0) { if (!skip_fb()) return STALL; st.phase = 1; }
    int tbl = (ipu.cmd >> 26) & 3, table;
    switch (tbl) {
    case 0: table = IPU_VLC_MBAI; break;
    case 1: {
        int pct = (ipu.ctrl >> 24) & 7;
        if (pct < 1 || pct > 4) { ipu.result = 0; return FAIL; }
        table = IPU_VLC_MBT_I + pct - 1;
        break;
    }
    case 2: table = IPU_VLC_MOTION; break;
    default: table = IPU_VLC_DMV; break;
    }
    uint32_t before = avail();
    int val = 0, r = get_vlc(table, &val);
    if (r == STALL) return STALL;
    if (r == FAIL) {
        ipu.result = 0;
        if ((peek() >> 8) == 1) ipu.scd = 1;             /* a start code where a VLC was expected */
        return FAIL;
    }
    ipu.result = ((before - avail()) << 16) | (uint16_t)val;
    return DONE;
}

static int cmd_setiq(void) {
    if (st.phase == 0) { if (!skip_fb()) return STALL; st.phase = 1; st.have = 0; }
    if (!collect(64)) return STALL;
    uint8_t* m = ipu.iq[(ipu.cmd >> 27) & 1];
    for (int i = 0; i < 64; i++) m[ipu_scan[0][i]] = st.bytes[i];   /* sent in zigzag order */
    return DONE;
}

static int cmd_setvq(void) {
    if (!collect(32)) return STALL;
    for (int i = 0; i < 16; i++) ipu.vq[i] = (uint16_t)(st.bytes[2 * i] | (st.bytes[2 * i + 1] << 8));
    return DONE;
}

static int cmd_bdec(void) {
    for (;;) switch (st.phase) {
    case 0:
        if (!skip_fb()) return STALL;
        latch_ctrl();
        st.intra = (ipu.cmd >> 27) & 1;
        st.dt = (ipu.cmd >> 25) & 1;
        st.qscale = qscale((int)(ipu.cmd >> 16));
        if ((ipu.cmd >> 26) & 1) reset_dc();             /* DCR */
        start_blocks();
        st.cbp = 63;
        st.phase = st.intra ? 2 : 1;
        break;
    case 1: {                                            /* coded_block_pattern of a non-intra MB */
        int r = get_vlc(IPU_VLC_CBP, &st.cbp);
        if (r != DONE) return r;
        st.phase = 2;
        break;
    }
    case 2: {
        int r = decode_blocks();
        if (r != DONE) return r;
        ipu.cbp = (uint32_t)st.cbp;
        st.phase = 3;
        break;
    }
    default:
        if (out_space() < 48) return STALL;
        out_push(st.mb16, 48);
        return DONE;
    }
}

enum { ID_START, ID_MBAI, ID_MBT, ID_DT, ID_QSC, ID_BLOCKS, ID_OUT, ID_NEXT, ID_ALIGN, ID_FIND,
       ID_SLICE_QSC, ID_SLICE_EXTRA };

static int cmd_idec(void) {
    int v, r;
    for (;;) switch (st.phase) {
    case ID_START:
        if (!skip_fb()) return STALL;
        latch_ctrl();
        st.qscale = qscale((int)(ipu.cmd >> 16));
        st.dtd = (ipu.cmd >> 24) & 1;
        st.sgn = (ipu.cmd >> 25) & 1;
        st.dte = (ipu.cmd >> 26) & 1;
        st.ofm = (ipu.cmd >> 27) & 1;
        st.intra = 1;
        st.cbp = 63;
        reset_dc();
        st.phase = ID_MBAI;
        break;
    case ID_MBAI:                                        /* macroblock_address_increment */
        if ((r = get_vlc(IPU_VLC_MBAI, &v)) != DONE) return r;
        if (v != IPU_MBAI_STUFFING && v != IPU_MBAI_ESCAPE) st.phase = ID_MBT;
        break;
    case ID_MBT:                                         /* macroblock_type (I picture) */
        if ((r = get_vlc(IPU_VLC_MBT_I, &v)) != DONE) return r;
        st.quant = v & IPU_MB_QUANT;
        st.phase = ID_DT;
        break;
    case ID_DT:                                          /* dct_type (end of macroblock_modes) */
        st.dt = 0;
        if (st.dtd && (r = get_bits(1, &st.dt)) != DONE) return r;
        st.phase = ID_QSC;
        break;
    case ID_QSC:                                         /* quantiser_scale_code if macroblock_quant */
        if (st.quant) {
            if ((r = get_bits(5, &v)) != DONE) return r;
            st.qscale = qscale(v);
        }
        start_blocks();
        st.phase = ID_BLOCKS;
        break;
    case ID_BLOCKS: {
        if ((r = decode_blocks()) != DONE) return r;
        uint8_t* raw8 = st.bytes;
        for (int i = 0; i < 384; i++) raw8[i] = (uint8_t)(st.mb16[i] < 0 ? 0 : st.mb16[i] > 255 ? 255 : st.mb16[i]);
        ipu_csc_mb(raw8, st.obuf, st.ofm, st.dte, st.sgn, ipu.th0, ipu.th1);
        st.oqw = st.ofm ? 32 : 64;
        ipu.cbp = 63;
        st.phase = ID_OUT;
        break;
    }
    case ID_OUT:
        if (out_space() < (uint32_t)st.oqw) return STALL;
        out_push(st.obuf, (uint32_t)st.oqw);
        st.phase = ID_NEXT;
        break;
    case ID_NEXT: {                                      /* another macroblock, or 23 zero bits */
        uint32_t a = avail(), n = a < 23 ? a : 23;
        if (n && (peek() >> (32 - n))) { st.phase = ID_MBAI; break; }
        if (a < 23) return STALL;
        st.phase = ID_ALIGN;
        break;
    }
    case ID_ALIGN: {                                     /* next_start_code(): to a byte boundary */
        uint32_t k = (8 - (ipu.bp & 7)) & 7;
        if (avail() < k) return STALL;
        skip(k);
        st.phase = ID_FIND;
        break;
    }
    case ID_FIND: {
        if (avail() < 32) return STALL;
        uint32_t bits = peek();
        if ((bits >> 8) == 1) {
            uint32_t code = bits & 0xFF;
            if (code < 0x01 || code > 0xAF) { ipu.scd = 1; return DONE; }   /* not a slice: done */
            skip(32);
            st.phase = ID_SLICE_QSC;
        } else if ((bits >> 24) == 0) {
            skip(8);                                     /* zero stuffing byte */
        } else {
            return FAIL;
        }
        break;
    }
    case ID_SLICE_QSC:                                   /* slice header: quantiser_scale_code */
        if ((r = get_bits(5, &v)) != DONE) return r;
        st.qscale = qscale(v);
        reset_dc();
        st.phase = ID_SLICE_EXTRA;
        break;
    default:                                             /* extra_bit_slice / extra_information_slice */
        if (avail() < 1) return STALL;
        if (peek() >> 31) {
            if (avail() < 9) return STALL;
            skip(9);
        } else {
            skip(1);
            st.phase = ID_MBAI;
        }
        break;
    }
}

static int cmd_csc_pack(int pack) {
    for (;;) {
        if (st.phase == 0) {
            st.mbc = (int)(ipu.cmd & 0x7FF);
            st.dte = (ipu.cmd >> 26) & 1;
            st.ofm = (ipu.cmd >> 27) & 1;
            st.have = 0;
            st.phase = 1;
        }
        if (st.phase == 1) {
            if (!st.mbc) return DONE;
            if (!collect(pack ? 1024 : 384)) return STALL;
            if (pack) {
                ipu_pack_mb(st.bytes, st.obuf, !st.ofm, st.dte, ipu.vq);
                st.oqw = st.ofm ? 32 : 8;
            } else {
                ipu_csc_mb(st.bytes, st.obuf, st.ofm, st.dte, 0, ipu.th0, ipu.th1);
                st.oqw = st.ofm ? 32 : 64;
            }
            st.phase = 2;
        }
        if (out_space() < (uint32_t)st.oqw) return STALL;
        out_push(st.obuf, (uint32_t)st.oqw);
        st.mbc--;
        st.have = 0;
        st.phase = 1;
    }
}

/* Run the current command as far as possible. */
static void ipu_run(void) {
    if (!ipu.busy) return;
    int r;
    switch (ipu.cmd >> 28) {
    case CMD_BCLR:
        ipu.in_rd = ipu.in_wr;
        ipu.bp = ipu.cmd & 0x7F;
        r = DONE;
        break;
    case CMD_IDEC:  r = cmd_idec(); break;
    case CMD_BDEC:  r = cmd_bdec(); break;
    case CMD_VDEC:  r = cmd_vdec(); break;
    case CMD_FDEC:  r = cmd_fdec(); break;
    case CMD_SETIQ: r = cmd_setiq(); break;
    case CMD_SETVQ: r = cmd_setvq(); break;
    case CMD_CSC:   r = cmd_csc_pack(0); break;
    case CMD_PACK:  r = cmd_csc_pack(1); break;
    case CMD_SETTH:
        ipu.th0 = (int)(ipu.cmd & 0x1FF);
        ipu.th1 = (int)((ipu.cmd >> 16) & 0x1FF);
        r = DONE;
        break;
    default:
        log_once("unknown command", ipu.cmd);
        r = DONE;
        break;
    }
    if (r == STALL) return;
    if (r == FAIL) {
        ipu.ecd = 1;
        log_once("decode error (ECD)", ipu.cmd);
    }
    ipu.busy = 0;
    intc_raise(INTC_IPU);
}

/* Run and report whether anything changed. */
static int ipu_progress(void) {
    uint64_t bits = ipu.bits_used;
    uint32_t out = ipu.out_wr;
    int busy = ipu.busy;
    ipu_run();
    return bits != ipu.bits_used || out != ipu.out_wr || busy != ipu.busy;
}

/* Let the DMAC move data for its IPU channels now (see the header comment). */
static void kick_dma(void) {
    static int active;
    if (active) return;
    active = 1;
    for (int i = 0; i < 100000; i++) {
        uint64_t before = ipu.dma_qw;
        dma_service();
        if (ipu.dma_qw == before) break;
    }
    active = 0;
}

static void start_command(uint32_t v) {
    if (ipu.busy) log_once("command written while busy", v);
    memset(&st, 0, offsetof(cmd_state, coef));
    ipu.cmd = v;
    ipu.busy = 1;
    ipu.started = 1;
    ipu.ecd = ipu.scd = 0;
    ipu_run();
}

/* ---- register / FIFO access ---------------------------------------------------------------------- */

static uint64_t reg_read(uint32_t reg) {
    uint32_t held = in_count(), fp = held < 2 ? held : 2, ifc = held - fp;
    switch (reg) {
    case 0x00:                                           /* IPU_CMD */
        if (!ipu.started) return peek();                 /* data loaded, no command yet: the bitstream */
        return ipu.result | ((uint64_t)ipu.busy << 63);
    case 0x10: {                                         /* IPU_CTRL */
        uint32_t ofc = out_count() < 8 ? out_count() : 8;
        return ifc | (ofc << 4) | ((ipu.cbp & 63) << 8) | ((uint32_t)ipu.ecd << 14) |
               ((uint32_t)ipu.scd << 15) | ipu.ctrl | ((uint32_t)ipu.busy << 31);
    }
    case 0x20:                                           /* IPU_BP */
        return ipu.bp | (ifc << 8) | (fp << 16);
    default:                                             /* IPU_TOP */
        return peek() | ((uint64_t)(avail() < 32) << 63);
    }
}

static void ipu_access_body(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!ipu.ready) ipu_init();
    if (phys >= 0x10007000) {                            /* FIFOs */
        uint32_t off = phys & 0xFFF;
        if (off < 0x10 && !is_write) {                   /* output FIFO */
            uint8_t qw[16] = { 0 };
            out_pop(qw, 1);
            memcpy(data, qw, size < 16 ? size : 16);
            ipu_progress();
            kick_dma();
            return;
        }
        if (off >= 0x10 && off < 0x20 && is_write) {     /* input FIFO */
            if (size != 16) { hw_log_unhandled("ipu", phys, data, size, is_write); return; }
            if (in_count() >= IN_QW) { log_once("input FIFO overflow", 0); return; }
            in_push(data);
            ipu_progress();
            kick_dma();
            return;
        }
        if (!is_write) memset(data, 0, size);
        hw_log_unhandled("ipu", phys, data, size, is_write);
        return;
    }
    uint32_t reg = phys & 0xFF0, off = phys & 0xF;
    if (reg > 0x30 || off + size > 16) {
        if (!is_write) memset(data, 0, size);
        hw_log_unhandled("ipu", phys, data, size, is_write);
        return;
    }
    if (!is_write) {
        uint8_t img[16] = { 0 };
        uint64_t v = reg_read(reg);
        memcpy(img, &v, 8);
        memcpy(data, img + off, size);
        return;
    }
    if (off != 0) return;                                /* upper halves are read-only */
    uint32_t v = 0;
    memcpy(&v, data, size < 4 ? size : 4);
    if (reg == 0x00) {                                   /* IPU_CMD: start a command */
        start_command(v);
        kick_dma();
    } else if (reg == 0x10) {                            /* IPU_CTRL */
        if (v & CTRL_RST) {
            if (ipu.busy) intc_raise(INTC_IPU);          /* aborting a command interrupts */
            ipu_reset();
        }
        ipu.ctrl = v & CTRL_RW;
        kick_dma();
    }
}

/* PS2_PERF: the IPU's decoding time is charged to IPU (perf.h). */
void ipu_access(uint32_t phys, void* data, unsigned size, int is_write) {
    int perf_prev = perf_enter(PERF_IPU);
    ipu_access_body(phys, data, size, is_write);
    perf_leave(perf_prev);
}

uint32_t ipu_dma_to(const void* qw, uint32_t qwc) {
    int perf_prev = perf_enter(PERF_IPU);
    if (!ipu.ready) ipu_init();
    const uint8_t* p = qw;
    uint32_t done = 0;
    while (done < qwc) {
        if (in_count() >= IN_QW) {
            if (!ipu_progress()) break;                  /* full and nothing consumes it: stall */
            continue;
        }
        while (done < qwc && in_count() < IN_QW) { in_push(p + 16 * done); done++; }
        ipu_run();
    }
    ipu.dma_qw += done;
    perf_leave(perf_prev);
    return done;
}

uint32_t ipu_dma_from(void* qw, uint32_t qwc) {
    int perf_prev = perf_enter(PERF_IPU);
    if (!ipu.ready) ipu_init();
    uint8_t* p = qw;
    uint32_t done = 0;
    for (;;) {
        done += out_pop(p + 16 * done, qwc - done);
        if (done == qwc || !ipu_progress()) break;
    }
    ipu.dma_qw += done;
    perf_leave(perf_prev);
    return done;
}
