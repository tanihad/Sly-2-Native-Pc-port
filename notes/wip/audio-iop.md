# WIP: audio-iop (IOP side of native audio, approach C)

**Goal**: statically recompile 989snd_Library + spiop (IRX) to C, run them on native IOP kernel
libraries (fiber threads, sema, timers, sifcmd server, cdvdman...) so the EE's 989snd RPCs are
served by the real driver; libsd calls go to runtime/sd.h.

## Done
- recomp/irx.py: IRX parser + relocator (R_MIPS_32/26/HI16/LO16), import/export tables.

## In progress
- recomp/iop.py: IOP function discovery + MIPS I codegen.

## Next
1. runtime/iop.h (ctx, memory accessors, kernel API), iop_kernel.c (sysmem, threads on fibers,
   sema, evf, intrman, timrman, scheduler/iop_run/iop_service), iop_libs.c (sysclib, stdio,
   ioman, lgaud, loadcore, libsd glue), iop_sif.c (sifman/sifcmd server, cdvdman), iop_loader.c.
2. runtime/stub/sd_stub.c (logging libsd backend; only linked when runtime/sd.c is absent).
3. sif.c / iop_modules.c / cdvd.c / ps2_runtime.c integration; build_game.py runs `recomp iop`.
4. Unit tests: tests/unit/iop_recomp (hand-encoded MIPS I), tests/unit/iop_kernel.
5. Run the game; check banner, RPC servers, bank loads, 240 Hz tick, sd stub traffic.

## Findings / traps
- 989snd .text ends with data (0x1C934-0x1C9C4, a table after the import stubs): code ranges
  stop at the first import/export table; export table is in .text too (130 R_MIPS_32 relocs).
- No gp-relative code, no load-delay hazards found by a first scan; `break` used by GCC div
  checks; no `add/addi/sub` in code (only in the data above).
- Module start of 989snd does not block (creates 2 RPC threads); spiop start registers 'SUKR'
  through snd989 export 93 (guest-to-guest call).
- spiop polls lgaud #18 (device count, written to *a0) every 500 ms; other lgaud calls only run
  with a device present.
- 989snd's CD callback (0x79DC) uses iSignalSema: CD callbacks run in interrupt context.
- 989snd timer: AllocHardTimer(1, 32, 1), compare = USec2SysClock(1e6)/240 = 153600; handler
  0x790C returns the next compare (or 0 when shutting down).
- EE retries sceSifBindRpc while client->server == 0 (0x231AC0..0x231B2C).
