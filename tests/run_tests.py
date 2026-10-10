"""End-to-end test: synthetic PS2 ELF -> recomp -> clang -> run natively -> compare registers.

Run from the project root:  .venv\\Scripts\\python.exe tests\\run_tests.py
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import make_test_elf  # noqa: E402

BUILD = ROOT / "build" / "test"
CLANG = ROOT / "tools" / "llvm-mingw" / "bin" / "clang.exe"


def run(cmd: list) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, cwd=ROOT)


def main() -> int:
    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(parents=True)
    elf = BUILD / "test.elf"
    elf.write_bytes(make_test_elf.build())

    gen = run([sys.executable, "-m", "recomp", "generate", elf, "-o", BUILD / "recomp"])
    if gen.returncode:
        print(gen.stdout, gen.stderr)
        return 1

    exe = BUILD / "test.exe"
    sources = sorted((BUILD / "recomp").glob("*.c")) + [ROOT / "runtime" / "ps2_runtime.c"]
    cc = run([CLANG, "-O2", "-std=c11", "-Wall", "-Wno-unused-label",
              "-I", ROOT / "runtime", "-I", BUILD / "recomp", *sources, "-o", exe])
    if cc.returncode or cc.stderr.strip():
        print("compile output:\n" + cc.stderr)
        if cc.returncode:
            return 1

    res = run([exe, elf, "--dump-regs"])
    regs = {}
    for line in res.stdout.splitlines():
        name, val = line.split("=")
        regs[int(name[1:])] = int(val, 16)

    failures = 0
    names = {16 + n: f"s{n}" for n in range(8)}
    for reg, want in make_test_elf.EXPECTED.items():
        got = regs.get(reg)
        ok = got == want
        failures += not ok
        print(f"  {'PASS' if ok else 'FAIL'}  ${names[reg]}: got {got:#018x}  want {want:#018x}" if got is not None
              else f"  FAIL  ${names[reg]}: missing (exit {res.returncode}, stderr: {res.stderr.strip()})")
    print(f"\n{len(make_test_elf.EXPECTED) - failures}/{len(make_test_elf.EXPECTED)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
