"""Split the executable code into functions."""
from dataclasses import dataclass

import rabbitizer as rz

from .decode import decode_range
from .elf import Program

I = rz.InstrId
JR_RA = 0x03E00008


@dataclass
class Function:
    start: int
    end: int                       # exclusive
    name: str
    instrs: list[rz.Instruction]


def looks_like_function_start(words: dict[int, int], addr: int) -> bool:
    """True if the code just before `addr` ends a function: `jr $ra` + delay slot, optionally
    followed by nop padding. Used to vet addresses that only appear as data/constants."""
    k = addr - 4
    for _ in range(4):                              # skip alignment padding
        if words.get(k) != 0:
            break
        k -= 4
    return words.get(k) == JR_RA or words.get(k - 4) == JR_RA


def has_prologue(words: dict[int, int], addr: int) -> bool:
    """`addiu $sp, $sp, -N`: allocating a stack frame is how most functions begin."""
    w = words.get(addr, 0)
    return (w >> 16) == 0x27BD and (w & 0x8000) != 0


def address_constants(instrs: list[rz.Instruction]) -> set[int]:
    """Addresses built with `lui reg, hi` + `addiu/ori reg2, reg, lo` (within a few instructions)."""
    out, hi = set(), {}
    for n, ins in enumerate(instrs):
        raw, u = ins.getRaw(), ins.uniqueId
        rs_, rt_ = (raw >> 21) & 31, (raw >> 16) & 31
        if u == I.cpu_lui:
            hi[rt_] = ((raw & 0xFFFF) << 16, n)
            continue
        if u in (I.cpu_addiu, I.cpu_ori) and rs_ in hi and n - hi[rs_][1] <= 8:
            lo = raw & 0xFFFF
            if u == I.cpu_addiu and lo & 0x8000:
                lo -= 0x10000
            out.add((hi[rs_][0] + lo) & 0xFFFFFFFF)
    return out


def after_return_starts(instrs: list[rz.Instruction]) -> set[int]:
    """The first instruction after `jr $ra` + delay slot (+ any nop padding). Usually the next
    function; when it's really code after an early return, merge_spurious undoes the split."""
    out = set()
    for n in range(len(instrs) - 2):
        if instrs[n].getRaw() == JR_RA:
            k = n + 2
            while k < len(instrs) and instrs[k].getRaw() == 0:
                k += 1
            if k < len(instrs):
                out.add(instrs[k].vram)
    return out


def switch_tables(prog: Program, instrs: list[rz.Instruction], lo: int, hi: int) -> list[tuple[int, list[int]]]:
    """Switch jump tables found from the code that uses them. A compiled switch dispatches with
        sltiu  t, idx, N          # bounds check: N cases
        lui/addiu base            # table address (in .rodata)
        lw     r, off(base + idx*4)
        jr     r
    Returns (address of the jr, case targets) for each one. Case targets are labels inside the
    function containing the jr, even when a case body looks like a whole function."""
    rodata = [(s.addr, s.addr + s.size) for s in prog.sections if not s.executable and s.size]
    out = []
    for n, ins in enumerate(instrs):
        raw = ins.getRaw()
        if ins.uniqueId != I.cpu_jr or (raw >> 21) & 31 == 31:
            continue
        window = instrs[max(0, n - 12):n]
        bases = [a for a in address_constants(window) if any(s <= a < e for s, e in rodata)]
        if not bases:
            continue
        base = bases[-1]
        count = None
        for w in reversed(window):
            if w.uniqueId == I.cpu_sltiu:
                count = w.getRaw() & 0xFFFF
                break
        targets = []
        for k in range(count if count is not None else 512):
            v = prog.read_u32(base + 4 * k)
            if v is None or not (lo <= v < hi) or v % 4:
                break
            targets.append(v)
        if targets:
            out.append((ins.vram, targets))
    return out


def jump_table_ranges(prog: Program, lo: int, hi: int, words: dict[int, int]) -> list[tuple[int, int]]:
    """Runs of 3+ consecutive data words that all point into [lo, hi) close together: switch
    jump tables. The code they point at belongs to one function and must not be split.
    C++ vtables look the same but point at function starts, so they're excluded."""
    out = []

    def is_jump_table(run: list[int]) -> bool:
        starts = sum(looks_like_function_start(words, v) for v in run)
        return len(run) >= 3 and starts * 2 < len(run)

    for s in prog.sections:
        if s.name != ".rodata" or not s.size:      # GCC emits jump tables into .rodata only
            continue
        data = prog.read_bytes(s.addr, s.addr + s.size)
        run: list[int] = []
        for k in range(0, len(data) - 3, 4):
            v = int.from_bytes(data[k:k + 4], "little")
            if lo <= v < hi and v % 4 == 0 and (not run or abs(v - run[0]) < 0x4000):
                run.append(v)
                continue
            if is_jump_table(run):
                out.append((min(run), max(run)))
            run = [v] if lo <= v < hi and v % 4 == 0 else []
        if is_jump_table(run):
            out.append((min(run), max(run)))
    return out


