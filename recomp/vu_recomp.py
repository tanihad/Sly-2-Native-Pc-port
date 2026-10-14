"""Static recompiler for VU micro programs (VU1 and VU0 micro mode) -> C.

The game's microprograms (the dvp-as overlays in the ELF's .vutext, listed by .DVP.ovlytab) are
translated ahead of time, like the EE code. One compile unit = one overlay image on one VU; its C
function takes an entry index and runs a program from that entry to the pair after its E bit,
exactly as runtime/vu_exec.c's interpreter would: same results, same XGKICK addresses and issue
cycles, same pair and cycle counts. The runtime (runtime/vu.c) picks a unit when a program starts
and the micro memory matches the unit's image; anything else runs in the interpreter.

Timing. The interpreter models a cycle counter with stalls (FMAC/ILW results 4 cycles after
issue, readers stall), Q/P results that land a fixed number of cycles after issue (not hazard
checked), MAC/status and clip flags that land 4 cycles after issue, and the VI-register quirk of
conditional branches. Here all of that is resolved at compile time by *block versioning*: code is
generated per (pc, timing state) where the timing state lists what is still in flight at the
start of the block (relative ready times), so every stall and cycle offset inside a block is a
constant and the generated code only adds constants to its cycle/pair counters. A block reached
with a different state gets another version. The values in flight (Q, P, flags) live in local
variables named by their queue position (q_next, p0/p1, fq0-3, cq0-3).

Flags are folded into the MAC/status (clip) registers at the producing op ("eagerly") when no
instruction that reads them is within the next 3 pairs; folding early is unobservable then. Only
ops near a reader keep their result queued until it lands.

Anything the compiler cannot (or should not) resolve statically -- a JR/JALR to a target outside
its candidate list, a branch in a branch delay slot, an invalid instruction, too many versions of
one block -- hands the program over to the interpreter mid-run: the generated code writes the
registers and the exact pipeline state (scoreboard, queued flags, Q/P, VI history) into vu_state
and calls vu_resume(). The interpreter stays the reference; see notes/vu-microcode.md.

Usage: python -m recomp vu <elf> -o <dir>      (see __main__.py)
"""
from __future__ import annotations

import struct
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

from .vu_decode import (B_E, B_I, BM_BC, BM_I, BM_Q, BM_VEC, CLIP_READERS, COND_BRANCHES, EFU_KIND,
                        MAC_READERS, STALL_EFU, STALL_P, STALL_Q, STATUS_READERS, VuOp, decode)

KEY_FLAGS_DIRTY = False      # True: unapplied FMAC flags carried across blocks (key bit); False: applied at block ends
# VF registers and ACC in a local struct whose address escapes (memory) instead of C locals: no PHI
# nodes for 32 vectors across hundreds of blocks, which made clang's register coalescer superlinear
REGS_IN_MEMORY = True
ENTRY_STICKY = (0xF, 0xB, 0x0)  # sticky-bit specialisations of every entry, most specific first
MAX_VERSIONS_PER_PC = 24
MAX_VERSIONS = 60000
ANY = -1                                  # successor that is not known statically

FMAC_KIND = {"add": "VU_K_ADD", "sub": "VU_K_SUB", "mul": "VU_K_MUL", "madd": "VU_K_MADD",
             "msub": "VU_K_MSUB"}
SCALE = {"0": "1.0f", "4": "16.0f", "12": "4096.0f", "15": "32768.0f"}
ISCALE = {"0": "1.0f", "4": "1.0f / 16.0f", "12": "1.0f / 4096.0f", "15": "1.0f / 32768.0f"}


# ---- program images ----------------------------------------------------------------------------

@dataclass
class Image:
    """Micro memory contents a compile unit is built for: chunks of pairs at VU addresses."""
    name: str
    unit: int                             # 0 = VU0, 1 = VU1
    chunks: list[tuple[int, list[int]]]   # (start pc, raw pairs)
    entries: list[int] = field(default_factory=list)

    @property
    def pcmask(self) -> int:
        return 0x7FF if self.unit else 0x1FF

    def pairs(self) -> dict[int, int]:
        out = {}
        for start, raws in self.chunks:
            for k, raw in enumerate(raws):
                out[(start + k) & self.pcmask] = raw
        return out


