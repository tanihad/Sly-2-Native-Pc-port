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
    overrides_ok = check_overrides()
    hooks_ok = check_hooks()
    patches_ok = check_patches()
    return 1 if failures or not overrides_ok or not hooks_ok or not patches_ok else 0


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


def check_hooks() -> bool:
    """recomp/hooks.py adds PS2_HOOK to a function's entry only if its bytes are the expected ones,
    and the generated code then calls it outside PS2_TRACE (so release builds keep it)."""
    sys.path.insert(0, str(ROOT))
    from recomp.codegen import override_body
    from recomp.functions import Function
    from recomp.hooks import HOOKS, Hook, hook_for
    from recomp.overrides import Override, function_sha1

    class Prog:                                     # 8 words at 0x1000
        def __init__(self, words): self.words = words
        def read_u32(self, a): return self.words[(a - 0x1000) // 4] if 0x1000 <= a < 0x1020 else None

    words = [0x3C07FFFF, 0x0000302D, 0x34E7F000, 0, 0x03E00008, 0, 0, 0]
    prog = Prog(words)
    saved = dict(HOOKS)
    try:
        HOOKS.clear()
        HOOKS[0x1000] = Hook("t", function_sha1(prog, 0x1000, 0x1020), "test")
        ok = hook_for(prog, 0x1000, 0x1020) is not None
        ok &= hook_for(Prog(words[:-1] + [1]), 0x1000, 0x1020) is None          # different bytes
        ok &= hook_for(prog, 0x1004, 0x1020) is None                             # not in the table
    finally:
        HOOKS.clear()
        HOOKS.update(saved)
    f = Function(0x1000, 0x1020, "t", [])
    o = Override("t", "", "return;", "test")
    ok &= "PS2_HOOK(0x00001000u);" in override_body(f, o, True)
    ok &= "PS2_HOOK" not in override_body(f, o, False)
    gen = (ROOT / "build" / "test" / "recomp")
    ok &= not any("PS2_HOOK(" in p.read_text(encoding="utf-8") for p in gen.glob("funcs_*.c"))  # test ELF: none
    print(f"  {'PASS' if ok else 'FAIL'}  entry hooks apply only to the expected bytes")
    return ok


def check_patches() -> bool:
    """recomp/patches.py inserts code at an instruction only if the function's bytes are the expected
    ones, before/after exactly that instruction, refuses delay slots and 'after' a jump; the test ELF
    gets none; every widescreen patch tests the aspect first (a no-op at 4:3)."""
    sys.path.insert(0, str(ROOT))
    from collections import Counter
    from recomp.codegen import FunctionTranslator
    from recomp.decode import decode
    from recomp.functions import Function
    from recomp.overrides import function_sha1
    from recomp import patches as P

    class Prog:                                     # 8 words at 0x1000
        def __init__(self, words): self.words = words
        def read_u32(self, a): return self.words[(a - 0x1000) // 4] if 0x1000 <= a < 0x1020 else None

    # lui a3,0xFFFF; daddu a2,zero,zero; ori a3,a3,0xF000; nop; jr ra; nop; nop; nop
    words = [0x3C07FFFF, 0x0000302D, 0x34E7F000, 0, 0x03E00008, 0, 0, 0]
    prog = Prog(words)
    sha = function_sha1(prog, 0x1000, 0x1020)
    saved = list(P.PATCHES)
    ok = True
    try:
        P.PATCHES[:] = [P.Patch("b", 0x1000, sha, 0x1008, "before", "MARK_B();", "t"),
                        P.Patch("a", 0x1000, sha, 0x1008, "after", "MARK_A();", "t")]
        got = P.patches_for(prog, 0x1000, 0x1020)
        ok &= sorted(p.name for p in got.get(0x1008, [])) == ["a", "b"]
        ok &= P.patches_for(Prog(words[:-1] + [1]), 0x1000, 0x1020) == {}     # different bytes
        f = Function(0x1000, 0x1020, "t", [decode(w, 0x1000 + 4 * i) for i, w in enumerate(words)])
        src = FunctionTranslator(f, {0x1000}, Counter()).translate(False, got)
        b, ins, a = src.find("MARK_B();"), src.find("/* 00001008:"), src.find("MARK_A();")
        ok &= 0 <= b < ins < a < src.find("/* 0000100C:")
        for bad in (P.Patch("d", 0x1000, sha, 0x1014, "before", "X();", "t"),       # delay slot of jr
                    P.Patch("j", 0x1000, sha, 0x1010, "after", "X();", "t")):       # after a jump
            try:
                FunctionTranslator(f, {0x1000}, Counter()).translate(False, {bad.at: [bad]})
                ok = False
            except ValueError:
                pass
    finally:
        P.PATCHES[:] = saved
    ok &= all(p.code.startswith("if (ps2_aspect.wide)") for p in P.PATCHES if p.name.startswith("ws_"))
    gen = (ROOT / "build" / "test" / "recomp")
    ok &= not any("recomp/patches.py" in p.read_text(encoding="utf-8") for p in gen.glob("funcs_*.c"))  # test ELF
    print(f"  {'PASS' if ok else 'FAIL'}  instruction patches apply only to the expected bytes and places")
    return ok


if __name__ == "__main__":
    sys.exit(main())
