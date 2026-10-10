/* Native sound backend: Sony libsd's API (sd.h) on a PC voice mixer.
 *
 * This replaces libsd + the SPU2 for the recompiled 989snd: the sceSd* calls are implemented on a
 * native model of what the SPU2 does with its registers (2 cores x 24 voices, PS2 ADPCM, 4-tap
 * Gaussian interpolation, ADSR, sweeps, noise, pitch modulation, dry/wet routing, reverb, core 0
 * into core 1, sound-data input), rendered on demand at 48 kHz by the output thread (sd_render).
 * Nothing is simulated register-by-register: the libsd calls are translated directly into voice
 * state changes.
 *
 * Sources: Sony's libsd semantics (entry encodings, units, defaults) as observed in the copy of
 * libsd 3.3 inside the game (see notes/audio-mixer.md for every value checked), ps2sdk's
 * libsd-common.h for the constant names, psx-spx for the SPU voice algorithms the SPU2 inherited
 * (ADPCM, Gaussian interpolation, ADSR, sweep, noise, pitch modulation, reverb), ps2tek for the
 * SPU2-specific parts (AutoDMA block layout).
 *
 * Threading: all sceSd* calls come from the emulation thread, sd_render from the output thread;
 * one SRW lock protects the whole state. Rendering takes it in 64-frame slices. Transfer-completion
 * notifications (iop_sd_transfer_done) are always made with the lock released. */
#include "sd_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- libsd constants (libsd.h / ps2sdk libsd-common.h) ---------------------------------------- */

enum {
    P_VOLL = 0x00, P_VOLR, P_PITCH, P_ADSR1, P_ADSR2, P_ENVX, P_VOLXL, P_VOLXR,
    P_MMIX = 0x08, P_MVOLL, P_MVOLR, P_EVOLL, P_EVOLR, P_AVOLL, P_AVOLR, P_BVOLL, P_BVOLR,
    P_MVOLXL, P_MVOLXR,
    S_PMON = 0x13, S_NON, S_KON, S_KOFF, S_ENDX, S_VMIXL, S_VMIXEL, S_VMIXR, S_VMIXER,
    A_ESA = 0x1C, A_EEA, A_TSA, A_IRQA,
    VA_SSA = 0x20, VA_LSAX, VA_NAX,
    R_ATTR = 0x23, R_ADMAS = 0x27, R_STATX = 0x28,    /* raw core registers in libsd's table */
};
enum { TRANS_WRITE = 0, TRANS_READ = 1, TRANS_STOP = 2, TRANS_WRITE_FROM = 3, TRANS_LOOP = 0x10 };
enum { ATTR_ON = 0x8000, ATTR_UNMUTE = 0x4000, ATTR_EFFECT = 0x0080, ATTR_IRQ = 0x0040 };
enum { SD_ERR_PARAM = -100 };

/* ---- state ------------------------------------------------------------------------------------- */

enum { PH_RELEASE = 0, PH_ATTACK, PH_DECAY, PH_SUSTAIN };    /* zeroed voice = silent, released */

typedef struct {
    uint16_t reg;                 /* VOLL/VOLR/MVOL register value */
    int32_t  cur;                 /* current signed volume (VOLX), -0x8000..0x7FFF */
    int32_t  level;               /* sweep magnitude 0..0x7FFF */
    uint32_t counter;
} sd_vol;

typedef struct {
    sd_vol   vol[2];
    uint16_t pitch, adsr1, adsr2;
    uint32_t ssa, lsa;            /* halfword addresses */
    uint32_t cur;                 /* halfword address of the block being played */
    uint32_t counter;             /* pitch counter: bits 12+ sample in block, 4-11 interpolation */
    int32_t  env;                 /* ENVX 0..0x7FFF */
    uint32_t env_counter;
    uint8_t  phase;
    uint8_t  flags;               /* loop flags of the current block */
    int32_t  hist[2];             /* ADPCM predictor: previous two decoded samples */
    int16_t  s[3 + 28];           /* 3 samples of the previous block + the current block */
} sd_voice;

typedef struct {
    int      active, loop;
    uint32_t base, size, ptr;     /* IOP addresses; ptr = next block to fetch */
    int16_t  fifo[2][256][2];     /* the SPU2's double-buffered sound-data input (MEMIN) */
    int      count, rd, pos;      /* queued blocks, block being played, frame within it */
} sd_adma;

typedef struct {
    sd_voice  v[24];
    uint32_t  pmon, non, vmixl, vmixr, vmixel, vmixer, kon, koff, endx;
    uint16_t  mmix, attr, spdif;
    sd_vol    mvol[2];
    int16_t   evol[2], avol[2], bvol[2];
    uint32_t  esa, eea_hi, tsa, irqa;
    int32_t   noise_timer;
    uint16_t  noise_level;
    int       effect_mode;
    sd_reverb rev;
    sd_adma   adma;
} sd_core;

static SRWLOCK  g_lock = SRWLOCK_INIT;
static int      g_ready;
static uint16_t g_ram[SD_RAM_HALFWORDS];
static sd_core  g_core[2];
static uint64_t g_frames;
static int      g_irq_pending[2];      /* ADMA half-buffer completions to announce */
static int16_t  g_gauss[512];
static int      g_log = -1;

#define LOCK()   AcquireSRWLockExclusive(&g_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&g_lock)

