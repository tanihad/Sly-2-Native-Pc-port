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
- (nothing committed yet)

## Next
1. Frontend: masked patches, download entries, partial-upload download, CT24/Z24 opaque fix, clear
   rect + clamped Z, records (full 192 B, sprite 64 B, line pixel int UV + lod_q), StateRec for the PS,
   texture levels from registers, partial frames (checkpoints), resync, post-pass flag. Verify:
   gpu_hwdry_all (0 PUNT, VRAM unchanged), gpu_fthread_check, gs-front.
2. Executor: swz_tab, data arena SRV, vram_buf UAV + cs_patch, zpatch, cs_load / cs_download /
   cs_clear, vram readback (run_sync), REF draw binding (UAV-only rendering, power-of-two viewport).
3. ps_gs (ROV REF) + VS for tri / sprite / line pixel; conformance for double plane evaluation.
4. gs.c: gs_compose_from(mem, history) (sw output unchanged); gs_replay -Ref (hw / both) picture
   compare, -Lockstep with checkpoints + bisection.
5. gs-draw scenes through both -Ref on hw and WARP (tests/unit/gs-hw).
6. Lockstep title, rv_prompt, ep8 hub; fix divergences.
7. Regression suite, GPU ms, appendix E, report.

## Findings / traps
- (see design appendices B-D for M0-M2 traps: fxc clamp miscompile, equal RTV/DSV sizes, frontend
  thread p_fields / r_used, gf_vram / gf_clut_src)
