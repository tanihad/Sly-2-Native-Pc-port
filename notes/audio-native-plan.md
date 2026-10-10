# Audio: native plan (decided 2026-10-10)

Decision by the project owner: audio must be native, not emulated. The research in
`notes/audio-plan.md` (branch hw/audio) compared reimplementing 989snd by hand (A) with running
the IOP sound modules on an emulated IOP + SPU2 (B). We take a third route that matches the rest
of the port:

## Approach C: recompile the IOP sound driver, replace the sound chip API natively

1. **Statically recompile the IOP sound modules to C**: 989snd_Library (29k instructions) and
   Sucker Punch's spiop (5.4k), taken from the executable's .irx section and relocated to a fixed
   IOP base address at build time. The recompiler already handles R5900 code; the IOP's R3000 code
   is a MIPS I subset. Differences to handle: IOP memory accessors (separate 2 MB address space),
   32-bit semantics, load delay slots (compiler-generated code fills them, so no change expected).
2. **Imports become native C**: the import stubs (`jr $ra; addiu $zero,$zero,N`) are replaced by
   direct calls into native implementations of the IOP libraries the modules use (threads,
   semaphores, event flags, sysmem, timrman's 240 Hz timer, sifcmd RPC server, cdvdman reads from
   the disc image, sysclib, printf). List: `notes/audio-plan.md` section 1.3.
3. **libsd is not run at all**: its 16 `sceSd*` functions used by 989snd (Init, Set/GetParam,
   Set/GetSwitch, Set/GetAddr, SetCoreAttr, Note2Pitch, VoiceTrans, BlockTrans, transfer status,
   effect attributes, transfer interrupt handler) are implemented natively on a PC voice mixer:
   2 x 24 voices, PS2 ADPCM sample decoding, pitch, ADSR envelopes, volumes, reverb, 48 kHz,
   output through WASAPI. This is the game's sound "backend", written for PC.
4. **IOP threads** (989snd's RPC threads, 240 Hz tick, stream threads, spiop's threads) run as
   fibers scheduled at the runtime's safe points, like the EE threads; they are cooperative and
   block on semaphores/RPC queues/sleep.
5. The EE side is unchanged: the game's 989snd RPC calls (servers 0x123456/0x123457) are served
   by the recompiled 989snd instead of `runtime/snd.c`.

## Movies

The IPU implementation is a native MPEG-2 decoder behind the IPU command interface (no cycle
emulation); it already decodes the intro movie from the disc correctly. Movie audio goes through
989snd's movie-sound commands, so it comes with approach C.
