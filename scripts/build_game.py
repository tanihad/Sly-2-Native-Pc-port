"""Recompile the game: ELF -> C (recomp) -> object files (clang, in parallel) -> sly2.exe.

Usage (from the project root or a worktree):
    <main checkout>\\.venv\\Scripts\\python.exe scripts\\build_game.py [--release] [-O 1] [--out build/sly2]
                                                                     [--package dist/Sly2]

--release: -O2 and no function tracing (the watchdog then can't report locations).
--package DIR: also link the player's build into DIR (scripts/package.py, notes/install.md): the same
  objects as <out>\\sly2.exe, linked as a Windows GUI program (no console window; the log goes to
  sly2.log in the user data folder) with an icon, version information and a manifest. Its user data
  lives in %LOCALAPPDATA%\\Sly2 unless DIR holds a portable.txt.

Development builds (<out>\\sly2.exe) are console programs: their settings, memory card and logs stay
next to the exe as always (runtime/settings.h).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from paths import CLANG, ELF, ROOT, TOOLS, runtime_sources

LIBS = ["-lwinmm", "-lgdi32", "-luser32", "-lxinput1_4", "-lsynchronization"]
WINDRES = TOOLS / "llvm-mingw" / "bin" / "x86_64-w64-mingw32-windres.exe"
GIT = TOOLS / "git" / "cmd" / "git.exe"


MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1">
  <assemblyIdentity type="win32" name="Sly2.PC" version="{ver}"/>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application><supportedOS Id="{{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}}"/></application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2, PerMonitor</dpiAwareness>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
                      processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
</assembly>
"""

RC = """#include <winver.h>
1 ICON "sly2.ico"
1 24 "sly2.manifest"
VS_VERSION_INFO VERSIONINFO
FILEVERSION {v0},{v1},{v2},0
PRODUCTVERSION {v0},{v1},{v2},0
FILEFLAGSMASK 0x3F
FILEFLAGS 0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
FILESUBTYPE 0
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "sly2-decomp (unofficial fan port)"
      VALUE "FileDescription", "Sly 2: Band of Thieves"
      VALUE "FileVersion", "{v0}.{v1}.{v2}"
      VALUE "InternalName", "sly2"
      VALUE "OriginalFilename", "sly2.exe"
      VALUE "ProductName", "Sly 2: Band of Thieves (native PC port)"
      VALUE "ProductVersion", "{v0}.{v1}.{v2} ({hash})"
      VALUE "Comments", "Recompiled from the player's own disc; runs from that disc image."
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"""

