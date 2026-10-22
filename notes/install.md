# The game folder (dist\Sly2): build, install, data

A folder the owner starts like any PC game: double-click `sly2.exe`, no console window, no files
from the disc next to it.

## Build it

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

Move or copy `dist\Sly2` anywhere (e.g. `C:\Games\Sly 2`).

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
- System requirements: Windows 10/11 64-bit, a 4-core CPU, a Direct3D 11 GPU as above with current drivers.
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
