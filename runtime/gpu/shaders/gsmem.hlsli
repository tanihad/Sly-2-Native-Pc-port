// GS local memory addressing on the GPU (notes/d3d11-renderer.md 3.2 swz_tab): the page tables of
// runtime/gs_mem_swizzle.h uploaded as data (gpu_exec.c builds swz_tab from gs_swz_* at start), and
// the address formulas of gs_mem_swizzle.h (gs_addr32/16/8/4), so the GPU reads and writes vram_buf
// and targets exactly where the software renderer reads and writes gs_vram.
#ifndef GPU_GSMEM_HLSLI
#define GPU_GSMEM_HLSLI

// swz_tab: u32 entries, tables at these entry offsets
#define SWZ_CT32  0u                     // [(y & 31) << 6 | (x & 63)] -> word in page
#define SWZ_Z32   2048u
#define SWZ_CT16  4096u                  // [(y & 63) << 6 | (x & 63)] -> half in page
#define SWZ_CT16S 8192u
#define SWZ_Z16   12288u
#define SWZ_Z16S  16384u
#define SWZ_T8    20480u                 // [(y & 63) << 7 | (x & 127)] -> byte in page
#define SWZ_T4    28672u                 // [(y & 127) << 7 | (x & 127)] -> nibble in page
#define SWZ_ENTRIES 45056u

// PSM values (runtime/gs_internal.h)
#define PSMCT32  0x00u
#define PSMCT24  0x01u
#define PSMCT16  0x02u
#define PSMCT16S 0x0Au
#define PSMT8    0x13u
#define PSMT4    0x14u
#define PSMT8H   0x1Bu
#define PSMT4HL  0x24u
#define PSMT4HH  0x2Cu
#define PSMZ32   0x30u
#define PSMZ24   0x31u
#define PSMZ16   0x32u
#define PSMZ16S  0x3Au

ByteAddressBuffer swz : register(t6);

uint swz_at(uint tab, uint i) { return swz.Load((tab + i) * 4u); }

// word address (0..1M-1) of pixel (x, y) (0..2047) of a 32-bit buffer at bp (blocks), bw (64-px units)
uint gs_addr32(uint tab, uint bp, uint bw, uint x, uint y) {
    return (bp * 64u + ((((y >> 5) * bw + (x >> 6)) << 11)) + swz_at(tab, ((y & 31u) << 6) | (x & 63u))) & 0xFFFFFu;
}
// halfword address of a 16-bit buffer's pixel
uint gs_addr16(uint tab, uint bp, uint bw, uint x, uint y) {
    return (bp * 128u + ((((y >> 6) * bw + (x >> 6)) << 12)) + swz_at(tab, ((y & 63u) << 6) | (x & 63u))) & 0x1FFFFFu;
}
uint gs_bw8(uint bw) { return bw > 1u ? bw >> 1 : bw; }
// byte address of a PSMT8 texel, nibble address of a PSMT4 texel
uint gs_addr8(uint bp, uint bw, uint x, uint y) {
    return (bp * 256u + ((((y >> 6) * gs_bw8(bw) + (x >> 7)) << 13)) + swz_at(SWZ_T8, ((y & 63u) << 7) | (x & 127u))) & 0x3FFFFFu;
}
uint gs_addr4(uint bp, uint bw, uint x, uint y) {
    return (bp * 512u + ((((y >> 7) * gs_bw8(bw) + (x >> 7)) << 14)) + swz_at(SWZ_T4, ((y & 127u) << 7) | (x & 127u))) & 0x7FFFFFu;
}

// the table of a frame / Z buffer format (32- or 16-bit layouts)
uint swz_tab_of(uint psm) {
    switch (psm) {
    case PSMZ32: case PSMZ24: return SWZ_Z32;
    case PSMCT16: return SWZ_CT16;
    case PSMCT16S: return SWZ_CT16S;
    case PSMZ16: return SWZ_Z16;
    case PSMZ16S: return SWZ_Z16S;
    default: return SWZ_CT32;
    }
}
bool psm_is16(uint psm) { return psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S; }

#endif
