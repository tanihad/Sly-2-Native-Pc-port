"""IOP modules (IRX files): parsing and relocation for the IOP recompiler.

An IRX is a relocatable MIPS ELF (e_type 0xFF80) linked at address 0. Its .iopmod program header
gives the module name, entry point, $gp and the text/data/bss sizes; one PT_LOAD segment holds
text + rodata + data (bss follows). SHT_REL sections list R_MIPS_32/26/HI16/LO16 relocations
whose r_offset is the module-relative address of the word to patch. Import tables (magic
0x41E00000) are lists of `jr $ra; addiu $zero, $zero, N` stubs that the IOP's loadcore patches
into jumps to the exporting library; export tables (magic 0x41C00000) are lists of function
pointers. Both live in .text.

The game embeds its IRX files back to back in the executable's .irx section; the EE copies them
to IOP RAM and asks LOADFILE to load them (runtime/iop_modules.c).
"""
import struct
from dataclasses import dataclass, field

PT_LOAD, PT_IOPMOD = 1, 0x70000080
SHT_REL = 9
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
IMPORT_MAGIC, EXPORT_MAGIC = 0x41E00000, 0x41C00000
JR_RA = 0x03E00008


@dataclass
class Section:
    name: str
    addr: int
    size: int
    executable: bool


@dataclass
class LibTable:
    lib: str
    version: int
    addr: int                    # address of the magic word
    end: int                     # first address after the table
    entries: list                # imports: [(stub address, function number)]; exports: [function address]


@dataclass
class IrxModule:
    name: str
    version: int
    entry: int
    gp: int
    text_size: int
    data_size: int
    bss_size: int
    image: bytearray             # PT_LOAD file bytes (text + rodata + data)
    memsz: int                   # including bss
    sections: list = field(default_factory=list)
    relocs: list = field(default_factory=list)      # (address, type) in file order
    imports: list = field(default_factory=list)     # LibTable
    exports: list = field(default_factory=list)     # LibTable
    base: int = 0                # where the image currently is linked

    def word(self, addr: int) -> int:
        off = addr - self.base
        return struct.unpack_from("<I", self.image, off)[0] if 0 <= off <= len(self.image) - 4 else 0

    @property
    def text(self) -> tuple[int, int]:
        return self.base, self.base + self.text_size

    def stubs(self) -> dict[int, tuple[str, int]]:
        return {a: (t.lib, n) for t in self.imports for a, n in t.entries}

    def relocated(self, base: int) -> "IrxModule":
        """A copy linked at `base` (must be 64 KB aligned, as loadcore's own results are not
        required to be: alignment keeps every LO16 unchanged, so relocation stays trivially
        checkable)."""
        assert self.base == 0 and base % 0x10000 == 0
        img = bytearray(self.image)

        def rd(a): return struct.unpack_from("<I", img, a)[0]
        def wr(a, v): struct.pack_into("<I", img, a, v & 0xFFFFFFFF)

        pending_hi: list[int] = []
        for addr, typ in self.relocs:
            if addr + 4 > len(img):
                raise ValueError(f"{self.name}: relocation at 0x{addr:X} outside the image")
            w = rd(addr)
            if typ == R_MIPS_32:
                wr(addr, w + base)
            elif typ == R_MIPS_26:
                target = ((w & 0x03FFFFFF) << 2) + base
                wr(addr, (w & 0xFC000000) | ((target >> 2) & 0x03FFFFFF))
            elif typ == R_MIPS_HI16:
                pending_hi.append(addr)
            elif typ == R_MIPS_LO16:
                lo = w & 0xFFFF
                slo = lo - 0x10000 if lo & 0x8000 else lo
                for h in pending_hi:
                    hw = rd(h)
                    value = ((hw & 0xFFFF) << 16) + slo + base
                    wr(h, (hw & 0xFFFF0000) | (((value + 0x8000) >> 16) & 0xFFFF))
                pending_hi.clear()
                wr(addr, (w & 0xFFFF0000) | ((slo + base) & 0xFFFF))
            else:
                raise ValueError(f"{self.name}: unsupported relocation type {typ} at 0x{addr:X}")
        if pending_hi:
            raise ValueError(f"{self.name}: HI16 relocation without a LO16 partner")

        def move(t: LibTable, imports: bool) -> LibTable:
            ents = [(a + base, n) for a, n in t.entries] if imports else [f + base for f in t.entries]
            return LibTable(t.lib, t.version, t.addr + base, t.end + base, ents)

        return IrxModule(self.name, self.version, self.entry + base, self.gp + base, self.text_size,
                         self.data_size, self.bss_size, img, self.memsz,
                         [Section(s.name, s.addr + base, s.size, s.executable) for s in self.sections],
                         [(a + base, t) for a, t in self.relocs],
                         [move(t, True) for t in self.imports], [move(t, False) for t in self.exports], base)


