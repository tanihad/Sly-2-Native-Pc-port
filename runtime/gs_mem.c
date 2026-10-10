/* GS local memory, transfers, CLUT, texel fetch. STUB: to be implemented (owner: gs-mem agent). */
#include "gs_internal.h"

uint8_t gs_vram[GS_VRAM_SIZE];

uint32_t gs_mem_read(int psm, uint32_t bp, uint32_t bw, int x, int y) { (void)psm; (void)bp; (void)bw; (void)x; (void)y; return 0; }
void gs_mem_write(int psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t value) { (void)psm; (void)bp; (void)bw; (void)x; (void)y; (void)value; }
void gs_clut_on_tex0(const gs_tex0* t, const gs_texclut* tc) { (void)t; (void)tc; }
uint32_t gs_tex_fetch(const gs_tex0* t, const gs_texa* ta, int u, int v) { (void)t; (void)ta; (void)u; (void)v; return 0; }
void gs_mem_xfer_start(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint32_t dir) { (void)bitbltbuf; (void)trxpos; (void)trxreg; (void)dir; }
void gs_mem_xfer_write(const void* data, uint32_t bytes) { (void)data; (void)bytes; }
uint32_t gs_mem_xfer_read(void* data, uint32_t bytes) { memset(data, 0, bytes); return bytes; }
int gs_mem_xfer_active(void) { return 0; }
