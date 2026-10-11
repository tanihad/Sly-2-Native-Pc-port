# vu-recomp: statically recompile VU microcode to native code

**Goal**: compile the game's VU1/VU0 microprograms to C at build time (exact w.r.t. the interpreter:
state hash, XGKICK addresses/cycles, pair/cycle counts), dispatch to them at run time by the micro
memory they cover, interpreter stays as reference/fallback; measure and document the speedup.

## Design (implemented)
- `recomp/vu_decode.py`: decoder mirroring runtime/vu_exec.c's decode exactly. `recomp/vu_recomp.py`:
  overlay discovery (.DVP.ovlytab; one image per overlay per VU), static analysis (successors,
  leaders, JR candidates from subroutine bodies, look-ahead tables FR/CR/BR), block-version
  exploration with a static timing state, C emission. `python -m recomp vu <elf> -o <dir>` (or
  `--image` for raw micro code); build_game.py writes <out>/vu and compiles it with
  -fno-slp-vectorize.
- Block versioning: version = (pc, pending VF fields / VI ready times, Q/P ready, queued MAC/clip
  entries, VI chain run, sticky bits known set `ks`). All stalls are compile-time constants.
- Flags: eager unless a reader/JR within 3 pairs; folded as lane masks (`fl` newest, `fk` OR)
  and applied to mac/status only at readers / block ends / end / handoff; components whose sticky
  bit is known set (ks) are not accumulated. Per image 3 functions (ks = F, B, 0; own .c files)
  + dispatcher on (status >> 6) & 0xF.
- VF/ACC live in a local struct whose address escapes (`RG->vf[n]`); other state in locals.
- Constants passed through `vuc_opaque` (LOI, MFIR, vf00) + `#pragma STDC FENV_ACCESS ON`.
- Handoff to the interpreter (vu_resume) for unknown JR targets, branch in delay slot, invalid ops,
  version cap, and near VU_MAX_PAIRS on backward jumps (one stub per target version: exact runaway).
- Runtime (vu.c): micro_gen bumped by vu_micro_mem()/EE micro writes; per (unit, pc) slot cache;
  whole-image memcmp validation; vu_execute(); PS2_VU_INTERP=1, PS2_VU_VERIFY=1 (+
  PS2_VU_VERIFY_DUMP=<file>); PS2_PERF shows "% recompiled".

## Done
- Recordings build\vurec\{intro,title,brief,museum}.vurec (6000 programs each) + mismatch.vurec:
  replay -Compiled 0 differ, 100% compiled.
- In-game PS2_VU_VERIFY: standard path 8.4M programs and museum gameplay 16.7M programs, 0 mismatches
  (older generator versions; redo with final).
- tests/unit/vu_recomp: random-program differential test; seeds 1-24 pass (found 3 real bugs: VI
  scoreboard overwrite after ILW, DAZ constant folding of LOI denormals, runaway).
- Pinned replay (VU_REPLAY_CPU=4, quiet machine): interpreter ~7.9-8.0 ns/pair, compiled (locals +
  lazy flags) ~1.9-2.1 ns/pair.
- In-game A/B (scripts/vu_ab.ps1, loaded machine): EE busy 12-17 -> 6.5-9.5 ms/VBlank; intro
  cutscene VBlanks 85-92% -> 97-100%; title: GS thread ~16 ms and flips ~55 (game draws more).

- Resumed after usage limit (22:20): final generator (de037da) replay on all 5 recordings exact,
  100% compiled. Loaded machine (4 other sly2 instances, 85% CPU), CPU 4 pinned, interleaved:
  interp 15.0-20.8 ns/pair, compiled 2.0-3.5 (intro 3.2-3.5, title 2.5-2.7, brief 2.5-2.8,
  museum 2.0-2.3): 6-7x.
- Release in-game PS2_VU_VERIFY (build\verrel, 150 s, intro+attract+title): 3.1M programs, 0
  mismatches. Debug build rebuilt with final generator (build\sly2).
- All unit tests (15 dirs, window -UnitOnly) + tests/run_tests.py pass.
- Frames between two runs are not comparable (frames_interp vs frames_interp2 differ too: wall-clock
  VBlanks); identity follows from VERIFY (data memory hashed at every XGKICK = GIF packets).

## Next
1. Debug in-game PS2_VU_VERIFY into museum gameplay (build\verdbg running), check frames.
2. In-game vu_ab.ps1 (release) + headroom.ps1 on the intro cutscene, compiled vs interp.
3. Docs: notes/performance.md (VU section, before/after, threads table), notes/progress.md.

## Findings / traps
- 376110651 uses a software return stack in vf02 and `jalr vi15, vi01` (target from data memory).
- The 2-pair stub 376110587 is uploaded over 376110651 (identical first 2 pairs); the game starts
  programs at routine starts (after B/JR delay slots): entries = start + pair after E/B/JR slots.
- clang folds float ops on known constants with RNE and IEEE denormals: FENV_ACCESS + opaque asm.
- Interpreter's VI scoreboard: a 1-cycle VI write after an ILW replaces the ILW's ready time.
- Compile time: 32 vector locals live across ~650 blocks -> Register Coalescer superlinear (330 s);
  SLP vectorizer also minutes. Fixed by VF/ACC in memory + -fno-slp-vectorize.
- .NET file APIs resolve relative paths against the main checkout; PowerShell .Replace on CRLF
  files fails silently; Start-Process ExitCode needs $p.Handle; long link lines need a .rsp.
- Machine shared (other agents, user apps): pin replays to a P-core, interleave A/B.