def parse(data: bytes) -> IrxModule:
    if data[:4] != b"\x7fELF" or struct.unpack_from("<H", data, 0x10)[0] != 0xFF80:
        raise ValueError("not an IRX (ELF type 0xFF80)")
    phoff, shoff = struct.unpack_from("<II", data, 0x1C)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 0x2A)
    mod = None
    image, memsz = bytearray(), 0
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from("<6I", data, phoff + i * phentsize)
        if p_type == PT_IOPMOD:
            _, entry, gp, text, dsize, bss, ver = struct.unpack_from("<6IH", data, p_off)
            name = data[p_off + 26:data.index(b"\0", p_off + 26)].decode("latin1")
            mod = (name, ver, entry, gp, text, dsize, bss)
        elif p_type == PT_LOAD:
            if p_vaddr != 0:
                raise ValueError("IRX segment not linked at 0")
            image, memsz = bytearray(data[p_off:p_off + p_filesz]), p_memsz
    if mod is None:
        raise ValueError("IRX without .iopmod header")
    m = IrxModule(*mod, image=image, memsz=memsz)

    shs = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    strtab = shs[shstrndx][4] if shstrndx < shnum else 0
    for sh in shs:
        nm = data[strtab + sh[0]:data.index(b"\0", strtab + sh[0])].decode("latin1") if strtab else ""
        if sh[1] == SHT_REL:
            for k in range(sh[5] // 8):
                off, info = struct.unpack_from("<II", data, sh[4] + 8 * k)
                m.relocs.append((off, info & 0xFF))
        elif sh[2] & 2 and sh[5]:                          # SHF_ALLOC
            m.sections.append(Section(nm, sh[3], sh[5], bool(sh[2] & 4)))

    reloc32 = {a for a, t in m.relocs if t == R_MIPS_32}
    for o in range(0, min(len(image), m.text_size) - 20, 4):
        magic = m.word(o)
        if magic not in (IMPORT_MAGIC, EXPORT_MAGIC) or m.word(o + 4) != 0:
            continue
        ver = m.word(o + 8) & 0xFFFF
        lib = bytes(image[o + 12:o + 20]).split(b"\0")[0].decode("latin1")
        p, entries = o + 20, []
        if magic == IMPORT_MAGIC:
            while m.word(p) == JR_RA and m.word(p + 4) >> 16 == 0x2400:
                entries.append((p, m.word(p + 4) & 0xFFFF))
                p += 8
            p += 4 if m.word(p) == 0 else 0                # terminating zero word
            m.imports.append(LibTable(lib, ver, o, p, entries))
        else:
            while p in reloc32:
                entries.append(m.word(p))
                p += 4
            p += 4 if m.word(p) == 0 else 0
            m.exports.append(LibTable(lib, ver, o, p, entries))
    return m


def modules_in(blob: bytes) -> list[tuple[int, bytes]]:
    """Split a concatenation of IRX files: (offset, file bytes) for each."""
    out, o = [], 0
    while o < len(blob) - 4:
        if blob[o:o + 4] != b"\x7fELF":
            o += 4
            continue
        shoff, = struct.unpack_from("<I", blob, o + 0x20)
        shentsize, shnum = struct.unpack_from("<HH", blob, o + 0x2E)
        end = shoff + shentsize * shnum
        for i in range(shnum):
            sh = struct.unpack_from("<10I", blob, o + shoff + i * shentsize)
            if sh[1] != 8:                                  # SHT_NOBITS has no file bytes
                end = max(end, sh[4] + sh[5])
        out.append((o, blob[o:o + end]))
        o += (end + 3) & ~3
    return out


def elf_irx_modules(elf_path: str) -> list[tuple[int, IrxModule]]:
    """The IRX files embedded in the game executable's .irx section: (EE address, module)."""
    d = open(elf_path, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    shs = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = shs[shstrndx][4]
    for sh in shs:
        nm = d[strtab + sh[0]:d.index(b"\0", strtab + sh[0])]
        if nm == b".irx":
            blob = d[sh[4]:sh[4] + sh[5]]
            return [(sh[3] + o, parse(f)) for o, f in modules_in(blob)]
    raise ValueError(f"{elf_path}: no .irx section")
