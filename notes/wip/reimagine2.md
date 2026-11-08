# WIP: next/reimagine2 (owner playtest fixes on 0.1.1214)

Branch next/reimagine2 from main 71ff737 (= d2de0e7 + progress note). Worktree sly2-worktrees\light.
Evidence videos: build\reimagine\videos2 (not committed).

Tasks (owner priority order):
1. [x] Fires cast light shafts -> shafts only from sky/moon key light; fires get warm glow + local light flicker.
2. [x] Tree shadows flicker when Sly walks under them (ep2, ep3 swamp) -> find cause, fix, add shot to stability gate.
3. [x] Sly blurry -> crisp at 1440p (anime_6B for characters? CAS sharpen? surface detail?), 1:1 crops.
4. [x] Reimagined on High (4x internal, 8x MSAA) <= 7.5 ms GPU at 1440p (120 fps + 10%).
5. [x] Deterministic stability gate (3/3 passes, each <= 1.15x Faithful).
6. [x] Videos: ep2 fire, trees, Sly close-up orbit (+1:1 stills), ep1 orbit High with frame time; before/after.
7. [x] Gates alone, commit, report.

## Status: COMPLETE (results: notes/reimagined-lighting.md "Owner playtest of 0.1.1214")

## Log
- Repro tools: build\reimagine\treewalk.ps1 (ep2 hub, pad +200:lup:400,+0:lleft:200,+400:lup:600 at 120 fps: Sly walks
  into big tree shadows ~v6500-6900; the hub's start time varies +-100 VBlanks per run), scratchpad grid.ps1 (contact sheet).
- ep2 hub start view (60 fps v5700): far torches cast streaks towards the upper left = shafts from the bright-texel
  centroid (torch sprites write no depth: "far"). Fix in code (uncommitted at first): shafts from the key light's picture
  position only, sky texels weighted by r_skyw (far + neutral hue: warm / saturated = fire, lamps, lasers never count);
  warm glow lights its surroundings (PS2_REIM_FIRE 0.5); CAS sharpening (PS2_REIM_CAS 0.6) in reim_comp.
- ep2 hub shadow map: sphere R 5120 (texel 5 world units!), 25 segments, ~1080 casters a frame; key changes 3 per run.
- DETERMINISM: pad paths were VBlank-relative to START; the hub start varies by 400+ VBlanks (X presses quantized) ->
  different content per run (one "tree walk" run was a Binocucom cutscene). Added to window.c: "@addr#k+n" anchors (the
  k-th call since the script was read, whatever the token order) + "dump:<n>" token with PS2_DUMP_FROM=script
  (ps2_runtime.c). Level transition 0x22B490 (entry hook, release builds): calls 1+2 = Resume Game (right after START),
  hub load = later call. Test settings now = owner's ini (preset high, 100%, MSAA 8x): render_scale=auto made the
  dumps switch 2560 -> 2389 wide mid-run (governor), which broke stability_check (it skips other sizes).
- DETERMINISM (done): one X 2000 VBlanks after START (light_shots -PadAll), PS2_FRAME_DT_FIXED=1 (patch frame_dt_min:
  dt = 1/rate per game frame) and no idle VBlanks between game frames in that mode (ps2_runtime.c host timer: pad frame
  = game frame even when dumps stall). Two identical runs: pic_diff mean block difference 0.25 / 255 (max 1.2), stability
  9.03 vs 9.00 flicker. Before that: BMP dump stalls added 10 Hz VBlanks -> the pad path drifted (Sly into a van).
- TREE FLICKER evidence (DBG=1 shadow-term view, before): over 1-3 frames whole shadow regions change (v7079: the big
  shadow in the lower left goes grey, then a different shape) with key and world matrix unchanged (LOG=3: key steady,
  pv x61 steady) but R2 segment count 33 -> 38 -> 33: the casters are only what the game draws for ITS camera, so
  casters above / behind the view (tree canopies when under the tree) come and go with the camera -> their shadows
  pop. Fix: static shadow cache (gpu_reim.h, static world casters accumulate in a light-space map that moves with
  whole grid steps by copying; per-frame map only the rest). Also: canopies are solid octagons in the map (no alpha
  test) -> cut-out casters.
- ROOT CAUSE found (LOG=3, deterministic): at X+1491 the WORLD MATRIX choice switched (pv[0] 0.0968 -> 0.1171, the
  rebuilt camera jumped (-3625, 3109, 449) -> (1358, -1734, 240) while the real camera moved smoothly; pv x34 -> x35):
  under the trees few world entries are drawn and another object's matrix (a plant group) won the per-segment count ->
  every receiver rebuilt in the wrong space -> shadows jump; the before run's visible flip is at the same X+1491. Fix:
  reim_finish - unlit entries only, counted over the whole frame, continuity of the rebuilt camera (< 600 units from the
  last frame's) preferred, the most common only on the first frame / a cut; lights clustered after the choice.
- With the world fix: DBG=1 view 9.25 / dark 10.35, motion 2.94 (before 13.96 / 14.72, 4.58); the former switch frame
  stays continuous (camera -3625,3109 -> -3615,3126). Full picture ep2 fire+trees: flicker 13.03/dark 10.45 motion 1.64
  -> 10.16/8.14 motion 1.46. Torch streaks gone (before frames 6758: rays from the torch; 6838-6878 a beam from a far torch).
- PERF (High, 1440p, MSAA 8x, 120 fps, in-run A/B 1024, perf2.ps1; not fixed-dt): ep1 orbit before (REIM2=0) Faithful
  5.44 / Reimagined 6.29 ms; after 5.58 / 6.79 (reim-smap 0.04 -> 0.33: static cache + cut-out casters; sc_every 2 then)
  -> sc_every 4 default. ep2: after 4.36 / 5.11, before 4.98. Owner's 9.3 = 0.1.1207 log "tex-build ... others 9.27 ms".
- SLY (ep2 close-up fast orbit, region 300x350 around him, 600 pictures): Faithful grad 2.63 lap 4.31 acut 21.7;
  0.1.1214 3.92 / 6.39 / 30.1; now (CAS 0.6) 5.13 / 12.96 / 37.0. Motion blur (the game's, preset = on) smears him in
  fast camera turns in every look. anime_6B exported to build\models\x4anime6b.bin (not tried in game yet).
- Shadow-term debug view (DBG=1) before 13.96 / dark 14.72 per 1000, motion 4.58 -> cache only (no world fix yet)
  9.76 / 10.72, motion 3.11.
- Code in this pass: alpha-tested cut-out casters (r2.hlsl r2_vs/ps_shadow_at, r2_render.c shpipe_at, state bit 14),
  re_x() experiments (PS2_REIM_X=<mask> + PS2_GPU_AB_R8=4096: 1 cut-out casters, 2 CAS, 4 key-light shafts, 8 fire
  light), PS2_REIM2=0 = 0.1.1214 behaviour (before clips).
