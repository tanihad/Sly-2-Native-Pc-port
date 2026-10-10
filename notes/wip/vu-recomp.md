# vu-recomp: statically recompile VU microcode to native code

**Goal**: compile the game's VU1/VU0 microprograms to C at build time (exact w.r.t. the interpreter:
state hash, XGKICK addresses/cycles, pair/cycle counts), dispatch to them at run time by the micro
memory they cover, interpreter stays as reference/fallback; measure and document the speedup.

## Design (decided)
- `recomp/vu*.py`: Python decoder mirroring runtime/vu_exec.c's decode (sources, dests, latencies,
  stall kinds, VI chain info), program discovery from the ELF's .DVP.ovlytab overlays (one compile
  unit per overlay image, entries = overlay start + pair after every E-bit delay slot + extra
  entries), exploration + C emission.
- Static timing by *block versioning*: a version = (pc, npc, ending, timing state). Timing state =
  pending VF field/VI(ILW) ready times (<=3 cycles), Q/P pending ready times, pending (unfolded)
  MAC/status and clip entries, the VI-branch chain run. All stalls/cycle offsets are compile-time
  constants inside a version; runtime keeps only `cyc` and `n` counters.
- Flags folded eagerly at the FMAC op when no flag reader (or JR/handoff) is within the next 3 pairs
  (same pair included); otherwise kept pending in canonical vars fq0..3 (FIFO). Same for clip.
- VI branch quirk: static run of consecutive chain writes of one register (pruned by lookahead of
  branch registers); old values saved in vho_<pc> locals.
- JR/JALR: switch over candidate targets (return points of the enclosing subroutines; JALR: routine
  starts); unknown target / version cap / invalid op / branch in delay slot -> handoff: materialize
  the pipeline state into vu_state and continue in the interpreter (vu_exec.c gets a resume entry).
- Runtime: micro-memory generation counter bumped by vu_micro_mem()/EE writes; per (unit, entry)
  cache {gen, fn, id}; validation = memcmp of the unit's covered ranges. PS2_VU_INTERP=1 forces the
  interpreter, PS2_VU_VERIFY=1 runs both and compares.
- Shared op semantics in runtime/vu_ops.h used by both interpreter and generated code.

## Done
- Recordings started (release build, build\vurec\{intro,title,brief}.vurec).

## In progress
- Writing recomp/vu decoder + codegen.

## Next
1. vu_ops.h refactor of vu_exec.c (replay must still match).
2. Python decoder + exploration + emitter; build integration (build_game.py, `python -m recomp vu`).
3. Runtime dispatch in vu.c, resume entry in vu_exec.c.
4. vu_replay.ps1 -Compiled; exactness on all recordings; museum gameplay recording.
5. In-game: frames identical (compare_frames), PS2_PERF before/after; docs.

## Findings / traps
- 376110651 uses a software return stack in vf02 (mfir/mr32/mtir) and `jalr vi15, vi01` with vi01
  loaded from data memory (callbacks): JR targets are not all statically resolvable.
