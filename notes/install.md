# The game folder (dist\Sly2): build, install, data

A folder the owner starts like any PC game: double-click `sly2.exe`, no console window, no files
from the disc next to it.

## Installer (Sly2-Setup.exe)

A "builder installer" for players: no commands, no console windows. It contains no game code or data
(only our recompiler/runtime sources and, in the offline variant, the free toolchain); the game's
translated code exists only after it is built on the player's PC from the player's disc.

**Make it**: `<main checkout>\.venv\Scripts\python.exe scripts\make_installer.py [--offline]` ->
`build\installer\Sly2-Setup.exe` (1.0 MB: 190 KB program + 0.8 MB sources), with `--offline` also
`Sly2-Setup-Offline.exe` (220 MB), and `SHA256SUMS.txt`. Needs a set-up checkout (`setup.ps1`): the
archives are taken from `tools\_downloads` and checked against the pins first.

- **Program**: `installer/*.c`, plain Win32 + common controls, built with llvm-mingw (`-Wall -Wextra
  -Werror`); no installer framework. `main.c` command line + wizard, `engine.c` the steps, `util.c`
  files/processes/SHA-256 (bcrypt)/registry/shortcuts/payload, `net.c` WinHTTP downloads, `disc.c` the
  disc check (= `scripts/extract_elf.py` / `runtime/disc.c`).
- **Payload**: appended to the program: entries + a table (name, offset, size, SHA-256) + a trailer
  `SLY2PAY1`; every entry is verified when unpacked ("the setup file is damaged: download it again").
  If the exe is Authenticode-signed after the payload is appended, the payload end is found from the PE
  certificate table, so signing works. `source.zip` = `git ls-files recomp runtime` + `scripts/
  {build_game,package,paths,extract_elf,make_icon}.py`, `scripts/hlslc.c`, `requirements.txt`,
  `version.txt` (`<commit count> <hash>`; `build_game.py` uses it when there is no `.git`).
  make_installer refuses any path under orig/, build/, dist/, tools/ or any .iso/.elf/.bin/SCUS_ name.
- **Single source of truth**: the toolchain pins (file, URL, SHA-256) are parsed from
  `scripts/setup-tools.ps1`, the expected executable (size, SHA-256) from `scripts/extract_elf.py`.

**Wizard pages**: Welcome (says "update" if an installation is found through the registry) -> "Your copy
of the game" (legal note; Next only after ticking "I have my own copy...") -> Install folder (default
`%LOCALAPPDATA%\Programs\Sly2`, no admin; space needed vs free; a folder that needs admin rights, e.g.
Program Files, offers to restart Setup elevated: then all-users Start menu + HKLM entry) -> Disc image
(checked when chosen: ISO 9660 -> SYSTEM.CNF -> BOOT2 -> SCUS_973.16 size + SHA-256; separate messages
for not-an-ISO (.bin/.cue/.chd/.cso), not a PS2 disc, another game/region (PAL/Japanese hints),
another version/patched, truncated) -> Ready (summary) -> Installing (status, progress bar, current
item with MB/s, a list of steps, taskbar progress; Cancel; on error the reason in red + Retry + Show log)
-> Done (desktop shortcut [on], free up disk space: remove the build files [on], keep the build tools
for faster updates [off], Play now [on]).

**What Install does** (`engine.c setup_install`; log: `<dir>\setup.log`):

1. Checks the image again, the free space, that the game is not running; writes `Uninstall.exe` (the
   program without its payload), `setup-state.ini` and the uninstall registry entry first, so a cancelled
   or failed install can always be removed from Apps & features.
