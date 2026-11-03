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
- [x] final design (478f6ad): chains dropped by default; in the class run before a foreign class kept + programs run
      with XGKICK dropped (VIF MARK 0xA5A5 / 0xA5A6 protocol), owed parity paid flip-only; tail flushed at the next hook.
      ALT ledge 3x grid: same shadow both modes (heat 276); ep2 / ep6 hc + tf PASS; kept 90 / frame, parity unpaid 0.
- [x] CPU A/B (PS2_GPU_AB_EE=3, 3x grid): ep1 +0.03 ms/frame, m1 +0.04, m2 -0.01 (table in the notes). Kept.
- [x] verification: ALT heat ledge 118 / spawn 154 (3x grid); MFT ledge shadow = exact (crops); tf lamp walk 700: grid 3x / 1x PASS, MFT PASS (1 of 2 runs one picture 25 blobs, not reproduced); MFT "other target" grid frame ~1 per run also with the fix off (R8)
- [x] gates A on 478f6ad: run_tests PASS, units 25/26 (window F11/Alt+Enter, known), debug build OK, gpu_diff_all 28/28, m9_exact S1+S3 identical
- [x] gates B: pc_text_boot release + debug PASS, package OK (0.1.1020), r2_sweep ab,hc,tf 33/33 PASS
- [x] COMPLETE (2026-11-03)
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

## Part 2: owner's report on 0.1.1032 (outlines = thin), 2026-11-03
- [x] merged main 57c8877; flicker_run -Ini "section.key=value;..." writes the run's sly2.ini (console exe: portable)
- [x] thin repro (owner's ini, lamp walk 700): MFT FAIL only on one-sided pictures (gaps / dump-induced grid frames);
      flicker_check: one-sided judgements now informational (--edges to count them)
- [x] real cause (thin or ps2 alike): PS2_R2_UNPAID_LOG / PS2_R2_SEQ_LOG show "0:118/114/0 11:2/0/0 6:..": a short
      exact class-11 run between the world's last class-0 run and the shadow class 6 -> the class-0 run was not "before
      foreign", its chains dropped, parity unpaid at class 6 (hundreds of frames per walk, toggling = flicker)
- [x] fix: before_foreign looks through short main-class runs (PS2_R2_SKIPRUN, default 16): unpaid 900 -> 2 (loading)
- [x] verified: ALT lamp walk (At 7900, 800 VBlanks, grid 3x): before, hybrid frames with the short class-11 run lost
      Sly's lamp shadow the exact neighbours had (wa0 8491 / 8493); after, the same pattern draws the exact shadow (wa2 8500)
- [x] owner set walk 700 @120 + DUMP_WAIT: grid PASS; MFT FAIL in both outline styles (om thin 4 over, omp ps2 5 over):
      dump-induced MFT grid fallbacks (~1 per 60 VBlanks with dump waits, none without dumps) break the motion-blur
      feedback for a few frames (v8547: whole frame lighter 10 frames after the v8537 fallback). Dump artefact; R8 note.
      motion_blur=off: MFT dumps come out 512x448 (the grid memory, not the MFT picture) - not usable
- [x] r2_sweep tf variants tft/tfq/tfb/tfp/tfo: ep1 6/6 PASS (settings confirmed in the logs); parity guard in flicker_run
- [ ] full gates (scratchpad gatesC.ps1: m9 base = main 57c8877), CPU A/B (GPU was busy at 13:11: frame ms invalid)