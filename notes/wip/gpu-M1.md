# WIP: gpu-M1 (frontend dry run)

**Goal**: milestone M1 of notes/d3d11-renderer.md section 11 on branch gpu/M1 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M1).

Status: in progress

## Plan
1. gs_replay `--vram-hash <file>` (per-VBlank FNV of gs_vram, synchronous replay) and the
   "before" hashes of all 24 recordings with the unmodified runtime (build\m1\vram_before\*.txt).
2. gs_setup.c/.h: plane setup (PLANE), rowstep_init/start/next, edge setup/min/max, line DDA moved
   out of gs_draw.c with no arithmetic change; gs_vtx in the header; front-end hook table.
3. runtime/gpu/gpu_front.c (classification, StateRec dedup, vertices/indices/records, D3D draw
   prediction, ROV runs, ALIAS16 batching), gpu_own.c (targets, page/lane ownership, block validity,
   uploads batching, clears), gpu_clut.c (CLUT ring, min/max alpha).
4. gs_replay `--backend hwdry` (+ `--front-only` timing), metrics JSON, summary.
5. Verification (vram hashes after == before on 24, gpu_diff_all unchanged, unit tests, hwdry on 24,
   game builds), per-recording tables here.

## Done
- Step 1: gs_replay `--vram-hash` (-VramHash), scripts/gs_vram_hashes.ps1 (all 24 from corpus.txt,
  -Compare), before hashes with a snapshot of the unmodified runtime (build\m1\runtime_before,
  build\m1\vram_before); combined hashes committed in tests/gpu/vram_hashes.txt.
- Step 2: runtime/gs_setup.c/.h (gs_vtx, planes, gs_tri_setup/planes, rowstep, edges, sprite rule,
  line DDA step, point pixel, frontend hook table gs_front + gs_sw_draw); gs_draw.c uses them and
  calls the hooks (prim, xfer_start, xfer_data, tex0, reset). **VRAM hashes identical on all 24
  recordings, every VBlank** (build\m1\vram_refactor_run.txt); gs-draw 4 variants pass.

## Next
- step 3 (gpu_front.c / gpu_own.c / gpu_clut.c), then 4, 5

## Findings / traps
