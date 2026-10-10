"""Usage:
    python -m recomp analyze  <elf>
    python -m recomp generate <elf> [-o build/recomp] [--per-file 500]
    python -m recomp iop      <elf> [-o build/iop]       (IOP sound modules, see recomp/iop.py)
"""
import argparse
import hashlib
from collections import Counter
from pathlib import Path

from .codegen import FunctionTranslator, cname, override_body
from .elf import load
from .functions import find_functions
from .overrides import override_for


def translate_all(path: str):
    prog = load(path)
    funcs = find_functions(prog)
    starts = {f.start for f in funcs}
    stats: Counter = Counter()
    bodies = []
    for f in funcs:
        o = override_for(prog, f.start, f.end)        # native replacement (recomp/overrides.py)
        if o:
            stats["native overrides"] += 1
            bodies.append(override_body(f, o))
        else:
            bodies.append(FunctionTranslator(f, starts, stats).translate())
    return prog, funcs, bodies, stats


def cmd_analyze(args) -> None:
    data = Path(args.elf).read_bytes()
    prog, funcs, _, stats = translate_all(args.elf)
    print(f"file      {args.elf}  ({len(data):,} bytes, sha1 {hashlib.sha1(data).hexdigest()})")
    print(f"entry     0x{prog.entry:08X}")
    for s in prog.segments:
        kind = "code" if s.executable else "data"
        print(f"segment   0x{s.vaddr:08X}-0x{s.end:08X}  {kind}  file {len(s.data):,} / mem {s.memsz:,} bytes")
    for s in prog.sections:
        print(f"section   0x{s.addr:08X}-0x{s.addr + s.size:08X}  {s.name:10} {'code' if s.executable else ''}")
    print(f"symbols   {len(prog.symbols):,} function symbols in the ELF")
    print(f"functions {len(funcs):,}")

    unimpl = {k[7:]: v for k, v in stats.items() if k.startswith("UNIMPL ")}
    total = sum(v for k, v in stats.items() if not k.startswith("UNIMPL ") and " " not in k)
    print(f"\ninstructions translated: {total:,}")
    print("most common:")
    for op, n in Counter({k: v for k, v in stats.items() if " " not in k}).most_common(25):
        print(f"  {op:12} {n:8,}")
    print(f"\nnot yet translated: {sum(unimpl.values()):,} instructions, {len(unimpl)} kinds")
    for op, n in sorted(unimpl.items(), key=lambda kv: -kv[1]):
        print(f"  {op:12} {n:8,}")
    for k in ("branch at end of function",):
        if stats[k]:
            print(f"\nwarning: {stats[k]} {k}")


def write_if_changed(path: Path, text: str) -> None:
    """Leave unchanged files alone, so their mtime (and the build's object files) stay valid."""
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8")


def cmd_generate(args) -> None:
    prog, funcs, bodies, stats = translate_all(args.elf)
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)

    protos = "\n".join(f"void {cname(f.start)}(ps2_ctx* ctx);" for f in funcs)
    write_if_changed(out / "recomp_funcs.h", f'#pragma once\n#include "ps2_runtime.h"\n\n{protos}\n')

    written = set()
    for n in range(0, len(bodies), args.per_file):
        chunk = "\n\n".join(bodies[n:n + args.per_file])
        name = f"funcs_{n // args.per_file:03}.c"
        write_if_changed(out / name, f'#include "recomp_funcs.h"\n\n{chunk}\n')
        written.add(name)
    for stale in [*out.glob("funcs_*.c"), *out.glob("*.o")]:   # file count can shrink between runs
        if stale.name not in written:
            stale.unlink()

    table = ",\n".join(f"    {{ 0x{f.start:08X}u, {cname(f.start)} }}" for f in funcs)
    write_if_changed(out / "func_table.c",
                     '#include "recomp_funcs.h"\n\n'
                     f"const ps2_func_entry ps2_funcs[] = {{\n{table}\n}};\n"
                     f"const size_t ps2_func_count = {len(funcs)};\n"
                     f"const uint32_t ps2_entry_point = 0x{prog.entry:08X}u;\n")

    unimpl = sum(v for k, v in stats.items() if k.startswith("UNIMPL "))
    files = (len(bodies) + args.per_file - 1) // args.per_file
    print(f"wrote {len(funcs):,} functions in {files} files to {out} ({unimpl:,} untranslated instructions, "
          f"{stats['native overrides']} native overrides)")


def cmd_iop(args) -> None:
    from .iop import generate
    print(generate(args.elf, args.output))


def cmd_disasm(args) -> None:
    from .decode import decode
    prog = load(args.elf)
    start = int(args.addr, 16)
    for n in range(args.count):
        a = start + 4 * n
        w = prog.read_u32(a)
        if w is None:
            break
        print(f"{a:08X}: {w:08X}  {decode(w, a).disassemble()}")


def main() -> None:
    p = argparse.ArgumentParser(prog="recomp", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("analyze", help="report what the executable contains")
    a.add_argument("elf")
    a.set_defaults(fn=cmd_analyze)
    g = sub.add_parser("generate", help="write C source for every function")
    g.add_argument("elf")
    g.add_argument("-o", "--output", default="build/recomp")
    g.add_argument("--per-file", type=int, default=500)
    g.set_defaults(fn=cmd_generate)
    o = sub.add_parser("iop", help="write C source for the game's IOP sound modules (989snd, spiop)")
    o.add_argument("elf")
    o.add_argument("-o", "--output", default="build/iop")
    o.set_defaults(fn=cmd_iop)
    d = sub.add_parser("disasm", help="disassemble an address range")
    d.add_argument("elf")
    d.add_argument("addr", help="start address in hex")
    d.add_argument("-n", "--count", type=int, default=40)
    d.set_defaults(fn=cmd_disasm)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
