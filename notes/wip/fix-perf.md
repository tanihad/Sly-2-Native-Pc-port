# fix-perf: full speed (steady 59.94 fields/s) with headroom, release build

Status: in progress

## Goal
Game runs at full NTSC speed with plenty of headroom, measured in the release build; pacing never
faster than real time; smooth presentation. Report before/after numbers per change.

## Done (commits on fix/fix-perf)
1. c5aaaab Instrumentation (runtime/perf.h, runtime/perf.c): PS2_PERF=1 per-second report
   (vblanks serviced/due, flips, busy ms/vblank, EE/GS/VU/VIF/IPU/IOP/present/gswait/idle shares,
   GS thread ms/vblank); PS2_PROFILE=1 (EE thread) / =gs (GS thread) stack-sampling profiler with
   self + inclusive tables from the exe's COFF symbols; PS2_PROFILE_SKIP=<s>.
2. 1b15313 Codegen: $zero reads as constant, `restrict` ctx, inline RAM/SPR fast path with
   out-of-line ps2_mem_read_slow/write_slow, -fno-auto-import; recomp writes only changed files.
3. 3a25f75 GS on its own thread (runtime/gs_thread.c): in-order SPSC queue of register writes and
   IMAGE data; SIGNAL/FINISH/LABEL stay on the EE thread; VBlank pictures composed in-stream from
   a snapshot of the display registers (gs_queue_vsync); EE waits at most one VBlank.
4. 3735796 Deferred band-parallel drawing (gs_draw_pixel.c): spans binned by VRAM page-row
   ownership, pixel workers draw while the GS thread sets up; hazard syncs (texture decode/evict,
   CLUT load, transfers, display); gs-draw unit tests have an async variant (3 workers).
5. 3ca142d Texture cache: a VRAM write drops only the tiles of the page-sized rectangles it backs
   (not the whole texture); per-tile hazard checks; palette change while old batches still sample
   -> new texel buffer, old one freed later ("graves"); pos_lost re-probe fix + tests.
6. 7491a63 Merge of main (eb96e20+): deinterlace runs on the GS thread with the VBlank count from
   the display snapshot (slot 15); GS trace uses gs_stream_vsync; t_trace syncs before reading.
   scripts/compare_frames.py compares two frame dump dirs (no PIL needed).
   main's exe for comparisons: build/mainref (git archive of main + fake .git file), build/mainref/build/rel.

7. (this commit) GS: textures sampled directly from VRAM (gs_tex_raw/gs_tex_texel in gs_draw.h,
   dsample_* in gs_draw_pixel.c), indexed via palette snapshots (gs_clut_lookup, ring of 2048 slots);
   feedback primitives (sample pages they write) sample a page snapshot gs_vram_shadow copied when
   their batch starts; tight texel ranges (no +-1 margin: false feedback / REPEAT wrap); decoded
   cache only for unverified formats. Deferred drawing: RAW (register_prim) / WAR (assign_band -2)
   ordering by batch boundaries, no waits; batch queue of 6 (start_job/retire_job, workers start
   the next batch); kick only before the GS thread sleeps; DB_STATES 256. PS2_GS_STATS=1,
   PS2_PROFILE=workers. GS stream recorder runtime/gs_record.c (PS2_GS_RECORD/_FROM/_FRAMES) +
   scripts/gs_replay.c/.ps1 (renders a recording with any runtime/ version; -Bench).
   Replay of the briefing (build/brief.gsrec, v2500+30): old (7491a63) and new frames identical
   (sync and 8 workers). GS-only: 23.6 -> 8.9 ms/frame (8 workers).

8. 14a7c8f Transfers/CLUT loads wait only if pending drawing touches their pages
   (gs_draw_pages_touched, gs_rect_pages); exact sprite texel ranges (fixed-point values of the
   pipeline; zero-weight bilinear texels excluded: the field-blend copy is no longer "feedback");
   pixel-local in-place filters (texel == own pixel, same base/width/layout) treated like blending;
   sync reasons in PS2_GS_STATS. VU: lazy pipeline retirement (sync_flags/clip/q/p at readers)
   6.2 -> 5.7 ns/pair (unit bench), in-game ~11-16 ns/pair. Crash handler in ps2_runtime.c.
   Recordings: build/brief.gsrec (v2500), build/title.gsrec (v1750, Sly intro); old (7491a63)
   vs new identical on both (build/oldrt = git archive 7491a63 runtime).
   NOTE: the machine is shared with other agents' game runs/builds: timings vary 2x; use
   interleaved A/B min-of-N (gs_replay -Bench) for GS numbers. Unexplained early exits of the game
   twice (no crash event): probably other agents killing sly2.exe; crash handler now in place.

9. b5b40e9 Pacing: VBlanks from a high-resolution waitable timer; idle EE woken via WaitOnAddress;
   PS2_PERF_BURN / PS2_PERF_BURN_GS + scripts/headroom.ps1; GS producer: cached free space,
   256-slot publish chunks, inline register mirror.
10. d05335b recomp/overrides.py: native override table (hash-guarded); D-cache maintenance loops
    0x234EA0, 0x234FD0, 0x23B170 -> no-ops; test in tests/run_tests.py; notes/recompiler-design.md.
11. db35529/9aa0b3f exact CSM1 CLUT page range (+ test of a GS-drawn CLUT). Block-level write
    tracking was tried: Sly 2's ~2 CLUT waits/frame are true block-level dependencies (CLUT in
    page 128 inside the Z buffer area, rewritten by drawing); reverted, page level kept.
