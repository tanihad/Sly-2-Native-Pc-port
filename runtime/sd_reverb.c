/* Native sound backend: SPU2 reverb ("effect") unit.
 *
 * Algorithm: the PS1 SPU reverb as documented by psx-spx ("SPU Reverb Formula"), which the SPU2
 * keeps per core with its own register names (FB_SRC_A = dAPF1, IIR_ALPHA = vIIR, ...). It runs at
 * half the output rate (24 kHz here, 22.05 kHz on the PS1) on a ring buffer in sound memory
 * between ESA and EEA; the wet input is decimated and the output interpolated with a 39-tap
 * half-band FIR (psx-spx describes a 39-tap filter; ours is a Blackman-windowed half-band design
 * with the same structure, centre tap 0x4000, generated in sd_reverb_init()).
 *
 * Presets: libsd 3.3's sceSdSetEffectAttr copies one of ten 32-halfword parameter sets and writes
 * the address entries x4 into the SPU2 (verified on the copy of libsd inside the game, data at
 * libsd+0x4AC0, sizes at +0x4A98). The values are the classic PS1 reverb presets (psx-spx "Reverb
 * Examples" lists the same Room/Studio/Hall/Space/Echo/Delay/Pipe numbers), not retimed for 48 kHz,
 * so on the PS2 every reverb is ~8% shorter than on the PS1; we reproduce that. Work area sizes are
 * libsd's table (in 8-byte units there): ESA = EEA - size + 1. */
#include "sd_internal.h"

#include <math.h>
#include <string.h>

const uint16_t sd_reverb_preset[SD_REVERB_MODES][RV_COUNT] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },  /* OFF */
    { 0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000 },  /* ROOM */
    { 0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000 },  /* STUDIO_A */
    { 0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000 },  /* STUDIO_B */
    { 0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000 },  /* STUDIO_C */
    { 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000 },  /* HALL */
    { 0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000 },  /* SPACE */
    { 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000 },  /* ECHO */
    { 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000 },  /* DELAY */
    { 0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000 },  /* PIPE */
};

const uint32_t sd_reverb_size[SD_REVERB_MODES] = {
    0x10, 0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00,
};

/* ---- half-band resampling filter ------------------------------------------------------------ */

#define FIR_TAPS   39
#define FIR_CENTER 19
static int16_t fir[FIR_TAPS];          /* 1.15; even offsets from the centre are zero */
static int     fir_ready;

void sd_reverb_init(void) {
    if (fir_ready) return;
    double h[FIR_TAPS], odd_sum = 0;
    const double pi = 3.14159265358979323846;
    for (int k = 0; k < FIR_TAPS; k++) {
        int m = k - FIR_CENTER;
        double w = 0.42 - 0.5 * cos(2 * pi * k / (FIR_TAPS - 1)) + 0.08 * cos(4 * pi * k / (FIR_TAPS - 1));
        if (m == 0)          h[k] = 0.5;
        else if (m % 2 == 0) h[k] = 0.0;
        else               { h[k] = sin(pi * m / 2) / (pi * m) * w; odd_sum += h[k]; }
    }
    for (int k = 0; k < FIR_TAPS; k++) {        /* DC gain exactly 1: odd taps sum to 0.5 */
        int m = k - FIR_CENTER;
        double v = (m != 0 && (m & 1)) ? h[k] * 0.5 / odd_sum : h[k];
        fir[k] = (int16_t)lrint(v * 32768.0 > 32767.0 ? 32767.0 : v * 32768.0);
    }
    fir[FIR_CENTER] = 0x4000;
    fir_ready = 1;
}

void sd_reverb_reset(sd_reverb* r) {
    uint16_t regs[RV_COUNT];
    memcpy(regs, r->regs, sizeof regs);
    memset(r, 0, sizeof *r);
    memcpy(r->regs, regs, sizeof regs);
}

static inline int32_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* FIR over the last FIR_TAPS entries of a 64-entry ring ending at `last`. */
static inline int32_t fir_apply(const int16_t* ring, uint32_t last) {
    int32_t acc = (int32_t)fir[FIR_CENTER] * ring[(last - FIR_CENTER) & 63];
    for (int k = 0; k < FIR_TAPS; k += 2)       /* only odd offsets from the centre are non-zero */
        acc += (int32_t)fir[k] * ring[(last - (uint32_t)k) & 63];
    return acc >> 15;
}

/* ---- the reverb step (24 kHz) ---------------------------------------------------------------- */

typedef struct { uint16_t* ram; uint32_t esa, size, pos; } rv_buf;

static inline uint32_t rv_addr(const rv_buf* b, int32_t off) {
    int32_t rel = (int32_t)((b->pos + (uint32_t)((off % (int32_t)b->size) + (int32_t)b->size)) % b->size);
    return (b->esa + (uint32_t)rel) & SD_RAM_MASK;
}
static inline int32_t rv_rd(const rv_buf* b, int32_t off) { return (int16_t)b->ram[rv_addr(b, off)]; }
static inline void rv_wr(const rv_buf* b, int32_t off, int32_t v) { b->ram[rv_addr(b, off)] = (uint16_t)clamp16(v); }
static inline int32_t mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }

