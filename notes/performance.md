# Performance: how the game is kept at full speed, and how to measure it

Target: the release build runs at full NTSC speed (59.94 VBlanks per second, the game flipping
~60 frames per second) in every scene, never faster than real time, with measured headroom.
Measured on the development PC: Intel i7-14700F (8 P-cores + 12 E-cores, 28 threads), Windows 11.
The machine is shared with other agents' builds and game runs, so single measurements vary by up
to 2x: compare interleaved A/B runs, take the minimum of several, and prefer the offline replays
(GS and VU) for code changes.

## Threads

| thread | work | per VBlank (briefing) |
|---|---|---|
| EE (main) | recompiled game code, VIF/VU microprograms (interpreter), GIF parsing, IOP services | busy ~12 ms, of which VU ~60% |
| GS | GS register stream: primitive setup, texture/CLUT handling, binning spans to bands | ~11 ms |
| pixel workers (8) | rasterising/texturing/blending the binned spans in parallel (bands = 64-px page columns x page rows); compose + present the VBlank picture | critical path ~7.5 ms at the title (PS2_GS_STATS) |
| host timer | 59.94 Hz VBlank and tick interrupts (high-resolution waitable timer) | - |
| window | Direct3D 11 presentation of the newest picture (vsync'd flip model), input | - |

The EE never waits for the GS except where it observes it (local->host transfers, CSR reset,
FINISH/SIGNAL), and at most one VBlank: VBlank pictures are composed in the GS stream from a
snapshot of the display registers. The GS thread never waits for the pixel workers except on true
hazards (a transfer or CLUT load into pages that pending drawing reads or writes, texture-cache
work); see `runtime/gs_draw_pixel.c` (deferred drawing) and `runtime/gs_thread.c`.

## Pacing

- VBlanks come from a host timer thread (high-resolution waitable timer; `runtime/ps2_runtime.c`
  `host_timer`). The game can never run faster than real time.
- If the emulation is late, missed VBlanks are dropped, not replayed in a burst (no catch-up
  storms); `vblanks N/M due` in PS2_PERF shows when that happens.
- An idle EE thread sleeps in WaitOnAddress until the next interrupt (no spinning).
- Presentation: the window thread shows the newest composed picture at every display refresh
  (triple buffer, Present with sync interval 1). On a 60 Hz display one picture repeats about every
  17 s (59.94 vs 60 Hz). Driving the VBlank from the display's vsync when it runs at ~60 Hz would
  remove that; not done (see Next).
- The process opts out of Windows power throttling (EcoQoS) at startup: a game window that is not
  in front (or a headless run) otherwise lands on E-cores at low clocks (VU 16.5 -> 12 ns/pair,
  EE busy 13.5 -> 12 ms per VBlank). `PS2_HIGH_QOS=0` keeps Windows' default.

## How to measure

Build: `python scripts\build_game.py --release --out build\rel`, run
`scripts\run_game.ps1 -Seconds N -Out build\rel` with the variables below set in the same command.
Scripted input for the standard path: `PS2_INPUT="600:square:12,2100:start:10"` (pad frames =
VBlanks; START at 2100 reaches the title menu at full speed; 1500 is too early).

Scenes on that path (VBlank numbers): copyright screen 0-450, in-engine intro cutscene 450-1500
(the heaviest VU load, ~950k pairs per VBlank), "Sly Cooper the thief" attract 1500-2000, title
menu 2000-2100, Binocucom briefing from ~2300. With an empty memory card (fresh output directory)
the "create a save file?" prompt appears first (SQUARE answers it).

| variable / tool | what it gives |
|---|---|
| `PS2_PERF=1` | once a second: VBlanks serviced/due, game flips, EE thread busy ms per VBlank and its split (EE code, GS = GIF/queueing, VU, VIF, IPU, IOP, present, gswait, idle), GS thread ms per VBlank, VU pairs/programs per VBlank and ns per pair |
| `PS2_GS_STATS=1` | every 5 s: deferred-drawing batches and why they were submitted, GS-thread syncs by reason (display, transfer, CLUT, EE, texture cache, palette), pixel work and the critical path |
| `PS2_PROFILE=1` / `gs` / `workers` | sampling profiler of the EE thread / GS thread / pixel workers, printed at exit (`PS2_PROFILE_SKIP=<s>` skips the start) |
| `scripts\headroom.ps1` | headroom test: `PS2_PERF_BURN=<ms>` burns per VBlank on the EE thread, `-Gs` (`PS2_PERF_BURN_GS`) per picture on the GS thread; `-From/-To` (`PS2_PERF_BURN_FROM/_TO`) burn only in a VBlank range so every run reaches the scene in the same state. Prints VBlanks %, flips/s and VU pairs per VBlank per 5-s window |
| `PS2_GS_RECORD=<file>` (`_FROM=<vblank>`, `_FRAMES=<n>`) + `scripts\gs_replay.ps1 -Rec <file> [-Runtime <dir>] [-Workers n] [-Bench]` | records the GS input stream; replays it with any version of the GS code (old runtime via `git archive`): frames for `scripts\compare_frames.py`, or a GS-only benchmark |
| `PS2_VU_RECORD=<file>` (`_FROM=<vblank>`, `_COUNT=<programs>`) + `scripts\vu_replay.ps1 -Rec <file> [-Runtime <dir>] [-Passes n]` | records microprograms with their inputs and results; replays them with any interpreter version: ns per pair on real game microcode, and an exactness check (pair/cycle counts, final state hash, XGKICK cycles). `VU_REPLAY_PROFILE=1` samples the instruction pointer |
| `PS2_GS_ASYNC=0`, `PS2_GS_THREADS=<n>` | GS on the EE thread (as in the unit tests) / number of pixel workers (default min(8, cores/2 - 1)) |

Notes: the game adapts its own work to the time it has (see the headroom table), so judge
headroom by flips/s and VBlanks serviced, not by busy time alone. Never let the compiler contract
VU arithmetic into FMA (`-march=x86-64-v3` changes results; the VU replay catches it).

## Before / after (release build)

Measured 2026-10-10 on the PC above (shared with other agents' runs; ~70% load at times).
"Before" is main's release exe (eb96e20+, the state this branch started from after merging main),
measured from the write times of frame dumps every 60 VBlanks (main has no PS2_PERF); the game
cannot run faster than its VBlanks, so VBlanks/s is its speed. "After" is this branch's release
build with PS2_PERF. Movie and memory-card prompt need an empty memory card (a fresh output
directory); the other scenes are on the standard input path.

| scene (VBlanks) | before: VBlanks/s | after: VBlanks/s | game flips/s | EE thread busy ms/VBlank | GS thread ms/VBlank |
|---|---|---|---|---|---|
| intro movie, IPU (60-360, empty card) | 58.0 | 59.9 | 60 | 16.7 (the game's own busy demux loop) | 0.6 |
| memory-card prompt (480-1440, empty card) | 31.7 | 59.9 | 61-62 | 9.6-11.6 | 8.3-9.8 |
| in-engine intro cutscene (600-1440) | 24.2-25.1 | 59.9 | 58-60 | 11.5-12.5 | 9-11.5 |
| "Sly Cooper the thief" attract (1560-1980) | 25.9-27.6 | 59.9 | 61 | 12.1-13.4 | 11.3-13.2 |
| title menu (2000-2760, no START) | 23.0 | 59.9 | 60-61 | 12.0-14.4 | 12-13.4 |
| Binocucom briefing (2500+, START at 2100) | 22.4 | 59.9 | 60 | 12.0-13.9 | 10.7-12.5 |

The non-release build (`build_game.py` default: -O1 with function tracing) also holds 60/60 on
this path (EE busy 11-14.6 ms per VBlank).

Headroom (scripts\headroom.ps1 with -From/-To on the scene; the largest burn per VBlank that leaves
VBlanks and flips/s unchanged; in brackets what happens at the next level):

| scene | EE thread | GS thread |
|---|---|---|
| memory-card prompt | 3 ms (6: the game draws less, flips ~59) | 6 ms (9: flips 59-60, less drawn) |
| intro cutscene (heaviest) | 2 ms (4: 56.6 flips/s) | 2-4 ms (6: 56.4) |
| title menu | 3 ms (6: 59.6, less drawn already at 3) | 3 ms (6: 58.8) |
| briefing | 6 ms (flips stay 60; less drawn from 2) | 4 ms (6: 59.0-59.6) |

The game draws less when its frame gets tight (VU pairs per VBlank drop while flips stay at 60
and the pictures look the same). It does not do this through the EE timers (scaling the timer
clock by 0.5 changed nothing); the trigger is not known. Without any burn, VU pairs per VBlank in
the same scene vary by +-10% between runs for the same reason.

Offline (same input, interleaved A/B, min of N):
- GS only (`gs_replay -Bench`, 8 workers): briefing recording 23.6 -> 8.9 ms per frame after
  direct texture sampling, title 8.8 -> 7.0 ms after page-column bands. Pictures are identical to
  the old renderer except 2-14 pixels per frame that differ by < 8 (perspective S/T/Q stepping at
  the new span cuts).
- VU (`vu_replay`, briefing, 6000 programs / 2.45M pairs): 7.8 ns per pair; in game ~11 ns per
  pair (cache pressure from the rest of the EE thread).

## What made the difference (commits on fix/fix-perf)

| change | effect |
|---|---|
| GS on its own thread, in-order register queue; pictures composed in-stream | EE no longer renders: memcard prompt ~33 -> ~57 VBlanks/s |
| deferred band-parallel drawing (pixel workers), RAW/WAR ordering by batches, queue of 6 batches | GS thread no longer rasterises; 60/60 |
| textures sampled directly from swizzled VRAM (palette snapshots for indexed formats), exact texel ranges, page-snapshot feedback | GS-only replay of the briefing 23.6 -> 8.9 ms per frame |
| transfers/CLUT loads wait only for drawing that touches their pages; picture composition as a worker job | fewer GS-thread stalls |
| spans cut at 64-px page columns, bands per page | title critical path 720 -> 450 ms/s |
| codegen: constant $zero, restrict ctx, inline RAM fast path | EE code cheaper |
| native override table (`recomp/overrides.py`): D-cache maintenance loops -> no-ops | removes useless cache-op loops |
| VU: lazy pipeline retirement | 6.2 -> 5.7 ns/pair (unit bench) |
| EcoQoS opt-out | VU 16.5 -> 12 ns/pair in headless runs |
| pacing: high-resolution waitable timer, drop missed VBlanks, WaitOnAddress idle | steady 59.94, no bursts |

## Next

- VU microcode recompiler (planned separate task): the interpreter is at ~7.7 ns per pair on real
  game microcode (~35 cycles); hazard tracking is ~20% of it and the flag pipeline ~10%, so only a
  recompiler gives a large gain. The VU replay is its benchmark and exactness check.
- EE-side GIF/VIF work (~13% of the EE thread): pass whole PATH1/PATH2 packets to the GS thread.
- VBlank driven by the display's vsync when it runs at ~60 Hz (smoother presentation).
- Direct3D 11 renderer (per the native-port plan) replaces the software pixel pipeline.
