# r2-flicker: shadows flicker + a blinking line, Modern renderer only (owner report, ep1 hub outdoors)

Branch next/r2-flicker (main 05097e6 + main 35bfeef with R8 merged in), worktree
C:\Users\tanih\Projects\sly2-worktrees\r2-flicker. Owner: render_scale auto, filters modern, fullscreen 120 fps, 1440p.
Summary for the docs: notes/modern-renderer.md "The owner's flicker report and the temporal check".

## Status
- [x] temporal tool: scripts\flicker_check.c + scripts\flicker_run.ps1; r2_sweep check tf; gates.json level3_temporal
- [x] diagnostics: PS2_DUMP_BMP / PS2_DUMP_WAIT, dump queue 32, PS2_MFT_LOG logs every grid frame, PS2_VU_SNAP(+_TRACK)
- [x] root cause of the missing / flickering lamp shadow: VIF1 state the dropped world chains leave (STMASK 3F3F3F3F
      for the shadow class's masked unpacks; MSCAL flip parity across classes). Fixed in r2_capture.c / r2_mesh.c
      (owe_vif / flush_vif / owe_flips_class); see the notes section.
- [x] verified on the final code: ALT 3x grid - same shadow in modern and exact frames; class begin mask 3F3F3F3F every
      frame; tf at the lamp spot PASS on the grid (236 pictures) and MFT 1440p MSAA 4x (240); hc ep1 / ep2 PASS.
- [ ] final gate run on c61586e code: run_tests PASS; tests\unit 25/26 (window F11 / Alt+Enter, known); debug + release
      builds OK; gpu_diff_all mixed S=1 28/28; m9_exact S1+S3 -Game
      identical; pc_text_boot release + debug PASS; package OK (0.1.1013); r2_sweep (parallel 3, GPU busy):
      25/33 - ab 11/11, failures = runs that drew 0 pictures / few pictures + m2 hc 4.2%, ep5 hc 0.25%: re-run: flips (MSCAL payment) broke ep6 -> off by default (0xFE);
      ep4 / ep6 / m2 / m3 hc + tf 8/8 PASS after that. NEXT: keep native chains with MSCALs patched to NOP (VIF MARK count)

## Reproduce (sequential runs only, ~2.5 min each)
  .\scripts\flicker_run.ps1 -Name x -Scale 3 -Episode 1 -Job m1_brawl -Play -At 7000 -Frames 30 -Keep -Pad '+3000:cross:6' `
      -Env "PS2_FRAME_RATE=120;PS2_MFT=0;PS2_R2_ALT=6900"     (PS2_R2_VIFSTATE=0: the old behaviour)
  .\build\flicker_check.exe --png build\warp\flk_x\frames\frame_007010.bmp build\x.png 400 800 1200 544 1   (and 7011)

## Findings / traps
- Paying the flips inside the main class broke ep2 (its next exact chain reads the "other" buffer the last exact chain
  filled); restoring registers after a class switch's packets would override them - both are gated now.
- Left: an overlapping shadow polygon at the screen's bottom edge still differs in the ledge view (~600 px at 3x).
- Parallel game runs perturb timing (scenes diverge); `Select-Object -First` kills an upstream warp run; PNG dumps too
  slow at 3x (use BMP); PS2_DUMP_WAIT at 60 fps stalls the EE.
