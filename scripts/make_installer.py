"""Build the Windows installer: build\\installer\\Sly2-Setup.exe (and with --offline Sly2-Setup-Offline.exe).

    <main checkout>\\.venv\\Scripts\\python.exe scripts\\make_installer.py [--offline] [--out build\\installer]

The setup program (installer/*.c, compiled with llvm-mingw) is a "builder installer": it installs our
recompiler and runtime and the pinned build toolchain, checks the player's own disc image, builds sly2.exe
on the player's PC and installs it (notes/install.md, "Installer").

What goes into the setup file (appended to the program as a payload, each entry with its SHA-256):
  source.zip       the tracked files the build needs: recomp/, runtime/, the build scripts, requirements.txt,
                   version.txt. Nothing from orig/, build/, dist/ or any other game data.
  --offline also:  the toolchain archives exactly as their publishers ship them (llvm-mingw, uv; same files
                   and SHA-256 as scripts/setup-tools.ps1 pins), python.zip (this checkout's
                   tools/python/cpython-<ver>-windows-x86_64-none, the python-build-standalone build uv
                   installed and verified) and uv-cache.zip (the cached rabbitizer / pyelftools wheels).

The pins (versions, URLs, SHA-256) are read from scripts/setup-tools.ps1, the expected executable from
scripts/extract_elf.py: one source of truth for setup.ps1 and the installer.
"""
import argparse
import fnmatch
import hashlib
import io
import os
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from paths import CLANG, MAIN, ROOT, TOOLS  # noqa: E402

WINDRES = TOOLS / "llvm-mingw" / "bin" / "x86_64-w64-mingw32-windres.exe"
GIT = TOOLS / "git" / "cmd" / "git.exe"
SRC = ROOT / "installer"

# The build's inputs (scripts/build_game.py and what it imports or runs).
PAYLOAD_DIRS = ["recomp", "runtime"]
PAYLOAD_FILES = ["scripts/build_game.py", "scripts/package.py", "scripts/paths.py", "scripts/extract_elf.py",
                 "scripts/make_icon.py", "scripts/disc_art.py", "scripts/hlslc.c", "requirements.txt"]
NEVER = ("orig/", "build/", "dist/", "tools/", ".venv/")

# Parts of llvm-mingw the game build never uses (other CPU targets, the debugger, clangd/clang-tidy, the
# Python of lldb, Linux sanitizer runtimes): not unpacked, which saves ~350 MB.
LLVM_EXCLUDES = ["*/aarch64-w64-mingw32/*", "*/armv7-w64-mingw32/*", "*/i686-w64-mingw32/*", "*/python/*",
                 "*/bin/lldb*", "*/bin/liblldb*", "*/bin/clangd.exe", "*/bin/clang-tidy.exe", "*/bin/libpython*",
                 "*/bin/aarch64-*", "*/bin/armv7-*", "*/bin/i686-*", "*/bin/arm64ec-*", "*/lib/clang/*/lib/linux/*",
                 "*/lib/windows/*-i386.*", "*/lib/windows/*-aarch64.*", "*/lib/windows/*-arm.*"]

# Disk use measured on a full install (notes/install.md "Installer: disk space"), MB.
NEED_SOURCES_MB = 10
NEED_BUILD_MB = 300        # build\rel (generated C + objects ~225), build\gen, hlslc cache, dist staging, orig ELF
NEED_GAME_MB = 30          # sly2.exe + README
NEED_PYTHON_MB = 90        # uv python install (60) + its download in the uv cache
NEED_VENV_MB = 40          # .venv (13) + uv cache with the two wheels (~20)
DOWNLOAD_EXTRA_MB = 25     # python-build-standalone (~20 MB) + wheels, on top of the archive sizes

# Progress bar (permille of the whole install) - tuned to the measured step durations.
PM = dict(TOOLS=20, PYTHON=300, PACKAGES=340, ELF=370, BUILD=380, BUILD_SPAN=590, INSTALL=975)