static void log_once(int* flag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_once(int* flag, const char* fmt, ...) {
    if (*flag) return;
    *flag = 1;
    va_list ap;
    va_start(ap, fmt);
    fputs("[sd] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
/* Log lines carry the audio clock (seconds of rendered audio = position in a PS2_AUDIO_WAV file). */
#define SDLOG(...) do { if (g_log > 0) { fprintf(stderr, "[sd %8.3f] ", (double)g_frames / 48000.0); \
                                         fprintf(stderr, __VA_ARGS__); } } while (0)

static inline int32_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static inline int32_t mul15(int32_t a, int32_t b) { return (a * b) >> 15; }

/* ---- one-time tables ------------------------------------------------------------------------- */

/* Gaussian interpolation table, used exactly as psx-spx describes the hardware's 512-entry table:
 *   out = g[0FFh-i]*oldest + g[1FFh-i]*older + g[100h+i]*old + g[i]*new   (each SAR 15)
 * g[n] samples a symmetric kernel K at distance (512-n)/256 samples. We generate K instead of
 * copying the ROM table: K(x) = A*(exp(-x^2/(2s^2)) - exp(-2/s^2)) on |x| < 2, with A and s fitted
 * so that K is 0 at distance 2 and matches the table's documented end values (g[100h] = 1307h at
 * distance 1, g[1FFh] = 59B3h next to distance 0). The four weights of every phase sum to
 * 7EA8h..7F88h (within 0.07 dB of unity), so the PS2's characteristic soft treble is kept. */
static void gauss_init(void) {
    const double A = 0.7022415937815975, s2 = 0.3240737457124835;
    const double c = exp(-4.0 / (2.0 * s2));
    for (int n = 0; n < 512; n++) {
        double x = (512 - n) / 256.0;
        g_gauss[n] = (int16_t)lrint(32768.0 * A * (exp(-x * x / (2.0 * s2)) - c));
    }
}

static void global_init(void) {          /* call with the lock held */
    if (g_ready) return;
    gauss_init();
    sd_reverb_init();
    if (g_log < 0) {
        const char* e = getenv("PS2_LOG_SD");
        g_log = e && *e && *e != '0';
    }
    g_ready = 1;
}

const int16_t* sd_gauss_table(void) {
    LOCK();
    global_init();
    UNLOCK();
    return g_gauss;
}

uint16_t* sd_ram(void) { return g_ram; }

/* ---- ADPCM ------------------------------------------------------------------------------------ */

/* psx-spx (SPU ADPCM = CD-XA ADPCM decoding): header byte 0 = shift (bits 0-3, 0 = loudest;
 * 13-15 behave as 9) and filter (bits 4-6, 0-4 defined; 5-7 treated as 0), byte 1 = loop flags,
 * then 28 4-bit samples, low nibble first:
 *   s = (nibble SHL 12) SAR shift + (old*f0 + older*f1 + 32) SAR 6, saturated to 16 bits. */
static const int32_t k_f0[8] = { 0, 60, 115, 98, 122, 0, 0, 0 };
static const int32_t k_f1[8] = { 0, 0, -52, -55, -60, 0, 0, 0 };

void sd_adpcm_decode_block(const uint8_t blk[16], int32_t hist[2], int16_t out[28]) {
    int shift = blk[0] & 0x0F;
    if (shift > 12) shift = 9;
    int filter = (blk[0] >> 4) & 7;
    int32_t f0 = k_f0[filter], f1 = k_f1[filter];
    int32_t h0 = hist[0], h1 = hist[1];
    for (int i = 0; i < 28; i++) {
        int32_t nib = (blk[2 + (i >> 1)] >> ((i & 1) * 4)) & 0xF;
        int32_t t = (int32_t)(int16_t)(uint16_t)(nib << 12) >> shift;
        int32_t s = clamp16(t + ((h0 * f0 + h1 * f1 + 32) >> 6));
        h1 = h0;
        h0 = s;
        out[i] = (int16_t)s;
    }
    hist[0] = h0;
    hist[1] = h1;
}

/* ---- envelopes and volume sweeps (psx-spx "Envelope Operation") ------------------------------- */

/* rate = shift << 2 | step (7 bits). Increase steps +7..+4, decrease -8..-5, scaled by
 * 1 << (11 - shift) below shift 11; above it the step happens every 1 << (shift - 11) samples
 * (shift >= 27, e.g. rate 7Fh, never steps). Exponential increase is 4x slower above 6000h,
 * exponential decrease scales the step by the current level. */
static inline void env_step(int32_t* level, uint32_t* counter, int rate, int expo, int decrease) {
    int shift = rate >> 2;
    int32_t step = decrease ? (rate & 3) - 8 : 7 - (rate & 3);
    uint32_t inc = 0x8000;
    if (shift < 11) step *= 1 << (11 - shift);
    else            inc >>= shift - 11;
    if (expo) {
        if (!decrease && *level > 0x6000) inc >>= 2;
        if (decrease) step = (step * *level) >> 15;
    }
    if (!inc) return;
    *counter += inc;
    if (*counter < 0x8000) return;
    *counter -= 0x8000;
    int32_t l = *level + step;
    *level = l < 0 ? 0 : l > 0x7FFF ? 0x7FFF : l;
}

static void vol_set(sd_vol* v, uint16_t reg) {
    v->reg = reg;
    if (reg & 0x8000) {            /* sweep: starts from the current volume */
        v->level = v->cur < 0 ? -v->cur : v->cur;
        if (v->level > 0x7FFF) v->level = 0x7FFF;
        v->counter = 0;
        v->cur = (reg & 0x1000) ? -v->level : v->level;
    } else {                       /* fixed: bits 0-14 = volume / 2 */
        v->cur = (int16_t)(uint16_t)(reg << 1);
    }
}

static inline void vol_tick(sd_vol* v) {
    if (!(v->reg & 0x8000)) return;
    env_step(&v->level, &v->counter, v->reg & 0x7F, (v->reg >> 14) & 1, (v->reg >> 13) & 1);
    v->cur = (v->reg & 0x1000) ? -v->level : v->level;
}

static inline void env_tick(sd_voice* vc) {
    switch (vc->phase) {
    case PH_ATTACK:
        env_step(&vc->env, &vc->env_counter, (vc->adsr1 >> 8) & 0x7F, vc->adsr1 >> 15, 0);
        if (vc->env >= 0x7FFF) { vc->env = 0x7FFF; vc->phase = PH_DECAY; vc->env_counter = 0; }
        break;
    case PH_DECAY: {
        int32_t sustain_level = ((vc->adsr1 & 0xF) + 1) << 11;
        if (vc->env > sustain_level)
            env_step(&vc->env, &vc->env_counter, ((vc->adsr1 >> 4) & 0xF) << 2, 1, 1);
        if (vc->env <= sustain_level) { vc->phase = PH_SUSTAIN; vc->env_counter = 0; }
        break;
    }
    case PH_SUSTAIN:
        env_step(&vc->env, &vc->env_counter, (vc->adsr2 >> 6) & 0x7F, vc->adsr2 >> 15, (vc->adsr2 >> 14) & 1);
        break;
    default:   /* release */
        if (vc->env)
            env_step(&vc->env, &vc->env_counter, (vc->adsr2 & 0x1F) << 2, (vc->adsr2 >> 5) & 1, 1);
        break;
    }
}

/* ---- voices ----------------------------------------------------------------------------------- */

static void load_block(sd_voice* vc, int decode) {
    const uint8_t* b = (const uint8_t*)&g_ram[vc->cur & SD_RAM_MASK & ~7u];
    vc->flags = b[1];
    if (b[1] & 4) vc->lsa = vc->cur;                /* loop start: remember this block */
    if (decode) {
        vc->s[0] = vc->s[28]; vc->s[1] = vc->s[29]; vc->s[2] = vc->s[30];
        sd_adpcm_decode_block(b, vc->hist, vc->s + 3);
    }
}

/* The current block is finished: follow its loop flags (psx-spx: 0/2 next block, 1 end+mute,
 * 3 end+repeat; ENDX is set on both ends and the voice jumps to the loop address). */
static void next_block(sd_core* k, int v, int decode) {
    sd_voice* vc = &k->v[v];
    if (vc->flags & 1) {
        k->endx |= 1u << v;
        vc->cur = vc->lsa;
        if (!(vc->flags & 2)) { vc->phase = PH_RELEASE; vc->env = 0; vc->env_counter = 0; }
    } else {
        vc->cur = (vc->cur + 8) & SD_RAM_MASK;
    }
    load_block(vc, decode);
}

static void key_on(sd_core* k, int v) {
    sd_voice* vc = &k->v[v];
    vc->cur = vc->ssa & ~7u;
    vc->counter = 0;
    vc->env = 0;
    vc->env_counter = 0;
    vc->phase = PH_ATTACK;
    vc->hist[0] = vc->hist[1] = 0;
    memset(vc->s, 0, sizeof vc->s);
    k->endx &= ~(1u << v);
    load_block(vc, 1);
}

static void key_off(sd_core* k, int v) {
    sd_voice* vc = &k->v[v];
    if (vc->phase != PH_RELEASE) { vc->phase = PH_RELEASE; vc->env_counter = 0; }
}

static inline uint32_t voice_nax(const sd_voice* vc) {
    /* The address of the halfword being fetched: inside the current block, after the header. */
    return (vc->cur + 1 + ((vc->counter >> 12) >> 2)) & SD_RAM_MASK;
}

#define CHUNK 64

/* A voice that is released at level 0 can only become audible again through KON, which resets
 * its decoder, so it just advances position, loop flags, NAX and ENDX (989snd polls them). */
static void voice_skip(sd_core* k, int v, uint32_t n) {
    sd_voice* vc = &k->v[v];
    uint32_t step = vc->pitch > 0x3FFF ? 0x4000 : vc->pitch;
    uint64_t pos = (uint64_t)vc->counter + (uint64_t)step * n;
    while (pos >= (28u << 12)) {
        pos -= 28u << 12;
        next_block(k, v, 0);
    }
    vc->counter = (uint32_t)pos;
    if ((vc->vol[0].reg | vc->vol[1].reg) & 0x8000)
        for (uint32_t i = 0; i < n; i++) { vol_tick(&vc->vol[0]); vol_tick(&vc->vol[1]); }
}

static void voice_render(sd_core* k, int v, uint32_t n, const int16_t* prev_out, int16_t* out,
                         const int16_t* noise, int32_t* dl, int32_t* dr, int32_t* wl, int32_t* wr) {
    sd_voice* vc = &k->v[v];
    uint32_t bit = 1u << v;
    int pmon = v > 0 && (k->pmon & bit);
    if (vc->phase == PH_RELEASE && vc->env == 0 && !pmon) {
        voice_skip(k, v, n);
        memset(out, 0, n * sizeof *out);
        return;
    }
    int use_noise = (k->non & bit) != 0;
    int mdl = (k->vmixl & bit) != 0, mdr = (k->vmixr & bit) != 0;
    int mwl = (k->vmixel & bit) != 0, mwr = (k->vmixer & bit) != 0;
    int sweep = ((vc->vol[0].reg | vc->vol[1].reg) & 0x8000) != 0;
    const int16_t* g = g_gauss;
    for (uint32_t i = 0; i < n; i++) {
        int32_t smp;
        if (use_noise) {
            smp = noise[i];
        } else {
            uint32_t p = vc->counter >> 12, gi = (vc->counter >> 4) & 0xFF;
            const int16_t* s = &vc->s[p];
            smp = ((g[0x0FF - gi] * s[0]) >> 15) + ((g[0x1FF - gi] * s[1]) >> 15)
                + ((g[0x100 + gi] * s[2]) >> 15) + ((g[gi] * s[3]) >> 15);
            smp = clamp16(smp);
        }
        int32_t o = mul15(smp, vc->env);
        out[i] = (int16_t)o;
        if (o) {
            int32_t l = mul15(o, vc->vol[0].cur), r = mul15(o, vc->vol[1].cur);
            if (mdl) dl[i] += l;
            if (mdr) dr[i] += r;
            if (mwl) wl[i] += l;
            if (mwr) wr[i] += r;
        }
        env_tick(vc);
        if (sweep) { vol_tick(&vc->vol[0]); vol_tick(&vc->vol[1]); }
        uint32_t step = vc->pitch;
        if (pmon) {   /* psx-spx: factor = previous voice's output + 8000h, pitch sign-extended */
            int32_t f = (int32_t)prev_out[i] + 0x8000;
            step = (uint32_t)((int32_t)(((int64_t)(int16_t)step * f) >> 15)) & 0xFFFF;
        }
        if (step > 0x3FFF) step = 0x4000;
        vc->counter += step;
        while (vc->counter >= (28u << 12)) {
            vc->counter -= 28u << 12;
            next_block(k, v, 1);
        }
    }
}

static void noise_render(sd_core* k, uint32_t n, int16_t* out) {
    int shift = (k->attr >> 10) & 0xF, step = ((k->attr >> 8) & 3) + 4;
    int32_t reload = 0x20000 >> shift;
    for (uint32_t i = 0; i < n; i++) {
        k->noise_timer -= step;
        if (k->noise_timer < 0) {
            uint16_t l = k->noise_level;
            uint16_t parity = ((l >> 15) ^ (l >> 12) ^ (l >> 11) ^ (l >> 10) ^ 1) & 1;
            k->noise_level = (uint16_t)((l << 1) | parity);
            k->noise_timer += reload;
            if (k->noise_timer < 0) k->noise_timer += reload;
        }
        out[i] = (int16_t)k->noise_level;
    }
}

/* ---- sound-data input (AutoDMA) ----------------------------------------------------------------
 * ps2tek: ADMA blocks are 1024 bytes, 512 bytes of left samples then 512 of right (256 stereo
 * frames), double-buffered in the core's MEMIN area. We model the transfer as fetching one block
 * from IOP RAM whenever a MEMIN half is free, so the DMA position runs one to two blocks ahead of
 * playback as on the hardware. Loop transfers (SD_TRANS_LOOP) treat the IOP buffer as two halves
 * and announce each completed half; one-shot transfers announce the end. */
static void adma_fetch(sd_adma* a, int core) {
    while (a->active && a->count < 2) {
        int16_t (*blk)[2] = a->fifo[(a->rd + a->count) & 1];
        uint32_t off = a->ptr - a->base;
        uint32_t take = off < a->size ? a->size - off : 0;
        if (take > 1024) take = 1024;
        memset(blk, 0, sizeof a->fifo[0]);
        const uint8_t* src = iop_mem(a->ptr);
        const uint8_t* end = take ? iop_mem(a->ptr + take - 1) : NULL;
        if (src && end) {
            uint32_t nl = take < 512 ? take : 512;
            for (uint32_t i = 0; i + 1 < nl; i += 2) blk[i >> 1][0] = (int16_t)(src[i] | src[i + 1] << 8);
            for (uint32_t i = 512; i + 1 < take; i += 2) blk[(i - 512) >> 1][1] = (int16_t)(src[i] | src[i + 1] << 8);
        }
        a->count++;
        uint32_t next = off + (take ? take : 1024);
        a->ptr = a->base + next;
        if (a->loop && off < a->size / 2 && next >= a->size / 2 && next < a->size) g_irq_pending[core]++;
        if (next >= a->size) {
            g_irq_pending[core]++;
            if (a->loop) a->ptr = a->base;
            else         a->active = 0;
        }
    }
}

static void adma_render(sd_adma* a, int core, uint32_t n, int32_t* l, int32_t* r) {
    adma_fetch(a, core);
    for (uint32_t i = 0; i < n; i++) {
        if (!a->count) { l[i] = r[i] = 0; continue; }
        l[i] = a->fifo[a->rd][a->pos][0];
        r[i] = a->fifo[a->rd][a->pos][1];
        if (++a->pos == 256) {
            a->pos = 0;
            a->rd ^= 1;
            a->count--;
            adma_fetch(a, core);
        }
    }
}

static uint32_t adma_status(const sd_adma* a) {
    uint32_t half = (a->size && a->ptr - a->base >= a->size / 2) ? 1u : 0u;
    return half << 24 | (a->active ? a->ptr & 0xFFFFFFu : 0u);
}

/* ---- core mixing ------------------------------------------------------------------------------ */

static inline uint32_t core_eea(const sd_core* k) { return (k->eea_hi << 16 | 0xFFFFu) & SD_RAM_MASK; }

static void core_render(int c, uint32_t n, const int32_t* ext_l, const int32_t* ext_r, int32_t* out_l, int32_t* out_r) {
    sd_core* k = &g_core[c];
    int32_t dl[CHUNK] = { 0 }, dr[CHUNK] = { 0 }, wl[CHUNK] = { 0 }, wr[CHUNK] = { 0 };
    int32_t ml[CHUNK], mr[CHUNK];
    int16_t noise[CHUNK], vout[2][CHUNK];
    if (k->non) noise_render(k, n, noise);
    memset(vout[1], 0, sizeof vout[1]);
    for (int v = 0; v < 24; v++)
        voice_render(k, v, n, vout[(v & 1) ^ 1], vout[v & 1], noise, dl, dr, wl, wr);
    adma_render(&k->adma, c, n, ml, mr);

    uint16_t mmix = k->mmix;
    int on = (k->attr & (ATTR_ON | ATTR_UNMUTE)) == (ATTR_ON | ATTR_UNMUTE);
    uint32_t esa = k->esa, eea = core_eea(k);
    int fx = (k->attr & ATTR_EFFECT) != 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t vdl = clamp16(dl[i]), vdr = clamp16(dr[i]), vwl = clamp16(wl[i]), vwr = clamp16(wr[i]);
        int32_t xl = ext_l ? mul15(ext_l[i], k->avol[0]) : 0, xr = ext_r ? mul15(ext_r[i], k->avol[1]) : 0;
        int32_t sl = mul15(ml[i], k->bvol[0]), sr = mul15(mr[i], k->bvol[1]);
        /* MMIX: 11/10 voice dry L/R, 9/8 voice wet L/R, 7/6 sound-data input dry L/R,
         * 5/4 sound-data input wet L/R, 3/2 external input dry L/R, 1/0 external input wet L/R */
        int32_t dry_l = (mmix & 0x800 ? vdl : 0) + (mmix & 0x80 ? sl : 0) + (mmix & 0x8 ? xl : 0);
        int32_t dry_r = (mmix & 0x400 ? vdr : 0) + (mmix & 0x40 ? sr : 0) + (mmix & 0x4 ? xr : 0);
        int32_t wet_l = (mmix & 0x200 ? vwl : 0) + (mmix & 0x20 ? sl : 0) + (mmix & 0x2 ? xl : 0);
        int32_t wet_r = (mmix & 0x100 ? vwr : 0) + (mmix & 0x10 ? sr : 0) + (mmix & 0x1 ? xr : 0);
        int32_t rl, rr;
        sd_reverb_sample(&k->rev, g_ram, esa, eea, fx, clamp16(wet_l), clamp16(wet_r), &rl, &rr);
        int32_t ol = clamp16(dry_l + mul15(rl, k->evol[0]));
        int32_t orr = clamp16(dry_r + mul15(rr, k->evol[1]));
        vol_tick(&k->mvol[0]);
        vol_tick(&k->mvol[1]);
        out_l[i] = on ? mul15(ol, k->mvol[0].cur) : 0;
        out_r[i] = on ? mul15(orr, k->mvol[1].cur) : 0;
    }
}

