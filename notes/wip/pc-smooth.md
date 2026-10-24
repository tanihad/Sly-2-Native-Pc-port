# pc-smooth (checkpoint)

**Goal**: remove the owner's stutter, menu shaking and shadow flicker on the PC port (GPU renderer, Balanced 3x, 16:9,
2560x1440 @ 165 Hz fixed refresh, frame rate 60 / 120 / 165).

## Done
- Read notes (frame-rate.md, d3d11-renderer.md N/O, pc-menu.md, pc-experience.md pacing), owner logs
  (C:\Users\tanih\AppData\Local\Sly2\sly2.log is from 10/22, an old build: F2 scale cycling, one governor drop
  3x -> 2x "busy 89%"; nothing about the latest session).
- Tools: PS2_PRESENT_TRACE (gpu_present.c: p/g/s lines per present / GPU time / shown present), PS2_PRESENT_DUMP +
  PS2_PRESENT_DUMP_AT=<s>:<n>[:x,y,w,h] (back buffer with overlay as PNG, cropped), scripts\smooth_run.ps1 (warp +
  run + summary; -NoStats, -Rate, -Scale auto default, headless 2560x1440), scripts\smooth_trace.py (summary + PNG panels).
- Baselines (headless, ep1 hub m1_brawl, HPT pad, auto 3x, 2560x1440) - see Findings.

- **Shake root cause found and fixed** (recomp/patches.py field_offset_flip / _flip2 / _init + motion_blur_progressive,
  runtime/gfx.c ps2_progressive): Sly 2 field-renders: after each VBlank wait the render thread stores half = !CSR.FIELD
  (0x2DEDB4) and calls sceGsSetHalfOffset (0x240AE0) -> XYOFFSET.Y + 0.5 on odd fields. In progressive output every
  other frame sat half a native line (= one output line, 3 px at 1440p) higher: a 60 Hz one-line bob of everything
  (menus, safehouse "Press X", pause screen, HUD), invisible when the game runs at 30 fps (all frames same field).
  Evidence: present dumps of a static region of the safehouse screen (Balanced auto 3x, 2560x1440): before 9.6-9.9%
  (blur off) / 22-31% (blur on) of the pixels changing every VBlank, best shift +-3 px alternating; after 0.00-0.01%,
  shift 0 (build/smooth/safe_fixon vs safe_fixoff; PS2_PROGRESSIVE_FIX=0 = old behaviour). The motion blur's V offset
  (0x10 / 0 by the init-time field parity 0x2DEDB8) aligns the other field's frame: in progressive it is 0.5 now.
  Native GS memory of both buffers was identical in slow (30 fps) sw runs - the half offset only alternates at 60 fps.
- flicker_strip (gpu_present.c) also strips the DY+1 variant of the game's wrong-field compensation circuit.
- gpu_hw.c: PS2_GS_SCALE / PS2_GS_GOVERNOR=0 now really keep the scale (gov_frame returned early only in gov_requests).
- Debug tools: PS2_FB_DUMP=<vsync>:<n>[:fbp,...] + PS2_FB_DUMP_DIR (gs.c: native buffers, sw; gpu_present.c: the
  displayed source targets at scale), PS2_DISPLAY_LOG=<vblank>:<n> (display registers + source targets per VBlank),
  PS2_PACE_LOG=4 (only changes), scripts/frame_diff.py (changed %, best whole-pixel shift), gs_draw.c brief trace now
  prints scissor y, OFY, v0.

- Display-locked cadence (frame_rate.c lock, gpu_hw.c clock_lock_k, gpu_present.c quantized phase): 165 Hz fixed at 60
  -> 55 frames/s, every picture 3 refreshes (real-monitor runs below). Limited to <= 60.
- Governor: warm-up >= 1 s, no immediate overload rule above 60 fps (gov_120 / gov_165: auto stays 3x, 98-99 fps).
- Flicker: idle ep2 hub on the merged code (pc-polish + perf P1): 60 fps 1.1-1.6% changing (animation), 120 fps (worker)
  0.6-0.8%, old 27% with +-3 px; notes/pc-smooth/field_bob_before_after.png.
- Notes: d3d11-renderer.md appendix Q, frame-rate.md "Display-locked cadence". Unit tests: pacing 73 + frame rate 24.
- Gates (final state, after merging main ce31cba): run_tests 16/16, every tests\unit suite, gpu_diff_all mixed S=1 28/28,
  gpu_game_all S=1 4196 pictures, pc_text_boot, debug + release builds, package.py - all green (build\gates.txt).

**Status: complete**

## Left (not done)
- PS2_PERF + PS2_GS_HW_STATS cost a ~12 ms hitch once per second (diagnostics only; measure with smooth_run -NoStats).
- Above 60 fps on a fixed-refresh display the frame rate still varies (no lock there; VRR needed).
- The graphics worker's known effect-object race (frame-rate.md "Correctness") was not seen as flicker in the idle hub.
## Real-monitor runs (log each; borderless on the Dell 165 Hz, VRR off, safehouse / ep2 hub, ~20 s windows)
1. rm_auto60 (pacing auto, 60): display classified FIXED -> locked 55: shown 18.2 ms x832, 12 ms x3.
2. rm_lock60 (pacing display): D stuck at ~50 ms after loading (pace_delay decays 0.05 ms/picture) -> lock phase now from
   the 95th percentile directly (up at once, down after 2 s).
3. rm_lock120 (120 locked to 82.5 / 165): broken - display VBlanks above 60 present repeated pictures, 90 ms queue -> the
   lock is limited to rates <= 60 (165 Hz at 60 -> 55).
4. rm_nolock60 / rm_lock60b / rm_lock120b: governor at 2x vs 3x (other agents' GPU load) - not comparable.
5. rm_nolock60s3 vs rm_lock60s3 (strict 3x, camera turning): before shown 12 ms x237 / 18 ms x872 / 66 ms x1, 155 of 1265
   presents never shown; after 18.17 ms x1160 of 1160, game dt p95 18.4 ms. notes/pc-smooth/shown_intervals_165hz.png.

## Findings / traps
- Other agents (pc-research) run game instances at the same time on the same GPU/CPU: absolute numbers are noisy; check
  `Get-Process sly2*` before runs and compare A/B close together.
- PS2_PERF + PS2_GS_HW_STATS cause a ~12 ms hitch once per second (every 60th VBlank: frame queued +12-14 ms after its
  VBlank): base_ep1_60 46 frames > 25 ms in 44 s with stats, 1 without (-NoStats). Measure with -NoStats.
- Headless 60 fps ep1 hub 3x, no stats: Present intervals median 16.69, p99 18.2, max 25.1 ms; game dt > 25 ms: 4 frames
  in 44 s. The CPU / GPU side is smooth at 60: the owner's stutter at 60 must be presentation (59.94 on 165 Hz = 2/3
  refresh holds) - real monitor needed.
- 120 / 165 (auto): governor "could not draw it in real time (171% at 3x) -> 1x" within ~6 s of boot; stays 1x; fps
  ~67 median, intervals 14.7 +- (92% > 1.5x of 8.3 ms). Also with PS2_GS_SCALE=3 (strict) -> 1x.
