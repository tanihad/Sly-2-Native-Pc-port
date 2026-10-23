# Frame rate above 60 and variable refresh (pc-fps)

Owner requests (2026-10-23): "choose the framerate cap and switch between 60 120 144 165 200", and G-Sync on the Dell
S2722DGM (2560 x 1440, 165 Hz, FreeSync Premium / G-SYNC Compatible) with an RTX 4060 Ti, which the pacing work had found
not active. Code: runtime/frame_rate.c/.h, ps2_runtime.c (host timer: frame limiter), recomp/patches.py (frame_*),
runtime/gpu/gpu_vrr.c, gpu_present.c / gpu_hw.c / gpu_exec.c (pc-fps parts), menu.c (Display > Frame rate).
Checkpoint history: notes/wip/pc-fps.md.

## How Sly 2 times its frames (SCUS_973.16)

- **Game clock** f_0014D0B8 (called once per main-loop frame with the clock at 0x2DDED0): dt = COP0 Count ticks since the
  last call (0x14D240 extends the 32-bit Count to 64 bits) x 1/294.912 MHz, **clamped to [1/60, 1/30] s**; then
  +0x10 = dt x +0xC, +0x14 += it; +0x40 = dt x +0x24, +0x3C += it; +0x4 = +0x40 x +0x8 (the game's dt, read by ~70
  sites), +0x0 += +0x4 (the game time t, read by ~400). Everything in the world is stepped by this real-time dt (the Sucker
  Punch engine of Sly 1 already worked so): the PS2's "30 fps scenes" are just slower frames with bigger steps.
- **Main loop** f_001A7518 never waits for a VBlank: camera, clock, world update (0x226550), HUD, wait for a free frame
  buffer (0x160828: WaitSema 0x2DEB1C), render list -> DMA chain (0x1CDA80, 0x157FA0), frame ready (0x160A98: SignalSema
  0x2DEB18), frame counter 0x2BE538++ (used only by 2D-scene "started less than 2 frames ago" checks).
- **Render thread** f_00160B70: waits for a frame (0x2DEB18), kicks its DMA (0x16F8B8: VIF1 -> VU1 -> GIF, on the EE thread
  in the port), frees the buffer (0x2DEB1C), then **0x16F590 -> 0x16F560 waits for the next VBlank** (flag 0x2DFB58 = 1,
  WaitSema 0x2DFB4C) and sets the display registers (field-dependent DISPFB / DISPLAY, 0x16F2D0).
