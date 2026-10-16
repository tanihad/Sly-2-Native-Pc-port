# WIP: gpu-M6 (in-game integration, opt-in)

**Goal**: milestone M6 of notes/d3d11-renderer.md section 11 on branch gpu/M6 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M6): the hw backend in the game behind PS2_GS_BACKEND=hw /
sly2.ini `[graphics] renderer = gpu` (default software), GPU compose + present on the render thread's
swap chain, pacing, device loss, verify, stats, live switch, frame dumps, packaged build.

**Status**: complete. As built, numbers, deviations and M7 notes: notes/d3d11-renderer.md appendix H.

## Plan (design decisions)
- New `runtime/gpu/gpu_hw.c` (+ `gpu_hw.h`): the in-game integration used by ps2_runtime.c and by
  gs_replay (`--game`): starts the render thread (swap chain on the window, or headless offscreen), the
  frontend (mixed = 1, thread = 1, display_ky2 = 0, S = 1), the sink; the GS thread's VBlank hook
  (`gpu_hw_vblank`: display snapshot -> frontend VBlank with display info); dumps via asynchronous picture
  read-back on a dump thread; PS2_GS_HW_STATS; PS2_GS_VERIFY; switch; device loss (restart of render thread +
  frontend; 3 losses -> sw).
- GPU compose (`GPU_FOP_PRESENT` extended): per circuit the colour target when the displayed rect is current
  there, else a scratch copy of vram_buf with the rect's GPU words downloaded (no ownership change).
  `cs_compose` reproduces gs_compose_from exactly, `cs_deint` the field deinterlacer.
- window.c: external presenter mode (window_gpu.h). gs_thread.c: VBlank hook, picture presented, read hook.
- gs_draw.c: HUD-transformed vertex copies get serial 0.
- PUNT with the frontend thread: counted, skipped, frontend to the same thread at the next VBlank, back after
  600 VBlanks without PUNT.

## Done
- 25e5b11 core; scratch-copy display path; corpus --game 24/24 (3816 pictures = reference compose),
  --game-verify 24/24 clean.
- Window tests (--external test, settings tests); switch / device loss / PUNT fallback on title replays.
- Pacing: picture reported at queue time (GPU_FD_SIGNAL); windowed hw 0-1 missed VBlanks in 38 s.
- notes/settings.md, notes/install.md.
- Weak gpu_hw references in ps2_runtime.c (run_tests 16/16); per-game-frame stats; verify dump pairs.
- HUD hint plumbing; 5-min PS2_GS_VERIFY=60 in ep3/ep5/ep8 hubs: 300 comparisons each, 0 beyond, memory
  flat, 0 underruns.
- Verifier tolerance class for downloaded GPU data (gpu_front_block_origin): fix-perf path PS2_GS_VERIFY=20
  480 comparisons, 0 beyond; pairs 151, 0 beyond the picture gate, min PSNR 51.37.
- Gates: gpu_diff_all 13/13, VRAM hashes unchanged, fthread check (+mixed), lockstep mixed/exact, game_all,
  game_all -Verify 1, unit suites, debug + release builds, package.py (hw GUI build runs, 60 flips/s).
- Windowed hub stats (appendix H): GS 2.9-4.5 ms, frontend median 3.5-5.5 / max 8.1, render CPU <= 1.9,
  60 flips/s, 0 dropped.
- Design: section 7 / 8.2 notes, appendix H.

## Findings / traps
- (M5) PowerShell ReadAllText + .Replace on CRLF files silently does nothing: use the Edit tool.
- PowerShell variables are case-insensitive: a script param $Exe and a local $exe are the same variable.
  `$x = if (...) { @('a') } else { ... }` unwraps one-element arrays: splatting @x then passes nothing useful.
- `*>` redirection of a script whose native child writes to stderr can report exit 1 although the child
  returned 0 (run_tests): check the output or run without `*>`.
- Do not edit sources while a gate script is building (broken builds / EPERM on mapped sections).
- In-game steady-state downloads exist (not in the corpus): the intro cutscene (~VBlank 1520-1880) and hubs
  have a colour target writing main-Z pages that the Z target then tests: download+load ops per frame. Exact
  (verify clean), counted; an M8/M9 item (xcopy across kinds is not possible: data reinterpretation). A 16:9
  title recording has 0 downloads.
- NVIDIA power management: with ~3 ms of GPU work per 16.7 ms the RTX 4060 Ti drops from P0 to P3/P5 about
  2 s after a load spike; frames then take 15-40 ms until the driver raises the clocks again. Anything that
  makes the EE wait for the render thread (picture done after Present) turns that into missed VBlanks;
  dropping presents made it worse. Fix: report the picture when its frame is queued.
- GS-thread CPU per VBlank from QueryThreadCycleTime includes the spin-waits; PS2_PERF's "GS thread
  ms/vblank" (busy time in the slot loop) is the comparable number.
- Separate hw and sw runs of a hub diverge frame by frame (game timing), so hw vs sw equivalence is measured
  inside one run (PS2_GS_VERIFY + PS2_GS_VERIFY_DUMP).
- Downloaded GPU data of a released target keeps its tolerance class (dual-source +-1 words) even after a
  partial upload makes the block "CPU" in the ownership map; the verifier must not hold it to exactness.