def git(*args: str) -> str:
    exe = str(GIT) if GIT.exists() else "git"
    return subprocess.run([exe, *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def parse_pins() -> tuple[list[dict], str]:
    text = (HERE / "setup-tools.ps1").read_text(encoding="utf-8")
    pins = []
    for block in re.findall(r"\[pscustomobject\]@\{(.*?)Check = '([^']*)' \}", text, re.S):
        body, check = block
        def field(name):
            m = re.search(rf"{name}\s*=\s*'([^']*)'", body)
            return m.group(1) if m else None
        flatten = re.search(r"Flatten\s*=\s*\$(true|false)", body)
        pins.append(dict(name=field("Name"), dir=field("Dir"), file=field("File"), url=field("Url"),
                         sha256=field("Sha256").lower(), check=check, flatten=flatten and flatten.group(1) == "true"))
    pyver = re.search(r"\$PythonVersion\s*=\s*'([^']+)'", text).group(1)
    pins = [p for p in pins if p["dir"] in ("llvm-mingw", "uv")]    # (MinGit: not needed, version.txt instead)
    if [p["dir"] for p in pins] != ["llvm-mingw", "uv"]:
        sys.exit("make_installer: could not read the llvm-mingw / uv pins from scripts/setup-tools.ps1")
    return pins, pyver


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def archive_info(pin: dict) -> dict:
    """Size, unpacked size and entry count of a pinned archive, from the copy setup.ps1 downloaded."""
    local = TOOLS / "_downloads" / pin["file"]
    if not local.exists():
        sys.exit(f"make_installer: {local} is missing (run setup.ps1 first: the installer is made from verified archives)")
    if sha256(local) != pin["sha256"]:
        sys.exit(f"make_installer: {local} does not match its pinned SHA-256")
    excludes = LLVM_EXCLUDES if pin["dir"] == "llvm-mingw" else []
    with zipfile.ZipFile(local) as z:
        infos = [i for i in z.infolist() if not any(fnmatch.fnmatchcase(i.filename, e) for e in excludes)]
    return dict(local=local, size=local.stat().st_size, entries=len(infos),
                installed_mb=int(sum(i.file_size for i in infos) / 2**20) + 1,
                excludes=" ".join(f'--exclude "{e}"' for e in excludes))


def version() -> tuple[int, str]:
    count = int(git("rev-list", "--count", "HEAD").strip())
    h = git("rev-parse", "--short", "HEAD").strip()
    dirty = git("status", "--porcelain", "--", *PAYLOAD_DIRS, *PAYLOAD_FILES, "installer").strip()
    return count, h + ("-dirty" if dirty else "")


def payload_files() -> list[str]:
    files = [f for f in git("ls-files", "--", *PAYLOAD_DIRS).splitlines() if f]
    files += PAYLOAD_FILES
    for f in files:
        if f.startswith(NEVER) or f.lower().endswith((".iso", ".elf", ".bin")) or "SCUS_" in f.upper():
            sys.exit(f"make_installer: refusing to package {f}")
    return sorted(set(files))


def zip_tree(z: zipfile.ZipFile, src: Path, arc_prefix: str, skip=lambda rel: False) -> None:
    for dirpath, dirnames, filenames in os.walk(src):
        d = Path(dirpath)
        dirnames[:] = [n for n in dirnames if not (d / n).is_symlink() and not os.path.isjunction(d / n)
                       and not skip((d / n).relative_to(src).as_posix() + "/")]
        for n in filenames:
            rel = (d / n).relative_to(src).as_posix()
            if not skip(rel):
                z.write(d / n, f"{arc_prefix}{rel}")


def c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_config(path: Path, pins: list[dict], pyver: str, count: int, ghash: str) -> None:
    sys.path.insert(0, str(HERE))
    import extract_elf
    lines = ["/* generated by scripts/make_installer.py - do not edit */", "#pragma once",
             f"#define SETUP_VERSION L\"0.1.{count}\"", f"#define SETUP_COMMIT L\"{ghash}\"",
             f"#define SETUP_PYTHON_VERSION L\"{pyver}\"",
             f"#define SETUP_ELF_SIZE {extract_elf.EXPECTED_SIZE}u", f"#define SETUP_ELF_SHA256 \"{extract_elf.EXPECTED_SHA256}\"",
             f"#define SETUP_NEED_SOURCES_MB {NEED_SOURCES_MB}ull", f"#define SETUP_NEED_BUILD_MB {NEED_BUILD_MB}ull",
             f"#define SETUP_NEED_GAME_MB {NEED_GAME_MB}", f"#define SETUP_NEED_PYTHON_MB {NEED_PYTHON_MB}ull",
             f"#define SETUP_NEED_VENV_MB {NEED_VENV_MB}ull"]
    download = int(sum(p["size"] for p in pins) / 2**20) + DOWNLOAD_EXTRA_MB
    keep = sum(p["installed_mb"] for p in pins) + NEED_PYTHON_MB - 30 + NEED_VENV_MB - 10
    lines += [f"#define SETUP_DOWNLOAD_MB {download}", f"#define SETUP_DOWNLOAD_MB_STR L\"{download}\"",
              f"#define SETUP_KEEP_TOOLS_MB {keep}"]
    lines += [f"#define SETUP_PM_{k} {v}" for k, v in PM.items()]
    lines.append(f"#define SETUP_NPINS {len(pins)}")
    shares = {"llvm-mingw": (0, 900), "uv": (900, 1000)}
    lines.append("static const setup_pin SETUP_PINS[] = {")
    for p in pins:
        a, b = shares[p["dir"]]
        lines.append(f"    {{ L{c_str(p['name'])}, L{c_str(p['dir'])}, L{c_str(p['file'])}, {c_str(p['file'])}, L{c_str(p['url'])}, "
                     f"\"{p['sha256']}\", {p['size']}ull, {int(p['flatten'])}, L{c_str(p['check'])}, {p['entries']}, "
                     f"L{c_str(p['excludes'])}, {p['installed_mb']}, {a}, {b} }},")
    lines.append("};")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1">
  <assemblyIdentity type="win32" name="Sly2.PC.Setup" version="{ver}"/>
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
1 ICON "{icon}"
1 24 "setup.manifest"
{art}
VS_VERSION_INFO VERSIONINFO
FILEVERSION 0,1,{count},0
PRODUCTVERSION 0,1,{count},0
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
      VALUE "CompanyName", "tanihad"
      VALUE "FileDescription", "Sly 2: Band of Thieves launcher and setup (builds the game from your own disc)"
      VALUE "FileVersion", "0.1.{count}"
      VALUE "InternalName", "Sly2-Setup"
      VALUE "OriginalFilename", "Sly2-Setup.exe"
      VALUE "ProductName", "Sly 2: Band of Thieves (native PC port)"
      VALUE "ProductVersion", "0.1.{count} ({hash})"
      VALUE "Comments", "Contains no game code or data."
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"""

LIBS = ["-lcomctl32", "-lole32", "-lshell32", "-luuid", "-lwinhttp", "-lbcrypt", "-ladvapi32", "-luser32", "-lgdi32"]


ART_EXT = ("png", "webp", "jpg", "jpeg", "bmp")      # (installer/art.c art_find's order)


def find_art(folder: Path) -> dict:
    """--art <folder>: its backdrop.* and logo.* (the first of each by art_find's order)."""
    if not folder.is_dir():
        sys.exit(f"make_installer --art: {folder} is not a folder")
    art = {}
    for base in ("backdrop", "logo"):
        art[base] = next((folder / f"{base}.{e}" for e in ART_EXT if (folder / f"{base}.{e}").is_file()), None)
    if not art["backdrop"] and not art["logo"]:
        sys.exit(f"make_installer --art: no backdrop.* or logo.* ({'/'.join(ART_EXT)}) in {folder}")
    art["icon"] = None
    return art


def rc_path(p: Path) -> str:
    return '"' + p.resolve().as_posix() + '"'


def build_stub(work: Path, count: int, ghash: str, test_build: bool = False, art: dict | None = None,
               name: str | None = None) -> Path:
    """The program. test_build (-DSLY2_TEST_BUILD, tests only, never released): updates may also come over
    plain HTTP from 127.0.0.1 and be signed by a /pubkey= test key. art (--art): the backdrop and logo as RCDATA
    resources ART_BACKDROP / ART_LOGO (installer/launcher.c load_art), art["icon"] (a .ico) as the program's icon."""
    import make_icon
    make_icon.make_ico(work / "setup.ico")
    (work / "setup.manifest").write_text(MANIFEST.format(ver=f"0.1.{count}.0"), encoding="utf-8")
    icon = "setup.ico"
    lines = []
    if art:
        if art.get("icon"):
            icon = art["icon"].resolve().as_posix()
        lines = [f"ART_{k.upper()} RCDATA {rc_path(art[k])}" for k in ("backdrop", "logo") if art.get(k)]
    rc = work / ("setup-art.rc" if art else "setup.rc")
    rc.write_text(RC.format(count=count, hash=ghash, icon=icon, art="\n".join(lines)), encoding="utf-8")
    res = rc.with_name(rc.stem + "_res.o")
    r = subprocess.run([str(WINDRES), "-I", str(work), rc.name, "-O", "coff", "-o", str(res)], cwd=work,
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("resource compiler failed:\n" + r.stderr + r.stdout)
    stub = work / (name or ("setup-stub-test.exe" if test_build else "setup-stub.exe"))
    srcs = [str(p) for p in sorted(SRC.glob("*.c"))]
    defs = ["-DSLY2_TEST_BUILD"] if test_build else []
    r = subprocess.run([str(CLANG), "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-municode", "-mwindows", *defs,
                        "-I", str(work), "-I", str(SRC), *srcs, str(res), *LIBS, "-s", "-o", str(stub)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("compiling the setup program failed:\n" + r.stderr[:8000])
    return stub


def make_art_icon(work: Path, count: int, ghash: str, art: dict) -> None:
    """art["icon"] = the logo's mask on the badge, made by the program itself (installer/art.c, /makeicon ... logo):
    a first build with the procedural icon makes it, the real build then carries it as icon 1."""
    ico = work / "owner-icon.ico"
    ico.unlink(missing_ok=True)
    if not art["logo"]:
        print("make_installer --art: no logo.* in the folder: the program keeps its own (procedural) icon")
        return
    tool = build_stub(work, count, ghash, name="setup-stub-icontool.exe")
    try:
        r = subprocess.run([str(tool), "/makeicon", str(art["logo"]), str(ico), "logo"], capture_output=True,
                           text=True, timeout=300)
    finally:
        tool.unlink(missing_ok=True)
    if r.returncode or not ico.exists():
        print(f"make_installer --art: WARNING: {art['logo'].name} is not the logo whose mask makes the icon "
              f"({(r.stdout or '').strip()}): the program keeps its own (procedural) icon")
        ico.unlink(missing_ok=True)
        return
    art["icon"] = ico


def ico_images(path: Path) -> list[bytes]:
    data = path.read_bytes()
    _r, _t, n = struct.unpack_from("<HHH", data, 0)
    out = []
    for i in range(n):
        size, off = struct.unpack_from("<II", data, 6 + 16 * i + 8)
        out.append(data[off:off + size])
    return out


def art_allow_list(art: dict) -> dict[str, str]:
    """SHA-256 -> what, of everything --art puts into the program: the two pictures as they are (RCDATA) and each
    image of the badge icon (RT_ICON). check_no_game_data allows exactly these (and only as the program's resources)."""
    allowed = {}
    for k in ("backdrop", "logo"):
        if art[k]:
            allowed[sha256(art[k])] = f"ART_{k.upper()} ({art[k].name})"
    if art["icon"]:
        for i, img in enumerate(ico_images(art["icon"])):
            allowed[hashlib.sha256(img).hexdigest()] = f"icon image {i + 1} (the logo's mask badge)"
    return allowed


def check_setup(setup: Path, allowed: set[str]) -> None:
    import check_no_game_data
    problems: list[str] = []
    check_no_game_data.check_setup_exe(setup, problems, allowed)
    if problems:
        setup.unlink()
        sys.exit("make_installer: refusing this Setup (something besides the owner's art):\n  " + "\n  ".join(problems))


def assemble(stub: Path, entries: list[tuple[str, Path]], out: Path) -> None:
    """stub + entry data + table + trailer (installer/util.c payload_open)."""
    tmp = out.with_suffix(".tmp")
    table = []
    with open(tmp, "wb") as f:
        f.write(stub.read_bytes())
        for name, path in entries:
            off = f.tell()
            h = hashlib.sha256()
            with open(path, "rb") as src:
                for chunk in iter(lambda: src.read(1 << 20), b""):
                    h.update(chunk)
                    f.write(chunk)
            table.append(struct.pack("<96sQQ32s", name.encode("utf-8"), off, f.tell() - off, h.digest()))
        tab = f.tell()
        for t in table:
            f.write(t)
        f.write(struct.pack("<8sIIQ", b"SLY2PAY1", len(table), 0, tab))
    os.replace(tmp, out)


def make_source_zip(path: Path, count: int, ghash: str) -> int:
    """source.zip: what Setup carries and what an update bundle is (scripts/make_release.py). Deterministic
    (fixed entry times), so the same commit gives the same bundle and SHA-256."""
    files = payload_files()
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            info = zipfile.ZipInfo(f, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, (ROOT / f).read_bytes(), compresslevel=9)
        info = zipfile.ZipInfo("version.txt", date_time=(2020, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, f"{count} {ghash}\n")
    # nothing of the game, ever: no picture, icon or sound, none of the disc's files (game art is made on the
    # player's PC: scripts/disc_art.py, notes/disc-art.md)
    import check_no_game_data
    problems: list[str] = []
    check_no_game_data.check_zip(path.name, path.read_bytes(), problems)
    if problems:
        path.unlink()
        sys.exit("make_installer: refusing to package game data:\n  " + "\n  ".join(problems))
    return len(files)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build" / "installer"))
    ap.add_argument("--offline", action="store_true", help="also build Sly2-Setup-Offline.exe (toolchain included)")
    ap.add_argument("--test-build", action="store_true",
                    help="also build Sly2-Setup-Test.exe for the update tests (plain HTTP to 127.0.0.1, /pubkey=); never release it")
    ap.add_argument("--art", metavar="FOLDER",
                    help="build the owner's pictures into the program: <folder>\\backdrop.* and logo.* as the window's "
                         "pictures, the logo's mask badge as the program's icon (the release Setup: notes/install.md)")
    args = ap.parse_args()
    out = Path(args.out)
    work = out / "work"
    work.mkdir(parents=True, exist_ok=True)

    pins, pyver = parse_pins()
    for p in pins:
        p.update(archive_info(p))
    count, ghash = version()
    write_config(work / "setup_config.h", pins, pyver, count, ghash)
    art = find_art(Path(args.art).resolve()) if args.art else None
    allow_file = out / "Sly2-Setup.art-allow.txt"
    if art:
        make_art_icon(work, count, ghash, art)
    elif allow_file.exists():
        allow_file.unlink()                          # (from an earlier --art build into this folder)
    stub = build_stub(work, count, ghash, art=art)

    src_zip = work / "source.zip"
    nfiles = make_source_zip(src_zip, count, ghash)
    setup = out / "Sly2-Setup.exe"
    assemble(stub, [("source.zip", src_zip)], setup)
    print(f"{setup}: {setup.stat().st_size / 2**20:.1f} MB (program {stub.stat().st_size // 1024} KB, "
          f"{nfiles} source files, version 0.1.{count} {ghash})")
    if art:
        allowed = art_allow_list(art)
        allow_file.write_text("".join(f"{h}  {what}\n" for h, what in allowed.items()), encoding="utf-8")
        check_setup(setup, set(allowed))
        print(f"Setup includes the owner's art from {Path(args.art).resolve()} "
              f"({', '.join(p.name for p in (art['backdrop'], art['logo']) if p)}"
              f"{', the logo badge as its icon' if art['icon'] else ''}); allow-list for check_no_game_data: {allow_file}")
    if args.test_build:
        test_stub = build_stub(work, count, ghash, test_build=True, art=art)
        test = out / "Sly2-Setup-Test.exe"
        assemble(test_stub, [("source.zip", src_zip)], test)
        print(f"{test}: test build (loopback HTTP + test keys allowed) - for tests only")

    if args.offline:
        pydir = next((d for d in (TOOLS / "python").glob(f"cpython-{pyver}-windows-x86_64-*")
                      if d.is_dir() and not os.path.isjunction(d) and (d / "python.exe").exists()), None)
        if not pydir:
            sys.exit(f"make_installer --offline: no tools/python/cpython-{pyver}-windows-x86_64-* (run setup.ps1)")
        py_zip = work / "python.zip"
        if not py_zip.exists() or py_zip.stat().st_mtime < (pydir / "python.exe").stat().st_mtime:
            with zipfile.ZipFile(py_zip, "w", zipfile.ZIP_DEFLATED) as z:
                zip_tree(z, pydir, pydir.name + "/", skip=lambda rel: "__pycache__/" in rel)
        cache = TOOLS / "uv-cache"
        keep = ("archive-v0/", "wheels-v7/", "simple-v26/", "CACHEDIR.TAG", ".gitignore")
        cache_zip = work / "uv-cache.zip"
        with zipfile.ZipFile(cache_zip, "w", zipfile.ZIP_DEFLATED) as z:
            zip_tree(z, cache, "", skip=lambda rel: not rel.startswith(keep) or rel.endswith(".lock") or rel == ".lock")
        entries = [("source.zip", src_zip)] + [(p["file"], p["local"]) for p in pins] + \
                  [("python.zip", py_zip), ("uv-cache.zip", cache_zip)]
        off = out / "Sly2-Setup-Offline.exe"
        assemble(stub, entries, off)
        if art:
            check_setup(off, set(art_allow_list(art)))
        print(f"{off}: {off.stat().st_size / 2**20:.1f} MB")

    # checksums next to the files (for a release page)
    with open(out / "SHA256SUMS.txt", "w", encoding="utf-8") as f:
        for exe in sorted(e for e in out.glob("Sly2-Setup*.exe") if e.name != "Sly2-Setup-Test.exe"):
            f.write(f"{sha256(exe)}  {exe.name}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