def _elf_sections(b: bytes) -> dict[str, tuple[int, int, int]]:
    shoff = struct.unpack_from("<I", b, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 46)
    secs = [struct.unpack_from("<10I", b, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    out = {}
    for s in secs:
        name = b[stroff + s[0]:b.index(b"\0", stroff + s[0])].decode()
        out[name] = (s[3], s[4], s[5])      # addr, file offset, size
    return out


def elf_overlays(elf_path: str) -> list[tuple[str, list[tuple[int, list[int]]]]]:
    """The dvp-as overlays of an ELF: (id, [(VU start pc, raw pairs)]). The overlay table
    (.DVP.ovlytab: name offset, EE address, VU address per MPG chunk) names sections
    `.DVP.overlay..<vuaddr>.<id>.<line>.<chunk>`; the code itself is in .vutext."""
    b = Path(elf_path).read_bytes()
    secs = _elf_sections(b)
    if ".DVP.ovlytab" not in secs or ".vutext" not in secs:
        return []
    _, toff, tsize = secs[".DVP.ovlytab"]
    _, soff, ssize = secs[".DVP.ovlystrtab"]
    vaddr, voff, _ = secs[".vutext"]
    strtab = b[soff:soff + ssize]
    progs: dict[str, list[tuple[int, list[int]]]] = {}
    for i in range(0, tsize, 12):
        nameoff, ee, vu = struct.unpack_from("<3I", b, toff + i)
        name = strtab[nameoff:strtab.index(b"\0", nameoff)].decode()
        size = secs[name][2]
        ident = name.split("..", 1)[1].split(".")[1]
        off = voff + (ee - vaddr)
        raws = [struct.unpack_from("<Q", b, off + 8 * k)[0] for k in range(size // 8)]
        progs.setdefault(ident, []).append((vu // 8, raws))
    return list(progs.items())


def default_entries(img: Image, ops: dict[int, VuOp]) -> list[int]:
    """Where programs can start: the start of the image and every routine start, i.e. the pair
    after the delay slot of an E bit (also the TPC that VIF MSCNT continues at) or of an
    unconditional B/JR, which nothing falls through to. Sly 2's renderers start programs (VIF
    MSCAL) at such routine starts, e.g. 376110651 at 0x2E20 (after a JR) and 0x2EE8 (after a B)."""
    ents = [min(start for start, _ in img.chunks) & img.pcmask]
    for pc, op in ops.items():
        nxt = (pc + 2) & img.pcmask
        if nxt not in ops:
            continue
        if op.bits & B_E or (not op.bits & B_I and op.lo in ("b", "jr")):
            ents.append(nxt)
    return sorted(set(ents))


# ---- static analysis ------------------------------------------------------------------------------

class Analysis:
    """Version-independent facts about an image: successors in execution order (over-approximated),
    leaders, JR/JALR target candidates, and look-ahead tables used to keep compile-time decisions
    exact: which pairs within the next k may read the MAC/status flags (FR), the clip flags (CR),
    or which VI registers conditional branches may read (BR)."""

    def __init__(self, img: Image, ops: dict[int, VuOp]):
        self.img, self.ops, m = img, ops, img.pcmask
        self.mask = m
        # successors of each pair in execution order; a delay slot continues at its branch's targets
        succ: dict[int, set[int]] = {}
        for pc in ops:
            s = {(pc + 1) & m}
            prev = ops.get((pc - 1) & m)
            if prev is not None and prev.is_branch and not prev.bits & B_I:
                if prev.lo in ("jr", "jalr"):
                    s.add(ANY)
                else:
                    s.add(prev.imm)
                    if prev.lo in COND_BRANCHES:
                        s.add((pc + 1) & m)
            succ[pc] = s
        self.succ = succ

        # subroutines: BAL targets and their bodies (calls inside are assumed to return)
        self.calls: dict[int, list[int]] = {}           # BAL target -> return points
        self.jalr_returns: list[int] = []
        for pc, op in ops.items():
            if op.lo == "bal":
                self.calls.setdefault(op.imm, []).append((pc + 2) & m)
            elif op.lo == "jalr":
                self.jalr_returns.append((pc + 2) & m)
        bodies = {s: self._body(s) for s in self.calls}
        self.jr_cands: dict[int, list[int]] = {}
        all_rets = sorted({r for rs in self.calls.values() for r in rs} | set(self.jalr_returns))
        for pc, op in ops.items():
            if op.lo == "jr":
                c = set()
                for s, body in bodies.items():
                    if pc in body:
                        c.update(self.calls[s])
                if c:
                    c.update(self.jalr_returns)
                else:
                    c.update(all_rets)
                self.jr_cands[pc] = sorted(x for x in c if x in ops)
            elif op.lo == "jalr":
                # callees from data: subroutine entries and routine starts (after a JR/E delay slot)
                c = set(self.calls)
                for q, o in ops.items():
                    if (o.lo == "jr" or o.bits & B_E) and (q + 2) & m in ops:
                        c.add((q + 2) & m)
                self.jr_cands[pc] = sorted(x for x in c if x in ops)

        leaders = set(img.entries)
        for pc, op in ops.items():
            if op.bits & B_I:
                continue
            if op.lo in ("b", "bal") or op.lo in COND_BRANCHES:
                leaders.add(op.imm)
            if op.lo in COND_BRANCHES or op.lo in ("bal", "jalr"):
                leaders.add((pc + 2) & m)
        for c in self.jr_cands.values():
            leaders.update(c)
        self.leaders = {x for x in leaders if x in ops}

        # look-ahead tables over k = 0..4 pairs starting at pc (pc counts as the first)
        def table(is_hit):
            t = {pc: [False] * 5 for pc in ops}
            for k in range(1, 5):
                for pc, op in ops.items():
                    hit = is_hit(op)
                    if not hit:
                        for s in succ[pc]:
                            if s == ANY or s not in ops or t[s][k - 1]:
                                hit = True
                                break
                    t[pc][k] = hit
            return t
        self.FR = table(lambda o: o.lo in STATUS_READERS or o.lo in MAC_READERS)
        self.CR = table(lambda o: o.lo in CLIP_READERS)
        br = {pc: [0] * 5 for pc in ops}
        for k in range(1, 5):
            for pc, op in ops.items():
                v = 0
                if op.lo in COND_BRANCHES and not op.bits & B_I:
                    for r in op.visrc:
                        if r:
                            v |= 1 << r
                for s in succ[pc]:
                    v |= 0xFFFF if s == ANY or s not in ops else br[s][k - 1]
                br[pc][k] = v
        self.BR = br

    def _body(self, start: int) -> set[int]:
        m, ops = self.mask, self.ops
        body: set[int] = set()
        stack = [start]
        while stack:
            pc = stack.pop()
            if pc in body or pc not in ops:
                continue
            body.add(pc)
            op = ops[pc]
            ds = (pc + 1) & m
            if op.bits & B_E:
                body.add(ds)
                continue
            if op.is_branch and not op.bits & B_I:
                body.add(ds)
                if op.lo == "jr":
                    continue
                if op.lo in ("bal", "jalr"):
                    stack.append((pc + 2) & m)
                elif op.lo == "b":
                    stack.append(op.imm)
                else:
                    stack += [op.imm, (pc + 2) & m]
                continue
            stack.append(ds)
        return body

    def br_after(self, pc: int) -> int:
        """VI registers that conditional branches may read within the 4 pairs after pc."""
        v = 0
        for s in self.succ[pc]:
            v |= 0xFFFF if s == ANY or s not in self.ops else self.BR[s][4]
        return v


# ---- timing state ----------------------------------------------------------------------------------

@dataclass(frozen=True)
class Key:
    """A block version: start pc and what is in flight there (ready times relative to the block's
    first issue cycle; all > 0)."""
    pc: int
    vf: tuple = ()        # ((reg, (rx, ry, rz, rw)), ...) pending VF fields (0 = ready)
    vi: tuple = ()        # ((reg, ready), ...) pending ILW results
    q: int = 0            # pending Q result (0 = none)
    p: tuple = ()         # pending P results, oldest first
    fq: tuple = ()        # pending MAC/status entries, oldest first (values in fq0..)
    cq: tuple = ()        # pending clip entries, oldest first (values in cq0..)
    run: tuple = ()       # VI chain run: (reg, ((reads, stalled, writer pc), ...)) newest first
    fd: int = 0           # 1: folded FMAC flags not yet applied to mac/status (in fl/fk)
    ks: int = 0           # sticky status bits known to be set (Z S U O = bits 0-3 of status >> 6)


class Sim:
    """Mutable timing state while generating one block version."""

    def __init__(self, key: Key):
        self.t = 0
        self.vf = {r: list(l) for r, l in key.vf}
        self.vi = dict(key.vi)
        self.q = key.q                  # ready time, 0 = nothing pending
        self.p = [(r, f"p{i}") for i, r in enumerate(key.p)]
        self.fq = [(r, f"fq{i}") for i, r in enumerate(key.fq)]
        self.cq = [(r, f"cq{i}") for i, r in enumerate(key.cq)]
        self.run = key.run              # () or (reg, entries)
        self.fd = key.fd
        self.ks = key.ks


def _free(used: list, prefix: str, n: int) -> str:
    names = {v for _, v in used}
    for i in range(n):
        if f"{prefix}{i}" not in names:
            return f"{prefix}{i}"
    raise AssertionError(f"no free {prefix} slot")


# ---- code generation ----------------------------------------------------------------------------

def VF(r: int) -> str:
    if r == 0:
        return "VF0"
    return f"RG->vf[{r}]" if REGS_IN_MEMORY else f"vf{r}"


ACC = "RG->acc" if REGS_IN_MEMORY else "acc"


def VI(r: int) -> str:
    return "0" if r == 0 else f"vi{r}"


class Unit:
    """Generates one compile unit (an image on one VU) as a C function."""

    def __init__(self, img: Image, fname: str):
        self.img, self.fname = img, fname
        m = img.pcmask
        self.mask = m
        raw = img.pairs()
        self.ops = {pc: decode(r, pc, m) for pc, r in raw.items()}
        if not img.entries:
            img.entries = default_entries(img, self.ops)
        self.an = Analysis(img, self.ops)
        self.versions: dict[Key, int] = {}
        self.keys: list[Key] = []
        self.per_pc: dict[int, int] = {}
        self.work: deque[int] = deque()
        self.code: dict[int, list[str]] = {}
        self.saves: set[int] = set()                  # pcs whose VI old value is kept (vho_<pc>)
        self.vf_used: set[int] = set()
        self.vf_written: set[int] = set()
        self.vi_used: set[int] = set()
        self.vi_written: set[int] = set()
        self.stats = {"handoffs": 0, "capped": 0, "pairs": 0}
        self.handoff_reasons: dict[str, int] = {}
        self.runaway_stubs: set[int] = set()
        for pc, op in self.ops.items():
            if op.vichain and (self.an.br_after(pc) >> op.vichain) & 1:
                self.saves.add(pc)

    # -- versions --
    def version(self, key: Key) -> int | None:
        vid = self.versions.get(key)
        if vid is not None:
            return vid
        n = self.per_pc.get(key.pc, 0)
        if n >= MAX_VERSIONS_PER_PC or len(self.keys) >= MAX_VERSIONS:
            self.stats["capped"] += 1
            return None
        self.per_pc[key.pc] = n + 1
        vid = len(self.keys)
        self.versions[key] = vid
        self.keys.append(key)
        self.work.append(vid)
        return vid

    def generate(self, ks: int = 0) -> None:
        """Explore from every entry with the sticky status bits ks known to be set at the start."""
        self.entry_ids = []
        for e in self.img.entries:
            self.entry_ids.append(self.version(Key(pc=e, ks=ks)))
        while self.work:
            vid = self.work.popleft()
            self.code[vid] = self.block(vid, self.keys[vid])
        self.stub_code: list[str] = []
        for vid in sorted(self.runaway_stubs):
            key = self.keys[vid]
            self.stub_code.append(f"H{vid}:")
            self.handoff(Sim(key), str(key.pc), str((key.pc + 1) & self.mask), 0, 0, "runaway guard", self.stub_code)

    # -- helpers emitting fold code --
    def accumulate(self, sim: Sim, m: str, out: list[str]) -> None:
        """Fold an FMAC result's flags (lane masks, vu_macmask): fl = newest, fk = OR since the last
        materialize() of the components whose sticky bit is not known to be set (sim.ks); mac/status
        are computed from them only where something reads them. Unneeded components (and the flags
        of every op but the last before a materialize) are dead code for the C compiler."""
        need = sum(0xF << (4 * c) for c in range(4) if not (sim.ks >> c) & 1)
        if need:
            mk = "fl" if need == 0xFFFF else f"(fl & 0x{need:X}u)"
            out.append(f"fl = {m}; fk {'|' if sim.fd else ''}= {mk};")
        else:
            out.append(f"fl = {m};")
        sim.fd = 1

    def materialize(self, sim: Sim, out: list[str]) -> None:
        if sim.fd:
            if sim.ks == 0xF:            # every sticky bit already set: only Z S U O and MAC change
                out.append("mac = vu_mac_of_mask(fl); status = (status & ~0xFu) | vu_zsuo_of_mask(fl);")
            else:
                out.append("mac = vu_mac_of_mask(fl); status = vu_status_of_masks(status, fl, fk);")
            sim.fd = 0

    def fold_flags(self, sim: Sim, upto: int | None, out: list[str]) -> None:
        """Fold MAC/status entries that landed by time upto (None: all) into mac/status."""
        while sim.fq and (upto is None or sim.fq[0][0] <= upto):
            _, v = sim.fq.pop(0)
            self.accumulate(sim, v, out)

    def fold_clip(self, sim: Sim, upto: int | None, out: list[str]) -> None:
        last = None
        while sim.cq and (upto is None or sim.cq[0][0] <= upto):
            _, last = sim.cq.pop(0)
        if last:
            out.append(f"clip = {last};")

    def fold_q(self, sim: Sim, upto: int | None, out: list[str]) -> None:
        if sim.q and (upto is None or sim.q <= upto):
            out.append("Q = q_next; status = vu_status_qfold(status, q_flags);")
            sim.q = 0

    def fold_p(self, sim: Sim, upto: int | None, out: list[str]) -> None:
        while sim.p and (upto is None or sim.p[0][0] <= upto):
            _, v = sim.p.pop(0)
            out.append(f"P = {v};")

    def canon(self, sim: Sim, T: int, out: list[str]) -> tuple:
        """Fold what has landed by T, move queued values into canonical slots and return the
        relative (fq, cq, p, q) parts of the successor key."""
        self.fold_flags(sim, T, out)
        if not KEY_FLAGS_DIRTY:
            self.materialize(sim, out)
        self.fold_clip(sim, T, out)
        self.fold_q(sim, T, out)
        self.fold_p(sim, T, out)
        parts = []
        for lst, prefix in ((sim.fq, "fq"), (sim.cq, "cq"), (sim.p, "p")):
            want = [f"{prefix}{i}" for i in range(len(lst))]
            have = [v for _, v in lst]
            if want != have:
                tmp = ", ".join(f"t{i} = {h}" for i, h in enumerate(have))
                mv = " ".join(f"{w} = t{i};" for i, w in enumerate(want))
                out.append(f"{{ uint32_t {tmp}; {mv} }}")
            parts.append(tuple(r - T for r, _ in lst))
            lst[:] = [(r, w) for (r, _), w in zip(lst, want)]
        return parts[0], parts[1], parts[2], (sim.q - T if sim.q else 0)

    def succ_key(self, sim: Sim, T: int, pc: int, fq, cq, p, q) -> Key:
        vf = tuple(sorted((r, tuple(max(0, x - T) for x in l)) for r, l in sim.vf.items() if max(l) > T))
        vi = tuple(sorted((r, x - T) for r, x in sim.vi.items() if x > T))
        run = sim.run
        if run and pc in self.ops and not (self.an.BR[pc][4] >> run[0]) & 1:
            run = ()
        return Key(pc=pc, vf=vf, vi=vi, q=q, p=p, fq=fq, cq=cq, run=run, fd=sim.fd, ks=sim.ks)

    # -- handoff to the interpreter --
    def handoff(self, sim: Sim, pc_expr: str, npc_expr: str, ending: int, k: int, why: str,
                out: list[str]) -> None:
        """Write the pipeline state as vu_exec.c keeps it and continue in the interpreter."""
        self.stats["handoffs"] += 1
        self.handoff_reasons[why] = self.handoff_reasons.get(why, 0) + 1
        t = sim.t
        out.append(f"/* handoff: {why} */")
        fd = sim.fd
        self.materialize(sim, out)
        sim.fd = fd                      # (only this path materializes; the caller's state is unchanged)
        out.append("vuc_pipe_clear(st);")
        for r, l in sim.vf.items():
            lanes = [x for x in l]
            if max(lanes) > t:
                for i, x in enumerate(lanes):
                    if x > t:
                        out.append(f"st->vf_ready[{r}][{i}] = cyc + {x};")
                out.append(f"st->vf_ready_any[{r}] = cyc + {max(lanes)};")
        for r, x in sim.vi.items():
            if x > t:
                out.append(f"st->vi_ready[{r}] = cyc + {x};")
        for i, (r, v) in enumerate(sim.fq):
            out.append(f"st->fq[{i}].ready = cyc + {r}; st->fq[{i}].mac = (uint16_t)vu_mac_of_mask({v}); "
                       f"st->fq[{i}].zsuo = (uint16_t)vu_zsuo_of_mask({v});")
        if sim.fq:
            out.append(f"st->fq_tail = {len(sim.fq)};")
        for i, (r, v) in enumerate(sim.cq):
            out.append(f"st->cq[{i}].ready = cyc + {r}; st->cq[{i}].clip = {v};")
        if sim.cq:
            out.append(f"st->cq_tail = {len(sim.cq)};")
        if sim.q:
            out.append(f"st->q_pend = 1; st->q_next = q_next; st->q_flags = q_flags; st->q_ready = cyc + {sim.q};")
        for i, (r, v) in enumerate(sim.p):
            out.append(f"st->p_next[{i}] = {v}; st->p_ready[{i}] = cyc + {r};")
        if sim.p:
            out.append(f"st->p_pend = {len(sim.p)};")
        if sim.run:
            reg, ents = sim.run
            for age, (rd, stl, wpc) in enumerate(ents[:4], 1):
                old = f"vho_{wpc:X}" if wpc in self.saves else "0"
                out.append(f"st->vih[(n + {k} - {age}) & 3] = {reg}u | {stl << 8 | rd << 9}u | (uint32_t){old} << 16;")
        out.append(f"ho_pc = {pc_expr}; ho_npc = {npc_expr}; ho_cyc = cyc + {t}; ho_n = n + {k}; ho_end = {ending};")
        out.append("goto handoff;")

    # -- one block version --
    def block(self, vid: int, key: Key) -> list[str]:
        ops, m, an = self.ops, self.mask, self.an
        sim = Sim(key)
        out: list[str] = [f"V{vid}: /* 0x{key.pc * 8:04X}{self.describe(key)} */"]
        pc, npc = key.pc, (key.pc + 1) & m
        dyn = None                 # after a branch: ("cond", T, F) or ("jr", cands, site pc)
        ending = 0
        k = 0
        while True:
            # pc is static here; npc may be the dynamic marker "D" (pc is the delay slot of a
            # conditional branch or JR/JALR): a handoff then continues at the branch's target
            npc_expr = self.dyn_pc_expr(dyn) if npc == "D" else str(npc)
            if pc not in ops:
                self.handoff(sim, str(pc), npc_expr, ending, k, "outside the image", out)
                return out
            op = ops[pc]
            why = None
            if op.up == "invalid" or op.lo == "invalid":
                why = "invalid instruction"
            elif op.lo == "xgkick" and self.img.unit == 0:
                why = "XGKICK on VU0"
            elif op.is_branch and not ending and npc != "D":
                ds = ops.get((pc + 1) & m)
                if ds is None or (ds.is_branch and not ds.bits & B_I) or (ds.bits & B_E and not op.bits & B_E):
                    why = "branch delay slot"
            if why:
                self.handoff(sim, str(pc), npc_expr, ending, k, why, out)
                return out
            new_pc = npc
            new_npc = "D" if npc == "D" else (npc + 1) & m
            body, br = self.pair(op, sim, k, new_npc)      # link value: the address after the delay slot
            out.append(f"{{ /* 0x{pc * 8:04X} {op.upper_word:08X} {op.lower_word:08X} t={sim.t - 1} */ "
                       f"VUC_TRACE({pc}, cyc + {sim.t - 1});")
            out.extend("    " + x for x in body)
            out.append("}")
            k += 1
            self.stats["pairs"] += 1
            if br is not None:
                if br[0] == "static":
                    new_npc = br[1]
                else:
                    assert npc != "D"
                    dyn = br
                    new_npc = "D"
            end_now = ending
            if op.bits & B_I:
                out.append(f"I = vuc_opaque(0x{op.imm & 0xFFFFFFFF:08X}u);")
            if not end_now and op.bits & B_E:
                ending = 1
            if end_now:
                if new_pc == "D":
                    tpc = self.dyn_pc_expr(dyn)
                else:
                    tpc = str(new_pc)
                self.end_program(sim, tpc, k, out)
                return out
            pc, npc = new_pc, new_npc
            if pc == "D":
                self.transition_dyn(sim, dyn, k, out)
                return out
            if pc in an.leaders and npc == (pc + 1) & m and not ending:
                self.transition(sim, pc, k, out, back=pc <= key.pc)
                return out

    def describe(self, key: Key) -> str:
        parts = []
        if key.vf:
            parts.append("vf " + " ".join(f"{r}:{''.join(map(str, l))}" for r, l in key.vf))
        if key.vi:
            parts.append("vi " + " ".join(f"{r}:{x}" for r, x in key.vi))
        if key.q:
            parts.append(f"q {key.q}")
        if key.p:
            parts.append("p " + ",".join(map(str, key.p)))
        if key.fq:
            parts.append("fq " + ",".join(map(str, key.fq)))
        if key.cq:
            parts.append("cq " + ",".join(map(str, key.cq)))
        if key.run:
            parts.append(f"run vi{key.run[0]}x{len(key.run[1])}")
        return (" | " + "; ".join(parts)) if parts else ""

    def dyn_pc_expr(self, dyn) -> str:
        if dyn[0] == "cond":
            return f"(tk ? {dyn[1]}u : {dyn[2]}u)"
        return "jt"

    def guarded_goto(self, vid: int) -> str:
        """Backward jump to a block version: close to the interpreter's runaway limit, hand over to
        the interpreter at the version's start instead (stub H<vid>, emitted once per version), so
        that a program without an E bit stops exactly where the interpreter stops it."""
        self.runaway_stubs.add(vid)
        return f"if (n >= VUC_RUNAWAY_HANDOFF) goto H{vid}; goto V{vid};"

    def transition(self, sim: Sim, pc: int, k: int, out: list[str], back: bool) -> None:
        T = sim.t
        fq, cq, p, q = self.canon(sim, T, out)
        key = self.succ_key(sim, T, pc, fq, cq, p, q)
        vid = self.version(key)
        if vid is None:
            self.handoff(sim, str(pc), str((pc + 1) & self.mask), 0, k, "version cap", out)
            return
        out.append(f"cyc += {T}; n += {k};")
        out.append(self.guarded_goto(vid) if back else f"goto V{vid};")

    def transition_dyn(self, sim: Sim, dyn, k: int, out: list[str]) -> None:
        T = sim.t
        fq, cq, p, q = self.canon(sim, T, out)
        m = self.mask
        if dyn[0] == "cond":
            _, tgt, fall, _site = dyn
            ids = []
            for pc in (tgt, fall):
                key = self.succ_key(sim, T, pc, fq, cq, p, q)
                ids.append(self.version(key) if pc in self.ops else None)
            if None in ids:
                # rare: one side has no version; hand off for that side
                for pc, vid, cond in ((tgt, ids[0], "tk"), (fall, ids[1], "!tk")):
                    if vid is None:
                        sub: list[str] = [f"if ({cond}) {{"]
                        self.handoff(sim, str(pc), str((pc + 1) & m), 0, k, "version cap", sub)
                        out.extend(sub)
                        out.append("}")
            out.append(f"cyc += {T}; n += {k};")
            go = [None, None]
            for i, (pc, vid) in enumerate(((tgt, ids[0]), (fall, ids[1]))):
                if vid is not None:
                    go[i] = self.guarded_goto(vid) if pc <= dyn[3] else f"goto V{vid};"
            if go[0] and go[1]:
                out.append(f"if (tk) {{ {go[0]} }} {go[1]}")
            elif go[0] or go[1]:
                out.append(go[0] or go[1])
            return
        _, cands, site = dyn
        cases = []
        for pc in cands:
            key = self.succ_key(sim, T, pc, fq, cq, p, q)
            vid = self.version(key)
            if vid is not None:
                cases.append((pc, vid))
        out.append(f"cyc += {T}; n += {k};")
        out.append("switch (jt) {")
        for pc, vid in cases:
            out.append(f"case {pc}: {self.guarded_goto(vid) if pc <= site else f'goto V{vid};'}")
        out.append("default: {")
        out.append(f"    vuc_unknown_jump(st, {site}u, jt);")
        sub: list[str] = []
        sim.t = 0
        for lst in (sim.fq, sim.cq, sim.p):
            lst[:] = [(r - T, v) for r, v in lst]
        sim.q = sim.q - T if sim.q else 0
        sim.vf = {r: [x - T for x in l] for r, l in sim.vf.items()}
        sim.vi = {r: x - T for r, x in sim.vi.items()}
        self.handoff(sim, "jt", f"(jt + 1) & {m}u", 0, 0, "unknown jump target", sub)
        out.extend("    " + x for x in sub)
        out.append("}")
        out.append("}")

    def end_program(self, sim: Sim, tpc: str, k: int, out: list[str]) -> None:
        self.fold_flags(sim, None, out)
        self.materialize(sim, out)
        self.fold_clip(sim, None, out)
        self.fold_q(sim, None, out)
        self.fold_p(sim, None, out)
        out.append(f"cyc += {sim.t}; n += {k}; tpc = {tpc}; goto done;")

    # -- one pair --
    def pair(self, op: VuOp, sim: Sim, k: int, npc) -> tuple[list[str], tuple | None]:
        """Code for one instruction pair issued at the earliest cycle allowed by the pipeline; updates
        the timing state. Returns (lines, branch) where branch is None, ("static", target) or the
        dynamic successor description."""
        an, m = self.an, self.mask
        t = sim.t
        stalled = 0
        if op.check:
            for reg, mask in op.srcs:
                l = sim.vf.get(reg)
                if l:
                    for i in range(4):
                        if mask >> i & 1 and l[i] > t:
                            t = l[i]
            for r in op.visrc:
                x = sim.vi.get(r, 0) if r else 0
                if x > t:
                    t = x
            if op.stall == STALL_Q:
                if sim.q and sim.q > t:
                    t = sim.q
            elif op.stall in (STALL_EFU, STALL_P):
                if sim.p and sim.p[-1][0] - 1 > t:
                    t = sim.p[-1][0] - 1
            stalled = 1 if t != sim.t else 0
        out: list[str] = []
        pc = op.pc

        # ---- upper: compute (reads registers as they were before the pair)
        utarget = None
        ureg = 0                         # VF register the upper op writes (0: none or ACC)
        up = op.up
        if up in FMAC_KIND or up in ("opmula", "opmsub"):
            self.vf_used.update((op.ufs, op.uft))
            a = f"vu_vclean({VF(op.ufs)})"
            if up in FMAC_KIND:
                if op.ubm == BM_VEC:
                    b = f"vu_vclean({VF(op.uft)})"
                elif op.ubm == BM_BC:
                    b = f"vu_vclean(vu_bcast({VF(op.uft)}, {op.ubc}))"
                elif op.ubm == BM_Q:
                    self.fold_q(sim, t, out)
                    b = "vu_vclean(vu_bits(Q))"
                else:
                    b = "vu_vclean(vu_bits(I))"
                out.append(f"unsigned z, u; __m128 ur = vu_fmac({FMAC_KIND[up]}, {a}, {b}, {ACC}, {op.udst}, &z, &u);")
                utarget = ACC if op.uacc else (VF(op.ufd) if op.ufd else None)
                ureg = 0 if op.uacc else op.ufd
            else:
                sub = 1 if up == "opmsub" else 0
                out.append(f"unsigned z, u; __m128 ur = vu_opm({sub}, {a}, vu_vclean({VF(op.uft)}), {ACC}, {op.udst}, &z, &u);")
                utarget = (VF(op.ufd) if op.ufd else None) if sub else ACC
                ureg = op.ufd if sub else 0
            out.append(f"uint32_t uf = vu_macmask(ur, {op.udst}, z, u);")
            if an.FR[pc][4]:
                self.fold_flags(sim, t, out)
                v = _free(sim.fq, "fq", 4)
                out.append(f"{v} = uf;")
                sim.fq.append((t + 4, v))
            else:
                self.fold_flags(sim, None, out)
                self.accumulate(sim, "uf", out)
        elif up in ("max", "mini"):
            self.vf_used.update((op.ufs, op.uft))
            if op.ubm == BM_VEC:
                b = VF(op.uft)
            elif op.ubm == BM_BC:
                b = f"vu_bcast({VF(op.uft)}, {op.ubc})"
            else:
                b = "vu_bits(I)"
            out.append(f"__m128 ur = vu_maxmin({1 if up == 'max' else 0}, {VF(op.ufs)}, {b});")
            utarget = VF(op.ufd) if op.ufd else None
            ureg = op.ufd
        elif up == "abs" or up.startswith("ftoi") or up.startswith("itof"):
            self.vf_used.add(op.ufs)
            if up == "abs":
                out.append(f"__m128 ur = vu_abs({VF(op.ufs)});")
            elif up.startswith("ftoi"):
                out.append(f"__m128 ur = vu_ftoi({VF(op.ufs)}, {SCALE[up[4:]]});")
            else:
                out.append(f"__m128 ur = vu_itof({VF(op.ufs)}, {ISCALE[up[4:]]});")
            utarget = VF(op.uft) if op.uft else None
            ureg = op.uft
        elif up == "clip":
            self.vf_used.update((op.ufs, op.uft))
            s, w = VF(op.ufs), VF(op.uft)
            out.append(f"clip_pipe = ((clip_pipe << 6) | vu_clip_judge(vu_lane({s}, 0), vu_lane({s}, 1), "
                       f"vu_lane({s}, 2), vu_lane({w}, 3))) & 0xFFFFFF;")
            if an.CR[pc][4]:
                self.fold_clip(sim, t, out)
                v = _free(sim.cq, "cq", 4)
                out.append(f"{v} = clip_pipe;")
                sim.cq.append((t + 4, v))
            else:
                sim.cq.clear()
                out.append("clip = clip_pipe;")
        elif up == "nop":
            pass
        else:
            raise AssertionError(up)

        # ---- VI history for the branch quirk: old value before the lower op writes
        if op.vichain and pc in self.saves:
            out.append(f"vho_{pc:X} = {VI(op.vichain)};")

        # ---- lower
        br = self.lower(op, sim, t, stalled, npc, out)

        # ---- commit the upper result (wins over a lower write to the same field)
        if utarget is not None:
            if ureg:
                self.vf_written.add(ureg)
            out.append(f"{utarget} = vu_blend({utarget}, ur, {op.udst});")

        # ---- timing state: results in flight
        for w in (op.uw, op.lw):
            if w:
                reg, mask = w
                l = sim.vf.setdefault(reg, [0, 0, 0, 0])
                for i in range(4):
                    if mask >> i & 1:
                        l[i] = t + 4
        if op.viw:
            # every write sets the register's ready time (a 1-cycle write after an ILW replaces the
            # ILW's later one, as in vu_exec.c mark_writes)
            sim.vi[op.viw] = t + op.viwlat
        # VI chain run (most recent consecutive chain writes of one register)
        if op.vichain:
            ent = (op.vichain_rd, stalled, pc)
            if sim.run and sim.run[0] == op.vichain and op.vichain_rd and not stalled:
                ents = (ent,) + sim.run[1][:3]
            else:
                ents = (ent,)
            sim.run = (op.vichain, ents)
        else:
            sim.run = ()
        # trim the run after its first entry the walk stops at
        if sim.run:
            ents = sim.run[1]
            for i, (rd, stl, _) in enumerate(ents):
                if stl or not rd:
                    sim.run = (sim.run[0], ents[:i + 1])
                    break
        sim.t = t + 1
        return out, br

    def bvi(self, r: int, sim: Sim, stalled: int) -> str:
        """VI register value as a conditional branch reads it (interpreter: branch_vi)."""
        if r == 0 or stalled or not sim.run or sim.run[0] != r:
            return VI(r)
        v = VI(r)
        for rd, stl, wpc in sim.run[1][:4]:
            assert wpc in self.saves, f"VI chain value of 0x{wpc * 8:04X} not kept"
            v = f"vho_{wpc:X}"
            if stl or not rd:
                break
        return v

    def lower(self, op: VuOp, sim: Sim, t: int, stalled: int, npc, out: list[str]):
        lo, m, unit = op.lo, self.mask, self.img.unit
        lt, ls = op.lt, op.ls
        L = out.append

        def setvi(r: int, expr: str) -> None:
            if r:
                self.vi_written.add(r)
                L(f"vi{r} = (uint16_t)({expr});")

        def use_vi(*rs):
            self.vi_used.update(r for r in rs if r)

        def vfw(r: int, expr: str, mask: int) -> None:
            if r:
                self.vf_written.add(r)
                self.vf_used.add(r)
                L(f"{VF(r)} = vu_blend({VF(r)}, {expr}, {mask});")

        def ldq(a: str) -> str:
            return f"vuc_ldq{unit}(st, dm, {a})"

        def status_now() -> None:
            self.fold_flags(sim, t, out)
            self.materialize(sim, out)
            self.fold_q(sim, t, out)

        if lo in ("nop", "loi", "waitq", "waitp"):
            return None
        if lo == "lq":
            use_vi(ls)
            L(f"__m128 lv = {ldq(f'(uint32_t)({VI(ls)} + ({op.imm}))')};")
            vfw(lt, "lv", op.ldst)
        elif lo == "sq":
            use_vi(lt); self.vf_used.add(ls)
            L(f"vuc_stq{unit}(st, dm, (uint32_t)({VI(lt)} + ({op.imm})), {VF(ls)}, {op.ldst});")
        elif lo in ("ilw", "ilwr"):
            use_vi(ls)
            setvi(lt, f"vuc_ldw{unit}(st, dm, (uint32_t)({VI(ls)} + ({op.imm})), {op.fsf})")
        elif lo in ("isw", "iswr"):
            use_vi(ls, lt)
            L(f"vuc_stw{unit}(st, dm, (uint32_t)({VI(ls)} + ({op.imm})), {op.ldst}, {VI(lt)});")
        elif lo == "iaddiu":
            use_vi(ls); setvi(lt, f"{VI(ls)} + {op.imm}")
        elif lo == "isubiu":
            use_vi(ls); setvi(lt, f"{VI(ls)} - {op.imm}")
        elif lo == "iaddi":
            use_vi(ls); setvi(lt, f"{VI(ls)} + ({op.imm})")
        elif lo in ("iadd", "isub", "iand", "ior"):
            use_vi(ls, lt)
            o = {"iadd": "+", "isub": "-", "iand": "&", "ior": "|"}[lo]
            setvi(op.ld, f"{VI(ls)} {o} {VI(lt)}")
        elif lo in ("fceq", "fcand", "fcor", "fcget"):
            self.fold_clip(sim, t, out)
            if lo == "fceq":
                setvi(1, f"(clip & 0xFFFFFF) == 0x{op.imm:X}u")
            elif lo == "fcand":
                setvi(1, f"(clip & 0x{op.imm:X}u) != 0")
            elif lo == "fcor":
                setvi(1, f"((clip | 0x{op.imm:X}u) & 0xFFFFFF) == 0xFFFFFF")
            else:
                setvi(lt, "clip & 0xFFF")
        elif lo == "fcset":
            sim.cq.clear()
            L(f"clip = clip_pipe = 0x{op.imm:X}u;")
        elif lo in ("fseq", "fsand", "fsor", "fsset"):
            status_now()
            if lo == "fseq":
                setvi(lt, f"(status & 0xFFF) == 0x{op.imm:X}u")
            elif lo == "fsand":
                setvi(lt, f"status & 0x{op.imm:X}u")
            elif lo == "fsor":
                setvi(lt, f"((status | 0x{op.imm:X}u) & 0xFFF) == 0xFFF")
            else:
                L(f"status = (status & 0x3F) | 0x{op.imm & 0xFC0:X}u;")
                sim.ks = (op.imm >> 6) & 0xF
        elif lo in ("fmeq", "fmand", "fmor"):
            self.fold_flags(sim, t, out)
            self.materialize(sim, out)
            use_vi(ls)
            if lo == "fmeq":
                setvi(lt, f"(mac & 0xFFFF) == {VI(ls)}")
            elif lo == "fmand":
                setvi(lt, f"mac & {VI(ls)}")
            else:
                setvi(lt, f"((mac | {VI(ls)}) & 0xFFFF) == 0xFFFF")
        elif lo == "b":
            return ("static", op.imm)
        elif lo == "bal":
            setvi(lt, f"{npc}u")
            return ("static", op.imm)
        elif lo in ("jr", "jalr"):
            use_vi(ls)
            L(f"jt = {VI(ls)} & {m}u;")
            if lo == "jalr":
                setvi(lt, f"{npc}u")
            return ("jr", self.an.jr_cands.get(op.pc, []), op.pc)
        elif lo in COND_BRANCHES:
            use_vi(lt, ls)
            if lo in ("ibeq", "ibne"):
                c = "==" if lo == "ibeq" else "!="
                L(f"tk = {self.bvi(lt, sim, stalled)} {c} {self.bvi(ls, sim, stalled)};")
            else:
                c = {"ibltz": "<", "ibgtz": ">", "iblez": "<=", "ibgez": ">="}[lo]
                L(f"tk = (int16_t){self.bvi(ls, sim, stalled)} {c} 0;")
            return ("cond", op.imm, (op.pc + 2) & m, op.pc)
        elif lo in ("move", "mr32"):
            self.vf_used.add(ls)
            src = VF(ls) if lo == "move" else f"_mm_shuffle_ps({VF(ls)}, {VF(ls)}, _MM_SHUFFLE(0, 3, 2, 1))"
            vfw(lt, src, op.ldst)
        elif lo in ("lqi", "lqd"):
            use_vi(ls)
            if lo == "lqi":
                L(f"uint32_t a = {VI(ls)};")
                L(f"__m128 lv = {ldq('a')};")
                setvi(ls, "a + 1")
            else:
                L(f"uint32_t a = ({VI(ls)} - 1u) & 0xFFFF;")
                L(f"__m128 lv = {ldq('a')};")
                setvi(ls, "a")
            vfw(lt, "lv", op.ldst)
        elif lo in ("sqi", "sqd"):
            use_vi(lt); self.vf_used.add(ls)
            if lo == "sqi":
                L(f"uint32_t a = {VI(lt)};")
                L(f"vuc_stq{unit}(st, dm, a, {VF(ls)}, {op.ldst});")
                setvi(lt, "a + 1")
            else:
                L(f"uint32_t a = ({VI(lt)} - 1u) & 0xFFFF;")
                L(f"vuc_stq{unit}(st, dm, a, {VF(ls)}, {op.ldst});")
                setvi(lt, "a")
        elif lo in ("div", "sqrt", "rsqrt"):
            self.fold_q(sim, t, out)                     # FDIV stalled until the previous result landed
            assert not sim.q
            self.vf_used.update((ls, op.lt))
            s, tt = f"vu_lane({VF(ls)}, {op.fsf})", f"vu_lane({VF(op.lt)}, {op.ftf})"
            if lo == "div":
                L(f"q_next = vu_div({s}, {tt}, &q_flags);")
            elif lo == "sqrt":
                L(f"q_next = vu_sqrt({tt}, &q_flags);")
            else:
                L(f"q_next = vu_rsqrt({s}, {tt}, &q_flags);")
            sim.q = t + (13 if lo == "rsqrt" else 7)
        elif lo == "mtir":
            self.vf_used.add(ls)
            setvi(lt, f"vu_lane({VF(ls)}, {op.fsf})")
        elif lo == "mfir":
            use_vi(ls)
            vfw(lt, f"vu_bits(vuc_opaque((uint32_t)(int32_t)(int16_t){VI(ls)}))", op.ldst)
        elif lo == "rnext":
            L("R = vu_rnext(R);")
            vfw(lt, "vu_bits(R)", op.ldst)
        elif lo == "rget":
            vfw(lt, "vu_bits(R)", op.ldst)
        elif lo == "rinit":
            self.vf_used.add(ls)
            L(f"R = 0x3F800000u | (vu_lane({VF(ls)}, {op.fsf}) & 0x7FFFFF);")
        elif lo == "rxor":
            self.vf_used.add(ls)
            L(f"R = 0x3F800000u | ((R ^ vu_lane({VF(ls)}, {op.fsf})) & 0x7FFFFF);")
        elif lo == "mfp":
            self.fold_p(sim, t, out)
            vfw(lt, "vu_bits(P)", op.ldst)
        elif lo in ("xtop", "xitop"):
            setvi(lt, "st->top" if lo == "xtop" else "st->itop")
        elif lo == "xgkick":
            use_vi(ls)
            L(f"st->cyc = cyc + {t}; vu_xgkick({VI(ls)});")
        elif lo in EFU_KIND:
            self.vf_used.add(ls)
            s = VF(ls)
            L(f"uint32_t pv = vu_efu({EFU_KIND[lo]}, vu_lane({s}, 0), vu_lane({s}, 1), vu_lane({s}, 2), "
              f"vu_lane({s}, 3), vu_lane({s}, {op.fsf}));")
            self.fold_p(sim, t, out)
            assert len(sim.p) <= 1
            v = _free(sim.p, "p", 2)
            L(f"{v} = pv;")
            sim.p.append((t + op.imm, v))
        else:
            raise AssertionError(lo)
        return None

    # -- the C function --
    def emit(self) -> str:
        m = self.mask
        vfs = sorted(r for r in self.vf_used | self.vf_written if r)
        vis = sorted(r for r in self.vi_used | self.vi_written if r)
        L = []
        L.append(f"/* {self.img.name} on VU{self.img.unit}: {len(self.keys)} block versions, "
                 f"{self.stats['pairs']} pairs generated, {self.stats['handoffs']} handoff sites */")
        L.append(f"uint32_t {self.fname}(vu_state* st, uint32_t entry) {{")
        L.append("    uint8_t* const dm = st->data;")
        L.append("    const __m128 vf0 = vuc_opaque_v(_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));")
        if REGS_IN_MEMORY:
            # the asm makes the struct's address escape: clang keeps it in memory (no SSA values)
            L.append("    vuc_regs regs, * const __restrict RG = &regs;")
            L.append('    __asm__ volatile("" : : "r"(RG) : "memory");')
            for r in vfs:
                L.append(f"    RG->vf[{r}] = _mm_loadu_ps(st->vf[{r}].f);")
            L.append("    RG->acc = _mm_loadu_ps(st->acc.f);")
        else:
            for r in vfs:
                L.append(f"    __m128 vf{r} = _mm_loadu_ps(st->vf[{r}].f);")
            L.append("    __m128 acc = _mm_loadu_ps(st->acc.f);")
        for r in vis:
            L.append(f"    uint16_t vi{r} = st->vi[{r}];")
        L.append("    uint32_t I = st->I, Q = st->Q, P = st->P, R = st->R;")
        L.append("    uint32_t status = st->status, mac = st->mac, clip = st->clip, clip_pipe = st->clip;")
        L.append("    uint32_t q_next = 0, q_flags = 0, p0 = 0, p1 = 0, fq0 = 0, fq1 = 0, fq2 = 0, fq3 = 0;")
        L.append("    uint32_t cq0 = 0, cq1 = 0, cq2 = 0, cq3 = 0, fl = 0, fk = 0;")
        L.append("    uint32_t cyc = 0, n = 0, tpc = 0, jt = 0; int tk = 0;")
        L.append("    uint32_t ho_pc = 0, ho_npc = 0, ho_cyc = 0, ho_n = 0; int ho_end = 0;")
        for pc in sorted(self.saves):
            L.append(f"    uint16_t vho_{pc:X} = 0;")
        L.append("    (void)Q; (void)P; (void)R; (void)I; (void)dm; (void)mac; (void)clip_pipe; (void)tk; (void)jt;")
        L.append("    (void)q_next; (void)q_flags; (void)p0; (void)p1; (void)fq0; (void)fq1; (void)fq2; (void)fq3;")
        L.append("    (void)cq0; (void)cq1; (void)cq2; (void)cq3; (void)ho_npc; (void)ho_cyc; (void)ho_n; (void)ho_end;")
        L.append("    (void)fl; (void)fk;")
        L.append("    switch (entry) {")
        for i, vid in enumerate(self.entry_ids):
            if vid is not None:
                L.append(f"    case {i}: goto V{vid};")
        L.append("    default: return VUC_NOT_COMPILED;")
        L.append("    }")
        for vid in range(len(self.keys)):
            L.extend("    " + x for x in self.code[vid])
        L.extend("    " + x for x in self.stub_code)
        writeback = []
        for r in vfs:
            if r in self.vf_written:
                writeback.append(f"_mm_storeu_ps(st->vf[{r}].f, {VF(r)});")
        writeback.append(f"_mm_storeu_ps(st->acc.f, {ACC});")
        for r in vis:
            if r in self.vi_written:
                writeback.append(f"st->vi[{r}] = vi{r};")
        writeback.append("st->I = I; st->Q = Q; st->P = P; st->R = R;")
        writeback.append("st->status = status; st->mac = mac; st->clip = clip; st->clip_pipe = clip_pipe;")
        L.append("done:")
        L.extend("    " + x for x in writeback)
        L.append("    st->tpc = tpc;")
        L.append("    st->programs++; st->pairs += n; st->cycles += cyc;")
        L.append("    return n;")
        L.append("handoff:")
        L.extend("    " + x for x in writeback)
        L.append("    return vu_resume(st, ho_pc, ho_npc, ho_cyc, ho_n, ho_end);")
        L.append("}")
        return "\n".join(L)

    def descriptor(self, cname: str) -> str:
        """vuc_program descriptor: the image it was compiled for (checked against micro memory
        before use) and the entry points."""
        ranges, words = [], []
        for start, raws in self.img.chunks:
            ranges.append(f"{{ {start}, {len(raws)} }}")
            for r in raws:
                words += [f"0x{r & 0xFFFFFFFF:08X}u", f"0x{r >> 32:08X}u"]
        ents = ", ".join(str(e) for e in self.img.entries)
        wl = []
        for i in range(0, len(words), 8):
            wl.append("    " + ", ".join(words[i:i + 8]) + ",")
        return "\n".join([
            f"static const uint16_t {cname}_entries[] = {{ {ents} }};",
            f"static const vuc_range {cname}_ranges[] = {{ {', '.join(ranges)} }};",
            f"static const uint32_t {cname}_code[] = {{",
            *wl,
            "};",
            f"const vuc_program {cname} = {{ \"{self.img.name}\", {self.img.unit}, "
            f"{len(self.img.entries)}, {cname}_entries, {len(ranges)}, {cname}_ranges, {cname}_code, "
            f"{self.fname} }};",
        ])


HEADER = [
    "/* The code runs under the VU MXCSR (round toward zero, flush to zero): the compiler must not",
    " * fold float operations on constants (vf00, LOI immediates) with its own rounding. FP exception",
    " * flags are of no interest, so unused float operations (flags nobody reads) can still go. */",
    "#pragma STDC FENV_ACCESS ON",
    "#pragma clang fp exceptions(ignore)",
    '#include "vu_compiled.h"',
    "",
]


def compile_image(img: Image, cname: str) -> tuple[list[tuple[str, str]], list[Unit]]:
    """C files for one image: a function per sticky-bit specialisation (ENTRY_STICKY: what the
    status register's sticky bits hold when a program starts; separate files so that they compile in
    parallel and stay small) and the descriptor with a dispatcher picking one at run time."""
    files, units, calls = [], [], []
    for ks in ENTRY_STICKY:
        u = Unit(img, f"{cname}_ks{ks}")
        u.generate(ks)
        units.append(u)
        files.append((f"{cname}_ks{ks}.c", "\n".join([
            f"/* Recompiled VU microcode ({img.name}, VU{img.unit}), sticky status bits {ks:X} known set at the "
            "start; generated by recomp/vu_recomp.py. */", *HEADER, u.emit(), ""])))
        calls.append((ks, u.fname))
    decls = "\n".join(f"uint32_t {fn}(vu_state* st, uint32_t entry);" for _, fn in calls)
    body = []
    for ks, fn in calls:
        body.append(f"    return {fn}(st, entry);" if ks == 0 else f"    if ((sticky & {ks}u) == {ks}u) return {fn}(st, entry);")
    disp = "\n".join([
        decls,
        "",
        "/* Sticky status bits are only ever set (FSSET, the one way to clear them, has a static immediate):",
        " * the code for the bits set at the start skips accumulating them. */",
        f"static uint32_t {cname}_run(vu_state* st, uint32_t entry) {{",
        "    const uint32_t sticky = (st->status >> 6) & 0xF;   /* Z S U O */",
        *body,
        "}",
    ])
    units[0].fname = f"{cname}_run"                  # the descriptor's function: the dispatcher
    files.append((f"{cname}.c", "\n".join([
        f"/* Recompiled VU microcode ({img.name}, VU{img.unit}): image, entries, dispatcher; generated by "
        "recomp/vu_recomp.py. */", '#include "vu_compiled.h"', "", disp, "", units[0].descriptor(cname), ""])))
    return files, units


def write_units(images: list[Image], out: Path, prefix: str = "vuc") -> list[str]:
    """Writes the C files of every image plus <prefix>_table.c (registration with the runtime).
    Returns report lines."""
    from .__main__ import write_if_changed
    out.mkdir(parents=True, exist_ok=True)
    names, report, written = [], [], set()
    for img in images:
        cname = f"{prefix}_{img.name}_vu{img.unit}"
        files, units = compile_image(img, cname)
        for fn, src in files:
            write_if_changed(out / fn, src)
            written.add(fn)
        names.append(cname)
        for ks, u in zip(ENTRY_STICKY, units):
            hand = ", ".join(f"{k} {v}" for k, v in sorted(u.handoff_reasons.items()))
            report.append(f"{img.name} VU{img.unit} sticky {ks:X}: {len(u.ops)} pairs, {len(img.entries)} entries, "
                          f"{len(u.keys)} block versions, {u.stats['pairs']} pairs generated"
                          f"{', handoffs: ' + hand if hand else ''}{', capped ' + str(u.stats['capped']) if u.stats['capped'] else ''}")
    decls = "\n".join(f"extern const vuc_program {n};" for n in names)
    lst = ",\n".join(f"    &{n}" for n in names)
    table = "\n".join([
        "/* Recompiled VU microprograms, registered with runtime/vu.c at startup. Generated. */",
        '#include "vu_compiled.h"',
        "",
        decls,
        "",
        f"static const vuc_program* const {prefix}_all[] = {{",
        lst,
        "};",
        "",
        f"__attribute__((constructor)) static void {prefix}_register(void) {{",
        f"    vu_register_compiled({prefix}_all, {len(names)});",
        "}",
        "",
    ])
    write_if_changed(out / f"{prefix}_table.c", table)
    written.add(f"{prefix}_table.c")
    for stale in out.glob("*.c"):
        if stale.name not in written:
            stale.unlink()
    return report


def game_images(elf_path: str) -> list[Image]:
    """Every overlay on VU1; overlays that fit VU0's 4 KB micro memory and use no VU1-only
    instruction also on VU0."""
    imgs = []
    for ident, chunks in elf_overlays(elf_path):
        imgs.append(Image(ident, 1, chunks))
        end = max(s + len(r) for s, r in chunks)
        if end <= 512:
            ops = {pc: decode(r, pc, 0x1FF) for pc, r in Image(ident, 0, chunks).pairs().items()}
            if not any(o.lo in ("xgkick", "xtop", "xitop") or o.lo in EFU_KIND for o in ops.values()):
                imgs.append(Image(ident, 0, chunks))
    return imgs
