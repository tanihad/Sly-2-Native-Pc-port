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
from paths import CLANG, runtime_sources  # noqa: E402

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
    sources = sorted((BUILD / "recomp").glob("*.c")) + runtime_sources()
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
    return 1 if failures or not check_overrides() else 0


def check_overrides() -> bool:
    """recomp/overrides.py replaces a function only if its bytes are the expected ones."""
    sys.path.insert(0, str(ROOT))
    from recomp.overrides import OVERRIDES, Override, function_sha1, override_for

    class Prog:                                     # 8 words at 0x1000
        def __init__(self, words): self.words = words
        def read_u32(self, a): return self.words[(a - 0x1000) // 4] if 0x1000 <= a < 0x1020 else None

    words = [0x3C07FFFF, 0x0000302D, 0x34E7F000, 0, 0x03E00008, 0, 0, 0]
    prog = Prog(words)
    saved = dict(OVERRIDES)
    try:
        OVERRIDES.clear()
        OVERRIDES[0x1000] = Override("t", function_sha1(prog, 0x1000, 0x1020), "return;", "test")
        ok = override_for(prog, 0x1000, 0x1020) is not None
        ok &= override_for(Prog(words[:-1] + [1]), 0x1000, 0x1020) is None      # different bytes
        ok &= override_for(prog, 0x1004, 0x1020) is None                         # not in the table
    finally:
        OVERRIDES.clear()
        OVERRIDES.update(saved)
    print(f"  {'PASS' if ok else 'FAIL'}  native overrides apply only to the expected bytes")
    return ok


if __name__ == "__main__":
    sys.exit(main())
