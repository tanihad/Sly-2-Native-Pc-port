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

## Measurements (release, memory-card prompt, 28-thread PC)
- Baseline: 30-35 vblanks/s serviced of 60, ~33 flips/s, busy 22-50 ms/vblank (GS 60%, VU 18%).
- After 3 (GS thread, fork-join pixels): ~57/60 vblanks, ~47 flips/s; GS thread saturated.
- After 4: 60/60 vblanks, 57-60 flips/s, EE thread busy ~10 ms/vblank (VU ~48%), GS thread ~15 ms.
- Frames identical to the baseline (frame_000840 etc., except the field-parity bottom row).
- Movie: 60/60, EE 80% is the game's own busy demux loop (f_001B4058 polls f_001B41D0/f_0024F770
  until state changes); not real work. Use a headroom test (burn) to show margin there.

## Next
1. Profile EE thread (VU interpreter ~8-9 ms/frame) and GS thread (setup) again; optimise:
   VU interpreter dispatch, GS setup (draw/tex_prepare/gs_tex_ensure), gs_display_regs.
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
