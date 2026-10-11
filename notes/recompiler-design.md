# PS2 → x86-64 static recompiler: design

Goal: run Sly 2 natively on Windows x86-64 by translating the game's Emotion Engine (MIPS R5900)
machine code into C ahead of time, compiling that C with a normal compiler, and linking it against
a runtime that stands in for the PS2's hardware and BIOS.

The original game executable is **not** embedded in the output. The runtime loads the user's own
dumped ELF at startup for its data sections.

## Pipeline

```
orig/SCxS_xxx.xx (PS2 ELF)
   │  recomp/elf.py        load PT_LOAD segments, entry point, symbols (if any)
   ▼
   │  recomp/decode.py     decode every word in executable segments with rabbitizer (R5900 mode)
   ▼
   │  recomp/functions.py  find function starts: entry, ELF symbols, jal/bal targets
   ▼
   │  recomp/codegen.py    one C function per MIPS function: f_<addr>(ps2_ctx*)
   ▼
build/recomp/*.c  +  runtime/*.c  ──clang──►  sly2.exe
```

## Translation model

- **CPU state** lives in `ps2_ctx` (runtime/ps2_runtime.h): 32 × 128-bit GPRs, HI/LO, HI1/LO1
  (the R5900's second multiply pipeline), SA, 32 FPRs, FPU accumulator, FCR31.
- **Memory**: one 32 MB buffer for main RAM. The uncached mirrors (0x2000_0000, 0x3000_0000) map
  onto it; the 16 KB scratchpad (0x7000_0000) is separate. Everything else (hardware registers,
  GS, VU memory) goes through slow-path handler functions.
- **Branches inside a function** become `goto` labels. **Delay slots**: the branch condition is
  evaluated first, then the delay-slot instruction, then the jump, which matches MIPS semantics.
  Branch-likely instructions run the delay slot only when taken.
- **Calls** (`jal`) become direct C calls. Execution resumes after the call at A+8.
- **Indirect jumps** (`jr` on a register other than `$ra`, usually switch jump tables) become a C
  `switch` over every instruction address in the function; anything else goes through the
  runtime's address→function lookup.
- **Indirect calls** (`jalr`) go through the same lookup (`ps2_call`).
- **Unknown/unhandled instructions** become `PS2_UNIMPL(...)`, which logs once at runtime. The
  `analyze` command counts them, so we implement what the game actually uses first.

## Native overrides (recomp/overrides.py)

The recompiler owns every function, so a function whose work is meaningless on the host can be
replaced wholesale by native C (native-port-principles.md, rule 4). `OVERRIDES` maps a function
start address to an `Override` (name, SHA-1 of the function's original bytes, C body, reason);
`translate_all` uses it instead of translating the function, but only when the bytes match, so a
different executable (another region, the synthetic test ELF) is translated normally
(`tests/run_tests.py` checks this). The generated function keeps its name `f_<addr>` and its
`PS2_TRACE`, and its comment carries the reason.

Current table - cache maintenance. The EE caches are not modelled (guest memory is always coherent
with DMA/VIF/GIF/IOP), so Sony's cache routines are no-ops by definition, but translated they still
loop over every line (or the whole 64 x 2 tag RAM with an `mfc0 TagLo` call per line):

| Address    | Function (libkernel)        | What the original loops do                          |
|------------|-----------------------------|-----------------------------------------------------|
| 0x00234EA0 | SyncDCache inner loop       | tag walk; write back lines in [a0, a1) (cache 0x14)  |
| 0x00234FD0 | InvalidDCache inner loop    | tag walk; invalidate lines in [a0, a1) (cache 0x16)  |
| 0x0023B170 | sceSifWriteBackDCache       | line by line over [a0, a0+a1) (cache 0x18), 22 callers |

The replacements return at once and leave the loops' scratch registers alone (callers are
compiled C and the functions return nothing). The wrappers that disable/enable interrupts around
the first two (0x234F48, 0x235078) are still translated as they are. To add an override: find the
function's bounds with `recomp/functions.py`, hash its bytes with `overrides.function_sha1`, and add
an entry with the reason.

## Entry hooks (recomp/hooks.py)

Debug builds trace every function entry (`PS2_TRACE`, which calls `ps2_calls_record` while a tool
such as PS2_CALLS or an `@` input anchor needs it); release builds compile tracing out. A debug
tool that must act on a particular call in every build lists the function in `HOOKS` (name, SHA-1
of its bytes, reason, applied only when the bytes match, like overrides). The generated function
then starts with `PS2_HOOK(<addr>)` after its `PS2_TRACE`: one test of `ps2_hooks_on` (0 unless a
tool switched it on) and a call of `ps2_hook(ctx, addr)` in runtime/ps2_runtime.c, which hands it to
the tool. The game itself is not changed.

| Address    | Function                         | Tool                                                |
|------------|----------------------------------|-----------------------------------------------------|
| 0x0022B490 | queue a level load (transition)  | PS2_WARP_TASK, runtime/warp.c (notes/warp.md)        |

## Known hard parts (in rough order of how much work they are)

1. **Graphics (GS + GIF + VIF + VU1)**: the game draws by sending packets through DMA to the
   vector unit VU1, whose microcode transforms vertices and feeds the Graphics Synthesizer. We
   need to run VU1 microcode (interpret or recompile it too) and render GS primitives with a
   modern API (Direct3D/Vulkan/OpenGL).
2. **IOP** (the second CPU: audio, CD/DVD, controllers, memory card): high-level emulate the
   IOP modules the game talks to over SIF/RPC instead of running IOP code.
3. **BIOS syscalls**: threads, semaphores, interrupts, DMA setup, implemented in the runtime.
4. **VU0** macro-mode instructions inside EE code, and VU0 microprograms.
5. **FPU accuracy**: the PS2 FPU is not IEEE 754 (no infinities/NaNs, clamps to the largest float,
   different rounding). Most code doesn't care; some physics/camera code will.
6. **Timing**: games assume a fixed CPU speed and vsync rate.

## Milestones

1. Analyze the ELF: segments, function count, instruction histogram, unimplemented instructions.
2. Generate C for every function that compiles cleanly (no hardware yet).
3. Runtime boots: BIOS HLE enough for crt0 → `main` to run until the first hardware wait.
4. DMA/GIF/GS: first image on screen.
5. Controllers, IOP HLE, audio, then accuracy and performance.
