/* Native sound backend: internals shared by sd.c, sd_reverb.c, audio_out.c and the unit tests.
 * The public (frozen) interface is sd.h; nothing here is called by the IOP side. */
#pragma once
#include "sd.h"

#define SD_RAM_HALFWORDS  (1u << 20)        /* 2 MB of sound memory */
#define SD_RAM_MASK       (SD_RAM_HALFWORDS - 1u)
#define SD_SAMPLE_RATE    48000

/* ---- pulled by the output thread (audio_out.c) ---------------------------------------------- */

/* Render `frames` stereo frames (interleaved L,R int16) at 48 kHz. Advances voice positions
 * (NAX/ENDX), envelopes and the sound-data input. Thread-safe; takes the backend lock in short
 * slices so the emulation thread is never blocked for long. */
void     sd_render(int16_t* out, uint32_t frames);
uint64_t sd_frames_rendered(void);   /* total frames rendered since start (an audio clock) */
void     sd_hold(int on);            /* debug pause: render silence and advance nothing while on */
/* Integration health counters: VoiceTrans writes into the block an audible voice was playing (a
 * stream refill too late or too early), and audible ADPCM blocks with impossible headers. */
void     sd_health(uint64_t* trans_overlaps, uint64_t* bad_blocks);
/* VoiceTrans writes so far: streams refill at a steady rate, so a drop shows a stream that stopped. */
uint64_t sd_voice_trans_writes(void);
/* Times the voices were held because the IOP had not run for longer than the driver limit, and the
 * frames of silence output meanwhile (see "the driver's pace" in sd.c). */
void     sd_driver_holds(uint64_t* holds, uint64_t* frames);
/* The driver limit in ms (0 = never hold). Default: PS2_AUDIO_IOP_LIMIT, else 100. Tests only. */
void     sd_set_driver_limit_ms(uint32_t ms);

/* ---- building blocks, exposed for the unit tests -------------------------------------------- */

/* Decode one 16-byte PS2 ADPCM block (28 samples). hist[0] = previous sample, hist[1] = the one
 * before; both are updated. */
void           sd_adpcm_decode_block(const uint8_t blk[16], int32_t hist[2], int16_t out[28]);
const int16_t* sd_gauss_table(void);   /* 512 entries, 1.15 fixed point */
uint16_t*      sd_ram(void);           /* sound memory (halfwords); tests only, no locking */

/* ---- reverb (sd_reverb.c) -------------------------------------------------------------------- */

/* The 32 reverb parameters in the order of the PS1 SPU registers 1F801DC0h..1F801DFEh (the SPU2
 * keeps this order: FB_SRC_A/B, IIR_ALPHA, ACC_COEF_A..D, IIR_COEF, FB_ALPHA, FB_X, the 20 address
 * registers IIR_DEST_A0..MIX_DEST_B1, IN_COEF_L/R). Address entries are in units of 4 halfwords,
 * exactly as libsd stores them before writing them x4 into the SPU2's address registers. */
enum {
    RV_dAPF1, RV_dAPF2, RV_vIIR, RV_vCOMB1, RV_vCOMB2, RV_vCOMB3, RV_vCOMB4, RV_vWALL, RV_vAPF1, RV_vAPF2,
    RV_mLSAME, RV_mRSAME, RV_mLCOMB1, RV_mRCOMB1, RV_mLCOMB2, RV_mRCOMB2, RV_dLSAME, RV_dRSAME,
    RV_mLDIFF, RV_mRDIFF, RV_mLCOMB3, RV_mRCOMB3, RV_mLCOMB4, RV_mRCOMB4, RV_dLDIFF, RV_dRDIFF,
    RV_mLAPF1, RV_mRAPF1, RV_mLAPF2, RV_mRAPF2, RV_vLIN, RV_vRIN, RV_COUNT
};

typedef struct {
    uint16_t regs[RV_COUNT];
    uint32_t pos;                  /* current buffer offset within the work area (halfwords) */
    int16_t  in_hist[2][64];       /* 48 kHz wet input history for the decimation filter */
    int16_t  out_hist[2][64];      /* zero-stuffed 48 kHz reverb output for the interpolation filter */
    uint32_t hist_pos;
    int32_t  phase;                /* 48 kHz sample parity: the reverb runs on every other sample */
} sd_reverb;

#define SD_REVERB_MODES 10
extern const uint16_t sd_reverb_preset[SD_REVERB_MODES][RV_COUNT];
extern const uint32_t sd_reverb_size[SD_REVERB_MODES];   /* work area size in bytes */

void sd_reverb_init(void);         /* builds the resampling filter */
void sd_reverb_reset(sd_reverb* r);
/* One 48 kHz sample: wet input in, reverb output (before EVOL) out. `esa`/`eea` are halfword
 * addresses of the work area (inclusive); `enabled` = ATTR effect bit (when 0, nothing is read or
 * written and the output decays through the interpolation filter). */
void sd_reverb_sample(sd_reverb* r, uint16_t* ram, uint32_t esa, uint32_t eea, int enabled,
                      int32_t in_l, int32_t in_r, int32_t* out_l, int32_t* out_r);
