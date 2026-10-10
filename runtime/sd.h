/* Native sound backend: Sony libsd's API (the calls 989snd makes) implemented on a PC voice mixer.
 *
 * FROZEN INTERFACE between the IOP side (recompiled 989snd/spiop + native IOP libraries, which
 * translate the guest's register arguments into these calls) and the mixer (sd.c, audio_out.c).
 *
 * Semantics follow Sony's libsd: entry codes, parameter numbering and units are those of
 * libsd.h/sdmacro.h (e.g. voice parameters are (param | core | voice << 1), SD_VP_VOLL, SD_S_KON,
 * SD_VA_SSA...). Addresses into "sound memory" are SPU2 RAM byte addresses (2 MB, which libsd
 * passes as bytes); IOP addresses are guest IOP RAM addresses (resolve with iop_mem()).
 *
 * Threading: every sd_* / sceSd* call comes from the emulation thread. Audio is rendered on the
 * output thread (audio_out.c), which reads voice state under the backend's own lock; voice
 * positions (NAX, ENDX) therefore advance with real playback, which 989snd's streaming polls.
 */
#pragma once
#include <stdint.h>

/* ---- libsd API (989snd imports libsd functions 4..26 listed in notes/audio-plan.md) ---------- */

int      sceSdInit(int flag);
void     sceSdSetParam(uint16_t entry, uint16_t value);
uint16_t sceSdGetParam(uint16_t entry);
void     sceSdSetSwitch(uint16_t entry, uint32_t value);
uint32_t sceSdGetSwitch(uint16_t entry);
void     sceSdSetAddr(uint16_t entry, uint32_t value);
uint32_t sceSdGetAddr(uint16_t entry);
void     sceSdSetCoreAttr(uint16_t entry, uint16_t value);
uint16_t sceSdNote2Pitch(uint16_t center_note, uint16_t center_fine, uint16_t note, int16_t fine);
/* Transfers between IOP RAM and sound memory (mode: SD_TRANS_WRITE/READ, SD_TRANS_MODE_DMA/IO).
 * They complete immediately in the mixer's memory; completion is announced through
 * iop_sd_transfer_done() (implemented by the IOP side), never by calling guest code directly. */
int      sceSdVoiceTrans(int16_t channel, uint16_t mode, uint32_t iop_addr, uint32_t spu_addr, uint32_t size);
uint32_t sceSdVoiceTransStatus(int16_t channel, int16_t flag);
/* BlockTrans: streaming PCM into a core's sound-data input (ADMA, used for movie sound), or a
 * one-shot block transfer, per Sony's mode bits; start_addr only for SD_BLOCK_START_ADDR modes. */
int      sceSdBlockTrans(int16_t channel, uint16_t mode, uint32_t iop_addr, uint32_t size, uint32_t start_addr);
uint32_t sceSdBlockTransStatus(int16_t channel, int16_t flag);

typedef struct {
    uint16_t core;               /* unused by libsd 3.x callers; kept for layout */
    int32_t  mode;               /* SD_REV_MODE_* (plus SD_REV_MODE_CLEAR_WA) */
    int16_t  depth_L, depth_R;
    int32_t  delay, feedback;
} sd_effect_attr;
int      sceSdSetEffectAttr(int core, const sd_effect_attr* attr);
int      sceSdClearEffectWorkArea(int core, int channel, int effect_mode);

/* ---- provided by the IOP side ----------------------------------------------------------------- */

/* A sceSdVoiceTrans/BlockTrans on `channel` (0/1) finished (or an ADMA half-buffer was consumed):
 * the IOP side runs the guest's transfer-interrupt handler (sceSdSetTransIntrHandler) at its next
 * safe point. May be called from the audio thread for ADMA; must be thread-safe. */
void     iop_sd_transfer_done(int channel);
uint8_t* iop_mem(uint32_t iop_addr);     /* guest IOP RAM (sif.c) */

/* ---- output (audio_out.c) ---------------------------------------------------------------------- */

void     audio_init(void);               /* start WASAPI output (48 kHz stereo) on its own thread */
