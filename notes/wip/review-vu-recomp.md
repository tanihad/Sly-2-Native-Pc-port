# review-vu-recomp: independent review of the VU microcode recompiler

**Status: complete.**

**Goal**: independently verify exactness of recomp/vu_recomp.py + runtime/vu.c (new recordings from
unseen scenes via warp, replay interp vs compiled, in-game PS2_VU_VERIFY), review timing model,
lookup/hash scheme, thread safety, fallback; failing test first for each bug, fix, verdict.

## Verdict
Exact and sound. The design (static timing via block versions, lazy flag folding within the 4-pair
landing window, sticky-bit specialisation, VI-chain saves, exact handoff to the interpreter, and the
memcmp-validated per-(unit, pc) dispatch) matches vu_exec.c in every path checked. One real bug
found and fixed (recompiler assertion/wrong npc for a handoff in a dynamic branch's delay slot);
it cannot occur with Sly 2's microcode (generated game code unchanged byte for byte).

## Done
- Merged main (3be5093) into next/vu-recomp (fast-forward).
- Code review of vu_recomp.py / vu_decode.py against vu_exec.c (pair order, stalls, FMAC/clip flag
  windows FR/CR incl. stalls and handoffs, sticky ks specialisation + FSSET, Q/P folding, EFU
  queue, VI branch chain + vho saves + handoff vih, E bit with branch/delay slot, tpc, I bit,
  runaway guard completeness, JR/JALR candidates + default handoff) and vu.c (gen bumps: VIF MPG
  per chunk + EE stores are the only micro writers; memcmp image match; per-(unit,pc) slots; VU0 vs
  VU1 units; non-entry starts interpreted; all VU work on the EE OS thread; interrupts deferred, so
  no reentry during XGKICK). Release build: 10 units, 0 version caps.
- In-game PS2_VU_VERIFY (release, build\warp\<name>\run.log): ep2hub 8.4M, m4brawl 9.4M, m1brawl
  8.4M, rooftop chase 5.2M, m3_boss 5.2M (intro), m4_hack 7.3M, m8_boss 23.1M: 67M programs,
  0 mismatches, 100% recompiled, no interpreter fallback logged.
- New recordings build\vurec\rv_{ep2hub,m4brawl,m1brawl,chase,boss3}.vurec (6000 programs each):
  replay interp and -Compiled: 0 programs differ, 100% compiled. Interp 8.2-8.4 ns/pair vs compiled
  1.42-1.51 (quiet-ish); chase/boss under load 18.9/26.3 vs 3.1/4.4.
- Fuzzer extended (tests/unit/vu_recomp/gen.py): every routine start as an entry, JALR, E bit on
  branches, branch in E slot, E in branch slot, M/D/T bits, invalid instructions, VU0 XGKICK,
  --cap N (run.ps1 -Cap). Seeds 1-7 incl. cap 1 and 2 (version-cap handoffs): 0 differ.
- test_vu_lookup.c: vu.c dispatch (entries, non-entry start, stale code after MPG/EE writes, overlap,
  VU0 vs VU1, code past its image) + ld/le/lf delay-slot handoffs. Failing first (generator
  assert), fixed in recomp/vu_recomp.py (11a6c10).
- All unit test folders (window -UnitOnly) and tests/run_tests.py pass; vu_recomp default run 55 s.
- notes/vu-microcode.md Verification section updated.

## Next
- Nothing required. Optional: make PS2_VU_VERIFY snapshot/restore VU1's registers around VU0
  programs (only matters for VU0 code that stores to 0x4000-0x43FF; Sly 2's VU0 library has no
  loads/stores). Optional: vuc_verify could skip the shadow run when the unit returns
  VUC_NOT_COMPILED (only possible if an entry hit the version cap; none do).

## Findings / traps
- PS2_VU_VERIFY slows the game to ~15-30 VBlanks/s; PS2_VU_RECORD_FROM counts delivered VBlanks,
  so record in separate normal-mode runs.
- Fuzz programs: entries inside the trailing subroutine (after `b`/`bal` in the E delay slot) run
  `jr vi14` with a random vi14 -> runaways; gen.py excludes entries >= sub.
- Review helper (not committed): scratchpad rv_run.ps1 = warp_run with a unique exe name
  (sly2_rv_<name>.exe) and -Record/-Verify/-Interp.
