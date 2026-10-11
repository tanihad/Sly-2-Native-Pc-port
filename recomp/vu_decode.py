"""VU micro-mode instruction decoder for the VU recompiler (recomp/vu_recomp.py).

This mirrors runtime/vu_exec.c's decoder field for field (operation ids, operand fields, the VF/VI
registers each pair reads and writes, write latencies, the FDIV/EFU stall kind, the VI-chain
information for the conditional-branch quirk), because the recompiled programs have to reproduce
the interpreter's timing model exactly. Change both together.

A pair is 64 bits: lower instruction in bits 0-31, upper in bits 32-63; upper bits 31-27 are the
I E M D T flags. With I set, the lower word is the immediate loaded into I.
"""
from __future__ import annotations

from dataclasses import dataclass, field

# upper-word control bits, shifted down (bits >> 27)
B_T, B_D, B_M, B_E, B_I = 0x01, 0x02, 0x04, 0x08, 0x10

# The instruction's 4-bit dest field has x in bit 3; internally lane 0 = x is bit 0.
REV4 = [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]

# second operand of an upper op: ft, ft broadcast lane, Q, I
BM_VEC, BM_BC, BM_Q, BM_I = range(4)

STALL_NONE, STALL_Q, STALL_P, STALL_EFU = range(4)

# ---- upper ----------------------------------------------------------------------------------------

def _bc4(op: str, acc: int) -> list:
    return [(op, BM_BC, acc)] * 4


# bits 5-0 below 0x30
UPPER1 = (_bc4("add", 0) + _bc4("sub", 0) + _bc4("madd", 0) + _bc4("msub", 0) + _bc4("max", 0)
          + _bc4("mini", 0) + _bc4("mul", 0) + [
              ("mul", BM_Q, 0), ("max", BM_I, 0), ("mul", BM_I, 0), ("mini", BM_I, 0),
              ("add", BM_Q, 0), ("madd", BM_Q, 0), ("add", BM_I, 0), ("madd", BM_I, 0),
              ("sub", BM_Q, 0), ("msub", BM_Q, 0), ("sub", BM_I, 0), ("msub", BM_I, 0),
              ("add", BM_VEC, 0), ("madd", BM_VEC, 0), ("mul", BM_VEC, 0), ("max", BM_VEC, 0),
              ("sub", BM_VEC, 0), ("msub", BM_VEC, 0), ("opmsub", BM_VEC, 0), ("mini", BM_VEC, 0)])
# bits 5-2 = 1111: index ((bits 10-6) << 2) | bits 1-0, below 0x30
UPPER2 = (_bc4("add", 1) + _bc4("sub", 1) + _bc4("madd", 1) + _bc4("msub", 1) + [
    ("itof0", 0, 0), ("itof4", 0, 0), ("itof12", 0, 0), ("itof15", 0, 0),
    ("ftoi0", 0, 0), ("ftoi4", 0, 0), ("ftoi12", 0, 0), ("ftoi15", 0, 0)] + _bc4("mul", 1) + [
    ("mul", BM_Q, 1), ("abs", 0, 0), ("mul", BM_I, 1), ("clip", 0, 0),
    ("add", BM_Q, 1), ("madd", BM_Q, 1), ("add", BM_I, 1), ("madd", BM_I, 1),
    ("sub", BM_Q, 1), ("msub", BM_Q, 1), ("sub", BM_I, 1), ("msub", BM_I, 1),
    ("add", BM_VEC, 1), ("madd", BM_VEC, 1), ("mul", BM_VEC, 1), ("invalid", 0, 0),
    ("sub", BM_VEC, 1), ("msub", BM_VEC, 1), ("opmula", BM_VEC, 1), ("nop", 0, 0)])
assert len(UPPER1) == 48 and len(UPPER2) == 48

FMAC_OPS = ("add", "sub", "mul", "madd", "msub")
CONV_OPS = ("abs", "ftoi0", "ftoi4", "ftoi12", "ftoi15", "itof0", "itof4", "itof12", "itof15")

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

# EFU latencies in cycles (ps2tek), as runtime/vu_exec.c efu_latency()
EFU_LATENCY = {"esadd": 11, "ersadd": 18, "eleng": 18, "erleng": 24, "ersqrt": 18, "esum": 12,
               "esqrt": 12, "ercpr": 12, "esin": 29, "eexp": 44, "eatan": 54, "eatanxy": 54,
               "eatanxz": 54}
