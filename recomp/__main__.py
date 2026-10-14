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
from .hooks import hook_for
from .overrides import override_for
from .patches import patches_for


def translate_all(path: str):
    prog = load(path)
    funcs = find_functions(prog)
    starts = {f.start for f in funcs}
    stats: Counter = Counter()
    bodies = []
    for f in funcs:
        hook = hook_for(prog, f.start, f.end) is not None   # entry hook (recomp/hooks.py)
        stats["entry hooks"] += hook
        o = override_for(prog, f.start, f.end)        # native replacement (recomp/overrides.py)
        p = patches_for(prog, f.start, f.end)         # instruction patches (recomp/patches.py)
        if o:
            if p:
                raise ValueError(f"f_{f.start:08X} has both a native override and instruction patches")
            stats["native overrides"] += 1
            bodies.append(override_body(f, o, hook))
        else:
            bodies.append(FunctionTranslator(f, starts, stats).translate(hook, p))
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
    hooked = "".join(f"0x{f.start:08X}u, " for f in funcs if hook_for(prog, f.start, f.end))
    elf = Path(args.elf).read_bytes()
    write_if_changed(out / "func_table.c",
                     '#include "recomp_funcs.h"\n\n'
                     f"const ps2_func_entry ps2_funcs[] = {{\n{table}\n}};\n"
                     f"const size_t ps2_func_count = {len(funcs)};\n"
                     f"const uint32_t ps2_entry_point = 0x{prog.entry:08X}u;\n"
                     "/* functions with an entry hook (recomp/hooks.py), 0-terminated */\n"
                     f"const uint32_t ps2_hook_addrs[] = {{ {hooked}0 }};\n"
                     "/* the executable this code was recompiled from: a disc image must hold exactly this\n"
                     " * one to be run without an ELF on the command line (runtime/launch.c) */\n"
                     f"const uint32_t ps2_elf_size = {len(elf)}u;\n"
                     f'const char ps2_elf_sha256[] = "{hashlib.sha256(elf).hexdigest()}";\n')

    unimpl = sum(v for k, v in stats.items() if k.startswith("UNIMPL "))
    files = (len(bodies) + args.per_file - 1) // args.per_file
    print(f"wrote {len(funcs):,} functions in {files} files to {out} ({unimpl:,} untranslated instructions, "
          f"{stats['native overrides']} native overrides, {stats['entry hooks']} entry hooks, "
          f"{stats['instruction patches']} instruction patches)")


def cmd_iop(args) -> None:
    from .iop import generate
    print(generate(args.elf, args.output))


def cmd_vu(args) -> None:
    from .vu_recomp import Image, game_images, write_units
    if args.image:
        data = Path(args.elf).read_bytes()
        base = int(args.base, 0) // 8
        raws = [int.from_bytes(data[i:i + 8], "little") for i in range(0, len(data) - 7, 8)]
        ents = [int(e, 0) // 8 for e in args.entries.split(",")] if args.entries else []
        images = [Image(args.name, args.unit, [(base, raws)], ents)]
    else:
        images = game_images(args.elf)
    report = write_units(images, Path(args.output), args.prefix)
    if args.verbose:
        print("\n".join(report))
    print(f"wrote {len(images)} recompiled VU microcode units to {args.output}")


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
    v = sub.add_parser("vu", help="write C source for the game's VU microprograms (recomp/vu_recomp.py)")
    v.add_argument("elf", help="the game ELF, or with --image a raw micro memory file")
    v.add_argument("-o", "--output", default="build/vu")
    v.add_argument("--image", action="store_true", help="elf is raw micro code (little-endian pairs)")
    v.add_argument("--base", default="0", help="--image: VU byte address of the file's first pair")
    v.add_argument("--unit", type=int, default=1, help="--image: VU number")
    v.add_argument("--name", default="image", help="--image: unit name")
    v.add_argument("--entries", default="", help="--image: entry byte addresses (default: start + after E bits)")
    v.add_argument("--prefix", default="vuc", help="C symbol prefix")
    v.add_argument("-v", "--verbose", action="store_true")
    v.set_defaults(fn=cmd_vu)
    d = sub.add_parser("disasm", help="disassemble an address range")
    d.add_argument("elf")
    d.add_argument("addr", help="start address in hex")
    d.add_argument("-n", "--count", type=int, default=40)
    d.set_defaults(fn=cmd_disasm)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
