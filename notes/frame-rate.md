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

(That was before pc-fps2: the graphics worker below takes the frame's graphics preparation off the EE thread.)

## High frame rate: approaches compared (pc-fps2)

Question: (a) pipeline / parallelise the per-frame graphics preparation (VIF1 unpack + VU1 microcode + GIF on the EE
thread) or (b) interpolate frames between 60 Hz game ticks (RT64 / Zelda 64 Recompiled style: matrices of two game frames,
in-between pictures by re-running the frame's VU1 work with interpolated matrices). Decided by measurement. Checkpoint
history: notes/wip/pc-fps2.md. Measured on the owner's PC (i7-14700F, RTX 4060 Ti), release build, headless, one game at
a time, 35 s windows of time-based scripted play (scripts/fps2_scene.ps1, fps2_batch.ps1, fps2_sum.py); ep1 hub = Sly
on the Paris roofs (save of job m1_brawl), ep3 / ep5 hubs after X, m8 boss and m3 heist as direct jobs.

### Step 1: profile (before, 3x, frame_rate 120)

| ep1 hub, per game frame | ms | |
|---|---|---|
| EE thread | 13.4 busy, 0% idle | recompiled game code 12%, GIF + GS queue (gs_write_reg) 33%, VU1 microcode 29%, VIF 24% |
| GS thread | 8.2 | decode, proxy records to the frontend (px_prim, reserve), waits |
| GPU frontend thread | 9.2 | fr_prim 34% self / 69% incl., own_draw, refresh |
| render thread CPU | 1.0 | |
| GPU at 3x | 11.5 | |

Hottest EE functions (PS2_PROFILE): the main VU1 renderer vuc_376110651 24% self, gs_write_reg 22% (a 16-byte queue slot per
GS register, ~10 ns each: ~450k registers per frame), upk_write 13% (VIF unpacks other than V4-32), gif_feed 11%, feed 9.5%;
the game's own code ~7%. Per frame in a hub: ~2.5M VU1 pairs in ~7500 microprograms, ~50k primitives. Per frame the render
thread (0x160B70) kicks one VIF1 chain (0x16F8B8 -> sceDmaSend, then WaitSema 0x2DFB50 until the GS FINISH interrupt),
one GIF chain, a 9-qword GIF setup transfer; the frame's DMA chain is double-buffered (0x2DEA50 + 0x50 x n, semaphores
0x2DEB18 / 0x2DEB1C). So the EE thread was the limit everywhere; the GS thread / frontend / GPU were next at 8-11 ms.

### Step 2: the prototypes

**(a) graphics worker + cheaper packets** (runtime/gfx_async.c/.h, gif_parse.h, vif.c):
- A1 *graphics worker*: a VIF1 source chain is handed to a host thread at its CHCR write; it runs exactly the code the EE
  ran (dma.c -> vif.c -> vu.c -> gif.c -> the GS queue). The worker owns DMA channel 1, VIF1, VU1, the GIF and the GS
  queue's producer side while a job is out; the EE waits for it (gfx_sync) before it touches any of them (VIF1 / GIF / VU1
  registers and memory, DMA ch1 and the global DMAC registers, GIF DMA, any GS queue write, VBlank pictures). Everything
  the worker causes that the EE can see - INTC (GS, VIF1), DMAC channel ends, GS SIGNAL / FINISH / LABEL - is deferred in
  order and applied on the EE when the job ends (ps2_service_irq -> gfx_poll), where the synchronous model raised it too.
  The game's render thread sleeps on its FINISH semaphore, so the main loop builds the next frame meanwhile (as on a PS2).
  A VIF stop (the i bit: Sly 2's VIF1 handler 0x167F78 reads VU1 results, then FBRST.STC) ends the job; the EE takes it
  from there and the next pump hands the rest to the worker again.
- A2 *raw PATH1 packets*: an XGKICK packet goes to the GS thread as one queue record (its qwords copied) and is parsed
  there (gif_parse.h: one parser source for gif.c and gs_thread.c) instead of a 16-byte slot per register. Not for
  packets with SIGNAL / FINISH / LABEL (EE timeline), nor while recording the GS stream or logging GIFtags. The GS
  register mirror for recordings is now kept where registers execute.
- VIF unpack fast paths: whole vectors of each format (V4-5, V1-8 masked, V2-16 with CL > WL skipping, V4-16, V4-8, ...)
  in loops specialised at compile time, no filling cycles (those keep the generic loop).

**(b) frame interpolation prototype** (runtime/interp.c, `PS2_INTERP=<k>` at frame_rate 60; patch interp_frame_done before
the render thread frees the frame's buffer, 0x160C68): the frame's VIF1 chain is walked for the matrices it uploads - the
camera (VU1 0x002 / 0x006 in the frame's first packet) and every object's world matrix (UNPACK V4-32 x 4 to VU1 0x000 in a
per-frame CNT packet), keyed by the static geometry the object CALLs next (+ occurrence; ~800 per frame in the ep1 hub, ~97%
matched); for t = j / (k + 1) the matrices are replaced in EE RAM by lerp(N-1, N, t), the frame's VIF1 and GIF chains run
again (no interrupts; the VIF i bit resumed by hand), and a picture of the buffer N draws into is queued (display snapshot
with the other DISPFB). Dumps of N-1 / in-between / N in the ep1 hub and the m8 boss show the camera and objects in between
(skinned characters, particles and 2D are not interpolated in the prototype).

### Results (fps = game frames per second, median of 35 s; p10 in brackets)

3x (the default Balanced preset):

| scene | before 120 | (a) 120 | before 165 | (a) 165 | (a): GPU ms at 3x |
|---|---|---|---|---|---|
| ep1 hub | 68.3 (64.8) | 86.7 (83.9) | 71.6 (66.6) | 87.0 (78.5) | 11.4 |
| ep3 hub | 98.0 (67.7) | 100.7 (81.1) | 99.2 (66.6) | 98.5 (80.5) | 9.6 |
| ep5 hub | 90.4 (79.5) | 101.9 (96.2) | 92.5 (78.5) | 102.0 (95.9) | 9.5 |
| m8 boss | 118.0 (95.6) | 118.2 (103.9) | 113.0 (95.8) | 126.7 (106.4) | 7.6 |
| m3 heist (light) | 119.2 | 119.2 | 145.8 | 143.6 | 6.8 |
| ep2 hub (fps_trace run, 165) | | | 74.8 | 95.6 | |

At 3x the GPU is now the limit (fps ~ 1000 / GPU ms). 2x (the Performance preset), where the CPU side shows:

| scene | before 120 | (a) 120 | before 165 | (a) 165 | (a) per frame: worker / GS thread / frontend / GPU ms |
|---|---|---|---|---|---|
| ep1 hub | 69.3 (64.0) | 96.2 (90.3) | 69.0 (66.6) | 97.0 (93.2) | 7.2 / 7.8 / 8.9 / 6.6 |
| ep3 hub | 117.5 (67.1) | 118.7 (81.3) | 120.6 (66.2) | 159.6 (80.7) | 5.4 / 3.5 / 4.1 / 5.2 |
| ep5 hub | 91.1 (77.9) | 118.4 (102.6) | 92.2 (79.3) | 124.0 (104.1) | 5.5 / 5.6 / 7.4 / 5.5 |
| m8 boss | 115.0 (96.8) | 118.7 (118.3) | 110.8 (96.0) | 145.1 (124.9) | 4.5 / 4.7 / 5.8 / 4.8 |
| m3 heist (light) | 119.5 | 119.0 | 163.6 | 163.5 | 1.2 / 1.1 / 1.5 / 4.2 |

With (a) the EE thread needs ~1.5-2 ms per frame (50-80% idle); the worker 1.2-7.2 ms (VU1 ~40%, VIF ~20% of it after the
unpack fast paths: 8.8 -> 7.2 ms in the ep1 hub); the GPU frontend (fr_prim) and the GS thread are the new limit in the
heaviest hub (~9 / ~8 ms: ~97 fps at 2x).

(b) at 2x, frame_rate 60, distinct pictures per second (game frames + in-between; VBlank pictures that repeat a frame not
counted), next to (a):

| scene | (a) 165 | (b) k = 1: game / distinct | (b) k = 2: game / distinct |
|---|---|---|---|
| ep1 hub | 97.0 | 40.3 / 80.7 | 30.0 / 89.9 |
| ep5 hub | 124.0 | 57.9 / 115.8 | 37.3 / 112.0 |
| m8 boss | 145.1 | 59.0 / 117.9 | (58.9 / 176.8: not comparable - at a 60 Hz tick the scripted fight ended with Sly falling, clouds only) |

An in-between picture costs what a frame costs: the re-run is the same VU1 work (470k pairs per job either way, m8 boss),
5.3-6.8 ms on the worker in the ep1 hub (a real frame's job: 6.2-7.0 ms), and the same GS thread / frontend / GPU work.
The game's code, the only part (b) saves, is ~2 ms of a frame and already runs beside the worker with (a).

### Decision: (a)

- (a) gives the higher real frame rate everywhere (ep1 hub 2x: 97 game frames/s against (b)'s 81-90 distinct pictures with
  only 30-40 game frames/s; ep5 124 against 112-116) and keeps the full game tick: every picture is a real game frame,
  input is read every frame (ep1 hub: a frame every 10.3 ms instead of a 25-33 ms tick under (b)), and nothing is guessed.
- (b) cannot be faster than (a): each in-between picture needs the whole per-picture pipeline (VU1 microcode on the CPU,
  GIF, GS decode, frontend, GPU) - unlike an N64 recompiler, where the GPU redoes the transforms for free. In heavy scenes
  the in-between pictures take the time the next game frame needs and the game's own tick drops below 60 (ep1 hub 60 ->
  30-40). It also adds latency (in-between pictures show the past: one tick behind), needs identity matching (instanced
  objects by occurrence, culling / sorting changes), leaves skinned characters, particles, EE-built geometry and the
  game's motion blur un-interpolated, and must re-run chains whose VIF interrupts the game handles.
- The owner's idea is not a win here; the one place it could still help is a scene that must stay at 60 (the RC chopper's
  controller, frame_rate.c keeps_60): with (a) the chopper keeps 60 game frames; interpolating those pictures would need
  the prototype's pacing and completeness problems solved first (left, below).

### What was implemented

(a), fully: the graphics worker (default above 60 fps; `PS2_GFX_ASYNC=0` never, `=1` also at 60), raw PATH1 packets
(`PS2_GIF_RAW=0` off) and the unpack fast paths (`PS2_VIF_UPK_FAST=0` off). At 60 the chain runs on the EE as before
(the GS stream is the synchronous model's exactly); raw packets and unpack fast paths are exact by construction and
checked in game. The prototype (b) stays in the tree for measurements (interp.c, only with PS2_INTERP). Tools:
PS2_PROFILE=front | render | worker (perf.c), the worker's split in the PS2_PERF line, PS2_GFX_ASYNC_LOG,
PS2_CHAIN_DUMP=<vblank>:<n>:<file> (chain_dump.c: a VIF1 chain's tags and VIF codes as text), PS2_WRITE_WATCH=<addr>
(gfx_async.c: who writes a 4 KB page of EE RAM).

### Correctness

- In-game checks over 16 scenes (scripts/fps2_check.ps1: hubs, turret, dance, RC chopper, hack, paraglide, train, battle,
  bosses ep1 / ep7 / ep8, the prologue; 120 and 165): `PS2_GIF_CHECK=1` (each raw packet's register stream hashed by
  gif.c's own parser and by the GS thread's) 0 differ in 10-20M packets per run; `PS2_VIF_UPK_FAST=check` (fast and generic
  unpack on copies, VU memory and VIF state compared) 0 differ in 14-40M unpack calls per run.
- `PS2_GFX_ASYNC_CHECK=<n>` (every n-th job: EE RAM copied at the kick, every byte the worker's DMA reads compared with the
  copy) found the game writing memory a kicked chain still reads in 5-14% of jobs: always the effect-object pool at
  0x1CBxxxx-0x1CDxxxx (per-object double buffers 0x5A0 / 0x840 apart), written by f_0015B428 (takes an effect object from
  a pool, 0x1390D8) and the main loop's updates (found with PS2_WRITE_WATCH). On a PS2 the DMA runs beside the EE the same
  way; with the worker such a frame shows a few effect objects one frame early. That is why the worker is used only above
  60 fps: at 60 the stream stays exactly the old one.
- Game speed / physics (scripts/fps_run.ps1 + fps_trace.py, ep2 hub, the same time-based input): run 338-339 u/s average at
  60 / 120 / 165 (339 before), jump +213.8 / +214.5 / +214.5 (peak after 0.44 / 0.43 / 0.44 s, air time 0.88 / 0.87 /
  0.87 s), camera turn 72.0 / 72.2 / 72.2 deg/s, every segment the same game seconds at every rate.
- Software renderer with the worker (PS2_GS_BACKEND=sw, 165): runs, pictures right, raw packets 0 differ.
- Gates: see notes/wip/pc-fps2.md (run on the final state).

### Left

- The heaviest hubs stop at ~97 fps (ep1) at 2x: the GPU frontend thread (fr_prim, ~9 ms per frame, ~99% busy in the
  profile; the GS thread waits on its ring) is the limit; then the GS thread (~8 ms) and the worker (VU1 microcode ~3 ms
  of 7). Measured without effect (ep1 hub 2x 165, A/B twice each, all within 94-99 fps): the frontend thread at above-
  normal priority, the process on the performance cores only (CPU sets), the five pipeline threads pinned to separate
  physical cores, two pictures in flight instead of one, a 32 MB GS queue instead of 4 MB. Offline the frontend needs
  5.9 ms per game frame of the gpu-C ep1_hub recording (gs_replay --front-thread), in game ~9. Next levers: split the
  frontend's per-primitive work (setup, culling, texel rects: pure functions of the primitive and its state) onto
  another thread, or make fr_prim cheaper; then parallelise VU1 batches.
- At 3x the GPU limits (ep1 hub 11.4 ms per frame): 120+ in heavy scenes needs 2x (the Performance preset).
- RC chopper at 60: interpolation would need paced in-between pictures (the prototype queues them back to back) and the
  frame's other chains / post effects re-run; or a fix of the chopper controller's frame-rate dependence itself.

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

## Display-locked cadence on fixed-refresh displays (pc-smooth)

On the owner's 165 Hz display without VRR, 59.94 frames/s cannot be shown evenly: pictures alternate 3 and 2 refreshes
(18.2 / 12.1 ms) and some presents are replaced before they are shown. Now (pacing auto, once the frame statistics say the
refresh is fixed; or pacing = display) the display's own VBlanks drive the game every k-th refresh at most 60 times a
second - 165 Hz: 55 frames/s, each picture exactly 3 refreshes (frame_rate_set_lock; d3d11-renderer.md appendix Q). The
game's speed is real time at any rate (its dt is 1/55 s, inside its own [1/60, 1/30] clamp), movies and RC chopper flights
keep 59.94. Measured on the owner's monitor: 1160 of 1160 pictures shown for 18.17 ms (before: 21% for 12.1 ms, 12% of the
presents never shown). The log says "[fps] frame rate locked to the display: 55.03 frames/s".
Settings above 60 are not locked (display VBlanks would present repeated pictures there): on a fixed 165 Hz display 120 /
165 still vary with the scene - turn VRR on (section "VRR" above), or use 60.
The governor keeps 3x at 120 / 165 now (warm-up >= 1 s, no immediate overload rule above 60): ep2 hub 98-99 frames/s.

## Recommended defaults (pc-smooth)

60 (default): smooth on every display - 59.94 on 60 / 120 / 240 Hz, 55 locked on 165 Hz without VRR, even with VRR.

## Defaults (pc-menu2, 2026-10-24)

The owner: "don't switch the monitor's refresh rate, just start at 60 fps; everyone's monitor has 60 Hz; this is meant for
other people too". The display-locked cadence is no longer automatic: `pacing = auto` (default) is a plain 60 (59.94) on every
display - on 60 / 120 / 240 Hz the display's VBlanks still drive the game (every picture the same number of refreshes, as
before), on 144 / 165 Hz without VRR pictures alternate 2 and 3 refreshes. The lock (55 fps on 165 Hz, every picture 3
refreshes) is the opt-in `pacing = display`, shown in Options > Display > Frame pacing as **Match display** ("the smoothest
without G-Sync / FreeSync"; gpu_hw.c `clock_lock_k`). The game starts borderless at the desktop's resolution and refresh
(notes/settings.md "Launch defaults"); no display mode is changed unless the player picks Fullscreen.
