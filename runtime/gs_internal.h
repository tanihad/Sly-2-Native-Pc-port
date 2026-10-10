/* GS internals shared by gs_mem.c (local memory, transfers, CLUT, texel fetch) and
 * gs_draw.c / gs.c (registers, primitives, rasterizer, display).
 *
 * FROZEN INTERFACE between the two GS halves; see hw.h for the rules. */
#pragma once
#include "hw.h"

#define GS_VRAM_SIZE (4u * 1024u * 1024u)
extern uint8_t gs_vram[GS_VRAM_SIZE];

/* Pixel storage modes (PSM field values) */
enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A
};

/* ---- raw pixel access (gs_mem.c) -----------------------------------------------------------------
 * bp: base pointer in 256-byte block units (TBP0/CBP/DBP/SBP units; FBP and ZBP registers are in
 *     8 KB page units, so callers pass FBP*32 and ZBP*32).
 * bw: buffer width in 64-pixel units (FBW/TBW/DBW/SBW).
 * Values are the stored bits of that format, right-aligned: 32 bits for *32, 24 bits for *24
 * (writes leave the top byte of the 32-bit word untouched), 16 bits for the 16 and 16S formats, 8 or 4 bits for
 * the T8/T4 (incl. 8H/4HL/4HH, which live inside 32-bit pixels) formats.
 * Coordinates wrap inside the 4 MB memory as the hardware does. */
uint32_t gs_mem_read(int psm, uint32_t bp, uint32_t bw, int x, int y);
void     gs_mem_write(int psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t value);

/* ---- textures (gs_mem.c) -------------------------------------------------------------------------- */

typedef struct {                 /* decoded TEX0 */
    uint32_t tbp0, tbw, psm, tw, th, tcc, tfx, cbp, cpsm, csm, csa, cld;
} gs_tex0;

typedef struct {                 /* decoded TEXA */
    uint32_t ta0, aem, ta1;
} gs_texa;

typedef struct {                 /* decoded TEXCLUT (used with CSM2) */
    uint32_t cbw, cou, cov;
} gs_texclut;

/* Handle CLUT loading for a TEX0 write (the CLD field rules, CBP0/CBP1 compare registers, CSM1
 * and CSM2 layouts). Called by gs_draw.c every time TEX0_1/TEX0_2 is written (and TEX2). */
void gs_clut_on_tex0(const gs_tex0* t, const gs_texclut* tc);

/* One texel at integer coordinates (already wrapped/clamped by the caller), expanded to
 * 0xAABBGGRR with GS alpha semantics (0x80 = 1.0): indexed formats go through the CLUT,
 * 24/16-bit formats through TEXA, and TCC=0 is NOT applied here (the caller decides). */
uint32_t gs_tex_fetch(const gs_tex0* t, const gs_texa* ta, int u, int v);

/* The CLUT entry an indexed texel with value `index` (0-255, or 0-15 for 4-bit formats) of texture
 * t gets, expanded like gs_tex_fetch (CSA, CPSM and TEXA applied). Lets gs_draw build a palette
 * snapshot instead of fetching one texel per index. */
uint32_t gs_clut_lookup(const gs_tex0* t, const gs_texa* ta, uint32_t index);

/* ---- transfers (gs_mem.c) -------------------------------------------------------------------------
 * Called by gs_draw.c when TRXDIR is written, with the current raw BITBLTBUF/TRXPOS/TRXREG.
 * dir 0 = host->local (data then arrives via gs_mem_xfer_write), 1 = local->host (data leaves
 * via gs_mem_xfer_read), 2 = local->local (performed immediately), 3 = off. */
void     gs_mem_xfer_start(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint32_t dir);
void     gs_mem_xfer_write(const void* data, uint32_t bytes);
uint32_t gs_mem_xfer_read(void* data, uint32_t bytes);
int      gs_mem_xfer_active(void);       /* a host<->local transfer still expects/has data */
