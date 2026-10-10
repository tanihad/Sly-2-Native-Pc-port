# review-play-1: independent review of play/play-1

**Goal**: verify play-1 (gameplay reached, AA1 lines, OSD/cache/SIF syscalls, input/dump tooling)
independently: rebuild, tests, reproduce evidence, check root-cause quality, principles, thread
safety, regressions vs main, cleanliness; fix suspected bugs test-first on this branch; verdict.

## Done
- Read CLAUDE.md, principles, checkpoints, progress, hle-architecture, notes/wip/play-1.md.
- Branch = main (9d6dd51) + fix/fix-perf at 7491a63 (merge 407f913) + play-1 commits.
  fix/fix-perf has moved on since (6 commits up to 9aa0b3f); play-1's own diff is `git diff 7491a63 HEAD`.

- Read the play-1 diff (gs_draw.c AA1, ps2_runtime.c syscalls/tooling, window.c input).
- Debug build OK; run_tests.py 16/16; all 11 unit suites pass (window with -UnitOnly).
- Reproduced (build\review\new1, fresh memcard PS2_MEMCARD_DIR=build\review\mc_new): memcard
  prompt -> intro -> title -> briefing -> gameplay at ~v4700: run, jump, attack, pause menu.
- Main reference build (git archive main -> build\review\main_tree, built to build\review\main_build):
  memcard prompt frames identical except the pulsing triangle glyph; intro movie in sync to v1560.
- Trial merges: clean with current fix/fix-perf (9aa0b3f); conflicts with audio/audio-iop in
  ps2_runtime.c, sif.c, window.c (context only, integrator).

## In progress
- Save and Quit + Resume reproduction via live play (build\review\live, mc_new).

## Next
1. Save/resume check; release build; anchors in PS2_INPUT_FILE lines (suspected bug: never fire).
2. Write failing tests for suspected bugs, fix, commit.
3. Verdict.

## Findings
- Syscall 0x6B wrapper at 0x234CF0 is called from 0x2391C8 (SIF init path); 0x4A/0x4B from the
  libkernel OSD version check at 0x239780 (sets version bits 13-15 = 1 and reads it back).
- Syscall 0x5B (kernel patch helper, a0=0xFC..0xFE) is still "not implemented" (also on main).