# VU_E_* ids in runtime/vu_ops.h
EFU_KIND = {"esadd": "VU_E_ESADD", "ersadd": "VU_E_ERSADD", "eleng": "VU_E_ELENG",
            "erleng": "VU_E_ERLENG", "eatanxy": "VU_E_EATANXY", "eatanxz": "VU_E_EATANXZ",
            "esum": "VU_E_ESUM", "esqrt": "VU_E_ESQRT", "ersqrt": "VU_E_ERSQRT",
            "ercpr": "VU_E_ERCPR", "esin": "VU_E_ESIN", "eatan": "VU_E_EATAN", "eexp": "VU_E_EEXP"}

COND_BRANCHES = ("ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez")
BRANCHES = ("b", "bal", "jr", "jalr") + COND_BRANCHES
STATUS_READERS = ("fseq", "fsand", "fsor", "fsset")
MAC_READERS = ("fmeq", "fmand", "fmor")
CLIP_READERS = ("fceq", "fcand", "fcor", "fcget")


@dataclass
class VuOp:
    pc: int
    raw: int                    # the pair (lower | upper << 32)
    bits: int = 0               # B_* flags
    # upper
    up: str = "nop"
    ubm: int = 0
    uacc: int = 0
    udst: int = 0               # lane mask, bit0 = x
    ufd: int = 0
    ufs: int = 0
    uft: int = 0
    ubc: int = 0
    # lower
    lo: str = "nop"
    ldst: int = 0
    lt: int = 0
    ls: int = 0
    ld: int = 0
    fsf: int = 0
    ftf: int = 0
    imm: int = 0                # immediate (sign-extended), branch target, I value or EFU latency
    # hazard bookkeeping (as vu_op in runtime/vu_internal.h)
    srcs: list = field(default_factory=list)    # [[reg, mask]] VF registers read (max 4), merged
    visrc: list = field(default_factory=lambda: [0, 0])
    uw: tuple | None = None     # (reg, mask) VF written by the upper op
    lw: tuple | None = None     # (reg, mask) VF written by the lower op
    viw: int = 0
    viwlat: int = 0
    vichain: int = 0
    vichain_rd: int = 0
    stall: int = STALL_NONE

    @property
    def lower_word(self) -> int:
        return self.raw & 0xFFFFFFFF

    @property
    def upper_word(self) -> int:
        return self.raw >> 32

    @property
    def check(self) -> bool:
        return bool(self.srcs or self.visrc[0] or self.stall)

    @property
    def is_branch(self) -> bool:
        return self.lo in BRANCHES

    @property
    def flag_writer(self) -> bool:
        """Upper ops that queue MAC/status flags (push_mac)."""
        return self.up in FMAC_OPS or self.up in ("opmula", "opmsub")

    def add_src(self, reg: int, mask: int) -> None:
        if not reg or not mask:
            return                          # vf0 is constant: never stalls
        for s in self.srcs:
            if s[0] == reg:
                s[1] |= mask
                return
        if len(self.srcs) < 4:
            self.srcs.append([reg, mask])

    def add_visrc(self, reg: int) -> None:
        if not reg:
            return
        if not self.visrc[0]:
            self.visrc[0] = reg
        else:
            self.visrc[1] = reg

    def set_viw(self, reg: int, lat: int, chain: int) -> None:
        if not reg:
            return
        self.viw, self.viwlat = reg, lat
        if chain:
            self.vichain = reg

    def set_lw(self, reg: int, mask: int) -> None:
        if reg and mask:
            self.lw = (reg, mask)


def _lowest_lane(m: int) -> int:
    return 0 if m & 1 else 1 if m & 2 else 2 if m & 4 else 3 if m & 8 else 0


