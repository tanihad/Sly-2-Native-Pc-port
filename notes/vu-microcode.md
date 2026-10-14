# VU micro mode: implementation notes and Sly 2's microcode

The game's microprograms are **statically recompiled** to C at build time (`recomp/vu_recomp.py`,
section "Recompiler" below), like the EE code; the interpreter is the reference and the fallback.

Files: `runtime/vu.c` (EE-facing API, memory, VU0 control registers, dispatch to recompiled code,
recording, verification), `runtime/vu_exec.c` (decoder + interpreter), `runtime/vu_ops.h` (the
semantics of every operation, shared by interpreter and recompiled code), `runtime/vu_compiled.h`
(interface of the generated code), `runtime/vu_internal.h` (state shared with the unit tests).
Recompiler: `recomp/vu_decode.py` (decoder, a mirror of vu_exec.c's), `recomp/vu_recomp.py`.
Tests and tools: `tests/unit/vu/` (`run.ps1`, `test_vu.c`, `vuasm.h` mini assembler,
`vudis.py` disassembler), `tests/unit/vu_recomp/` (random programs: recompiled vs interpreter),
`scripts/vu_replay.ps1` (recordings), `scripts/vu_rec_stats.py`.

Sources: Sony's VU User's Manual (instruction set, encodings, flags), ps2tek
(https://psi-rockin.github.io/ps2tek/: pipelines, latencies, flag layouts), the PCSX2 team's
public VU documentation blog post (behaviour notes only, no code), and the game's own microcode,
which turned out to confirm several choices below.

## Execution model

- One 64-bit pair per cycle: lower word at +0, upper at +4; upper bits 31-27 = I E M D T.
- Micro memory is pre-decoded into a cache of `vu_op` entries with a specialised handler per
  upper/lower op. Each entry keeps the raw pair it was decoded from and is re-decoded when memory
  differs, so VIF MPG writes through `vu_micro_mem()` and EE stores need no invalidation hook.
- Everything is synchronous: `vu1_start()` / `vu0_callms()` run until the pair after the E bit.
  VPU-STAT always reads 0, `vu1_busy()` is 0.
- A cycle counter advances by one per pair plus stall cycles. Stalls matter even though values
  are written at issue: Q, P and the flags are *not* hazard-checked and arrive a fixed number of
  cycles after issue, so the number of cycles between a DIV and a Q read depends on stalls.

### Within a pair

The upper op reads its operands, then the lower op executes (seeing the registers as they were
before the pair), then the upper result is committed, then the I bit loads I. Consequences:
- a lower SQ/MOVE of a register the upper writes uses the old value; an upper op reading a register
  the lower loads (LQ) uses the old value;
- if both write the same VF field the upper result wins (Sony manual);
- an upper `...i` op in a pair with the I bit uses the *previous* I. The game's compiler output
  depends on this: e.g. `[I] addi.x vf05, vf00, I / loi 0.6` at 0x00B8 of program 188054331 uses
  the 160.0 loaded by the previous pair, and the 0.6 is used by the next pair.

### Pipelines and latencies

| Unit | Model |
|------|-------|
| FMAC (upper ops; LQ/LQI/LQD/MOVE/MR32/MFIR/MFP/RGET/RNEXT VF writes) | result ready 4 cycles after issue; readers stall per field (up to 3 cycles). vf0, ACC, I, Q, P, R are not checked (ACC chains MULA/MADDA... run back to back). |
| MAC/status flags, clip flags | queued with ready = issue + 4; FM*/FS*/FC* read the visible copy. Sticky status bits OR in when an op retires. FSSET writes the sticky bits; FCSET overwrites clip and drops in-flight CLIP results. |
| FDIV | DIV/SQRT: Q ready at issue + 7, RSQRT at + 13. A read before that sees the old Q. A second FDIV op (and WAITQ) stalls until the first is done; WAITQ's pair (including its upper op) then sees the new Q. Status I/D flags arrive with Q. |
| EFU (P) | ESADD 11, ERSADD 18, ELENG 18, ERLENG 24, ERSQRT 18, ESQRT 12, ERCPR 12, ESUM 12, ESIN 29, EEXP 44, EATAN/EATANxy/EATANxz 54 (ps2tek). P visible at issue + latency. EFU stalls are the FDIV ones minus a cycle (no writeback hazard check: "a stall lasts for one cycle less than usual"): WAITP and a new EFU op on a busy EFU both wait until latency - 1, so two P results can be in flight. Sly 2 issues EFU ops back to back: `eleng P, vf03 / eleng P, vf04 / mfp.x ... / eleng P, vf05 / mfp.y ...` (376110651 at 0x00D0, 188054619 at 0x00A8) reads each length while the next one runs. |
| Integer | IALU results visible to the next pair (bypass). ILW/ILWR results take 4 cycles; readers stall. |
| Branches | one delay slot, modelled with MIPS-style pc/npc: a branch in a delay slot runs one instruction at the first target, then goes to the second target (unverified on hardware; Sly 2 never does it). BAL/JALR link = address after the delay slot / 8. |
| E bit | the pair after the E bit (its delay slot) still runs; TPC = the address after it (used by MSCNT). The pipeline drains at the end: pending Q/P/flags land. |

### VI register hazard for conditional branches

A conditional branch reads a VI register as it was *before* a chain of up to four immediately
preceding pairs that write that register (each one directly before the next). The chain starts
at a write and continues only while the next pair reads AND writes the register (`iaddi vi5,
vi5, 1`, `sqi ..., (vi5++)`); a write that does not read it (`iaddiu vi5, vi0, 7`) starts a new
chain, so the branch sees the value from just before that write. A stall inside the
chain lets the writes land: the chain then starts at the stalled pair; a stall on the branch
itself cancels the effect. FS/FM/FC flag ops write their VI result without this delay. JR/JALR
are not affected. ILW/ILWR results make the branch stall, so it reads the loaded value. (Source:
PCSX2 VU documentation, "VI register delays on conditional branches"; the author notes the stall
rule is believed, not proven.) Sly 2's microcode never branches on a just-written VI register.

### Numbers (no Inf/NaN on the VU)

The interpreter runs with MXCSR = round toward zero + flush-to-zero + denormals-are-zero, set in
`vu.c` around each program (and restored around the GIF call for XGKICK):
- truncating results like the VU FMAC; overflow saturates to +-FLT_MAX (O flag), underflow and
  denormal inputs become zero (U flag when the exact result was not zero);
- exponent-255 bit patterns are ordinary huge numbers on the VU; arithmetic inputs are clamped to
  +-FLT_MAX first (`vclean`) so the host never sees Inf/NaN;
- MAX/MINI compare raw bits as sign-magnitude integers and pass the winner through unchanged
  (denormals and exponent-255 values included). The game relies on this: it builds GIF-tag
  fields with `maxi.x vf02, vf00, I` where I = 0x00008001, 0x10004000, 0x0000000E, ...;
- ABS, MOVE, MR32, loads/stores are raw bit operations;
- FTOIn truncates and saturates (0x7FFFFFFF / 0x80000000); ITOFn converts (toward zero) then
  scales;
- DIV x/0 gives +-0x7FFFFFFF with D set; 0/0 gives I; SQRT of a negative number uses |x| and
  sets I; RSQRT likewise.

Known deviations (documented, deliberate):
- results that overflow are 0x7F7FFFFF (host FLT_MAX), not the VU's 0x7FFFFFFF, and an exact
  FLT_MAX result also reports O; exponent-255 inputs are treated as FLT_MAX;
- the VU's addition keeps only one guard bit when aligning the smaller operand, so for large
  exponent differences it can differ from exact truncation by one ulp; not modelled;
- EFU functions use libm (sinf, atanf, expf, sqrtf) instead of the hardware's series;
- R: RNEXT is a 23-bit maximal LFSR (x^23 + x^18 + 1); Sony only says "M-sequence";
- D/T debug bits are ignored (logged once); the M bit is irrelevant without an EE interlock.

### Memory and VU0

- VU0 4 KB data / 4 KB micro, VU1 16 KB / 16 KB; addresses wrap (quadword mask 0xFF / 0x3FF).
  EE mapping as before: 0x11000000 VU0 micro, 0x11004000 VU0 data, 0x11008000 VU1 micro,
  0x1100C000 VU1 data.
- VU0 data addresses 0x4000-0x43FF (quadword 0x400-0x43F) are VU1's registers: VF0-31, then VI0-15
  and control registers 16-31 in the x word (status, MAC, clip, R, I, Q, P, TPC).
- `vu0_callms` copies the macro-mode registers from `ps2_ctx` (vf, vacc, vi, vi_reg = I, vq, vp,
  vr) in, runs until E, and copies them back. R keeps only its 23 mantissa bits (exponent 0x7F) on
  the way in, since the EE-side copy can hold anything (the inline CTC2 stores all 32 bits). VU0
  flags, TPC, CMSAR0/1 and FBRST live in `vu.c`.
- CFC2/CTC2 16-31: status (only sticky bits writable), MAC (read-only), clip, P, TPC, CMSAR0,
  FBRST (RS0/RS1 reset flags/TPC; DE/TE bits read back), VPU-STAT (0), CMSAR1 (write starts VU1).
- XGKICK calls `gif_path1_kick(vu_data_mem(1), addr & 0x3FF)` synchronously at the instruction.

## Interpreter performance

i7-14700F, game build flags (-O1): about 6 ns per pair on a transform/perspective/clip loop
(`bench` in `test_vu.c`, ~1.5 VU cycles per pair) and 1.8 ns per NOP pair. On real game microcode
(recordings, pinned to a P-core) ~8 ns per pair; in the game ~11-13 ns (cache pressure from the
rest of the EE thread). Hot helpers are forced inline because -O1 doesn't inline them.

## Recompiler (`recomp/vu_recomp.py`)

Native-port principle 1: game code is compiled. `python -m recomp vu <elf> -o <dir>` (run by
`scripts/build_game.py` into `<out>/vu/`) translates every microcode image of the game to C;
`runtime/vu.c` runs the compiled program whenever micro memory holds a compiled image, the
interpreter otherwise. The compiled code is exact against the interpreter: same registers, flags,
Q/P/R/I, TPC and data memory, same XGKICK addresses *and issue cycles*, same pair and cycle counts.

**What gets compiled.** Every dvp-as overlay in `.vutext` (`.DVP.ovlytab`: chunks and their VU
addresses) is an image, compiled for VU1, and for VU0 when it fits 4 KB and uses no VU1-only
instruction. Entry points: the image start and every routine start, i.e. the pair after the delay
slot of an E bit (also where VIF MSCNT continues) or of an unconditional B/JR. The game starts
programs at such routine starts through VIF MSCAL (376110651 at 0x2E20, 0x2EE8, 0x2FC8, ...), and
it uploads the 2-pair stub 376110587 (`[E] nop; nop`) over 376110651, whose first two pairs are the
same. One image is one C function per "sticky" specialisation (below) plus a dispatcher.

**Static timing: block versions.** The interpreter's timing model (vu_internal.h) is resolved at
compile time. Code is generated per *block version* = (start pc, timing state), where the state is
what is in flight at the block's first issue cycle, relative to it: VF fields and ILW results not
yet ready (<= 3 cycles), the Q result and up to two P results with their ready times, MAC/status
and clip results queued but not landed, the run of chained VI writes the branch quirk can reach,
and which sticky status bits are known to be set. Inside a version every stall, issue cycle and
landing is a compile-time constant; the generated code only adds constants to its cycle and pair
counters and folds Q/P/flag results at the points where the interpreter would see them. A block
reached in another state gets another version (376110651: 1932 pairs -> ~650 versions, ~3400
pairs of code; capped at 24 versions per block, beyond which the program continues in the
interpreter). Branch targets, return points and routine starts split blocks.

**Flags.** An FMAC op's MAC/status result lands 4 cycles after issue. If no instruction within the
next 3 pairs (including its own pair, along any path) reads the flags, it is folded at once; it is
then not observable when. Folding is lazy: each result is a packed lane mask (`vu_macmask`), `fl`
keeps the newest and `fk` the OR of the sticky components, and MAC/status are computed from them
only at a reader, at block ends and at the end. Sticky bits are only ever set (FSSET, the one way
to clear them, has an immediate known at compile time), so code specialised for "sticky Z S U O
already set" skips accumulating those: each image has functions for sticky bits F (all), B (Z S O)
and 0, picked at program start from `(status >> 6) & 0xF`. Sly 2 starts VU1 programs with 0x2C0
(intro), 0x7C0 (briefing) or 0xFC0 (gameplay) and VU0 programs with 0x2C0; with all set, only the
last FMAC op of a block computes flags. Results near a reader stay queued (fq0-3) and land exactly.
Clip results work the same way.

**VI branch quirk.** The state keeps the run of consecutive chained writes of one VI register
before the current pair (pruned when no conditional branch within 4 pairs reads it); a writer
whose old value a branch may need saves it (`vho_<pc>`), and the branch reads it statically.

**JR/JALR.** Targets are dynamic (376110651 keeps return addresses on a stack in vf02 and calls
`jalr vi15, vi01` with vi01 loaded from data memory). A JR dispatches with a switch over candidates:
the return points of the calls to every subroutine whose body contains it (bodies from BAL targets,
calls assumed to return); JALR: subroutine entries and routine starts. Any other target hands over
to the interpreter (logged once: `jump from ... is not in the recompiled code`); none in Sly 2.

**Handoff.** Where compiled code cannot continue (unknown jump target, branch in a delay slot,
invalid instruction, version cap, and backward jumps once a program nears the interpreter's
runaway limit, so that a program without an E bit stops exactly where the interpreter stops it)
it writes the registers and the pipeline exactly as `vu_exec.c` keeps it (scoreboard, queued flag
and clip entries, Q/P in flight, VI history) and calls `vu_resume()`.

**Floating point.** Every operation's semantics comes from `runtime/vu_ops.h`, shared with the
interpreter. The generated code runs under the VU MXCSR (round toward zero, FTZ/DAZ), but clang
folds operations on constants with round-to-nearest and IEEE denormals: e.g. `loi 0.00787...`
then `addi.w vf25, vf00, I` (1.00787... rounds differently), or `loi -6.68e-39` (a denormal) then
`mulai ACC, vf07, I` (DAZ makes I zero for the U flag). So generated files have `#pragma STDC
FENV_ACCESS ON`, and LOI immediates, MFIR values and vf00 pass through an empty asm (`vuc_opaque`).
They also have `#pragma clang fp exceptions(ignore)`: nothing reads the host's FP exception flags,
so float operations whose results are unused (flag inputs nobody reads) can still be removed.

**Code shape and build time.** VF registers and ACC live in a local struct whose address escapes
(`RG->vf[n]`); with 32 vector locals live across hundreds of blocks clang's register coalescer
went superlinear (330 s for one image), and its SLP vectorizer (useless on intrinsics) cost
minutes, so `build_game.py` compiles `<out>/vu/*.c` with `-fno-slp-vectorize` (~25 s CPU for the
largest file, files in parallel).

**Run time.** `vu_execute()` (vu.c): a write to micro memory (`vu_micro_mem()`, the pointer VIF
MPG writes through, or an EE store) bumps the unit's generation; a program start at (unit, pc)
looks up a slot cached per generation, checking candidate images with memcmp of their chunks.
`PS2_VU_INTERP=1` runs the interpreter only; `PS2_VU_VERIFY=1` runs every compiled program and the
interpreter on a copy of the state and compares everything (`PS2_VU_VERIFY_DUMP=<file>` writes
mismatching programs as a recording for `vu_replay.ps1 -Compiled`). `PS2_PERF` shows the share of
pairs that ran in recompiled code.

**Verification.**
- `scripts/vu_replay.ps1 -Rec <recording> -Compiled <out>\vu`: runs recorded game programs (inputs
  from `PS2_VU_RECORD`, results from the interpreter) through the dispatcher; prints differences
  field by field. Recordings of the intro cutscene, title, briefing and museum gameplay: 0
  differences, 100% of programs compiled.
- In-game `PS2_VU_VERIFY` with the final generator: 0 mismatches over 3.1M programs (release:
  intro cutscene, attract, title) and 12.6M programs (debug build: memory-card prompt, intro, title,
  Binocucom briefing, museum gameplay); the data memory is hashed at every XGKICK, so the GIF
  packets, and with them the frames, are the interpreter's. (Frame dumps of two runs cannot be
  compared directly: VBlanks follow the wall clock, so two interpreter runs differ as well.)
- `tests/unit/vu_recomp/run.ps1`: random programs of every instruction (results read 0-4 pairs
  after their writes, flag reads near writers, Q/P around DIV/EFU, branches on just-written VI
  registers, loops, BAL/JR, JR to non-return points, branches in delay slots, a runaway program)
  on random states including denormals and exponent-255 values. It found three real bugs: a 1-cycle
  VI write after an ILW replaces the ILW's ready time in the interpreter's scoreboard; clang's
  constant folding under DAZ; inexact runaway stops. Since the independent review it also starts
  every program at each of its routine starts (not only 0), and generates JALR, E bits on branch
  pairs, branches in E delay slots, E bits in branch delay slots, M/D/T bits, invalid instructions
  and XGKICK on VU0; `-Cap N` lowers the versions-per-block cap so that the version-cap handoff runs
  (cap 1 and 2: 0 differences). `test_vu_lookup.c` (same run.ps1) checks vu.c's dispatch: entries,
  a start inside an image that is not an entry, stale code after MPG and EE writes, a smaller image
  over a larger one, a VU1 image in VU0's memory, code that runs off its image, and handoffs from
  the delay slot of IBNE/JR/IBEQ (the review's one bug: an invalid instruction or a VU0 XGKICK in
  the delay slot of a conditional branch or JR made the recompiler assert; the handoff now continues
  at the branch target; Sly 2's microcode has no such pair, so the game's code was unaffected).
- Independent review (notes/wip/review-vu-recomp.md): new recordings of scenes the implementer did
  not use (Episode 2 India hub, Episode 4 prison interior `m4_brawl`, Murray's `m1_brawl`, the
  prologue rooftop chase, Rajan's level `m3_boss`; 6000 programs each) replay with 0 differences
  through the interpreter and the compiled code, 100% compiled; in-game `PS2_VU_VERIFY` (release)
  in those scenes plus `m4_hack` and the final boss `m8_boss`: 67M programs, 0 mismatches.
- Known limit of `PS2_VU_VERIFY`: the interpreter's shadow run shares VU1's registers, so a VU0
  program that writes VU1 registers through data addresses 0x4000-0x43FF would have them written
  twice (and could report a false mismatch). Sly 2's VU0 library has no load/store at all.
- Debugging aids: `-DVU_TRACE` makes interpreter and compiled code print every pair's address and
  issue cycle; `VU_FUZZ_ONLY=<program>:<state>` runs one random case.

Speed: see `notes/performance.md`. On the same recordings, pinned to one core: recompiled ~2 ns per
pair against ~8 for the interpreter on a quiet machine, 2-3.5 against 15-21 on a loaded one; in game
the VU share of the EE thread drops from ~60% to ~10-15%.

Steps while building it: the first version (static timing, flags folded eagerly) ran the replays
at 2.1-3.3 ns per pair against 9-11 interpreted on a loaded machine; FMAC flags as lazy lane masks
made it ~15% faster; VF/ACC in memory instead of 32 vector locals cut the compile time (330 s ->
~25 s for the largest image) without a measurable cost; the sticky-bit specialisation (with all
sticky bits set, as in gameplay, only a block's last FMAC op computes flags) and compiling the
generated code with FP exceptions ignored (dead flag computations become removable: ~17% less
code in the sticky-F functions) reduce the flag work further.

## Disassembler

`tests/unit/vu/vudis.py` (stand-alone Python):

    <python> tests\unit\vu\vudis.py --sly2 <SCUS_973.16> [--stats]
    <python> tests\unit\vu\vudis.py --bin dump.bin --base 0x0 --vu 1

`--sly2` finds the programs through the dvp-as overlay table (`.DVP.ovlytab` + `.DVP.ovlystrtab`
sections: name, EE address, VU address per MPG chunk; the `.DVP.overlay..*` sections themselves
are zero-filled placeholders). `.vutext` itself is a set of DMA packets: a tag and VIF
NOP + MPG (up to 256 instructions) before each chunk. All 4,220 pairs decode with no invalid
instruction, and the unit tests decode them again through the C decoder.

## Sly 2's microprograms (.vutext 0x00259DF0-0x00262330)

Eight overlays; every VU1 program is uploaded at VU address 0, so they replace each other.
"Loaded from" = EE code that builds the DMA address (lui/addiu pair).

| Overlay id | Unit | Pairs | Chunks (EE) | Loaded from | Routines (E bits at) | Character |
|------------|------|-------|-------------|-------------|----------------------|-----------|
| 11753019 | VU0 | 84 | 0x259E00 | 0x1A78C4 | 0x0000, 0x0050, 0x00E0, 0x0110, 0x0140, 0x01B0, 0x0290 | VCALLMS library, see below |
| 376109499 | VU1 | 472 | 0x25A0C8, 0x25A8D0 | 0x13A840 | 0x0380, 0x0860, 0x0EB0 | 3 routines; CLIP x12, DIV x4, EFU (ESIN, ESQRT, ERLENG), XGKICK x3 |
| 188055835 | VU1 | 280 | 0x25AFA8, 0x25B7B0 | 0x211A98 | 8 ends (0x0020 ... 0x08B0) | no FDIV/CLIP, XGKICK x9: screen-space / packet builder |
| 188054331 | VU1 | 292 | 0x25B888, 0x25C090 | 0x16EA30 | 0x0000, 0x07E0, 0x0858 | transform loop, 3 vertices per pass, DIV per vertex, BAL/JR subroutines |
| 188054619 | VU1 | 972 | 4 chunks from 0x25C1C8 | 0x16DA74 | 0x0000, 0x1A58, 0x1A90, 0x1BB8 | clipping renderer: CLIP x27, FCAND/FCOR/FCSET/FCGET, DIV x19, BAL x25 |
| 376110587 | VU1 | 2 | 0x25E060 | 0x1A7934 | 0x0000 | `[E] nop` stub |
| 752221467 | VU1 | 186 | 0x25E098 | 0x167FDC | 0x0000, 0x0540 | ELENG/ERLENG with WAITP, DIV x3 |
| 376110651 | VU1 | 1932 | 8 chunks from 0x25E688 | 0x1609D8, 0x16E1C0 | 10 ends (0x0000 ... 0x3B78) | main renderer, fills 15.1 KB of the 16 KB: DIV x24, CLIP x23, BAL x93, JR x30, XGKICK x10, LOI x27 |

Totals: 4,220 pairs; 4,185 of 8,440 slots are NOPs. Most common real ops: LQ 470, ILW 259,
SQ 201, IADDIU 192, IADD 173, IBEQ 173, MADDAy 142, MULAx 136, MADDw 125, BAL 125. Present:
XTOP x23 (double-buffered VIF input), XGKICK x34, CLIP x66, DIV x56, WAITQ x27, WAITP x17,
FSAND x25, FCAND x24, FCOR x18, FMAND x7, FCSET x6, FCGET x4, EFU: ERLENG 10, ELENG 9, ESIN,
ESQRT, ERSQRT. Absent: ILWR/ISWR, RNEXT/RGET/RINIT/RXOR, XITOP, FMEQ/FMOR/FSEQ/FSOR/FSSET,
EATAN*, EEXP, ESUM.

VU0 library (VCALLMS targets found in the recompiled EE code: 0x2, 0xC, 0x1E, 0x24, 0x2A, 0x38,
i.e. the pair right after each E delay slot):
- 0x0010: transform vf24 by the matrix in vf28-31 (w = 1) and grow a bounding box (MINI/MAX vf25/26);
- 0x0060: transform vf5-vf8 by the 4x4 matrix vf1-vf4;
- 0x00F0 / 0x0120: transform vf5 / vf6 as points (w = 1);
- 0x0150: rotate vf5 by vf1-vf3, normalise with RSQRT + WAITQ, w = dot with vf4;
- 0x01C0: intersection test: FMAND on the sign bits of `msubx.xyzw vf00, ...` (vf00 used as a
  flags-only destination) exactly 4 cycles later, FSAND on a SUB 4 cycles later, then DIV/WAITQ,
  result clamped with MINIw. Used as a unit test: with flags visible any earlier, both checks
  take the wrong branch.

### Timing tricks the emulation has to get right

- Q software pipelining: the transform loops issue the DIV for vertex n+1 and then multiply
  vertex n by Q two to four pairs later, deliberately reading the old Q (14 such sites in
  188054331, 188054619 and 376110651). In 188054331's loop (0x0470-0x04E8) DIVs are only five
  pairs apart, so the FDIV-busy stall is what keeps each Q read on the right vertex.
- Flag reads placed exactly at the 4-cycle latency (VU0 0x01C0) or 1-3 pairs after a newer FMAC
  op, reading the older op's flags (FSAND in 188054331/188054619/376110651).
- LOI one pair ahead of its use, including chains of `[I]` pairs whose upper op uses the previous
  immediate (above).
- MAXi/MINIi on integer bit patterns (GIF tag building) and the FLT_MAX constant 0x7F7FFFFF.
- Not used: VI-chain branches, work in E-bit delay slots, branches in delay slots.
