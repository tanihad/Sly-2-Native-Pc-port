"""Random VU microprograms for the recompiler's differential test (tests/unit/vu_recomp/run.ps1).

Writes <out>/programs.bin (a list of images: unit, pair count, pairs) and the recompiled C
(recomp/vu_recomp.py) of every image into <out>/gen/. test_vu_recomp.c runs each program with the
interpreter and with the compiled code from the same random states and compares everything.

The programs are random mixes of every instruction the recompiler handles, with the timing-
sensitive patterns made likely: results read 0-4 pairs after they are written, flag reads near
flag writers, Q/P reads around DIV/EFU, conditional branches on just-written VI registers (the
branch quirk), forward branches and bounded loops, BAL/JR subroutines, JR to targets that are not
return points (the compiled code hands over to the interpreter), branches in delay slots.
    <python> gen.py <out dir> [--count 160] [--seed 1]
"""
from __future__ import annotations

import argparse
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from recomp.vu_decode import decode  # noqa: E402
from recomp.vu_recomp import Image, write_units  # noqa: E402

U1 = {"add": 0x00, "sub": 0x04, "madd": 0x08, "msub": 0x0C, "max": 0x10, "mini": 0x14, "mul": 0x18}  # + bc
U1X = {"mulq": 0x1C, "maxi": 0x1D, "muli": 0x1E, "minii": 0x1F, "addq": 0x20, "maddq": 0x21, "addi": 0x22,
       "maddi": 0x23, "subq": 0x24, "msubq": 0x25, "subi": 0x26, "msubi": 0x27, "add": 0x28, "madd": 0x29,
       "mul": 0x2A, "max": 0x2B, "sub": 0x2C, "msub": 0x2D, "opmsub": 0x2E, "mini": 0x2F}
U2BC = {"adda": 0, "suba": 4, "madda": 8, "msuba": 12, "mula": 24}
U2 = {"itof0": 16, "itof4": 17, "itof12": 18, "itof15": 19, "ftoi0": 20, "ftoi4": 21, "ftoi12": 22,
      "ftoi15": 23, "mulaq": 28, "abs": 29, "mulai": 30, "clip": 31, "addaq": 32, "maddaq": 33, "addai": 34,
      "maddai": 35, "subaq": 36, "msubaq": 37, "subai": 38, "msubai": 39, "adda": 40, "madda": 41,
      "mula": 42, "suba": 44, "msuba": 45, "opmula": 46, "nop": 47}
L7 = {"lq": 0x00, "sq": 0x01, "ilw": 0x04, "isw": 0x05, "iaddiu": 0x08, "isubiu": 0x09, "fceq": 0x10,
      "fcset": 0x11, "fcand": 0x12, "fcor": 0x13, "fseq": 0x14, "fsset": 0x15, "fsand": 0x16, "fsor": 0x17,
      "fmeq": 0x18, "fmand": 0x1A, "fmor": 0x1B, "fcget": 0x1C, "b": 0x20, "bal": 0x21, "jr": 0x24,
      "jalr": 0x25, "ibeq": 0x28, "ibne": 0x29, "ibltz": 0x2C, "ibgtz": 0x2D, "iblez": 0x2E, "ibgez": 0x2F}
LS1 = {"iadd": 0x30, "isub": 0x31, "iaddi": 0x32, "iand": 0x34, "ior": 0x35}
LS2 = {"move": 0x30, "mr32": 0x31, "lqi": 0x34, "sqi": 0x35, "lqd": 0x36, "sqd": 0x37, "div": 0x38, "sqrt": 0x39,
       "rsqrt": 0x3A, "waitq": 0x3B, "mtir": 0x3C, "mfir": 0x3D, "ilwr": 0x3E, "iswr": 0x3F, "rnext": 0x40,
       "rget": 0x41, "rinit": 0x42, "rxor": 0x43, "mfp": 0x64, "xtop": 0x68, "xitop": 0x69, "xgkick": 0x6C,
       "esadd": 0x70, "ersadd": 0x71, "eleng": 0x72, "erleng": 0x73, "eatanxy": 0x74, "eatanxz": 0x75,
       "esum": 0x76, "esqrt": 0x78, "ersqrt": 0x79, "ercpr": 0x7A, "waitp": 0x7B, "esin": 0x7C, "eatan": 0x7D,
       "eexp": 0x7E}
