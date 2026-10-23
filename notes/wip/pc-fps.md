# WIP: pc-fps (branch next/pc-fps, worktree sly2-worktrees\pc-fps)

**Goal**: (A) find why VRR (G-Sync Compatible) did not engage for the game on the owner's Dell S2722DGM / RTX 4060 Ti and
fix it on our side, log clearly whether VRR is active; (B) a frame-rate setting (`[display] frame_rate` 60 | 120 | 144 |
165 | 200 | unlimited | display) running the game loop once per presented frame with correct game speed, menu row.

## Done (committed)
- (A) Root cause (evidence below): the NVIDIA driver has VRR off / not possible for the display itself; the driver profile
  says G-SYNC "windowed and full screen". Nothing in our swap chain prevents VRR (a minimal flip app in independent flip
  gets none either). runtime/gpu/gpu_vrr.c/.h: NVAPI read-only (DRS G-SYNC mode + app override, NvAPI_Disp_GetVRRInfo),
  DXGI composition mode; gpu_present.c logs "VRR (at start): NVIDIA G-SYNC: ...; this display: ..." + advice, and a clear
  "VRR: ACTIVE / NOT ACTIVE (fixed X Hz) ... presentation <composed|independent flip>, window in front?" verdict.
  scripts/vrr_probe.c: standalone probe.
  Evidence: NVAPI probe (scratchpad): 0x1194F158 "Enable G-SYNC globally" = 2 (fullscreen AND windowed), 0x1094F1F7
  "VRR requested state" = 2, 0x10A879CF = 0 (allow), no sly2.exe profile; NvAPI_Disp_GetVRRInfo on \\.\DISPLAY1 (DELL
  S2722DGM, DisplayPort, EDID FreeSync range 48-165): enabled 0, possible 0, requested 0, in VRR mode 0. vrr_probe: 10 ms
  presents shown on the 6.06 ms grid in borderless (DXGI "overlay" = independent flip, foreground), borderless+tearing,
  windowed (composed): FIXED 165 Hz.