static void render_chunk(int16_t* out, uint32_t n) {
    int32_t l0[CHUNK], r0[CHUNK], l1[CHUNK], r1[CHUNK];
    core_render(0, n, NULL, NULL, l0, r0);
    core_render(1, n, l0, r0, l1, r1);         /* core 0's output is core 1's external input */
    for (uint32_t i = 0; i < n; i++) {
        out[2 * i]     = (int16_t)clamp16(l1[i]);
        out[2 * i + 1] = (int16_t)clamp16(r1[i]);
    }
    g_frames += n;
}

void sd_render(int16_t* out, uint32_t frames) {
    while (frames) {
        uint32_t n = frames < CHUNK ? frames : CHUNK;
        int irq[2];
        LOCK();
        global_init();
        render_chunk(out, n);
        irq[0] = g_irq_pending[0]; irq[1] = g_irq_pending[1];
        g_irq_pending[0] = g_irq_pending[1] = 0;
        UNLOCK();
        for (int c = 0; c < 2; c++)
            while (irq[c]--) iop_sd_transfer_done(c);
        out += 2 * n;
        frames -= n;
    }
}

uint64_t sd_frames_rendered(void) {
    LOCK();
    uint64_t f = g_frames;
    UNLOCK();
    return f;
}

/* ---- libsd API -------------------------------------------------------------------------------- */

