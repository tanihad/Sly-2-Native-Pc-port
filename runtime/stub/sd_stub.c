/* Stand-in for the native sound backend (sd.h) while the real mixer (runtime/sd.c) is not in the
 * tree. It produces no sound, but it keeps what 989snd can observe consistent: sound memory
 * receives the transfers, keyed-on voices advance through their ADPCM blocks in real time
 * (NAX, ENDX, loop flags), so code that polls a voice's position (989snd's movie sound, VAG
 * streams) sees playback progress. Transfers complete at once, with the completion interrupt.
 *
 * Linked by scripts/build_game.py only when runtime/sd.c does not exist.
 * PS2_LOG_SD=1 logs every call; otherwise the first few of each kind and a summary every 5 s.
 * Entry encodings follow libsd's sdmacro.h: (register << 8) | (voice << 1) | core, with 0x40
 * set for voice address registers. */
#include "../sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int log_all = -1;
static unsigned n_param, n_switch, n_addr, n_kon, n_koff, n_vtrans, n_btrans;
static uint32_t vtrans_bytes;
static uint16_t params[0x10000];
static uint32_t switches[0x100][2];
static uint8_t spu_ram[0x200000];

typedef struct {
    int on, released;
    uint32_t ssa, lsa, nax;          /* byte addresses in sound memory */
    double frac;                     /* samples into the current 28-sample block */
    int64_t last;                    /* QPC of the last position update */
} voice;
static voice voices[2][24];
static uint32_t endx[2];
static LARGE_INTEGER qpc_freq;

static int verbose(unsigned count) {
    if (log_all < 0) log_all = getenv("PS2_LOG_SD") != NULL;
    return log_all || count <= 8;
}

static void summary(void) {
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 5000) return;
    last = now;
    fprintf(stderr, "[sd-stub] totals: %u param, %u switch, %u addr writes; %u key-ons, %u key-offs; "
                    "%u voice transfers (%u bytes), %u block transfers\n",
            n_param, n_switch, n_addr, n_kon, n_koff, n_vtrans, vtrans_bytes, n_btrans);
}

