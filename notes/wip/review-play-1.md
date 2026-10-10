# review-play-1: independent review of play/play-1

**Goal**: verify play-1 (gameplay reached, AA1 lines, OSD/cache/SIF syscalls, input/dump tooling)
independently: rebuild, tests, reproduce evidence, check root-cause quality, principles, thread
safety, regressions vs main, cleanliness; fix suspected bugs test-first on this branch; verdict.

## Done
- Read CLAUDE.md, principles, checkpoints, progress, hle-architecture, notes/wip/play-1.md.
- Branch = main (9d6dd51) + fix/fix-perf at 7491a63 (merge 407f913) + play-1 commits.
  fix/fix-perf has moved on since (6 commits up to 9aa0b3f); play-1's own diff is `git diff 7491a63 HEAD`.

## In progress
- Reading the play-1 diff (gs_draw.c AA1, ps2_runtime.c syscalls/tooling, window.c input).

## Next
1. Review diff; build debug + release; unit tests + run_tests.py.
2. Reproduce: new game -> briefing -> gameplay; pause; save/resume; compare boot frames with main.
3. Write failing tests for suspected bugs, fix, commit.
4. Verdict.

## Findings
