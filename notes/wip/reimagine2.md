# WIP: next/reimagine2 (owner playtest fixes on 0.1.1214)

Branch next/reimagine2 from main 71ff737 (= d2de0e7 + progress note). Worktree sly2-worktrees\light.
Evidence videos: build\reimagine\videos2 (not committed).

Tasks (owner priority order):
1. [ ] Fires cast light shafts -> shafts only from sky/moon key light; fires get warm glow + local light flicker.
2. [ ] Tree shadows flicker when Sly walks under them (ep2, ep3 swamp) -> find cause, fix, add shot to stability gate.
3. [ ] Sly blurry -> crisp at 1440p (anime_6B for characters? CAS sharpen? surface detail?), 1:1 crops.
4. [ ] Reimagined on High (4x internal, 8x MSAA) <= 7.5 ms GPU at 1440p (120 fps + 10%).
5. [ ] Deterministic stability gate (3/3 passes, each <= 1.15x Faithful).
6. [ ] Videos: ep2 fire, trees, Sly close-up orbit (+1:1 stills), ep1 orbit High with frame time; before/after.
7. [ ] Gates alone, commit, report.

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
- Code in this pass: alpha-tested cut-out casters (r2.hlsl r2_vs/ps_shadow_at, r2_render.c shpipe_at, state bit 14),
  re_x() experiments (PS2_REIM_X=<mask> + PS2_GPU_AB_R8=4096: 1 cut-out casters, 2 CAS, 4 key-light shafts, 8 fire
  light), PS2_REIM2=0 = 0.1.1214 behaviour (before clips).