L_NOP = 0x8000033C
U_NOP = 0x000002FF
EFU = ["esadd", "ersadd", "eleng", "erleng", "eatanxy", "eatanxz", "esum", "esqrt", "ersqrt", "ercpr", "esin",
       "eatan", "eexp"]
# VI registers random instructions may write; vi12-vi15 are kept for loops/calls
VIW = list(range(1, 12))


def up(op, dest=15, fd=0, fs=0, ft=0, bc=0, flags=0):
    """flags: I E M D T bits (I = 0x10 ... T = 0x01)."""
    if op in U2 or (op in U2BC):
        k = U2[op] if op in U2 else U2BC[op] + bc
        return flags << 27 | dest << 21 | ft << 16 | fs << 11 | (k >> 2) << 6 | 0x3C | (k & 3)
    code = U1[op] + bc if op in U1 and op not in U1X else U1X[op]
    return flags << 27 | dest << 21 | ft << 16 | fs << 11 | fd << 6 | code


def up_bc(op, dest, fd, fs, ft, bc, flags=0):
    return flags << 27 | dest << 21 | ft << 16 | fs << 11 | fd << 6 | (U1[op] + bc)


def lo7(op, dest=0, t=0, s=0, imm11=0):
    return L7[op] << 25 | dest << 21 | t << 16 | s << 11 | (imm11 & 0x7FF)


def lo_s1(op, t=0, s=0, d=0):
    return 0x40 << 25 | t << 16 | s << 11 | d << 6 | LS1[op]


def lo_s2(op, dest=0, t=0, s=0, fsf=0, ftf=0):
    k = LS2[op]
    if op in ("div", "rsqrt", "sqrt", "mtir", "rinit", "rxor") or op in EFU:
        dest = (ftf << 2) | fsf
    return 0x40 << 25 | dest << 21 | t << 16 | s << 11 | (k >> 2) << 6 | 0x3C | (k & 3)


def iaddiu(t, s, imm):
    return L7["iaddiu"] << 25 | ((imm >> 11) & 15) << 21 | t << 16 | s << 11 | (imm & 0x7FF)


