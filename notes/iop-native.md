# The IOP side of native audio (approach C): recompiled 989snd + spiop on a native IOP kernel

Status (2026-10-10, branch audio/audio-iop): implemented, integrated with the native libsd
backend (`runtime/sd.c` + WASAPI, `notes/audio-mixer.md`) and running in the game: the game's
sound plays through it from the boot movie to gameplay. Plan and decision:
`notes/audio-native-plan.md`; research on the modules and the protocol: `notes/audio-plan.md`.
(`runtime/stub/sd_stub.c` is only built when `runtime/sd.c` is absent.)

## What runs

- **989snd_Library 3.1** and **spiop 1.0**, taken from the executable's `.irx` section, are
  statically recompiled to C at build time (`python -m recomp iop <elf> -o build/sly2/iop`, run by
  `scripts/build_game.py`): 634 functions + 113 import stubs, no untranslated instruction.
- At run time the EE loads its nine IOP modules through LOADFILE as before. For these two, the
  runtime places the image the EE sent at the address the code was recompiled for, relocates it,
  checks it against CRCs recorded by the recompiler, and runs the module's start function natively
  (`iop_loader.c`). The other seven modules are still only acknowledged (their services are the C
  stand-ins: pad.c, mc.c, cdvd.c); libsd is replaced by the native backend, lgAud by a stub.
- 989snd registers its RPC servers 0x123456/0x123457 itself (sceSifRegisterRpc); sif.c binds the
  EE to them, queues every call for the IOP thread serving it and sends the reply when that thread
  finishes, possibly much later (bank loads read the disc first).
- `runtime/snd.c` is the fallback: `PS2_AUDIO=0` (or a module whose CRC does not match) keeps the
  old acknowledging behaviour, including its silent model of spiop's three stream channels (so
  dialogue lines still advance without sound). It reads stream headers with the same ISO reader
  as the IOP's cdvdman (`cdvd_read_host` in cdvd.c).

