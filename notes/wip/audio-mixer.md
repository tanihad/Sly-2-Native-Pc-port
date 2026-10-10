# WIP: audio-mixer (native libsd backend: runtime/sd.c + audio_out.c)

**Goal**: implement runtime/sd.h natively (voice engine, reverb, ADMA, transfers) + WASAPI output.

## Done
- Research (see Findings).
- runtime/sd_internal.h, sd.c (API, voices, ADMA, mixing), sd_reverb.c (presets + algorithm + FIR),
  audio_out.c/.h (WASAPI, null sink, WAV, test tone).
- tests/unit/sd/run.ps1: test_sd (674 checks) + test_audio_out (12 checks) all pass; WASAPI works here.

## In progress
- Game build check, notes/audio-mixer.md.

## Next
1. Build the game (build_game.py) to confirm sd.c/audio_out.c link without new libs.
2. Write notes/audio-mixer.md (design, sources, integration notes), mark this file complete, commit.

## Findings (checked against the game's own libsd 3.3 in the ELF .irx section, `irx_tool.py dis sound ...`)
- Entry decoding (SetParam 0x3690): param = entry >> 8, core = entry & 1, voice = (entry >> 1) & 0x1F;
  bit 0x80 only selects the register stride of the shared volume block (MVOL.. at 0x760 + core*0x28).
  Register table at libsd 0x4980: 0-7 voice regs, 8 MMIX, 9..0x12 MVOLL/R EVOLL/R AVOLL/R BVOLL/R MVOLXL/R,
  0x13 PMON 0x14 NON 0x15 KON 0x16 KOFF 0x17 ENDX 0x18 VMIXL 0x19 VMIXEL 0x1A VMIXR 0x1B VMIXER,
  0x1C ESA 0x1D EEA 0x1E TSA 0x1F IRQA 0x20 SSA 0x21 LSAX 0x22 NAX (0x40 bit in 989snd's entries ignored).
- SetAddr: hi = v >> 17, lo = (v >> 1) & 0xFFF8 (16-byte granularity); EEA writes only hi.
  GetAddr: (hi << 17) | (lo << 1); EEA reads (hi << 17) | 0x1FFFF.
- SetSwitch: lo 16 bits + (v >> 16) & 0xFF; GetSwitch: lo | hi << 16.
- sceSdInit(0): ATTR c0 0xC000 / c1 0xC001, SPDIF 0xC032, VMIX*/VMIXE* all on, MMIX c0 0xFF0 c1 0xFFC,
  MVOL 0, EVOL 0, EEA hi c0 0xE c1 0xF, AVOL c0 0 / c1 0x7FFF, BVOL 0; every voice VOL 0, PITCH 0x3FFF,
  ADSR 0, SSA = 0x5000 bytes where a silent loop block (header 00 07) is written; KON all, KOFF all, ENDX=0.
- SetCoreAttr: 2 effect (ATTR bit 7), 4 irq (bit 6), 6 mute (bit 14), 8 noise clock (bits 8-13, 6 bits), 0xA SPDIF.
- Note2Pitch tables = floor(0x8000 * 2^(n/12)) and floor(0x8000 * 2^(k/1536)) (verified all 140 entries).
- SetEffectAttr: sizes (8-byte units) 2,4D8,3E8,908,DFC,15BC,1ED8,3008,3008,780; ESA = EEA - size*8 + 1;
  32 halfwords per preset = PS1 reverb registers in PS1 order (dAPF1..vRIN), address values written x4
  (halfword offsets); ECHO/DELAY: delay d -> (d+1)*64 / *32 address rewrites, vWALL = feedback*258;
  EVOL = depth; mode|0x100 clears the work area, mode|0x200 selects DMA channel 1 for that clear.
- VoiceTrans: size rounded up to 64 bytes and returned (989snd treats return < size as an error).
- 989snd: MMIX c0 0xF00, c1 0xF0C (bits 8-11 voice dry/wet, 2-3 external input dry); movie sound uses
  BlockTrans(ch, 0x13 = WRITE_FROM|LOOP, buf, size, start) + BVOL + MMIX |= 0xC0, STOP returns status & 0xFFFFFF;
  streams set LSAX to 0x5000 before KON and rewrite LSAX (twice) while playing; polls NAX (3 reads until stable).
