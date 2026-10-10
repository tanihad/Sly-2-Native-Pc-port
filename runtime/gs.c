/* GS privileged registers (0x12000000-0x12001FFF), CSR/IMR events and the PCRTC display output.
 *
 * Privileged registers are 64-bit at 16-byte spacing. CSR: bits 0-4 are the SIGNAL, FINISH,
 * HSINT, VSINT and EDWINT events (write 1 to clear), bit 9 RESET, bit 13 FIELD (toggles every
 * VSync), bits 14-15 FIFO status (always "empty": drawing is synchronous), REV/ID in bits 16-31.
 * IMR bits 8-12 mask the same events; an unmasked event raises INTC_GS when it is set. */
#include "gs_draw.h"
#include "perf.h"

#include <stdlib.h>

enum {
    CSR_SIGNAL = 1u << 0, CSR_FINISH = 1u << 1, CSR_HSINT = 1u << 2, CSR_VSINT = 1u << 3,
    CSR_EDWINT = 1u << 4, CSR_RESET = 1u << 9, CSR_FIELD = 1u << 13, CSR_FIFO_EMPTY = 1u << 14,
};
#define CSR_REV_ID ((0x1Bu << 16) | (0x55u << 24))

enum { P_PMODE, P_SMODE1, P_SMODE2, P_SRFSH, P_SYNCH1, P_SYNCH2, P_SYNCV, P_DISPFB1, P_DISPLAY1,
       P_DISPFB2, P_DISPLAY2, P_EXTBUF, P_EXTDATA, P_EXTWRITE, P_BGCOLOR, P_COUNT };

static uint64_t priv[P_COUNT];
static uint32_t csr_events, csr_field;
static uint32_t imr = 0x7F00;            /* all events masked */
static uint64_t busdir, siglblid;

/* ---- events ------------------------------------------------------------------------------------- */

static void set_events(uint32_t bits) {
    uint32_t fresh = bits & ~csr_events;
    csr_events |= bits;
    if (fresh & ~(imr >> 8) & 0x1F) intc_raise(INTC_GS);
}

void gs_csr_signal(uint32_t id, uint32_t mask) {
    uint32_t s = (uint32_t)siglblid;
    siglblid = (siglblid & 0xFFFFFFFF00000000ull) | ((s & ~mask) | (id & mask));
    set_events(CSR_SIGNAL);
}

void gs_csr_finish(void) { set_events(CSR_FINISH); }

void gs_csr_label(uint32_t id, uint32_t mask) {
    uint32_t l = (uint32_t)(siglblid >> 32);
    siglblid = (siglblid & 0xFFFFFFFFull) | ((uint64_t)((l & ~mask) | (id & mask)) << 32);
}

void gs_vsync(void) {
    csr_field ^= CSR_FIELD;
    set_events(CSR_VSINT | CSR_HSINT);   /* many HSyncs happened since the last VSync too */

    static int log_gs = -1;
    static uint32_t vsyncs;
    if (log_gs < 0) log_gs = getenv("PS2_LOG_GS") != NULL;
    if (log_gs && ++vsyncs % 60 == 0) {
        fprintf(stderr, "[gs] last 60 vsyncs: %u register writes, %u primitives (%u triangles, %u sprites), "
                        "%u transfers, %u image bytes\n", gs_stats.regs, gs_stats.prims, gs_stats.tris,
                gs_stats.sprites, gs_stats.xfers, gs_stats.image_bytes);
        memset(&gs_stats, 0, sizeof gs_stats);
    }
}

static uint64_t csr_value(void) { return csr_events | csr_field | CSR_FIFO_EMPTY | CSR_REV_ID; }

static void csr_write(uint64_t v) {
    csr_events &= ~(uint32_t)(v & 0x1F);
    if (v & CSR_RESET) {
        csr_events = 0;
        gs_draw_reset();
    }
}

static void imr_write(uint64_t v) {
    uint32_t old = imr;
    imr = (uint32_t)(v & 0x7F00);
    if (((old & ~imr) >> 8) & csr_events & 0x1F) intc_raise(INTC_GS);   /* unmasking a pending event */
}

/* ---- register access ---------------------------------------------------------------------------- */