class Gen:
    def __init__(self, rng: random.Random, unit: int):
        self.r, self.unit = rng, unit
        self.pairs: list[list[int]] = []    # [upper, lower]

    def reg(self):
        r = self.r
        return r.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, r.randrange(32)])

    def dest(self):
        return self.r.choice([15, 15, 8, 4, 2, 1, 14, 7, 3, 12, 5, 10, 6, 9, 11, 13, 0])

    def rand_upper(self, flags=0):
        r = self.r
        k = r.random()
        if r.random() < 0.03:
            flags |= r.choice([0x01, 0x02, 0x04])  # T, D, M bits: ignored by both (D/T logged once)
        if k < 0.3:
            return U_NOP | flags << 27
        if k < 0.55:
            op = r.choice(list(U1))
            return up_bc(op, self.dest(), self.reg(), self.reg(), self.reg(), r.randrange(4), flags)
        if k < 0.75:
            op = r.choice(list(U1X))
            return up(op, self.dest(), self.reg(), self.reg(), self.reg(), flags=flags)
        if k < 0.85:
            op = r.choice(list(U2BC))
            return up(op, self.dest(), 0, self.reg(), self.reg(), r.randrange(4), flags)
        op = r.choice(list(U2))
        if op == "clip":
            return up(op, 14, 0, self.reg(), self.reg(), flags=flags)
        return up(op, self.dest(), 0, self.reg(), self.reg(), flags=flags)

    def rand_lower(self, allow_kick=True):
        r = self.r
        vi = r.choice(VIW)
        any_vi = r.randrange(16)
        ops = ["lq", "sq", "ilw", "isw", "iaddiu", "isubiu", "iadd", "isub", "iaddi", "iand", "ior",
               "fceq", "fcset", "fcand", "fcor", "fseq", "fsset", "fsand", "fsor", "fmeq", "fmand", "fmor", "fcget",
               "move", "mr32", "lqi", "sqi", "lqd", "sqd", "div", "sqrt", "rsqrt", "waitq", "mtir", "mfir",
               "ilwr", "iswr", "rnext", "rget", "rinit", "rxor", "mfp", "waitp", "xtop", "xitop", "nop", "nop"]
        if self.unit == 1:
            ops += EFU + ["mfp", "waitp"]
            if allow_kick:
                ops += ["xgkick"]
        op = r.choice(ops)
        lane = 1 << r.randrange(4)
        if op == "nop":
            return L_NOP
        if op == "lq":
            return lo7(op, self.dest(), self.reg(), any_vi, r.randrange(-40, 40))
        if op == "sq":
            return lo7(op, self.dest(), any_vi, self.reg(), r.randrange(-40, 40))
        if op in ("ilw", "isw"):
            return lo7(op, lane, vi if op == "ilw" else any_vi, any_vi, r.randrange(-40, 40))
        if op in ("iaddiu", "isubiu"):
            return iaddiu(vi, any_vi, r.randrange(0x8000)) | (L7[op] - L7["iaddiu"]) << 25
        if op in ("iadd", "isub", "iand", "ior"):
            return lo_s1(op, any_vi, r.randrange(16), vi)
        if op == "iaddi":
            return lo_s1(op, vi, any_vi, r.randrange(32))
        if op in ("fceq", "fcand", "fcor", "fcset"):
            return L7[op] << 25 | r.choice([0, 1, 0x3F, 0xFFF, 0xFFFFFF, r.randrange(1 << 24)])
        if op in ("fseq", "fsand", "fsor", "fsset"):
            imm = r.choice([0x1, 0x2, 0x3, 0xF, 0x30, 0xFC0, 0xFFF, r.randrange(1 << 12)])
            return L7[op] << 25 | (imm >> 11) << 21 | vi << 16 | (imm & 0x7FF)
        if op in ("fmeq", "fmand", "fmor"):
            return lo7(op, 0, vi, any_vi)
        if op == "fcget":
            return lo7(op, 0, vi)
        if op in ("move", "mr32"):
            return lo_s2(op, self.dest(), self.reg(), self.reg())
        if op in ("lqi", "lqd"):
            return lo_s2(op, self.dest(), self.reg(), vi)
        if op in ("sqi", "sqd"):
            return lo_s2(op, self.dest(), vi, self.reg())
        if op in ("div", "rsqrt"):
            return lo_s2(op, 0, self.reg(), self.reg(), r.randrange(4), r.randrange(4))
        if op == "sqrt":
            return lo_s2(op, 0, self.reg(), 0, 0, r.randrange(4))
        if op == "mtir":
            return lo_s2(op, 0, vi, self.reg(), r.randrange(4))
        if op == "mfir":
            return lo_s2(op, self.dest(), self.reg(), any_vi)
        if op in ("ilwr", "iswr"):
            return lo_s2(op, lane, vi if op == "ilwr" else any_vi, any_vi)
        if op in ("rnext", "rget", "mfp"):
            return lo_s2(op, self.dest(), self.reg())
        if op in ("rinit", "rxor"):
            return lo_s2(op, 0, 0, self.reg(), r.randrange(4))
        if op in ("xtop", "xitop"):
            return lo_s2(op, 0, vi)
        if op == "xgkick":
            return lo_s2(op, 0, 0, any_vi)
        if op in EFU:
            return lo_s2(op, 0, 0, self.reg(), r.randrange(4))
        return lo_s2(op)                    # waitq, waitp

    def emit(self, u, l):
        self.pairs.append([u, l])
        return len(self.pairs) - 1

    def program(self, runaway: bool = False) -> list[int]:
        r = self.r
        n = r.randrange(20, 110)
        loop_at = r.randrange(n) if r.random() < 0.5 else -1
        sub_at = r.randrange(n) if r.random() < 0.5 else -1
        jr_at = r.randrange(n) if r.random() < 0.15 else -1
        jalr_at = r.randrange(n) if r.random() < 0.25 else -1
        mid_e_at = r.randrange(n) if r.random() < 0.3 else -1
        fixups = []
        loops = []                       # (counter init, last pair) of each loop: no jumps into them
        loop_start = None
        i = 0
        while len(self.pairs) < n:
            kind = r.random()
            if len(self.pairs) == loop_at and loop_start is None:
                init = self.emit(U_NOP, iaddiu(15, 0, r.randrange(1, 4)))
                loop_start = len(self.pairs)
                loop_at = len(self.pairs) + r.randrange(2, 12)
            elif len(self.pairs) >= loop_at > 0 and loop_start is not None:
                self.emit(self.rand_upper(), lo_s1("iaddi", 15, 15, 31))                 # vi15 -= 1
                if r.random() < 0.5:
                    self.emit(self.rand_upper(), self.rand_lower())
                off = loop_start - (len(self.pairs) + 1)
                self.emit(self.rand_upper(), lo7("ibne", 0, 15, 0, off))
                loops.append((init, self.emit(self.rand_upper(), self.rand_lower())))  # delay slot
                loop_start, loop_at = None, -1
            elif loop_start is not None:
                self.emit(self.rand_upper(), self.rand_lower())                         # loop body: no jumps
            elif len(self.pairs) == sub_at:
                fixups.append(("bal", self.emit(U_NOP, 0)))
                self.emit(self.rand_upper(), self.rand_lower())
                sub_at = -1
            elif len(self.pairs) == jr_at:
                # JR to a pair that is no return point (the compiled code hands over to the interpreter)
                fixups.append(("jr", self.emit(U_NOP, 0)))
                self.emit(U_NOP, lo7("jr", 0, 0, 12))
                self.emit(self.rand_upper(), self.rand_lower())
                jr_at = -1
            elif len(self.pairs) == jalr_at:
                # JALR through vi13: to the subroutine (a routine start: compiled) or elsewhere (handoff)
                fixups.append(("jalr", self.emit(U_NOP, 0), r.random() < 0.7))
                self.emit(self.rand_upper(), lo7("jalr", 0, 14, 13))
                self.emit(self.rand_upper(), self.rand_lower())
                jalr_at = -1
            elif len(self.pairs) == mid_e_at:
                # end of a routine in the middle: the pair after its delay slot is another entry
                self.emit(self.rand_upper(0x08), self.rand_lower(False))
                self.emit(self.rand_upper(), self.rand_lower(False))
                mid_e_at = -1
            elif kind < 0.08 and len(self.pairs) + 3 < n:
                # forward conditional branch, often on a just-written VI register
                b = r.choice(["ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"])
                rt, rs = r.choice(VIW + [0]), r.choice(VIW)
                if r.random() < 0.5:                                                   # chain writes
                    for _ in range(r.randrange(1, 4)):
                        self.emit(self.rand_upper(), lo_s1("iaddi", rs, rs, r.randrange(32)) if r.random() < 0.7
                                  else iaddiu(rs, r.randrange(16), r.randrange(8)))
                fixups.append(("fwd", self.emit(U_NOP, lo7(b, 0, rt, rs, 0)), r.randrange(2, 10)))
                ds = self.rand_lower()
                if r.random() < 0.03:
                    ds = lo7("b", 0, 0, 0, r.randrange(1, 4))                         # branch in the delay slot
                # E bit in a branch delay slot: the pair at the target is the E delay slot
                self.emit(self.rand_upper(0x08 if r.random() < 0.04 else 0), ds)
            elif kind < 0.1 and len(self.pairs) + 3 < n:
                fixups.append(("b", self.emit(self.rand_upper(), lo7("b")), r.randrange(2, 8)))
                self.emit(self.rand_upper(), self.rand_lower())
            elif kind < 0.16:
                self.emit(self.rand_upper(0x10), struct.unpack("<I", struct.pack("<f", r.choice(
                    [1.0, -2.5, 0.0078740781, 3.1, 1e-30, 1e30, 160.0]))) [0] if r.random() < 0.8 else r.randrange(1 << 32))
            else:
                self.emit(self.rand_upper(), self.rand_lower())
            i += 1
        if runaway:                                                                     # no E bit ever
            k = self.emit(self.rand_upper(), lo7("b", 0, 0, 0, -1))
            self.emit(self.rand_upper(), self.rand_lower())
        end = len(self.pairs)
        kind = r.random()
        if kind < 0.12:                                    # E bit on a branch: TPC = its target
            fixups.append(("tosub", self.emit(self.rand_upper(0x08), lo7("b"))))
        elif kind < 0.24:
            fixups.append(("tosub", self.emit(self.rand_upper(0x08), lo7(
                r.choice(["ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"]), 0, r.choice(VIW), r.choice(VIW)))))
        else:
            self.emit(self.rand_upper(0x08), self.rand_lower(False) if r.random() < 0.7 else L_NOP)   # E bit
        if kind >= 0.24 and r.random() < 0.15:             # a branch in the E delay slot (BAL writes its link)
            fixups.append(("tosub", self.emit(self.rand_upper(), lo7(r.choice(["b", "bal"]), 0, 14))))
        else:
            self.emit(self.rand_upper(), self.rand_lower(False))                                    # delay slot
        # subroutine after the end: a few pairs, JR vi14
        sub = len(self.pairs)
        self.sub = sub
        for _ in range(r.randrange(1, 5)):
            self.emit(self.rand_upper(), self.rand_lower())
        self.emit(U_NOP, lo7("jr", 0, 0, 14))
        self.emit(self.rand_upper(), self.rand_lower())
        def outside_loops(t: int) -> int:
            for a, b in loops:
                if a < t <= b:
                    return b + 1
            return t
        for f in fixups:
            k = f[1]
            if f[0] == "bal":
                self.pairs[k][1] = lo7("bal", 0, 14, 0, sub - (k + 1))
            elif f[0] == "jr":
                self.pairs[k][1] = iaddiu(12, 0, outside_loops(min(k + 3 + r.randrange(0, 6), end)))
            elif f[0] == "jalr":
                self.pairs[k][1] = iaddiu(13, 0, sub if f[2] else outside_loops(min(k + 3 + r.randrange(0, 6), end)))
            elif f[0] == "tosub":
                self.pairs[k][1] = (self.pairs[k][1] & ~0x7FF) | ((sub - (k + 1)) & 0x7FF)
            else:
                tgt = outside_loops(min(k + f[2], end))
                self.pairs[k][1] = (self.pairs[k][1] & ~0x7FF) | ((tgt - (k + 1)) & 0x7FF)
        return [u << 32 | l for u, l in self.pairs]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--count", type=int, default=160)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    out = Path(args.out)
    rng = random.Random(args.seed)
    images, subs = [], []
    for i in range(args.count):
        unit = 1 if i % 4 else 0
        g = Gen(rng, unit)
        raws = g.program(runaway=i == 7)            # one program without an end (runaway guard)
        if unit == 0 and len(raws) > 500:
            raws = raws[:500]
        for pc, raw in enumerate(raws):                                  # the encoder must round-trip
            op = decode(raw, pc, 0x7FF if unit else 0x1FF)
            assert op.up != "invalid" and op.lo != "invalid", (i, pc, hex(raw))
        # entries: the image start and every routine start (recomp default_entries), as the game uses
        images.append(Image(f"fuzz{i}", unit, [(0, raws)]))
        subs.append(g.sub)
    out.mkdir(parents=True, exist_ok=True)
    report = write_units(images, out / "gen", "fz")
    # programs.bin: unit, pair count, entry count, entries (all but those in the subroutine after the
    # end, which returns through a random vi14), pairs
    blob = bytearray()
    for img, sub in zip(images, subs):
        raws = img.chunks[0][1]
        ents = [e for e in img.entries if e < sub]
        blob += struct.pack("<III", img.unit, len(raws), len(ents)) + b"".join(struct.pack("<I", e) for e in ents)
        blob += b"".join(struct.pack("<Q", x) for x in raws)
    (out / "programs.bin").write_bytes(bytes(blob))
    hand = sum(1 for line in report if "handoffs" in line)
    print(f"{len(images)} random programs, {hand} with handoff sites -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