def _decode_upper(op: VuOp, u: int) -> None:
    dst, ft, fs, fd = REV4[(u >> 21) & 15], (u >> 16) & 31, (u >> 11) & 31, (u >> 6) & 31
    f6 = u & 63
    e = ("invalid", 0, 0)
    if f6 < 0x30:
        e = UPPER1[f6]
    elif f6 >= 0x3C:
        k = (((u >> 6) & 31) << 2) | (u & 3)
        if k < 0x30:
            e = UPPER2[k]
    name, bm, acc = e
    op.up, op.ubm, op.uacc, op.udst = name, bm, acc, dst
    op.ufd, op.ufs, op.uft, op.ubc = fd, fs, ft, u & 3
    if name in FMAC_OPS or name in ("max", "mini"):
        op.add_src(fs, dst)
        if bm == BM_VEC:
            op.add_src(ft, dst)
        elif bm == BM_BC:
            op.add_src(ft, 1 << (u & 3))
        if not acc and fd and dst:
            op.uw = (fd, dst)
        if name in ("max", "mini") and bm == BM_Q:
            op.up = "nop"                   # no such encoding (the C table has no handler)
    elif name in ("opmula", "opmsub"):
        op.add_src(fs, 7)
        op.add_src(ft, 7)
        if name == "opmsub" and fd and dst:
            op.uw = (fd, dst)
    elif name in CONV_OPS:
        op.add_src(fs, dst)
        if ft and dst:
            op.uw = (ft, dst)
    elif name == "clip":
        op.add_src(fs, 7)
        op.add_src(ft, 8)


