"""VU (PS2 Vector Unit) micro-mode disassembler.

Usage:
    python vudis.py --sly2 <SCUS_973.16>          disassemble every microprogram in the ELF's .vutext,
                                                   using the dvp-as overlay table (.DVP.ovlytab)
    python vudis.py --sly2 <elf> --stats           summary only (sizes, op histogram, timing tricks)
    python vudis.py --bin <file> [--base 0x0]      raw micro code (little-endian 64-bit pairs)

Encodings follow Sony's VU User's Manual (micro instruction set): each 64-bit pair holds the
lower instruction in bits 0-31 and the upper instruction in bits 32-63; upper bits 31-27 are the
I, E, M, D, T flags. With I set, the lower word is a float immediate loaded into I ("loi").
Self-contained: no third-party modules.
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field

LANES = "xyzw"


def dest(d: int) -> str:
    """4-bit dest field (x = bit 3) -> '.xyzw' suffix."""
    return "." + "".join(c for i, c in enumerate(LANES) if d & (8 >> i)) if d else ""


def vf(n: int) -> str:
    return f"vf{n:02d}"


def vi(n: int) -> str:
    return f"vi{n & 15:02d}"


def sext(v: int, bits: int) -> int:
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def f32(u: int) -> float:
    return struct.unpack("<f", struct.pack("<I", u))[0]


# ---- upper ----------------------------------------------------------------------------------------

BC = "xyzw"
UPPER1 = {}
for i, base in enumerate(["add", "sub", "madd", "msub", "max", "mini", "mul"]):
    for b in range(4):
        UPPER1[i * 4 + b] = (base + BC[b], "bc")
UPPER1.update({0x1C: ("mulq", "q"), 0x1D: ("maxi", "i"), 0x1E: ("muli", "i"), 0x1F: ("minii", "i"),
               0x20: ("addq", "q"), 0x21: ("maddq", "q"), 0x22: ("addi", "i"), 0x23: ("maddi", "i"),
               0x24: ("subq", "q"), 0x25: ("msubq", "q"), 0x26: ("subi", "i"), 0x27: ("msubi", "i"),
               0x28: ("add", "v"), 0x29: ("madd", "v"), 0x2A: ("mul", "v"), 0x2B: ("max", "v"),
               0x2C: ("sub", "v"), 0x2D: ("msub", "v"), 0x2E: ("opmsub", "v"), 0x2F: ("mini", "v")})
UPPER2 = {}
for i, base in enumerate(["adda", "suba", "madda", "msuba"]):
    for b in range(4):
        UPPER2[i * 4 + b] = (base + BC[b], "abc")
for b in range(4):
    UPPER2[0x18 + b] = ("mula" + BC[b], "abc")
for k, n in enumerate([0, 4, 12, 15]):
    UPPER2[0x10 + k] = (f"itof{n}", "conv")
    UPPER2[0x14 + k] = (f"ftoi{n}", "conv")
UPPER2.update({0x1C: ("mulaq", "aq"), 0x1D: ("abs", "conv"), 0x1E: ("mulai", "ai"), 0x1F: ("clipw", "clip"),
               0x20: ("addaq", "aq"), 0x21: ("maddaq", "aq"), 0x22: ("addai", "ai"), 0x23: ("maddai", "ai"),
               0x24: ("subaq", "aq"), 0x25: ("msubaq", "aq"), 0x26: ("subai", "ai"), 0x27: ("msubai", "ai"),
               0x28: ("adda", "av"), 0x29: ("madda", "av"), 0x2A: ("mula", "av"), 0x2C: ("suba", "av"),
               0x2D: ("msuba", "av"), 0x2E: ("opmula", "av"), 0x2F: ("nop", "nop")})


def upper_name(u: int) -> tuple[str, str]:
    f6 = u & 63
    if f6 < 0x30:
        return UPPER1[f6]
    if f6 >= 0x3C:
        k = ((u >> 6) & 31) << 2 | (u & 3)
        if k in UPPER2:
            return UPPER2[k]
    return ("INVALID", "bad")


def dis_upper(u: int) -> str:
    name, form = upper_name(u)
    d, ft, fs, fd = (u >> 21) & 15, (u >> 16) & 31, (u >> 11) & 31, (u >> 6) & 31
    m = name + dest(d)
    if form == "nop":
        return "nop"
    if form == "bad":
        return f"INVALID 0x{u & 0x07FFFFFF:07X}"
    if form == "bc":
        return f"{m} {vf(fd)}, {vf(fs)}, {vf(ft)}{BC[u & 3]}"
    if form in ("q", "i"):
        return f"{m} {vf(fd)}, {vf(fs)}, {form.upper()}"
    if form == "v":
        return f"{m} {vf(fd)}, {vf(fs)}, {vf(ft)}"
    if form == "abc":
        return f"{m} ACC, {vf(fs)}, {vf(ft)}{BC[u & 3]}"
    if form in ("aq", "ai"):
        return f"{m} ACC, {vf(fs)}, {form[1].upper()}"
    if form == "av":
        return f"{m} ACC, {vf(fs)}, {vf(ft)}"
    if form == "conv":
        return f"{m} {vf(ft)}, {vf(fs)}"
    if form == "clip":
        return f"clipw.xyz {vf(fs)}, {vf(ft)}w"
    return name


# ---- lower ----------------------------------------------------------------------------------------

LOWER7 = {0x00: "lq", 0x01: "sq", 0x04: "ilw", 0x05: "isw", 0x08: "iaddiu", 0x09: "isubiu",
          0x10: "fceq", 0x11: "fcset", 0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset",
          0x16: "fsand", 0x17: "fsor", 0x18: "fmeq", 0x1A: "fmand", 0x1B: "fmor", 0x1C: "fcget",
          0x20: "b", 0x21: "bal", 0x24: "jr", 0x25: "jalr", 0x28: "ibeq", 0x29: "ibne",
          0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}
LOWER_S1 = {0x30: "iadd", 0x31: "isub", 0x32: "iaddi", 0x34: "iand", 0x35: "ior"}
LOWER_S2 = {0x30: "move", 0x31: "mr32", 0x34: "lqi", 0x35: "sqi", 0x36: "lqd", 0x37: "sqd",
            0x38: "div", 0x39: "sqrt", 0x3A: "rsqrt", 0x3B: "waitq", 0x3C: "mtir", 0x3D: "mfir",
            0x3E: "ilwr", 0x3F: "iswr", 0x40: "rnext", 0x41: "rget", 0x42: "rinit", 0x43: "rxor",
            0x64: "mfp", 0x68: "xtop", 0x69: "xitop", 0x6C: "xgkick", 0x70: "esadd", 0x71: "ersadd",
            0x72: "eleng", 0x73: "erleng", 0x74: "eatanxy", 0x75: "eatanxz", 0x76: "esum",
            0x78: "esqrt", 0x79: "ersqrt", 0x7A: "ercpr", 0x7B: "waitp", 0x7C: "esin", 0x7D: "eatan",
            0x7E: "eexp"}
BRANCHES = {"b", "bal", "jr", "jalr", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"}
EFU = {"esadd", "ersadd", "eleng", "erleng", "eatanxy", "eatanxz", "esum", "esqrt", "ersqrt",
       "ercpr", "esin", "eatan", "eexp"}


def lower_name(l: int) -> str:
    op7 = l >> 25
    if op7 == 0x40:
        f6 = l & 63
        if f6 >= 0x3C:
            return LOWER_S2.get(((l >> 6) & 31) << 2 | (l & 3), "INVALID")
        return LOWER_S1.get(f6, "INVALID")
    return LOWER7.get(op7, "INVALID")


def is_lower_nop(l: int) -> bool:
    n = lower_name(l)
    return n == "move" and (((l >> 21) & 15) == 0 or ((l >> 16) & 31) == 0)


def dis_lower(l: int, pc: int, pcmask: int) -> tuple[str, int | None]:
    """Returns (text, branch target or None). pc is in 64-bit units."""
    n = lower_name(l)
    d, t, s, dd = (l >> 21) & 15, (l >> 16) & 31, (l >> 11) & 31, (l >> 6) & 31
    imm11 = sext(l & 0x7FF, 11)
    fsf, ftf = BC[(l >> 21) & 3], BC[(l >> 23) & 3]
    target = (pc + 1 + imm11) & pcmask
    m = n + dest(d)
    if n == "INVALID":
        return f"INVALID 0x{l:08X}", None
    if is_lower_nop(l):
        return "nop", None
    if n == "lq":
        return f"{m} {vf(t)}, {imm11}({vi(s)})", None
    if n == "sq":
        return f"{m} {vf(s)}, {imm11}({vi(t)})", None
    if n in ("ilw", "isw"):
        return f"{m} {vi(t)}, {imm11}({vi(s)})", None
    if n in ("iaddiu", "isubiu"):
        return f"{n} {vi(t)}, {vi(s)}, 0x{((l >> 10) & 0x7800) | (l & 0x7FF):X}", None
    if n in ("fceq", "fcand", "fcor"):
        return f"{n} vi01, 0x{l & 0xFFFFFF:06X}", None
    if n == "fcset":
        return f"{n} 0x{l & 0xFFFFFF:06X}", None
    if n == "fcget":
        return f"{n} {vi(t)}", None
    imm12 = ((l >> 10) & 0x800) | (l & 0x7FF)
    if n in ("fseq", "fsand", "fsor"):
        return f"{n} {vi(t)}, 0x{imm12:03X}", None
    if n == "fsset":
        return f"{n} 0x{imm12:03X}", None
    if n in ("fmeq", "fmand", "fmor"):
        return f"{n} {vi(t)}, {vi(s)}", None
    if n == "b":
        return f"b 0x{target * 8:04X}", target
    if n == "bal":
        return f"bal {vi(t)}, 0x{target * 8:04X}", target
    if n == "jr":
        return f"jr {vi(s)}", None
    if n == "jalr":
        return f"jalr {vi(t)}, {vi(s)}", None
    if n in ("ibeq", "ibne"):
        return f"{n} {vi(t)}, {vi(s)}, 0x{target * 8:04X}", target
    if n in ("ibltz", "ibgtz", "iblez", "ibgez"):
        return f"{n} {vi(s)}, 0x{target * 8:04X}", target
    if n in ("iadd", "isub", "iand", "ior"):
        return f"{n} {vi(dd)}, {vi(s)}, {vi(t)}", None
    if n == "iaddi":
        return f"iaddi {vi(t)}, {vi(s)}, {sext(dd, 5)}", None
    if n in ("move", "mr32"):
        return f"{m} {vf(t)}, {vf(s)}", None
    if n in ("lqi", "lqd"):
        return (f"{m} {vf(t)}, ({vi(s)}++)" if n == "lqi" else f"{m} {vf(t)}, (--{vi(s)})"), None
    if n in ("sqi", "sqd"):
        return (f"{m} {vf(s)}, ({vi(t)}++)" if n == "sqi" else f"{m} {vf(s)}, (--{vi(t)})"), None
    if n in ("div", "rsqrt"):
        return f"{n} Q, {vf(s)}{fsf}, {vf(t)}{ftf}", None
    if n == "sqrt":
        return f"sqrt Q, {vf(t)}{ftf}", None
    if n in ("waitq", "waitp"):
        return n, None
    if n == "mtir":
        return f"mtir {vi(t)}, {vf(s)}{fsf}", None
    if n == "mfir":
        return f"{m} {vf(t)}, {vi(s)}", None
    if n in ("ilwr", "iswr"):
        return f"{m} {vi(t)}, ({vi(s)})", None
    if n in ("rnext", "rget"):
        return f"{m} {vf(t)}, R", None
    if n in ("rinit", "rxor"):
        return f"{n} R, {vf(s)}{fsf}", None
    if n == "mfp":
        return f"{m} {vf(t)}, P", None
    if n in ("xtop", "xitop"):
        return f"{n} {vi(t)}", None
    if n == "xgkick":
        return f"xgkick {vi(s)}", None
    if n in ("esadd", "ersadd", "eleng", "erleng", "eatanxy", "eatanxz", "esum"):
        return f"{n} P, {vf(s)}", None
    if n in EFU:
        return f"{n} P, {vf(s)}{fsf}", None
    return n, None


@dataclass
class Pair:
    pc: int
    upper: int
    lower: int

    @property
    def flags(self) -> str:
        return "".join(c for b, c in zip((31, 30, 29, 28, 27), "IEMDT") if self.upper >> b & 1)


def disassemble(pairs: list[Pair], pcmask: int) -> list[str]:
    targets = set()
    for p in pairs:
        if not p.upper >> 31 & 1:
            _, t = dis_lower(p.lower, p.pc, pcmask)
            if t is not None:
                targets.add(t)
    out = []
    for p in pairs:
        up = dis_upper(p.upper)
        if p.upper >> 31 & 1:
            lo = f"loi {f32(p.lower):.9g}  (0x{p.lower:08X})"
        else:
            lo, _ = dis_lower(p.lower, p.pc, pcmask)
        fl = f"[{p.flags}]" if p.flags else ""
        lab = "L:" if p.pc in targets else "  "
        out.append(f"{lab} {p.pc * 8:04X}: {p.upper:08X} {p.lower:08X}  {fl:5s} {up:36s} {lo}")
        if p.upper >> 30 & 1:
            out.append("")                   # blank line after the E bit's delay slot is clearer below
    return out


# ---- ELF + dvp-as overlay table ---------------------------------------------------------------

def elf_sections(b: bytes) -> dict[str, tuple[int, int, int]]:
    shoff = struct.unpack_from("<I", b, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 46)
    secs = [struct.unpack_from("<10I", b, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    out = {}
    for s in secs:
        name = b[stroff + s[0]:b.index(b"\0", stroff + s[0])].decode()
        out[name] = (s[3], s[4], s[5])       # addr, file offset, size
    return out


@dataclass
class Overlay:
    name: str
    ee_addr: int
    vu_addr: int
    size: int


@dataclass
class Program:
    ident: str
    chunks: list[Overlay] = field(default_factory=list)

    @property
    def size(self) -> int:
        return sum(c.size for c in self.chunks)


def sly2_programs(b: bytes) -> tuple[list[Program], dict]:
    secs = elf_sections(b)
    taddr, toff, tsize = secs[".DVP.ovlytab"]
    _, soff, ssize = secs[".DVP.ovlystrtab"]
    strtab = b[soff:soff + ssize]
    progs: dict[str, Program] = {}
    for i in range(0, tsize, 12):
        nameoff, ee, vu = struct.unpack_from("<3I", b, toff + i)
        name = strtab[nameoff:strtab.index(b"\0", nameoff)].decode()
        size = secs[name][2]
        parts = name.split("..", 1)[1].split(".")   # <vuaddr>.<id>.<line>.<chunk>
        progs.setdefault(parts[1], Program(parts[1])).chunks.append(Overlay(name, ee, vu, size))
    return list(progs.values()), secs


def program_pairs(b: bytes, secs: dict, prog: Program) -> list[Pair]:
    vaddr, voff, _ = secs[".vutext"]
    pairs = []
    for c in prog.chunks:
        off = voff + (c.ee_addr - vaddr)
        for k in range(c.size // 8):
            lo, up = struct.unpack_from("<II", b, off + 8 * k)
            pairs.append(Pair(c.vu_addr // 8 + k, up, lo))
    return pairs


# ---- statistics ---------------------------------------------------------------------------------

def stats(pairs: list[Pair], pcmask: int) -> dict:
    ops = Counter()
    ends, entries = [], [pairs[0].pc] if pairs else []
    for i, p in enumerate(pairs):
        un, _ = upper_name(p.upper)
        ops[un] += 1
        if p.upper >> 31 & 1:
            ops["loi"] += 1
        else:
            ln = "nop" if is_lower_nop(p.lower) else lower_name(p.lower)
            ops[ln] += 1
        if p.upper >> 30 & 1:
            ends.append(p.pc)
            if i + 2 < len(pairs):
                entries.append(pairs[i + 2].pc)
    return {"ops": ops, "ends": ends, "entries": entries}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sly2", help="ELF with .vutext and a .DVP.ovlytab overlay table")
    ap.add_argument("--bin", help="raw micro code file")
    ap.add_argument("--base", default="0", help="VU address of the raw file's first byte")
    ap.add_argument("--vu", type=int, default=1, help="VU number (sets the branch wrap mask)")
    ap.add_argument("--stats", action="store_true", help="summary only")
    args = ap.parse_args()
    pcmask = 0x7FF if args.vu == 1 else 0x1FF
    if args.bin:
        data = open(args.bin, "rb").read()
        base = int(args.base, 0) // 8
        pairs = [Pair(base + k, *reversed(struct.unpack_from("<II", data, 8 * k))) for k in range(len(data) // 8)]
        print("\n".join(disassemble(pairs, pcmask)))
        return 0
    if not args.sly2:
        ap.error("need --sly2 <elf> or --bin <file>")
    b = open(args.sly2, "rb").read()
    progs, secs = sly2_programs(b)
    total = Counter()
    for prog in progs:
        pairs = program_pairs(b, secs, prog)
        st = stats(pairs, pcmask)
        total.update(st["ops"])
        ee = ", ".join(f"0x{c.ee_addr:08X}" for c in prog.chunks)
        print(f"== program {prog.ident}: {len(pairs)} pairs ({prog.size:#x} bytes), "
              f"{len(prog.chunks)} MPG chunk(s) at EE {ee}")
        print(f"   E bits at: {', '.join(f'0x{e * 8:04X}' for e in st['ends'])}")
        interesting = ["xgkick", "div", "rsqrt", "sqrt", "waitq", "waitp", "clipw", "fcand", "fcor", "fceq",
                       "fcset", "fcget", "fmand", "fmor", "fmeq", "fsand", "fsor", "fseq", "fsset", "xtop",
                       "xitop", "loi", "b", "bal", "jr", "jalr", "ibeq", "ibne", "ibltz", "ibgtz", "iblez",
                       "ibgez", "mfp", "rnext", "rget", "rinit", "rxor", "INVALID"] + sorted(EFU)
        print("   " + ", ".join(f"{k} {st['ops'][k]}" for k in interesting if st["ops"][k]))
        if not args.stats:
            print("\n".join(disassemble(pairs, pcmask)))
        print()
    print("== all programs: " + ", ".join(f"{k} {v}" for k, v in total.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
