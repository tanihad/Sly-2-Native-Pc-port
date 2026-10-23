# WIP: pc-fps (branch next/pc-fps, worktree sly2-worktrees\pc-fps)

**Goal**: (A) find why VRR (G-Sync Compatible) did not engage for the game on the owner's Dell S2722DGM / RTX 4060 Ti and
fix it on our side, log clearly whether VRR is active; (B) a frame-rate setting (`[display] frame_rate` 60 | 120 | 144 |
165 | 200 | unlimited | display) running the game loop once per presented frame with correct game speed, menu row.

## Done
- RE of the game's timing (see Findings).
- NVAPI read-only probe (scratchpad nvprobe.c): base + global profile: 0x1194F158 "Enable G-SYNC globally" = 2
  (fullscreen AND windowed), 0x1094F1F7 "VRR requested state" = 2, 0x10A879CF "G-SYNC" (app override) = 0 (allow); no
  sly2.exe application profile. So the driver's G-SYNC mode is NOT "fullscreen only".
- scripts/vrr_probe.c: standalone probe (flip-discard swap chain like the game's) presenting off-grid (10 ms) in
  borderless / borderless+tearing / windowed / exclusive and reading the shown intervals + DXGI composition mode.

## In progress
- Real-monitor VRR probe run (logged below before each run).

## Real-monitor runs
- (none yet)

## Next
1. VRR probe on the real monitor (< 20 s), interpret.
2. Fix on our side + VRR log line (gpu_present.c pace_statistics), NVAPI settings line at start.
3. Frame-rate mode (B): VBlank clock at R Hz, dt clamp patch (0x14D0B8), movies at 59.94, governor period.

## Findings / traps
- Game clock f_0014D0B8(a0 = 0x2DDED0 clock, a1): dt = (COP0 Count delta via 0x14D240, 64-bit extended) / 294.912 MHz
  (0x31690453), clamped to [1/60 (0x3C888889), 1/30 (0x3D088889)]. +0x10 = dt * +0xC, +0x14 += it; +0x40 = dt * +0x24,
  +0x3C += it; if !a1: +0x4 = +0x40 * +0x8 (game dt), +0x0 += +0x4 (game time t, read by ~400 sites).
  -> at > 60 fps the lower clamp makes the game run fast (120 fps: 2x): must be patched.
- Main loop f_001A7518: per frame 0x20E890, 0x1871C0, 0x14BA38, camera 0x14D470, clock 0x14D0B8, world 0x226550, HUD
  0x1D9648, 0x160828 (WaitSema 0x2DEB1C: a free frame buffer of 2), render list 0x1CDA80 / 0x157FA0, 0x160A98 (SignalSema
  0x2DEB18: frame ready), frame counter 0x2BE538++. No VBlank wait in the main thread.
- Render thread f_00160B70: WaitSema(0x2DEB18) -> kick the frame's DMA -> SignalSema(0x2DEB1C) -> 0x16F590: waits for the
  next VBlank (0x16F560: flag 0x2DFB58 = 1, WaitSema 0x2DFB4C) then sets the display (field-dependent DISPFB/DISPLAY).
- The game's only VBlank handler: INTC 2 -> 0x16F468 (signals 0x2DFB4C when 0x2DFB58 is set; display state machine
  0x2DEDBC). Other INTC handlers: 5 (VIF1) 0x167F78, 0 (GS) 0x16F868, 11 (TIM2) 0x239F28 (library). IOP gets no VBlank.
  So the game's frame rate = the VBlank rate when it keeps up; nothing else in the game counts VBlanks.
