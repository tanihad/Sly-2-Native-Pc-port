"""End-to-end test: synthetic PS2 ELF -> recomp -> clang -> run natively -> compare registers.

Run from the project root:  .venv\\Scripts\\python.exe tests\\run_tests.py
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "scripts"))
import make_test_elf  # noqa: E402
from paths import CLANG  # noqa: E402

BUILD = ROOT / "build" / "test"


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
    sources = sorted((BUILD / "recomp").glob("*.c")) + sorted((ROOT / "runtime").glob("*.c"))
    cc = run([CLANG, "-O2", "-std=c11", "-Wall", "-Wno-unused-label",
              "-I", ROOT / "runtime", "-I", BUILD / "recomp", *sources,
              "-lwinmm", "-lgdi32", "-luser32", "-lxinput1_4", "-lsynchronization", "-o", exe])
    if cc.returncode or cc.stderr.strip():
        print("compile output:\n" + cc.stderr)
        if cc.returncode:
            return 1

    res = run([exe, elf, "--dump-regs"])
    regs = {}
    for line in res.stdout.splitlines():
        name, val = line.split("=")
        regs[int(name[1:])] = int(val, 16)

    checks = [(reg, want, 64) for reg, want in make_test_elf.EXPECTED.items()] + \
             [(reg, want, 128) for reg, want in make_test_elf.EXPECTED128.items()]
    failures = 0
    for reg, want, bits in checks:
        name = make_test_elf.NAMES[reg]
        if reg not in regs:
            failures += 1
            print(f"  FAIL  ${name}: missing (exit {res.returncode}, stderr: {res.stderr.strip()[-300:]})")
            continue
        got = regs[reg] & ((1 << bits) - 1)
        ok = got == want
        failures += not ok
        width = bits // 4 + 2
        print(f"  {'PASS' if ok else 'FAIL'}  ${name:2}: got {got:#0{width}x}  want {want:#0{width}x}")
    print(f"\n{len(checks) - failures}/{len(checks)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
