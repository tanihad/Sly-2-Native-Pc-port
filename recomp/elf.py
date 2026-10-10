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
class Section:
    name: str
    addr: int
    size: int
    executable: bool


@dataclass
class Program:
    path: str
    entry: int
    segments: list[Segment]
    symbols: dict[int, str] = field(default_factory=dict)   # function address -> name
    sections: list[Section] = field(default_factory=list)

    def code_ranges(self) -> list[tuple[int, int]]:
        """Address ranges that hold instructions: executable sections if the ELF has section
        headers (segments often mix code and data), otherwise whole executable segments."""
        # .vutext holds vector-unit microcode, a different instruction set from the EE's.
        ranges = [(s.addr, s.addr + s.size) for s in self.sections
                  if s.executable and s.size and not s.name.startswith(".vu")]
        return ranges or [(s.vaddr, s.vaddr + len(s.data)) for s in self.code_segments()]

    def read_bytes(self, start: int, end: int) -> bytes:
        for s in self.segments:
            if s.vaddr <= start and end <= s.vaddr + len(s.data):
                return s.data[start - s.vaddr:end - s.vaddr]
        raise ValueError(f"0x{start:08X}-0x{end:08X} is not file-backed")

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

        sections = [Section(sec.name, sec["sh_addr"], sec["sh_size"], bool(sec["sh_flags"] & 0x4))
                    for sec in elf.iter_sections() if sec["sh_addr"]]   # SHF_EXECINSTR = 0x4

        symbols = {}
        for sec in elf.iter_sections():
            if isinstance(sec, SymbolTableSection):
                for sym in sec.iter_symbols():
                    if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"] and sym.name:
                        symbols[sym["st_value"]] = sym.name

        return Program(path=path, entry=elf["e_entry"], segments=segments, symbols=symbols,
                       sections=sections)
