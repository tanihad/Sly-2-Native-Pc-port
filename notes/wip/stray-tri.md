# stray-tri: one-frame huge / sliver triangles on Modern (MFT), owner clip 0.1.1064

Branch next/stray-tri (main 8ddc789), worktree C:\Users\tanih\Projects\sly2-worktrees\stray-tri. Never push / merge.
Owner: ep1 hub night, rooftops, Modern, outlines thin, filters modern, render_scale auto, 1920x1080, ~180 fps uncapped.
Evidence: build\clip\evidence (f1155 = big orange triangle, x1417 / w1417 etc. = slivers).
Root cause summary goes to notes/modern-renderer.md.

## Status
- [ ] release build (build\rel)
- [ ] headless repro (MFT 1080p, thin) + detector
- [ ] bisect (PS2_MFT=0, PS2_R2_FANS=0, PS2_R2_GCLEAN=0, outlines ps2, Exact)
- [ ] root cause + fix
- [ ] regression check (tf / mf with MFT on)
- [ ] gates: run_tests, gpu_diff_all, r2_sweep ep1 + 2-3 scenes mf,tf

## Findings
- Tools: PS2_DUMP_SPIKE=1 (ps2_runtime.c dump_spike: online one-picture blink detector, writes only event triples
  spike_<v>.bmp; _N blobs, _T threshold, _MAX); PS2_R2_STRAY=1 (r2_capture.c stray_check: native plain-pipeline
  triangles with a vertex outside the guard band - 0 in every run: R2's native path is clean); PS2_R2_KEEPCHAIN_AB=n;
  scripts\stray_run.ps1 (owner settings, 1080p MFT, long rooftop route; ~3-4 min a run, 2 in parallel).
- Runs are not deterministic (guard fights in some runs only). Real garbage seen: ab2 v8308 (keep ON phase): flat
  pale-blue slab + dark red wedge for one picture (exact path, straight edges) = the owner's slivers.
- Bisect so far (spike events, noisy): base 31 / lt0 (LOOKTHROUGH=0) 23 / nofans 68 / kc0 (KEEPCHAIN=0) 1 / kc0b 0 /
  base2 0; AB (keep on odd 300-VBlank phases): ON 11, OFF 4. LOOKTHROUGH=0 does not fix it.