int sceSdInit(int flag) {
    LOCK();
    global_init();
    int hot = flag & 0xF;              /* libsd: a non-zero flag keeps volumes and effect setup */
    for (int c = 0; c < 2; c++) {
        sd_core* k = &g_core[c];
        k->attr = (uint16_t)((hot ? 0xC080 : 0xC000) | c);
        k->spdif = 0xC032;
        k->vmixl = k->vmixr = k->vmixel = k->vmixer = 0xFFFFFF;
        k->mmix = c ? 0xFFC : 0xFF0;
        if (!hot) {
            vol_set(&k->mvol[0], 0);
            vol_set(&k->mvol[1], 0);
            k->evol[0] = k->evol[1] = 0;
            k->eea_hi = c ? 0xF : 0xE;
            k->avol[0] = k->avol[1] = c ? 0x7FFF : 0;
            k->bvol[0] = k->bvol[1] = 0;
        }
        k->adma.active = 0;
        k->adma.count = 0;
        sd_reverb_reset(&k->rev);
    }
    /* A silent looping block at 0x5000 (header 00 07): every voice starts there. */
    memset(&g_ram[0x2800], 0, 16);
    g_ram[0x2800] = 0x0700;
    for (int c = 0; c < 2; c++) {
        sd_core* k = &g_core[c];
        k->tsa = 0x2808;
        for (int v = 0; v < 24; v++) {
            sd_voice* vc = &k->v[v];
            vol_set(&vc->vol[0], 0);
            vol_set(&vc->vol[1], 0);
            vc->pitch = 0x3FFF;
            vc->adsr1 = vc->adsr2 = 0;
            vc->ssa = 0x2800;
            key_on(k, v);
            key_off(k, v);
        }
        k->kon = k->koff = 0xFFFFFF;
        k->endx = 0;
    }
    UNLOCK();
    SDLOG("sceSdInit(%d)\n", flag);
    return 0;
}

