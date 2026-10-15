# WIP: gpu-M2 (submission go/no-go)

**Goal**: milestone M2 of notes/d3d11-renderer.md section 11 on branch gpu/M2 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M2): execute hwdry's GPU_FOP stream on the render thread
with real D3D11 objects and a trivial PS, measure render-thread submit ms and GS-thread frontend ms,
decide GO / NO-GO.

**Status: in progress.**

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
- Step 2/3 (commit 080ca43): runtime/gpu/gpu_exec.c (render-thread executor: targets with pair-size
  matching, dynamic VB/IB/record/StateRec/per-draw-constant buffers mapped once per frame, blend/depth
  state caches, redundancy filter, CB offsets per draw, ROV binding with read-only DSV + depth SRV,
  ALIAS16/load dispatches, clears, shadow copies, patches, CLUT ring, zresolve quads),
  shaders/submit.hlsl (+ perms.txt: 24 ps_ff, 3 ps_rov, 4 VS, cs_touch), GPU_OP_FRAME, gpu_front
  arena detach (zero-copy hand-off), gs_replay -Backend hwsubmit (-Pace 59.94 Hz, -Scale, -Profile
  sampling profiler, -StreamHash), scripts/gpu_submit_bench.ps1 (median of N runs).
- Frontend fix: a line whose DDA covers no pixel no longer opens a D3D draw (ep8 boss predicted max
  5909 -> 5079 D3D draws; 82k empty draws in ep8 boss before).
- First quiet-machine results (S=1, paced, 5 runs, before the frontend thread): render thread
  steady-state worst frame ep8 boss 5.35 ms, ep3 hub 6.37, ep5 5.46, ep2 hub 5.24, ThiefNet 6.94
  (worst incl. replay-start frame 8.6-11.2 ms); render CPU worst 1.4-2.2 ms; driver threads 1.8-3.5
  ms/game frame; 0.31-0.43 us/draw wall. GS side (decode + frontend, one thread): 6.9-9.9 ms/game
  frame (gpu_front_bench -Runs 5, quiet) -> NO-GO without restructuring.
- Frontend thread (runtime/gpu/gpu_fthread.c, gs_replay -FrontThread): GS thread decodes, proxy hooks
  ship prims (compact 32-B vertices + 4-slot cache by decoder vertex serial), state deltas, transfers
  with block contents, CLUT changes, VBlanks through an 8 MB SPSC ring; stream-hash identical to
  same-thread on ThiefNet, FMV, ep8 boss. gs_vtx got a `serial` field (gs_draw.c vertex()).

## In progress
- Commit fb62a15 (paused mid-edit): send_state now diffs only a word list of the gs_prim fields and
  registers the frontend reads (build_wlist; PS2_GPU_FTHREAD_FULL=1 = all words), vertex cache by
  serial only. Needs: build, stream-hash equivalence threaded vs same-thread (thiefnet, ep8 boss, fmv,
  then all 24), timing (decode-only vs threaded GS vs frontend thread).

## Next
1. Verify fb62a15 (above). If the proxy is still too expensive, cut further (fewer state words).
2. gpu_front_bench / gpu_submit_bench with -FrontThread (5 runs, quiet); per-thread numbers.
3. Patch upload coalescing for the replay-start frame (948 UpdateSubresource calls) if cheap.
4. GO / NO-GO; results here and design appendix D.
5. Verification: run_tests.py, unit suites (window -UnitOnly), gpu_diff_all gate, VRAM hashes (gs_vtx
   serial touches gs_draw.c), game debug + --release build, sw default.

## Findings / traps
- Wall time of the render thread tracks GPU time when frames are submitted back to back (GPU-bound:
  ~0.5-1 us per draw of GPU front-end time with trivial shaders); -Pace gives game-like timing.
- NVIDIA driver worker threads do ~2x the render thread's CPU (other threads 1.8-3.5 ms/game frame).
- PowerShell: "...$nl" + letters inside double quotes is a variable ($nlstatic): concatenate instead.
  [IO.File] relative paths resolve against the main checkout: always absolute. Commands containing
  `Remove-Item Env:...` are blocked by the sandbox: assign $env:X = $null.
- The frontend's emit_vertex compared 48 of gs_vtx's 52 bytes (missed `v`); now memcmp up to serial.
- Producer/consumer rings: per-record shared-line traffic (rd stored per record) and waking per record
  cost more than the work; hysteresis + rd every 64 KB.
- Per-thread CPU (QueryThreadCycleTime) in the proxy experiments looked inflated; PS2_GPU_FTHREAD_DROP=1
  measures the proxy alone (records dropped), PS2_GPU_FTHREAD_STATS=1 prints record volumes.