- (B) runtime/frame_rate.c/.h: setting + live applier (SET_APPLY_FRAME_RATE), env PS2_FRAME_RATE; settings row
  `[display] frame_rate` (30..1000, unlimited=-1, display=-2; template text).
  ps2_runtime.c host timer: frame limiter (VBlank when the game's render thread waits for one, >= 1/rate apart; 59.94 Hz
  fallback when it does not ask), back to the exact 59.94 schedule at 60.
  Patches (recomp/patches.py): frame_dt_min (0x14D120: the clock's 1/60 lower clamp -> 1/625 above 60), frame_vblank_wait
  (0x16F560 entry -> frame_vblank_request), frame_movie_begin/end (0x1B3BD0 / 0x1B3EB4: movies at 59.94).
  gpu_pace: governor budget = the frame period (G.period_ms; sw fallback still judged at 59.94); gpu_present: pacing
  state follows pace_content_ms(); display clock thread: k from the frame period (display rate = frame rate -> display
  VBlanks drive the game); frame_rate_note_display from the measured refresh.
- Test tools: window.c time tokens r<s> and holds "<s>s"; PS2_FPS_TRACE (per-VBlank: t, dt, player obj +0x230 via camera
  target 0x2DE1A0, camera 0x2DDFD0, model centre z obj+0x2198); PS2_DUMP_SECS; scripts/fps_run.ps1, fps_trace.py,
  fps_sheet.py.
- Results so far (headless, ep2 hub, 3-4 instances in parallel): run speed 510 u/s steady at 60/120/200, same ramp;
  jump +217 / +216 / +216 / +216 (60/120/165/200), peak 0.44 s, air 0.89 s; double jump second +110; camera turn
  64 deg/s at all rates; title reached at game t 20.1 / 20.0 / 19.9 / 19.9 s (intro cutscene + attract are game-time).
  Movies: at 120 VBlanks the intro movie played ~2x fast -> movie patches (59.94 during movies).
- EE throughput limits the rate: ep2 hub alone at 120 = 65-113 fps (EE 9-15 ms/frame: VU1 microcode, GIF/VIF on the EE
  thread; profile: game code only 5%). Intro cutscene ~120 fps.

## Real-monitor runs
- 2026-10-23 V1: vrr_probe.exe 3 s x (borderless, borderless+tearing, windowed, exclusive), 10 ms cadence, ~15 s total.
- 2026-10-23 V2: vrr_probe.exe 4 s borderless + 4 s exclusive (SetFullscreenState, swap chain without tearing).
- 2026-10-23 M1/M2: game borderless (pace_run.ps1, 45 s each from boot: movie, intro cutscene, title), frame_rate =
  display (165) then 120, PS2_PACE_LOG=1, scale 3, the game closes itself (watchdog). M3: display again (45 s) after the
  governor change. M4: display (45 s) with the limiter instead of display VBlanks above 60.
  M5: display (45 s) after: no present skipping / queue 2 / 10 Hz idle VBlanks above 60.

## Next
1. Job/minigame matrix at 60 vs 165 (and 120/200 for some): m2_dance_audition, m2_turret, m4_hack, m2_rc_chopper,
   m5_battlezone, a boss; job intro cutscenes; compare traces + fps_sheet pictures by game time.
2. Movie check at 120 (intro movie duration with the movie patches).
3. Menu row (menu.c MENU_ROW_FRAME_RATE, Display page) + notes/frame-rate.md + settings.md + pc-experience.md.
4. Gates (run_tests, unit suites, gpu_diff_all, pc_text_boot, debug + release builds, package.py).
5. One short real-monitor run with frame_rate = 165 / display (owner's monitor) to see presentation.

## Findings / traps
- Game clock f_0014D0B8(a0 = 0x2DDED0 clock, a1): dt = (COP0 Count delta via 0x14D240, 64-bit extended) / 294.912 MHz
  (0x31690453), clamped to [1/60 (0x3C888889), 1/30 (0x3D088889)]. +0x10 = dt * +0xC, +0x14 += it; +0x40 = dt * +0x24,
  +0x3C += it; if !a1: +0x4 = +0x40 * +0x8 (game dt), +0x0 += +0x4 (game time t, read by ~400 sites).
- Main loop f_001A7518: clock, world update, WaitSema(0x2DEB1C) buffer free (0x160828), render list, SignalSema 0x2DEB18
  (0x160A98), frame counter 0x2BE538++ (used only by 2D scene state "started < 2 frames ago" checks 0x212544..).
- Render thread f_00160B70: WaitSema(0x2DEB18) -> kick the frame's DMA (0x16F8B8 -> VIF1, runs on the EE thread) ->
  SignalSema(0x2DEB1C) -> 0x16F590: wait next VBlank (0x16F560: 0x2DFB58 = 1, WaitSema 0x2DFB4C) then the display regs.
- The game's only VBlank handler: INTC 2 -> 0x16F468. Others: 5 (VIF1) 0x167F78, 0 (GS) 0x16F868, 11 (TIM2) 0x239F28.
- 0x2DE1C0 = camera focus (no jumps); camera 0x2DDF40, target object ptr at +0x260 (0x2DE1A0), Sly in ep2 hub 0xCFF160
  (+0x230 = grounded position, does not follow jumps; obj+0x2190 = a model centre that does).
- PS2_PEEK every VBlank costs ~4 ms per VBlank (unbuffered stderr fprintf, 28% of the EE thread): never trace with it.
- PowerShell [IO.File] / Set-Content: absolute paths; runtime files are CRLF (use the Edit tool or `r`n in replacements).
- DXGI exclusive fullscreen refuses swap chains with the FRAME_LATENCY_WAITABLE_OBJECT flag (INVALID_CALL).
- Discord is running on the owner's PC (its overlay can break independent flip; not observed in the probe: "overlay").
