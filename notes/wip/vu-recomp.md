# vu-recomp: statically recompile VU microcode to native code

**Goal**: compile the game's VU1/VU0 microprograms to C at build time (exact w.r.t. the interpreter:
state hash, XGKICK addresses/cycles, pair/cycle counts), dispatch to them at run time by the micro
memory they cover, interpreter stays as reference/fallback; measure and document the speedup.

## Design (implemented)
- `recomp/vu_decode.py`: decoder mirroring runtime/vu_exec.c's decode exactly (sources, dests,
  latencies, stall kinds, VI chain info). `recomp/vu_recomp.py`: overlay discovery from
  .DVP.ovlytab (one compile unit per overlay image per VU), static analysis (successors, leaders,
  JR candidates via subroutine bodies, look-ahead tables FR/CR/BR), block-version exploration with
  a static timing state, C emission. `python -m recomp vu <elf> -o <dir>` (or `--image` for raw
  micro code). build_game.py runs it into <out>/vu.
- Static timing by *block versioning*: a version = (pc, timing state) with pending VF fields / ILW
  results, Q/P ready times, pending MAC/status and clip entries (fq0-3/cq0-3), the VI chain run.
  All stalls are compile-time constants; runtime keeps only `cyc` and `n`.
- Flags folded eagerly unless a reader (or JR) is within the next 3 pairs (same pair included).
- JR/JALR: switch over candidates; unknown target, branch in delay slot, invalid op, version cap ->
  handoff (pipeline state written to vu_state, vu_resume() in vu_exec.c continues).
- Runtime (vu.c): micro_gen bumped by vu_micro_mem()/EE micro writes; per (unit, pc) slot cache;
  whole-image memcmp validation; `vu_execute()`; PS2_VU_INTERP=1, PS2_VU_VERIFY=1 (+
  PS2_VU_VERIFY_DUMP=<file> writes mismatching programs as a recording); PS2_PERF shows % recompiled.
- runtime/vu_ops.h: op semantics shared by interpreter and generated code. Generated files have
  `#pragma STDC FENV_ACCESS ON` (else clang folds e.g. `addi.w vf25, vf00, I` after a LOI with
  round-to-nearest; the VU runs round-toward-zero).

## Done
- Recordings build\vurec\{intro,title,brief}.vurec (6000 programs each; release, standard path).
- Replay -Compiled: 0 programs differ on all 3 + mismatch.vurec; 100% programs compiled;
  2.1-3.3 ns/pair vs 9-11 interpreted (replay, loaded machine).
- In-game PS2_VU_VERIFY (release, standard path, 100 s): 8.4M programs, 0 mismatches.
- In-game PS2_PERF first look: EE busy 12-14 -> 5-8 ms/VBlank, VU ~12 -> 3-5 ns/pair.
- scripts/vu_rec_stats.py: which images/entries recordings use.

## Next
1. Museum gameplay: debug build + anchors, PS2_VU_VERIFY run, and a recording.
2. Differential random-program test (tests/unit/vu_recomp): generator + harness comparing
   vu_run vs compiled on random programs/states (VI chain quirk, EFU, FSSET, branches...).
3. Frame identity check (compare_frames) compiled vs interpreter.
4. Interleaved A/B perf script (machine is shared: other agents' sly2.exe); profile generated code
   and optimize (flags, vclean, register traffic); build time (biggest unit 24 s at -O2).
5. Docs: notes/vu-microcode.md, notes/performance.md; unit tests + run_tests.py pass.

## Findings / traps
- 376110651 uses a software return stack in vf02 (mfir/mr32/mtir) and `jalr vi15, vi01` with vi01
  loaded from data memory: JR targets resolved by candidate lists (no unknown jumps seen so far).
- The 2-pair stub 376110587 (`[E] nop; nop`) is uploaded over 376110651 whose first 2 pairs are
  identical; the game then starts programs at routine starts (after B/JR delay slots) such as
  0x2E20/0x2EE8 -> entries = image start + pair after every E/B/JR delay slot.
- PowerShell .Replace() on CRLF files silently fails: use the Edit tool.
- The GS thread can become the bottleneck once the EE is fast (seen 15-16 ms/VBlank with flips
  50-55 while another agent's game was running): measure interleaved.