README = """Sly 2: Band of Thieves - native PC port (unofficial)
=====================================================

Installed with Setup: start "Sly 2 - Band of Thieves" (Start menu or desktop) and press Play.
The launcher keeps its own copy of your disc image, so the original .iso is not needed any
more; it also offers updates, Repair and Uninstall.

A game folder built from the sources: start sly2.exe. On the first start it asks for your disc
image: an .iso of your own Sly 2: Band of Thieves (USA, SCUS-97316) disc. The game checks that
it is that disc and remembers where it is. The image is read while you play, so leave it where
it is (if you move it, the game asks again).

Your settings (sly2.ini), saves (the memcard folder) and the log (sly2.log) are in
%LOCALAPPDATA%\\Sly2. Put a file named portable.txt next to sly2.exe to keep them in this
folder instead.

Settings: pause the game (START / Options) and choose "Options", or press Esc (keyboard)
or the PS button (DualSense / DualShock 4) at any time. Graphics (presets Original /
Performance / Balanced / Quality, render scale, outlines, filters), display (Windowed /
Borderless / Fullscreen, resolution, monitor, aspect ratio, HUD layout, frame rate), audio
(volumes, speakers) and controls (vibration, dead zones, camera and aiming inversion, key and
button bindings) change while you play and are saved at once. Display mode, resolution,
monitor and renderer are applied with "Apply changes"; a new display mode or resolution is
kept only when you confirm it within 15 seconds.

Render scale is how sharp the game is drawn compared with your screen: 100% = at least your
screen's (or window's) resolution, 50% = half of it, Auto = as high as your graphics card
keeps up with, up to 100%, Original PS2 = the PS2's 512 x 448. The Graphics page shows what
is drawn and what is shown ("Rendering at 2048 x 1344 -> shown at 2560 x 1440").

Quit: "Quit to Desktop" in the pause menu (or in Options, or the title screen's SELECT menu),
or close the window / Alt+F4. The game asks first; progress since the last save is lost, so
save from the pause menu ("Save Game...") before. Alt+Enter or F11: borderless fullscreen.
The window's size, position and display mode are remembered.

Controls: a PlayStation 5 (DualSense) or PlayStation 4 (DualShock 4) controller over USB or
Bluetooth, an Xbox controller, most other USB gamepads, or the keyboard (WASD to move, IJKL
camera, Space jump, Left Ctrl / F / R the other face buttons, Enter start). Controllers can be
connected or disconnected at any time; they rumble and the PlayStation light bar turns blue.
"Controller test.cmd" (or sly2.exe --pad-test) shows what the game sees from your controller
and tests its rumble. Keys and buttons can be changed in Options > Controls.

Graphics: the game draws on your graphics card (Direct3D 11) in 16:9, sharply scaled to the
window, at 60 frames per second; render scale Auto picks the resolution and lowers it on a
graphics card that cannot keep up. Shortcut keys while playing (the same settings as Options):
  F2 / Shift+F2   render scale up / down (1x-8x the PS2's; exact from then on), Ctrl+F2 Auto
  F3              outlines: PS2 (one PS2 pixel wide) / thin
  F4              filters: Modern (motion blur, sharp progressive picture) / Faithful (as a PS2
                  on a TV: flicker filter, interlacing)
  F5 motion blur, F6 flicker filter, Shift+F6 interlacing, F7 16:9 / 4:3, F8 HUD at the
  screen edges / centred
Every change is remembered in sly2.ini (notes on each setting are in that file).

Requirements: Windows 10 or 11 (64-bit) and a 4-core CPU. Graphics: a Direct3D 11 card
(feature level 11_0 or 11_1) with typed UAV loads and double-precision shaders, current
drivers. Rasterizer-ordered views (most cards since about 2015) make it faster; without them
the same picture takes up to ~1.7x the graphics time. Tested on a GeForce RTX 4060 Ti, where
3x 16:9 uses about half of the card's time; slower cards get 2x or 1x from the automatic
setting. If the graphics card renderer cannot start, the game uses its software renderer
(the PS2's resolution, needs a fast CPU) and sly2.log says why; renderer = software in
sly2.ini selects it on purpose.

Saves from an older build (a memcard\\slot0 folder next to an older sly2.exe) are not moved
automatically: copy that memcard folder into %LOCALAPPDATA%\\Sly2 (or next to this exe in
portable mode) while the game is not running.

"Windows protected your PC" on the first start: this program was built on your own PC and is
not signed; click More info, then Run anyway. This folder can be copied to your other PCs
as it is (they need the disc image too). It contains code recompiled from your copy of the
game: do not share it publicly.

Build {ver} ({hash}).
"""


PROGRESS = False


def progress(stage: str, done: int | None = None, total: int | None = None) -> None:
    if PROGRESS:
        print(f"[progress] {stage}" + (f" {done}/{total}" if total else ""), flush=True)


GPU_DIR = ROOT / "runtime" / "gpu"
R2_DIR = ROOT / "runtime" / "r2"                   # (r2-m1) the modern renderer: built with the GPU backend's headers
GEN_DIR = ROOT / "build" / "gen"                   # generated headers (build/gen/gpu_shaders.h)


def compile_one(src: Path, obj_dir: Path, flags: list[str]) -> tuple[Path, str]:
    gpu = src.parent in (GPU_DIR, R2_DIR)
    obj = obj_dir / (("gpu_" if src.parent == GPU_DIR and not src.name.startswith("gpu_") else "") + src.with_suffix(".o").name)
    if src.parent.name == "vu":
        # recompiled VU microcode: already SIMD; clang's SLP vectorizer only costs minutes on these
        # huge functions (recomp/vu_recomp.py)
        flags = flags + ["-fno-slp-vectorize"]
    deps = list((ROOT / "runtime").glob("*.h"))
    if gpu:                                        # only the GPU backend depends on its headers/shaders
        deps += list(GPU_DIR.glob("*.h")) + list(R2_DIR.glob("*.h")) + [GEN_DIR / "gpu_shaders.h"]
    headers = max(h.stat().st_mtime for h in deps)
    stamp = obj_dir / "flags.txt"
    if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, headers, stamp.stat().st_mtime):
        return obj, ""
    res = subprocess.run([str(CLANG), *flags, "-std=c11", "-c", "-Wno-unused-label", "-Wno-unused-variable",
                          "-I", str(ROOT / "runtime"), "-I", str(src.parent), "-I", str(GPU_DIR), "-I", str(GEN_DIR), str(src),
                          "-o", str(obj)],
                         capture_output=True, text=True)
    return obj, (res.stderr if res.returncode else "")


