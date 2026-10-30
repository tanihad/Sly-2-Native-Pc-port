# r2-m5b: modern renderer R5 part 2 (guard-band clipper native, then the undecoded variants)

**Goal**: the VU clipper (0x2540 / 0x2150 / 0x1E88 / 0x1DC8 / 0x2058) reproduced natively in its own arithmetic so
guard-band meshes with crossing triangles go native (A/B Z gate holds); then billboards, lit 2nd sub-batch, 0x0E30,
0x16B0, 0x13F0; sky / warps last. Profile before / after.
Worktree C:\Users\tanih\Projects\sly2-worktrees\r2-m5b, branch next/r2-m5b (from main 8e272dd).

## Done
- Base exe: build\base\sly2_base.exe (main 8e272dd, release).
- Profile before (chaos, PS2_R2=hybrid PS2_GPU_PROF=1 PS2_R2_STATS=1, tag prof_before, prof_window): GPU ms
  ep1 3.17 (ff-tri-fast-z 0.77, r2 0.71), ep3 3.33 (ff-tri-fast-z 1.10, r2 0.48), m1 2.98 (r2 0.65, ff-tri-fast-z 0.64),
  m2 4.62 (ff-tri-fast-z 1.75, ref-tri 0.72, part 0.66). Census guard vertices: ep1 7055 (9%), ep3 6831, ep5 3831,
  m1 4887 (23%), m2 3725 (28%) - the largest exact share.
- Clipper: runtime/r2/r2_clip.c. Not a C port: the game's own recompiled 0x2540 runs on a private VU1 (vu.c
  vu_private_run / vu_private_init / vu_compiled_image; thread-local XGKICK redirect) with the three records of each
  clipped triangle built exactly as 0x06D0 / 0x0BE0 leave them (VU MXCSR SSE). 0x0C60's verdict per triangle in C
  (r2_clip_verdict). Oracle (scripts/r2_mesh_check.c; build line in scratchpad mc.ps1 = clang -O2 -std=c11 -w -I runtime
  -I runtime\r2 runtime\vu.c runtime\vu_exec.c scripts\r2_mesh_check.c runtime\r2\r2_mesh.c runtime\r2\r2_vertex.c
  runtime\r2\r2_outline.c runtime\r2\r2_clip.c): ep1 / ep3 / ep5 hubs + m2 recordings: every verdict identical
  (~160k triangles), every clipper sequence identical (26 sequences, 60 clipped triangles in m2).
- Hybrid: r2_capture.c clip_fans: run table (R2E_RUNS: the strip triangles as index runs, plain pipeline, no geometry
  shader) + packets into the slot (R2E_FANS, R2H_OUTLINE hint after the entry; the run ends there; epilogue after the
  fans unless the last sub-batch kicked fans). r2.hlsl r2_gs_clip (bracket / fallback) discards crossing triangles
  (GPU clipping only with PS2_R2_CLIP=1). Bracket: R2D_FANS -> gf_r2_skip = 2 keeps TRIFAN prims.
  PS2_R2_FANS=0 off; PS2_R2_FANS_DROP=1/2 diagnostics. Stats "clipper:".
- In-run A/B of EE-side features: PS2_GPU_AB=<n> PS2_GPU_AB_EE=1 (gpu_exec.c gpu_ab_phase; first 8 frames of a phase
  not counted). Diagnostics: "[r2] front: Z beyond the FF range" / "ROV primitives by reason" per phase.
- Found: degenerate objects (clip coordinates ~1e27, vertices on both sides of w = 0) give fans with Z beyond
  GPU_Z_FF_MAX; one such vertex sets zt->z_high and the rest of the frame goes to the ROV tier (ref-other +0.19 ms).
  Such entries stay exact (r2_clip_why 5). Result (A/B, A = guard entries exact): ep1 3.73 -> 3.52 ms, m2 4.98 -> 4.69.
- Sweep ep1 / ep5 / m2 ab + hc pass (first build); hc ep1 / m2 pass with run tables.

**Status**: complete (2026-10-29). Write-up: notes/modern-renderer.md "R5 part 2 as built".

## Gates (final code)
- run_tests; all 26 tests/unit suites (gs-front needed gpu_ab_phase in gpu_front.c: fixed); gpu_diff_all mixed S=1 28/28;
  m9_exact S1 + S3 -Game all identical (base gs_replay from `git archive 8e272dd`); pc_text_boot release + debug; debug +
  release builds; package.py (0.1.970); r2_sweep 21 / 22 (ep7 hc: harness flake, fails without the clipper too).
- Chaos 3x (clean runs before the owner's games started): ep1 284.9 -> 324.7 fps (1% low 127 -> 243, GPU 3.42 -> 2.97),
  m2 190.1 -> 227.8 (GPU 5.16 -> 4.26), m1 285.7 -> 290.7 (3.43 -> 3.29).

## Left
- Chaos ep3 / ep5 at 3x and all five scenes at 4x on a quiet machine (every attempt overlapped VALORANT / DayZ).
- Census of the other swept scenes; the scratchpad gates / census scripts were not added to scripts/.
- Lit sub-batches with clipped triangles (exact: r2_lit_eval colours in the records), lit 2nd sub-batch with unwritten
  colours, billboards (private VU could run 0x0070; camera VU 9 = f_00157FA0 sp + 0x70), 0x0E30, 0x16B0, 0x13F0, sky,
  warps: each <= ~1% of the vertices.
## Findings / traps
- Worktree: git = C:\Users\tanih\Projects\sly2-decomp\tools\git\cmd\git.exe; python = main .venv; clang from main
  tools\llvm-mingw\bin (the worktree's scripts\env.ps1 points at missing worktree tools).
- r2_capture.c / gpu_*.c are CRLF: PowerShell here-string replaces silently miss; use the Edit tool. "R" is a
  PowerShell alias (Invoke-History): never name a helper function R.
- VU marks in the vertex w word (base 0x7000): +0x1000 culled, +0x2000 clipped ([10].z = 1), +0x4000 backface.
- CF (clip flag) latency 4 cycles; the recompiled C (build\rel\vu\vuc_376110651_vu1_ks0.c) encodes it.
- R2's clip pipeline (geometry shader) made the guard meshes cost ~80% of the exact path's FF; the run tables fix it.
- Single-run chaos comparisons of GPU categories are noise; use PS2_GPU_AB (+ _EE) in one run.