static int64_t now_qpc(void) {
    LARGE_INTEGER t;
    if (!qpc_freq.QuadPart) QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* Advance a voice to the present: 48 kHz output, pitch 0x1000 = one sample per output sample. */
static void advance(int core, int v) {
    voice* vo = &voices[core][v];
    int64_t t = now_qpc();
    if (!vo->on) { vo->last = t; return; }
    uint16_t pitch = params[0x0200 | (v << 1) | core];
    double samples = (double)(t - vo->last) * 48000.0 / (double)qpc_freq.QuadPart * pitch / 4096.0;
    vo->last = t;
    vo->frac += samples;
    for (int guard = 0; vo->frac >= 28.0 && guard < 100000; guard++) {
        vo->frac -= 28.0;
        uint8_t flags = spu_ram[(vo->nax + 1) & 0x1FFFFF];
        if (flags & 4) vo->lsa = vo->nax;                 /* loop start */
        if (flags & 1) {                                  /* loop end: jump, and stop unless repeat */
            endx[core] |= 1u << v;
            if (!(flags & 2)) vo->released = 1;
            vo->nax = vo->lsa;
        } else {
            vo->nax = (vo->nax + 16) & 0x1FFFFF;
        }
    }
}

int sceSdInit(int flag) {
    fprintf(stderr, "[sd-stub] sceSdInit(%d): no sound backend linked (runtime/sd.c missing), positions simulated\n", flag);
    memset(params, 0, sizeof params);
    memset(voices, 0, sizeof voices);
    return 0;
}

void sceSdSetParam(uint16_t entry, uint16_t value) {
    if ((entry >> 8) == 0x02 && !(entry & 0x40)) advance(entry & 1, (entry >> 1) & 31);   /* pitch change */
    params[entry] = value;
    if (verbose(++n_param)) fprintf(stderr, "[sd-stub] SetParam(0x%04X, 0x%04X)\n", entry, value);
    summary();
}

uint16_t sceSdGetParam(uint16_t entry) {
    if ((entry >> 8) == 0x05) {                          /* SD_VP_ENVX: sounding until released */
        voice* vo = &voices[entry & 1][(entry >> 1) & 31];
        advance(entry & 1, (entry >> 1) & 31);
        return vo->on && !vo->released ? 0x7FFF : 0;
    }
    return params[entry];
}

/* Switch registers: 0x15 = SD_S_KON, 0x16 = SD_S_KOFF, 0x17 = SD_S_ENDX. */
void sceSdSetSwitch(uint16_t entry, uint32_t value) {
    int core = entry & 1;
    uint8_t reg = (uint8_t)(entry >> 8);
    switches[reg][core] = value;
    if (reg == 0x15 && value) {
        n_kon++;
        if (verbose(n_kon)) fprintf(stderr, "[sd-stub] key on: core %d voices 0x%06X\n", core, value & 0xFFFFFF);
        for (int v = 0; v < 24; v++) {
            if (!(value & (1u << v))) continue;
            voice* vo = &voices[core][v];
            vo->on = 1; vo->released = 0; vo->frac = 0;
            vo->nax = vo->ssa; vo->lsa = vo->ssa;
            vo->last = now_qpc();
            endx[core] &= ~(1u << v);
        }
    } else if (reg == 0x16 && value) {
        n_koff++;
        if (verbose(n_koff)) fprintf(stderr, "[sd-stub] key off: core %d voices 0x%06X\n", core, value & 0xFFFFFF);
        for (int v = 0; v < 24; v++)
            if (value & (1u << v)) { advance(core, v); voices[core][v].released = 1; }
    } else if (verbose(++n_switch)) {
        fprintf(stderr, "[sd-stub] SetSwitch(0x%04X, 0x%06X)\n", entry, value);
    }
    summary();
}

uint32_t sceSdGetSwitch(uint16_t entry) {
    int core = entry & 1;
    if ((entry >> 8) == 0x17) {
        for (int v = 0; v < 24; v++) advance(core, v);
        return endx[core];
    }
    return switches[(entry >> 8) & 0xFF][core];
}

void sceSdSetAddr(uint16_t entry, uint32_t value) {
    int core = entry & 1, v = (entry >> 1) & 31;
    if (verbose(++n_addr)) fprintf(stderr, "[sd-stub] SetAddr(0x%04X, 0x%06X)\n", entry, value);
    if (!(entry & 0x40) || v >= 24) return;
    switch (entry >> 8) {
    case 0x20: voices[core][v].ssa = value & 0x1FFFF0; break;   /* SD_VA_SSA */
    case 0x21: advance(core, v); voices[core][v].lsa = value & 0x1FFFF0; break;   /* SD_VA_LSAX */
    default: break;
    }
}

uint32_t sceSdGetAddr(uint16_t entry) {
    int core = entry & 1, v = (entry >> 1) & 31;
    if ((entry & 0x40) && v < 24) {
        voice* vo = &voices[core][v];
        switch (entry >> 8) {
        case 0x20: return vo->ssa;
        case 0x21: advance(core, v); return vo->lsa;
        case 0x22: advance(core, v); return vo->nax;           /* SD_VA_NAX */
        default: break;
        }
    }
    return 0;
}

void sceSdSetCoreAttr(uint16_t entry, uint16_t value) {
    fprintf(stderr, "[sd-stub] SetCoreAttr(0x%04X, 0x%04X)\n", entry, value);
}

/* Equal temperament around the centre note: 0x1000 plays the sample at its recorded rate. */
uint16_t sceSdNote2Pitch(uint16_t center_note, uint16_t center_fine, uint16_t note, int16_t fine) {
    double semis = (double)((int)note - (int)center_note) + ((double)fine - (double)center_fine) / 128.0;
    double p = 4096.0;
    for (; semis >= 12; semis -= 12) p *= 2;
    for (; semis < 0; semis += 12) p /= 2;
    static const double step = 1.0594630943592953;
    while (semis >= 1) { p *= step; semis -= 1; }
    p *= 1.0 + semis * (step - 1.0);
    return p > 0x3FFF ? 0x3FFF : (uint16_t)p;
}

int sceSdVoiceTrans(int16_t channel, uint16_t mode, uint32_t iop_addr, uint32_t spu_addr, uint32_t size) {
    n_vtrans++;
    vtrans_bytes += size;
    if (verbose(n_vtrans))
        fprintf(stderr, "[sd-stub] VoiceTrans(ch %d, mode 0x%X, IOP 0x%08X -> SPU 0x%06X, %u bytes)\n", channel, mode,
                iop_addr, spu_addr, size);
    uint8_t* src = iop_mem(iop_addr);
    uint8_t* end = iop_mem(iop_addr + (size ? size - 1 : 0));
    if (!src || !end) fprintf(stderr, "[sd-stub] VoiceTrans source 0x%08X+%u is not IOP RAM\n", iop_addr, size);
    else if (!(mode & 1)) {                                    /* SD_TRANS_WRITE */
        for (uint32_t i = 0; i < size; i++) spu_ram[(spu_addr + i) & 0x1FFFFF] = src[i];
    } else {                                                   /* SD_TRANS_READ */
        for (uint32_t i = 0; i < size; i++) src[i] = spu_ram[(spu_addr + i) & 0x1FFFFF];
    }
    iop_sd_transfer_done(channel);
    summary();
    return (int)size;
}

uint32_t sceSdVoiceTransStatus(int16_t channel, int16_t flag) { (void)channel; (void)flag; return 1; }

int sceSdBlockTrans(int16_t channel, uint16_t mode, uint32_t iop_addr, uint32_t size, uint32_t start_addr) {
    n_btrans++;
    if (verbose(n_btrans))
        fprintf(stderr, "[sd-stub] BlockTrans(ch %d, mode 0x%X, IOP 0x%08X, %u bytes, start 0x%X)\n", channel, mode,
                iop_addr, size, start_addr);
    iop_sd_transfer_done(channel);
    return 0;
}

uint32_t sceSdBlockTransStatus(int16_t channel, int16_t flag) { (void)channel; (void)flag; return 0; }

int sceSdSetEffectAttr(int core, const sd_effect_attr* attr) {
    fprintf(stderr, "[sd-stub] SetEffectAttr(core %d, mode %d, depth %d/%d, delay %d, feedback %d)\n", core,
            attr->mode, attr->depth_L, attr->depth_R, attr->delay, attr->feedback);
    return 0;
}

int sceSdClearEffectWorkArea(int core, int channel, int effect_mode) {
    fprintf(stderr, "[sd-stub] ClearEffectWorkArea(core %d, ch %d, mode %d)\n", core, channel, effect_mode);
    return 0;
}

void audio_init(void) {}
