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

## Next
1. VU interpreter (EE thread, ~9.5 ms/frame in the briefing): profile, pre-decoded dispatch.
   Then GS setup (draw/triangle setup, deferred_span) and the pixel pipeline (18 ns/pixel).
2. Cache-maintenance override table in the recompiler (0x234EA0/0x234FD0 loops) + doc.
3. Frame pacing: vblank skip policy when behind; presentation smoothness; PS2_PERF_BURN headroom test.
4. Debug build numbers; final report; notes/progress.md + notes/performance.md docs.

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