static int bad_voice(int voice) {
    static int logged;
    if (voice < 24) return 0;
    log_once(&logged, "voice index %d out of range (ignored)", voice);
    return 1;
}

void sceSdSetParam(uint16_t entry, uint16_t value) {
    int core = entry & 1, voice = (entry >> 1) & 0x1F, p = entry >> 8;
    static int unk;
    LOCK();
    global_init();
    sd_core* k = &g_core[core];
    if (p < 8) {
        if (!bad_voice(voice)) {
            sd_voice* vc = &k->v[voice];
            switch (p) {
            case P_VOLL:  vol_set(&vc->vol[0], value); break;
            case P_VOLR:  vol_set(&vc->vol[1], value); break;
            case P_PITCH: vc->pitch = value; break;
            case P_ADSR1: vc->adsr1 = value; break;
            case P_ADSR2: vc->adsr2 = value; break;
            case P_ENVX:  vc->env = value & 0x7FFF; break;
            case P_VOLXL: vc->vol[0].cur = (int16_t)value; vc->vol[0].level = abs(vc->vol[0].cur); break;
            case P_VOLXR: vc->vol[1].cur = (int16_t)value; vc->vol[1].level = abs(vc->vol[1].cur); break;
            }
        }
    } else {
        switch (p) {
        case P_MMIX:   k->mmix = value & 0xFFF; break;
        case P_MVOLL:  vol_set(&k->mvol[0], value); break;
        case P_MVOLR:  vol_set(&k->mvol[1], value); break;
        case P_EVOLL:  k->evol[0] = (int16_t)value; break;
        case P_EVOLR:  k->evol[1] = (int16_t)value; break;
        case P_AVOLL:  k->avol[0] = (int16_t)value; break;
        case P_AVOLR:  k->avol[1] = (int16_t)value; break;
        case P_BVOLL:  k->bvol[0] = (int16_t)value; break;
        case P_BVOLR:  k->bvol[1] = (int16_t)value; break;
        case P_MVOLXL: k->mvol[0].cur = (int16_t)value; break;
        case P_MVOLXR: k->mvol[1].cur = (int16_t)value; break;
        case R_ATTR:   k->attr = value; break;
        default:       log_once(&unk, "sceSdSetParam: unhandled entry 0x%04X", entry); break;
        }
    }
    UNLOCK();
}