Verified in the game with sound (2026-10-10, see `notes/audio-mixer.md`, "Integration in the
game"): spiop opens the two looping music streams (32 kHz and 44.1 kHz " KPV", channels 0/1) at
the save prompt, the game pauses them during the prompt (proc 3) and resumes them (proc 4); each
Binocucom dialogue line is a VAGp stream on channel 2 that the real spiop reports finished (proc 8
= 0) exactly one header length after it was resumed, after which the game closes it (proc 2) and
opens the next line. `scripts/audio_streams.py` checks this from a `PS2_LOG_SND=1` run.

Observed in the game (headless run, `scripts/run_game.ps1`): the 989snd banner, both servers
registered by the recompiled code, the boot bank load returning handle 0x001D1F00, the sound tick
at 240.0 Hz, sample uploads (~1.4 MB) and key-ons reaching the libsd backend, spiop streaming the
cutscene dialogue from disc; title screen and the episode intro behave as before. The IOP costs
about 0.2-0.4% of the emulation thread (more with `PS2_LOG_SND`/`PS2_LOG_SD`, which write a line
per command or voice event).

## IOP memory map (2 MB, `iop_ram[]` in iop_kernel.c; also in `runtime/iop.h`)

| Range | Use |
|-------|-----|
| 0x000000-0x01FFFF | "kernel" area, never allocated: pad.c's pad blocks at 0x1E000, C RPC server handles at 0x1D000 (opaque numbers), the SIF command buffer at 0x1F000 |
| 0x020000-0x04CA88 | 989snd_Library (fixed link address, reserved at startup) |
| 0x050000-0x055700 | spiop (fixed link address, reserved at startup) |
| rest up to 0x1FFFFF | sysmem: one first-fit allocator (256-byte granules) for AllocSysMemory, thread stacks (ALLOC_LAST), the interrupt stack, the EE's IOPHEAP requests and the C RPC servers' receive buffers |

The old fixed areas (RPC buffers at 0x100000 + 16 KB per server, a bump heap at 0x140000) are gone,
and with them their overlap. QueryMaxFreeMemSize reports what is really free (about 0xC0000 bytes
while the game runs), so 989snd sees a 2 MB machine.

## The recompiler's IOP mode (`recomp/irx.py`, `recomp/iop.py`)

- `irx.py` parses IRX files (ELF type 0xFF80: `.iopmod`, one load segment, SHT_REL sections,
  import tables 0x41E00000 and export tables 0x41C00000 inside `.text`) and relocates them
  (R_MIPS_32/26/HI16/LO16). Link addresses are 64 KB aligned (`MODULES` in iop.py).
- Code ranges: `.text` up to the first import/export table; nothing after the last table (989snd
  keeps a data table there). Function starts: entry, exports, jal/bal targets, `lui`+`addiu`
  constants whose `lui` carries a HI16 relocation, R_MIPS_32 words outside `.text` (except switch
  tables), weak starts after `jr $ra` and after unconditional jumps; a merge pass removes weak
  boundaries that a branch (or a `j` to a non-function) crosses. Switch tables are recognised from
  `sltiu` + table base + `jr` and become C `switch` statements.
- Code generation: 32-bit registers (`iop_ctx`), IOP memory accessors (`iop_r32` etc.: RAM and its
  mirrors, 1 KB scratchpad, I/O logged), MIPS I only (anything else is an error, MIPS II+ and COP
  instructions do not occur), R3000A divide results, `break` logged (GCC's divide-by-zero check),
  `IOP_POLL` at loop heads.
- **Load delay slots**: every load is checked; if the next instruction reads the loaded register
  it gets the old value (emitted explicitly). The game's modules have none; the unit test has one.
- **Imports**: a `jal` to a stub becomes a direct C call: into the recompiled exporter for guest
  libraries (spiop -> snd989 exports), otherwise `iop_imp_<lib>_<function>()` (native). Stubs are
  also emitted as functions so that function pointers to imports work.

## The native IOP kernel and libraries

| File | Contents |
|------|----------|
| `runtime/iop.h` | context, accessors, memory map, API for sif.c/iop_modules.c |
| `runtime/iop_internal.h` | kernel-internal API (blocking, waking, interrupt calls), KE_* codes |
| `runtime/iop_kernel.c` | sysmem, loadcore, threads + scheduler, thsemap, thevent, intrman, timrman |
| `runtime/iop_libs.c` | sysclib, stdio printf (`[iop]` prefix), ioman (no files), lgaud stub, libsd -> sd.h, transfer interrupt |
| `runtime/iop_sif.c` | sifcmd server side (RPC queues and loop), sifman (IOP->EE DMA), cdvdman (disc reads, ISO 9660 SearchFile) |
| `runtime/iop_loader.c` | LOADFILE of recompiled modules: CRC, relocation, start in a kernel thread |
| `runtime/stub/sd_stub.c` | logging libsd backend used until runtime/sd.c exists |

Execution model:
- **Threads** are Windows fibers with an `iop_ctx` and a stack in IOP RAM. The scheduler follows
  the IOP kernel: the ready thread with the lowest priority number runs until it blocks or a
  higher-priority thread becomes ready (checked after every waking call: StartThread,
  SignalSema, WakeupThread, SetEventFlag, ChangeThreadPriority).
- **When the IOP runs** (2026-10-11, next/ep1): like the PS2's IOP CPU, on a host thread of its own
  ("the IOP thread", `iop_thread_start`, started by `main` when the recompiled modules are present).
  It runs a pass of `iop_run()` (threads until all are blocked, or 2 ms) every millisecond, at
  once when kicked (`iop_kick`: sif.c after queuing an RPC request), and again right away after a
  pass that ended at its time slice with threads still ready. A thread that runs for more than 2 ms
  without blocking (a busy-wait) is preempted at its next loop head and continued in the next pass.
  The IOP thread is an MMCSS "Pro Audio" thread like the sound output thread (fallback:
  THREAD_PRIORITY_HIGHEST, whose 1 ms sleeps took 40-120 ms on a machine running ~10 games).
  All IOP kernel state is guarded by the IOP lock (recursive; `iop_lock`/`iop_unlock`, iop.h): the
  IOP thread holds it for each pass; the EE side takes it for its few direct calls (RPC queueing
  and server lookup, IOPHEAP alloc/free, module loading and binding, which still run passes on the
  EE thread: boot only). The IOP->EE packet queue (sif.c) and the ISO reader (cdvd.c) have locks of
  their own; libsd calls take sd.c's lock as before. `PS2_IOP_THREAD=0` restores the old model.
- **Why its own thread**: the IOP used to run only at the EE's safe points (`ps2_service_irq` ~1 kHz,
  between DMA tags via `ps2_poll_iop`, after RPC requests). Whenever the EE thread stopped (a long
  frame, waiting for the GS thread, descheduled on a busy machine) the IOP stopped too, and 989snd's
  stream chain (half change seen by the 240 Hz tick -> refill VoiceTrans by the stream thread ->
  LSAX moved to the other half by a later tick) did not finish within one half buffer (0x1000 bytes,
  163 ms at 44.1 kHz): the voice looped inside the half it had just played, 989snd saw NAX run
  backwards (error 0x37 at 0x33058) and stopped the stream for good: the Paris hub's and safehouse's
  ambience/music went silent, as did ep2's episode menu music. Running the IOP from the EE side
  also cost the EE time (ps2_poll_iop between DMA tags). Measured on a loaded machine: before, 20-80
  ticks per 10 s more than 8 ms late (up to 360 ms); with the IOP thread the longest gap between
  passes is ~5-15 ms (rarely ~30 ms), and the safehouse's 9 streams ran 16 minutes without a stop.
- **Interrupts**: hard timers, CD completion, SIF DMA completion callbacks and libsd transfer
  completions are delivered between thread slices and at loop heads of code with interrupts
  enabled (CpuSuspendIntr state is per thread). Handlers run on the interrupt stack with
  QueryIntrContext() = 1; `i*` calls only make threads ready.
- **timrman** follows real time: 989snd's timer (`AllocHardTimer(SYSCLOCK, 32, 1)`, compare =
  1 s / 240) calls its handler at 240 Hz of host time; ticks missed while the EE was busy are
  delivered back to back (a stall over 1 s is dropped).
