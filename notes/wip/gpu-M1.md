# WIP: gpu-M1 (frontend dry run)

**Goal**: milestone M1 of notes/d3d11-renderer.md section 11 on branch gpu/M1 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M1).

Status: in progress (implementation done, final verification running)

## Done
- Step 1: gs_replay `--vram-hash` (-VramHash), scripts/gs_vram_hashes.ps1 (all 24 from corpus.txt,
  -Compare), before hashes with a snapshot of the unmodified runtime (build\m1\runtime_before,
  build\m1\vram_before); combined hashes committed in tests/gpu/vram_hashes.txt.
- Step 2: runtime/gs_setup.c/.h (gs_vtx, planes, gs_tri_setup/planes, rowstep + gs_rowstep_attr,
  edges, sprite rule, line DDA step, point pixel, gs_llround, frontend hook table gs_front +
  gs_sw_draw); gs_draw.c uses them and calls the hooks (prim, xfer_start, xfer_data, tex0, reset).
  **VRAM hashes identical on all 24 recordings, every VBlank**; gs-draw 4 variants pass.
- Step 3: runtime/gpu/gpu_front.h (public: config, per-frame counters, command stream ops),
  gpu_front_int.h, gpu_front.c (classification, StateRec dedup, vertices/indices/records, draw
  merging, ROV runs, ALIAS16 batches, clears, commutation estimate, trace), gpu_own.c (targets,
  per-block ownership/serials/CPU word masks, loads, downloads, patches, shadow copies, eviction),
  gpu_clut.c (CLUT ring, content change detection, min/max alpha cache).
- Step 4: gs_replay `--backend hwdry` (-Backend hwdry, -FrontOnly, -Ref, -StraddleRov, -SelfCheck,
  -DecodeOnly, -Define), metrics JSON (per VBlank + sum + max), summary on stderr, exit 5 on PUNT,
  "replay thread CPU" (QueryThreadCycleTime) line; scripts/gpu_hwdry_all.ps1 (all 24: hwdry with sw,
  VRAM hash vs tests/gpu/vram_hashes.txt, 0 PUNT, build\m1\hwdry\table.md + summary.json).
- tests/unit/gs-front (24 checks: gs_llround, Z clear, shadow volume ROV run, ALIAS16 hazard split,
  pass-A all-pass/straddle/As>128, Z32 high, FMV uploads -> 1 patch + TBW 12, photo REGION_CLAMP
  view kept 200 frames, T8H darken feedback copy vs RGB-only no copy, AA1 outline pair pixels).
- Corpus run 2 (before the CLUT change): 24/24 ok, 0 PUNT, VRAM unchanged (table below).

## Next
1. Final gpu_hwdry_all (with -SelfCheck), front-only/decode-only timing medians for the heavy
   recordings, tables into this note.
2. gpu_diff_all gate (13, baseline unchanged), run_tests.py, every tests/unit/*/run.ps1 (window
   -UnitOnly), build_game.py debug + --release, default still sw.
3. Design doc appendix "As built: M1" + section 6/4 notes; mark complete.

## Findings / traps
- Recordings start mid-frame: the CT16 shuffle can come before its C32 target was drawn in the
  recording -> a CT16 FRAME creates its C32 target (generic, exact via loads).
- Target height from SCISSOR is wrong (a field buffer drawn with scissor up to 255 overlapped the photo
  copy at 070): rows now grow with the drawn/read extent (own_grow). Same-kind targets overlapping at
  another base are separate targets (block ownership makes the aliasing exact, counted
  `target_overlaps`); the design's "starting inside -> PUNT" rule is not needed.
- Per-block ownership (256-byte GS blocks: blk_writer, blk_serial, 64-bit CPU word mask) instead of
  per-page: Sly uploads CLUTs/textures into Z pages every frame, partial blocks included.
- **CLUT loads read Z-buffer words**: ep4/ep5 load a 256-entry T8 CLUT at CBP 1000 every frame of
  which only ~112 entries were uploaded; the rest of the 16x16 block was last written by the Z buffer.
  Downloading would stall every frame; the unused entries are never indexed (else the PS2 shows Z
  garbage too), so the frontend counts it (`clut_gpu_reads`) and does not download. M3 lockstep checks.
- The T8H darken writes alpha too (FBMSK 0), so it IS a feedback hazard (design 5.1 assumed RGB only):
  one shadow copy of the dirty bbox per darken pass (block-level dirty tracking), ~1 per game frame.
- Outline pairs (ZB_ONLY line, then AA1 RGB_ONLY line) alternate pipeline keys: 2 D3D draws per
  outline; commutation by bbox barely helps (segments touch). Biggest draw-count contributor with
  pass A/B alternation. See commute column.
- ROV runs: shadow volumes alternate Z-tested / not: runs merge across zt (a run binds the colour
  target, depth when any draw needs it); ROV draws merge across blend/test/Z mode (data).
  Remaining runs come from pass-A draws with As > 128 (lerp blend): each is a ROV run + Z resolve
  (ep7 boss: 215/frame, 22% of lerp kicks). M2/M5 must weigh this.
- Frontend cost (front-only minus decode-only, replay thread CPU): ep8 boss ~1.9 ms per VBlank
  (~4.5 ms per game frame), decode alone ~1.9 ms/VBlank, on a machine at ~90% load. Hot spots fixed:
  per-pixel ALIAS16 hazard bitmaps -> rectangles; state analysis cache (32 entries, signature incl.
  CLUT slot); clean-target fast path; texture source cache; gs_llround (llround libcall).
- gs_draw.c: in frontend-only mode bind_textures skips gs_tex_bind (sw texture cache); a live
  backend switch (M6) must set env_dirty.
- PowerShell: here-strings without a trailing newline glue lines; `R` is an alias (Invoke-History).
  The agent sandbox blocks commands whose text contains `Remove-Item $var` in some forms.