void gs_priv_access(uint32_t phys, void* data, unsigned size, int is_write) {
    uint32_t off = phys - 0x12000000u;
    unsigned sub = phys & 7;
    if (size > 8) {                      /* 128-bit: low half is the register, high half is padding */
        if (!is_write) memset(data, 0, size);
        gs_priv_access(phys, data, 8, is_write);
        return;
    }
    unsigned n = size > 8 - sub ? 8 - sub : size;
    uint64_t* reg = NULL;
    uint64_t tmp;
    int special = 0;                     /* 1 CSR, 2 IMR */
    if (off < 0x100) {
        if ((off & 0xF) >= 8 || (off >> 4) >= P_COUNT) { if (!is_write) memset(data, 0, size); return; }
        reg = &priv[off >> 4];
    } else if ((off & ~7u) == 0x1000) { tmp = csr_value(); reg = &tmp; special = 1; }
    else if ((off & ~7u) == 0x1010) { tmp = imr; reg = &tmp; special = 2; }
    else if ((off & ~7u) == 0x1040) reg = &busdir;
    else if ((off & ~7u) == 0x1080) reg = &siglblid;
    else {
        if (!is_write) memset(data, 0, size);
        hw_log_unhandled("gs", phys, data, size, is_write);
        return;
    }
    if (!is_write) {
        GS_INIT();
        memset(data, 0, size);
        memcpy(data, (uint8_t*)reg + sub, n);
        return;
    }
    uint64_t v = 0;
    memcpy((uint8_t*)&v + sub, data, n);
    if (special == 1) { csr_write(v); return; }
    uint64_t m = n == 8 ? ~0ull : (((1ull << (n * 8)) - 1) << (sub * 8));
    uint64_t merged = (*reg & ~m) | (v & m);
    if (special == 2) imr_write(merged);
    else {
        if ((reg == &priv[P_DISPFB1] || reg == &priv[P_DISPFB2]) && *reg != merged)
            perf_state.flips++;          /* PS2_PERF: the game showed another buffer */
        *reg = merged;
    }
    if (reg == &priv[P_EXTWRITE] && (merged & 1)) {
        static int logged;
        if (!logged) { logged = 1; fprintf(stderr, "[gs] EXTWRITE (display capture) not emulated\n"); }
    }
}

/* ---- display (PCRTC) ---------------------------------------------------------------------------- */

typedef struct {
    int fbp, fbw, psm, dbx, dby, dx, dy, magh, magv, dw, dh;
    const gs_fmt* fmt;
} circuit;

static void decode_circuit(int i, circuit* c) {
    uint64_t fb = priv[P_DISPFB1 + 2 * i], d = priv[P_DISPLAY1 + 2 * i];
    c->fbp = (int)(fb & 0x1FF); c->fbw = (int)((fb >> 9) & 0x3F); c->psm = (int)((fb >> 15) & 0x1F);
    c->dbx = (int)((fb >> 32) & 0x7FF); c->dby = (int)((fb >> 43) & 0x7FF);
    c->dx = (int)(d & 0xFFF); c->dy = (int)((d >> 12) & 0x7FF);
    c->magh = (int)((d >> 23) & 0xF); c->magv = (int)((d >> 27) & 3);
    c->dw = (int)((d >> 32) & 0xFFF); c->dh = (int)((d >> 44) & 0x7FF);
    c->fmt = gs_fmt_of(c->psm);
    if (!c->fmt) c->fmt = gs_fmt_of(PSMCT32);
}

/* One output row of a circuit as 0xAARRGGBB (alpha in the GS's 8-bit form); x < 0 = outside. */
static void read_row(const circuit* c, int fy, const int* xs, int w, uint32_t* out) {
    const gs_fmt* f = c->fmt;
    fy &= 2047;
    const uint16_t* trow = f->tab + (((uint32_t)fy & ((1u << f->ph_shift) - 1)) << 6);
    const uint32_t prow = (uint32_t)c->fbp + ((uint32_t)fy >> f->ph_shift) * (uint32_t)c->fbw;
    for (int i = 0; i < w; i++) {
        int fx = xs[i];
        if (fx < 0) { out[i] = 0; continue; }
        uint32_t v;
        if (gs_swz_fast) {
            uint32_t a = (((prow + ((uint32_t)fx >> 6)) & 511u) << 13) | ((uint32_t)trow[fx & 63] << f->elem_shift);
            if (f->bits == 16) { uint16_t h; memcpy(&h, gs_vram + a, 2); v = h; }
            else memcpy(&v, gs_vram + a, 4);
        } else {
            v = gs_mem_read(c->psm, (uint32_t)c->fbp * 32, (uint32_t)c->fbw, fx, fy);
        }
        if (f->bits == 16)
            out[i] = ((v & 0x1F) << 19) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 3) | (v & 0x8000 ? 0x80000000u : 0);
        else
            out[i] = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF) | (f->bits == 24 ? 0x80000000u : (v & 0xFF000000u));
    }
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t al) {      /* a*al + b*(255-al), al 0..255 */
    uint32_t r = (((a >> 16) & 0xFF) * al + ((b >> 16) & 0xFF) * (255 - al) + 127) / 255;
    uint32_t g = (((a >> 8) & 0xFF) * al + ((b >> 8) & 0xFF) * (255 - al) + 127) / 255;
    uint32_t bl = ((a & 0xFF) * al + (b & 0xFF) * (255 - al) + 127) / 255;
    return (r << 16) | (g << 8) | bl;
}

