# Progress log

## Resume here (read first in a new session)

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