def build_shaders() -> str:
    """scripts/hlslc.c -> build/tools/hlslc.exe, then runtime/gpu/shaders/perms.txt ->
    build/gen/gpu_shaders.h with the System32 d3dcompiler_47.dll (notes/d3d11-renderer.md 9).
    Returns an error message, or "" on success."""
    hlslc_src = ROOT / "scripts" / "hlslc.c"
    hlslc = ROOT / "build" / "tools" / "hlslc.exe"
    hlslc.parent.mkdir(parents=True, exist_ok=True)
    GEN_DIR.mkdir(parents=True, exist_ok=True)
    if not hlslc.exists() or hlslc.stat().st_mtime < hlslc_src.stat().st_mtime:
        res = subprocess.run([str(CLANG), "-O1", "-std=c11", "-Wall", "-Wextra", "-o", str(hlslc), str(hlslc_src)],
                             capture_output=True, text=True)
        if res.returncode:
            return "hlslc build failed:\n" + res.stderr
    res = subprocess.run([str(hlslc), str(GPU_DIR / "shaders" / "perms.txt"), str(GEN_DIR / "gpu_shaders.h"),
                          str(ROOT / "build" / "hlslc_cache")],   # (compiled shaders kept per source content)
                         capture_output=True, text=True)
    if res.returncode:
        return "shader compilation failed:\n" + res.stdout + res.stderr
    print(res.stdout.strip())
    return ""


