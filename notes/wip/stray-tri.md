# stray-tri: one-frame huge / sliver triangles on Modern (MFT), owner clip 0.1.1064

Branch next/stray-tri (main 8ddc789), worktree C:\Users\tanih\Projects\sly2-worktrees\stray-tri. Never push / merge.
Owner: ep1 hub night, rooftops, Modern, outlines thin, filters modern, render_scale auto, 1920x1080, ~180 fps uncapped.
Evidence: build\clip\evidence (f1155 = big orange triangle, x1417 / w1417 etc. = slivers).
Root cause summary goes to notes/modern-renderer.md.

## Status
- [x] release build (build\rel)
- [x] headless repro (MFT 1080p, thin) + detectors
- [x] root cause: R2 private-VU clipper fans of degenerate objects (w ~2^69) with impossible Q (see modern-renderer.md
      "Stray triangles"); badq triangles hybrid 1178 / FANS=0 0 / Exact 0
- [x] fix r2_clip.c reason 6 (Q): after fix badq 0
- [x] regression check: mft_pops.ps1 (sweep mf) fails on badq triangles
- [x] gates: run_tests 16/16, gpu_diff_all mixed S=1 28/28, r2_sweep ep1,ep3,ep5,m2_boardroom mf,tf 8/8 PASS (stray
      triangles 0); mf ep1 with PS2_R2_FANQ=0: FAIL, 10 stray triangles (the check catches it)
- [x] COMPLETE (2026-11-04)

## Findings
- Tools: PS2_DUMP_SPIKE=1 (ps2_runtime.c dump_spike: online one-picture blink detector, writes only event triples
  spike_<v>.bmp; _N blobs, _T threshold, _MAX); PS2_R2_STRAY=1 (r2_capture.c stray_check: native plain-pipeline
  triangles with a vertex outside the guard band - 0 in every run: R2's native path is clean); PS2_R2_KEEPCHAIN_AB=n;
  scripts\stray_run.ps1 (owner settings, 1080p MFT, long rooftop route; ~3-4 min a run, 2 in parallel).
- Runs are not deterministic (guard fights in some runs only). Real garbage seen: ab2 v8308 (keep ON phase): flat
  pale-blue slab + dark red wedge for one picture (exact path, straight edges) = the owner's slivers.
- Bisect so far (spike events, noisy): base 31 / lt0 (LOOKTHROUGH=0) 23 / nofans 68 / kc0 (KEEPCHAIN=0) 1 / kc0b 0 /
  base2 0; AB (keep on odd 300-VBlank phases): ON 11, OFF 4. LOOKTHROUGH=0 does not fix it.
- Later: RUNPROG=1 0, RUNPROG_K 0/0 (short windows), PS2_MFT=0 (grid) 0 events in 6144 pictures over the whole
  route (MFT runs of the same length: 2..68) -> likely MFT-specific. Many events are noise (fights, close camera).
- More tools: PS2_GF_BIGTRI=<px> + _FILE + _FROM (gpu_front.c: big exact triangles to a file), PS2_MFT_LOG_BA=n (op
  list of frames that go back from phase B to A), PS2_MFT_PLANLOG=<file> (every planned MFT frame's phases),
  PS2_VIF_TOPLOG (VU1 runs with the same TOP, not useful).
- Found: in the ep1 hub every frame goes B -> A: after the 2D MARK come the outlines (thousands of line draws, in
  phase B on the picture) and then a late native R2 segment (-> phase A, unresolve) + one draw, then PRESENT.