- **DelayThread** wakes on host time, at the earliest at the next scheduler pass.
- **cdvdman**: `sceCdRead` copies from the ISO (cdvd.c's `cdvd_read_host`) at once; the drive stays
  busy until the next interrupt point, where `sceCdSync(0)` waiters wake and the sceCdCallback
  handler runs in interrupt context (989snd's handler uses iSignalSema). Status: 0x0A (paused) or
  0x06 (reading).
- **sifman**: `sceSifSetDma(Intr)` copies IOP -> EE immediately; the callback (`func(data)`) runs at
  the next interrupt point; `sceSifDmaStat` reports done (-1). `sceSifGetOtherData` copies EE -> IOP.
- **libsd**: each import is a direct call to the sd.h function. `iop_sd_transfer_done(channel)`
  (thread-safe) queues the handler registered with `sceSdSetTransIntrHandler(channel, func, arg)`;
  it runs as `func(channel, arg)` in interrupt context at the next interrupt point, once per
  completion (completions are counted per channel, not merged).
- **RPC receive buffers**: the EE DMAs a call's arguments into the server's single buffer before
  the call packet, so a call that arrives while an earlier one for the same server is still queued
  has overwritten its arguments (as on the PS2, where the IOP runs independently too; the game's
  EE side waits for each reply). `iop_rpc_request` logs it if it ever happens ("arguments
  overwritten"); not seen in any run.
- **lgaud**: spiop polls #18 (device count, written through a0) every 500 ms; it reports 0, every
  other call fails with -1 (they are only reached with a headset present).

## What the libsd backend must provide (from 989snd's behaviour)

- Transfers (`sceSdVoiceTrans`, `sceSdBlockTrans`) report completion through
  `iop_sd_transfer_done()`; `sceSdVoiceTransStatus(ch, 1)` must not block forever.
- **Voice positions must advance with playback**: the EE's movie player polls 989snd command 91,
  which returns `sceSdGetAddr(SD_VA_NAX | voice) - buffer start` for the movie-sound voices and
  waits for it to move; streams poll NAX/ENDX the same way. With positions frozen the intro movie
  stops. The stub simulates this (ADPCM loop flags in a copy of sound memory, 48 kHz x pitch).
- ENVX (`sceSdGetParam(SD_VP_ENVX)`) is read to find finished voices.

## Debugging

- `PS2_LOG_IOP=1`: kernel calls (threads, memory, RPC replies with their first word, CD reads).
- A status line every 10 s: timer rate, how many timer interrupts were delivered more than 8 ms
  late and the worst lateness, IOP share of wall time, thread states, free IOP memory, RPC calls
  per server, CD reads; then the IOP thread's line: its CPU time, the longest gap between passes
  (and within it the longest wait for the IOP lock and the longest 1 ms sleep), the longest pass,
  and the longest hold of the IOP lock by another thread; then every IOP thread that used up 2 ms
  time slices without blocking since the last line (busy-waiting, or the IOP thread descheduled /
  blocked meanwhile: slices count wall time; debug builds add the function entered last). The
  IOP thread hands this text to a writer thread (`host_log_async`, audio_out.c): a write to stderr
  can block for 100 ms and more on a busy machine.
- **Logging and the IOP's timing**: a direct `fprintf` on the IOP thread can block 50-300 ms on a
  loaded machine (another thread holding the CRT stream lock is descheduled), which alone broke
  streams in early runs. The status lines, `PS2_LOG_SD*` (sd.c) and the output thread's lines now
  go through `host_log_async`; with `PS2_LOG_SD_VOICES` on, the safehouse's streams ran clean
  (989snd moved LSAX 4 ms after a half change at the median, 8 ms at p99). Still direct, so still
  perturbing: `PS2_LOG_SND`, `PS2_LOG_IOP`, `PS2_IOP_WATCH`, 989snd's own printf. Stream health
  without any log: the `[audio] status` line counts voice transfers per 10 s.
- `PS2_PROFILE=iop`: the sampling profiler (perf.c) samples the IOP thread, only while a pass of it
  has run longer than 4 ms: shows where long passes block or spin (report at exit).
- `PS2_IOP_WATCH=<hex>[,<hex>...]` (debug builds): logs every entry to those recompiled IOP
  functions with a0-a3, ra and thread, e.g. 22140 = 989snd's error report (a0 = code: 0x37 = a
  stream voice's NAX ran backwards inside a half, 0x34/0x35 = NAX outside the buffer, 0x48 ...).
- `PS2_LOG_SD_VOICES=<core 0 hex mask>[,<core 1 mask>]` (sd.c): loop-end jumps, loop-start blocks,
  pitch changes and SSA/LSAX writes of those voices (idle voices parked on a one-block loop are
  skipped). A stream failure shows as a jump back into the half just played with no LSAX write
  since the previous jump.
- `PS2_LOG_SND=1`: every 989snd command the EE sends, with its data words and result
  (`[snd] v<vblank> cmd <n> (<bytes>): ... -> <result>`; direct calls are marked "direct"), the
  same format as the fallback snd.c. spiop's procs are commands 76/104 with 'SUKR' (524B5553) and
  the proc number in the first two words. The arguments are copied before the call and each line
  is written at once (lines used to be split by other threads' output).
- `PS2_LOG_SD=1|2`: the backend's log (`notes/audio-mixer.md`); the stub logs every libsd call.
- `PS2_AUDIO=0`: old behaviour (modules acknowledged, snd.c answers).
- Calls to addresses that are not recompiled functions, `break`, unimplemented instructions and I/O
  accesses are logged once with the last IOP functions (`iop_trace`).

## Tests

- `tests/unit/iop_recomp/run.ps1`: two synthetic IRX modules assembled from hand-encoded MIPS I
  (`irx_asm.py`, every word checked with rabbitizer) are recompiled by `recomp/iop.py` and loaded by
  the runtime loader (relocation, CRCs, module start with argv), then ALU, shifts, mult/div edge
  cases, loads/stores incl. lwl/lwr/swl/swr at every alignment, branches and delay slots, link
  registers, a loop, a load-delay hazard, a relocated switch table, jalr through a relocated
  pointer, a `j` tail call, $zero, and imports (native, guest-to-guest, a 5th argument on the
  stack) are checked against values computed independently in Python.
- `tests/unit/iop_kernel/run.ps1`: sysmem, priority preemption, semaphore wake order (priority and
  FIFO), sleep/wakeup counting, DelayThread timing, a 1 kHz hard timer (interrupt context,
  iSignalSema, stopping), event flags, the RPC loop (deferred replies, queued requests), CD
  read/sync/callback, ISO 9660 SearchFile, SIF DMA and libsd completion callbacks, the time slice
  of a busy-waiting thread, then the IOP thread (a 1 kHz timer, DelayThread and an RPC reply in
  real time with nobody calling `iop_run` on the test thread; nothing runs while the test holds the
  IOP lock and the missed ticks follow; `iop_service` no longer runs the IOP), sysclib, printf with
  stack arguments.

## Open points

- Only one bank load (the boot bank) happens before the museum prologue; level banks load later
  in the game and take the same path.
- Reverb (989snd command 14 -> sceSdSetEffectAttr) is not requested up to the first gameplay.
- The IOP has its own OS thread now (see "When the IOP runs"). Stream data still comes from
  `sceCdRead`, which reads the ISO synchronously on the IOP thread (fast from the file cache; a
  cold, busy disk would stall the IOP for the read).
- ADMA streaming (`sceSdBlockTrans` loop mode) is not used by what has run so far; the backend
  already announces each consumed half buffer (`iop_sd_transfer_done()`, unit-tested in
  tests/unit/sd), but no game path has exercised it.