static void reverb_step(sd_reverb* r, const rv_buf* b, int32_t in_l, int32_t in_r, int32_t* out_l, int32_t* out_r) {
#define A(x) ((int32_t)r->regs[x] * 4)                 /* address offset in halfwords */
#define V(x) ((int32_t)(int16_t)r->regs[x])            /* signed 1.15 volume */
    int32_t lin = mul(V(RV_vLIN), in_l), rin = mul(V(RV_vRIN), in_r);
    int32_t viir = V(RV_vIIR), vwall = V(RV_vWALL), t;

    /* same side reflection (L-to-L, R-to-R) */
    t = rv_rd(b, A(RV_mLSAME) - 1);
    rv_wr(b, A(RV_mLSAME), mul(clamp16(lin + mul(rv_rd(b, A(RV_dLSAME)), vwall)) - t, viir) + t);
    t = rv_rd(b, A(RV_mRSAME) - 1);
    rv_wr(b, A(RV_mRSAME), mul(clamp16(rin + mul(rv_rd(b, A(RV_dRSAME)), vwall)) - t, viir) + t);
    /* different side reflection (R-to-L, L-to-R) */
    t = rv_rd(b, A(RV_mLDIFF) - 1);
    rv_wr(b, A(RV_mLDIFF), mul(clamp16(lin + mul(rv_rd(b, A(RV_dRDIFF)), vwall)) - t, viir) + t);
    t = rv_rd(b, A(RV_mRDIFF) - 1);
    rv_wr(b, A(RV_mRDIFF), mul(clamp16(rin + mul(rv_rd(b, A(RV_dLDIFF)), vwall)) - t, viir) + t);

    /* early echo (comb filters) */
    int32_t l = mul(V(RV_vCOMB1), rv_rd(b, A(RV_mLCOMB1))) + mul(V(RV_vCOMB2), rv_rd(b, A(RV_mLCOMB2)))
              + mul(V(RV_vCOMB3), rv_rd(b, A(RV_mLCOMB3))) + mul(V(RV_vCOMB4), rv_rd(b, A(RV_mLCOMB4)));
    int32_t rr = mul(V(RV_vCOMB1), rv_rd(b, A(RV_mRCOMB1))) + mul(V(RV_vCOMB2), rv_rd(b, A(RV_mRCOMB2)))
               + mul(V(RV_vCOMB3), rv_rd(b, A(RV_mRCOMB3))) + mul(V(RV_vCOMB4), rv_rd(b, A(RV_mRCOMB4)));
    l = clamp16(l); rr = clamp16(rr);

    /* late reverb: two all-pass filters */
    int32_t va1 = V(RV_vAPF1), va2 = V(RV_vAPF2), d1 = A(RV_dAPF1), d2 = A(RV_dAPF2);
    t = rv_rd(b, A(RV_mLAPF1) - d1);  l = clamp16(l - mul(va1, t));   rv_wr(b, A(RV_mLAPF1), l);  l = clamp16(mul(l, va1) + t);
    t = rv_rd(b, A(RV_mRAPF1) - d1);  rr = clamp16(rr - mul(va1, t)); rv_wr(b, A(RV_mRAPF1), rr); rr = clamp16(mul(rr, va1) + t);
    t = rv_rd(b, A(RV_mLAPF2) - d2);  l = clamp16(l - mul(va2, t));   rv_wr(b, A(RV_mLAPF2), l);  l = clamp16(mul(l, va2) + t);
    t = rv_rd(b, A(RV_mRAPF2) - d2);  rr = clamp16(rr - mul(va2, t)); rv_wr(b, A(RV_mRAPF2), rr); rr = clamp16(mul(rr, va2) + t);
#undef A
#undef V
    *out_l = l;
    *out_r = rr;
}

void sd_reverb_sample(sd_reverb* r, uint16_t* ram, uint32_t esa, uint32_t eea, int enabled,
                      int32_t in_l, int32_t in_r, int32_t* out_l, int32_t* out_r) {
    uint32_t p = ++r->hist_pos & 63;
    r->in_hist[0][p] = (int16_t)clamp16(in_l);
    r->in_hist[1][p] = (int16_t)clamp16(in_r);
    int16_t yl = 0, yr = 0;
    if (r->phase) {
        esa &= SD_RAM_MASK;
        eea &= SD_RAM_MASK;
        if (enabled && eea > esa) {
            rv_buf b = { ram, esa, eea - esa + 1, 0 };
            if (r->pos >= b.size) r->pos %= b.size;
            b.pos = r->pos;
            int32_t ol, orr;
            reverb_step(r, &b, fir_apply(r->in_hist[0], p), fir_apply(r->in_hist[1], p), &ol, &orr);
            yl = (int16_t)ol;
            yr = (int16_t)orr;
            r->pos = (r->pos + 1) % b.size;
        }
    }
    r->phase ^= 1;
    r->out_hist[0][p] = yl;
    r->out_hist[1][p] = yr;
    *out_l = clamp16(fir_apply(r->out_hist[0], p) * 2);   /* zero-stuffed: x2 restores the level */
    *out_r = clamp16(fir_apply(r->out_hist[1], p) * 2);
}
