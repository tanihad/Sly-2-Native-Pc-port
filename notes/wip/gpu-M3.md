# WIP: gpu-M3 (REF tier core at S=1)

**Goal**: milestone M3 of notes/d3d11-renderer.md section 11 on branch gpu/M3 (worktree
C:\Users\tanih\Projects\sly2-worktrees\gpu-M3): the REF tier (every draw on the ROV path) renders
exactly at S=1; lockstep against the software renderer with bisection.

**Status**: in progress.

## Decisions (M3)
- `-Backend both` / `hw` without `-Ref` stay the M0 all-fallback path (gpu_diff_all keeps its 13/13
  0-diff baseline). `-Ref` selects the real GPU path: frontend (REF: every draw ROV) + render-thread
  executor (gpu_exec.c) + reference compose of the hw memory image.
- Depth truth in REF mode = zpatch (R32G32_UINT UAV, .x = raw Z word: Z32/Z24 full 32-bit word so the
  Z24 top byte survives, Z16 16-bit value). No DSV use in REF mode; resolves ignored in REF.
- vram_buf: DEFAULT raw buffer with SRV + UAV. Patches carry per-word bit masks (only uploaded bits are
  written: gs_vram's other bits are not trusted, hw mode never draws into gs_vram), applied by a CS.
  The CPU mirror of M2 is gone. "blk_cpu" now means "vram_buf holds the truth for these words".
- Downloads are GPU-side (writer target sample 0 -> vram_buf, word-masked to the GPU words) and
  carry (local block, absolute block, writer, word mask). An upload over GPU-written words that does
  not overwrite every bit of the block downloads the block first (no lost writers).
- CPU copies of hw memory (lockstep, reference compose) = download every GPU block into vram_buf,
  then read vram_buf back.
- Lockstep: checkpoints at FRAME/ZBUF register changes (in fr_prim, before the primitive) and at
  VBlank; the frontend closes a partial frame (no PRESENT) for the render thread; compare per block.
  Divergence after the frame's first post pass (ALIAS16 / target view) is reported separately; the
  hw memory is resynced from sw at VBlank after a post pass (M4 implements post passes).

## Done
- Frontend (gpu_front.c/h, gpu_own.c): gpu_staterec / gpu_rec_tri (256 B: 32-bit wrapped attribute
  rows, int64 Z, S/T/Q doubles, left edges, STQ batch-cut table) / gpu_rec_sprite (64 B, V plane
  doubles) / gpu_rec_linepix (int U/V, lod_q) in gpu_front.h; texture levels from registers
  (tex_levels = bind_textures); masked patches (pend_mask), writer downloads (gpu_dl_entry), upload
  over GPU words downloads first, CT24/Z24 never opaque, clear rect + clamped Z, checkpoints
  (gpu_front_checkpoint + hook), resync (also at start), post-pass counter, display_ky2 config.
- Shaders: gsmem.hlsli (swz_tab addressing), mem.hlsl (cs_patch/load/download/clear), gs.hlsl (REF
  pixel pipeline + VS for tri/sprite/linepix, probe records).
- Executor (gpu_exec.c rewritten): vram_buf SRV+UAV, swz_tab, data arena SRV, zpatch R32_UINT,
  REF draws (UAV-only, 16384 viewport), gpu_exec_read_memory, PS2_GPU_PROBE=x,y pixel probe.
- gs.c: gs_compose_from(snap, mem, hist) (gs_compose unchanged for sw).
- gs_replay: -Ref with hw/both (reference compose, picture compare), -Lockstep (checkpoints at
  FRAME/ZBUF changes + VBlank, skip after post pass, resync), automatic bisection (--bisect child:
  primitive dump, differing pixels, crops).
- Lockstep (hw adapter): title 58 compares 0 differ, rv_prompt 39/0, ep8 hub 418/0 (commit 7d463bd+).

## Next
1. Unit harness: gs-draw scenes through both -Ref (lockstep compare at every pixel read) on hw and
   WARP (tests/unit/gs-hw run.ps1 variant), fix divergences.
2. Conformance for rcp_exact / double plane eval (optional; lockstep covers).
3. Regression: run_tests.py, all unit suites, gpu_diff_all 13/13, VRAM hashes, fthread check,
   gs-front, debug+release game builds; GPU ms (hw -Ref -Bench) on title/rv_prompt/ep8 hub.
4. Design appendix E + WIP final + report.

## Findings / traps
- (see design appendices B-D for M0-M2 traps: fxc clamp miscompile, equal RTV/DSV sizes, frontend
  thread p_fields / r_used, gf_vram / gf_clut_src)
- Display-family colour targets had ky = 2 at S=1 while their Z target kept ky = 1 (state_update
  copies the family without set_scale/TARGET op): REF Z samples landed on wrong rows. M3 runs 1x1
  (gpu_front_config.display_ky2 = 0); M7 must give Z targets their colour target's factors.
- The recording's initial memory is not a transfer: vram_buf must be patched with all of gs_vram at
  start (gpu_front_start -> own_resync).
- With -Lockstep the VBlank resync makes the displayed (previous) buffer equal to sw: compare pictures
  without -Lockstep (ep8 hub then differs everywhere post passes run: M4).
- Sly uses STQ widely (title: 868 STQ triangles with batch cuts); the exact float stepping emulation
  (pieces at 64-px columns + batch cuts, Newton+residual-corrected 1/q, `precise`) gives 0 diffs.
- PowerShell [IO.File]::ReadAllText + .Replace with LF strings silently does nothing on CRLF files
  (gs.c): use the Edit tool for CRLF files.
