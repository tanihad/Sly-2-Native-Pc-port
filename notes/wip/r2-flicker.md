# r2-flicker: shadows flicker + a blinking line, Modern renderer only (owner report, ep1 hub outdoors)

Branch next/r2-flicker (main 05097e6 + main 35bfeef with R8 merged in), worktree
C:\Users\tanih\Projects\sly2-worktrees\r2-flicker. Owner: render_scale auto, filters modern, fullscreen 120 fps, 1440p.
Owner clue (via coordinator): it happens with light around Sly (lamps / lit areas, ep1 hub at night).
Summary for the docs: notes/modern-renderer.md "The owner's flicker report and the temporal check".

## Status
- [x] temporal tool: scripts\flicker_check.c + scripts\flicker_run.ps1; r2_sweep check tf; gates.json level3_temporal
- [x] diagnostics: PS2_DUMP_BMP / PS2_DUMP_WAIT, dump queue 32, PS2_MFT_LOG logs every grid frame
- [ ] root cause of the missing lamp shadow (below) - NOT found; experiments listed
- [ ] gates (run in progress at the time of writing: scratchpad gates.ps1), r2_sweep ab + hc + tf

## Reproduce the missing shadow (deterministic enough, ~2.5 min per run, sequential runs only)
  .\scripts\flicker_run.ps1 -Name x -Episode 1 -Job m1_brawl -Play -At 7000 -Frames 6 -Keep -Pad '+3000:cross:6' `
      -Env "PS2_FRAME_RATE=120;PS2_DUMP_RAW=1;PS2_MFT=0[;more]" [-Mode off]
  .\build\flicker_check.exe --png build\warp\flk_x\frames\frame_007003.bmp build\flicker\x.png 400 800 1200 544 1
  Shadow of Sly's tail on the ledge (raw ~x 780-1020, y 1200-1340): present with -Mode off and PS2_R2_KINDS=props;
  absent in hybrid, also with PS2_R2_FX=0, PS2_GPU_LAZYZ=0, PS2_GPU_ROV_EARLYZ=0, PS2_R2_FANS=0 (+GCLEAN=0),
  PS2_GFX_ASYNC=0, PS2_R2_KINDS=world, forced R2 FBA 0/1, R2 never writing alpha, PS2_R2_ZBIAS -64/+256/+4096, owing
  VIF1 DBF flips for dropped meshes (m->flips), restoring the render list's zeroed CALLs after the chain is built.
  Opdump (PS2_GPU_OPDUMP=6800): the volume draw has 4347 vertices (world native) vs 2250 (props only) - the volumes
  themselves differ. Next: what the shadow class's VU1 program reads that the world's static packets leave.

## Findings / traps
- PNG frame dumps too slow for every VBlank at 3x; BMP raw + 32 slots keep up at 60 fps. DUMP_WAIT at 60 fps stalls.
- Parallel game runs perturb timing: scenes diverge (pad tokens fire at other game states). Run comparisons sequentially.
- Bracket replays use interlaced pictures unless --progressive (temporal tools then see field flicker).
- `Select-Object -First` in a pipeline kills the upstream warp run (PowerShell stops the pipeline).