2. Unpacks `source.zip` into `<dir>\builder` (old sources replaced; if `builder\built-version.txt` names
   another version, `builder\build` is deleted: the payload's file times are older than the player's
   objects, so build_game's time check could keep stale objects).
3. Toolchain into `builder\tools`, each archive from the first source that has it: this setup file
   (offline variant) -> `/from=<folder>` -> a complete earlier download -> download. Downloads: WinHTTP,
   automatic proxy, TLS 1.2/1.3, resumable (`.part` + `Range`; a server without Range: start over),
   5 attempts with back-off, then an error with Retry; SHA-256 checked before use (mismatch: the file is
   deleted, "damaged or not the expected file"). Unpacked with Windows' own `tar.exe` (Windows 10 1803+)
   with no window; the archive is deleted afterwards.
   | archive | from | SHA-256 | size |
   |---|---|---|---|
   | llvm-mingw-20261006-ucrt-x86_64.zip | github.com/mstorsjo/llvm-mingw releases | 317492c4...f52d17a | 182 MB |
   | uv-x86_64-pc-windows-msvc.zip (uv 0.13.0) | github.com/astral-sh/uv releases | 088962f9...dba656c | 15 MB |
   | Python 3.13.16 (python-build-standalone 20261009, install_only_stripped) | `uv python install` (uv checks the SHA-256 built into the pinned uv) | | ~20 MB |
   | rabbitizer 1.16.2, pyelftools 0.33 | PyPI via `uv pip install --require-hashes -r requirements.txt` | requirements.txt | <1 MB |
   llvm-mingw is unpacked without the parts the build never uses (aarch64/armv7/i686 sysroots, lldb,
   clangd, clang-tidy, lldb's Python, Linux sanitizer runtimes): 340 MB instead of 715 MB.
   `uv python install --no-bin --no-registry`: nothing in `~\.local\bin` or `HKCU\Software\Python`.
   Offline variant / `/from`: Python is copied (a `cpython-3.13.16-windows-x86_64-*` folder) and the
   packages come from a uv cache with `--offline` (same layout as `setup.ps1 -From`).
4. `extract_elf.py --iso <image>` (into `builder\orig`), then `build_game.py --release --out builder\build\rel
   --package builder\dist --progress` (`[progress] <stage> [i/n]` lines drive the bar), with no window and
   below-normal priority, the whole process tree in a job object (Cancel kills it).
5. Copies `sly2.exe`, `README.txt`, `Controller test.cmd` into `<dir>` ("close Sly 2 and click Retry" if
   it is running); sets `[game] disc = <image>` in `%LOCALAPPDATA%\Sly2\sly2.ini` (or the portable folder:
   other keys untouched; a new file gets only that key), so the first start needs no file dialog; Start
   menu shortcut `Sly 2 - Band of Thieves.lnk`; EstimatedSize.
6. Finish: desktop shortcut, clean-up (`builder\build`, `orig`, `dist`, `tools\_downloads`; without "keep
   tools" the whole `builder`), Play now (started through Explorer when Setup runs elevated, never as admin).

**Updates / repair**: run Setup again (any version): it finds the installation (registry
`InstallLocation` + `setup-state.ini`), keeps saves/settings (they are not in the install folder),
reuses kept tools whose `.sly2-pin` matches the pinned SHA-256, rebuilds and replaces the game.

**Uninstall** (`Uninstall.exe`, Apps & features; `/silent` keeps saves, `/deletesaves` removes them):
asks for confirmation, waits for the game to close, removes the shortcuts recorded in
`setup-state.ini`, `builder`, the game files, `setup.log`, the registry entry, then asks (default No)
whether to delete `%LOCALAPPDATA%\Sly2` (saves, settings, logs). Only files Setup made are removed: the
folder itself goes only if empty (a hidden `cmd` removes `Uninstall.exe` and the folder 2 s after it
exits). An all-users install asks for elevation. Its log: `%TEMP%\sly2-uninstall.log`.

**Disk space** (measured 2026-10-24, e2e run): during the build `builder` holds llvm-mingw 340 MB, uv
32 MB, Python 60 MB, uv cache 17 MB, venv 2 MB, sources 2 MB, build intermediates 175 MB, plus the
archive being unpacked (182 MB) -> peak about 0.85 GB; Setup asks for 1.1 GB (estimate + 10%). After
"free up disk space": the game folder is **26 MB**; with "keep the build tools" about 480 MB.
Downloads (online): about 220 MB. Time: 73 s for the whole install on an i7-14700F (28 threads,
archives from a local server); the build is ~80 s of it there, mostly compiling 135 files in parallel
(a 4-core PC: several minutes).

**Command line** (also for CI): `Sly2-Setup.exe /silent /iso=<image> [/dir=<folder>] [/keeptools]
[/keepbuild] [/nodesktop] [/play] [/offline] [/from=<folder>] [/mirror=<URL or folder>] [/log=<file>]`;
exit codes 0 ok, 1 failed, 2 bad arguments, 3 cancelled, 4 wrong disc, 5 not enough space, 6 needs admin.
`Uninstall.exe [/silent] [/deletesaves]`. Test overrides (never touch the player's things):
`/startmenu= /desktopdir= /userdata= /regkey=<HKCU subkey>`. `/mirror=` replaces the download URLs'
folder (e.g. a local server); `/from=` = the `setup.ps1 -From` layout. Tools: `/version`, `/payload`,
`/sha256 <file>`, `/checkdisc <image>`, `/download <url> <file> [sha256 [size]]`, `/uitest` (pages driven
in a hidden window), `/uishot=<folder>` (pages rendered off-screen to .bmp).

**Tests**: `tests\unit\installer\run.ps1` (quick: tool commands, payload contents, the disc check on
synthetic images + the real one, downloads from `range_server.py` on 127.0.0.1 - complete, dropped and
resumed, resume of a partial file, no-Range server, 404, wrong SHA-256, nothing listening - `/uitest`,
a damaged payload removed again by Uninstall.exe). `tests\installer\e2e.ps1` (~5 min, ~2 GB in %TEMP%,
removed): bad archive / 404 first, then install with the archives over HTTP from 127.0.0.1 and Python/
packages from a `/from` folder, the installed game boots headless from the image Setup wrote into
sly2.ini (PS2_HEADLESS, PS2_WATCHDOG -> exit 3), shortcuts + registry in test locations, update in place
keeps a save, uninstall keeps saves, then the offline exe with `/offline` and `Uninstall /deletesaves`.
Not testable here: the real downloads from github.com / PyPI and `uv python install` (no network for
agents), the UAC prompt of an all-users install.

## From a fresh clone (any Windows 10/11 x64 PC)

```powershell
git clone https://github.com/tanihad/sly2-decomp; cd sly2-decomp
powershell -ExecutionPolicy Bypass -File setup.ps1        # toolchain into tools\ and .venv\ (once)
copy <your disc image>.iso orig\disc\                      # Sly 2, USA, SCUS-97316
powershell -ExecutionPolicy Bypass -File build.ps1        # -> dist\Sly2\sly2.exe (+ README.txt)
```

- `setup.ps1` (-> `scripts\setup-tools.ps1`) installs only what the game build needs, pinned:
  llvm-mingw 20261006 (ucrt, x86_64), uv 0.13.0, MinGit 2.56.0.2 (only if no `git` is on PATH), each
  checked against the SHA-256 GitHub publishes for that release asset (the values are in the
  script); Python 3.13.16 through `uv python install` (python-build-standalone; uv verifies its own
  checksums); `requirements.txt` (rabbitizer, pyelftools) with `--require-hashes`. Nothing is
  installed outside the repo folder; re-running skips what is there.
  - `-DryRun`: prints every archive, its URL or local source and its SHA-256; changes nothing.
  - `-From <dir>`: offline. Archives are taken from `<dir>`, `<dir>\_downloads` or
    `<dir>\tools\_downloads` (verified in place, never modified); Python from a
    `cpython-3.13.16-windows-x86_64-*` folder in `<dir>\python` or `<dir>\tools\python` (copied); the
    packages from a uv cache (`<dir>\uv-cache` or `<dir>\tools\uv-cache`, merged into ours) or wheels
    in `<dir>\wheels`, with `uv pip install --offline`. Pointing it at another set-up checkout works:
    `setup.ps1 -From C:\path\to\other\sly2-decomp`. Missing pieces are an error, never a download.
  - `-WithRE`: also Ghidra + JDK + the Emotion Engine plugin, objdiff-cli, splat/capstone
    (`requirements-re.txt`); latest releases, online only.
- `build.ps1 [-Iso <image>] [-Dest <dir>]`: `scripts\extract_elf.py` takes `SCUS_973.16` out of the
  image into `orig\` (ISO 9660 -> `SYSTEM.CNF` -> `BOOT2`, size and SHA-256 must be the USA v1.00
  executable; another game, region or version is refused with a message), then `scripts\package.py`.
  `scripts\build_game.py` also extracts the executable by itself when `orig\SCUS_973.16` is missing.
  The shaders are compiled at build time with Windows' own `d3dcompiler_47.dll`.
- Tested (2026-10-22) in a fresh `git clone --no-local` in %TEMP% with a clean PATH (no git, no
  project Python): `setup.ps1 -From <main checkout>` 11 s, `build.ps1 -Iso <image>` 50 s on 28
  threads, 27 MB exe that boots on the GPU renderer, the software renderer and WARP.
- Disk: tools ~250 MB + downloads ~260 MB (kept in `tools\_downloads`), build ~150 MB, the game
  folder 27 MB.

## Build it (development checkout or worktree)

```powershell
<main checkout>\.venv\Scripts\python.exe scripts\package.py      # -> dist\Sly2\sly2.exe + README.txt
```

`scripts\package.py` runs `scripts\build_game.py --release --out build\rel --package dist\Sly2`:
the release build (as scripts\play.ps1 uses; objects reused if up to date), then the same objects
linked again as a Windows GUI program (`-mwindows`) with a resource file made at build time:

- an application icon, drawn by `scripts\make_icon.py` (an original picture - a thief's mask under a
  crescent moon - rendered procedurally; the disc has no Windows icon, and no game artwork is used),
- version information (0.1.<commit count>, product version with the commit hash),
- a manifest (per-monitor DPI aware, long paths, common controls 6, Windows 10/11).

The folder holds `sly2.exe` (only system DLLs: user32, gdi32, winmm, kernel32 and the Universal
CRT; Direct3D 11, XInput and the file dialog are loaded at run time) and a short `README.txt`.
`dist/` is git-ignored: it is built from the owner's disc and must never be committed or shared.
The executable contains recompiled game code but not the game's executable file or data: those are
read from the disc image at run time.

Move or copy `dist\Sly2` anywhere (e.g. `C:\Games\Sly 2`), also to the owner's other PCs: it does not
depend on the build tree (it needs only the disc image there, asked for on the first start).

## Portability of sly2.exe (checked 2026-10-22, pc-portable)

- **DLLs** (`llvm-readobj --coff-imports`): KERNEL32, USER32, GDI32, WINMM,
  api-ms-win-core-synch-l1-2-0 (WaitOnAddress, Windows 8+) and the Universal CRT
  (api-ms-win-crt-*, part of Windows 10). No MinGW runtime DLLs (libc++, libunwind, winpthread):
  the C runtime is the UCRT and nothing else is dynamically linked. Loaded at run time, each with a
  fallback or a message: d3d11, dxgi, d3dcompiler_47 (inbox since Windows 8.1; the software
  presenter's shaders), xinput1_4 / 1_3 / 9_1_0, ole32, comdlg32, shell32, avrt (MMCSS), dwmapi.
- **CPU**: clang's default x86-64 target, no `-march`, no target attributes. A scan of the whole
  `.text` (4.48 M instructions, `llvm-objdump -d -M intel`) finds only SSE2-baseline instructions
  plus 19 `tzcnt`, which is LLVM's encoding of `rep bsf` for count-trailing-zeros of a nonzero value
  (it executes as BSF on CPUs without BMI1, with the same result). No SSE3/SSSE3/SSE4/AVX/AVX2/
  AVX-512/FMA/BMI2/POPCNT/CMPXCHG16B. Minimum: any x86-64 CPU that runs 64-bit Windows 10.
- **Windows**: minimum Windows 10 1809 (stated floor). What needs newer than Windows 8: the
  UCRT (inbox from Windows 10), the GPU renderer's FLIP_DISCARD swap chain (Windows 10; the software
  presenter falls back to FLIP_SEQUENTIAL and then GDI), DXGI tearing (checked with
  CheckFeatureSupport, off when unsupported), high-resolution waitable timers (Windows 10 1803;
  every user falls back to Sleep/timeBeginPeriod when creation fails), MMCSS via avrt (optional).
- **GPU**: the renderer checks feature level 11_0/11_1, typed UAV loads, double-precision shaders and
  dual-source blending and creates every shader at start; anything missing -> software renderer
  with one log line. Tested headless on the fresh-clone build: hardware (RTX 4060 Ti), WARP,
  PS2_GPU_NO_ROV=1, PS2_GPU_FL=11_0, PS2_GPU_FAIL=device/caps/shader (each -> software renderer),
  PS2_GS_BACKEND=sw. The NVIDIA-motivated precision code (`precise` + multiplying by a constant 1
  from a constant buffer so a driver cannot fuse a*b+c into an FMA) only forces correctly rounded
  IEEE results and is harmless elsewhere; the comments about NVIDIA power states only explain the
  auto-scale timing. Known gap: GPUs without double precision (Intel Xe / Iris Xe / Arc) get the
  software renderer.

## Start-up (runtime/launch.c)

1. User data folder (notes/settings.md): `%LOCALAPPDATA%\Sly2`, or the exe's folder if
   `portable.txt` is next to it (`PS2_USER_DIR` overrides; console/development builds always use
   the exe's folder).
2. Log: without a console (the GUI build started from Explorer or a shortcut) stderr goes to
   `<user data>\sly2.log` (the run before: `sly2.previous.log`). When stderr is a console, file or
   pipe (scripts\run_game.ps1 redirects it) it stays there, so scripts read logs as before. Fatal
   start-up errors are also shown in a message box in that case.
3. Game executable and disc image, first match wins:
   - `argv[1]` is an ELF (`sly2.exe orig\SCUS_973.16`, development and tests): that file, unchecked;
     the disc image from `PS2_ISO`, else `[game] disc` in sly2.ini;
   - `argv[1]` is a disc image (`sly2.exe x.iso`, or an .iso dropped on the exe);
   - `PS2_ISO=<image>`;
   - `[game] disc` in sly2.ini;
   - nothing configured, or the configured image is unusable, in the GUI build (no console, not
     PS2_HEADLESS): a message says why (if there was a path), then a Windows file-open dialog asks
     for the `.iso`; the choice is checked and stored in sly2.ini, cancelling exits. Console builds
     never show a dialog: they print what to set and exit with code 1.
4. Checking an image (runtime/disc.c): ISO 9660 volume -> `SYSTEM.CNF` -> `BOOT2 = cdrom0:\SCUS_973.16;1`
   -> that file's size and SHA-256 must equal the executable the build was recompiled from
   (`ps2_elf_size`, `ps2_elf_sha256`, written into the generated func_table.c by
   `recomp generate`; for this build 2,597,048 bytes, a511a94b...6a73). Then the executable is
   loaded from the image into EE RAM, and cdvd.c reads the same image for the game's disc access.
   Another game, another version (e.g. a PAL disc or a patched executable), a damaged image or a
   non-ISO file are each refused with their own message.

## While running / exit

- Window title "Sly 2: Band of Thieves" (`[window] show_fps = on` adds the frame rate; "(GPU)" with the
  GPU renderer).
- Renderer (since M11): the Direct3D 11 GPU renderer by default - 3x internal resolution (`internal_scale =
  auto`: lowered automatically on a GPU that cannot keep up), 16:9, Modern filters, 60 fps; keys in
  notes/controls.md. At start-up it checks the device (feature level 11_0 or 11_1, typed UAV loads,
  double-precision shaders, dual-source blending; rasterizer-ordered views optional) and creates every shader
  it uses; if any of that fails the game runs on the software renderer, with one log line
  `[ps2] GPU renderer unavailable (<reason>): using the software renderer` (and the window's own presenter: no
  crash, no dialog). `[graphics] renderer = software` (or `PS2_GS_BACKEND=sw`) selects the software renderer.
  A lost device is recreated (twice); a third loss switches to the software renderer for the rest of the
  session.
- System requirements: Windows 10 (1809+) / 11 64-bit, any x86-64 CPU (4 cores recommended), a Direct3D 11
  GPU as above with current drivers (see "Portability of sly2.exe").
  Measured on the RTX 4060 Ti: 3x 16:9 costs 7-11 ms of GPU time per game frame (about half the GPU's time);
  GPUs without rasterizer-ordered views need up to ~1.7x that (notes/d3d11-renderer.md appendix M), and the
  automatic scale picks 2x or 1x where 3x does not fit. The software renderer needs a fast CPU (its GS thread
  takes ~16 ms per VBlank in heavy scenes).
- Alt+Enter / F11: borderless fullscreen; Esc leaves it.
- Closing the window (or Alt+F4, or Windows logging off): the window's placement and fullscreen
  state go to `[window]` in sly2.ini, then `launch_exit` waits for a memory card operation in
  progress (mc.c holds a lock around every card RPC; at most 2 s) so the process never ends in the
  middle of writing a card file, and exits with code 0. Card writes are complete host file writes,
  so they are on disk (in the OS file cache) as soon as each operation returns.
- Next start: the window opens where it was (if that place is still on a monitor), maximized
  and/or fullscreen as it was.

## Moving saves from an older build

Older builds kept the memory card in `memcard\slot0` next to the exe (development builds still
do: e.g. `build\rel\memcard\slot0` for scripts\play.ps1). The game does not move saves by itself.
To continue those saves with the dist build, copy the whole `memcard` folder into
`%LOCALAPPDATA%\Sly2\` (or next to `dist\Sly2\sly2.exe` with a `portable.txt`) while the game is not
running. `sly2.ini` can be copied the same way.

## Development and tests (unchanged)

- `scripts\run_game.ps1`, `scripts\play.ps1`, warp/live scripts: console builds with `argv[1]` = the
  ELF and `PS2_ISO`; settings, card and logs next to the exe as before.
- Every env var keeps working with the GUI build too (PS2_DUMP_FRAMES, PS2_INPUT, PS2_WATCHDOG, ...).
- Tests: `tests\unit\disc\run.ps1` (ISO 9660 / SYSTEM.CNF / hash checks on synthetic images, the
  real image if present, the dialog loop with a scripted prompt, launch_load_game's sources);
  `tests\unit\window` (sly2.ini updates, window state written on close). Tests set
  `PS2_USER_DIR` so they never touch the player's `%LOCALAPPDATA%\Sly2`.
