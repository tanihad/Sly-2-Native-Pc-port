"""Split the executable code into functions."""
from dataclasses import dataclass

import rabbitizer as rz

from .decode import decode_range
from .elf import Program


@dataclass
class Function:
    start: int
    end: int                       # exclusive
    name: str
    instrs: list[rz.Instruction]


def find_functions(prog: Program) -> list[Function]:
    funcs: list[Function] = []
    for seg in prog.code_segments():
        instrs = decode_range(seg.data, seg.vaddr)
        seg_end = seg.vaddr + len(instrs) * 4

        def in_seg(a: int) -> bool:
            return seg.vaddr <= a < seg_end and a % 4 == 0

        starts = {a for a in prog.symbols if in_seg(a)}
        if in_seg(prog.entry):
            starts.add(prog.entry)
        for ins in instrs:
            if ins.uniqueId == rz.InstrId.cpu_jal:
                target = ins.getInstrIndexAsVram()
            elif ins.isBranch() and ins.doesLink():          # bal / bgezal / bltzal
                target = ins.getBranchVramGeneric()
            else:
                continue
            if in_seg(target):
                starts.add(target)
        if not starts or min(starts) != seg.vaddr:
            starts.add(seg.vaddr)                           # code before the first known function

        ordered = sorted(starts)
        for i, start in enumerate(ordered):
            end = ordered[i + 1] if i + 1 < len(ordered) else seg_end
            body = instrs[(start - seg.vaddr) // 4:(end - seg.vaddr) // 4]
            name = prog.symbols.get(start, f"f_{start:08X}")
            funcs.append(Function(start, end, name, body))
    return funcs