def _decode_lower(op: VuOp, l: int, pc: int, pcmask: int) -> None:
    op7, ldst = l >> 25, REV4[(l >> 21) & 15]
    t, s = (l >> 16) & 31, (l >> 11) & 31
    it, is_, id_ = t & 15, s & 15, (l >> 6) & 15
    imm11 = (l & 0x7FF) - 0x800 if l & 0x400 else l & 0x7FF
    target = (pc + 1 + imm11) & pcmask
    if op7 == 0x40:
        f6 = l & 63
        lop = LOWER_S2.get((((l >> 6) & 31) << 2) | (l & 3), "invalid") if f6 >= 0x3C else LOWER_S1.get(f6, "invalid")
    else:
        lop = LOWER7.get(op7, "invalid")
    op.ldst = ldst
    op.fsf = (l >> 21) & 3
    op.ftf = (l >> 23) & 3
    if lop == "lq":
        op.lt, op.ls, op.imm = t, is_, imm11
        op.add_visrc(is_); op.set_lw(t, ldst)
    elif lop == "sq":
        op.ls, op.lt, op.imm = s, it, imm11
        op.add_src(s, ldst); op.add_visrc(it)
    elif lop in ("ilw", "ilwr"):
        op.lt, op.ls, op.imm = it, is_, imm11 if lop == "ilw" else 0
        op.fsf = _lowest_lane(ldst)
        op.add_visrc(is_); op.set_viw(it, 4, 1)
    elif lop in ("isw", "iswr"):
        op.lt, op.ls, op.imm = it, is_, imm11 if lop == "isw" else 0
        op.add_visrc(is_); op.add_visrc(it)
    elif lop in ("iaddiu", "isubiu"):
        op.lt, op.ls, op.imm = it, is_, ((l >> 10) & 0x7800) | (l & 0x7FF)
        op.add_visrc(is_); op.set_viw(it, 1, 1)
    elif lop in ("fceq", "fcand", "fcor"):
        op.imm = l & 0xFFFFFF
        op.set_viw(1, 1, 0)
    elif lop == "fcset":
        op.imm = l & 0xFFFFFF
    elif lop == "fcget":
        op.lt = it
        op.set_viw(it, 1, 0)
    elif lop in ("fseq", "fsand", "fsor"):
        op.lt, op.imm = it, ((l >> 10) & 0x800) | (l & 0x7FF)
        op.set_viw(it, 1, 0)
    elif lop == "fsset":
        op.imm = ((l >> 10) & 0x800) | (l & 0x7FF)
    elif lop in ("fmeq", "fmand", "fmor"):
        op.lt, op.ls = it, is_
        op.add_visrc(is_); op.set_viw(it, 1, 0)
    elif lop == "b":
        op.imm = target
    elif lop == "bal":
        op.lt, op.imm = it, target
        op.set_viw(it, 1, 1)
    elif lop == "jr":
        op.ls = is_
        op.add_visrc(is_)
    elif lop == "jalr":
        op.lt, op.ls = it, is_
        op.add_visrc(is_); op.set_viw(it, 1, 1)
    elif lop in ("ibeq", "ibne"):
        op.lt, op.ls, op.imm = it, is_, target
        op.add_visrc(it); op.add_visrc(is_)
    elif lop in ("ibltz", "ibgtz", "iblez", "ibgez"):
        op.ls, op.imm = is_, target
        op.add_visrc(is_)
    elif lop in ("iadd", "isub", "iand", "ior"):
        op.ld, op.ls, op.lt = id_, is_, it
        op.add_visrc(is_); op.add_visrc(it); op.set_viw(id_, 1, 1)
    elif lop == "iaddi":
        op.lt, op.ls = it, is_
        v = (l >> 6) & 31
        op.imm = v - 32 if v & 16 else v
        op.add_visrc(is_); op.set_viw(it, 1, 1)
    elif lop in ("move", "mr32"):
        if not t or not ldst:
            lop = "nop"                     # MOVE.none vf0, vf0 is the canonical NOP
        else:
            op.lt, op.ls = t, s
            op.add_src(s, ldst if lop == "move" else (((ldst << 1) | (ldst >> 3)) & 15))
            op.set_lw(t, ldst)
    elif lop in ("lqi", "lqd"):
        op.lt, op.ls = t, is_
        op.add_visrc(is_); op.set_lw(t, ldst); op.set_viw(is_, 1, 1)
    elif lop in ("sqi", "sqd"):
        op.ls, op.lt = s, it
        op.add_src(s, ldst); op.add_visrc(it); op.set_viw(it, 1, 1)
    elif lop in ("div", "rsqrt"):
        op.ls, op.lt = s, t
        op.add_src(s, 1 << op.fsf); op.add_src(t, 1 << op.ftf)
        op.stall = STALL_Q
    elif lop == "sqrt":
        op.lt = t
        op.add_src(t, 1 << op.ftf)
        op.stall = STALL_Q
    elif lop == "waitq":
        op.stall = STALL_Q
    elif lop == "mtir":
        op.lt, op.ls = it, s
        op.add_src(s, 1 << op.fsf); op.set_viw(it, 1, 1)
    elif lop == "mfir":
        if not t or not ldst:
            lop = "nop"
        else:
            op.lt, op.ls = t, is_
            op.add_visrc(is_); op.set_lw(t, ldst)
    elif lop in ("rget", "mfp"):
        if not t or not ldst:
            lop = "nop"
        else:
            op.lt = t
            op.set_lw(t, ldst)
    elif lop == "rnext":                    # advances R even without a destination
        op.lt = t
        op.set_lw(t, ldst)
    elif lop in ("rinit", "rxor"):
        op.ls = s
        op.add_src(s, 1 << op.fsf)
    elif lop in ("xtop", "xitop"):
        op.lt = it
        op.set_viw(it, 1, 1)
    elif lop == "xgkick":
        op.ls = is_
        op.add_visrc(is_)
    elif lop in ("esadd", "ersadd", "eleng", "erleng", "eatanxy", "eatanxz", "esum"):
        op.ls, op.stall, op.imm = s, STALL_EFU, EFU_LATENCY[lop]
        op.add_src(s, 3 if lop == "eatanxy" else 5 if lop == "eatanxz" else 15 if lop == "esum" else 7)
    elif lop in ("esqrt", "ersqrt", "ercpr", "esin", "eatan", "eexp"):
        op.ls, op.stall, op.imm = s, STALL_EFU, EFU_LATENCY[lop]
        op.add_src(s, 1 << op.fsf)
    elif lop == "waitp":
        op.stall = STALL_P
    op.lo = lop


def decode(raw: int, pc: int, pcmask: int) -> VuOp:
    """Decode the pair at instruction address pc (64-bit units)."""
    op = VuOp(pc=pc, raw=raw)
    u, l = raw >> 32, raw & 0xFFFFFFFF
    op.bits = u >> 27
    _decode_upper(op, u)
    if op.bits & B_I:
        op.lo, op.imm = "loi", l
    else:
        _decode_lower(op, l, pc, pcmask)
    op.vichain_rd = 1 if op.vichain and op.vichain in op.visrc else 0
    return op


def branch_targets(op: VuOp, pcmask: int) -> list[int] | None:
    """Static successors after the delay slot of a branch pair: [target] for B/BAL, [target,
    fall-through] for conditional branches, None for JR/JALR (dynamic)."""
    after = (op.pc + 2) & pcmask
    if op.lo in ("b", "bal"):
        return [op.imm]
    if op.lo in COND_BRANCHES:
        return [op.imm, after]
    return None
