# review-vu-recomp: independent review of the VU microcode recompiler

**Goal**: independently verify exactness of recomp/vu_recomp.py + runtime/vu.c (new recordings from
unseen scenes via warp, replay interp vs compiled, in-game PS2_VU_VERIFY), review timing model,
lookup/hash scheme, thread safety, fallback; failing test first for each bug, fix, verdict.

## Done
- Merged main (3be5093) into next/vu-recomp (fast-forward).

- Code review of vu_recomp.py / vu_decode.py against vu_exec.c (pair order, stalls, FMAC/clip flag
  windows FR/CR incl. stalls and handoffs, sticky ks specialisation + FSSET, Q/P folding, EFU
  queue, VI branch chain + vho saves + handoff vih, E bit with branch/delay slot, tpc, I bit,
  runaway guard completeness, JR/JALR candidates + default handoff) and vu.c (gen bumps: VIF MPG
  per chunk + EE stores are the only micro writers; memcmp image match; per-(unit,pc) slots; VU0 vs
  VU1 units; non-entry starts interpreted; all VU work on the EE OS thread; interrupts deferred, so
  no reentry during XGKICK). No exactness bug found by reading. Release build: 10 units, 0 version
  caps (report in this file's Findings).
- Review helper (not committed): scratchpad rv_run.ps1 = warp_run with a unique exe name
  (sly2_rv_<name>.exe), -Record/-Verify/-Interp.
- In-game PS2_VU_VERIFY (release) running: ep2hub (-Episode 2 -Play), m4brawl (-Direct), m1brawl
  (Murray, -Direct): 4.2M programs each so far, 0 mismatches, 100% recompiled.

## In progress
- Verify runs finishing (200 s). NOTE: verify mode runs at ~15-30 VBlanks/s, so PS2_VU_RECORD_FROM
  5500 is never reached there: record in separate normal-mode runs.

## Next
1. Record runs (normal mode, release): ep2hub, m4brawl, m1brawl, rooftop chase
   (t0_rooftop_start -Direct), boss (m3_boss or m8_boss -Direct); replay interp + -Compiled.
2. Verify runs for chase + boss.
3. Extend tests/unit/vu_recomp fuzzer: JALR, multiple entries (start at every routine start), E bit
   on a branch / branch in E delay slot / E in a branch delay slot; lookup tests (stale MPG, EE store,
   partial overlap, VU0 vs VU1, non-entry start).
4. Fix any bug (failing test first), verdict.

## Findings / traps
