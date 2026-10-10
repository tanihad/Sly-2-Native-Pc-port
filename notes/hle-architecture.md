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

## Building and running from a worktree

tools/, orig/ and .venv/ only exist in the main checkout
(`C:\Users\tanih\Projects\sly2-decomp`); the scripts find them automatically.

- Python: `C:\Users\tanih\Projects\sly2-decomp\.venv\Scripts\python.exe`
- clang: `C:\Users\tanih\Projects\sly2-decomp\tools\llvm-mingw\bin\clang.exe`
- git: `C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe`
- Recompiler tests: `<python> tests\run_tests.py`
- Full game build: `<python> scripts\build_game.py` (40 s, output in `build\sly2\` of your tree)
- Run: `powershell -ExecutionPolicy Bypass -File scripts\run_game.ps1 -Seconds 8 [-Dump <dir>]`
  (headless; `-Dump` writes displayed frames as PNG every 60 VBlanks; `PS2_LOG_SIF=1` logs IOP
  traffic; the watchdog prints the hottest recompiled functions when it stops the game)
- Disassemble game code: `<python> -m recomp disasm orig\SCUS_973.16 <hexaddr> -n 40`
  (run from your tree; pass the main checkout's `orig\SCUS_973.16` path)
- GS primitive trace: `PS2_GS_TRACE_RECT=x0,y0,x1,y1` (drawing-buffer pixels after XYOFFSET; Sly 2
  draws 512x224 field buffers, so halve the y of a dumped 448-line frame) logs every primitive
  touching the rectangle: vertices, PRIM attributes, FRAME/ZBUF/TEST/ALPHA/TEX0/TEX1/CLAMP/TEXA and
  the texels it covers, plus TRXDIR transfers. `PS2_GS_TRACE_OUT=<file>` (buffered; stderr is slow),
  `PS2_GS_TRACE_FROM=<vsync>`, `PS2_GS_TRACE_MAX=<n>` (default 64), `PS2_GS_TRACE_BRIEF=1` (one line
  per primitive), `PS2_GS_TRACE_MINSIZE=<px>` (skip small ones), `PS2_GS_TRACE_TEXELS=0`.
  `PS2_PEEK=12000000:2,12000070:4` reads the GS privileged registers (PMODE, DISPFB1, ...).
