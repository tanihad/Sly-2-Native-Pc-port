# The Direct3D 11 GPU renderer (as built, M0-M11)

The game's default GS renderer since M11. Design and the history of every milestone: notes/d3d11-renderer.md (sections
1-13, appendices B-M). This page is the map for someone changing it.

## What it does

The PS2's GS is not emulated pixel by pixel: every GS primitive the recompiled game draws becomes Direct3D 11 draws at an
integer internal scale N (default `internal_scale = auto` = 3x, lowered on a slow GPU), with sample 0 of every pixel
exactly the PS2's (exact integer depth, the GS blend / test / texture arithmetic in integers). Effects the hardware output
merger cannot express (COLCLAMP off, FBMSK bit masks, DATE, Cd-dependent blends with C > 128, PABE / DTHE, high Z32 ...)
run on the **ROV tier**: the same pixel pipeline as a pixel shader on rasterizer-ordered views (or, on GPUs without ROVs,
plain UAV draws of disjoint primitives). 16-bit frames over 32-bit targets (the game's CT16 channel shuffles) are
**ALIAS16** runs on a staging texture. The PCRTC (both circuits, flicker filter, field display) is composed on the GPU.
The software renderer (runtime/gs_draw*.c) stays as the fallback and as the oracle of every test.

## Threads

| thread | does | code |
|---|---|---|
| EE | the game; GS register writes / image transfers into the GS queue | gs_thread.c (unchanged) |
| GS | decodes the stream (`env_update`, `draw()`; one decoder for both renderers), ships primitives to the frontend | gs_draw.c, gpu_fthread.c (proxy) |
| frontend | tier classification, StateRec, records, ownership of GS memory pages / byte lanes, targets, CLUT ring, uploads, downloads -> command chunks | gpu_front.c, gpu_own.c, gpu_clut.c |
| render | the only D3D11 user: device, executes frames (FF / ROV / ALIAS16 draws, memory ops, Z sync), compose, present, timestamps | gpu_d3d11.c, gpu_exec.c, gpu_present.c |
| window | messages, input, fullscreen; no device of its own with the GPU renderer (window_gpu.h) | window.c |

The EE never waits for Present: a VBlank's picture counts as done when its frame is queued (2 frames in flight normally,
6 for 2 s after a slowdown, + the frames the paced present holds back). In-game integration (start, switch, device loss,
verifier, stats, internal scale governor): gpu_hw.c. Game speed never depends on the GPU (pc-pacing, appendix N of
d3d11-renderer.md): a scale the GPU cannot draw in real time is lowered by the governor (gpu_pace.c) before the render
queue can hold the game back for more than about a second; presentation (paced / immediate, vsync, VRR detection) is
gpu_present.c + gpu_pace.c.

## Files (runtime/gpu)

`gpu.h` (render-thread interface), `gpu_cmd.c` (command chunks / arenas), `gpu_d3d11.c` (device, caps, start-up checks,
shader warm-up, render loop), `gpu_exec.c` (frame executor), `gpu_present.c` (compose, resample to the window, swap
chain, timestamps, read-backs), `gpu_front.c/_int.h/.h` (frontend), `gpu_own.c` (targets, ownership, rescale), `gpu_clut.c`,
`gpu_fthread.c` (frontend thread), `gpu_hw.c/.h` (the game's integration), `gpu_verify.c` (memory comparison under the
10.2 tolerances), `gpu_conf.c` (conformance kernels for tests), `shaders/*.hlsl` + `perms.txt` (compiled at build time by
scripts/hlslc.c into build/gen/gpu_shaders.h; no shader compiler at run time).

## Start-up and fallback (M11)

`gpu_hw_start` -> `gpu_start` with `warmup`: device (feature level 11_1, else 11_0), required capabilities (typed UAV
loads of R8G8B8A8_UINT, double-precision shaders, dual-source blending; ROVs optional), swap chain, then every shader
the game uses is created (47 objects, a few ms: drivers compile in the background and keep their own disk cache). Any
failure -> one log line `[ps2] GPU renderer unavailable (<reason>): using the software renderer` and the software
renderer with the window's own presenter. A lost device is recreated twice (GS memory reloaded from the CPU side);
the third loss switches to software. Test switches: `PS2_GPU_FAIL=device|caps|swapchain|shader`, `PS2_GPU_FL=11_0`,
`PS2_GPU_NO_ROV=1`, `PS2_GPU_LOSE_AT=<vblank>,...`, `PS2_GPU_ADAPTER=warp` (the CPU rasterizer: ~12 pictures/s at 1x).

## Settings (notes/settings.md) and keys (notes/controls.md)

