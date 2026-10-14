# WIP: gpu-M2 (submission go/no-go)

**Goal**: milestone M2 of notes/d3d11-renderer.md section 11 on branch gpu/M2 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M2): execute hwdry's GPU_FOP stream on the render thread
with real D3D11 objects and a trivial PS, measure render-thread submit ms and GS-thread frontend ms,
decide GO / NO-GO.

**Status: in progress.**

## Done
- Step 0 (baseline refresh, commit "gpu-M2: baseline refresh"): next/gpu's merge of main brought the
  PCRTC field-mode fix in runtime/gs.c gs_compose ((DH+1)/2 lines per field). Evidence:
  - gpu/M1's runtime exported (git archive 7a2271c runtime -> build\m2\m1\runtime): its sw replay
    hashes equal the old baseline on all 13 (sync for title/rv_prompt/ep8_boss, 4 workers others).
  - old vs new sw pictures (scripts\compare_frames.py) on all 13 recordings: 902 frames differ, every
    difference is on output line 446 only (box y 446..446), no other pixel of any picture changed.
  - VRAM (all 4 MB after every VBlank, scripts\gs_vram_hashes.ps1) on all 24 recordings: identical to
    tests/gpu/vram_hashes.txt (the fix is display-only; the pixel-worker start-race fix, the IOP thread
    and the GS wait hook change no drawing).
  - new sw hashes identical synchronously and threaded (4 workers, -Backend both); hw-vs-sw 0 px.
  - tests/gpu/baseline.json regenerated (gpu_diff_all -Update): 13/13 pass, 0 differing pixels.

## In progress
- Step 2: runtime/gpu/gpu_exec.c (render-thread executor) + shaders/submit.hlsl; written in scratch, moving in.

## Next
1. Step 0: prove sw differences vs gpu/M1 are only the bottom display line(s) (gs_compose field mode
   fix), VRAM hashes unchanged; regenerate tests/gpu/baseline.json (gpu_diff_all -Update); commit.
2. Render-thread executor for GPU_FOP (gpu_exec.c): targets, rings (vtx/idx/rec/state), state objects,
   trivial VS/PS, draws, ROV runs, clears, copies, ALIAS16 dispatch; timing per frame/per draw.
3. gs_replay -Backend hwsubmit (+ -Bench) and scripts/gpu_submit_bench.ps1 (median of 5).
4. Measure frontend properly (quiet machine, medians of 5), profile, fix cheap things; frontend
   thread after decode if needed.
5. GO / NO-GO; write results to this note and design appendix D.
6. Verification: run_tests.py, unit suites, gpu_diff_all gate, VRAM hashes, game build debug+release.

## Findings / traps