uint16_t sceSdGetParam(uint16_t entry) {
    int core = entry & 1, voice = (entry >> 1) & 0x1F, p = entry >> 8;
    static int unk;
    uint16_t r = 0;
    LOCK();
    const sd_core* k = &g_core[core];
    if (p < 8) {
        if (!bad_voice(voice)) {
            const sd_voice* vc = &k->v[voice];
            switch (p) {
            case P_VOLL:  r = vc->vol[0].reg; break;
            case P_VOLR:  r = vc->vol[1].reg; break;
            case P_PITCH: r = vc->pitch; break;
            case P_ADSR1: r = vc->adsr1; break;
            case P_ADSR2: r = vc->adsr2; break;
            case P_ENVX:  r = (uint16_t)vc->env; break;
            case P_VOLXL: r = (uint16_t)vc->vol[0].cur; break;
            case P_VOLXR: r = (uint16_t)vc->vol[1].cur; break;
            }
        }
    } else {
        switch (p) {
        case P_MMIX:   r = k->mmix; break;
        case P_MVOLL:  r = k->mvol[0].reg; break;
        case P_MVOLR:  r = k->mvol[1].reg; break;
        case P_EVOLL:  r = (uint16_t)k->evol[0]; break;
        case P_EVOLR:  r = (uint16_t)k->evol[1]; break;
        case P_AVOLL:  r = (uint16_t)k->avol[0]; break;
        case P_AVOLR:  r = (uint16_t)k->avol[1]; break;
        case P_BVOLL:  r = (uint16_t)k->bvol[0]; break;
        case P_BVOLR:  r = (uint16_t)k->bvol[1]; break;
        case P_MVOLXL: r = (uint16_t)k->mvol[0].cur; break;
        case P_MVOLXR: r = (uint16_t)k->mvol[1].cur; break;
        case R_ATTR:   r = k->attr; break;
        case R_ADMAS:  r = k->adma.active ? (uint16_t)(1u << core) : 0; break;
        case R_STATX:  r = 0; break;                      /* never busy: transfers are immediate */
        default:       log_once(&unk, "sceSdGetParam: unhandled entry 0x%04X", entry); break;
        }
    }
    UNLOCK();
    return r;
}

void sceSdSetSwitch(uint16_t entry, uint32_t value) {
    int core = entry & 1, s = entry >> 8;
    static int unk;
    value &= 0xFFFFFF;
    LOCK();
    global_init();
    sd_core* k = &g_core[core];
    switch (s) {
    case S_PMON:   k->pmon = value; break;
    case S_NON:    k->non = value; break;
    case S_VMIXL:  k->vmixl = value; break;
    case S_VMIXEL: k->vmixel = value; break;
    case S_VMIXR:  k->vmixr = value; break;
    case S_VMIXER: k->vmixer = value; break;
    case S_KON:
        k->kon = value;
        for (int v = 0; v < 24; v++)
            if (value & (1u << v)) {
                key_on(k, v);
                SDLOG("KON  c%d v%-2d ssa %05X pitch %04X adsr %04X/%04X vol %04X/%04X\n", core, v,
                      k->v[v].ssa << 1, k->v[v].pitch, k->v[v].adsr1, k->v[v].adsr2,
                      k->v[v].vol[0].reg, k->v[v].vol[1].reg);
            }
        break;
    case S_KOFF:
        k->koff = value;
        for (int v = 0; v < 24; v++)
            if (value & (1u << v)) key_off(k, v);
        if (value) SDLOG("KOFF c%d %06X\n", core, value);
        break;
    case S_ENDX:   k->endx = 0; break;        /* a write clears the end flags */
    default:       log_once(&unk, "sceSdSetSwitch: unhandled entry 0x%04X", entry); break;
    }
    UNLOCK();
}

uint32_t sceSdGetSwitch(uint16_t entry) {
    int core = entry & 1, s = entry >> 8;
    static int unk;
    uint32_t r = 0;
    LOCK();
    const sd_core* k = &g_core[core];
    switch (s) {
    case S_PMON:   r = k->pmon; break;
    case S_NON:    r = k->non; break;
    case S_KON:    r = k->kon; break;
    case S_KOFF:   r = k->koff; break;
    case S_ENDX:   r = k->endx; break;
    case S_VMIXL:  r = k->vmixl; break;
    case S_VMIXEL: r = k->vmixel; break;
    case S_VMIXR:  r = k->vmixr; break;
    case S_VMIXER: r = k->vmixer; break;
    default:       log_once(&unk, "sceSdGetSwitch: unhandled entry 0x%04X", entry); break;
    }
    UNLOCK();
    return r;
}

/* libsd writes byte addresses as hi = addr >> 17, lo = (addr >> 1) & 0xFFF8 (16-byte units). */
static inline uint32_t addr_from_bytes(uint32_t v) { return ((v >> 17) << 16 | ((v >> 1) & 0xFFF8u)) & SD_RAM_MASK; }