int gs_display(uint32_t* dst, int stride, int* w, int* h) {
    GS_INIT();
    uint64_t pmode = priv[P_PMODE];
    int en[2] = { (int)(pmode & 1), (int)((pmode >> 1) & 1) };
    *w = *h = 0;
    if (!en[0] && !en[1]) return 0;
    int mmod = (int)((pmode >> 5) & 1), slbg = (int)((pmode >> 7) & 1), alp = (int)((pmode >> 8) & 0xFF);
    uint64_t smode2 = priv[P_SMODE2];
    int field_mode = (smode2 & 3) == 3;  /* interlaced, FFMD: the buffer holds one field */

    circuit c[2];
    int ux0 = 1 << 30, uy0 = 1 << 30, ux1 = 0, uy1 = 0;
    for (int i = 0; i < 2; i++) {
        decode_circuit(i, &c[i]);
        if (!en[i]) continue;
        ux0 = c[i].dx < ux0 ? c[i].dx : ux0;
        uy0 = c[i].dy < uy0 ? c[i].dy : uy0;
        ux1 = c[i].dx + c[i].dw + 1 > ux1 ? c[i].dx + c[i].dw + 1 : ux1;
        uy1 = c[i].dy + c[i].dh + 1 > uy1 ? c[i].dy + c[i].dh + 1 : uy1;
    }
    const circuit* ref = &c[en[1] ? 1 : 0];
    int hs = ref->magh + 1, vs = ref->magv + 1;
    int W = (ux1 - ux0) / hs, H = (uy1 - uy0) / vs;
    if (W > 1024) W = 1024;
    if (H > 1024) H = 1024;
    if (W <= 0 || H <= 0) return 0;
    {                                    /* log each new display configuration once */
        static uint64_t last[5];
        uint64_t now[5] = { pmode & 0xFFFF, smode2 & 3, en[0] ? priv[P_DISPFB1] : 0, en[1] ? priv[P_DISPFB2] : 0,
                            ((uint64_t)W << 32) | (uint64_t)H };
        static int logged;
        if (memcmp(now, last, sizeof now) && logged < 16) {
            logged++;
            memcpy(last, now, sizeof now);
            fprintf(stderr, "[gs] display %dx%d: PMODE 0x%04X SMODE2 %d, circuit1 %s FBP %d PSM 0x%X, circuit2 %s FBP %d PSM 0x%X\n",
                    W, H, (unsigned)(pmode & 0xFFFF), (int)(smode2 & 3), en[0] ? "on" : "off", c[0].fbp, c[0].psm,
                    en[1] ? "on" : "off", c[1].fbp, c[1].psm);
        }
    }

    static int xs[2][1024];
    static uint32_t row[2][1024];
    for (int i = 0; i < 2; i++) {
        if (!en[i]) continue;
        for (int ox = 0; ox < W; ox++) {
            int X = ux0 + ox * hs;
            xs[i][ox] = (X >= c[i].dx && X <= c[i].dx + c[i].dw) ? (c[i].dbx + (X - c[i].dx) / (c[i].magh + 1)) & 2047 : -1;
        }
    }
    uint32_t bg = (uint32_t)priv[P_BGCOLOR];
    bg = ((bg & 0xFF) << 16) | (bg & 0xFF00) | ((bg >> 16) & 0xFF);

    int last_fy[2] = { -1, -1 }, last_out = -1;    /* buffer rows in row[] / in the previous output row */
    for (int oy = 0; oy < H; oy++) {
        int L = uy0 + oy * vs, in[2] = { 0, 0 };
        for (int i = 0; i < 2; i++) {
            if (!en[i] || L < c[i].dy || L > c[i].dy + c[i].dh) continue;
            int rel = (L - c[i].dy) / (c[i].magv + 1);
            if (field_mode) rel >>= 1;
            if (c[i].dby + rel != last_fy[i]) {          /* line doubling / MAGV: reuse the row */
                read_row(&c[i], c[i].dby + rel, xs[i], W, row[i]);
                last_fy[i] = c[i].dby + rel;
            }
            in[i] = 1;
        }
        uint32_t* d = dst + (size_t)oy * (size_t)stride;
        if (!in[0]) {                    /* circuit 2 (or the background) only */
            if (slbg) for (int x = 0; x < W; x++) d[x] = bg;
            else if (in[1] && oy > 0 && last_out == last_fy[1]) memcpy(d, d - stride, (size_t)W * 4);
            else if (in[1]) for (int x = 0; x < W; x++) d[x] = row[1][x] & 0xFFFFFF;
            else for (int x = 0; x < W; x++) d[x] = 0;
            last_out = in[1] && !slbg ? last_fy[1] : -1;
            continue;
        }
        last_out = -1;
        for (int x = 0; x < W; x++) {
            uint32_t lower = slbg ? bg : (in[1] && xs[1][x] >= 0) ? row[1][x] & 0xFFFFFF : 0;
            if (xs[0][x] < 0) { d[x] = lower; continue; }
            uint32_t top = row[0][x];
            if (en[1] || slbg) {
                /* PMODE.MMOD: 1 = the fixed ALP (0xFF = 1.0), 0 = circuit 1's pixel alpha (GS alpha,
                 * 0x80 = 1.0) */
                uint32_t pa = top >> 24, al = mmod ? (uint32_t)alp : pa >= 0x80 ? 255u : pa * 2;
                d[x] = mix(top, lower, al);
            } else {
                d[x] = top & 0xFFFFFF;
            }
        }
    }
    *w = W;
    *h = H;
    return 1;
}