def _version() -> tuple[int, str]:
    """(commit count, short hash) of the source tree, for the version resource. A tree unpacked by the
    installer (scripts/make_installer.py) has no git: its version.txt says "<count> <hash>"."""
    vfile = ROOT / "version.txt"
    if vfile.exists() and not (ROOT / ".git").exists():
        try:
            n, h = vfile.read_text(encoding="utf-8").split()[:2]
            return int(n), h
        except (OSError, ValueError):
            pass
    git = str(GIT) if GIT.exists() else "git"
    try:
        n = subprocess.run([git, "rev-list", "--count", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout
        h = subprocess.run([git, "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout
        return int(n.strip() or 0), h.strip() or "unknown"
    except (OSError, ValueError):
        return 0, "unknown"


def package(objs: list[Path], out: Path, dest: Path) -> int:
    """Link the player's build (GUI subsystem, icon, version info, manifest) into dest."""
    import make_icon
    work = out / "package"
    work.mkdir(parents=True, exist_ok=True)
    count, ghash = _version()
    v = (0, 1, count)
    make_icon.make_ico(work / "sly2.ico")
    (work / "sly2.manifest").write_text(MANIFEST.format(ver=f"{v[0]}.{v[1]}.{v[2]}.0"), encoding="utf-8")
    (work / "sly2.rc").write_text(RC.format(v0=v[0], v1=v[1], v2=v[2], hash=ghash), encoding="utf-8")
    res = work / "sly2_res.o"
    rc = subprocess.run([str(WINDRES), "-I", str(work), "sly2.rc", "-O", "coff", "-o", str(res)],
                        cwd=work, capture_output=True, text=True)
    if rc.returncode:
        print("resource compiler failed:\n" + (rc.stderr or rc.stdout)[:4000])
        return 1
    exe = work / "sly2.exe"
    link = subprocess.run([str(CLANG), *map(str, objs), str(res), "-mwindows", *LIBS, "-o", str(exe)],
                          capture_output=True, text=True)
    if link.returncode:
        print(link.stderr[:6000])
        return 1
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, dest / "sly2.exe")
    (dest / "README.txt").write_text(README.format(ver=f"{v[0]}.{v[1]}.{v[2]}", hash=ghash), encoding="utf-8")
    (dest / "Controller test.cmd").write_text('@start "" "%~dp0sly2.exe" --pad-test\r\n', encoding="ascii", newline="")   # input_padtest.c
    print(f"packaged {dest / 'sly2.exe'} ({exe.stat().st_size / 1e6:.1f} MB, version {v[0]}.{v[1]}.{v[2]} {ghash})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(ELF))
    ap.add_argument("-O", dest="opt", default=None, help="clang optimization level (default 1, or 2 with --release)")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--out", default=str(ROOT / "build" / "sly2"))
    ap.add_argument("--package", default=None, metavar="DIR", help="also link the player's build into DIR")
    ap.add_argument("--progress", action="store_true",
                    help="print '[progress] <stage> [i/n]' lines (the installer's progress bar, installer/engine.c)")
    args = ap.parse_args()
    global PROGRESS
    PROGRESS = args.progress
    out = Path(args.out)
    # -fno-auto-import: globals are in this executable, so access them directly instead of through
    # a .refptr pointer (MinGW's provision for data imported from DLLs): one load less per access.
    flags = [f"-O{args.opt or ('2' if args.release else '1')}", "-fno-auto-import"]
    if args.release:
        flags.append("-DPS2_NO_TRACE")

    t0 = time.time()
    if not Path(args.elf).exists() and Path(args.elf) == ELF:
        # a fresh checkout has only the disc image: take the executable from it (scripts/extract_elf.py)
        import extract_elf
        if extract_elf.main([]) != 0:
            return 1
    progress("generate")
    gen = subprocess.run([sys.executable, "-m", "recomp", "generate", args.elf, "-o", str(out / "recomp")],
                         cwd=ROOT, capture_output=True, text=True)
    print(gen.stdout.strip() or gen.stderr)
    if gen.returncode:
        return 1
    # The IOP sound modules (989snd, spiop) embedded in the executable: recomp/iop.py
    progress("iop")
    iop = subprocess.run([sys.executable, "-m", "recomp", "iop", args.elf, "-o", str(out / "iop")],
                         cwd=ROOT, capture_output=True, text=True)
    print(iop.stdout.strip() or iop.stderr)
    if iop.returncode:
        return 1
    # The VU microprograms (.vutext overlays): recomp/vu_recomp.py
    progress("vu")
    vu = subprocess.run([sys.executable, "-m", "recomp", "vu", args.elf, "-o", str(out / "vu")],
                        cwd=ROOT, capture_output=True, text=True)
    print(vu.stdout.strip() or vu.stderr)
    if vu.returncode:
        return 1

    obj_dir = out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    stamp = obj_dir / "flags.txt"
    if not stamp.exists() or stamp.read_text() != " ".join(flags):
        stamp.write_text(" ".join(flags))              # flag change: rebuild everything

    # The Direct3D 11 GS backend (runtime/gpu, notes/d3d11-renderer.md): shaders first, compile
    # errors fail the build. Selected at run time by PS2_GS_BACKEND (default sw).
    progress("shaders")
    err = build_shaders()
    if err:
        print(err[:6000])
        return 1

    sources = (sorted((out / "recomp").glob("*.c")) + sorted((out / "iop").glob("*.c")) +
               sorted((out / "vu").glob("*.c")) + runtime_sources() + sorted(GPU_DIR.glob("*.c")) +
               sorted(R2_DIR.glob("*.c")))
    objs, errors = [], []
    progress("compile", 0, len(sources))
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        futures = [pool.submit(compile_one, s, obj_dir, flags) for s in sources]
        if PROGRESS:                                   # (completion order, for the installer's bar)
            for i, _ in enumerate(as_completed(futures), 1):
                progress("compile", i, len(sources))
        for f in futures:                              # (source order: the link order stays the same)
            obj, err = f.result()
            objs.append(obj)
            if err:
                errors.append(err)
    if errors:
        print(f"{len(errors)} file(s) failed to compile; first errors:\n" + "\n".join(errors)[:6000])
        return 1
    print(f"compiled {len(sources)} files ({' '.join(flags)}) in {time.time() - t0:.0f}s")

    exe = out / "sly2.exe"
    progress("link")
    link = subprocess.run([str(CLANG), *map(str, objs), *LIBS, "-o", str(exe)], capture_output=True, text=True)
    if link.returncode:
        print(link.stderr[:6000])
        return 1
    print(f"linked {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.0f}s total")
    if args.package:
        if not args.release:
            print("note: --package without --release packages a debug build (-O1, function tracing)")
        progress("package")
        return package(objs, out, Path(args.package))
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main())