12. 8ba2bd6 VBlank picture = job in the deferred-drawing queue (picture_run on a worker); no
    per-VBlank drain on the GS thread. Display pages count as reads for transfers (field mode:
    half height). Transfer waits ~30/s are a CLUT upload into page 192 (true block overlap).

13. 3d31ea4 Spans cut at 64-px page columns, bands per page (row group x column): the 512x224
    field buffers spread over up to 32 bands; critical path 720 -> 450 ms/s at the title.
    39cc883 gs_write_reg fast path (cached free space).
14. 9dc5cd1 VU microprogram recorder PS2_VU_RECORD/_FROM/_COUNT + scripts/vu_replay.ps1 (benchmark
    on real game microcode + exactness check: pair/cycle counts, state hash, XGKICK cycles; any
    runtime/ version via -Runtime). build/brief.vurec (v2500, 6000 programs, 2.45M pairs): 7.8
    ns/pair. VU_REPLAY_PROFILE=1 samples the instruction pointer.
    VU experiments on the replay (A/B, ns/pair, 7.85 baseline): no hazard tracking 6.35 (upper
    bound), no flag pipeline 7.13, no vclean 7.68; a 128-bit VF write-mask ring scoreboard instead
    of per-lane ready times: exact but no faster (7.8); branch instead of cmov in stall checks: no
    change; -march=x86-64-v2 / -O3: no change; -march=x86-64-v3 CHANGES RESULTS (FMA contraction:
    5 programs differ) -> never let the compiler contract VU math. Conclusion: the interpreter is
    at ~7.7 ns/pair (~35 cycles); big gains need the VU recompiler (separate task). Reverted.
15. c37c297 Opt out of Windows power throttling (EcoQoS) in main(): headless/background runs were
    on E-cores/low clocks: VU 16.5 -> 12 ns/pair in game, EE busy 13.5 -> 12 ms/VBlank.
    Thread priority above normal for the EE thread: no measurable change (not kept).
16. Headroom: PS2_PERF_BURN_FROM/_TO (burn only in a VBlank range so all runs reach the scene in
    the same state; a slowed game took START in another state before); headroom.ps1 prints flips/s
    and VU pairs/VBlank (scene fingerprint) per burn and -From/-To.
    Scenes now (main's memcard/movie changes): copyright screen v0-450, in-engine intro cutscene
    v450-1500 (heaviest VU, ~950k pairs/VBlank), "Sly Cooper the thief" attract ~1500-2000, title
    menu ~2000-2100, briefing from ~2300 (START at 2100). No memory-card prompt / FMV appear in
    this boot path any more.

## Measurements (release, memory-card prompt, 28-thread PC)
- Baseline: 30-35 vblanks/s serviced of 60, ~33 flips/s, busy 22-50 ms/vblank (GS 60%, VU 18%).
- After 3 (GS thread, fork-join pixels): ~57/60 vblanks, ~47 flips/s; GS thread saturated.
- After 4: 60/60 vblanks, 57-60 flips/s, EE thread busy ~10 ms/vblank (VU ~48%), GS thread ~15 ms.
- Frames identical to the baseline (frame_000840 etc., except the field-parity bottom row).
- Movie: 60/60, EE 80% is the game's own busy demux loop (f_001B4058 polls f_001B41D0/f_0024F770
  until state changes); not real work. Use a headroom test (burn) to show margin there.

- After the merge (release, 8 workers, 60 s, PS2_INPUT 600:square): movie 60/60 (EE busy loop);
  memcard prompt/title 46-60 vblanks/s: GS thread 15-21 ms/vblank (bottleneck), EE ~10-12 ms
  (VU ~50% of the EE thread). main's release exe: ~29 vblanks/s.
- START at pad frame 1500 is too early at full speed (title appears ~v1600): use 2100.

- After 7 (briefing, START at 2100): GS thread ~13-14 ms/vblank (setup ~10 ms: 12.5k prims,
  1.85M pixels per frame), pixel work ~33 ms CPU/frame (18 ns/pixel!), critical path ~10 ms/frame;
  EE thread busy 15-16 ms/vblank with VU ~60% -> the EE thread is the bottleneck now.

17. Per-scene before/after and headroom measured: notes/performance.md (tables). main's exe:
    22-32 VBlanks/s in 3D scenes (frame-dump timestamps); this branch: 59.9 everywhere, flips ~60.
    Headroom EE 2-6 ms (intro tightest), GS 3-6 ms. The game draws less when frames get tight
    (VU pairs/VBlank drop, flips stay 60): not via the EE timers (timer clock x0.5: no change).
    Default -O1 build also 60/60. Memory-card prompt and movie appear only with an empty card
    (fresh output dir: build/fresh_*).

## Next
1. Final report (StructuredOutput). Optional later: EE-side GIF/VIF packets to the GS thread
   (~13% of the EE thread); VBlank from display vsync; VU recompiler (separate task).

## Findings / traps
- Runtime sources are CRLF in the working tree; PowerShell string Replace with `n fails silently.
- NEVER use relative paths with [IO.File]::ReadAllText/WriteAllText: .NET's current directory is the
  MAIN checkout (C:\Users\tanih\Projects\sly2-decomp), not the worktree. This once modified the main
  checkout's runtime/gs_draw.c (restored with git checkout; main is clean).
- Fork-join of single primitives (parts of ~1k px) gained little: hand-off cache misses ~= the work.
- Deferred drawing must cut spans exactly where immediate drawing (B0) cuts them: S/T/Q are float
  accumulators, a different cut point changes texel coordinates slightly (seen as 122 bytes of
  difference on the prompt before this was fixed).
- GS thread "busy ms/vblank" saturates at ~16.7 when the GS is the bottleneck; look at flips/s.
