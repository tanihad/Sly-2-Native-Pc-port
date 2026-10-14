"""Recompile the game: ELF -> C (recomp) -> object files (clang, in parallel) -> sly2.exe.

Usage (from the project root or a worktree):
    <main checkout>\\.venv\\Scripts\\python.exe scripts\\build_game.py [--release] [-O 1] [--out build/sly2]

--release: -O2 and no function tracing (the watchdog then can't report locations).
"""
import argparse
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from paths import CLANG, ELF, ROOT, runtime_sources


GPU_DIR = ROOT / "runtime" / "gpu"
GEN_DIR = ROOT / "build" / "gen"                   # generated headers (build/gen/gpu_shaders.h)


def compile_one(src: Path, obj_dir: Path, flags: list[str]) -> tuple[Path, str]:
    gpu = src.parent == GPU_DIR
    obj = obj_dir / (("gpu_" if gpu and not src.name.startswith("gpu_") else "") + src.with_suffix(".o").name)
    if src.parent.name == "vu":
        # recompiled VU microcode: already SIMD; clang's SLP vectorizer only costs minutes on these
        # huge functions (recomp/vu_recomp.py)
        flags = flags + ["-fno-slp-vectorize"]
    deps = list((ROOT / "runtime").glob("*.h"))
    if gpu:                                        # only the GPU backend depends on its headers/shaders
        deps += list(GPU_DIR.glob("*.h")) + [GEN_DIR / "gpu_shaders.h"]
    headers = max(h.stat().st_mtime for h in deps)
    stamp = obj_dir / "flags.txt"
    if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, headers, stamp.stat().st_mtime):
        return obj, ""
    res = subprocess.run([str(CLANG), *flags, "-std=c11", "-c", "-Wno-unused-label", "-Wno-unused-variable",
                          "-I", str(ROOT / "runtime"), "-I", str(src.parent), "-I", str(GEN_DIR), str(src),
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
    res = subprocess.run([str(hlslc), str(GPU_DIR / "shaders" / "perms.txt"), str(GEN_DIR / "gpu_shaders.h")],
                         capture_output=True, text=True)
    if res.returncode:
        return "shader compilation failed:\n" + res.stdout + res.stderr
    print(res.stdout.strip())
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(ELF))
    ap.add_argument("-O", dest="opt", default=None, help="clang optimization level (default 1, or 2 with --release)")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--out", default=str(ROOT / "build" / "sly2"))
    args = ap.parse_args()
    out = Path(args.out)
    # -fno-auto-import: globals are in this executable, so access them directly instead of through
    # a .refptr pointer (MinGW's provision for data imported from DLLs): one load less per access.
    flags = [f"-O{args.opt or ('2' if args.release else '1')}", "-fno-auto-import"]
    if args.release:
        flags.append("-DPS2_NO_TRACE")

    t0 = time.time()
    gen = subprocess.run([sys.executable, "-m", "recomp", "generate", args.elf, "-o", str(out / "recomp")],
                         cwd=ROOT, capture_output=True, text=True)
    print(gen.stdout.strip() or gen.stderr)
    if gen.returncode:
        return 1
    # The IOP sound modules (989snd, spiop) embedded in the executable: recomp/iop.py
    iop = subprocess.run([sys.executable, "-m", "recomp", "iop", args.elf, "-o", str(out / "iop")],
                         cwd=ROOT, capture_output=True, text=True)
    print(iop.stdout.strip() or iop.stderr)
    if iop.returncode:
        return 1
    # The VU microprograms (.vutext overlays): recomp/vu_recomp.py
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
    err = build_shaders()
    if err:
        print(err[:6000])
        return 1

    sources = (sorted((out / "recomp").glob("*.c")) + sorted((out / "iop").glob("*.c")) +
               sorted((out / "vu").glob("*.c")) + runtime_sources() + sorted(GPU_DIR.glob("*.c")))
    objs, errors = [], []
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for obj, err in pool.map(lambda s: compile_one(s, obj_dir, flags), sources):
            objs.append(obj)
            if err:
                errors.append(err)
    if errors:
        print(f"{len(errors)} file(s) failed to compile; first errors:\n" + "\n".join(errors)[:6000])
        return 1
    print(f"compiled {len(sources)} files ({' '.join(flags)}) in {time.time() - t0:.0f}s")

    exe = out / "sly2.exe"
    link = subprocess.run([str(CLANG), *map(str, objs), "-lwinmm", "-lgdi32", "-luser32", "-lxinput1_4",
                           "-lsynchronization", "-o", str(exe)], capture_output=True, text=True)
    if link.returncode:
        print(link.stderr[:6000])
        return 1
    print(f"linked {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.0f}s total")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main())
