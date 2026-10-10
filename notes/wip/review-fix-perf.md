# review-fix-perf: independent review of fix/fix-perf

Status: in progress

## Goal
Verify fix-perf independently (rebuild, unit tests, game run, reproduce perf evidence, compare
frames with main), check root-cause quality, native-port principles, thread safety, regressions and
cleanliness; fix confirmed bugs with a failing test first; give a verdict.

## Done
- main (9d6dd51) already merged (HEAD..main empty; eb96e20..9d6dd51 only notes/progress.md).
- Fresh builds: build/rv_rel (release, 32 s), build/rv_def (-O1). main's release from git archive:
  build/rv_mainsrc -> build/rv_mainrel.
- All 11 tests/unit/*/run.ps1 pass; tests/run_tests.py 16/16 + overrides check pass.
- Release run, fresh card, standard input, 75 s: 59.9 VBlanks/s in every 1-s window (movie 16.7 ms
  busy = demux loop; prompt/intro/attract/title/briefing EE busy 9.4-14.7 ms, GS 6-14.7 ms), flips
  58-63/s. main's release: 73 dumps in 170 s (~25.8 VBlanks/s).
- VU replay build/brief.vurec: branch 7.80 ns/pair, main's interpreter 9.92 ns/pair, both 0 programs
  differ (main runtime + vu_record.h copy in build/rv_mainrt).
- Prompt frames vs main differ (7658 px <= 32 levels in text) at full speed. Root cause: game timing,
  not renderer: (a) GS recording at the prompt (build/rv_prompt.gsrec) replayed through main's GS code
  (shim build/rv_tools/gs_replay_main.c) and the branch's (sync and 8 workers): 20/20 identical at
  tolerance 0; (b) branch slowed with PS2_PERF_BURN=16 in v400-700: prompt frames match main within
  2 levels (plus field-parity bottom row).

- GS replays (own recordings, release exe): intro v1000+16 main vs branch <= 2 levels on <= 28 px
  per frame; briefing v2640+16 <= 3 levels on <= 12 px; async 8 workers == sync bit-identical on
  prompt, intro, briefing. GS-only briefing: main 25.8 ms/VBlank, branch sync 25.5, 8 workers 11.4.

- Headroom spot check (briefing, -From 2400 -To 3700): burn 6 ms on EE -> 100% VBlanks, flips
  59.2-60.2 (vs 60.0-60.4), VU pairs/VBlank 840k -> 610k (game draws less; documented).
- BUG fixed (gs_draw.c tex_prepare): an in-place filter sprite ("local": each pixel samples the
  texel at its own position) was treated as pixel-local even when its area went beyond the texture
  (CLAMP / REPEAT / REGION modes): pixels then sample other pixels' texels in pages of other bands,
  unordered -> nondeterministic output with pixel workers (1 in 7 async runs wrong) and live-read
  semantics in sync drawing (differs from the snapshot semantics of all other feedback). Test
  t_direct_order "in-place filter beyond its texture" (CLAMP and REPEAT, 64x64 sprite on a 64x32
  texture): failed sync (32 rows) and async (4 rows, intermittent) before; passes now (async 10/10).
  Fix: texels_unwrapped() (shared with the P.nowrap test) required for local. Replays of prompt,
  intro, briefing recordings: bit-identical before/after (sync and 8 workers).

## In progress
- Thread-safety review (gs_thread.c, gs_draw_pixel.c, gs_draw_tex.c, gs.c reviewed: OK apart from
  the bug above).

## Next
2. Review thread safety of gs_thread.c / gs_draw_pixel.c / perf.c / ps2_runtime.c pacing.
3. Review `restrict ctx` in codegen (callees touching ctx via other pointers?), overrides callers.
4. Headroom spot check; default build run.
5. Write verdict.

## Findings
