# WIP: audio-iop (IOP side of native audio, approach C)

**Status: complete** (2026-10-10). Design and usage: `notes/iop-native.md`.

**Goal**: statically recompile 989snd_Library + spiop (IRX) to C, run them on native IOP kernel
libraries (fiber threads, sema, timers, sifcmd server, cdvdman...) so the EE's 989snd RPCs are
served by the real driver; libsd calls go to runtime/sd.h.

## Done
- recomp/irx.py: IRX parser + relocator (R_MIPS_32/26/HI16/LO16), import/export tables.
- recomp/iop.py (`python -m recomp iop <elf> -o dir`): discovery + MIPS I codegen; 634 functions,
  113 stubs, 0 untranslated, 0 load-delay hazards. build_game.py runs it.
- runtime/iop.h (ctx, accessors, memory map), iop_internal.h, iop_kernel.c, iop_libs.c,
  iop_sif.c (incl. ISO 9660 sceCdSearchFile), iop_loader.c; runtime/stub/sd_stub.c (voice
  positions simulated; linked only if runtime/sd.c is absent, via scripts/paths.runtime_sources).
- sif.c (IOP servers, deferred replies, sysmem RPC buffers), iop_modules.c (native load,
  IOPHEAP on sysmem), cdvd.c (cdvd_read_host), ps2_runtime.c (iop_init/iop_service/poll flag).
- Game: banner, both servers registered, bank load -> handle 0x001D1F00, tick 240.0/s, IOP
  0.2% CPU, sd stub gets voice transfers/key-ons; with PS2_INPUT reaches title + episode intro
  like the old build. PS2_AUDIO=0 behaves like the old build.
- Tests: tests/unit/iop_recomp, tests/unit/iop_kernel; tests/run_tests.py, pad/mc/timer pass.

## Next (for whoever continues)
1. When runtime/sd.c (mixer) lands: call audio_init() in ps2_runtime.c main; check that NAX/ENDX
   advance (movie player polls 989snd command 91 = NAX of the movie voices).
2. Play into a level and watch level bank loads (0x123457 fn 3) and VAG streams.

## Findings / traps
- 989snd .text ends with data (0x1C934-0x1C9C4, a table after the import stubs): code ranges
  stop at the first import/export table; export table is in .text too (130 R_MIPS_32 relocs).
- No gp-relative code, no load-delay hazards; `break` used by GCC div checks; no `add/addi/sub`
  in code (only in the data above).
- Module start of 989snd does not block (creates 2 RPC threads); spiop start registers 'SUKR'
  through snd989 export 93 (guest-to-guest call). The EE passes no module args to either.
- spiop polls lgaud #18 (device count, written to *a0) every 500 ms; other lgaud calls only run
  with a device present.
- 989snd's CD callback (0x79DC) uses iSignalSema: CD callbacks run in interrupt context.
- 989snd timer: AllocHardTimer(1, 32, 1), compare = USec2SysClock(1e6)/240 = 153600; handler
  0x790C returns the next compare (or 0 when shutting down). Missed ticks must be replayed:
  with a cap of 8 the tick ran at 145-200 Hz because long EE frames skip safe points.
- EE retries sceSifBindRpc while client->server == 0 (0x231AC0..0x231B2C).
- The intro movie's audio is voice-based: 989snd cmd 91 returns NAX - buffer start, and the EE
  movie player waits for it to move. A backend with frozen voice positions stalls the movie
  (first attempt with a plain logging stub hung in the intro).
- LOADFILE fn 6 args sit at offset 8 + 252 of the RPC data (lgAud gets "rpc=0").
