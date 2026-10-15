# WIP: gpu-M2 (submission go/no-go)

**Goal**: milestone M2 of notes/d3d11-renderer.md section 11 on branch gpu/M2 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M2): execute hwdry's GPU_FOP stream on the render thread
with real D3D11 objects and a trivial PS, measure render-thread submit ms and GS-thread frontend ms,
decide GO / NO-GO.

**Status: complete. Verdict GO** (design appendix D).

## Done
- Step 0 (baseline refresh, commit b5a6253): next/gpu's merge of main brought the PCRTC field-mode
  fix in runtime/gs.c gs_compose ((DH+1)/2 lines per field). Evidence:
  - gpu/M1's runtime exported (git archive 7a2271c runtime -> build\m2\m1\runtime): its sw replay
    hashes equal the old baseline on all 13 (sync for title/rv_prompt/ep8_boss, 4 workers others).
  - old vs new sw pictures (scripts\compare_frames.py) on all 13 recordings: 902 frames differ, every
    difference is on output line 446 only (box y 446..446), no other pixel of any picture changed.
  - VRAM (all 4 MB after every VBlank, scripts\gs_vram_hashes.ps1) on all 24 recordings: identical to
    tests/gpu/vram_hashes.txt (the fix is display-only; the pixel-worker start-race fix, the IOP thread
    and the GS wait hook change no drawing).
  - new sw hashes identical synchronously and threaded (4 workers, -Backend both); hw-vs-sw 0 px.
  - tests/gpu/baseline.json regenerated (gpu_diff_all -Update): 13/13 pass, 0 differing pixels.
- Render-thread executor (commit 080ca43 and later): runtime/gpu/gpu_exec.c, shaders/submit.hlsl
  (+ perms.txt: 24 ps_ff, 3 ps_rov, 4 VS, ps_depth, ps_flat, cs_touch), GPU_OP_FRAME, zero-copy arena
  hand-off (gpu_front_detach/release), vram_buf patches through a CPU mirror (one update for big ones).
- gs_replay -Backend hwsubmit (-Pace 59.94 Hz, -Scale, -FrontThread, -Pin, -Profile sampling profiler,
  -StreamHash), scripts/gpu_submit_bench.ps1 (median of N runs, machine load per run),
  scripts/gpu_fthread_check.ps1 (threaded vs same-thread stream identity on all 24).
- Frontend fixes: a line with no pixel inside the scissor opens no D3D draw (ep8 boss predicted max
  5909 -> 5079); emit_vertex compared 48 of 52 gs_vtx bytes (now all values).
- Frontend thread (runtime/gpu/gpu_fthread.c): GS thread decodes, proxy hooks ship primitives (8 B +
  new vertices, 32 B each, 4-slot cache by the decoder's vertex serial, gs_vtx.serial), state as the
  changed words of a fixed list of gs_prim fields/registers (none when nothing the frontend reads
  changed: no new version, saves its state lookup), transfers with their blocks' contents, CLUT
  changes as changed 16-byte pieces, VBlanks; 8 MB SPSC ring, records staged in L1 and streamed with
  non-temporal stores, rd/pub touched every 64 KB, producer sleeps on its own word with hysteresis.
  Stream hash identical to same-thread on all 24 recordings (gpu_fthread_check.ps1).
- Verification so far (commit 0dba62e and later): every tests\unit\*\run.ps1 exit 0 (window -UnitOnly
  904 checks; gs-front 24 after adding gpu_fthread.c to its build), tests\run_tests.py 16/16,
  build_game.py debug + --release OK (PS2_GS_BACKEND still sw only), VRAM hashes 24/24 identical
  (gs_vtx serial in gs_draw.c changes nothing), gpu_diff_all 13/13 0 px on the refreshed baseline,
  gpu_hwdry_all -SelfCheck 24/24 (0 PUNT, VRAM unchanged).

## Measurements (quiet windows; the machine is shared: HUD agent runs 2 game instances often)
- Render thread (S=1, paced, 5 runs, before the frontend thread): steady-state worst frame ep8 boss
  5.35 ms, ep3 hub 6.37, ep5 5.46, ep2 hub 5.24, ThiefNet 6.94 (worst incl. replay-start frame
  8.6-11.2); render CPU worst 1.4-2.2 ms; driver threads 1.8-3.5 ms/game frame; 0.31-0.43 us/draw.
- GS side, per game frame (modes.ps1, 3 alternating runs, load 4%): ep8 boss decode alone ~2.6 ms;
  same-thread decode+frontend median 7.13 / worst steady 7.65; frontend thread: GS thread 5.07 / 5.41,
  frontend thread 5.03 / 5.38. ThiefNet: same-thread 9.92 / 10.34; threaded GS 6.78 / 7.09, frontend
  7.71 / 8.03.

## Final results (quiet windows, 5 runs; design appendix D has the tables)
- Render thread, paced, frontend thread on: worst steady frame ep8 boss 4.93 ms (gate 8), ep3 hub 5.27,
  ep5 street 5.07, ep2 hub 4.90, ThiefNet 6.73; incl. replay-start frame 7.9-10.8; median 1.1-1.9;
  render CPU worst 1.2-1.8; 0.25-0.31 us per draw (wall); GPU median 1.6-3.3 ms; driver threads 1.4-3.2.
- GS side per game frame (median / worst steady), frontend thread: ep8 boss GS 5.39 / 5.76, frontend
  5.26 / 5.61 (gate 6); ep3 5.80 / 5.68; ep5 5.37 / 5.52; ep2 hub 5.86 / 6.23; ThiefNet 6.70 / 7.10.
  Same thread: ep8 7.16, ep3 8.25, ep5 7.74, ep2 8.73, ThiefNet 9.76 (NO-GO without the thread).
- GO for M3; ThiefNet / ep2 hub frontend over 6 ms -> M9 frontend hot spots (appendix D list).

## Next (later milestones)
- M3: see design appendix D, "What M3 must know".
- M9: frontend hot spots; GPU front-end cost of 5-6.5k draws.

## Findings / traps
- Wall time of the render thread tracks GPU time when frames are submitted back to back (GPU-bound:
  ~0.5-1 us per draw of GPU front-end time with trivial shaders); -Pace gives game-like timing.
- NVIDIA driver worker threads do ~2x the render thread's CPU (other threads 1.8-3.5 ms/game frame).
- Shared machine: per-thread CPU doubles while another agent runs games (E-cores / SMT siblings);
  only same-session A/B comparisons are meaningful; pinning to P-cores (-Pin) made things worse when
  the siblings were busy, so the bench does not pin.
- PowerShell: "...$nl" + letters inside double quotes is a variable ($nlstatic): concatenate instead.
  [IO.File] relative paths resolve against the main checkout: always absolute. Commands containing
  `Remove-Item Env:...` or `rmdir /s` are blocked by the sandbox: $env:X = $null, new directories.
  A script parameter $A collides with a local $a (case-insensitive).
- Ring between threads: WaitOnAddress on a word the other side keeps changing returns at once (a busy
  loop: the producer burned as much CPU as the consumer); per-record shared-line traffic costs more
  than the work; byte volume matters (state: only changed words of used fields; CLUT: changed pieces).
- PS2_GPU_FTHREAD_DROP=1 measures the proxy alone (records dropped), PS2_GPU_FTHREAD_STATS=1 prints
  record volumes, PS2_GPU_FTHREAD_FULL=1 sends every state word.
