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


def compile_one(src: Path, obj_dir: Path, flags: list[str]) -> tuple[Path, str]:
    obj = obj_dir / src.with_suffix(".o").name     # never next to the source
    headers = max(h.stat().st_mtime for h in (ROOT / "runtime").glob("*.h"))
    stamp = obj_dir / "flags.txt"
    if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, headers, stamp.stat().st_mtime):
        return obj, ""
    res = subprocess.run([str(CLANG), *flags, "-std=c11", "-c", "-Wno-unused-label", "-Wno-unused-variable",
                          "-I", str(ROOT / "runtime"), "-I", str(src.parent), str(src), "-o", str(obj)],
                         capture_output=True, text=True)
    return obj, (res.stderr if res.returncode else "")


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

    obj_dir = out / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    stamp = obj_dir / "flags.txt"
    if not stamp.exists() or stamp.read_text() != " ".join(flags):
        stamp.write_text(" ".join(flags))              # flag change: rebuild everything

    sources = sorted((out / "recomp").glob("*.c")) + sorted((out / "iop").glob("*.c")) + runtime_sources()
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