`[graphics] renderer = gpu|software`, `internal_scale = auto|1..8` (F2 / Shift+F2 / Ctrl+F2), `outlines` (F3),
`filters` preset (F4) + `motion_blur` (F5) / `flicker_filter` (F6) / `interlacing` (Shift+F6), `scaler`, `pacing`;
`[display] aspect` (F7: anamorphic display targets at 16:9, 4 x 6 samples per pixel at 3x) and `hud` (F8). Everything
applies from the next frame (targets are rescaled, nothing restarts).

**Internal scale governor** (pc-pacing; gpu_pace.c `pace_gov`, fed by gpu_hw.c on the GS thread): load = GPU ms per VBlank
picture (drawing + compose + present, timestamps) / 16.68 ms. auto (`internal_scale = auto`, up to `internal_scale_max`,
default 3): down when > 90% for 3 s (to the scale predicted at <= 80%), up when the next scale is predicted at <= 75% from
the lower quartile of 15 s (NVIDIA lowers clocks at light load), back-off 120 s doubling. A fixed scale (and `native`) is the
player's maximum: lowered only when it does not fit (> 100% for 2 s; or the overload rule: the last 12 pictures over 2.5
frame periods or 24 over 1.4 -> to 85% at once), raised back when it fits. Evidence beats timestamps: a second with >= 10%
skipped presents or >= 50 ms of the game waiting for the render queue counts as 105% (another program on the GPU). No
judging for 60 pictures after a light second (GPU clocks rising). Even 1x too slow for 10 s: the software renderer takes
over. `PS2_GS_SCALE` / `PS2_GS_GOVERNOR=0`: a scale exactly (tests); `PS2_GS_AUTO_SLOW=<factor>[:<s>]`, `PS2_GS_AUTO_LOG=1`.

## Diagnostics

`PS2_GS_HW_STATS=1|2` (per second: flips, per-thread ms, GPU ms, draws, PUNT, downloads, memory every 10 s),
`PS2_GPU_PROF=1|2` (GPU time per phase), `PS2_GS_VERIFY=N` (+ `PS2_GS_VERIFY_DUMP`: GPU memory vs the software renderer
every N VBlanks), `PS2_GS_DEBUG_KEYS=1` (F9 GPU <-> software, Shift+F9 device loss), `PS2_GS_MEM_MAP=1`,
`PS2_GPU_FRONT_TRACE=<vblank>`, `PS2_GPU_PROBE`.

## Tests and gates (run all after a renderer change)

| gate | what |
|---|---|
| tests/unit/gs-hw/run.ps1 | conformance kernels, start-up / fallback checks, gs-draw scenes REF / mixed (also without ROVs) on hardware and WARP |
| tests/unit/gs-front, gs-draw, window | frontend units, software renderer, settings / keys |
| scripts/gpu_diff_all.ps1 -Mode mixed / ref -Scale 1 / 3 | corpus pictures vs the software renderer (10.2 gates), PUNT 0 |
| scripts/gpu_game_all.ps1 [-Scale 3] | the game's path on the corpus: pictures = reference compose (S=1), box gate (S=3, rv_prompt waived) |
| scripts/gpu_lockstep_all.ps1 [-Mixed] [-Scale 3] | GS memory vs sw at every FRAME / ZBUF change and VBlank |
| scripts/gpu_sample_all.ps1 -Scale 3 | FF vs REF on every sample of every target |
| scripts/gpu_fthread_check.ps1 [-Mixed] | frontend thread == same-thread frontend (stream hashes) |
| scripts/gs_vram_hashes.ps1 -Backend hwdry | the frontend leaves the software renderer bit-identical |
| scripts/m9_exact.ps1 | every sample of every target bit-identical to a baseline build (optimisations; PS2_GPU_NO_ROV=1: the no-ROV path) |
| tests/unit/gs-front | frontend counters, incl. xcopy staleness (test 10) |
| scripts/m8_batch.ps1 (54 scenes) | in-game sweep with the verifier; scripts/m11_long.ps1: 22 min through all episodes (memory) |

Recordings: tests/gpu/corpus.txt (28, made from the owner's disc; never committed).

## Known limits (owner decisions pending, appendix M)

- ep5 tank battle gas cloud: +-3-5 colour drift under ~7k layered dual-source blends per frame (verifier budget
  exceeded once in M8); the exact alternative costs 4-5x GPU time. Left as is.
- Distant hub scenery drawn from a reduced copy is sharp at 3x (the PS2 shows it blocky); a per-effect native read would
  be a Sly-specific rule. Left as is.
- GPUs without rasterizer-ordered views: exact pictures, up to ~1.7x the GPU time (ep7 boss at 3x 16:9).