void sceSdSetAddr(uint16_t entry, uint32_t value) {
    int core = entry & 1, voice = (entry >> 1) & 0x1F, a = entry >> 8;
    static int unk, nax;
    uint32_t hw = addr_from_bytes(value);
    LOCK();
    global_init();
    sd_core* k = &g_core[core];
    switch (a) {
    case A_ESA:   k->esa = hw; break;
    case A_EEA:   k->eea_hi = (value >> 17) & 0xF; break;
    case A_TSA:   k->tsa = hw; break;
    case A_IRQA:  k->irqa = hw; break;
    case VA_SSA:  if (!bad_voice(voice)) k->v[voice].ssa = hw; break;
    case VA_LSAX: if (!bad_voice(voice)) k->v[voice].lsa = hw; break;
    case VA_NAX:  log_once(&nax, "sceSdSetAddr(NAX) ignored"); break;
    default:      log_once(&unk, "sceSdSetAddr: unhandled entry 0x%04X", entry); break;
    }
    UNLOCK();
}

uint32_t sceSdGetAddr(uint16_t entry) {
    int core = entry & 1, voice = (entry >> 1) & 0x1F, a = entry >> 8;
    static int unk;
    uint32_t r = 0;
    LOCK();
    const sd_core* k = &g_core[core];
    switch (a) {
    case A_ESA:   r = k->esa << 1; break;
    case A_EEA:   r = k->eea_hi << 17 | 0x1FFFFu; break;
    case A_TSA:   r = k->tsa << 1; break;
    case A_IRQA:  r = k->irqa << 1; break;
    case VA_SSA:  if (!bad_voice(voice)) r = k->v[voice].ssa << 1; break;
    case VA_LSAX: if (!bad_voice(voice)) r = k->v[voice].lsa << 1; break;
    case VA_NAX:  if (!bad_voice(voice)) r = voice_nax(&k->v[voice]) << 1; break;
    default:      log_once(&unk, "sceSdGetAddr: unhandled entry 0x%04X", entry); break;
    }
    UNLOCK();
    return r;
}

void sceSdSetCoreAttr(uint16_t entry, uint16_t value) {
    int core = entry & 1;
    static int unk;
    LOCK();
    global_init();
    sd_core* k = &g_core[core];
    switch (entry & 0xE) {
    case 0x2: k->attr = (uint16_t)((k->attr & ~ATTR_EFFECT) | ((value & 1) << 7)); break;
    case 0x4: k->attr = (uint16_t)((k->attr & ~ATTR_IRQ) | ((value & 1) << 6)); break;
    case 0x6: k->attr = (uint16_t)((k->attr & ~ATTR_UNMUTE) | ((value & 1) << 14)); break;
    case 0x8: k->attr = (uint16_t)((k->attr & 0xC0FF) | ((value & 0x3F) << 8)); break;
    case 0xA: k->spdif = value; break;
    default:  log_once(&unk, "sceSdSetCoreAttr: unhandled entry 0x%04X", entry); break;
    }
    UNLOCK();
}

/* Sony's tables: 0x8000 * 2^(n/12) and 0x8000 * 2^(k/1536), truncated (matches libsd 3.3). */
static uint16_t note_semi(int n) { return (uint16_t)floor(32768.0 * pow(2.0, n / 12.0)); }
static uint16_t note_fine(int k) { return (uint16_t)floor(32768.0 * pow(2.0, k / 1536.0)); }

/* Same arithmetic as libsd 3.3's sceSdNote2Pitch (16-bit wraps, truncating divisions, a rounding
 * right shift for low octaves and no left shift above two octaves over the centre note). */
uint16_t sceSdNote2Pitch(uint16_t center_note, uint16_t center_fine, uint16_t note, int16_t fine) {
    int32_t f = (int32_t)fine + center_fine;              /* 1/128 semitones */
    int32_t fs = (f >= 0 ? f : f + 127) >> 7;            /* whole semitones in f, toward zero */
    int32_t n = (int16_t)(uint16_t)(note + fs - center_note);
    int32_t oct = n / 12;                                  /* toward zero */
    int32_t shift = oct - 2;
    int32_t fr = f - fs * 128;                             /* fine remainder, sign of f */
    int32_t semi = (int16_t)(uint16_t)(n - oct * 12);
    if (semi < 0 || (semi == 0 && fr < 0)) { semi += 12; shift = oct - 3; }
    if (fr < 0) { semi = semi - 1 + fs; fr += (fs + 1) * 128; }
    semi = (int16_t)(uint16_t)semi;
    /* libsd indexes its tables without bounds checks; outside them we use the formula. */
    double sv = (semi >= 0 && semi < 12) ? note_semi(semi) : floor(32768.0 * pow(2.0, semi / 12.0));
    double fv = (fr >= 0 && fr < 128) ? note_fine(fr) : floor(32768.0 * pow(2.0, fr / 1536.0));
    uint32_t r = (uint32_t)((int64_t)(sv * fv) >> 16);
    int16_t sh = (int16_t)shift;
    if (sh < 0) {
        uint32_t s = (uint32_t)(-sh) & 31;
        r = (r + (1u << ((s - 1) & 31))) >> s;
    }
    return (uint16_t)r;
}

/* ---- transfers -------------------------------------------------------------------------------- */

/* Default for builds without the IOP side (the game before integration, tools): nobody to tell.
 * The IOP side's definition replaces this one at link time. */
__attribute__((weak)) void iop_sd_transfer_done(int channel) { (void)channel; }

static uint8_t* iop_range(uint32_t addr, uint32_t* len) {
    uint8_t* p = iop_mem(addr);
    if (!p) return NULL;
    while (*len && !iop_mem(addr + *len - 1)) *len = *len > 0x1000 ? *len - 0x1000 : 0;   /* clip */
    return p;
}

