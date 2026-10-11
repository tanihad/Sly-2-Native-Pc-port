# Hardware layer: architecture and rules for implementers

The game's EE code is statically recompiled to C (`recomp/`). Everything else the PS2 provides is
emulated by the C runtime in `runtime/`. This document is the brief for anyone implementing a
hardware subsystem.

## Current state (boot sequence)

The game boots through kernel setup, IOP module loading, CD/DVD init, threads and VSync, then
reaches its intro movie, where it drives the IPU and DMAC. Nothing is drawn yet: DMAC, GIF, VIF,
VU1, GS rendering, IPU, timers, pad and memory card are stubs. See `notes/progress.md`.

## Interfaces

- `runtime/hw.h`: **frozen** interface between all subsystems (read it first; it states the
  conventions: synchronous execution, interrupts via `intc_raise`/`dmac_raise`, access handlers).
- `runtime/gs_internal.h`: **frozen** interface between the two GS halves.
- `runtime/hw.c` (dispatch, INTC, D_STAT, debug UART), `runtime/ps2_runtime.c` (interrupt
  delivery, VBlank, syscalls), `runtime/threads.c`, `runtime/sif.c`, `recomp/`: owned by the
  integrator. Do not edit them; if you need a change there, describe it in your report.

## File ownership

| Subsystem | Files you own (create/replace)                      | Notes                                   |
|-----------|-----------------------------------------------------|-----------------------------------------|
| gs-mem    | `runtime/gs_mem.c` (+ `runtime/gs_mem_*.c/h`)        | VRAM swizzling, transfers, CLUT, texels |
| gs-draw   | `runtime/gs.c`, `runtime/gs_draw.c` (+ `gs_draw_*`)  | registers, rasterizer, display, CSR     |
| gif       | `runtime/gif.c`                                      | GIFtags, PATH1/2/3                      |
| dma       | `runtime/dma.c`                                      | DMAC channels, chains, MFIFO, stall     |
| vif       | `runtime/vif.c`                                      | VIF0/1 codes, UNPACK, MPG, MSCAL        |
| vu        | `runtime/vu.c` (+ `runtime/vu_*.c/h`)                | VU0/VU1 micro mode interpreter          |
| ipu       | `runtime/ipu.c` (+ `runtime/ipu_*.c/h`)              | MPEG-2 decode commands, CSC             |
| timer     | `runtime/timer.c`                                    | T0-T3, compare/overflow interrupts      |
| window    | `runtime/window.c`                                   | Win32 window, present, keyboard+XInput  |
| pad       | `runtime/pad.c`                                      | padman RPC + per-VBlank pad data        |
| mc        | `runtime/mc.c`                                       | mcserv RPC backed by host files         |

Unit tests go in `tests/unit/<subsystem>/` with a `run.ps1` that builds and runs them (output in
`build/unit/<subsystem>/`). Tests must link only your files plus small fakes for neighbours.

## Rules

1. Implement from hardware documentation (ps2tek: https://psi-rockin.github.io/ps2tek/, Sony's
   EE/GS/VU user manuals, MPEG-2 spec for the IPU). Do not copy code from GPL emulators
   (PCSX2, GSdx, Play!). Writing your own implementation of documented behaviour is the goal.
2. Correct first, then fast: this runs every frame. No per-pixel/per-vertex heap allocation,
   no logging in hot paths (log first occurrence only, via `hw_log_unhandled` or a static flag).
3. Everything is synchronous (see hw.h). Return partial counts only for real hardware stalls.
4. Never call game code or deliver interrupts directly; use `intc_raise` / `dmac_raise`.
5. Don't modify files you don't own. Don't push. Commit to your own branch.

## Threads (performance)

Rule 3 describes the interface. Behind it, the GS runs on its own thread (`runtime/gs_thread.c`:
the EE queues register writes and image data in order and waits only where it observes the GS)
with pixel workers drawing deferred batches (`runtime/gs_draw_pixel.c`); VBlanks come from a host
timer thread. Measuring and tuning: `notes/performance.md`.

## Building and running from a worktree

tools/, orig/ and .venv/ only exist in the main checkout
(`C:\Users\tanih\Projects\sly2-decomp`); the scripts find them automatically.

