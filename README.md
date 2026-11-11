# Sly 2: Band of Thieves - native PC port (unofficial)

**Website and download: https://tanihad.github.io/Sly-2-Native-Pc-port/**

A native Windows version of **Sly 2: Band of Thieves** (PS2, 2004), made from your own copy of the
game. It is not an emulator: the game's code is translated ahead of time ("statically recompiled")
from the PS2 executable into C and compiled into a normal 64-bit Windows program, and the PS2
hardware is replaced by PC equivalents - Direct3D 11 graphics (higher resolutions, widescreen),
WASAPI sound, controller/keyboard input, plain files for saves.

**This repository contains no game code and no game data.** You need your own disc of
Sly 2: Band of Thieves, **USA release (SCUS-97316)**, as an `.iso` image. The build reads the game's
executable from that image and recompiles it on your PC; the game then reads its data from the
image while you play.

## Install (easy way)

1. Get **`Sly2-Setup.exe`** (about 1 MB) and start it. No administrator rights are needed.
   (Windows may say "Windows protected your PC": the setup program is not signed. Click
   **More info**, then **Run anyway**.)
2. Confirm that you have your own copy of the game, pick the install folder (the suggestion,
   `%LOCALAPPDATA%\Programs\Sly2`, is fine) and choose your disc image (`.iso`). Setup checks the image
   right away and tells you if it is the wrong game, region or version.
3. Click **Install**. Setup downloads the free build tools it needs (about 220 MB: the llvm-mingw
   compiler and Python with two packages, each file checked against its published SHA-256), builds the
   game from your disc - about 5 to 20 minutes, depending on the PC - and adds a Start menu shortcut.
   Nothing is installed outside the install folder; you can keep using the PC meanwhile.
4. On the last page: desktop shortcut, **Play now**, and **free up disk space** (removes the build
   files; tick "Keep the build tools" if you want later updates without the download). The installed
   game takes about 30 MB.

- **Offline PC**: `Sly2-Setup-Offline.exe` (about 220 MB) carries the build tools and downloads nothing.
- **Update or repair**: run the newer setup program; your saves and settings are kept.
- **Uninstall**: Windows Settings > Apps > "Sly 2: Band of Thieves (PC port)" (or `Uninstall.exe` in the
  install folder). It asks before it deletes your saves and settings.
- Setup needs about 1.1 GB of free disk space while it builds. If something fails, the window shows why
  and **Retry** continues where it stopped; the details are in `setup.log` in the install folder.

