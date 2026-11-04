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