- Python: `C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe`
- clang: `C:\Users\tanih\Projects\sly2-decomp\tools\llvm-mingw\bin\clang.exe`
- git: `C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe`
- Recompiler tests: `<python> tests\run_tests.py`
- Full game build: `<python> scripts\build_game.py` (1.5-4 min from scratch, the recompiled VU
  microcode files are the largest; output in `build\sly2\` of your tree)
- Run: `powershell -ExecutionPolicy Bypass -File scripts\run_game.ps1 -Seconds 8 [-Dump <dir>]`
  (headless; `-Dump` writes displayed frames as PNG every 60 VBlanks; `PS2_LOG_SIF=1` logs IOP
  traffic; the watchdog prints the hottest recompiled functions when it stops the game)
- Sound: headless runs render audio in real time into a null sink; `-Wav <file>` records it
  (`[dump] frame_N.png at audio T s` lines map dumped frames to WAV time), `-Pad "..."` sets
  PS2_INPUT. `PS2_INPUT="t95:start"` presses at a wall-clock time (scenes synchronised to sound
  last a fixed real time). Analyse with `<python> scripts\wav_analyze.py <wav> [--fft]`. Details:
  `notes/audio-mixer.md` ("Integration in the game").
- Disassemble game code: `<python> -m recomp disasm orig\SCUS_973.16 <hexaddr> -n 40`
  (run from your tree; pass the main checkout's `orig\SCUS_973.16` path)
- GS primitive trace: `PS2_GS_TRACE_RECT=x0,y0,x1,y1` (drawing-buffer pixels after XYOFFSET; Sly 2
  draws 512x224 field buffers, so halve the y of a dumped 448-line frame) logs every primitive
  touching the rectangle: vertices, PRIM attributes, FRAME/ZBUF/TEST/ALPHA/TEX0/TEX1/CLAMP/TEXA and
  the texels it covers, plus TRXDIR transfers. `PS2_GS_TRACE_OUT=<file>` (buffered; stderr is slow),
  `PS2_GS_TRACE_FROM=<vsync>`, `PS2_GS_TRACE_MAX=<n>` (default 64), `PS2_GS_TRACE_BRIEF=1` (one line
  per primitive), `PS2_GS_TRACE_MINSIZE=<px>` (skip small ones), `PS2_GS_TRACE_TEXELS=0`.
  `PS2_PEEK=12000000:2,12000070:4` reads the GS privileged registers (PMODE, DISPFB1, ...).
  `PS2_POKE=<hexaddr>=<hexvalue>[,...]` writes 32-bit words every VBlank (debug aid, off by default),
  e.g. `PS2_POKE=3D4AC8=28` holds Bentley's health (game state `bentley.hpCur`, scripts/warp_schema.txt)
  to reach the later stages of a fight that scripted input cannot win.
  `PS2_POKE_FILE=<path>`: the same `<hexaddr>=<hexvalue>[,...]` lines appended to the file while the
  game runs are written once (checked every 15 VBlanks, logged as `[poke]`), for live sessions.
- Live input (`PS2_INPUT_FILE`, `scripts\live_game.ps1`): the token `pause` stops the game at the next
  dumped VBlank (latest.png shows it) until the next line arrives, so real-time sections (turrets,
  boss fights) can be played step by step: end each line with e.g. `+30:pause`. Tokens are not
  case-sensitive (`lxFF` = `lxff`; before, an upper-case token was dropped as an unknown button).
- Scripted input (`PS2_INPUT`, runtime/window.c header): absolute `<padframe>:<button>[:<hold>]`,
  relative `+n:...`, anchored to game code `@<func>[/<ra>][+n]:...` (after a call of a recompiled
  function, optionally from the call site returning to ra) and `@!<func>[+n]:...` (after it went
  quiet for n frames), `none`, partial sticks `lxHH/lyHH/rxHH/ryHH`, `ram` (EE RAM dump). Sly 2:
  `@187D80/1DD96C+30:start` presses START once the title screen accepts it; `@!227820+90:none` waits
  for the end of a Binocucom conversation. Anchors need the debug build.
- Live play: `scripts\live_game.ps1` starts the game in the background with `PS2_INPUT_FILE`
  (lines appended to build\live\input.txt are read while it runs); `scripts\live_look.ps1 -Send
  "+0:lup:60" -Wait 3` sends input and tiles the newest frames into build\live\look.png;
  `scripts\live_goto.py x,y ...` steers Sly to world positions, `scripts\live_hop.py` jumps towards
  one (both need `PS2_PEEK="2DE1C0:4,2DDFD0:4"`: player focus and camera position).
- Start at any episode or job (memory-card saves, `notes/warp.md`): `scripts\warp_run.ps1 -Episode N
  [-Job <job>] [-Direct] -Play`; `scripts\warp.py make|list|dump`; `run_game.ps1 -Card <dir>`
  (PS2_MEMCARD_DIR); `PS2_WARP_TASK=<task oid>[:<world>]` (any build: an entry hook,
  recomp/hooks.py) starts a job on "Resume"; `PS2_TASK_SET=<task oid>:<state>[@<vblank>],...` /
  `PS2_TASK_FILE=<path>` (lines appended while it runs; `<oid>:?` prints the state) change a task's
  state through the game's own 0x164DF0 at the top of the main loop (entry hook on 0x20E890).
- VU microcode is recompiled (`recomp/vu_recomp.py`, `notes/vu-microcode.md`): `PS2_VU_INTERP=1`
  runs the interpreter instead; `PS2_VU_VERIFY=1` runs every recompiled program and the interpreter
  on a copy and compares registers, flags, data memory, XGKICKs (with the data memory at each kick)
  and timing (`PS2_VU_VERIFY_DUMP=<file>`: mismatches as a recording); `PS2_VU_RECORD` +
  `scripts\vu_replay.ps1 -Rec <file> [-Compiled <out>\vu]` replays recorded programs;
  `scripts\vu_rec_stats.py` lists the images/entries a recording uses; `tests\unit\vu_recomp`.
- Frame dumps: `PS2_DUMP_FROM=<vblank>`, `PS2_DUMP_RING=<n>` (keep the newest n), `latest.png`;
  `PS2_DUMP_RAM=<vblank>,...`; `PS2_CALLS=<from>:<n>,...` (up to 8 call-profile windows);
  `scripts\contact_sheet.py out.png frames... [--cols --scale --crop x0,y0,x1,y1 --zoom n]`.