- **The game's only VBlank handler** is INTC 2 -> 0x16F468 (signals 0x2DFB4C when 0x2DFB58 is set; display state machine
  0x2DEDBC). The other INTC handlers are VIF1 (0x167F78), GS (0x16F868) and TIM2 (0x239F28, a library); the IOP gets no
  VBlank (989snd's tick is the IOP hard timer, the mixer runs on its own clock). Nothing else in the game counts VBlanks.
- **Movies** (the player 0x1B3BD0, its own decode / display loop) are paced by the VBlanks they wait for: at 120 VBlanks
  per second the intro movie played twice as fast.

So the game's frame rate is simply the VBlank rate it gets (while it keeps up), and above 60 only the clamp's lower bound
1/60 is in the way: every 120 fps frame would advance the world by 1/60 s (double speed).

## What frame_rate does

`[display] frame_rate = 60 | 120 | 144 | 165 | 200 | <30..1000> | display | unlimited` (live; PS2_FRAME_RATE; menu
Display > Frame rate). **60 (default) changes nothing**: the 59.94 Hz VBlank timer and the game's own clamp.

Above 60:
- **Frame limiter** (ps2_runtime.c host timer): patch `frame_vblank_wait` (entry of 0x16F560) tells the timer that the
  render thread waits for its VBlank; the VBlank is raised then, at most one per 1 / rate s (display = the display's
  measured refresh; unlimited = at most 500 per second). Without a request: 59.94 Hz (boot, loading, anything not driven by
  the main loop), or 10 Hz while requests keep coming but the frames are just slower (a VBlank without a new frame would
  only compose and present the same picture again; measured: a GPU behind the limiter was slowed further by those
  pictures). Back at 60 the 59.94 Hz schedule continues from the last VBlank.
- **dt clamp** (patch `frame_dt_min`, after `mtc1 $f2` at 0x14D120): the lower bound becomes 1/625 s; the 1/30 upper
  bound stays (below 30 fps the game slows down as on a PS2).
- **Movies stay at 59.94** (patches `frame_movie_begin` / `_end` at the movie player's entry and return).
- **RC chopper flights stay at 59.94** (frame_rate.c `keeps_60`: the patch passes the vtable of the camera's target,
  0x2DE1A0 -> +0xC; the RC chopper class 0x2D5898, episodes 2 / 4 / 6): its controller is not frame-rate independent
  (below).
- Presentation (GPU renderer): one picture per game frame; the display's own VBlanks never drive the game above 60 (they
  would present a picture per refresh even without a new frame); no present is skipped (when the GPU is slower than the
  limiter every picture is "late"); the render queue holds 2 frames (low latency; the EE then waits for the GPU, which is
  harmless because the game's speed is real time). The pacing state (paced present, VRR classifier) follows the frame
  period (pace_content_ms). The internal-scale governor keeps judging against the 59.94 Hz frame: the resolution is lowered
  to keep 60 fps, never to reach a higher frame rate (the first version lowered 3x -> 1x at 165 fps); its "game waited for the
  GPU" evidence is ignored above 60 (the game waits for the GPU by design there).

## Tests (scripts/fps_run.ps1 -> trace.txt per VBlank, fps_trace.py, fps_sheet.py; headless release build)

Time-based input (`r<s>` tokens and holds in seconds, runtime/window.c) gives every rate the same real-time input;
PS2_FPS_TRACE logs game t / dt, the camera target's position (+0x230), a model centre height (target +0x2198, follows
jumps) and the camera; PS2_DUMP_SECS dumps pictures every N wall seconds.

| system | 60 | 120 | 165 | 200 | unlimited | verdict |
|---|---|---|---|---|---|---|
| run speed (ep2 hub, stick held 2 s) | 510 u/s, 0.15 s ramp | 510, same ramp | - | 510, same ramp | 339 u/s avg (same as 120/200) | same |
| jump height / time to peak / air time | +217 / 0.45 s / 0.90 s | +216 / 0.45 / 0.89 | +216 / 0.44 / 0.89 | +216 / 0.43 / 0.88 | +215 / 0.46 / 0.90 | same |
| double jump (second part) | +110 | +136 (heavily loaded run, other timing) | +111 | +111 | +109 | same |
| camera turn (right stick) | 64 deg/s | 63 | 64 | 65 | - | same |
| intro cutscene + attract (game time at the title's START) | t 20.13 | 20.01 | 19.92 | 19.90 | - | same |
| intro movie (IPU active) | 2.2-6.2 s | 2.4-6.4 s with the movie patches (2.3-4.3 s = 2x fast before) | | | | fixed |
| Binocucom briefing (m4_hack) | voice-driven; ends ~3 game s later than at 165 | | game clock 5% behind the wall clock under 2-instance load | | | real-time audio vs a game clock that loses frames > 1/30 s (60 gains instead: below) |
| dance audition (no input) | Job Failed at ~74 s wall | | Job Failed at the same time (+-2 s) | | | same |
| turret (m2_turret start, idle) / hack (m4_hack hub part) | pictures match by game time | | match | | | same |
| RC chopper, stick released | sways +-1200 sideways, +-500 forward | sideways settles in 5 s | sideways settles | sideways settles, forward sway larger (+-850) | | **differs -> keeps 60** |
| every episode hub (ep1-8, -Play: walk, jump, camera; 4 instances in parallel) | | | all 8 hubs load and play, no errors; jumps +213..+215 (ep2/3/5/6), ep1 / ep7 in their Binocucom briefing | | | same |

Notes:
- At 60 the port's game clock runs up to ~4% faster than real time in busy scenes: frames whose dt is under 1/60 (jitter
  of the EE thread) are clamped up (m4_hack briefing: 17.10 game s in 16.48 wall s; 438 of 888 frames exactly 1/60).
  Above 60 that gain is gone; frames over 1/30 s still lose time (as on a PS2).
- The frame counter 0x2BE538 is used only for "a 2D scene state started less than 2 frames ago" checks: at 165 fps that
  is 12 ms instead of 33 ms (no visible effect found).
- Not covered by these runs (need hands-on play): boss fights, the hack / turret / tank minigames while played, dance with
  input, every job's cutscenes. No frame-rate-dependent code is known there, but the RC chopper shows such code exists.

Gates run on the final state: tests\run_tests.py 16/16, every tests\unit suite (pacing now also builds test_frame_rate.c:
19 checks; menu 229), gpu_diff_all mixed S=1 28/28, pc_text_boot (also with PS2_FRAME_RATE=165), debug + release builds,
scripts\package.py.

## How fast it actually runs (i7-14700F, RTX 4060 Ti, 2560 x 1440)

The frame rate above 60 is limited by the EE side, not the setting: per frame the game's DMA chain runs VIF1 unpacking,
the VU1 microprograms and the GIF on the EE thread (profile of the ep2 hub at 120: recompiled game code 5%, VU1 microcode
17%, gs_write_reg / gif_feed / feed / upk_write 37% self, waits in ntdll the rest), then the GS thread, the GPU frontend and
the GPU each need 3-11 ms per frame at 3x.
- Intro cutscene (headless, alone): 117-121 fps at 120; real monitor, frame_rate = display (165): median 162 fps at 1x,
  67-119 fps at 3x (GPU / frontend bound).
- ep2 hub (headless, alone): 111-113 fps where the view is light, 62-77 fps in heavy views, peaks 139 (unlimited).
- So 120 is mostly reached in light scenes, 144 / 165 / 200 only in the lightest; the game's speed is right at any rate
  (dt from real time), but the frame rate varies with the scene. On a fixed-refresh display a varying frame rate judders
  more than a steady 60; with variable refresh it is smooth.

## VRR (G-Sync / FreeSync) on the owner's PC: why it did not engage

Measured 2026-10-23 (read only, nothing changed):
- NVIDIA driver profile (NVAPI DRS, global + base): "Enable G-SYNC globally" (0x1194F158) = 2 = **windowed and full screen**,
  "VRR requested state" (0x1094F1F7) = 2, "G-SYNC" app override (0x10A879CF) = 0 (allow); no profile for sly2.exe.
- **NvAPI_Disp_GetVRRInfo for the DELL S2722DGM (DisplayPort; EDID: FreeSync range 48-165 Hz): enabled 0, possible 0,
  requested 0, in VRR mode 0** - the driver has variable refresh off for this display.
- scripts/vrr_probe.c (a minimal D3D11 flip-model app like ours, presents every 10 ms): borderless fullscreen in front
  (DXGI composition "overlay" = independent flip), with and without tearing, and windowed (composed by DWM): every present
  shown on the 6.06 ms refresh grid (6.06 / 12.12 ms) = **fixed 165 Hz for any program**.
So it is not our swap chain (FLIP_DISCARD, ALLOW_TEARING, independent flip, Present(1) - all VRR-compatible). The display
is not in VRR mode at all: the monitor's own FreeSync / Adaptive-Sync switch and/or the per-display "Enable G-SYNC,
G-SYNC Compatible" tick in NVIDIA Control Panel is off (the global mode only matters once the display is enabled).

**What the owner should check**: (1) the monitor's OSD (Menu > Display): AMD FreeSync Premium / Adaptive-Sync = On (on
many monitors motion-blur reduction / MPRT modes switch it off); (2) NVIDIA Control Panel > Display > Set up G-SYNC: tick
"Enable G-SYNC, G-SYNC Compatible", "Enable for windowed and full screen mode", select the S2722DGM, tick "Enable settings
for the selected display model" if shown, Apply; (3) optionally Display > G-SYNC Compatible Indicator on, to see it engage.
The game's log then says "VRR (at start): ... this display: variable refresh enabled" and, after a few seconds in front,
"VRR: ACTIVE".