The setup program is made by `scripts\make_installer.py`; how it works: [notes/install.md](notes/install.md#installer-sly2-setupexe).

## Requirements

| | |
|---|---|
| OS | Windows 10 (version 1809 or later) or Windows 11, 64-bit |
| CPU | any x86-64 CPU (the program uses only the baseline x86-64 instruction set, SSE2); 4 or more cores recommended |
| Graphics | a Direct3D 11 GPU (feature level 11_0) with typed UAV loads and double-precision shaders - most NVIDIA GeForce and AMD Radeon cards of the last ten years - with current drivers. Without one (for example Intel Iris Xe / Arc, which have no double precision) the game uses its software renderer, which needs a fast CPU |
| Disk | Setup: about 1.1 GB free while building, about 30 MB afterwards. Building from source: about 1.3 GB (toolchain, downloads, build). Plus your disc image (about 2.9 GB) |
| Game | your own Sly 2: Band of Thieves (USA, SCUS-97316) disc image, `.iso` |

## Build from source (developers)

Building installs nothing system-wide: `setup.ps1` puts a portable compiler (llvm-mingw clang),
Python 3.13 and two Python packages into this folder (`tools\`, `.venv\`) - about 260 MB of
downloads, each checked against its published SHA-256. Git is fetched as well if you don't have it.

1. **Get the code**: `git clone https://github.com/tanihad/sly2-decomp` (or download the ZIP from
   GitHub and unpack it), into a short path such as `C:\Games\sly2-decomp`.
2. **Set up the toolchain** (once): double-click `setup.cmd`, or in PowerShell in that folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File setup.ps1
   ```

3. **Add your disc image**: make an `.iso` of your own disc with a disc imaging tool and copy it
   into `orig\disc\`. Other formats (`.bin/.cue`, `.chd`, `.cso`) must be converted to `.iso` first.
4. **Build** (one to a few minutes depending on the CPU; later builds reuse unchanged parts):
   double-click `build.cmd`, or

   ```powershell
   powershell -ExecutionPolicy Bypass -File build.ps1
   ```

   (`build.ps1 -Iso D:\path\to\Sly2.iso` uses an image somewhere else). The build checks that the
   image is the right game and version and stops with a message if it is not.
5. **Play**: start `dist\Sly2\sly2.exe`. On the first start it asks for your disc image (pick the
   same `.iso`) and remembers it. The `dist\Sly2` folder can be moved or copied anywhere - for
   example `C:\Games\Sly 2`, or another of your own PCs; it does not need the build folder (the
   other PC only needs the disc image too).

## Controls and settings

- Controllers (Xbox-style/XInput) and the keyboard; the button and key lists are in
  [notes/controls.md](notes/controls.md).
- Alt+Enter or F11: fullscreen. F2 / Shift+F2: internal resolution up / down; F3-F8: outlines,
  filters, motion blur, 16:9 / 4:3, HUD placement. Changes are remembered.
- All settings are in `sly2.ini` ([notes/settings.md](notes/settings.md)). The default is the GPU
  renderer at 3x the PS2 resolution in 16:9, lowered automatically on a GPU that cannot keep up.

## Where things are

| | |
|---|---|
| Settings, saves, log | `%LOCALAPPDATA%\Sly2`: `sly2.ini`, `memcard\slot0` (your saves), `sly2.log` (this run), `sly2.previous.log` |
| Portable mode | an empty file named `portable.txt` next to `sly2.exe`: all of the above is kept in that folder instead |

## Troubleshooting

- **"Windows protected your PC"** (SmartScreen) on the first start: the program was built on your
  PC and is not signed. Click **More info**, then **Run anyway**.
- **"Running scripts is disabled on this system"**: start the scripts exactly as shown above, with
  `powershell -ExecutionPolicy Bypass -File ...`.
- **The disc image is refused**: it must be the USA release (SCUS-97316) as an unmodified `.iso`;
  other regions, other versions and damaged, patched or compressed images are refused with a
  message saying which.
- **Graphics**: if the GPU renderer cannot start (old GPU, missing capability, driver problem) the
  game runs on its software renderer, and `sly2.log` has a line `[ps2] GPU renderer unavailable
  (<reason>)`. Update the graphics driver first. `renderer = software` under `[graphics]` in
  `sly2.ini` selects the software renderer on purpose. If the game runs slowly, lower the internal
  resolution with Shift+F2 (or `internal_scale = 1` in `sly2.ini`).
- **Anything else** (no sound, controller not found, a crash): look at `sly2.log`.
- **Start over**: delete `%LOCALAPPDATA%\Sly2\sly2.ini` (settings; the game asks for the disc again).
  Your saves are in the `memcard` folder next to it - keep it.
- **A PC without internet access**: `setup.ps1 -From <folder>` installs from archives copied there
  (for example the `tools\_downloads` folder of a set-up checkout); `setup.ps1 -DryRun` lists what
  would be downloaded, from where and with which SHA-256. Details: [notes/install.md](notes/install.md).

## Legal

Sly 2: Band of Thieves is a trademark and copyright of Sucker Punch Productions and Sony Interactive
Entertainment. This is an unofficial fan project, not affiliated with or endorsed by them. The
repository holds only original code (the recompiler and the PC runtime) and notes; it contains no
game code, data or artwork, and `orig\`, `build\` and `dist\` are never committed. You need your own
legally obtained disc.

The `dist\Sly2\sly2.exe` you build contains code recompiled from your copy of the game: use it on
your own PCs and **do not share it publicly**.

## For developers

This is also a research workspace. Notes on the recompiler, the runtime and the renderer are in
`notes/` (start with `notes/native-port-principles.md` and `notes/recompiler-design.md`); `recomp/`
(Python) translates the executable to C and `runtime/` (C) stands in for the PS2 hardware. Tests:
`.venv\Scripts\python.exe tests\run_tests.py` and `tests\unit\*\run.ps1` (the installer's full
install/boot/uninstall run: `tests\installer\e2e.ps1`). Load the toolchain into a
shell with `. .\scripts\env.ps1`. Reverse-engineering tools (Ghidra with the Emotion Engine plugin,
objdiff, splat): `setup.ps1 -WithRE`. Tools that replay GPU corpus recordings take their folder from
`-CorpusRoot` or `SLY2_GPU_CORPUS` and skip when there is none.
