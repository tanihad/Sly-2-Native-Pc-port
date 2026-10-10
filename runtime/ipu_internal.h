/* IPU internals shared by ipu.c, ipu_vlc.c, ipu_idct.c and the IPU unit tests.
 *
 * Bitstream convention used throughout: a 32-bit window holds the next bits of the stream, the
 * first (oldest) bit in bit 31. The IPU reads its input FIFO in memory byte order, most
 * significant bit of each byte first, i.e. exactly the byte order of an MPEG file. */
#pragma once
#include <stdint.h>

/* ---- scan orders (ISO/IEC 13818-2 7.3.1): raster index (v * 8 + u) of each scan position ------- */

extern const uint8_t ipu_scan[2][64];       /* [0] zigzag (AS = 0), [1] alternate scan (AS = 1) */

/* ---- variable-length codes (ISO/IEC 13818-2 Annex B) ------------------------------------------- */

/* A decoded symbol: len = code length in bits (0 = no valid code starts with these bits). */
typedef struct { int16_t val; uint8_t len; uint8_t sub; } ipu_vlc_entry;

/* Two-level lookup: the first 8 bits index l1; longer codes continue in l2. */
typedef struct {
    ipu_vlc_entry l1[256];
    ipu_vlc_entry l2[1024];
    int l2_used;
    int max_len;
} ipu_vlc_table;

typedef struct { const char* code; int16_t val; } ipu_vlc_code;   /* code: '0'/'1', spaces ignored */

/* Build a table; returns 0 on success, -1 if the codes are not prefix-free or too long. */
int ipu_vlc_build(ipu_vlc_table* t, const ipu_vlc_code* codes, int n);

static inline ipu_vlc_entry ipu_vlc_get(const ipu_vlc_table* t, uint32_t bits) {
    ipu_vlc_entry e = t->l1[bits >> 24];
    if (e.sub) e = t->l2[e.val + ((bits << 8) >> (32 - e.sub))];
    return e;
}

/* Code lists, exported for the unit tests (count in the matching *_n). */
enum { IPU_VLC_MBAI, IPU_VLC_MBT_I, IPU_VLC_MBT_P, IPU_VLC_MBT_B, IPU_VLC_MBT_D, IPU_VLC_MOTION,
       IPU_VLC_DMV, IPU_VLC_CBP, IPU_VLC_DC_LUMA, IPU_VLC_DC_CHROMA, IPU_VLC_DCT0, IPU_VLC_DCT1,
       IPU_VLC_COUNT };
extern const ipu_vlc_code* const ipu_vlc_codes[IPU_VLC_COUNT];
extern const int ipu_vlc_code_count[IPU_VLC_COUNT];
extern ipu_vlc_table ipu_vlc_tables[IPU_VLC_COUNT];
void ipu_vlc_init(void);                     /* builds ipu_vlc_tables (idempotent) */

/* Macroblock address increment values beyond 1..33 (as the IPU's VDEC reports them). */
enum { IPU_MBAI_STUFFING = 0x22, IPU_MBAI_ESCAPE = 0x23 };

/* macroblock_type flags as VDEC reports them (bit order of Tables B-2..B-4). */
enum { IPU_MB_INTRA = 1, IPU_MB_PATTERN = 2, IPU_MB_BACKWARD = 4, IPU_MB_FORWARD = 8, IPU_MB_QUANT = 16 };

/* DCT coefficient table entries: val = run | (level << 8), or one of these. Codes are listed
 * without their sign bit; a coefficient's sign is the bit after the code. */
enum { IPU_DCT_EOB = -1, IPU_DCT_ESCAPE = -2 };

/* Decode one DCT coefficient symbol from a 32-bit window (escape included).
 * table: 0 = B-14, 1 = B-15. first: first coefficient of a non-intra block (B-14 "1s" = 0,1).
 * Returns the total length in bits (0 = invalid code). *run = -1 for end of block. */
int ipu_dct_symbol(uint32_t bits, int table, int first, int mpeg1, int* run, int* level);

/* ---- inverse DCT -------------------------------------------------------------------------------- */

/* 8x8 inverse DCT, IEEE 1180-1990 accurate. in: 64 coefficients in raster order (v * 8 + u), each in
 * [-2048, 2047]. out: 8 rows of 8 samples, row r at out[r * stride], saturated to [-256, 255]. */
void ipu_idct(const int16_t in[64], int16_t* out, int stride);

/* ---- colour space conversion --------------------------------------------------------------------- */

/* Convert one RAW8 macroblock (Y 16x16, then Cb 8x8, then Cr 8x8, 384 bytes) to RGB32 (1024 bytes,
 * R G B A per pixel) or RGB16 (512 bytes, A1 B5 G5 R5 little-endian). */
void ipu_csc_mb(const uint8_t* raw8, uint8_t* out, int rgb16, int dither, int sgn, int th0, int th1);

/* PACK one RGB32 macroblock (1024 bytes) to RGB16 (512 bytes) or INDX4 (128 bytes, via clut). */
void ipu_pack_mb(const uint8_t* rgb32, uint8_t* out, int indx4, int dither, const uint16_t clut[16]);
