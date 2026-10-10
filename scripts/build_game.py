"""Recompile the game: ELF -> C (recomp) -> object files (clang, in parallel) -> sly2.exe.

Usage (from the project root):
    .venv\\Scripts\\python.exe scripts\\build_game.py [--elf orig/SCUS_973.16] [-O1]
"""
import argparse
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLANG = ROOT / "tools" / "llvm-mingw" / "bin" / "clang.exe"
OUT = ROOT / "build" / "sly2"


def compile_one(src: Path, opt: str) -> tuple[Path, str]:
    obj = src.with_suffix(".o")
    if obj.exists() and obj.stat().st_mtime > src.stat().st_mtime \
            and obj.stat().st_mtime > (ROOT / "runtime" / "ps2_runtime.h").stat().st_mtime:
        return obj, ""
    res = subprocess.run([str(CLANG), opt, "-std=c11", "-c", "-Wno-unused-label", "-Wno-unused-variable",
                          "-I", str(ROOT / "runtime"), "-I", str(src.parent), str(src), "-o", str(obj)],
                         capture_output=True, text=True)
    return obj, (res.stderr if res.returncode else "")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=str(ROOT / "orig" / "SCUS_973.16"))
    ap.add_argument("-O", dest="opt", default="1", help="clang optimization level (default 1)")
    args = ap.parse_args()
    opt = f"-O{args.opt}"

    t0 = time.time()
    gen = subprocess.run([sys.executable, "-m", "recomp", "generate", args.elf, "-o", str(OUT / "recomp")],
                         cwd=ROOT, capture_output=True, text=True)
    print(gen.stdout.strip() or gen.stderr)
    if gen.returncode:
        return 1

    sources = sorted((OUT / "recomp").glob("*.c")) + [ROOT / "runtime" / "ps2_runtime.c"]
    objs, errors = [], []
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for obj, err in pool.map(lambda s: compile_one(s, opt), sources):
            objs.append(obj)
            if err:
                errors.append(err)
    if errors:
        print(f"{len(errors)} file(s) failed to compile; first errors:\n" + "\n".join(errors)[:4000])
        return 1
    print(f"compiled {len(sources)} files ({opt}) in {time.time() - t0:.0f}s")

    exe = OUT / "sly2.exe"
    link = subprocess.run([str(CLANG), *map(str, objs), "-o", str(exe)], capture_output=True, text=True)
    if link.returncode:
        print(link.stderr[:4000])
        return 1
    print(f"linked {exe.relative_to(ROOT)} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - t0:.0f}s total")
    return 0


if __name__ == "__main__":
    sys.exit(main())
