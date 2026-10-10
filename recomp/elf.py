"""Load a PS2 executable (a 32-bit little-endian MIPS ELF)."""
from dataclasses import dataclass, field

from elftools.elf.constants import P_FLAGS
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


@dataclass
class Segment:
    vaddr: int
    data: bytes          # file-backed bytes
    memsz: int           # may exceed len(data); the rest is zero-filled (.bss)
    executable: bool

    @property
    def end(self) -> int:
        return self.vaddr + self.memsz


@dataclass
class Program:
    path: str
    entry: int
    segments: list[Segment]
    symbols: dict[int, str] = field(default_factory=dict)   # function address -> name

    def code_segments(self) -> list[Segment]:
        return [s for s in self.segments if s.executable]

    def read_u32(self, addr: int) -> int | None:
        for s in self.segments:
            off = addr - s.vaddr
            if 0 <= off <= len(s.data) - 4:
                return int.from_bytes(s.data[off:off + 4], "little")
        return None


def load(path: str) -> Program:
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if elf.elfclass != 32 or not elf.little_endian or elf["e_machine"] != "EM_MIPS":
            raise ValueError(f"{path}: not a 32-bit little-endian MIPS ELF")

        segments = []
        for seg in elf.iter_segments():
            if seg["p_type"] != "PT_LOAD" or seg["p_memsz"] == 0:
                continue
            segments.append(Segment(
                vaddr=seg["p_vaddr"],
                data=seg.data(),
                memsz=seg["p_memsz"],
                executable=bool(seg["p_flags"] & P_FLAGS.PF_X),
            ))

        symbols = {}
        for sec in elf.iter_sections():
            if isinstance(sec, SymbolTableSection):
                for sym in sec.iter_symbols():
                    if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"] and sym.name:
                        symbols[sym["st_value"]] = sym.name

        return Program(path=path, entry=elf["e_entry"], segments=segments, symbols=symbols)
