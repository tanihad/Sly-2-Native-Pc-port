# Progress log

## Resume here (read first in a new session)

**2026-10-23: pc-portable MERGED into main (ca6ee02):** fresh clone -> setup.ps1 -> own .iso in orig\disc -> build.ps1 -> dist\Sly2\sly2.exe (tested twice from clean clones, offline via -From); any x86-64 CPU, Windows 10 1809+, system DLLs only; README rewritten. Follow-up queued: GPUs without double-precision shaders (Intel Iris Xe / Arc) fall back to the software renderer - emulate doubles in the shaders (renderer task) after the current streams. Still running: pc-pacing, pc-menu, pc-input, pc-text.

**PAUSED by the user (2026-10-22, shortly after starting the PC-experience streams).** Nothing is running. All five worktrees (pc-pacing, pc-menu, pc-input, pc-text, pc-portable; branches next/pc-*) are committed and pushed; uncommitted edits were saved as 'PAUSED ... unverified' WIP commits (pc-input 8 files, pc-text 4, pc-portable 1). pc-menu had only read code and started a baseline build (no commits yet). On 'continue': resume each agent from its notes/wip/<stream>.md (they are told to run headless; the owner did not like game windows popping up while using the PC).

**2026-10-22 (later): OWNER REJECTED the PS2-style experience** (see notes/pc-experience.md). Four agents running from main e66b3a5, each with notes/wip/<stream>.md checkpoints: pc-pacing (game slows down at high internal scale + judder on the owner's 2560x1440 165 Hz monitor; auto scale dropped 3x->2x in their session), pc-menu (in-game Options menu + graphics presets Original/Performance/Balanced/Quality/Custom + settings model), pc-input (DualSense/DS4 HID USB+BT, rumble, remapping, menu input API; owner uses a PS5 controller, none connected during development - real-hardware check pending with the owner), pc-text (no PS2 memory-card prompts/wording). Merge all four into main when done, then rebuild dist and have the owner test (incl. DualSense).

**2026-10-22: THE GPU RENDERER IS COMPLETE AND THE DEFAULT (M0-M11 merged into main).** The game
starts on the Direct3D 11 renderer with internal_scale = auto (3x on the RTX 4060 Ti), 16:9 native,
ps2 outlines, Modern preset; software renderer selectable ([graphics] renderer = software) and
the automatic fallback when the GPU path cannot start (one log line). GPUs without ROVs run an
exact no-ROV path (slower). Docs: notes/gpu-renderer.md (as-built map), notes/d3d11-renderer.md
(design + appendices A-M), notes/settings.md, notes/controls.md, notes/install.md.
Final M11 results: all gates green on 28 recordings (bit-exact at S=1; lockstep 28343 comparisons
0 differ); 55-scene in-game sweep 13,260 verifier comparisons, 3 beyond tolerance (dual-source
deep-layering drift, owner question (a)); 200 s default regression 59.9-60 flips/s, memory flat
555-618 MB; 22-minute long session through every episode: no growth, 0 underruns. M11 also fixed
an M4-era feedback-copy staleness bug (gpu_own.c; ep6 RC) and made tests/unit/iop_kernel robust.
Open owner questions: (a) accept the gas-cloud +-3..16 colour drift under deep translucent
layering (exact costs 4-5x GPU); (b) Faithful preset keeping distant hub scenery PS2-blocky
(needs a Sly-specific rule). Possible follow-ups: faster no-ROV path (layering instead of one draw
per overlapping primitive); splitting the repo into a reusable PS2 recomp core + per-game folders
(for Sly 3), which the owner deferred until Sly 2 is finished.

**2026-10-22: THE GPU RENDERER IS COMPLETE AND THE DEFAULT (M0-M11 merged into main).** The game
starts on the Direct3D 11 renderer with internal_scale = auto (3x on the RTX 4060 Ti), 16:9 native,
ps2 outlines, Modern preset; software renderer selectable ([graphics] renderer = software) and
the automatic fallback when the GPU path cannot start (one log line). GPUs without ROVs run an
exact no-ROV path (slower). Docs: notes/gpu-renderer.md (as-built map), notes/d3d11-renderer.md
(design + appendices A-M), notes/settings.md, notes/controls.md, notes/install.md.
Final M11 results: all gates green on 28 recordings (bit-exact at S=1; lockstep 28343 comparisons
0 differ); 55-scene in-game sweep 13,260 verifier comparisons, 3 beyond tolerance (dual-source
deep-layering drift, owner question (a)); 200 s default regression 59.9-60 flips/s, memory flat
555-618 MB; 22-minute long session through every episode: no growth, 0 underruns. M11 also fixed
an M4-era feedback-copy staleness bug (gpu_own.c; ep6 RC) and made tests/unit/iop_kernel robust.
Open owner questions: (a) accept the gas-cloud +-3..16 colour drift under deep translucent
layering (exact costs 4-5x GPU); (b) Faithful preset keeping distant hub scenery PS2-blocky
(needs a Sly-specific rule). Possible follow-ups: faster no-ROV path (layering instead of one draw
per overlapping primitive); splitting the repo into a reusable PS2 recomp core + per-game folders
(for Sly 3), which the owner deferred until Sly 2 is finished.

**2026-10-19: M9 COMPLETE, merged into next/gpu and main (7f661f4): at 3x 16:9 in game GPU 7.3-10.9 ms (was 10-16), frontend -30%, GS thread <= 6 ms except ep3 hub 6.07, peak memory 1.6 GB -> 0.8 GB, adaptive frames in flight (2, up to 6 after slowdowns), latency queue->vsync 14 ms median; one recorded exactness trade (palette-bounded source alpha: affected primitives join the +-1 FF blend class, PS2_GPU_NO_TEX_ALPHA=1 reverts). A stray M9 experiment (conservative depth in runtime/gpu/shaders/gs.hlsl) had been written into the main checkout: saved to the scratchpad and discarded. M11 (default switch, auto internal scale, no-ROV path, long-session memory, flaky iop_kernel test, docs) running in worktree gpu-M11 (branch gpu/M11).** Earlier: **PAUSED by the user (2026-10-18).** Nothing is running. M9 (worktree gpu-M9, branch gpu/M9, pushed) was stopped mid-edit again (memory stats line; last commit unverified): verify it first, then continue from notes/wip/gpu-M9.md, then M11. **2026-10-17 (later): M8 (54-scene sweep: 0 divergence, 0 PUNT) and M10 (native widescreen on the GPU) COMPLETE, merged into next/gpu and main (405ce80); all tests pass; dist rebuilt. M9 (performance) running in worktree gpu-M9 (branch gpu/M9). Open owner questions: accept the ep5 tank gas-cloud +-3..5 colour drift (proposed: accept); distant hub scenery sharper at 3x (proposed: sharp in Modern, PS2-blocky in Faithful). Then M11.** **2026-10-17: M7 COMPLETE, merged into next/gpu and main (2207bb4): 3x default, outlines ps2|thin, Modern/Faithful + toggles, hotkeys F2-F6 (GPU renderer only; software stays default). Disk was full (0.5 GB): deleted 245 GB of regenerable frame dumps/exes/WAVs/RAM dumps from worktree build dirs (corpus .gsrec, screenshots kept); every agent prompt now carries disk-hygiene rules. Running in parallel: M8 (episode sweep, worktree gpu-M8) and M10 (widescreen GPU side, worktree gpu-M10). Then M9 (performance: ep3/ep5 frontend 11-15 ms and GPU 9.5-10.6 ms at 3x over budget), M11. Flaky: tests/unit/iop_kernel failed once under heavy load, passes 3/3 on rerun - make it robust in M11.** Earlier: **PAUSED by the user (2026-10-16).** Nothing is running. M7 (worktree gpu-M7, branch gpu/M7, pushed) was stopped mid-edit (latency measurement; last commit unverified): verify it first, then continue from notes/wip/gpu-M7.md, then M8..M11. **2026-10-16: M6 COMPLETE; next/gpu (M0-M6) MERGED INTO MAIN: GPU renderer opt-in via sly2.ini [graphics] renderer = gpu (or PS2_GS_BACKEND=hw); software stays the default. M7 (3x scaling + outline/filter settings) running in worktree gpu-M7 (branch gpu/M7). For M9: on main in a loaded run (M7 agent busy) the museum had frontend thread 17-21 ms per game frame vs 4-6 ms M6 measured in hubs — re-measure on a quiet machine. Also M6: up to 6 frames in flight (latency) and ~850 MB committed in hw mode.** **2026-10-16: M5 COMPLETE and merged into next/gpu (ee7e27b): GPU 3-6 ms per game frame at S=1 (REF 8-15). M6 (in-game integration, opt-in PS2_GS_BACKEND=hw) running in worktree gpu-M6 (branch gpu/M6, notes/wip/gpu-M6.md).** Earlier: **PAUSED by the user (2026-10-15 11:15).** Nothing is running. M5 (worktree gpu-M5, branch gpu/M5, pushed) was stopped mid-edit (gs_replay options/config wiring; last commit unverified): verify it first, then continue from notes/wip/gpu-M5.md, then M6..M11. **2026-10-15: M3 and M4 COMPLETE and merged into next/gpu (06b9bc5; M4: lockstep 24/24 recordings exact on hw + WARP). M5 (FF fast path) running in worktree gpu-M5 (branch gpu/M5, notes/wip/gpu-M5.md).** Earlier: **PAUSED by the user (2026-10-14, 22:25).** Nothing is running. Unfinished: GPU M3 in worktree `sly2-worktrees\gpu-M3` (branch gpu/M3, pushed). Its last commit 2200666 holds an unverified mid-edit (a PABE shortcut fix in the frontend: A==B, D==Cd is not 'colour unchanged' when PABE is on and As < 0x80). Verify that first, then continue from notes/wip/gpu-M3.md. On "continue": finish M3, then M4..M11 (merge each into next/gpu from `sly2-worktrees\gpu-integ`), sending the owner short progress updates.

**GPU: M2 = GO, merged; main merged into next/gpu (0f3fa1b). M3 (exact REF tier at S=1) running in worktree gpu-M3 (branch gpu/M3, notes/wip/gpu-M3.md).** Earlier: M2 resumed in gpu-M2. HUD edge
anchoring (hud = edges|centered), live F7 aspect / F8 HUD switch and the Binocucom side fill are
MERGED into main (next/hud; notes/widescreen.md 'HUD anchors'). Then M3..M11 one agent per milestone.

**PAUSED by the user (2026-10-14).** Nothing is running. Only unfinished task: GPU milestone M2
(worktree `sly2-worktrees\gpu-M2`, branch gpu/M2, pushed). Done there: baseline refresh (b5a6253),
render-thread executor of the frontend stream + gs_replay -Backend hwsubmit + bench script
(080ca43); the last commit fb62a15 is an unverified mid-edit (compact prim/state record rewrite):
verify or finish it first, then continue from notes/wip/gpu-M2.md. On "continue" the owner wants
EVERYTHING finished: M2..M11 (notes/d3d11-renderer.md section 11), merging each milestone into
next/gpu from `sly2-worktrees\gpu-integ`, then the GPU-dependent settings, HUD edge anchoring and
the open items below.

**PC packaging merged into main (2026-10-14):** `python scripts\package.py` -> dist\Sly2\sly2.exe
(GUI, icon, version info; reads SCUS_973.16 straight from the validated disc image; first run asks
for the .iso; data in %LOCALAPPDATA%\Sly2 or next to the exe with portable.txt; window state
remembered). Dev/console builds and all scripts behave as before. notes/install.md.

**Widescreen merged into main (2026-10-14):** 16:9 by default (sly2.ini `[display] aspect`,
PS2_ASPECT override; notes/widescreen.md, notes/settings.md). Instruction patches
(recomp/patches.py) widen the camera aspect/frustum and keep screen-space projections 4:3; HUD and
2D get GS register 0x7F side-band hints (runtime/gs_hint.h) that the sw renderer applies at kick;
movies are pillarboxed. 4:3 is pixel-identical to before. Regression: 60.1 VBlanks/s avg, 0
underruns. Limits: HUD is centre-anchored (sits in the 4:3 safe area), aspect read at start-up.
The `[graphics]` settings (internal_scale, outlines, filters + toggles) are parsed but only take
effect with the GPU renderer. The GPU renderer's M10 must implement the same hint contract.

**GPU renderer status (2026-10-14, later):** M0, C, M1 merged into next/gpu (coordinator worktree
`sly2-worktrees\gpu-integ`), and main merged into next/gpu (4b5b1da; window.c 'rec' token now -9).
M2 (go/no-go submission benchmark + baseline refresh for the PCRTC fix) running in worktree
gpu-M2 (branch gpu/M2; checkpoint notes/wip/gpu-M2.md). Widescreen (EE patch + sw HUD hints +
settings file) running in worktree `widescreen` (branch next/widescreen; notes/wip/widescreen.md).

**Main 60f11e6 (2026-10-14): whole-game sweep COMPLETE and merged.** Prologue and Episodes 1-8 were
each played end to end by a tester (every job started, completion flows + autosaves checked,
every episode transition through the Episode 8 credits). Merged: next/integrate (all ep branches,
VU review fix 11a6c10, IOP on its own host thread with MMCSS + mixer hold PS2_AUDIO_IOP_LIMIT,
async host logging, PCRTC field-mode fix, pixel-worker start-race fix, sd.c noise-voice fix, debug
tools PS2_TASK_SET/FILE, PS2_POKE variants, live 'pause'/'release'/'calls'/'watch' tokens).
Regression on main (release, quiet-ish machine): boot -> museum 200 s, 60.0 VBlanks/s average
(min 58), 0 underruns, 153 MB committed flat; all 17 unit suites + run_tests pass.
Open (from notes/wip/integrate.md on next/integrate): one 5.07 s EE stall seen once under load at
the briefing->museum load (not reproduced); music-stop-under-heavy-load fix merged but not
re-verified with ~10 games running; ep6 moose "polygon shards" when the camera is inside the
model (unverified, probably game-side); ep7 one-frame blue band at an RC camera cut (not
reproduced). Performance limit: the software GS thread (~16 ms/VBlank) -> the GPU renderer.

**GPU renderer (2026-10-14):** design finished: `notes/d3d11-renderer.md` on branch next/gpu
(= next/d3d11). Design 1 (GS draws -> D3D11 draws at an integer internal scale, exact integer depth,
ROV paths for what fixed-function blending can't express; software renderer stays the default and
the reference until M11). Milestones C (corpus extension, worktree gpu-C, branch gpu/C) and M0
(harness, render thread, all-fallback backend; worktree gpu-M0, branch gpu/M0) are running as
background agents with checkpoints notes/wip/gpu-C.md / gpu-M0.md. UPDATE: M0 and C are complete
and merged into next/gpu (M0: 0 differing pixels on all 13 recordings, conformance exact on the
4060 Ti and WARP, both support ROV; C: 11 more recordings, 24 total in tests/gpu/corpus.txt, and
no GS readbacks anywhere in the game). M1 (frontend dry run) is running in worktree gpu-M1
(branch gpu/M1); merge it into next/gpu from the coordinator worktree `sly2-worktrees\gpu-integ`
when it passes. Then M2
(go/no-go benchmark, needs a quiet machine), M3... per section 11 of the design.
The sweep continuation (wf_1522fb31-777) was resumed again on 2026-10-14; ep4 is COMPLETE.

**RESUMED 2026-10-11 (after the pause below):** continuation workflow `whole-game-sweep-continue`
(run wf_1522fb31-777) runs only the unfinished testers ep1, ep2, ep3, ep4, ep7, then the VU
review, then integrate (finished reports passed in as args; full copies in
build\sweep-finished.json). The D3D11 design workflow resumed as wf_45705d77-396. Resume either
with its `resumeFromRunId`.

**PAUSED by the user on 2026-10-11** (to use the computer). Both workflows were stopped,
all agent game processes were killed, and every worktree is clean (unfinished edits committed as
"PAUSED ... unverified" WIP in ep3 and d3d11), and all next/* branches are pushed. Finished agents:
warp, vu-recomp, ep5, ep6, ep8 (results in the run journal of wf_1f8b6c38-938). Unfinished:
ep1, ep2, ep3, ep4, ep7 (continue from notes/wip/epN.md), then the VU review and the integrate
step; and the D3D11 design workflow (wf_45705d77-396; its understand agents had just started).
Note: on the last resume, ep5/ep6 started a new session ("session 6 start") although they had
finished, so resuming by run id may re-run finished testers. Better: a continuation script that
runs only the unfinished testers and passes the finished results from the journal to integrate.

**Main 81186bf (2026-10-11), episode testers: merge main into your branch.** It has the EE thread
fix from next/ep5 (runtime/threads.c): thread churn leaked ~1 MB/s of fiber stacks, and a restarted
thread resumed its old fiber (StartThread after a normal return spun forever; after ExitThread or
TerminateThread it resumed inside the old syscall). Hangs or "stuck" scripts that involve thread
restarts, and growing memory in long runs, may be this bug. ep5, ep6, ep8 are complete; the
workflow was resumed again (ep1-4, ep7, VU review, integrate pending).

**Direct3D 11 renderer design (2026-10-11), running in parallel:** workflow
`d3d11-renderer-design` (run wf_45705d77-396; worktree `sly2-worktrees\d3d11`, branch
next/d3d11): renderer map + census of Sly 2's GS usage over the .gsrec corpus + prior art, then
3 competing designs (high-level translation / GPU-exact compute / Sly-specialised hybrid), a
3-lens judge panel, and the final design in `notes/d3d11-renderer.md` on next/d3d11. Each agent
checkpoints to `notes/wip/d3d11-*.md` there. Resume: `Workflow({scriptPath: <session>\workflows\
scripts\d3d11-renderer-design-wf_45705d77-396.js, resumeFromRunId: "wf_45705d77-396"})`.

**Main 125390a (2026-10-11):** the VU microcode recompiler and the warp tool's release-build
direct starts are merged. Release regression: 60 fps, VU 100% recompiled (2.2 ns/pair), EE thread
7.7 ms/frame (was ~14), GS thread ~16.4 ms/frame (now the busiest: the game draws more detail when
it has time; the Direct3D 11 renderer is the fix), audio 0 underruns. Still unmerged: next/ep1..8
(episode testers; ep7 verified the Episode 7 -> 8 transition; ep6 investigates Neyla chopper
stages ending in "Job Failed"); the VU review and the integrate step are pending in the
resumed workflow.

**RESUMED on 2026-10-11:** workflow `whole-game-sweep` (run wf_1f8b6c38-938) is running again:
episode testers continue from their checkpoints, then the VU recompiler review and the integrate
step. The state below is the pause point.

**PAUSED by the user on 2026-10-11** (to use the computer). Nothing is running. Main (78c51f8+)
has everything up to and including the episode warp tool and is playable (`scripts\play.ps1`).
Unmerged work, all committed and pushed to GitHub, each with `notes/wip/<task>.md` checkpoints:
- `next/vu-recomp` (worktree `sly2-worktrees\vu-recomp`): VU microcode recompiler, COMPLETE per its
  checkpoint (exact vs the interpreter on recordings + fuzzing). Its independent review was
  interrupted; run the review (prompt in the `whole-game-sweep` workflow script), then merge.
- `next/ep1`..`next/ep8`: per-episode playtesting/fixes. Every job type tested starts in every
  episode; no runtime blockers reported. ep1/ep2/ep3 end with a "PAUSED mid-edit" commit holding
  unfinished, untested fixes (ep1: audio/IOP timing in sd.c/iop_kernel.c; ep2/ep3: GS thread /
  pixel workers) — finish and verify those before merging. Notable open finding (ep2): menu
  music stream can stop when the IOP runs late while the EE waits for the GS.
- To resume: same session -> `Workflow({scriptPath: <whole-game-sweep script>, resumeFromRunId:
  "wf_1f8b6c38-938"})` (finished agents replay from cache; the others continue from their
  checkpoints), then the `integrate` step merges everything and runs the full regression.
  New session -> start one agent per unfinished branch with its checkpoint, then integrate.
Next after that: Direct3D 11 renderer (resolution scaling), widescreen, 60 Hz-synced pacing.

**warp (2026-10-10):** start the game at any episode or job without playing up to it:
`scripts\warp_run.ps1 -Episode N [-Job m3_heist] [-Direct] -Play [-Release]`, or
`scripts\warp.py make --episode N` / `--job <job>` then `scripts\play.ps1`. Memory-card saves built
from the reverse-engineered progress blob (schema + task graph read from the game), plus a
debug-only hook that starts a job directly (runtime/warp.c, PS2_WARP_TASK). All 9 episode starts
and 10 sampled jobs verified, no runtime blockers found. Guide and format: `notes/warp.md`.
**next/vu-recomp (2026-10-10):** the game's VU microcode is statically recompiled to C at build
time (`recomp/vu_recomp.py`: all 8 overlays of `.vutext`, the VU1 renderers and the VU0 library);
`runtime/vu.c` runs the compiled program whenever micro memory holds a compiled image, the
interpreter otherwise (logged once; `PS2_VU_INTERP=1` forces it). Exact against the interpreter
(registers, flags, Q/P, data memory, XGKICK addresses and cycles, pair/cycle counts): recordings of
intro, title, briefing and museum gameplay replay with 0 differences, in-game `PS2_VU_VERIFY`
found 0 mismatches in 15.7M programs, and `tests\unit\vu_recomp` runs random programs differentially.
VU cost per pair 4-7x lower; the VU share of the EE thread drops from ~60% to ~10-15%. Numbers:
`notes/performance.md`; design: `notes/vu-microcode.md` (Recompiler).

**fix/fix-perf (2026-10-10):** the release build (and the -O1 default build) runs at full speed,
59.94 VBlanks/s and ~60 game flips/s, on the movie, memory-card prompt, intro cutscene, title and
Binocucom briefing (main before: 22-32 VBlanks/s in the 3D scenes). GS on its own thread with
deferred band-parallel pixel workers, direct texture sampling from VRAM, EcoQoS opt-out, timer
pacing. Headroom 2-6 ms per VBlank on the EE thread (the intro cutscene is the tightest: 2 ms) and
3-6 ms on the GS thread. The EE thread is now ~60% VU interpreter: the VU recompiler is the next
big step. Tools and numbers: `notes/performance.md` (PS2_PERF, PS2_GS_STATS, profilers, headroom,
GS and VU record/replay).

**Newest (main c8cc6ef):** perf, gameplay and native audio are merged. The release build runs at
60 fps with sound from boot to gameplay in the Cairo Museum prologue (`scripts\play.ps1` builds
and launches it). Workflow `whole-game-sweep` is running: `next/warp` (episode warp tool),
`next/vu-recomp` (static VU microcode recompiler), `next/ep1`..`next/ep8` (one playtest/fix agent
per episode, after the warp tool), then `next/integrate` (merge all + full regression). Worktrees
under `C:\Users\tanih\Projects\sly2-worktrees\`; each keeps `notes/wip/<task>.md`.

**Previous:** main (eb96e20) reaches the title screen and the Bentley/Sly Binocucom briefing
(SQUARE answers "Yes" on the memory-card prompt: PS2_INPUT="600:square:12,1500:start:10").
fix-render and fix-input are merged. Workflow `continue-to-playable` is running:
`fix/fix-perf` (60/60 fps in release on menus already; merging main, continuing its checkpoint),
`audio/audio-iop` (recompiled 989snd/spiop + native mixer; finishing integration),
`play/play-1` (pushing into gameplay). Each has `notes/wip/<task>.md`; resume from those.

**Earlier (2026-10-10, later):** all `hw/*` branches are merged into main. The game boots, plays
the intro movie, shows the mask logo and renders the memory-card prompt ("Do you want to create
a Sly 2 Save File?"), where it currently stops reacting to input (presses do reach the game's
joystick object). Running in parallel, each in a worktree under
`C:\Users\tanih\Projects\sly2-worktrees\`:
- workflow `playtest-fixes`: `fix/fix-input` (why the prompt ignores input, then push further
  into the game), `fix/fix-perf` (game runs ~77% speed; profiler + optimisations),
  `fix/fix-render` (grey button glyph, accuracy).
- workflow `native-audio`: `audio/audio-iop` (recompile 989snd/spiop + native IOP libraries) and
  `audio/audio-mixer` (native libsd mixer + WASAPI, interface `runtime/sd.h`), then integration
  on `audio/audio-iop`.
If interrupted: each branch has commits and `notes/wip/<task>.md` checkpoints; restart an agent in
that worktree with the task text from the workflow scripts plus the checkpoint protocol.
Debug tools: PS2_INPUT (scripted presses), PS2_PEEK (memory), PS2_LOG_PAD/SIF/GS, frame dumps.

Earlier state on 2026-10-10:
- `main`: recompiler + runtime boot the game to its intro movie; hardware layer architecture with
  frozen interfaces (`runtime/hw.h`, `runtime/gs_internal.h`) and stubs for every subsystem.
- Design rules: `notes/native-port-principles.md` (native port, not an emulator),
  `notes/hle-architecture.md` (file ownership, build/run from worktrees),
  `notes/audio-native-plan.md` (audio: recompile 989snd/spiop, native sceSd mixer + WASAPI).
- Subsystem implementations were built in parallel on branches `hw/<name>`, one git worktree each
  under `C:\Users\tanih\Projects\sly2-worktrees\<name>` (also pushed to GitHub):
  gs-mem, gs-draw, gif, dma, vif, vu, ipu, timer, window, pad, mc, audio (research notes only).
  Each has unit tests in `tests/unit/<name>/run.ps1`. Check each branch's state with
  `git -C <worktree> log main..HEAD` and `git -C <worktree> status` (an agent may have stopped
  with uncommitted work).

If the hardware workflow (`ps2-hardware-layer`) was interrupted: do NOT rerun it from the top.
For each `hw/<name>` worktree whose work is unfinished (no review commits, or uncommitted files),
start one agent in that worktree with the original requirements (in the session's workflow script
`ps2-hardware-layer-*.js`) plus the checkpoint protocol (`notes/agent-checkpoints.md`): it reads the
branch's commits and any `notes/wip/<name>.md`, finishes implementation, then the adversarial
review. Same session: `Workflow({scriptPath, resumeFromRunId})` returns finished agents instantly.

Next steps, in order:
1. Merge the `hw/*` branches into main (file ownership means few conflicts), apply the
   integrator requests the agents listed (e.g. extra link libraries for window.c's D3D11), build,
   run `scripts\run_game.ps1 -Seconds 20 -Dump build\frames`, look at the frames.
2. Phase 2 (native pieces): Direct3D 11 renderer replacing gs-draw's software rasterizer; static
   recompilation of the VU microcode (the vu branch's interpreter becomes the reference);
   native audio per `notes/audio-native-plan.md`.
3. Play-test and fix until the game is playable start to finish; then performance and polish
   (resolution scaling, widescreen).

## Target build

- Game: Sly 2: Band of Thieves, NTSC-U, `SCUS_973.16`, VER 1.00 (SYSTEM.CNF)
- Executable SHA-1: `de2af92a54de71e33edc8a160c31c0337a95e6f5`
- Disc filesystem holds only SYSTEM.CNF, SCUS_973.16, IOPRP300.IMG, Z6TAIL.DUP (32 MB padding).
  The ~2.8 GB of game data is outside the filesystem, so the game reads it by raw sector (LBA).

## Executable layout

| Section   | Range                   | Notes                                                    |
|-----------|-------------------------|----------------------------------------------------------|
| .text     | 0x00100000-0x00259DF0   | EE code, recompiled                                      |
| .vutext   | 0x00259DF0-0x00262330   | VU microcode, different ISA, not EE code                 |
| .rodata   | 0x00262380-0x002AC864   | includes switch jump tables                              |
| .data     | 0x002AC880-0x002DD230   | vtables, init/ctor tables, libkernel patch blobs         |
| .bss      | 0x002DD380-0x00452200   |                                                          |
| .irx      | 0x00472200-0x004E5E40   | embedded IOP modules (second segment)                    |

No symbol table.

## Recompiler status (2026-10-10)

- ~6,900 functions found; 371k EE instructions, all but 14 translated (12 `vcallms`, 2 `eret`).
- VU0 macro mode and the MMI instructions the game uses are implemented and tested.
- Function discovery: jal/j targets, code constants, data pointers (vtables, ctor tables),
  after-`jr $ra` boundaries, with switch jump tables protected and a merge pass for bad guesses.

## Boot progress (what runs before it stops)

1. crt0: SetupThread, SetupHeap, FlushCache.
2. libkernel kernel patches: SetSyscall + FindAddress scan of kernel RAM to locate the syscall
   table (runtime emulates a real table at phys 0x10000), alarm patch copied from .data to
   0x80076000 and a second patch at 0x80075000 (not recompiled; runtime HLE stands in).
3. Timer 2 setup, AddIntcHandler/EnableIntc, CreateThread, ChangeThreadPriority,
   GetOsdConfigParam, CreateSema.
4. All C++ static constructors and init tables.
5. SIF setup: AddDmacHandler(5 = SIF0), EnableDmac, polls SifGetReg(4) for IOP boot.
6. SIF RPC init (sreg 0 handshake), then CD/DVD init. Game halts unless disc type == 0x14
   (PS2 DVD); runtime answers from the ISO.
7. IOP reboot with IOPRP300.IMG, LOADFILE version check ("3000"), modules loaded:
   sio2man, padman, mcman, mcserv, Sound_Device_Library (libsd), USB_driver, lgAud,
   989snd_Library, spiop (Sucker Punch).
8. GS reset + display mode (sceGsResetGraph, NTSC interlaced), pad, memory card, fileio and
   989snd servers bound. 989snd batches need 0xFFFFFFFF sentinels around results.
9. Worker thread (f_00160B70) started; requires real threads (fibers in runtime/threads.c).
10. VSync interrupt at 59.94 Hz, serviced at loop heads.
11. CD streaming starts (lbn ~959782, 128-sector chunks, double-buffered at 0x01BE0400 /
    0x01C20400). **Now at the intro movie: sceMpeg drives the IPU (0x10002000) and waits.**

## IOP services (runtime stand-ins)

| SID        | Service                     | Status                                  |
|------------|-----------------------------|-----------------------------------------|
| 0x80000592 | CD init                     | ok                                      |
| 0x80000593 | CD S-commands               | fn 3 disc type, fn 4 error; fn 34 TODO  |
| 0x80000595 | CD N-commands               | fn 1 read (from ISO), 5-8, 14 ready     |
| 0x8000059C | CD (callback?)              | zeros                                   |
| 0x80000003 | IOP heap                    | alloc/free                              |
| 0x80000006 | LOADFILE                    | version, module loads (logged)          |
| 0x80000100/101 | padman                  | zeros (no controller yet)               |
| 0x80000400 | mcserv (memory card)        | zeros                                   |
| 0x80000001 | fileio                      | zeros                                   |
| 0x00123456/57 | 989snd                   | sentinel replies, silent                |

## Bugs found and fixed

- RAM fast-path mask `0xCFFFFFFF` also matched 0x1xxxxxxx, so all hardware registers
  (DMA, IPU, VIF, INTC, VU memory) silently read/wrote RAM. Covered by a test now.

## Blockers

- (resolved) Windows Smart App Control blocked fresh builds; user turned it off.

## Open questions

- [ ] Which IOP modules does the game load (from .irx and IOPRP300.IMG), and what RPCs does it use?
- [ ] How does the game locate its data on disc (sector table in the executable?)

## Matched functions (decomp work, paused)

| Function | Address | Source file | Date |
|----------|---------|-------------|------|