int sceSdVoiceTrans(int16_t channel, uint16_t mode, uint32_t iop_addr, uint32_t spu_addr, uint32_t size) {
    static int bad;
    int ch = channel & 1, dir = mode & 3;
    if (dir == TRANS_STOP) return 0;
    if (dir > TRANS_READ) return SD_ERR_PARAM;
    uint32_t n = (size + 63) & ~63u;                       /* DMA moves whole 64-byte blocks */
    uint32_t len = n;
    uint8_t* iop = iop_range(iop_addr, &len);
    if (!iop || len < n) log_once(&bad, "sceSdVoiceTrans: IOP range %08X+%X out of bounds", iop_addr, n);
    LOCK();
    global_init();
    uint8_t* ram = (uint8_t*)g_ram;
    uint32_t spu = spu_addr & 0x1FFFFEu;
    for (uint32_t done = 0; iop && done < len;) {
        uint32_t chunk = len - done, room = 0x200000u - spu;
        if (chunk > room) chunk = room;
        if (dir == TRANS_WRITE) memcpy(ram + spu, iop + done, chunk);
        else                    memcpy(iop + done, ram + spu, chunk);
        done += chunk;
        spu = (spu + chunk) & 0x1FFFFEu;
    }
    g_core[ch].tsa = ((spu_addr + n) >> 1) & SD_RAM_MASK;
    UNLOCK();
    SDLOG("VoiceTrans ch%d %s iop %06X spu %06X size %X\n", ch, dir ? "read" : "write", iop_addr, spu_addr, size);
    iop_sd_transfer_done(ch);
    return (int)n;
}

uint32_t sceSdVoiceTransStatus(int16_t channel, int16_t flag) {
    (void)channel; (void)flag;
    return 1;                                            /* transfers complete immediately */
}

int sceSdBlockTrans(int16_t channel, uint16_t mode, uint32_t iop_addr, uint32_t size, uint32_t start_addr) {
    static int rd;
    int core = channel & 1, dir = mode & 3;
    int ret = 0;
    LOCK();
    global_init();
    sd_adma* a = &g_core[core].adma;
    switch (dir) {
    case TRANS_WRITE:
    case TRANS_WRITE_FROM:
        a->base = iop_addr;
        a->size = size;
        a->loop = (mode & TRANS_LOOP) != 0;
        a->ptr = (dir == TRANS_WRITE_FROM && start_addr - iop_addr < size) ? start_addr : iop_addr;
        a->active = size != 0;
        a->count = a->rd = a->pos = 0;
        break;
    case TRANS_STOP:
        ret = (int)adma_status(a);
        a->active = 0;
        a->count = a->rd = a->pos = 0;
        break;
    default:
        log_once(&rd, "sceSdBlockTrans: read mode not supported");
        break;
    }
    UNLOCK();
    SDLOG("BlockTrans ch%d mode %X iop %06X size %X start %06X\n", core, mode, iop_addr, size, start_addr);
    return ret;
}

uint32_t sceSdBlockTransStatus(int16_t channel, int16_t flag) {
    (void)flag;
    LOCK();
    uint32_t r = adma_status(&g_core[channel & 1].adma);
    UNLOCK();
    return r;
}

/* ---- effects ---------------------------------------------------------------------------------- */

static void clear_work_area(sd_core* k, int mode) {
    uint32_t eea = core_eea(k), n = sd_reverb_size[mode] >> 1;
    uint32_t start = (eea + 1 - n) & SD_RAM_MASK;
    for (uint32_t i = 0; i < n; i++) g_ram[(start + i) & SD_RAM_MASK] = 0;
}

int sceSdSetEffectAttr(int core, const sd_effect_attr* attr) {
    int mode = attr->mode & 0xFF, clear = (attr->mode & 0x100) != 0;
    if (mode >= SD_REVERB_MODES) return SD_ERR_PARAM;
    LOCK();
    global_init();
    sd_core* k = &g_core[core & 1];
    uint16_t* rg = k->rev.regs;
    memcpy(rg, sd_reverb_preset[mode], sizeof k->rev.regs);
    if (mode == 7 || mode == 8) {                         /* ECHO, DELAY: delay and feedback */
        int32_t d = (attr->delay + 1) << 5;
        rg[RV_mLSAME]  = (uint16_t)(2 * d - rg[RV_dAPF1]);
        rg[RV_mRSAME]  = (uint16_t)(d - rg[RV_dAPF2]);
        rg[RV_dLSAME]  = (uint16_t)(rg[RV_dRSAME] + d);
        rg[RV_mLCOMB1] = (uint16_t)(rg[RV_mRCOMB1] + d);
        rg[RV_mRAPF1]  = (uint16_t)(rg[RV_mRAPF2] + d);
        rg[RV_mLAPF1]  = (uint16_t)(rg[RV_mLAPF2] + d);
        rg[RV_vWALL]   = (uint16_t)(attr->feedback * 258);
    }
    k->effect_mode = mode;
    k->evol[0] = attr->depth_L;
    k->evol[1] = attr->depth_R;
    k->esa = (core_eea(k) + 1 - (sd_reverb_size[mode] >> 1)) & SD_RAM_MASK;
    sd_reverb_reset(&k->rev);
    if (clear) clear_work_area(k, mode);
    UNLOCK();
    SDLOG("SetEffectAttr c%d mode %X depth %d/%d delay %d feedback %d\n", core & 1, attr->mode,
          attr->depth_L, attr->depth_R, attr->delay, attr->feedback);
    return 0;
}

int sceSdClearEffectWorkArea(int core, int channel, int effect_mode) {
    (void)channel;
    int mode = effect_mode & 0xFF;
    if (mode >= SD_REVERB_MODES) return SD_ERR_PARAM;
    if (mode == 0) return 0;
    LOCK();
    clear_work_area(&g_core[core & 1], mode);
    UNLOCK();
    return 0;
}
