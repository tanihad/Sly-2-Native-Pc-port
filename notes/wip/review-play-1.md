# review-play-1: independent review of play/play-1

**Status: complete.** Verdict: approve, with one tooling bug fixed (7cd52d2). Merge fix/fix-perf
first: this branch carries its older snapshot 7491a63.

**Goal**: verify play-1 (gameplay reached, AA1 lines, OSD/cache/SIF syscalls, input/dump tooling)
independently: rebuild, tests, reproduce evidence, check root-cause quality, principles, thread
safety, regressions vs main, cleanliness; fix suspected bugs test-first on this branch; verdict.

## Done
- Branch = main (9d6dd51) + fix/fix-perf at 7491a63 (merge 407f913) + play-1 commits. The play-1
  changes alone are `git diff 7491a63 HEAD`. The merge's ps2_runtime.c conflict was resolved
  correctly: both sides' changes are kept, and dump_from is folded into fix-perf's present_frame.
- Debug and release builds link. run_tests.py passes 16/16. All 11 unit suites pass (window with
  -UnitOnly).
- New game reproduced with a fresh memcard (build\review\new1; PS2_MEMCARD_DIR=build\review\mc_new):
  memcard prompt (SQUARE), intro, title, briefing, then gameplay at ~v4700. Run, jump, attack and
  the pause menu all work.
- Save and Quit reproduced in live play (build\review\live). Sequence: pause, down, cross, up
  ("Yes, Quit"), cross, notice, SQUARE (= Continue). This rewrites save1 and the game restarts at the
  intro. The title then says "Press START button to Resume Game", and START resumes Sly in the
  museum at (4915,-5211).
- Main reference build: `git archive main` into build\review\main_tree, built in
  build\review\main_build. Memcard prompt frames are identical apart from the pulsing triangle
  glyph. The intro movie stays in sync up to v1560. On main, the log shows Get/SetOsdConfigParam,
  iFlushCache and sceSifStopDma as "not implemented" and "AA1 drawn without it", as reported.
- Integration: the merge with the fix/fix-perf tip (9aa0b3f) is clean. In that merged tree
  (build\review\merged_tree), gs-draw (168 checks) and window pass, and the game build resumes
  into gameplay at ~58 VBlanks/s. Against audio/audio-iop there are context conflicts in
  ps2_runtime.c, sif.c and window.c, which the integrator will need to resolve.
- BUG, fixed in 7cd52d2: "@func" anchors in live PS2_INPUT_FILE lines never fired, because the
  runtime only registered the anchors it found in PS2_INPUT at boot. Repro: the live line
  "@187D80+0:none" stayed unresolved through gameplay, pause, the save and the title (where 187D80
  runs). Fix: ps2_func_calls registers an anchor on its first query (on the emulation thread, like
  the counting), and anchors_init is removed. Release builds now say that anchors never fire. After
  the fix, the same line fires at pad frame 290, and the release run prints the warning.

## Findings
- AA1 (gs_draw.c draw_line): two pixels per step across the minor axis, with coverage replacing As,
  and only when ABE is set. P.iip is forced on so that per-span alpha reaches the colour stage,
  while prim_iip keeps the real IIP for plane setup. Sprites and points reset P.d* in set_steps and
  draw_point, so forcing iip there is harmless. Unit-tested, including a Sly-style outline (Z
  GREATER, RGB_ONLY). State is touched only on the GS thread. The outlines in this dark level are
  faint, and there is no hardware reference to compare against.
- The dashed thin dark "line" near Sly's ear in the intro is not an outline. It is a thin
  textured tristrip (rgba 0, ATST GEQUAL 0x7E, Z GREATER) narrower than a pixel, and it is broken
  on main as well. Trace: build\review\trace_line2.txt.
- Syscalls: the 0x6B wrapper at 0x234CF0 is called from 0x2391C8 on the SIF init/reboot path. The
  0x4A/0x4B wrappers are called from the libkernel OSD version check at 0x239780 (it sets version
  bits 13-15 to 1 and reads them back) and from 0x24A868/0x24A888. iFlushCache is -0x68, called
  from 0x167F8C. The wrappers for 0x6E/0x6F (Get/SetOsdConfigParam2) exist, but no run called them.
- Still "not implemented" (already so on main): syscall 0x5B at 0x23995C (a0 = 0xFC..0xFE, kernel
  patch helper). It has not mattered so far.
- Minor, not fixed: scripts\live_look.ps1 and live_climb.ps1 hardcode the main checkout's python
  path, while the other scripts find it through .git. PS2_DUMP_RING never deletes a dump whose
  "deleting" VBlank was skipped (display off). AA1 on triangles and sprites is still ignored (logged
  once).
- Scope: the first objective of the prologue (the elevator switch via the whale skeleton) was not
  completed. Bentley's hacking, Murray and Episode 1 are untested. None of the runs showed a
  runtime error during gameplay.