**What the game logs now** (gpu_vrr.c, gpu_present.c): at start exactly what the driver reports ("VRR (at start):
NvAPI_Disp_GetVRRInfo: enabled 0, possible 0, requested 0, indicator 0, in VRR mode 0 (flags 0x0); driver profile (global):
G-SYNC mode 2 (windowed and full screen), Monitor Technology 0 (G-SYNC allowed) -> variable refresh not enabled for this display
(the driver says it is not possible)") and what to change; every display verdict from the DXGI frame
statistics as "VRR: ACTIVE ..." / "VRR: NOT ACTIVE - the display refreshes at a fixed X Hz ... presentation <composed |
independent flip>, game window <in front | NOT in front>"; and "pace: no frame statistics for N s ... the game window is
not in front" when Windows stops reporting (measured: statistics stop while another window has the focus - every
agent-launched run, which cannot take the foreground; the earlier "statistics stopped after 2 s" was this).
The PC Settings Display page shows it read-only: **Variable refresh: Active / Off in the driver / monitor / Off in the driver
(G-SYNC not enabled) / Not active (fixed 165 Hz) / Not needed (60 Hz display) / Unknown** (MENU_ROW_VRR, a MENU_ITEM_INFO row;
gpu_vrr_status, updated at start and every 2 s from the frame statistics), with a note on what to turn on.
DXGI exclusive fullscreen (SetFullscreenState) is refused for swap chains with the frame-latency waitable object; the
game's "exclusive" mode stays the borderless popup + display mode switch (display_mode.c).

## Recommended defaults

- frame_rate = **60** stays the default: the game's look and timing are the PS2's, the GPU budget is what the presets
  were validated for, and on the owner's display without VRR a steady 60 judders less than a varying 70-160.
- With VRR enabled on the display: 120 or "display" is smooth (the limiter's even cadence, VRR shows each frame as it
  comes), with the resolution the GPU can hold (Balanced 3x: ~100 fps at 1440p; Performance 2x for 144+).
- Without VRR on a 165 Hz display: 60 (or "display" for the lowest latency, accepting the variation).