def merge_spurious(ordered: list[int], strong: set[int], instrs: list[rz.Instruction], lo: int) -> list[int]:
    """Drop guessed starts that cut through a function: if code between two starts branches to
    an address in a different chunk, the boundary between them can't be real (unless both
    sides are proven by direct calls, in which case the branch is a hand-written tail jump)."""
    def branch_targets(a: int, b: int):
        for ins in instrs[(a - lo) // 4:(b - lo) // 4]:
            if ins.isBranch() and not ins.doesLink():
                yield ins.getBranchVramGeneric()

    starts = list(ordered)
    changed = True
    while changed:
        changed = False
        for idx in range(len(starts)):
            s = starts[idx]
            e = starts[idx + 1] if idx + 1 < len(starts) else lo + len(instrs) * 4
            for t in branch_targets(s, e):
                if t >= e:                                  # forward out of this chunk
                    drop = {x for x in starts if e <= x <= t and x not in strong}
                elif t < s and s not in strong:             # backward into an earlier chunk
                    drop = {s}
                else:
                    continue
                if drop:
                    starts = [x for x in starts if x not in drop]
                    changed = True
                    break
            if changed:
                break
    return starts


def find_functions(prog: Program) -> list[Function]:
    code = [(lo, hi, decode_range(prog.read_bytes(lo, hi), lo)) for lo, hi in prog.code_ranges()]
    words = {ins.vram: ins.getRaw() for _, _, instrs in code for ins in instrs}

    # Addresses that reach code without a direct call: constants in code, pointers in data
    # (C++ vtables, callback and init tables). Only .rodata can also hold switch jump tables.
    referenced, from_rodata = set(), set()
    for _, _, instrs in code:
        referenced |= address_constants(instrs)
    for s in prog.sections:
        if not s.executable and s.size and s.name in (".data", ".rodata", ".sdata"):
            data = prog.read_bytes(s.addr, s.addr + s.size)
            vals = {int.from_bytes(data[k:k + 4], "little") for k in range(0, len(data) - 3, 4)}
            (from_rodata if s.name == ".rodata" else referenced).update(vals)

    funcs: list[Function] = []
    for lo, hi, instrs in code:
        end = lo + len(instrs) * 4

        def in_range(a: int) -> bool:
            return lo <= a < end and a % 4 == 0

        starts = {a for a in prog.symbols if in_range(a)}
        if in_range(prog.entry):
            starts.add(prog.entry)
        for ins in instrs:
            if ins.uniqueId == I.cpu_jal:
                target = ins.getInstrIndexAsVram()
            elif ins.isBranch() and ins.doesLink():          # bal / bgezal / bltzal
                target = ins.getBranchVramGeneric()
            else:
                continue
            if in_range(target):
                starts.add(target)
        # Switch statements: case labels belong to the function with the dispatching jr, and
        # nothing between the jr and its cases may start a new function.
        switches = switch_tables(prog, instrs, lo, end)
        case_labels = {t for _, ts in switches for t in ts}
        tables = [(min(jr, min(ts)) - 1, max(jr, max(ts))) for jr, ts in switches]
        tables += jump_table_ranges(prog, lo, end, words)     # data-only heuristic, as a fallback
        def in_table(x: int) -> bool:
            return any(a < x <= b for a, b in tables)
        # Code addresses held in data or built as constants are entry points (function pointers,
        # vtables, handlers passed to the kernel). Data can hold numbers that merely look like code
        # addresses, so a pointer only counts as proven if the code there also looks like a
        # function start; otherwise it's a guess.
        def plausible(a: int) -> bool:
            return looks_like_function_start(words, a) or has_prologue(words, a)
        pointed = {a for a in referenced if in_range(a) and a not in case_labels} | \
                  {a for a in from_rodata if in_range(a) and a not in case_labels
                   and (not in_table(a) or plausible(a))}
        strong = set(starts) | {lo} | {a for a in pointed if plausible(a) and not in_table(a)}
        starts |= strong | pointed
        starts |= {x for x in after_return_starts(instrs) if not in_table(x)}
        # `j` is how compilers tail-call; occasionally it's an intra-function jump, which the
        # merge pass sorts out like any other guess.
        starts |= {ins.getInstrIndexAsVram() for ins in instrs
                   if ins.uniqueId == I.cpu_j and in_range(ins.getInstrIndexAsVram())
                   and not in_table(ins.getInstrIndexAsVram())}
        starts.add(lo)                                      # code before the first known function

        # A delay slot can never start a function: it belongs to the branch before it.
        def is_delay_slot(a: int) -> bool:
            prev = a - 4
            if prev < lo:
                return False
            p = instrs[(prev - lo) // 4]
            return p.isBranch() or p.isJump()
        starts = {a for a in starts if not is_delay_slot(a)}

        ordered = merge_spurious(sorted(starts), strong, instrs, lo)
        for i, start in enumerate(ordered):
            stop = ordered[i + 1] if i + 1 < len(ordered) else end
            body = instrs[(start - lo) // 4:(stop - lo) // 4]
            name = prog.symbols.get(start, f"f_{start:08X}")
            funcs.append(Function(start, stop, name, body))
    return funcs
